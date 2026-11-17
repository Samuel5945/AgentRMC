#include "ui/WorkWebView.h"

#include <QCoreApplication>
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

#include <windows.h>

#include <objbase.h>

#include "WebView2.h"

namespace {

// 动态加载：工程不链接 WebView2Loader.lib，运行时把 loader 取进来。
// 路径可用 AGENTRMC_WV2_LOADER 覆盖；缺省按「从源码树里跑」的相对位置找。
HMODULE loadLoader() {
    const QByteArray fromEnv = qgetenv("AGENTRMC_WV2_LOADER");
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    if (!fromEnv.isEmpty()) {
        candidates << QString::fromLocal8Bit(fromEnv);
    }
    // 从 exe 往上找 .webview2-sdk：源码树里跑是 build/Release → build → native，
    // 写死层数会在换个构建目录后悄悄找不到。
    QDir dir(appDir);
    for (int up = 0; up < 4; ++up) {
        candidates << dir.absolutePath() + QStringLiteral(
                          "/.webview2-sdk/runtimes/win-x64/native/"
                          "WebView2Loader.dll")
                   << dir.absolutePath() + QStringLiteral(
                          "/.webview2-sdk/build/native/x64/WebView2Loader.dll");
        if (!dir.cdUp()) {
            break;
        }
    }
    candidates << appDir + QStringLiteral("/WebView2Loader.dll");
    for (const QString &path : candidates) {
        const QString full = QDir::cleanPath(path);
        if (!QFileInfo::exists(full)) {
            continue;
        }
        if (HMODULE mod = LoadLibraryW(reinterpret_cast<LPCWSTR>(full.utf16()))) {
            qInfo().noquote() << "[webview2] loader" << full;
            return mod;
        }
    }
    qWarning().noquote() << "[webview2] 找不到 WebView2Loader.dll，试过"
                         << candidates;
    return nullptr;
}

typedef HRESULT(STDMETHODCALLTYPE *CreateEnvFn)(
    LPCWSTR browserExecutableFolder, LPCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *options,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);

// 数据里出现 "</script>" 会把内联脚本提前关掉；JSON 里把 < 转义掉不影响语义。
QString jsEscape(const QString &json) {
    QString out = json;
    out.replace(QLatin1Char('<'), QStringLiteral("\\u003c"));
    return out;
}

template <typename Base>
class ComHandler : public Base {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Base)) {
            *object = this;
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

protected:
    volatile LONG refs_ = 1;
};

using EnvCompleted =
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
using ControllerCompleted =
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
using WebMessage = ICoreWebView2WebMessageReceivedEventHandler;

class EnvHandler : public ComHandler<EnvCompleted> {
public:
    explicit EnvHandler(WorkWebView *owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                     ICoreWebView2Environment *environment) override;

private:
    WorkWebView *owner_;
};

class ControllerHandler : public ComHandler<ControllerCompleted> {
public:
    explicit ControllerHandler(WorkWebView *owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                     ICoreWebView2Controller *controller) override;

private:
    WorkWebView *owner_;
};

class MessageHandler : public ComHandler<WebMessage> {
public:
    explicit MessageHandler(WorkWebView *owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2 *,
        ICoreWebView2WebMessageReceivedEventArgs *args) override;

private:
    WorkWebView *owner_;
};

// 文档就绪才能开始 ExecuteScript 喂数据；在那之前推的状态先攒着。
class NavHandler : public ComHandler<ICoreWebView2NavigationCompletedEventHandler> {
public:
    explicit NavHandler(WorkWebView *owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) override {
        BOOL ok = FALSE;
        if (args != nullptr) {
            args->get_IsSuccess(&ok);
        }
        owner_->handleNavigationCompleted(ok != FALSE);
        return S_OK;
    }

private:
    WorkWebView *owner_;
};

// 环境回调里要用到 ControllerHandler，所以实现挪到类外、声明之后。
HRESULT EnvHandler::Invoke(HRESULT result,
                           ICoreWebView2Environment *environment) {
    if (FAILED(result) || environment == nullptr) {
        qWarning("webview2: 环境创建失败 hr=0x%08lx",
                 static_cast<unsigned long>(result));
        return result;
    }
    HWND hwnd = reinterpret_cast<HWND>(owner_->winId());
    const HRESULT hr = environment->CreateCoreWebView2Controller(
        hwnd, new ControllerHandler(owner_));
    // 这个 environment 是调用方传进来的引用，回调返回后由 SDK 自己放；
    // 这里再 Release 一次会让后面异步的控制器创建拿到悬空指针（实测 0xC0000005）。
    if (FAILED(hr)) {
        qWarning("webview2: 请求建控制器失败 hr=0x%08lx",
                 static_cast<unsigned long>(hr));
    }
    return hr;
}

HRESULT ControllerHandler::Invoke(HRESULT result,
                                  ICoreWebView2Controller *controller) {
    if (FAILED(result) || controller == nullptr) {
        qWarning("webview2: 控制器创建失败 hr=0x%08lx",
                 static_cast<unsigned long>(result));
        return result;
    }
    ICoreWebView2 *webview = nullptr;
    if (FAILED(controller->get_CoreWebView2(&webview)) || webview == nullptr) {
        qWarning("webview2: 拿不到 CoreWebView2");
        return E_FAIL;
    }
    owner_->applyWebviewRaw(controller, webview);
    return S_OK;
}

// 页面必须 postMessage 一个「对象」：get_WebMessageAsJson 给的是那条消息的 JSON
// 形式。若页面传 JSON.stringify(obj)，这里拿到的就是一串带引号的字符串字面量，
// .object() 解出来为空，点击静默失效（实测踩过）。
HRESULT MessageHandler::Invoke(
    ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) {
    LPWSTR json = nullptr;
    if (FAILED(args->get_WebMessageAsJson(&json)) || json == nullptr) {
        return S_OK;
    }
    const QJsonObject obj = QJsonDocument::fromJson(
                                QString::fromWCharArray(json).toUtf8())
                                .object();
    CoTaskMemFree(json);
    if (!obj.isEmpty()) {
        owner_->handleAction(obj);
    }
    return S_OK;
}

}  // namespace

// 运行时在这两处之一：机器级或用户级安装的 Edge WebView2 Runtime。
bool WorkWebView::runtimeAvailable() {
    if (loadLoader() == nullptr) {
        return false;
    }
    const QStringList roots = {
        QStringLiteral("C:/Program Files (x86)/Microsoft/EdgeWebView/"
                       "Application"),
        QStringLiteral("C:/Program Files/Microsoft/EdgeWebView/Application"),
    };
    for (const QString &root : roots) {
        const QDir app(root);
        if (!app.exists()) {
            continue;
        }
        const QStringList versions =
            app.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (!versions.isEmpty()) {
            return true;
        }
    }
    return false;
}

struct WorkWebView::Private {
    HMODULE loader = nullptr;
    ICoreWebView2Controller *controller = nullptr;
    ICoreWebView2 *webview = nullptr;
    ICoreWebView2_2 *webview2 = nullptr;  // 消息回调在这版接口上
    EventRegistrationToken messageToken{};
    EventRegistrationToken navToken{};
};

WorkWebView::WorkWebView(QWidget *parent)
    : QWidget(parent), d_(new Private) {
    // WebView2 的回调要求 STA；Qt 主线程本来就是 STA，这里只是保险。
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    setMinimumHeight(240);
    // 不等 showEvent：页签里的子控件首次显示的时序不稳（实测 showEvent 没来），
    // 而 winId() 自己就能把 HWND 建出来，事件循环一空就动手。
    QTimer::singleShot(0, this, [this] {
        if (!webviewReady() && !creating_) {
            create();
        }
    });
}

WorkWebView::~WorkWebView() {
    if (d_->webview != nullptr && d_->navToken.value != 0) {
        d_->webview->remove_NavigationCompleted(d_->navToken);
    }
    if (d_->webview2 != nullptr) {
        d_->webview2->remove_WebMessageReceived(d_->messageToken);
        d_->webview2->Release();
    }
    if (d_->controller != nullptr) {
        d_->controller->Close();
        d_->controller->Release();
    }
    if (d_->webview != nullptr) {
        d_->webview->Release();
    }
    delete d_;
}

void WorkWebView::setSources(StateProvider provider, ActionHandler action) {
    provider_ = std::move(provider);
    action_ = std::move(action);
}

void WorkWebView::setResourceRoot(const QString &folder) {
    resourceRoot_ = folder;
}

bool WorkWebView::webviewReady() const {
    return d_->webview != nullptr;
}

void WorkWebView::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (!webviewReady() && !creating_) {
        create();
    }
}

void WorkWebView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    syncBounds();
}

void WorkWebView::syncBounds() {
    if (d_->controller == nullptr) {
        return;
    }
    // WebView2 的边界是物理像素，Qt 给的是逻辑像素；控件还没布局时是 0x0，
    // 喂 0x0 之后若没人生成 resize 事件，页面就永远画不出来（实测白板）。
    //
    // 但尺寸不能取控件自己的 width()×devicePixelRatioF()：换缩放 / 改分辨率 /
    // 插拔屏之后这两个值都可能是旧的（2026-11-17 实测：窗口 1060 逻辑宽，控件还是
    // 1404、倍率还是旧的 2.65 → 喂进去 2808 物理宽，页面按 1404 CSS 排版，
    // 顶栏右侧那排「检查更新/主题/新手引导 + 窗口按钮」整块被窗口裁掉，只剩
    // 「监听」胶囊贴着右边＝用户报的「顶栏显示不全」）。顶层窗客户区是 Windows
    // 那边的当前事实，PMv2 进程下 GetClientRect 给的就是物理像素，正好是这里
    // 要的单位，两个陈旧源一次绕开。
    LONG w = 0;
    LONG h = 0;
    QWidget *top = window();
    if (top != nullptr) {
        RECT client{};
        if (GetClientRect(reinterpret_cast<HWND>(top->winId()), &client)) {
            w = client.right - client.left;
            h = client.bottom - client.top;
        }
    }
    if (w <= 0 || h <= 0) {
        // 兜底：顶层还没有 HWND 之类，退回控件尺寸 × 当前倍率。
        const qreal ratio =
            top != nullptr ? top->devicePixelRatioF() : devicePixelRatioF();
        w = static_cast<LONG>(width() * ratio);
        h = static_cast<LONG>(height() * ratio);
    } else if (top != nullptr && top != this) {
        // 控件本身也可能停在旧尺寸（它就是控制器的父窗，比边界小的话页面照样被裁）：
        // 顺手拉回顶层客户区。本工程里外壳 WebView 就是整窗（外层布局零边距、别的块
        // 都 hide），所以原点恒为 0,0。
        const QSize want = top->rect().size();
        if (want.isValid() && want.width() > 0 && want.height() > 0 &&
            size() != want) {
            setGeometry(QRect(QPoint(0, 0), want));
        }
    }
    const RECT r{0, 0, w, h};
    d_->controller->put_IsVisible(w > 0 && h > 0 ? TRUE : FALSE);
    d_->controller->put_Bounds(r);
}

void WorkWebView::create() {
    creating_ = true;
    d_->loader = loadLoader();
    if (d_->loader == nullptr) {
        qWarning("webview2: 找不到 WebView2Loader.dll");
        creating_ = false;
        return;
    }
    auto createEnv = reinterpret_cast<CreateEnvFn>(GetProcAddress(
        d_->loader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (createEnv == nullptr) {
        qWarning("webview2: loader 里没有 CreateCoreWebView2EnvironmentWithOptions");
        creating_ = false;
        return;
    }
    const QString userData = QDir::tempPath() + QStringLiteral("/AgentRMCWebView2");
    QDir().mkpath(userData);
    auto *handler = new EnvHandler(this);
    const HRESULT hr = createEnv(nullptr,
                                 reinterpret_cast<LPCWSTR>(userData.utf16()),
                                 nullptr, handler);
    // 自己那份引用无论如何都交回去：调用方成功时会另加一次引用，回调跑完再放。
    handler->Release();
    if (FAILED(hr)) {
        qWarning("webview2: 启动环境失败 hr=0x%08lx",
                 static_cast<unsigned long>(hr));
        creating_ = false;
    }
}

void WorkWebView::applyWebviewRaw(void *controller, void *webview) {
    auto *ctrl = static_cast<ICoreWebView2Controller *>(controller);
    auto *view = static_cast<ICoreWebView2 *>(webview);
    // 回调参数不是归我们的引用：Invoke 返回后 SDK 就把它放了。不留 AddRef，
    // 下一次（定时器里）再 put_Bounds 就是拿悬空指针——实测 0xC0000005。
    ctrl->AddRef();
    d_->controller = ctrl;
    d_->webview = view;
    creating_ = false;

    ICoreWebView2Settings *settings = nullptr;
    if (SUCCEEDED(view->get_Settings(&settings)) && settings != nullptr) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->Release();
    }
    // 实测：Controller2 的透明背景透出的是「桌面」，不是 Qt 画好的主窗背景——
    // Chromium 的合成层和 Qt 的 backing store 不是一回事，透过来的是壁纸和别的窗口。
    // 所以这一页保持不透明，主题背景由页面自己画：素材目录映射成虚拟域名，
    // 插画 + 压暗纱走 CSS；classic 用主题纯色底。DefaultBackgroundColor 只当
    // 首帧之前的兜底色，用同一个 bg，免得闪一下白。
    const QString stateJson = provider_ ? provider_() : QStringLiteral("{}");
    const QJsonObject state =
        QJsonDocument::fromJson(stateJson.toUtf8()).object();
    const QJsonObject theme = state.value(QStringLiteral("theme")).toObject();
    COREWEBVIEW2_COLOR base{255, 11, 14, 20};
    const QColor parsed = QColor(theme.value(QStringLiteral("base")).toString());
    if (parsed.isValid()) {
        base = {static_cast<BYTE>(255), static_cast<BYTE>(parsed.red()),
                static_cast<BYTE>(parsed.green()),
                static_cast<BYTE>(parsed.blue())};
    }
    ICoreWebView2Controller2 *ctrl2 = nullptr;
    if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ICoreWebView2Controller2),
                                       reinterpret_cast<void **>(&ctrl2))) &&
        ctrl2 != nullptr) {
        ctrl2->put_DefaultBackgroundColor(base);
        ctrl2->Release();
    }
    if (!resourceRoot_.isEmpty()) {
        // 虚拟域名映射挂在 ICoreWebView2_3 上；映射本身留在 webview 里，不用留接口指针。
        ICoreWebView2_3 *view3 = nullptr;
        if (SUCCEEDED(view->QueryInterface(__uuidof(ICoreWebView2_3),
                                           reinterpret_cast<void **>(&view3))) &&
            view3 != nullptr) {
            const HRESULT mapped = view3->SetVirtualHostNameToFolderMapping(
                reinterpret_cast<LPCWSTR>(
                    QStringLiteral("agentrmc.theme").utf16()),
                reinterpret_cast<LPCWSTR>(resourceRoot_.utf16()),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            if (FAILED(mapped)) {
                qWarning("webview2: 主题素材域名映射失败 hr=0x%08lx",
                         static_cast<unsigned long>(mapped));
            }
            view3->Release();
        } else {
            qWarning("webview2: 拿不到 ICoreWebView2_3，主题背景图不可用");
        }
    }

    ICoreWebView2_2 *view2 = nullptr;
    if (FAILED(view->QueryInterface(__uuidof(ICoreWebView2_2),
                                    reinterpret_cast<void **>(&view2))) ||
        view2 == nullptr) {
        qWarning("webview2: 拿不到 ICoreWebView2_2，点击回传不可用");
        pendingRefresh_ = false;
        pushState(stateJson);
        return;
    }
    d_->webview2 = view2;

    auto *nav = new NavHandler(this);
    const HRESULT navAdded = view->add_NavigationCompleted(nav, &d_->navToken);
    nav->Release();
    if (FAILED(navAdded)) {
        qWarning("webview2: 挂不上导航回调 hr=0x%08lx",
                 static_cast<unsigned long>(navAdded));
    }

    auto *handler = new MessageHandler(this);
    const HRESULT added =
        view2->add_WebMessageReceived(handler, &d_->messageToken);
    handler->Release();
    if (FAILED(added)) {
        qWarning("webview2: 挂不上消息回调 hr=0x%08lx",
                 static_cast<unsigned long>(added));
    }
    pendingRefresh_ = false;
    // 创建往往发生在布局之前：先同步一次，剩下交给下一轮事件循环再补。
    syncBounds();
    QTimer::singleShot(0, this, [this] { syncBounds(); });
    pushState(stateJson);
}

void WorkWebView::handleAction(const QJsonObject &action) {
    if (action_) {
        action_(action);
    }
}

void WorkWebView::refresh() {
    if (!webviewReady()) {
        pendingRefresh_ = true;
        return;
    }
    pushState(provider_ ? provider_() : QStringLiteral("{}"));
}

void WorkWebView::pushLive(const QJsonObject &live) {
    if (!documentReady_ || d_->webview == nullptr) {
        return;
    }
    const QString json = QString::fromUtf8(
        QJsonDocument(live).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral("window.__setLive(")
                           + jsEscape(json) + QStringLiteral(");");
    d_->webview->ExecuteScript(reinterpret_cast<LPCWSTR>(script.utf16()),
                               nullptr);
}

void WorkWebView::pushTheme(const QJsonObject &theme) {
    if (!documentReady_ || d_->webview == nullptr) {
        refresh();
        return;
    }
    const QString json = QString::fromUtf8(
        QJsonDocument(theme).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral("window.__setTheme(")
                           + jsEscape(json) + QStringLiteral(");");
    d_->webview->ExecuteScript(reinterpret_cast<LPCWSTR>(script.utf16()),
                               nullptr);
}

void WorkWebView::pushChat(const QJsonObject &page) {
    if (!documentReady_ || d_->webview == nullptr) {
        return;
    }
    const QString json = QString::fromUtf8(
        QJsonDocument(page).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral("window.__setChatPage(")
                           + jsEscape(json) + QStringLiteral(");");
    d_->webview->ExecuteScript(reinterpret_cast<LPCWSTR>(script.utf16()),
                               nullptr);
}

void WorkWebView::pushSessions(const QJsonObject &catalog) {
    if (!documentReady_ || d_->webview == nullptr) {
        return;
    }
    const QString json = QString::fromUtf8(
        QJsonDocument(catalog).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral("window.__setSessions(")
                           + jsEscape(json) + QStringLiteral(");");
    d_->webview->ExecuteScript(reinterpret_cast<LPCWSTR>(script.utf16()),
                               nullptr);
}

void WorkWebView::pushSessionChat(const QJsonObject &page) {
    if (!documentReady_ || d_->webview == nullptr) {
        return;
    }
    const QString json = QString::fromUtf8(
        QJsonDocument(page).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral("window.__setSessionChat(")
                           + jsEscape(json) + QStringLiteral(");");
    d_->webview->ExecuteScript(reinterpret_cast<LPCWSTR>(script.utf16()),
                               nullptr);
}

void WorkWebView::pushSessionResult(const QJsonObject &result) {
    if (!documentReady_ || d_->webview == nullptr) {
        return;
    }
    const QString json = QString::fromUtf8(
        QJsonDocument(result).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral("window.__setDispatchResult(")
                           + jsEscape(json) + QStringLiteral(");");
    d_->webview->ExecuteScript(reinterpret_cast<LPCWSTR>(script.utf16()),
                               nullptr);
}

void WorkWebView::pushDeleteResult(const QJsonObject &result) {
    if (!documentReady_ || d_->webview == nullptr) {
        return;
    }
    const QString json = QString::fromUtf8(
        QJsonDocument(result).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral("window.__setDeleteResult(")
                           + jsEscape(json) + QStringLiteral(");");
    d_->webview->ExecuteScript(reinterpret_cast<LPCWSTR>(script.utf16()),
                               nullptr);
}

// 首帧只能整页给（NavigateToString 没有「换个 body」的写法）；文档一旦好了，
// 后面一律 ExecuteScript 只喂数据。整页重载会把选中态、滚动位置、hover 全冲掉
// （实测：标为已处理之后列表回到顶部、按钮变灰），事件一密还会闪。
void WorkWebView::pushState(const QString &stateJson) {
    if (d_->webview == nullptr) {
        return;
    }
    if (documentReady_) {
        executeState(stateJson);
        return;
    }
    if (navigating_) {
        pendingState_ = stateJson;  // 整页在路上，只留最后一次
        return;
    }
    navigating_ = true;
    pendingState_.clear();
    const QString html = htmlFor(stateJson);
    const HRESULT hr = d_->webview->NavigateToString(
        reinterpret_cast<LPCWSTR>(html.utf16()));
    if (FAILED(hr)) {
        navigating_ = false;
        qWarning("webview2: NavigateToString 失败 hr=0x%08lx",
                 static_cast<unsigned long>(hr));
    }
}

void WorkWebView::executeState(const QString &stateJson) {
    if (d_->webview == nullptr) {
        return;
    }
    const QString script = QStringLiteral("window.__setState(")
                           + jsEscape(stateJson) + QStringLiteral(");");
    const HRESULT hr = d_->webview->ExecuteScript(
        reinterpret_cast<LPCWSTR>(script.utf16()), nullptr);
    if (FAILED(hr)) {
        qWarning("webview2: ExecuteScript 失败 hr=0x%08lx",
                 static_cast<unsigned long>(hr));
    }
}

void WorkWebView::handleNavigationCompleted(bool success) {
    navigating_ = false;
    if (!success) {
        qWarning("webview2: 文档加载失败");
        return;
    }
    documentReady_ = true;
    if (!pendingState_.isEmpty()) {
        const QString pending = pendingState_;
        pendingState_.clear();
        executeState(pending);
    }
}

QString WorkWebView::htmlFor(const QString &stateJson) const {
    // 二维码库（qrcode-generator，MIT）：系统页「手机访问」卡片画配对码用。
    // 从 qrc 读进来拼在页面脚本最前面；读不到就只影响二维码，别的照常。
    QString qrLib;
    QFile qrFile(QStringLiteral(":/qrcode.js"));
    if (qrFile.open(QIODevice::ReadOnly)) {
        qrLib = QString::fromUtf8(qrFile.readAll());
    }
    // 状态由 Qt 侧一次推全（work / workers / health / theme / events）。页面只管三件事：
    // 排版、把动作发回 Qt、把 theme 里的 token 落成 CSS 变量。
    //
    // 底是页面自己画的，不是「透明透出主窗」：实测 WebView2 的透明背景透出来的是
    // 桌面，拿不到 Qt 画好的主窗背景。所以美术主题走「插画 + 压暗纱」两层背景 +
    // 一张半透明卡片（和 QFrame#card 同一套 token），classic 走主题纯色底。
    // 只有阶段色（等你处理/待验收/进行中）跟 Qt 侧一样保持硬编码，两边同色。
    return QString::fromUtf8(R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<style>
 :root{
  --bg:#f3f5f8;
  --surface:#ffffff;--surface2:#eef1f5;
  --text:#171e2a;--text-soft:#1c2331;--text-dim:#2a323f;--muted:#48525f;--muted2:#5a6472;
  --ghost:#4a5361;--faint:#8f98a3;--line:#c9d0d9;--raised:#ffffff;--accent:#4f8cff;
  --accent-hover:#6299ff;--on-accent:#ffffff;--danger:#e5484d;--warn:#d29922;
  --ok:#3fb950;--pending:#f0883e;--info:#58a6ff;
  --w6:rgba(15,23,42,.047);--w8:rgba(15,23,42,.063);--w10:rgba(15,23,42,.078);
  --w14:rgba(15,23,42,.110);--w16:rgba(15,23,42,.125);--w18:rgba(15,23,42,.141);
  --w30:rgba(15,23,42,.235);--a26:rgba(79,140,255,.102);--a36:rgba(79,140,255,.141);
  --a64:rgba(79,140,255,.251);
  --f10:10px;--f11:11px;--f12:12px;--f13:13px}
 html,body{margin:0;height:100%;background:transparent;color:var(--text);
   font-family:"Microsoft YaHei UI","Segoe UI",sans-serif;overflow:hidden}
 /* .page/.foot 自带 display:flex，会盖掉 UA 的 [hidden]{display:none}，
    藏起来的页会把头行漏在别的页下面（实测）。统一用 !important 压住。 */
 [hidden]{display:none !important}
 body{display:flex;flex-direction:column}
 .tabs{flex:0 0 auto;display:flex;gap:4px;padding:8px 12px 0}
 .tab{border:1px solid transparent;background:transparent;color:var(--muted);
   font-size:var(--f12);padding:7px 14px;border-radius:8px;cursor:default}
 .tab:hover{color:var(--text-dim);background:var(--w8)}
 .tab.active{background:var(--a26);border-color:var(--a64);color:var(--text)}
 .tab .count{font-size:var(--f10);color:var(--muted2);margin-left:5px}
 /* 内容整体装在一张卡里：Qt 侧 buildWorkPage 就是 QFrame#card，美术主题下
    这张卡是半透明 rgba，插画从底下透过来但字有底可读。不做卡的话 11px 灰字
    直接压在立绘上，右边一列时间基本读不出来（鲸鱼娘实测）。
    CSS 注释只能用星号斜杠：写成 // 会让紧跟其后的整条规则被解析器丢掉。 */
 .card{flex:1 1 auto;min-height:0;display:flex;flex-direction:column;
   margin:10px 14px 14px;padding:13px 14px 0;border-radius:16px;
   border:1px solid var(--w18);
   background:linear-gradient(180deg,var(--surface),var(--surface2))}
 .page{flex:1 1 auto;min-height:0;display:flex;flex-direction:column}
 .head{flex:0 0 auto;display:flex;align-items:center;gap:8px;flex-wrap:wrap}
 .total{font-size:var(--f13);font-weight:700;color:var(--text)}
 .chip{font-size:var(--f11);font-weight:600;padding:2px 9px;border-radius:10px;
   background:var(--w6);border:1px solid var(--w14)}
 .spacer{margin-left:auto}
 /* 下拉/输入框跟 Qt 侧一套：半透明底 + 细描边。原来用 --raised（不透明
    #161d2a）+ --line（浅板岩灰），在美术主题的半透明卡片上是一块块实心
    黑砖，2026-11-10 用户反馈「下拉颜色太重」。 */
 select{font-size:var(--f11);color:var(--text);background:var(--w8);
   border:1px solid var(--w14);border-radius:8px;padding:3px 6px}
 select:hover{background:var(--w10);border-color:var(--w30)}
 select:focus{outline:none;background:var(--w10);border-color:var(--accent)}
 .ghost{border:0;background:transparent;color:var(--text-dim);
   font-size:var(--f11);padding:2px 6px;cursor:default}
 .ghost:hover{color:var(--accent)}
 .health{flex:0 0 auto;color:var(--muted);font-size:var(--f11);margin-top:6px}
 .list{flex:1 1 auto;min-height:0;overflow-y:auto;margin-top:6px;
   scrollbar-width:none}
 .list::-webkit-scrollbar{width:0;height:0}
 .row{position:relative;padding:8px 96px 8px 21px;cursor:default}
 .row:hover{background:var(--w6)}
 .row.sel{background:var(--a36)}
 .row.msel{background:var(--w10);border-radius:8px}
 .rowacts{position:absolute;right:10px;top:50%;transform:translateY(-50%);
   display:flex;gap:4px}
 /* 行内两键：回到 Agent = 箭头进窗口（切到宿主窗口），标为已处理 = 对勾。
     去描边、16px 图标、和顶栏同一档 7px 圆角，两处图标语言统一
     （2026-11-17 用户：看看回到 Agent 和标记已处理怎么优化下图标）。
     常态不做降透明处理：0.55~0.72 实测在 1x 屏上几乎看不见，得悬停才找得到。 */
 .icobtn{width:30px;height:30px;border-radius:7px;border:0;background:transparent;
   color:var(--muted);display:flex;align-items:center;justify-content:center;
   padding:0;cursor:pointer}
 .icobtn svg{width:16px;height:16px;display:block;pointer-events:none}
 .icobtn:hover{background:var(--w10);color:var(--accent)}
 .icobtn.ack:hover{background:var(--ok);color:#fff}
 .bar{position:absolute;left:9px;top:12px;width:3px;height:32px;border-radius:2px}
 .l1{display:flex;align-items:baseline;gap:8px;white-space:nowrap;overflow:hidden}
 .name{font-weight:600;font-size:var(--f12);color:var(--text)}
 .stage{font-size:var(--f10)}
 .where{font-size:var(--f10);color:var(--muted2);overflow:hidden;
   text-overflow:ellipsis}
 .time{margin-left:auto;font-size:var(--f10);color:var(--ghost)}
 .l2{font-size:var(--f11);margin-top:2px;color:var(--text-soft);
   white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
 /* 事件行照 QListWidget#logList::item：5px 6px 内边距、圆角、hover 一层 w6；
    长文案折行（Qt 侧 wordWrap=true），时间戳和正文之间是两个空格。 */
 .ev{padding:5px 6px;border-radius:6px;font-size:var(--f11);line-height:1.5;
   white-space:pre-wrap;word-break:break-word}
 .ev:hover{background:var(--w6)}
 /* 系统页：几行读数 + Worker 表 + 底部按钮。表头/行用 grid 对齐。 */
 .lines{flex:0 0 auto;font-size:var(--f11);color:var(--muted);line-height:1.75;
   word-break:break-word}
 .wtable{flex:1 1 auto;min-height:0;overflow-y:auto;margin-top:6px;
   scrollbar-width:none;font-size:var(--f11)}
 .wtable::-webkit-scrollbar{width:0;height:0}
 .wrow{display:grid;grid-template-columns:1.3fr .8fr .5fr .7fr .6fr .55fr;
   gap:6px;padding:5px 6px;border-bottom:1px solid var(--w8);align-items:center}
 .wrow.head{color:var(--muted2);font-size:var(--f10);border-bottom:1px solid var(--w14)}
 .wrow:nth-child(even){background:var(--w6)}
 .wrow .wfg{color:var(--accent)}
 /* 手机访问卡片：开关行常驻，开启后展开网址/配对二维码/设备列表。 */
 .lanbox{flex:0 0 auto;margin-top:8px;padding:8px 10px;border-radius:10px;
   border:1px solid var(--w14);background:var(--w6)}
 .lanhead{display:flex;align-items:center;gap:8px}
 .lanrow{display:flex;align-items:center;gap:8px;margin-top:6px;flex-wrap:wrap}
 #lanQrWrap{display:flex;align-items:center;gap:12px;margin-top:8px}
 #lanQr svg{display:block;width:200px;height:200px;border-radius:8px}
 #lanDevices{margin-top:6px}
 .landev{display:flex;align-items:center;gap:8px;padding:3px 0;font-size:var(--f11)}
 .landev .dn{color:var(--text)}
 .landev .dm{color:var(--muted2);font-size:var(--f10);margin-right:auto}
 /* 按键映射：场景行 + 键位/手势表 + 底部提示和接管开关。 */
 .keyhead{flex:0 0 auto;display:flex;align-items:flex-start;gap:8px}
 .scene{display:flex;align-items:baseline;gap:6px;min-width:0;flex:1}
 .dot{width:6px;height:6px;border-radius:3px;flex:0 0 auto;align-self:center}
 .scname{font-weight:600;font-size:var(--f12);color:var(--text);white-space:nowrap}
 .scrule{font-size:var(--f10);color:var(--muted2);overflow:hidden;
   text-overflow:ellipsis;white-space:nowrap}
 .maphead{display:flex;gap:8px;padding:6px 11px 6px 7px;font-size:var(--f10);
   color:var(--muted2);border-bottom:1px solid var(--w10)}
 .maphead .k{width:88px;flex:0 0 88px}
 .maphead .g{flex:1}
 .mapgtitle{display:flex;align-items:center;gap:8px;padding:6px 2px 2px;
   font-size:var(--f10);color:var(--muted2)}
 .hair2{flex:1;height:1px;background:var(--w14)}
 .keyrow{display:flex;align-items:center;gap:8px;padding:2px 4px;
   border-radius:6px}
 .keyrow.sel{background:var(--a26)}
 .keycell{width:88px;flex:0 0 88px;display:flex;align-items:center;gap:7px;
   padding-left:3px;cursor:default;overflow:hidden}
 .keycell .glyph{width:16px;text-align:center;color:var(--muted);flex:0 0 auto}
 .keycell .kname{font-size:var(--f11);color:var(--text-dim);white-space:nowrap;
   overflow:hidden;text-overflow:ellipsis}
 .gcell{flex:1;min-width:0;padding:3px 8px;border-radius:6px;cursor:default}
 .gcell.edit:hover{background:var(--w6)}
 .gcell .t{font-size:var(--f11);color:var(--text-soft);white-space:nowrap;
   overflow:hidden;text-overflow:ellipsis}
 .gcell .e{font-size:var(--f10);color:var(--muted2);white-space:nowrap;
   overflow:hidden;text-overflow:ellipsis}
 .gcell.off .t{color:var(--muted)}
 /* 开关：和 Qt ToggleSwitch 同形状的胶囊。 */
 .sw{width:36px;height:19px;border-radius:10px;border:1px solid var(--w18);
   background:var(--w10);position:relative;padding:0;cursor:default;flex:0 0 auto}
 .sw::after{content:"";position:absolute;left:2px;top:2px;width:13px;height:13px;
   border-radius:50%;background:var(--muted);transition:left .12s ease}
 .sw.on{background:var(--a36);border-color:var(--a64)}
 .sw.on::after{left:19px;background:var(--accent)}
 .swlabel{font-size:var(--f10);color:var(--muted2);white-space:nowrap}
 /* 应用页 */
 .sectitle{font-size:var(--f12);font-weight:700;color:var(--text)}
 .mutedline{font-size:var(--f11);color:var(--muted);line-height:1.6}
 .approw{display:flex;align-items:center;gap:10px;padding:5px 9px;border-radius:6px}
 .approw:hover{background:var(--w6)}
 .approw .aname{width:92px;flex:0 0 92px;font-size:var(--f11);color:var(--text);
   white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
 .approw .astate{flex:1;min-width:0;font-size:var(--f11);color:var(--muted);
   white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
 .approw .astate.miss{color:var(--faint)}
 /* 使用引导：整页内部滚（同 Qt QScrollArea），行是 92px 键 + 说明。 */
 .guide{flex:1 1 auto;min-height:0;overflow-y:auto;padding-right:4px;
   scrollbar-width:none}
 .guide::-webkit-scrollbar{width:0;height:0}
 .gsec{margin-bottom:14px}
 .ghead{font-weight:600;font-size:var(--f11);color:var(--text);margin-bottom:6px}
 .grow{display:flex;gap:8px;font-size:var(--f11);line-height:1.65;margin-bottom:5px}
 .gk{width:92px;flex:0 0 92px;color:var(--muted2)}
 .gt{flex:1;color:var(--text-dim);word-break:break-word}
 .links{display:flex;align-items:center;gap:8px;margin-top:12px;flex-wrap:wrap}
 .empty{flex:1 1 auto;padding:48px 16px;text-align:center}
 .empty h3{font-size:var(--f12);margin:0 0 6px;font-weight:600;color:var(--text)}
 .empty p{font-size:var(--f11);color:var(--muted);margin:0 0 12px;line-height:1.6;
   white-space:pre-line}
 .foot{flex:0 0 auto;display:flex;align-items:center;gap:8px;padding:8px 0 12px}
 .hair{height:1px;background:var(--w10)}
 .hint{font-size:var(--f11);color:var(--ghost);margin-right:auto}
 .btn{border:1px solid var(--w18);background:transparent;color:var(--text-dim);
   font-size:var(--f11);border-radius:9px;padding:6px 12px;cursor:default}
 .btn:hover{color:var(--text);background:var(--w8)}
 .btn.primary{background:var(--accent);border-color:transparent;color:var(--on-accent);
   font-weight:600;padding:6px 14px}
 .btn.primary:hover{background:var(--accent-hover)}
 .btn:disabled{color:var(--faint);background:transparent;border-color:var(--w10)}
 .btn.primary:disabled{background:var(--w8);border-color:transparent}
 /* 窄窗时按钮自己收边距，别把左栏头部那排挤成折行（实测 250px 左栏 +
    「朗读最近/试一句/遥控器」三颗会各自竖排）。 */
 @media (max-width:1040px){
   .btn{padding:6px 9px}
   .lhead .hbtn{padding:2px 4px}
   .vdock .btn{padding:6px 8px}
 }
 .placeholder{flex:1 1 auto;padding:40px;text-align:center;color:var(--muted);
   font-size:var(--f12)}
/* 派发页：Agent 胶囊 + 项目分组 + 会话行；组可折叠，归档/未分类任务单列。 */
.dsphead{flex:0 0 auto;display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.dsp-summary{font-size:var(--f11);color:var(--muted)}
.dspchips{flex:0 0 auto;display:flex;align-items:center;gap:6px;flex-wrap:wrap;
  margin-top:7px}
.dspagent{display:flex;align-items:center;gap:6px;min-height:28px;padding:0 10px;
  border:1px solid var(--w14);border-radius:8px;background:var(--w6);
  color:var(--text-dim);font-size:var(--f11);cursor:default}
.dspagent:hover{background:var(--w10)}
.dspagent.on{border-color:var(--a64);background:var(--a26);color:var(--accent);
  font-weight:600}
.dspagent .st{width:6px;height:6px;border-radius:50%;background:var(--faint);
  flex:0 0 6px}
.dspagent .st.on{background:var(--ok)}
.dspagent .n{font-size:var(--f10);color:var(--faint)}
.dspagent.on .n{color:var(--accent)}
#dspList{margin-top:4px}
.dspgroup{margin:0}
.dspghead{display:flex;align-items:center;gap:6px;width:100%;min-height:32px;
  margin:0;padding:5px 5px;border:0;border-radius:6px;background:transparent;
  color:var(--muted2);font-size:var(--f11);font-weight:600;text-align:left;
  cursor:default}
.dspghead:hover{background:var(--w6)}
.dspgchev{font-size:13px;line-height:1;color:var(--faint);
  transform:rotate(90deg);transition:transform .16s ease;flex:0 0 auto}
.dspgroup.closed .dspgchev{transform:rotate(0deg)}
.dspgname{flex:0 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap}
.dspgarch{flex:0 0 auto;font-size:9.5px;font-weight:600;color:var(--warn);
  border:1px solid var(--w14);border-radius:6px;padding:0 5px;line-height:1.5}
.dspgcount{margin-left:auto;flex:0 0 auto;font-size:var(--f10);
  color:var(--faint);background:var(--w6);border-radius:7px;padding:1px 6px}
/* 已归档是整个列表的底部分区：一条分隔线把它跟活跃项目隔开，整区默认收起。
   归档会话仍按项目分组，但组头不再每行重复「已归档」徽标——区头已经说过了。
   清理入口就长在区头右侧：清理的对象是这一区，不该跑到顶栏去够。 */
.dspsec{margin-top:12px;border-top:1px solid var(--w14);padding-top:3px}
.dspsechead{display:flex;align-items:center;gap:6px;min-height:30px}
.dspsecfold{display:flex;align-items:center;gap:6px;flex:1;min-width:0;
  margin:0;padding:4px 5px;border:0;border-radius:6px;background:transparent;
  color:var(--muted);font-size:var(--f10);font-weight:600;letter-spacing:.02em;
  text-align:left;cursor:default}
.dspsecfold:hover{background:var(--w6)}
.dspsecchev{flex:0 0 auto;font-size:12px;line-height:1;color:var(--faint);
  transform:rotate(90deg);transition:transform .16s ease}
.dspsec.closed .dspsecchev{transform:rotate(0deg)}
.dspsecname{flex:0 0 auto}
.dspseccount{flex:0 0 auto;margin-left:auto;font-size:var(--f10);color:var(--faint);
  background:var(--w6);border-radius:7px;padding:1px 6px}
.dspsecclean{flex:0 0 auto;height:22px;padding:0 8px;border:1px solid var(--w14);
  border-radius:6px;background:transparent;color:var(--muted2);
  font-size:var(--f10);cursor:default}
.dspsecclean:hover{background:var(--w10);color:var(--text)}
.dspsecclean.on{border-color:var(--a64);background:var(--a26);color:var(--accent);
  font-weight:600}
.dspsecclean:disabled{opacity:.45}
.dspsecempty{padding:9px 6px;font-size:var(--f10);color:var(--muted2)}
.dspsecbody[hidden]{display:none}
.dspsecbody .dspghead{color:var(--muted);font-weight:500}
.dspbody[hidden]{display:none}
.dspsess{display:flex;align-items:center;gap:10px;padding:7px 9px;
  border-radius:8px;border-bottom:1px solid var(--w8);cursor:default}
.dspsess:hover{background:var(--w6)}
.dspsess.readonly{opacity:.82}
.dspsess .t{flex:1;min-width:0}
.dspsess .title{font-size:var(--f12);font-weight:600;color:var(--text);
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.dspsess .sub{display:flex;align-items:center;gap:8px;margin-top:2px;
  font-size:var(--f10);color:var(--muted2);min-width:0}
.dspsess .sub .u{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.dspsess .rt{flex:0 0 auto;color:var(--faint)}
.dspsess .ro{flex:0 0 auto;color:var(--warn)}
.dspsess .st{flex:0 0 auto;font-size:var(--f10);padding:1px 6px;border-radius:7px;
  background:var(--w6)}
.dspsess .arr{flex:0 0 auto;font-size:16px;color:var(--faint)}
.dspempty{padding:34px 18px;text-align:center;color:var(--muted);
  font-size:var(--f11);line-height:1.7}
.dspempty b{display:block;color:var(--text);font-size:var(--f12);margin-bottom:5px}
.dspnotice{flex:0 0 auto;margin-top:6px;padding:6px 9px;border-radius:8px;
  font-size:var(--f10);line-height:1.55;color:var(--text-dim);
  background:var(--w6);border:1px solid var(--w14);word-break:break-all}
.dspnotice.bad{color:var(--warn);border-color:var(--w30)}
.dspsess.clean{cursor:default}
.dspsess.clean:hover{background:var(--a26)}
.dspsess.clean.sel{background:var(--a26);border-color:var(--a64)}
.dspcheck{width:16px;height:16px;border-radius:5px;border:1.5px solid var(--w30);
  background:var(--w6);flex:0 0 16px;display:flex;align-items:center;
  justify-content:center;color:var(--on-accent);font-size:10px;font-weight:700}
.dspcheck.on{background:var(--accent);border-color:var(--accent)}
.dspcheck.on::before{content:'✓'}
.dspdel{flex:0 0 auto;border:1px solid var(--w14);border-radius:6px;
  background:transparent;color:var(--faint);font-size:var(--f10);padding:2px 7px;
  cursor:default}
.dspdel:hover{color:var(--danger);border-color:var(--w30);background:var(--w6)}
.dspdel:disabled{opacity:.45}
.dspbatch{flex:0 0 auto;display:flex;align-items:center;gap:8px;padding:8px 2px 12px;
  border-top:1px solid var(--w14);margin-top:4px}
.dspbatch #dspBatchCount{font-size:var(--f11);font-weight:600;color:var(--accent)}
.dspbatch .btn.primary{background:var(--danger);border-color:var(--danger)}
/* 聊天页：点待办进入，打开即最新记录；旧记录上翻按页加载。
   滚动条隐藏（全局约定），上下渐隐暗示可滚；新消息 pill 不打断读历史。 */
 .chathead{flex:0 0 auto;display:flex;align-items:center;gap:8px}
 .chathead .name{font-weight:700;font-size:var(--f13);overflow:hidden;
   text-overflow:ellipsis;white-space:nowrap}
 .chatwrap{flex:1 1 auto;min-height:0;position:relative;margin-top:8px}
 .chatlist{position:absolute;inset:0;overflow-y:auto;scrollbar-width:none;
   display:flex;flex-direction:column;gap:8px;padding:12px 6px}
 .chatlist::-webkit-scrollbar{width:0;height:0}
 .chatfade{position:absolute;left:0;right:0;height:16px;pointer-events:none;z-index:1}
 .chatfade.top{top:0;background:linear-gradient(180deg,var(--surface),transparent)}
 .chatfade.bottom{bottom:0;background:linear-gradient(0deg,var(--surface2),transparent)}
 /* Agent 回复平铺全文（长代码/长文不用气泡挤宽度），「我」的话保留右侧气泡。 */
 .bub{flex:0 0 auto;font-size:var(--f12);line-height:1.6;white-space:pre-wrap;
   word-break:break-word}
 .bub.user{align-self:flex-end;max-width:82%;padding:8px 12px;border-radius:12px;
   background:var(--a26);border:1px solid var(--a64);color:var(--text)}
 .bub.assistant{align-self:stretch;max-width:100%;padding:2px 4px;
   color:var(--text-soft)}
 .bub .who{display:block;font-weight:600;font-size:var(--f10);color:var(--muted);
   margin-bottom:2px}
 .bub .bt{display:block;font-size:var(--f10);color:var(--muted2);margin-top:4px}
 .reqcard{flex:0 0 auto;align-self:stretch;margin:6px 0;padding:10px 12px;
   border-radius:10px;background:var(--w8);border:1px solid var(--a64)}
 .reqbtn{display:block;width:100%;text-align:left;margin:4px 0;padding:8px 10px;
   border-radius:8px;border:1px solid var(--a36);background:transparent;
   font-size:var(--f12);color:var(--text)}
 .reqbtn:hover{background:var(--w6)}
 .reqbtn.on{border-color:var(--accent);background:var(--a26)}
 .reqcustom{display:block;width:100%;box-sizing:border-box;margin:4px 0;
   padding:8px 10px;border-radius:8px;border:1px solid var(--a36);
   background:transparent;font-size:var(--f12);color:var(--text)}
 .reqsend{margin-top:6px}
 .reqrow{display:flex;gap:8px;margin-top:6px}
 .reqrow .reqbtn{flex:1;width:auto;text-align:center}
 .reqstate{font-size:var(--f11);color:var(--muted);margin-top:4px}
 .reqhead{font-weight:600;font-size:var(--f12);color:var(--text);margin-bottom:4px}
 .reqreason{font-size:var(--f11);color:var(--text-soft);white-space:pre-wrap;
   word-break:break-word;margin-bottom:4px}
 .reqq{margin:4px 0}
 .reqqt{font-size:var(--f12);color:var(--text);white-space:pre-wrap;word-break:break-word}
 .reqopt{font-size:var(--f11);color:var(--text-soft);padding:1px 0 1px 8px}
 .reqhint{font-size:var(--f10);color:var(--muted2);margin-top:6px}
 .chatload{align-self:center;font-size:var(--f10);color:var(--muted2);padding:4px;
   flex:0 0 auto}
 .chatempty{flex:1;display:flex;align-items:center;justify-content:center;
   color:var(--muted);font-size:var(--f12);text-align:center;padding:20px;
   line-height:1.7;white-space:pre-line}
 .chatfoot{flex:0 0 auto;display:flex;gap:8px;margin-top:8px}
 .chatfoot input{flex:1;min-width:0;font-size:var(--f12);color:var(--text);
   background:var(--w6);border:1px solid var(--w14);border-radius:9px;
   padding:7px 10px;font-family:inherit}
 .chatfoot input::placeholder{color:var(--muted2)}
 .chatfoot input:focus{outline:none;border-color:var(--accent);background:var(--w8)}
 .newpill{position:absolute;left:50%;transform:translateX(-50%);bottom:10px;z-index:2;
   border:1px solid var(--a64);background:var(--accent);color:var(--on-accent);
   font-size:var(--f11);font-weight:600;border-radius:12px;padding:4px 12px;
   cursor:default}

 /* ---------- 整窗外壳：顶栏 + 左栏（Qt chrome 藏起来后由这里画） ---------- */
 #topbar{flex:0 0 44px;display:flex;align-items:center;gap:10px;
   padding:8px 10px 4px 16px}
 .brandlogo{width:22px;height:22px;border-radius:6px;background:var(--accent);
   color:var(--on-accent);font-weight:700;font-size:var(--f12);
   display:flex;align-items:center;justify-content:center;flex:0 0 22px}
 .brand{font-size:var(--f13);font-weight:700;color:var(--text)}
 /* 监听态只留一颗点（2026-11-17 用户：监听中只保留绿点）：胶囊的边框和「监听中」
    三个字都是常驻噪音，而顶栏右侧的节奏应该由图标主导。点还是开关本身——
    点一下开始/停止，悬停给 tooltip，无障碍名在 aria-label 上。
    点了以外的地方不再是 6px 小点：8px + 一圈淡绿光晕，静默时也能看出「在听」。 */
 .pill{width:28px;height:26px;display:flex;align-items:center;
   justify-content:center;border:0;border-radius:7px;background:transparent;
   padding:0;flex:0 0 28px;cursor:default}
 .pill:hover{background:var(--w10)}
 .pill .dot{width:8px;height:8px;border-radius:50%;background:var(--line);
   flex:0 0 8px;transition:background .12s,box-shadow .12s}
 .pill .dot.on{background:var(--ok);box-shadow:0 0 0 3px rgba(63,185,80,.16)}
 .dot{width:6px;height:6px;border-radius:3px;background:var(--line);flex:0 0 6px}
 .dot.on{background:var(--ok)}
 .hbtn{border:0;background:transparent;color:var(--text-dim);
   font-size:var(--f11);padding:2px 6px;cursor:default}
 .hbtn:hover{color:var(--accent)}
 .hbtn.boxed{border:1px solid var(--w18);border-radius:8px;padding:4px 10px}
 .hbtn.boxed:hover{background:var(--w8);color:var(--text)}
 .hbtn:disabled{color:var(--faint)}
 /* 顶栏右侧两排键（2026-11-17 用户：能图标化的就图标化，大小要和谐）：
    检查更新/主题/新手引导从文字键改成图标键，和窗口键共用 28×26 网格、7px 圆角、
    16px 图标（currentColor，跟主题与浅色模式）、同一档悬停底。文字按钮排一排
    宽度差太多，一列看过去节奏是断的；图标等宽以后整排才齐。
    类名用 topico 不用 icobtn —— 后者是列表行内的动作键，同名会互相覆盖。 */
 .hgroup{display:flex;align-items:center;gap:2px;margin-left:4px}
 .wgroup{display:flex;align-items:center;gap:2px;margin-left:8px}
 .topico{width:28px;height:26px;border:0;border-radius:7px;background:transparent;
   color:var(--text-dim);display:flex;align-items:center;justify-content:center;
   flex:0 0 28px;padding:0;cursor:default}
 .topico svg{width:16px;height:16px;display:block;pointer-events:none}
 .topico:hover{background:var(--w10);color:var(--text)}
 .topico:active{background:var(--w14)}
 /* 新手引导不再画外框（2026-11-17 用户：问号外面矩形框多余）：三颗动作键完全同构。 */
 .winbtn{width:28px;height:26px;border:0;border-radius:7px;background:transparent;
   color:var(--text-dim);display:flex;align-items:center;justify-content:center;
   cursor:default;flex:0 0 28px;padding:0}
 .winbtn svg{width:16px;height:16px;display:block;pointer-events:none}
 .winbtn:hover{background:var(--w10);color:var(--text)}
 .winbtn.close:hover{background:var(--danger);color:#fff}
 .winbtn .grest{display:none}
 .winbtn.maxed .gmax{display:none}
 .winbtn.maxed .grest{display:block}
 #main{flex:1 1 auto;min-height:0;display:flex;gap:12px;padding:12px 14px 14px}
 #left{flex:0 0 286px;width:286px;min-height:0;display:flex;flex-direction:column}
 #lremote{flex:1;min-height:0;display:flex;flex-direction:column;gap:12px}
 .panel{display:flex;flex-direction:column;gap:8px;min-height:0;
   padding:13px 16px;border-radius:16px;border:1px solid var(--w18);
   background:linear-gradient(180deg,var(--surface),var(--surface2))}
 #vcard{flex:1 1 auto}
 #rcard{flex:0 0 auto;max-height:392px}
 #lvoice{flex:1 1 auto}
 .lhead{display:flex;align-items:center;gap:8px;flex:0 0 auto}
 .ltitle{font-size:var(--f11);font-weight:700;color:var(--muted);
   letter-spacing:1px;white-space:nowrap;flex:0 0 auto}
 .lhead .spacer{margin-left:auto}
 .vstate{border:1px solid var(--w14);background:var(--w6);border-radius:10px;
   padding:8px;font-size:var(--f11);color:var(--text-dim);min-height:48px;
   display:flex;flex-direction:column;justify-content:center;gap:2px;
   word-break:break-word}
 .vstate.accent{color:var(--accent)}
 .vstate.warn{color:var(--warn)}
 .vstate.drop{border:1px dashed var(--a64);background:var(--a26);
   color:var(--accent)}
 .vstate .tr{color:var(--text)}
 .tools{display:flex;gap:6px;flex:0 0 auto}
 .wave{height:40px;flex:0 0 40px;border-radius:10px;border:1px solid var(--w18);
   background:var(--w8);display:flex;align-items:center;justify-content:center;
   gap:2px;cursor:default;overflow:hidden;padding:0}
 .wave.on{background:var(--a26);border-color:var(--a64)}
 .wave.rec{background:rgba(63,185,80,.14);border-color:rgba(63,185,80,.45)}
 .wave i{width:3px;border-radius:2px;background:var(--muted);height:6px}
 .wave.on i{background:var(--accent)}
 .wave.rec i{background:var(--ok)}
 .picker{display:flex;gap:6px;flex:0 0 auto}
 .picker select{flex:1;min-width:0}
 .lfoot{font-size:var(--f10);color:var(--muted);flex:0 0 auto}
 .badge{font-size:var(--f10);color:var(--muted2);background:var(--w8);
   border:1px solid var(--w14);padding:2px 8px;border-radius:8px}
 .rbody{display:flex;gap:10px;flex:1 1 auto;min-height:0}
 .rdev{flex:0 0 92px;width:92px;height:268px;background:#0b0e14;
   border-radius:14px;position:relative}
 .rk{position:absolute;width:22px;height:22px;border-radius:50%;
   background:#161d2a;border:1px solid #232c3b;color:#8f98a3;
   display:flex;align-items:center;justify-content:center;font-size:10px}
 .rk.vol{width:12px;height:34px;border-radius:6px}
 .rk.mode{border-color:var(--a64);color:var(--accent)}
 .rk.lit{border-color:var(--accent);color:var(--accent);background:var(--a26)}
 .rlegend{flex:1;min-width:0;display:flex;flex-direction:column;gap:5px;
   font-size:var(--f10);overflow:hidden}
 .rl{display:flex;gap:6px;align-items:baseline;color:var(--text-dim);
   white-space:nowrap;overflow:hidden}
 .rl .g{flex:0 0 14px;width:14px;text-align:center;color:var(--muted)}
 .rl .a{overflow:hidden;text-overflow:ellipsis}
 .rl.ir{color:var(--muted)}
 .rl.lit{color:var(--accent)}
 .rfoot{display:flex;align-items:center;gap:7px;font-size:var(--f10);
   color:var(--muted);flex:0 0 auto}
 .rfoot .spacer{margin-left:auto}
 .vline{font-size:var(--f10);color:var(--muted);flex:0 0 auto}
 .vhist{flex:1 1 auto;min-height:0;overflow-y:auto;scrollbar-width:none;
   display:flex;flex-direction:column;gap:2px}
 .vhist::-webkit-scrollbar{width:0;height:0}
 .vh{font-size:var(--f11);padding:4px 6px;border-radius:6px;
   word-break:break-word;cursor:pointer;flex:0 0 auto;max-width:88%}
 .vh.out{color:var(--text);text-align:right;margin-left:auto;background:var(--w8)}
 .vh.in{color:var(--accent);text-align:left;margin-right:auto;background:var(--w6)}
 .vh:hover{background:var(--w6)}
 .vempty{flex:1 1 auto;display:flex;align-items:center;justify-content:center;
   color:var(--muted);font-size:var(--f11);text-align:center}
 .vdock{display:flex;flex-direction:column;gap:6px;flex:0 0 auto}
 .vrow{display:flex;gap:6px}
 /* 输入框和下拉同一套：半透明底 + 细描边，悬停加深一档、聚焦换主题色。
    原来用 --raised（不透明 #161d2a）+ --line（浅板岩灰），在美术主题的
    半透明卡片上就是一块实心黑砖（2026-11-10 用户反馈「就重在下拉/输入框」）。 */
 .vrow input{font-size:var(--f11);color:var(--text);
   background:var(--w6);border:1px solid var(--w14);border-radius:8px;
   font-family:inherit}
 .vrow input{flex:1;min-width:0;padding:5px 8px}
 .vrow input::placeholder{color:var(--muted2)}
 .vrow input:hover{background:var(--w8);border-color:var(--w30)}
 .vrow input:focus{outline:none;background:var(--w8);
   border-color:var(--accent)}
 .btn.toggled{background:var(--a26);border-color:var(--a64);color:var(--accent)}
 #right{flex:1 1 auto;min-width:0;min-height:0;display:flex;
   flex-direction:column}
 #right .tabs{padding:0 2px}
 #right .card{margin:10px 0 0}
 /* ---------- 无边框窗口的边缘拖拽热区（2026-11-17 用户：整体界面宽高可拉伸缩小） ----------
    WebView2 铺满整窗，Chromium 的子窗把 WM_NCHITTEST 全吃成 HTCLIENT，系统认不出
    「鼠标在窗口边上」，所以拉边改由页面这层热区发起：按下就发 winResize，Qt 那边
    转成 WM_NCLBUTTONDOWN(HTxxx) 进系统缩放循环。5px 细条不挡点击（顶栏按钮在 8px
    内边距之下），四角 10px 给斜向缩放。 */
 .rz{position:fixed;z-index:90}
 .rz-t{left:0;right:0;top:0;height:5px;cursor:ns-resize}
 .rz-b{left:0;right:0;bottom:0;height:5px;cursor:ns-resize}
 .rz-l{top:0;bottom:0;left:0;width:5px;cursor:ew-resize}
 .rz-r{top:0;bottom:0;right:0;width:5px;cursor:ew-resize}
 .rz-tl{left:0;top:0;width:10px;height:10px;cursor:nwse-resize}
 .rz-tr{right:0;top:0;width:10px;height:10px;cursor:nesw-resize}
 .rz-bl{left:0;bottom:0;width:10px;height:10px;cursor:nesw-resize}
 .rz-br{right:0;bottom:0;width:10px;height:10px;cursor:nwse-resize}
 /* 窗口拉窄/拉矮时先让布局让步：左栏收窄、品牌名收掉、遥控器那颗设备图收掉
    （图例还留着，功能没少）；页签在更窄时横向可滚，不换行不裁切。 */
 .tabs{overflow-x:auto;overflow-y:hidden;scrollbar-width:none}
 .tabs::-webkit-scrollbar{width:0;height:0}
 @media (max-width:1040px){
   /* 左栏收到 268px：再窄 lhead 那排（语音 + 朗读最近/试一句/遥控器）就要折行，
     底部那排下拉也会被挤成「Sens」（实测 212/250px 都踩到）。 */
   #left{flex:0 1 268px;min-width:268px}
   .brand{display:none}
   .tab{padding:7px 10px}
   .lhead{gap:6px}
 }
 /* 头部那排按钮永不折行：窄窗宁可让标题让位，也不要把「语音」竖排。 */
 .lhead{flex-wrap:nowrap;white-space:nowrap;min-width:0}
 @media (max-height:640px){
   .rdev{display:none}
   #rcard{max-height:none}
   #topbar{flex:0 0 40px;padding:6px 10px 2px 14px}
 }
</style></head><body>
<div class="rz rz-t" data-dir="top"></div>
<div class="rz rz-b" data-dir="bottom"></div>
<div class="rz rz-l" data-dir="left"></div>
<div class="rz rz-r" data-dir="right"></div>
<div class="rz rz-tl" data-dir="topleft"></div>
<div class="rz rz-tr" data-dir="topright"></div>
<div class="rz rz-bl" data-dir="bottomleft"></div>
<div class="rz rz-br" data-dir="bottomright"></div>
<div id="topbar">
  <span class="brandlogo">A</span><span class="brand">AgentRMC</span>
  <span class="spacer" style="margin-left:auto"></span>
  <button class="pill" id="listenPill" title="监听开关：点击开始 / 停止监听" aria-label="监听开关"><span class="dot" id="listenDot"></span></button>
  <span class="hgroup">
  <button class="topico" id="hUpdate" title="检查更新：看看有没有新版本" aria-label="检查更新"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><g transform="translate(12 12) scale(0.8) translate(-12 -12)" stroke-width="2.4"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></g></svg></button>
  <button class="topico" id="hTheme" title="主题与外观：换主题、调卡片不透明度与背景压暗" aria-label="主题"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M20.1 14.9A8.6 8.6 0 1 0 16.3 19.5Q14.2 14.2 20.1 14.9Z"/><circle cx="8.6" cy="10.2" r="1.05" fill="currentColor" stroke="none"/><circle cx="12" cy="7.8" r="1.05" fill="currentColor" stroke="none"/><circle cx="15.4" cy="10.2" r="1.05" fill="currentColor" stroke="none"/></svg></button>
  <button class="topico" id="hGuide" title="新手引导：没有遥控器怎么用、怎么配对、怎么接入 Agent" aria-label="新手引导"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="8.6"/><path d="M9.5 9.7a2.6 2.6 0 0 1 5.2.8c0 1.8-2.6 2.2-2.6 3.9"/><circle cx="12" cy="17.3" r="0.95" fill="currentColor" stroke="none"/></svg></button>
  </span>
  <span class="wgroup">
  <button class="winbtn" id="wMin" title="最小化" aria-label="最小化"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M6 12h12"/></svg></button>
  <button class="winbtn" id="wMax" title="最大化 / 还原（双击顶栏）" aria-label="最大化 / 还原"><svg class="gmax" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linejoin="round"><rect x="5.6" y="5.6" width="12.8" height="12.8" rx="2"/></svg><svg class="grest" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><rect x="8.6" y="8.6" width="9.8" height="9.8" rx="2"/><path d="M6.1 15.4V8.1a2 2 0 0 1 2-2h7.3"/></svg></button>
  <button class="winbtn close" id="wClose" title="关闭" aria-label="关闭"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M6.6 6.6l10.8 10.8M17.4 6.6L6.6 17.4"/></svg></button>
  </span>
</div>
<div id="main">
<div id="left">
  <div id="lremote">
    <div class="panel" id="vcard">
      <div class="lhead"><span class="ltitle">语音</span><span class="spacer"></span><select id="vBackend" title="语音怎么识别：转发当前应用的快捷键，或本地 / 云端模型"></select></div>
      <div class="vstate" id="vState"></div>
      <div class="tools"><button class="btn" id="vTest" title="录 3 秒并转写一次，用来确认本地模型是否正常">试一句</button><button class="btn" id="vSettings">模型设置</button><button class="btn" id="vMain" title="切到无遥控器的简洁语音界面：波形键一键说话">语音主界面</button></div>
      <button class="wave" id="vWave" title="语音键：点击说话，再点结束"></button>
      <div class="picker"><select id="pkAgent"></select><select id="pkMode"></select></div>
      <div class="lfoot" id="vFoot"></div>
    </div>
    <div class="panel" id="rcard">
      <div class="lhead"><span class="ltitle">遥控器</span><span class="spacer"></span><button class="btn" id="toVoice" title="把这块换成语音主界面（打字/说话都行）">语音</button><span class="badge" id="rBadge">待机</span></div>
      <div class="rbody"><div class="rdev" id="rDev"></div><div class="rlegend" id="rLegend"></div></div>
      <div class="rfoot"><span class="dot" id="rDot"></span><span id="rState"></span><span class="spacer"></span><button class="btn" id="btSet" title="打开 Windows 蓝牙设置">蓝牙设置</button></div>
    </div>
  </div>
  <div class="panel" id="lvoice" hidden>
    <div class="lhead"><span class="dot" id="vDot2"></span><span class="ltitle" style="color:var(--text);letter-spacing:0">语音</span><span class="spacer"></span><button class="hbtn" id="readLast" title="用桌宠的 TTS 念出最近一条转写">朗读最近</button><button class="hbtn" id="vTest2" title="录 3 秒并转写一次，用来确认本地模型是否正常">试一句</button><button class="hbtn" id="toRemote" title="把左栏这块切回遥控器卡">遥控器</button></div>
    <button class="wave" id="vWave2" title="语音键：点击说话，再点结束"></button>
    <div class="picker"><select id="pkAgent2"></select><select id="pkMode2"></select></div>
    <div class="vline" id="vLine">点击开始 · 再点结束</div>
    <div class="vhist" id="vHist"></div>
    <div class="vempty" id="vEmpty">说完的话会攒在这里</div>
    <div class="vdock">
      <div class="vrow"><input type="text" id="vTyped" placeholder="说一句话 · Enter 发送"><button class="btn" id="vSend">发送</button></div>
      <div class="vrow"><select id="vBackend2" style="flex:1 1 auto"></select><button class="btn" id="chatBtn" style="flex:0 0 auto" title="桌宠对话模式：说话发给模型聊，回复用 TTS 念出来">对话</button><button class="btn" id="ttsBtn" style="flex:0 0 auto" title="语音反馈：Base URL / API Key / TTS 模型自填">语音</button><button class="btn" id="vSettings2" style="flex:0 0 auto" title="模型设置：识别模型 / 云端 ASR / 桌宠对话">模型</button></div>
    </div>
  </div>
</div>
<div id="right">
<div class="tabs" id="tabs"></div>
<div class="card">
<div id="work" class="page">
  <div class="head">
    <span class="total" id="total"></span>
    <span id="chips"></span>
    <span class="spacer"></span>
    <select id="filter"></select>
    <button class="ghost" id="multi" title="勾选多条待办，一次性标为已处理">多选</button>
    <button class="ghost" id="clear">清空已完成</button>
  </div>
  <div class="health" id="health"></div>
  <div class="list" id="list"></div>
</div>
<div id="dispatch" class="page" hidden>
  <div class="dsphead">
    <span class="total">派发会话</span>
    <span class="dsp-summary" id="dspSummary"></span>
    <span class="spacer"></span>
    <button class="ghost" id="dspRefresh" title="重新读取各 Agent 的最近会话">刷新会话</button>
  </div>
  <div class="dspchips" id="dspAgents"></div>
  <div class="dspnotice" id="dspNotice" hidden></div>
  <div class="list" id="dspList"></div>
  <div class="dspempty" id="dspEmpty" hidden>
    <b>还没有可读的会话</b>
    Agent 产生过会话后，这里会按项目列出来。目录只读，不改动宿主当前会话。
  </div>
  <div class="dspbatch" id="dspBatch" hidden>
    <span id="dspBatchCount">已选 0 条</span>
    <button class="ghost" id="dspCheckAll">全选</button>
    <span class="spacer"></span>
    <button class="btn primary" id="dspDelete">删除所选</button>
    <button class="ghost" id="dspBatchCancel">取消</button>
  </div>
</div>
<div id="dchat" class="page" hidden>
  <div class="chathead">
    <button class="btn" id="dchatBack" title="回到派发列表">‹ 派发</button>
    <span class="name" id="dchatTitle"></span>
    <span class="stage" id="dchatStage"></span>
    <span class="spacer"></span>
    <button class="ghost" id="dchatRefresh" title="重新读取这个会话的最新记录">刷新</button>
    <span class="time" id="dchatMeta"></span>
  </div>
  <div class="chatwrap">
    <div class="chatfade top"></div>
    <div class="chatlist" id="dchatList"></div>
    <div class="chatfade bottom"></div>
  </div>
  <div class="chatfoot">
    <input id="dchatText" placeholder="写一句任务，派到这个会话（Enter 发送）">
    <button class="btn primary" id="dchatSend">派发</button>
  </div>
  <div class="notice" id="dchatNotice" hidden style="margin-top:6px"></div>
</div>
<div id="chat" class="page" hidden>
  <div class="chathead">
    <button class="btn" id="chatBack" title="回到待办列表">‹ 待办</button>
    <span class="name" id="chatTitle"></span>
    <span class="stage" id="chatStage"></span>
    <span class="spacer"></span>
    <span class="time" id="chatMeta"></span>
  </div>
  <div class="chatwrap">
    <div class="chatfade top"></div>
    <div class="chatlist" id="chatList"></div>
    <div class="chatfade bottom"></div>
    <button class="newpill" id="chatNew" hidden>有新消息 ↓</button>
  </div>
  <div class="chatfoot">
    <input id="chatText" placeholder="派回这条会话的指令（Enter 发送，回执到了转进行中）">
    <button class="btn primary" id="chatSend">发送</button>
  </div>
  <div class="notice" id="chatNotice" hidden style="margin-top:6px"></div>
  <button class="btn" id="chatRestart" hidden style="margin-top:6px;width:100%;
    color:var(--warn);border-color:var(--warn)"
    title="ZCode 对话区不热刷新：重启宿主后打开会话即可看到派发消息全文。会中断 ZCode 里正在跑的其它轮次">重启 ZCode</button>
</div>
<div id="empty" class="empty" hidden>
  <h3>没有需要你处理的事</h3>
  <p>Agent 等你确认、或任务做完时，这里会自动出现一条。
先给 Qoder / ZCode / WorkBuddy 装上 agentrmc-bridge 插件。</p>
  <button class="btn" id="workGuide">看怎么接入 Agent</button>
</div>
<div id="events" class="page" hidden>
  <div class="head">
    <span class="total" id="evCount">0 条</span>
    <span class="spacer"></span>
    <button class="ghost" id="evClear">清空</button>
  </div>
  <div class="list" id="evList"></div>
  <div class="empty" id="evEmpty" hidden>
    <h3>还没有事件</h3>
    <p>遥控器按键、语音转写、Agent 通知都会按时间记在这里，
排查「按了没反应」时先看这一页。</p>
  </div>
</div>
<div id="system" class="page" hidden>
  <div class="head"><span class="sectitle">系统健康</span></div>
  <div class="lines" id="sysLines" style="margin-top:8px"></div>
  <div class="lanbox" id="lanBox">
    <div class="lanhead">
      <span class="sectitle">手机访问</span>
      <span class="swlabel" id="lanState">已关闭</span>
      <span class="spacer"></span>
      <button class="sw" id="lanSw" title="开启后局域网里的手机可配对访问待办与派发；默认关闭，可随时吊销设备"></button>
    </div>
    <div id="lanBody" hidden>
      <div class="mutedline" id="lanUrls" style="margin-top:6px"></div>
      <div class="lanrow">
        <button class="btn" id="lanPairBtn">显示配对二维码</button>
        <span class="mutedline" id="lanPairHint">手机扫码即可配对 · 10 分钟内有效</span>
      </div>
      <div id="lanQrWrap" hidden>
        <div id="lanQr"></div>
        <div class="mutedline" id="lanCode" style="flex:1;min-width:0;word-break:break-all"></div>
      </div>
      <div id="lanDevices"></div>
    </div>
  </div>
  <div class="wtable" id="sysWorkers"></div>
  <div class="foot" style="border-top:0;padding:8px 0 4px">
    <button class="btn" id="sysRefresh">刷新</button>
    <button class="btn" id="sysBridge" title="一键复制安装话术：粘给任意一个正在干活的 Agent，它自己按所在宿主装好。顺便看各家装没装、进程里跑的是哪一版。">接入 Agent</button>
    <button class="btn" id="sysUpgrade" title="升级 Qoder / Qoder CN / ZCode 三家已装的插件。WorkBuddy AI / WorkBuddy 只在这里报状态：它们的市场源带自动更新，宿主自己拉新版，重启宿主生效。">升级联动插件</button>
    <button class="btn" id="sysOpenDir">打开数据目录</button>
  </div>
</div>
<div id="keymap" class="page" hidden>
  <div class="keyhead">
    <div class="scene">
      <span class="dot" id="kmDot"></span>
      <span class="scname" id="kmName"></span>
      <span class="scrule" id="kmRule"></span>
    </div>
    <select id="kmScene" title="跟随前台：切窗口自动换映射；选具体场景：固定预览这个场景"></select>
    <button class="btn" id="kmProfiles" title="直接编辑 profiles.json">高级配置</button>
    <button class="btn" id="kmReload">重新加载</button>
  </div>
  <div class="maphead"><span class="k">键位</span><span class="g">单击</span><span class="g">双击</span><span class="g">长按</span></div>
  <div class="list" id="kmList" style="margin-top:2px"></div>
  <div class="foot">
    <span class="hint" id="kmHint">点击手势格改键 · 自动保存</span>
    <span class="swlabel" title="打开后当前前台场景生效：吞掉映射键原生事件，再按映射注入；实体键盘同名键也会按映射处理；双击键的单击延迟约 0.3 秒">接管原生键</span>
    <button class="sw" id="kmTakeover"></button>
  </div>
</div>
<div id="apps" class="page" hidden>
  <div class="head">
    <span class="sectitle">应用启动</span>
    <span class="spacer"></span>
    <button class="ghost" id="apAdd" title="加一个自定义应用：填名字和一个 exe/快捷方式/网址路径，之后说「打开 名字」就能启动">添加应用</button>
    <button class="ghost" id="apRescan" title="装好或删掉应用后点这里，重新扫一遍开始菜单">重新扫描</button>
  </div>
  <div class="mutedline" style="margin-top:6px">说「打开剪映」「打开 Qoder」「打开浏览器」就启动，不用再打字；开始菜单里的其它应用按名字说也能开（例如「打开 WorkBuddy AI」）。</div>
  <div class="hair" style="margin-top:8px"></div>
  <div class="list" id="apList"></div>
  <div class="foot" style="flex-wrap:wrap">
    <span class="hint" id="apSummary" style="white-space:normal;line-height:1.5"></span>
    <span class="swlabel" title="开着：本地识别出的「打开 xx」直接启动应用，不再贴进输入框；想把这句话原样打给 Agent，就关掉它">语音启动</span>
    <button class="sw" id="apVoice"></button>
    <span class="swlabel" title="开着：说「音量调到 50」「亮度暗一点」「下一首」「关屏幕」直接执行，不再贴进输入框；关机/重启/睡眠/休眠要再说一句「确认关机」这样的口令才动手">语音系统控制</span>
    <button class="sw" id="apSystem"></button>
  </div>
</div>
<div id="guide" class="page" hidden>
  <div class="guide" id="guBody"></div>
</div>
<div class="foot" id="foot" style="flex-wrap:wrap">
  <span class="hint" id="hint"></span>
  <button class="btn primary" id="ackBatch" hidden>✓ 标为已处理</button>
</div>
</div>
</div>
</div>
<script>
)HTML")
                         + qrLib
                         + QString::fromUtf8(R"HTML(
// 首帧的状态嵌在文档里；之后 Qt 只调 window.__setState(json) 换数据，不重载文档。
let STATE = )HTML")
                         + jsEscape(stateJson)
                         + QString::fromUtf8(R"HTML(;
let WORK = {items:[]}, WORKERS = [], EVENTS = [], now = Date.now();
const COLOR = {'needs-me':'var(--pending)','review':'var(--ok)','working':'var(--info)'};
// 事件按 kind 上色，和 Qt 侧 appendEvent 同一套：错误红、听写/系统命令绿、
// 状态灰（muted token）、其余（按键等）用 text.dim。阶段色一样两边硬编码同值。
const EVCOLOR = {error:'var(--danger)',voice:'var(--ok)',system:'var(--ok)',status:'var(--muted)'};
let evSeen = 0;
let SYSTEM = {}, KEYMAP = {}, APPS = {}, GUIDE = {};
let HEADER = {}, LEFT = {};
const LABEL = {'needs-me':'等你处理','review':'待验收','working':'进行中'};
const TABS = ['待我处理','派发','事件流','系统','按键映射','应用','使用引导'];
let activeTab = 0, selected = '', filterExe = '';
let multiMode = false; const multiSel = new Set();  // 待办多选批量销账
let DSP=null,DSP_LOADING=false,DSP_AT=0,DSP_SELECTED='',DSP_SIG='',DSP_TIMER=null;
let DSP_NEW=false,DSP_CLEAN=false,DSP_DELETING=false,DSP_NOTICE_TIMER=null;
const DSP_GROUP_STATE=new Map();
const DSP_DEL_SEL=new Set();
let DSCHAT=null,dchatOpen=false,dchatMsgs=[],dchatIds=new Set(),dchatTotal=0,
    dchatHasMore=false,dchatLoading=false,dchatKey='',DCHAT_PENDING='',
    DCHAT_REFRESHING=false;
const $ = id => document.getElementById(id);
// CSS 变量名 → token 名，和 style.qss 里的 @token@ 同名；缺项保持 :root 的兜底值。
const TOKENS = {'--bg':'bg','--surface':'card.bg','--surface2':'card.bg2',
  '--text':'text','--text-soft':'text.soft','--text-dim':'text.dim',
  '--muted':'muted','--muted2':'muted.2','--ghost':'ghost','--faint':'faint',
  '--line':'line','--raised':'raised','--accent':'accent',
  '--accent-hover':'accent.hover','--on-accent':'on.accent','--danger':'danger',
  '--warn':'warn','--ok':'ok','--pending':'pending','--info':'info','--w6':'w6','--w8':'w8','--w10':'w10','--w14':'w14',
  '--w16':'w16','--w18':'w18','--w30':'w30','--a26':'a26','--a36':'a36',
  '--a64':'a64'};
function applyTheme(t){
  const s = document.documentElement.style;
  Object.keys(TOKENS).forEach(v=>{if(t[TOKENS[v]]) s.setProperty(v,t[TOKENS[v]]);});
  // 字号倍率：和 ThemeManager::scaledPx 同一套算法（四舍五入到整像素、下限 7px），
  // 否则 QSS 缩了、这一页还是老尺寸，一眼看出不齐。
  const fs = t.fontScale || 1;
  [10,11,12,13].forEach(n=>s.setProperty('--f'+n, Math.max(7, Math.round(n*fs))+'px'));
  // 底必须由页面自己画满：透明透出来的是桌面，不是主窗背景（实测）。
  // 美术主题两层——插画 + 压暗纱（浅色是白纱、深色是黑纱，Qt 侧同一公式）；
  // classic 没有插画，就是主题纯色底。
  const b = document.body.style;
  b.backgroundColor = t.bg || 'var(--bg)';
  if (t.image) {
    const veil = t.veil || 'transparent';
    b.backgroundImage = 'linear-gradient('+veil+','+veil+'), url("'+t.image+'")';
    b.backgroundSize = 'cover, cover';
    b.backgroundPosition = 'center, center';
  } else {
    b.backgroundImage = 'none';
  }
}
function base(p){const t=(p||'').replace(/[\/\\]+$/,'');
  const i=Math.max(t.lastIndexOf('/'),t.lastIndexOf('\\'));return i<0?t:t.slice(i+1);}
function where(it){if(!it.session_id) return '未关联会话';
  const b=base(it.project);
  if(b!=='.'&&b!=='..'&&b) return b;
  // 项目缺失时退会话标题（DSH 投影缓存补的），再退短 id——与 Qt 侧同口径。
  if(it.session_title) return it.session_title;
  return '会话 '+String(it.session_id).slice(0,8);}
// 同宿主同项目重名时才补会话身份：优先标题（截 12 字），没标题才退短 id。
// WHERE_HITS 在 render 里按可见行数一遍，避免每行各数一遍。
let WHERE_HITS={};
function agentName(it){return it.title||
  ((WORKERS.find(w=>w.exe===it.agent_exe)||{}).name||'');}
function whereLabel(it){
  let w=where(it);
  if(!it.session_id||w.indexOf('会话 ')===0||w==='未关联会话') return w;
  if((WHERE_HITS[agentName(it)+'|'+w]||0)>1){
    if(it.session_title&&w!==it.session_title){
      const t=it.session_title;
      w+=' · '+(t.length>12?t.slice(0,12)+'…':t);
    }else{
      w+=' · '+String(it.session_id).slice(0,6);
    }
  }
  return w;
}
function waitText(it){if(!it.attention_since) return '';
  const secs=(now-Date.parse(it.attention_since))/1000;
  if(!(secs>=600)) return '';const m=secs/60;
  if(m<60) return ' · 等 '+Math.floor(m)+' 分';
  if(m<1440) return ' · 等 '+Math.floor(m/60)+' 小时';
  return ' · 等 '+Math.floor(m/1440)+' 天';}
function isStale(it){return it.stage==='working' && it.updated_at &&
  (now-Date.parse(it.updated_at))>1800000;}
// ISO(UTC) → 本地 HH:mm：updated_at/last_event_at 是带 Z 的 UTC 串，直接
// slice(11,16) 抠出来的是 UTC 钟点（本机 UTC+8，17:40 的活会显示成 09:40，
// 被当成早上没动的旧账）。排序和「等 X 分」用的是绝对时刻，不受影响。
function hhmm(iso){const t=Date.parse(iso||'');if(isNaN(t)) return '';
  const d=new Date(t);
  return String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');}
function rank(s){return s==='needs-me'?0:s==='review'?1:s==='working'?2:3;}
// 必须 post 对象：postMessage(JSON.stringify(obj)) 传的是字符串，
// C++ 侧 get_WebMessageAsJson 拿到的是带引号的字符串字面量，.object() 解出空，
// 点击就静默失效（实测踩过）。
function post(cmd,id){window.chrome.webview.postMessage(
  id?{cmd:cmd,id:id}:{cmd:cmd});}
// 带附加字段的动作（改键、开关、场景…）走这个；同样必须 post 对象。
function postObj(obj){window.chrome.webview.postMessage(obj);}
function visible(){return (WORK.items||[]).filter(i=>!i.done).filter(i=>{
  if(!filterExe) return true;
  return (i.agent_exe||'').toLowerCase()===filterExe.toLowerCase() ||
         (i.title||'').toLowerCase()===(WORKERS.find(w=>w.exe===filterExe)||{}).name?.toLowerCase();
});}
function sorted(){
  return visible().sort((a,b)=>{
    if(rank(a.stage)!==rank(b.stage)) return rank(a.stage)-rank(b.stage);
    if(rank(a.stage)<=1){
      const wa=Date.parse(a.attention_since||a.updated_at||0);
      const wb=Date.parse(b.attention_since||b.updated_at||0);
      if(wa!==wb) return wa-wb;}
    return Date.parse(b.updated_at||0)-Date.parse(a.updated_at||0);});
}
function stageColor(it){
  return isStale(it)?'var(--muted)':(COLOR[it.stage]||'var(--muted)');}
function stageLabel(it){
  return isStale(it)?'状态待确认':(LABEL[it.stage]||'待办');}
function renderTabs(){
  const el=$('tabs');
  // 只建一次，之后原地改状态：5 秒一次的刷新若在按下/松开之间重建 DOM，
  // 这次点击就落在被替换掉的节点上，丢一次（实测偶尔点页签没反应）。
  if(el.children.length!==TABS.length){
    el.replaceChildren();
    TABS.forEach((name,i)=>{
      const b=document.createElement('button');
      b.className='tab';
      b.textContent=name;
      b.addEventListener('click',()=>{activeTab=i;render();});
      el.append(b);});
  }
  TABS.forEach((name,i)=>{
    const b=el.children[i];
    b.classList.toggle('active',i===activeTab);
    let c=b.querySelector('.count');
    if(i===0&&WORK.attention_count){
      if(!c){c=document.createElement('span');c.className='count';b.append(c);}
      c.textContent=WORK.attention_count;
    }else if(c){c.remove();}
  });
}
function renderChips(){
  const el=$('chips'); el.replaceChildren();
  const rows=visible();
  const defs=[['needs-me','var(--pending)','等你'],['review','var(--ok)','待验收'],
              ['working','var(--info)','进行中']];
  defs.forEach(([stage,color,label])=>{
    const n=rows.filter(r=>r.stage===stage).length;
    if(!n) return;
    const s=document.createElement('span');s.className='chip';
    s.style.color=color;s.textContent=label+' '+n;el.append(s);});
}
function renderFilter(){
  const sel=$('filter'); sel.replaceChildren();
  const all=document.createElement('option');all.value='';all.textContent='全部 Worker';
  sel.append(all);
  WORKERS.filter(w=>w.alive).forEach(w=>{
    const o=document.createElement('option');o.value=w.exe;o.textContent=w.name;
    sel.append(o);});
  sel.value=filterExe;
  sel.onchange=()=>{filterExe=sel.value;render();};
}
function renderHealth(){
  const h=STATE.health||{};
  const ok=!!h.bridge;
  const n=WORKERS.filter(w=>w.alive).length;
  const last=hhmm(h.last_event_at);
  $('health').innerHTML='<span style="color:'+(ok?'var(--ok)':'var(--pending)')+'">●</span> '+
    (ok?'桥接正常':'桥接未启动')+' · '+n+' 个 Worker'+
    (last?' · 最近 '+last:'');
}
// 回复正文开头常带「Agent名：」前缀（stop 提醒拼的），行首已有 Agent 名，
// 剥掉免得一行里同一名字出现两遍（用户 2026-11-16，与手机端同款）。
function stripAgentPrefix(msg,title){
  const t=String(title||'');const s=String(msg||'');
  if(!t) return s;
  if(s.startsWith(t+'：')) return s.slice(t.length+1);
  if(s.startsWith(t+': ')) return s.slice(t.length+2);
  if(s.startsWith(t+':')) return s.slice(t.length+1);
  return s;
}
function row(it){
  const el=document.createElement('div');
  el.className='row'+(it.id===selected?' sel':'')+
    (multiMode&&multiSel.has(it.id)?' msel':'');
  const bar=document.createElement('div');bar.className='bar';
  bar.style.background=stageColor(it);
  const l1=document.createElement('div');l1.className='l1';
  const name=document.createElement('span');name.className='name';name.textContent=it.title||'';
  const stage=document.createElement('span');stage.className='stage';
  stage.style.color=stageColor(it);stage.textContent=stageLabel(it);
  const wh=document.createElement('span');wh.className='where';wh.textContent=whereLabel(it);
  const time=document.createElement('span');time.className='time';
  time.textContent=hhmm(it.updated_at)+waitText(it);
  l1.append(name,stage,wh,time);
  const l2=document.createElement('div');l2.className='l2';
  l2.textContent=stripAgentPrefix(it.message,it.title);
  el.append(bar,l1,l2);
  // 单击进聊天页（打开即最新记录）；回到 Agent 走行内 ↗ 图标。多选模式下点击改为
  // 勾选/取消，进聊天先点「完成」退回单选。
  el.addEventListener('click',()=>{
    if(multiMode){
      if(multiSel.has(it.id)) multiSel.delete(it.id); else multiSel.add(it.id);
      el.classList.toggle('msel',multiSel.has(it.id));
      renderFoot();
      return;
    }
    selected=it.id;postObj({cmd:'chatOpen',id:it.id});
  });
  // 行内图标按钮：回到 Agent / 标为已处理（去文字，图标 + tooltip）。
  // stopPropagation：别把这次点击变成进聊天页。
  const acts=document.createElement('div');acts.className='rowacts';
  const raiseB=document.createElement('button');raiseB.className='icobtn';
  // 「回到 Agent」= 箭头进窗口（不是外链的「方框 + 往外的箭头」：那是「在浏览器
  // 里打开」的语义）。框里加一条横贯的标题栏，16px 下才读得出是「某个应用的
  // 窗口」而不是空白方块；箭头尖和框留 0.4 单位缝，贴住会糊成一团（8x 样张比对）。
  raiseB.innerHTML='<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><rect x="9.4" y="4.2" width="10.6" height="15.6" rx="2.6"/><path d="M9.4 8.8h10.6"/><path d="M2.6 12h6.2"/><path d="M5.8 9.2L9 12l-3.2 2.8"/></svg>';
  raiseB.title='回到 Agent（没开着会先启动宿主）';
  raiseB.setAttribute('aria-label','回到 Agent');
  raiseB.onclick=(e)=>{e.stopPropagation();
    const dd=STATE.dispatch||{};
    post((dd.noticeId===it.id && dd.noticeExe==='ZCode.exe')?'raiseZcode':'open',it.id);};
  const ackB=document.createElement('button');ackB.className='icobtn ack';
  ackB.innerHTML='<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M5.2 12.4l4.3 4.3L18.8 7.5"/></svg>';
  ackB.title='标为已处理';
  ackB.setAttribute('aria-label','标为已处理');
  ackB.onclick=(e)=>{e.stopPropagation();post('ack',it.id);};
  acts.append(raiseB,ackB);
  el.append(acts);
  return el;
}
function renderFoot(){
  const selItem=(WORK.items||[]).find(i=>i.id===selected && !i.done);
  const d=STATE.dispatch||{};
  // 通知只跟当次派发的那张卡走，别把上一条的拒绝原因挂在新选中的卡下面。
  const notice=(d.noticeId && d.noticeId!==selected)?'':(d.notice||'');
  // ZCode 派发通知态：对话区要重启才可见，行内「回到 Agent」改走聚焦不销账
  // （raiseZcode），Enter 同款。
  const zcodeNotice=!!(selItem && notice && d.noticeExe==='ZCode.exe');
  window.ZC_NOTICE=zcodeNotice;
  $('multi').textContent=multiMode?'完成':'多选';
  if(multiMode){
    // 多选模式：批量销账（派发走聊天页的输入框）。
    $('hint').textContent='勾选待办，一次性标为已处理 · 已选 '+multiSel.size+' 条';
    $('hint').style.color='';
    $('ackBatch').hidden=multiSel.size===0;
    $('ackBatch').textContent='✓ 标为已处理（'+multiSel.size+'）';
    return;
  }
  $('ackBatch').hidden=true;
  // 通知顶掉键位提示占头行：状态比键位值钱，别两行都挤在脚部。
  $('hint').textContent=notice||(selItem
    ? '点击待办进聊天 · 行内图标回到/销账 · Delete 标为已处理'
    : '点击待办进聊天页 · 行内图标回到/销账');
  $('hint').style.color=notice?'var(--text-dim)':'';
}
function renderEvents(){
  $('evCount').textContent=EVENTS.length+' 条';
  $('evEmpty').hidden=EVENTS.length>0;
  const list=$('evList');
  list.hidden=EVENTS.length===0;
  const keep=list.scrollTop;
  // 和 Qt 的 scrollToBottom 一致：来了新事件就滚到底；条数没变的刷新
  // （换主题、别的页签动静）不动滚动位置。
  const grew=EVENTS.length>evSeen;
  list.replaceChildren();
  EVENTS.forEach(ev=>{
    const d=document.createElement('div');
    d.className='ev';
    d.style.color=EVCOLOR[ev.kind]||'var(--text-dim)';
    d.textContent=ev.text||'';
    list.append(d);
  });
  list.scrollTop=grew?list.scrollHeight:keep;
  evSeen=EVENTS.length;
}
function renderSystem(){
  const s=SYSTEM||{};
  const el=$('sysLines'); el.replaceChildren();
  ['bridge','uptime','last','spool'].forEach(k=>{
    const d=document.createElement('div'); d.textContent=s[k]||''; el.append(d);});
  const pl=document.createElement('div'); pl.textContent=s.plugin||'';
  pl.title=s.pluginTip||''; el.append(pl);
  const pr=document.createElement('div'); pr.textContent=s.problems||'';
  if(!s.problemsOk) pr.style.color='var(--pending)'; el.append(pr);
  const t=$('sysWorkers'); t.replaceChildren();
  const head=document.createElement('div'); head.className='wrow head';
  ['Worker','状态','待你','待验收','进行中','最近'].forEach(x=>{
    const c=document.createElement('span'); c.textContent=x; head.append(c);});
  t.append(head);
  (s.workers||[]).forEach(w=>{
    const row=document.createElement('div'); row.className='wrow';
    const n=document.createElement('span'); n.textContent=w.name||'';
    if(w.foreground) n.className='wfg';
    const st=document.createElement('span'); st.textContent=w.state||'';
    const a=document.createElement('span'); a.textContent=w.needs||0;
    const b=document.createElement('span'); b.textContent=w.review||0;
    const c=document.createElement('span'); c.textContent=w.working||0;
    const d=document.createElement('span'); d.textContent=w.last||'—';
    row.append(n,st,a,b,c,d); t.append(row);});
  renderLan(s.lan||{});
}
// 手机访问卡片：开关/网址/配对二维码/设备吊销。配对码只在这张卡里出现，
// 不下发手机（/m/state 里没有 lan 块）。二维码内容 = 首个 LAN 网址 + #pair=码。
function renderLan(l){
  const sw=$('lanSw');
  sw.classList.toggle('on',!!l.enabled);
  sw.onclick=()=>postObj({cmd:'lanToggle',on:!l.enabled});
  $('lanState').textContent=l.running?('监听中 · 端口 '+(l.port||45916))
    :(l.enabled?'启动失败（端口被占）':'已关闭');
  $('lanBody').hidden=!l.enabled;
  if(!l.enabled) return;
  const urls=l.urls||[];
  const lu=$('lanUrls');lu.replaceChildren();
  if(!urls.length){lu.textContent='找不到局域网 IPv4 地址（检查网卡）';}
  else{
    // 多网卡都列出来让人挑；192.168（家用 WiFi）标「推荐」，手机一般用这个。
    lu.append('手机浏览器打开：');
    urls.forEach((u,i)=>{
      if(i)lu.append('  ·  ');
      const s=document.createElement('span');s.textContent=u;
      if(u.indexOf('://192.168.')>=0){s.style.color='var(--ok)';
        const b=document.createElement('span');b.textContent='（推荐）';
        b.style.color='var(--ok)';lu.append(s,b);}
      else{lu.append(s);}
    });
  }
  const p=l.pairing||{};
  $('lanPairBtn').textContent=p.active?'收起二维码':'显示配对二维码';
  $('lanPairBtn').onclick=()=>postObj({cmd:p.active?'lanPairEnd':'lanPair'});
  $('lanPairHint').textContent=l.running?'手机扫码即可配对 · 10 分钟内有效'
    :'先解决端口占用，再配对';
  const wrap=$('lanQrWrap');
  if(p.active&&urls.length&&typeof qrcode==='function'){
    wrap.hidden=false;
    const sig=p.code+'|'+urls[0];
    if($('lanQr').dataset.sig!==sig){
      $('lanQr').dataset.sig=sig;
      try{
        const qr=qrcode(0,'M');
        // 配对词是中文：fragment 用 percent 编码成纯 ASCII，二维码不必处理 UTF-8，
        // 手机页 decodeURIComponent 还原（hex 回退码也兼容）。
        qr.addData(urls[0]+'#pair='+encodeURIComponent(p.code));
        qr.make();
        $('lanQr').innerHTML=qr.createSvgTag(4,8);
      }catch(e){$('lanQr').textContent='二维码生成失败：'+e;}
    }
    $('lanCode').textContent='配对词 '+p.code+'（扫不了可在手机页手动输入）';
  }else{
    wrap.hidden=true;$('lanQr').dataset.sig='';
  }
  const dv=$('lanDevices');dv.replaceChildren();
  (l.devices||[]).forEach(d=>{
    const row=document.createElement('div');row.className='landev';
    const n=document.createElement('span');n.className='dn';n.textContent=d.name||'手机';
    const m=document.createElement('span');m.className='dm';
    m.textContent='配对 '+hhmm(d.pairedAt)+' · 最近 '+hhmm(d.lastSeen);
    const rev=document.createElement('button');rev.className='btn';
    rev.textContent='吊销';rev.style.padding='2px 8px';
    rev.title='吊销后这台手机的令牌立即失效，要重新配对';
    rev.onclick=()=>postObj({cmd:'lanRevoke',id:d.id});
    row.append(n,m,rev);dv.append(row);
  });
  if(!(l.devices||[]).length){
    const none=document.createElement('div');none.className='mutedline';
    none.textContent='还没有配对过的手机';dv.append(none);
  }
}
function renderKeymap(){
  const k=KEYMAP||{};
  // 场景行：跟随前台绿点、固定场景黄点（和 Qt refreshScene 同色）。
  $('kmDot').style.background = k.pinned ? 'var(--warn)' : 'var(--ok)';
  $('kmName').textContent = k.sceneName||'—';
  $('kmRule').textContent = k.sceneRule||'';
  $('kmRule').title = k.sceneRuleTip||'';
  // 场景下拉只在选项真的变了时重建：每 5 秒一次的刷新不该把正开着的下拉关掉。
  const sel=$('kmScene');
  const sig=(k.pinned||'')+'|'+((k.scenes||[]).join(','));
  if(sel.dataset.sig!==sig){
    sel.dataset.sig=sig;
    sel.replaceChildren();
    const auto=document.createElement('option'); auto.value='';
    auto.textContent='跟随前台'; sel.append(auto);
    (k.scenes||[]).forEach(n=>{
      const o=document.createElement('option'); o.value=n; o.textContent=n;
      sel.append(o);});
    sel.onchange=()=>postObj({cmd:'keyScene',name:sel.value});
  }
  sel.value=k.pinned||'';
  const list=$('kmList'); const keep=list.scrollTop; list.replaceChildren();
  (k.groups||[]).forEach(g=>{
    const t=document.createElement('div'); t.className='mapgtitle';
    const l=document.createElement('span'); l.textContent=g.title||'';
    const h=document.createElement('span'); h.className='hair2';
    t.append(l,h); list.append(t);
    (g.rows||[]).forEach(r=>{
      const row=document.createElement('div');
      row.className='keyrow'+(r.vk===k.selected?' sel':'');
      const kc=document.createElement('div'); kc.className='keycell';
      const gph=document.createElement('span'); gph.className='glyph';
      gph.textContent=r.glyph||'';
      const kn=document.createElement('span'); kn.className='kname';
      kn.textContent=r.name||'';
      kc.append(gph,kn);
      kc.onclick=()=>postObj({cmd:'keySelect',vk:r.vk});
      row.append(kc);
      [['single'],['double'],['hold']].forEach(([g2])=>{
        const c=r[g2]||{};
        const cell=document.createElement('div');
        cell.className='gcell'+(c.editable?' edit':'');
        const tt=document.createElement('div'); tt.className='t';
        tt.textContent=c.title||'';
        const ee=document.createElement('div'); ee.className='e';
        ee.textContent=c.effect||'';
        if(!c.title || c.title==='—') cell.classList.add('off');
        cell.title=c.tip||'';
        cell.append(tt,ee);
        // 只有可改的格子才回传；语音键/对焦键这些固定行和 Qt 一样不给点。
        if(c.editable) cell.onclick=()=>postObj({cmd:'keyEdit',vk:r.vk,gesture:g2});
        row.append(cell);
      });
      list.append(row);
    });
  });
  list.scrollTop=keep;
  const hint=$('kmHint'); hint.textContent=k.hint||'';
  hint.style.color=k.hintColor||'';
  const sw=$('kmTakeover');
  sw.classList.toggle('on',!!k.takeover);
  sw.onclick=()=>postObj({cmd:'keyTakeover',on:!k.takeover});
}
function renderApps(){
  const a=APPS||{};
  const list=$('apList'); const keep=list.scrollTop; list.replaceChildren();
  (a.rows||[]).forEach(r=>{
    const row=document.createElement('div'); row.className='approw';
    const n=document.createElement('span'); n.className='aname';
    n.textContent=r.name||'';
    const s=document.createElement('span');
    s.className='astate'+(r.found?'':' miss');
    s.textContent=r.state||''; s.title=r.stateTip||'';
    const open=document.createElement('button'); open.className='btn';
    open.textContent='打开'; open.disabled=!r.found;
    open.title='现在打开一次（语音说「打开 '+(r.name||'')+'」同样有效）';
    open.onclick=()=>postObj({cmd:'appOpen',id:r.id});
    row.append(n,s,open);
    if(r.custom){
      const del=document.createElement('button'); del.className='btn';
      del.textContent='删';
      del.title='从自定义应用里移除（不影响应用本身）';
      del.onclick=()=>postObj({cmd:'appRemove',name:r.name,path:r.customPath});
      row.append(del);
    }
    list.append(row);
  });
  list.scrollTop=keep;
  $('apSummary').textContent=a.summary||'';
  const v=$('apVoice');
  v.classList.toggle('on',!!a.voiceLaunch);
  v.onclick=()=>postObj({cmd:'appVoice',on:!a.voiceLaunch});
  const sy=$('apSystem');
  sy.classList.toggle('on',!!a.systemControl);
  sy.onclick=()=>postObj({cmd:'appSystem',on:!a.systemControl});
}
function renderGuide(){
  const g=GUIDE||{};
  const body=$('guBody'); const keep=body.scrollTop; body.replaceChildren();
  const intro=document.createElement('div'); intro.className='mutedline';
  intro.style.marginBottom='14px'; intro.textContent=g.intro||'';
  body.append(intro);
  (g.sections||[]).forEach(sec=>{
    const box=document.createElement('div'); box.className='gsec';
    const h=document.createElement('div'); h.className='ghead';
    h.textContent=sec.title||''; box.append(h);
    (sec.rows||[]).forEach(pair=>{
      const r=document.createElement('div'); r.className='grow';
      const kk=document.createElement('div'); kk.className='gk';
      kk.textContent=pair[0]||'';
      const vv=document.createElement('div'); vv.className='gt';
      vv.textContent=pair[1]||'';
      r.append(kk,vv); box.append(r);
    });
    body.append(box);
  });
  const links=document.createElement('div'); links.className='links';
  const mk=(label,fn)=>{const b=document.createElement('button');
    b.className='btn'; b.textContent=label; b.onclick=fn; return b;};
  links.append(mk('官网  ↗',()=>postObj({cmd:'guideSite'})));
  links.append(mk('GitHub  ↗',()=>postObj({cmd:'guideGithub'})));
  const copy=mk('复制安装话术',()=>{
    postObj({cmd:'guideCopy'});
    copy.textContent='已复制 ✓ 去粘给 Agent';
    setTimeout(()=>{copy.textContent='复制安装话术';},2600);
  });
  links.append(copy);
  const v=document.createElement('span'); v.className='mutedline';
  v.style.marginLeft='8px'; v.textContent='当前版本 v'+(g.version||'');
  links.append(v);
  body.append(links);
  body.scrollTop=keep;
}
// ---------- 派发页：Agent / 项目 / 会话 → 聊天记录 → 派发 ----------
function dspAgo(iso){
  const t=Date.parse(iso||'');if(isNaN(t))return '';
  const s=Math.max(0,(Date.now()-t)/1000);
  if(s<60)return '刚刚';
  if(s<3600)return Math.floor(s/60)+' 分钟前';
  if(s<86400)return Math.floor(s/3600)+' 小时前';
  if(s<86400*30)return Math.floor(s/86400)+' 天前';
  return new Date(t).toLocaleDateString();
}
function dspHasProject(p){
  const b=base(p);return !!b&&b!=='.'&&b!=='..';
}
function dspProjectName(p){return dspHasProject(p)?base(p):'未分类任务';}
// 分组必须按完整路径，不能只按最后一级目录：同名 tuwen-editor 可能同时存在
// 活跃仓库和桌面旧目录，按 basename 合并会把活跃项目误标成已归档。
function projectKey(p){
  return String(p||'').trim().replace(/[\\/]+$/,'').replace(/\//g,'\\').toLowerCase();
}
function projectParentName(p){
  const t=String(p||'').trim().replace(/[\\/]+$/,'');
  const i=Math.max(t.lastIndexOf('/'),t.lastIndexOf('\\'));
  return i<0?'':base(t.slice(0,i));
}
function requestSessions(force){
  if(DSP_LOADING)return;
  if(!force&&DSP&&Date.now()-DSP_AT<30000)return;
  DSP_LOADING=true;
  const btn=$('dspRefresh');if(btn)btn.disabled=true;
  postObj({cmd:'sessionsLoad'});
  if(DSP_TIMER)clearTimeout(DSP_TIMER);
  DSP_TIMER=setTimeout(()=>{
    if(!DSP_LOADING)return;
    DSP_LOADING=false;
    const b=$('dspRefresh');if(b)b.disabled=false;
    if(activeTab===1&&!dchatOpen)render();
  },12000);
}
window.__setSessions=function(c){
  const wasEmpty=!DSP;
  DSP=c||{};DSP_LOADING=false;DSP_AT=Date.now();
  if(wasEmpty)DSP_NEW=true;
  if(DSP_TIMER){clearTimeout(DSP_TIMER);DSP_TIMER=null;}
  const btn=$('dspRefresh');if(btn)btn.disabled=false;
  const agents=DSP.agents||[];
  if(!agents.some(a=>a.exe===DSP_SELECTED))DSP_SELECTED=agents.length?agents[0].exe:'';
  DSP_SIG='';
  if(activeTab===1&&!dchatOpen)render();
};
function dspAgentChip(agent){
  const b=document.createElement('button');b.className='dspagent';
  b.classList.toggle('on',agent.exe===DSP_SELECTED);
  b.setAttribute('aria-pressed',String(agent.exe===DSP_SELECTED));
  const st=document.createElement('span');st.className='st'+(agent.alive?' on':'');
  const name=document.createElement('span');name.textContent=agent.name||agent.exe;
  const n=document.createElement('span');n.className='n';
  n.textContent=String((agent.sessions||[]).length);
  b.append(st,name,n);
  b.onclick=()=>{
    if(DSP_SELECTED===agent.exe)return;
    DSP_SELECTED=agent.exe;DSP_SIG='';
    if(DSP_CLEAN)DSP_DEL_SEL.clear();
    $('dspList').scrollTop=0;render();
  };
  return b;
}
function dspSession(agent,row,archived){
  const el=document.createElement('div');el.className='dspsess';
  if(row.can_dispatch===false)el.classList.add('readonly');
  if(DSP_CLEAN)el.classList.add('clean');
  const key=agent.exe+'|'+row.id;
  if(DSP_CLEAN&&DSP_DEL_SEL.has(key))el.classList.add('sel');
  el.tabIndex=0;el.setAttribute('role','button');
  el.setAttribute('aria-label',(row.title||'会话')+'，'+
    (dspHasProject(row.project)&&!row.generated?dspProjectName(row.project):'未分类任务')+
    '，'+dspAgo(row.updated_at));
  let chk=null;
  if(DSP_CLEAN){
    if(row.can_delete===false){
      const ro=document.createElement('span');ro.className='ro';ro.textContent='不可删';
      el.append(ro);
    }else{
      chk=document.createElement('span');
      chk.className='dspcheck'+(DSP_DEL_SEL.has(key)?' on':'');
      chk.setAttribute('aria-hidden','true');
      el.append(chk);
    }
  }
  const t=document.createElement('div');t.className='t';
  const title=document.createElement('div');title.className='title';
  title.textContent=row.title||('会话 '+String(row.id||'').slice(0,8));
  const sub=document.createElement('div');sub.className='sub';
  const upd=document.createElement('span');upd.className='u';
  upd.textContent='最近更新 · '+dspAgo(row.updated_at);
  sub.append(upd);
  if(row.can_dispatch===false){
    const ro=document.createElement('span');ro.className='ro';ro.textContent='只读';
    sub.append(ro);
  }
  t.append(title,sub);
  el.append(t);
  if(row.stage){
    const st=document.createElement('span');st.className='st';
    st.style.color=COLOR[row.stage]||'var(--muted)';
    st.textContent=LABEL[row.stage]||'';
    el.append(st);
  }
  if(archived&&!DSP_CLEAN){
    const del=document.createElement('button');del.className='dspdel';
    del.textContent=row.can_delete===false?'不可删':'删除';
    del.disabled=row.can_delete===false||DSP_DELETING;
    del.title=row.can_delete===false
      ?'该宿主没有安全的会话物理删除接口，请在宿主内处理'
      :'删除这条归档会话（会先备份并二次确认）';
    del.onclick=e=>{e.stopPropagation();
      if(row.can_delete!==false)deleteRows([{agent,row}]);};
    el.append(del);
  }else if(!DSP_CLEAN){
    const arr=document.createElement('span');arr.className='arr';arr.textContent='›';
    el.append(arr);
  }
  const toggle=()=>{
    if(DSP_DELETING||row.can_delete===false)return;
    if(DSP_DEL_SEL.has(key))DSP_DEL_SEL.delete(key);else DSP_DEL_SEL.add(key);
    el.classList.toggle('sel',DSP_DEL_SEL.has(key));
    if(chk)chk.classList.toggle('on',DSP_DEL_SEL.has(key));
    updateDspBatch();
  };
  const open=()=>{if(DSP_CLEAN)toggle();else openDSession(agent,row);};
  el.onclick=open;
  el.onkeydown=e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();open();}};
  return el;
}
// stateKeyOverride：同名的两个组（活跃的「未分类任务」和它的已归档组）不能
// 共用折叠状态，各给一个 key。hideBadge：归档分区里的组头不再重复「已归档」。
function dspGroup(agent,label,rows,fullProject,archived,defaultOpen,stateKeyOverride,hideBadge){
  const groupKey=fullProject?projectKey(fullProject):'__ungrouped';
  const stateKey=stateKeyOverride||(agent.exe+'|'+groupKey);
  const group=document.createElement('div');group.className='dspgroup';
  if(archived)group.classList.add('archived');
  const head=document.createElement('button');head.className='dspghead';
  const chev=document.createElement('span');chev.className='dspgchev';chev.textContent='›';
  const name=document.createElement('span');name.className='dspgname';name.textContent=label;
  head.append(chev,name);
  if(archived&&!hideBadge){
    const a=document.createElement('span');a.className='dspgarch';a.textContent='已归档';
    head.append(a);
  }
  const count=document.createElement('span');count.className='dspgcount';
  count.textContent=rows.length+' 个';
  head.append(count);
  head.title=fullProject||'没有项目的单任务会话（含宿主自动生成的临时工作区）';
  const body=document.createElement('div');body.className='dspbody';
  rows.forEach(row=>body.append(dspSession(agent,row,archived)));
  const stored=DSP_GROUP_STATE.get(stateKey);
  const open=(DSP_CLEAN&&archived)?true
    :(stored===undefined?!!defaultOpen:stored);
  group.classList.toggle('closed',!open);
  body.hidden=!open;
  head.setAttribute('aria-expanded',String(open));
  head.onclick=()=>{
    const opening=body.hidden;
    body.hidden=!opening;
    group.classList.toggle('closed',!opening);
    head.setAttribute('aria-expanded',String(opening));
    DSP_GROUP_STATE.set(stateKey,opening);
  };
  group.append(head,body);
  return group;
}
// 已归档单独成区：整区默认收起（用户 2026-11-17 定：别跟活跃项目混在一串），
// 展开后内部仍按项目分组——结构跟活跃区一样，只是归属和视觉层级低一档。
// 清理入口就放在区头右侧（用户 2026-11-17 定）：清理的对象就是这一区。
function dspArchiveSection(agent,count,defaultOpen,deletable,cleaning){
  const sec=document.createElement('div');sec.className='dspsec';
  const head=document.createElement('div');head.className='dspsechead';
  const fold=document.createElement('button');fold.className='dspsecfold';
  const chev=document.createElement('span');chev.className='dspsecchev';chev.textContent='›';
  const name=document.createElement('span');name.className='dspsecname';name.textContent='已归档';
  const cnt=document.createElement('span');cnt.className='dspseccount';
  cnt.textContent=cleaning?(count+' 条可删'):(count+' 个会话');
  fold.append(chev,name,cnt);
  fold.title=cleaning
    ?'清理模式：勾选要删除的归档会话，删除前会备份并要求二次确认。'
    :'已归档会话：项目目录已不存在，或宿主侧已归档。展开可查看记录、删除。';
  head.append(fold);
  if(deletable>0||cleaning){
    const clean=document.createElement('button');
    clean.className='dspsecclean'+(cleaning?' on':'');
    clean.textContent=cleaning?'退出清理':'清理归档';
    clean.disabled=DSP_DELETING;
    clean.title=cleaning?'退出清理，回到正常浏览'
      :'只处理已归档会话；删除前会备份并要求二次确认';
    clean.onclick=e=>{
      e.stopPropagation();
      if(DSP_DELETING)return;
      DSP_CLEAN=!DSP_CLEAN;DSP_DEL_SEL.clear();DSP_SIG='';
      $('dspList').scrollTop=0;$('dspNotice').hidden=true;
      render();
    };
    head.append(clean);
  }
  const body=document.createElement('div');body.className='dspsecbody';
  const stateKey=agent.exe+'|__archive-section';
  const stored=DSP_GROUP_STATE.get(stateKey);
  // 清理模式必须展开（要挑条目），不管之前手动收成什么样。
  const open=cleaning?true:(stored===undefined?!!defaultOpen:stored);
  const labelOf=opening=>cleaning
    ?('已归档，'+count+' 条可删，'+(opening?'已展开':'已收起'))
    :('已归档，'+count+' 个会话，'+(opening?'已展开':'已收起'));
  sec.classList.toggle('closed',!open);
  body.hidden=!open;
  fold.setAttribute('aria-expanded',String(open));
  fold.setAttribute('aria-label',labelOf(open));
  fold.onclick=()=>{
    const opening=body.hidden;
    body.hidden=!opening;
    sec.classList.toggle('closed',!opening);
    fold.setAttribute('aria-expanded',String(opening));
    fold.setAttribute('aria-label',labelOf(opening));
    DSP_GROUP_STATE.set(stateKey,opening);
  };
  sec.append(head,body);
  return {el:sec,body};
}
function dspArchivedRows(agent){
  return (agent&&agent.sessions||[]).filter(r=>r.archived&&r.can_delete!==false);
}
function dspCurrentAgent(){
  const agents=DSP&&DSP.agents||[];
  return agents.find(a=>a.exe===DSP_SELECTED)||agents[0]||null;
}
function updateDspBatch(){
  const agent=dspCurrentAgent();
  const rows=dspArchivedRows(agent);
  $('dspBatch').hidden=!DSP_CLEAN;
  $('dspBatchCount').textContent='已选 '+DSP_DEL_SEL.size+' 条';
  const cleanBtn=document.querySelector('#dspList .dspsecclean');
  if(cleanBtn)cleanBtn.disabled=DSP_DELETING;
  $('dspRefresh').disabled=DSP_DELETING;
  $('dspDelete').disabled=DSP_DELETING||!DSP_DEL_SEL.size;
  $('dspDelete').textContent=DSP_DELETING?'删除中…':'删除所选';
  $('dspCheckAll').disabled=DSP_DELETING||!rows.length;
  $('dspCheckAll').textContent=(rows.length&&DSP_DEL_SEL.size===rows.length)?'全不选':'全选';
}
function showDspNotice(text,bad){
  const el=$('dspNotice');if(!el)return;
  el.hidden=false;el.textContent=text;el.classList.toggle('bad',!!bad);
  if(DSP_NOTICE_TIMER)clearTimeout(DSP_NOTICE_TIMER);
  DSP_NOTICE_TIMER=setTimeout(()=>{el.hidden=true;el.textContent='';},6500);
}
function deleteRows(pairs){
  if(DSP_DELETING||!pairs.length)return;
  DSP_DELETING=true;
  updateDspBatch();
  const items=pairs.map(({agent,row})=>({exe:agent.exe,session:row.id,
    project:row.project||'',title:row.title||''}));
  showDspNotice('等待确认：将删除 '+items.length+' 条归档会话并备份…');
  postObj({cmd:'sessionDelete',items});
}
window.__setDeleteResult=function(r){
  DSP_DELETING=false;updateDspBatch();
  if(!r)return;
  if(r.canceled){showDspNotice('已取消，没有删除任何会话。');return;}
  const failed=(r.failed||[]).length;
  if((r.deleted||0)>0){
    DSP_CLEAN=false;DSP_DEL_SEL.clear();DSP_SIG='';
    showDspNotice('已删除 '+(r.deleted||0)+' 条归档会话'
      +(failed?'，'+failed+' 条失败':'')
      +(r.backup?('；备份：'+r.backup):''),failed>0);
    render();
    requestSessions(true);
  }else{
    showDspNotice('没有删除成功：'+((r.error||'未知原因'))
      +(r.backup?('；备份：'+r.backup):''),true);
  }
};
function renderDispatch(){
  const list=$('dspList'),chips=$('dspAgents');
  if(!DSP){
    chips.replaceChildren();
    $('dspEmpty').hidden=true;list.hidden=false;
    list.replaceChildren();
    const d=document.createElement('div');d.className='dspempty';
    d.textContent=DSP_LOADING?'正在读取会话目录…':'会话目录还没加载';
    list.append(d);
    $('dspSummary').textContent=DSP_LOADING?'读取中…':'';
    if(!DSP_LOADING)requestSessions();
    return;
  }
  const agents=DSP.agents||[];
  if(!agents.length){
    chips.replaceChildren();list.replaceChildren();list.hidden=true;
    $('dspEmpty').hidden=false;$('dspSummary').textContent='';
    $('dspBatch').hidden=true;
    return;
  }
  if(!agents.some(a=>a.exe===DSP_SELECTED))DSP_SELECTED=agents[0].exe;
  const sig=DSP_SELECTED+'|'+(DSP_CLEAN?'clean':'')+'|'+JSON.stringify(DSP);
  if(sig===DSP_SIG){updateDspBatch();return;}
  DSP_SIG=sig;
  chips.replaceChildren();
  agents.forEach(a=>chips.append(dspAgentChip(a)));
  const agent=agents.find(a=>a.exe===DSP_SELECTED)||agents[0];
  const rows=agent.sessions||[];
  const groups=new Map(),unclassified=[];
  rows.forEach(row=>{
    // 没有项目，或项目是宿主自动生成的临时工作区（WorkBuddy 的日期目录等）：
    // 都是「单任务」，统一进未分类任务，不按目录名/日期各开一组。
    if(!dspHasProject(row.project)||row.generated){unclassified.push(row);return;}
    const key=projectKey(row.project);
    if(!groups.has(key))groups.set(key,{path:row.project,items:[]});
    groups.get(key).items.push(row);
  });
  const allGroups=[...groups.values()];
  const nameHits=new Map();
  allGroups.forEach(g=>{
    const n=base(g.path).toLowerCase();
    nameHits.set(n,(nameHits.get(n)||0)+1);
  });
  const active=[],archived=[];
  allGroups.forEach(g=>{
    let label=base(g.path);
    if(nameHits.get(label.toLowerCase())>1){
      const parent=projectParentName(g.path);
      if(parent)label+=' · '+parent;
    }
    // 同一项目按每行自己的归档状态拆两组：活跃组只放未归档的会话，已归档的
    // （老路径、宿主归档）单独进「已归档」区，两边绝不混在一组里。
    const key=projectKey(g.path);
    const live=g.items.filter(r=>!r.archived);
    const dead=g.items.filter(r=>!!r.archived);
    if(live.length)active.push({key,label,items:live,fullProject:g.path});
    if(dead.length)archived.push({key,label,items:dead,fullProject:g.path});
  });
  // 未分类任务同样按归档拆：活跃单任务留在上面，已归档单任务进已归档区。
  const uncLive=unclassified.filter(r=>!r.archived);
  const uncDead=unclassified.filter(r=>!!r.archived);
  const deletable=dspArchivedRows(agent);
  // 选择里已经不在当前归档集（别的 Agent/已被删）的项清掉。
  if(DSP_CLEAN){
    const valid=new Set(deletable.map(r=>agent.exe+'|'+r.id));
    DSP_DEL_SEL.forEach(k=>{if(!valid.has(k))DSP_DEL_SEL.delete(k);});
  }
  $('dspRefresh').hidden=DSP_CLEAN;
  $('dspBatch').hidden=!DSP_CLEAN;
  // 摘要只报活跃面：归档数量交给底部「已归档」区头，主线不混着算。
  const liveCount=rows.filter(r=>!r.archived).length;
  $('dspSummary').textContent=DSP_CLEAN
    ?(agent.name+' · 选择要删除的归档会话 · '+deletable.length+' 条可删')
    :(agent.name+' · '+active.length+' 个项目'
      +(uncLive.length?' · '+uncLive.length+' 个未分类任务':'')
      +' · '+liveCount+' 个活跃会话');
  const keep=list.scrollTop;list.replaceChildren();list.hidden=false;
  $('dspEmpty').hidden=true;
  const shown = DSP_CLEAN ? archived.filter(g=>g.items.some(r=>r.can_delete!==false)) : null;
  const uncShown = DSP_CLEAN ? uncDead.filter(r=>r.can_delete!==false) : uncDead;
  if(!DSP_CLEAN&&!rows.length){
    list.hidden=true;$('dspEmpty').hidden=false;
    $('dspEmpty').querySelector('b').textContent='还没有可读的会话';
  }else{
    if(!DSP_CLEAN){
      active.forEach((g,i)=>list.append(
        dspGroup(agent,g.label,g.items,g.fullProject,false,i===0)));
      // 单任务会话统一收进一个「未分类任务」组，默认折叠。
      if(uncLive.length)list.append(dspGroup(agent,'未分类任务',uncLive,'',false,false,
        agent.exe+'|__unclassified'));
    }
    // 已归档全部收进列表底部的独立分区：不再跟活跃项目平铺在同一条列表里。
    // 清理模式始终渲染这一区（哪怕一条可删的都没有），退出清理的入口就在区头上。
    const ar= DSP_CLEAN ? shown : archived;
    const archTotal=DSP_CLEAN
      ?deletable.length
      :ar.reduce((n,g)=>n+g.items.length,0)+uncShown.length;
    if(ar.length||uncShown.length||DSP_CLEAN){
      const box=dspArchiveSection(agent,archTotal,DSP_CLEAN,deletable.length,DSP_CLEAN);
      ar.forEach((g,i)=>box.body.append(
        dspGroup(agent,g.label,g.items,g.fullProject,true,DSP_CLEAN?true:i===0,
          agent.exe+'|__archived|'+g.key,true)));
      // 没有项目的已归档单任务也在区里，跟活跃的未分类任务各记各的折叠状态。
      if(uncShown.length)box.body.append(
        dspGroup(agent,'未分类任务',uncShown,'',true,DSP_CLEAN,
          agent.exe+'|__unclassified-archived',true));
      if(DSP_CLEAN&&!ar.length&&!uncShown.length){
        const e=document.createElement('div');e.className='dspsecempty';
        e.textContent='没有可删除的归档会话：这个 Agent 没有已归档记录，或宿主不提供安全删除。';
        box.body.append(e);
      }
      list.append(box.el);
    }
    list.scrollTop=DSP_NEW?0:keep;
    DSP_NEW=false;
  }
  updateDspBatch();
  if(!DSP_CLEAN&&Date.now()-DSP_AT>30000&&!DSP_LOADING)requestSessions();
}
function openDSession(agent,row){
  DSCHAT={agent,row,meta:{},canDispatch:row.can_dispatch!==false};
  dchatOpen=true;dchatKey=agent.exe+'|'+row.id;
  dchatMsgs=[];dchatIds=new Set();dchatTotal=0;dchatHasMore=false;dchatLoading=true;
  DCHAT_PENDING='';DCHAT_REFRESHING=false;
  $('dchatText').value='';$('dchatNotice').hidden=true;$('dchatNotice').textContent='';
  render();
  postObj({cmd:'sessionOpen',exe:agent.exe,session:row.id,offset:0,limit:50});
}
function renderDchatHeader(){
  if(!DSCHAT)return;
  const row=DSCHAT.row,agent=DSCHAT.agent,m=DSCHAT.meta||{};
  $('dchatTitle').textContent=m.sessionTitle||m.title||row.title||'会话';
  $('dchatStage').textContent=LABEL[row.stage]||'';
  $('dchatStage').style.color=COLOR[row.stage]||'';
  $('dchatMeta').textContent=(agent.name||agent.exe||'')
    // 宿主自动生成的临时工作区（日期目录）不当项目名露出来，按单任务处理。
    +(dspHasProject(row.project)&&!row.generated?' · '+dspProjectName(row.project):'')
    +' · 共 '+dchatTotal+' 条';
  $('dchatText').placeholder=DSCHAT.canDispatch
    ?'写一句任务，派到这个会话（Enter 发送）'
    :'这个 Agent 暂不支持手机/桌面派发，记录只读';
  $('dchatText').disabled=!DSCHAT.canDispatch;
  $('dchatSend').disabled=!DSCHAT.canDispatch;
  if(!DSCHAT.canDispatch){
    $('dchatNotice').hidden=false;
    $('dchatNotice').textContent='只读：该 Agent 的派发通道未开通；记录仍可查看。';
  }
}
function paintDchat(mode,prevH,prevTop){
  const list=$('dchatList');list.replaceChildren();
  const m=DSCHAT&&DSCHAT.meta||{};
  if(!dchatMsgs.length){
    const e=document.createElement('div');e.className='chatempty';
    e.textContent=(m.supported===false)
      ?(m.error||'暂不支持聊天记录查看'):'这条会话还没有对话记录';
    list.append(e);
  }else{
    dchatMsgs.forEach(msg=>list.append(bubble(msg)));
  }
  if(mode==='init'){
    list.scrollTop=list.scrollHeight;
    requestAnimationFrame(()=>{list.scrollTop=list.scrollHeight;
      requestAnimationFrame(()=>{list.scrollTop=list.scrollHeight;});});
  }else if(mode==='older'){
    list.scrollTop=prevTop+(list.scrollHeight-prevH);
  }
  renderDchatHeader();
}
window.__setSessionChat=function(p){
  if(!dchatOpen||!DSCHAT||!p||!p.sessionId||p.sessionId!==DSCHAT.row.id)return;
  DSCHAT.meta=p;
  dchatTotal=p.total||0;
  const msgs=p.messages||[];
  const list=$('dchatList');
  const prevH=list.scrollHeight,prevTop=list.scrollTop;
  if((p.offset||0)>0){
    const fresh=msgs.filter(m=>!dchatIds.has(msgKey(m)));
    fresh.forEach(m=>dchatIds.add(msgKey(m)));
    dchatMsgs=fresh.concat(dchatMsgs);
    paintDchat('older',prevH,prevTop);
  }else{
    dchatMsgs=msgs.slice();dchatIds=new Set(dchatMsgs.map(msgKey));
    paintDchat('init');
  }
  dchatLoading=false;
  dchatHasMore=dchatMsgs.length<dchatTotal;
  if(DCHAT_REFRESHING){
    DCHAT_REFRESHING=false;
    $('dchatNotice').hidden=true;$('dchatNotice').textContent='';
  }
  renderDchatHeader();
};
function sendDchat(){
  if(!dchatOpen||!DSCHAT||!DSCHAT.canDispatch)return;
  const t=$('dchatText').value.trim();if(!t)return;
  DCHAT_PENDING=t;
  $('dchatSend').disabled=true;$('dchatSend').textContent='派发中…';
  const n=$('dchatNotice');n.hidden=false;n.textContent='正在派发，等回执…';
  postObj({cmd:'dispatchSession',exe:DSCHAT.agent.exe,session:DSCHAT.row.id,
    project:DSCHAT.row.project||'',text:t});
}
window.__setDispatchResult=function(r){
  if(!dchatOpen||!DSCHAT)return;
  $('dchatSend').disabled=!DSCHAT.canDispatch;$('dchatSend').textContent='派发';
  const n=$('dchatNotice');n.hidden=false;
  if(r&&r.ok){
    if($('dchatText').value===DCHAT_PENDING)$('dchatText').value='';
    n.textContent='已派发。送达回执到了会进「待我处理」；这里继续看会话记录。';
  }else{
    n.textContent='没发出去：'+((r&&r.error)||'未知原因')+'（草稿已保留）';
  }
  DCHAT_PENDING='';
};
function closeDSession(){
  dchatOpen=false;DSCHAT=null;dchatMsgs=[];dchatIds=new Set();dchatKey='';
  $('dchatSend').disabled=false;$('dchatSend').textContent='派发';
  render();
}
function render(){
  renderTop();
  renderLeft();
  renderTabs();
  const workView=activeTab===0;
  const dispatchView=activeTab===1;
  const chatOn=workView&&!!(STATE.chat&&STATE.chat.id);
  const dchatOn=dispatchView&&dchatOpen;
  $('chat').hidden=!chatOn;
  $('work').hidden=!workView||chatOn;
  $('dispatch').hidden=!dispatchView||dchatOn;
  $('dchat').hidden=!dchatOn;
  // 底部提示/批量条只属于待办列表：聊天页有自己的输入行，不再叠一条提示。
  $('foot').hidden=!workView||chatOn;
  $('events').hidden=activeTab!==2;
  $('system').hidden=activeTab!==3;
  $('keymap').hidden=activeTab!==4;
  $('apps').hidden=activeTab!==5;
  $('guide').hidden=activeTab!==6;
  $('empty').hidden=true;
  if(dispatchView){
    if(dchatOn){renderDchatHeader();return;}
    renderDispatch();return;
  }
  if(activeTab===2){renderEvents();return;}
  if(activeTab===3){renderSystem();return;}
  if(activeTab===4){renderKeymap();return;}
  if(activeTab===5){renderApps();return;}
  if(activeTab===6){renderGuide();return;}
  if(!workView){
    return;
  }
  if(chatOn){renderChatFrame();return;}
  const rows=sorted();
  if(!rows.length){
    const filtered=!!filterExe;
    $('empty').hidden=false;
    $('empty').querySelector('h3').textContent=
      filtered?'这个 Worker 没有待办':'没有需要你处理的事';
    $('total').textContent='';
    $('chips').replaceChildren();
    $('list').replaceChildren();
    renderFilter();renderHealth();renderFoot();
    return;
  }
  $('total').textContent=rows.length+' 件待办';
  renderChips();renderFilter();renderHealth();
  // 重名计数与 Qt 侧 whereHits 同一口径：agent|where 出现几次。
  WHERE_HITS={};
  rows.forEach(r=>{const k=agentName(r)+'|'+where(r);
    WHERE_HITS[k]=(WHERE_HITS[k]||0)+1;});
  // 增量刷新：整页不重载，滚动位置自己留着（原来每次 NavigateToString 都会回顶部）。
  const list=$('list');const keep=list.scrollTop;
  list.replaceChildren();
  rows.forEach(r=>list.append(row(r)));
  list.scrollTop=keep;
  renderFoot();
}

// ---------- 整窗 chrome：顶栏 ----------
function renderTop(){
  const on=!!HEADER.listening;
  // 监听态只画一颗点（胶囊与文字已去掉）：tooltip 跟着状态说清「点一下会发生什么」。
  $('listenPill').className='pill'+(on?' on':'');
  $('listenDot').className='dot'+(on?' on':'');
  const pill=$('listenPill');
  pill.title=on?'监听中：点一下停止监听':'未监听：点一下开始监听';
  pill.setAttribute('aria-label',on?'监听中，点击停止':'未监听，点击开始');
  // 最大化/还原是同一颗键上的两个图标（.gmax / .grest），切 class 而不是换字。
  $('wMax').classList.toggle('maxed',!!HEADER.maximized);
}
// ---------- 左栏：下拉原地更新，开着下拉时不被 5 秒刷新关掉 ----------
function setSel(el,opts,value,enabled){
  const sig=JSON.stringify(opts);
  if(el.dataset.sig!==sig){
    el.dataset.sig=sig;
    el.replaceChildren();
    opts.forEach(o=>{const op=document.createElement('option');
      op.value=o.id;op.textContent=o.name;el.append(op);});
  }
  if(enabled!==undefined) el.disabled=!enabled;
  if(document.activeElement!==el && el.value!==value) el.value=value;
}
// 短名：独立面板一行要挤三个按钮，长标签会被截成「本地 Q」。
const BACKENDS=[{id:'hotkey',name:'转发'},
  {id:'sensevoice',name:'Sense'},{id:'qwen3',name:'Qwen'},
  {id:'api',name:'云端'}];
// 遥控器实体：键位坐标照 RemotePanel 的排布，灯态走 mode/lit/linked 三层。
const RPOS={0:[35,10,'○'],122:[35,40,'◎'],116:[35,70,'●'],38:[35,100,'↑'],
  37:[12,130,'←'],13:[35,130,'OK'],39:[58,130,'→'],40:[35,160,'↓'],
  27:[12,192,'↩'],93:[35,192,'☰'],36:[58,192,'⌂'],vol:[40,224,'']};
function buildRemote(){
  const dev=$('rDev');
  if(dev.children.length) return;
  Object.keys(RPOS).forEach(k=>{
    const [x,y,g]=RPOS[k];
    const b=document.createElement('span');
    b.className='rk'+(k==='vol'?' vol':'');
    b.dataset.vk=k;
    b.style.left=x+'px';b.style.top=y+'px';
    b.textContent=g;
    dev.append(b);});
}
function waveBars(el){
  if(el.children.length) return;
  for(let i=0;i<40;i++){const b=document.createElement('i');el.append(b);}
}
function paintWave(el,level,rec,on){
  waveBars(el);
  el.className='wave'+(rec?' rec':(on?' on':''));
  const t=Date.now()/90;
  [...el.children].forEach((b,i)=>{
    const idle=4+Math.round(10*Math.abs(Math.sin(i*0.55)));
    const h=rec?Math.max(3,Math.round(3+level*30*
      (0.35+0.65*Math.abs(Math.sin(i*0.5+t))))):idle;
    b.style.height=h+'px';});
}
let litVk=0, litUntil=0;
function renderLeft(){
  const L=LEFT, V=L.voice||{}, P=L.picker||{}, R=L.remote||{}, W=L.vpage||{};
  $('lremote').hidden=!!L.form;
  $('lvoice').hidden=!L.form;
  // 语音卡
  setSel($('vBackend'),BACKENDS,V.backend||'hotkey');
  setSel($('vBackend2'),BACKENDS,V.backend||'hotkey');
  const st=$('vState');
  st.className='vstate'+(V.kind&&V.kind!=='dim'?' '+V.kind:'');
  st.replaceChildren();
  const s1=document.createElement('span');s1.textContent=V.status||'';st.append(s1);
  if(V.transcript){const tr=document.createElement('span');tr.className='tr';
    tr.textContent=V.transcript.length>40?V.transcript.slice(0,40)+'…':V.transcript;
    tr.title=V.transcript;st.append(tr);}
  $('vTest').disabled=!V.testEnabled;
  $('vTest2').disabled=!V.testEnabled;
  $('vFoot').textContent=V.foot||'';
  $('vFoot').hidden=!V.footVisible;
  setSel($('pkAgent'),P.agents||[],P.agent||'');
  setSel($('pkMode'),P.modes||[],P.mode||'',P.modeEnabled);
  setSel($('pkAgent2'),(W.picker||{}).agents||[],(W.picker||{}).agent||'');
  setSel($('pkMode2'),(W.picker||{}).modes||[],(W.picker||{}).mode||'',
         (W.picker||{}).modeEnabled);
  paintWave($('vWave'),0,false,!!HEADER.listening);
  paintWave($('vWave2'),0,false,!!HEADER.listening);
  // 遥控器卡
  $('rBadge').textContent=R.badge||'待机';
  $('rState').textContent=R.state||'';
  $('rDot').className='dot'+(R.listening?' on':'');
  buildRemote();
  const mode=new Set(R.modeKeys||[]);
  const lit=Date.now()<litUntil?litVk:0;
  [...$('rDev').children].forEach(b=>{
    const vk=b.dataset.vk==='vol'?0:+b.dataset.vk;
    b.className='rk'+(b.dataset.vk==='vol'?' vol':'')+
      (mode.has(vk)?' mode':'')+((lit&&lit===vk)?' lit':'');});
  const leg=$('rLegend');
  const entries=R.entries||[];
  const lsig=JSON.stringify(entries)+ '|' + lit;
  if(leg.dataset.sig!==lsig){
    leg.dataset.sig=lsig;
    leg.replaceChildren();
    entries.forEach(e=>{
      const r=document.createElement('div');
      r.className='rl'+(e.infrared?' ir':'')+((lit&&lit===e.vk)?' lit':'');
      const g=document.createElement('span');g.className='g';
      // Qt 侧话筒/喇叭是矢量图标：web 用同形 SVG，别拿 VOL 三个字母挤图标列。
      if(e.glyph==='VOL'){g.innerHTML='<svg width="12" height="10" viewBox="0 0 12 10"><path d="M1 3.5h2.5L7 1v8L3.5 6.5H1z" fill="currentColor"/><path d="M8.5 3a3 3 0 0 1 0 4" stroke="currentColor" fill="none"/></svg>';}
      else if(!e.glyph){g.innerHTML='<svg width="10" height="12" viewBox="0 0 10 12"><rect x="3" y="0.5" width="4" height="6.5" rx="2" fill="currentColor"/><path d="M1.5 5.5v1a3.5 3.5 0 0 0 7 0v-1M5 10v1.5" stroke="currentColor" fill="none"/></svg>';}
      else{g.textContent=e.glyph;}
      const a=document.createElement('span');a.className='a';
      a.textContent=e.action||'';a.title=(e.label||'')+' · '+(e.action||'');
      r.append(g,a);leg.append(r);});
  }
  // 语音形态
  $('vDot2').className='dot'+(HEADER.listening?' on':'');
  $('readLast').disabled=!W.readLastEnabled;
  $('vLine').hidden=!W.stateLineVisible;
  const hist=W.history||[];
  $('vHist').hidden=!hist.length;
  $('vEmpty').hidden=!!hist.length;
  const hs=JSON.stringify(hist);
  if($('vHist').dataset.sig!==hs){
    $('vHist').dataset.sig=hs;
    const keep=$('vHist').scrollTop;
    $('vHist').replaceChildren();
    hist.forEach(h=>{const d=document.createElement('div');
      // 收发分边：我说的靠右（正文色），Agent 回复靠左（强调色）。
      d.className='vh '+(h.in?'in':'out');
      d.textContent=h.t;d.title='点击复制';
      d.onclick=()=>postObj({cmd:'copy',text:h.full});
      $('vHist').append(d);});
    $('vHist').scrollTop=keep;
  }
  $('chatBtn').className='btn'+(W.chatMode?' toggled':'');
}
// 20Hz 轻量通道：只动波形和按键灯，不重排页面。
window.__setLive=function(l){
  if(l.level!==undefined||l.recording!==undefined){
    const rec=l.recording!==undefined?l.recording:HEADER.recording;
    const lv=l.level||0;
    paintWave($('vWave'),lv,rec,!!HEADER.listening);
    paintWave($('vWave2'),lv,rec,!!HEADER.listening);
  }
  if(l.lit){litVk=l.lit;litUntil=Date.now()+350;renderLeft();}
};
// ---------- chrome 接线 ----------
$('listenPill').onclick=()=>postObj({cmd:'listen'});
$('hUpdate').onclick=()=>postObj({cmd:'update'});
$('hTheme').onclick=()=>postObj({cmd:'theme'});
$('hGuide').onclick=()=>postObj({cmd:'guide'});
$('wMin').onclick=()=>postObj({cmd:'winMin'});
$('wMax').onclick=()=>postObj({cmd:'winMax'});
$('wClose').onclick=()=>postObj({cmd:'winClose'});
// 边缘热区：按下就把方向交给 Qt 进系统缩放循环（鼠标抓住窗口边、跟着走）。
// 只在主按钮按下时发；触屏/右键不动。
document.querySelectorAll('.rz').forEach(el=>{
  el.addEventListener('mousedown',e=>{
    if(e.button!==0) return;
    e.preventDefault();e.stopPropagation();
    postObj({cmd:'winResize',dir:el.dataset.dir});
  });
});
$('topbar').addEventListener('mousedown',e=>{
  if(e.target.closest('button,select')) return;
  postObj({cmd:'winDrag'});});
$('topbar').addEventListener('dblclick',e=>{
  if(e.target.closest('button,select')) return;
  postObj({cmd:'winMaxToggle'});});
$('vBackend').onchange=e=>postObj({cmd:'vBackend',key:e.target.value});
$('vBackend2').onchange=e=>postObj({cmd:'vBackend',key:e.target.value});
$('vTest').onclick=()=>postObj({cmd:'vTest'});
$('vTest2').onclick=()=>postObj({cmd:'vTest'});
$('vSettings').onclick=()=>postObj({cmd:'vSettings'});
$('vSettings2').onclick=()=>postObj({cmd:'vSettings'});
$('vMain').onclick=()=>postObj({cmd:'vMain'});
$('vWave').onclick=()=>postObj({cmd:'vWave'});
$('vWave2').onclick=()=>postObj({cmd:'vWave'});
$('pkAgent').onchange=e=>postObj({cmd:'pkAgent',id:e.target.value});
$('pkMode').onchange=e=>postObj({cmd:'pkMode',id:e.target.value});
$('pkAgent2').onchange=e=>postObj({cmd:'pkAgent',id:e.target.value});
$('pkMode2').onchange=e=>postObj({cmd:'pkMode',id:e.target.value});
$('toVoice').onclick=()=>postObj({cmd:'leftForm',voice:true});
$('toRemote').onclick=()=>postObj({cmd:'leftForm',voice:false});
$('btSet').onclick=()=>postObj({cmd:'bt'});
$('readLast').onclick=()=>postObj({cmd:'readLast'});
$('vSend').onclick=()=>{const t=$('vTyped').value;
  if(t.trim()){postObj({cmd:'vTyped',text:t});$('vTyped').value='';}};
$('vTyped').addEventListener('keydown',e=>{
  if(e.key==='Enter'){const t=e.target.value;
    if(t.trim()){postObj({cmd:'vTyped',text:t});e.target.value='';}}});
$('chatBtn').onclick=()=>postObj({cmd:'chatToggle',on:!((LEFT.vpage||{}).chatMode)});
$('ttsBtn').onclick=()=>postObj({cmd:'tts'});
$('clear').onclick=()=>post('clear');
$('evClear').onclick=()=>post('clearLog');
$('multi').onclick=()=>{multiMode=!multiMode;multiSel.clear();render();};
$('ackBatch').onclick=()=>{multiSel.forEach(id=>post('ack',id));
  multiSel.clear();render();};
$('chatRestart').onclick=()=>{
  if(confirm('重启 ZCode 桌面？会中断它里面正在跑的其它轮次；重启后打开会话即可看到派发消息全文。'))
    postObj({cmd:'restartZcode'});
};
$('workGuide').onclick=()=>{activeTab=6;render();};
// 系统/映射/应用页的动作按钮：状态机全在 Qt 侧，这里只发命令。
$('sysRefresh').onclick=()=>postObj({cmd:'sysRefresh'});
$('sysBridge').onclick=()=>postObj({cmd:'sysBridge'});
$('sysUpgrade').onclick=()=>postObj({cmd:'sysUpgrade'});
$('sysOpenDir').onclick=()=>postObj({cmd:'sysOpenDir'});
$('kmProfiles').onclick=()=>postObj({cmd:'keyProfiles'});
$('kmReload').onclick=()=>postObj({cmd:'keyReload'});
$('apAdd').onclick=()=>postObj({cmd:'appAdd'});
$('apRescan').onclick=()=>postObj({cmd:'appRescan'});
$('dspRefresh').onclick=()=>requestSessions(true);
$('dspBatchCancel').onclick=()=>{
  if(DSP_DELETING)return;
  DSP_CLEAN=false;DSP_DEL_SEL.clear();DSP_SIG='';render();
};
$('dspCheckAll').onclick=()=>{
  if(DSP_DELETING)return;
  const rows=dspArchivedRows(dspCurrentAgent());
  if(rows.length&&DSP_DEL_SEL.size===rows.length)DSP_DEL_SEL.clear();
  else rows.forEach(r=>DSP_DEL_SEL.add(DSP_SELECTED+'|'+r.id));
  DSP_SIG='';render();
};
$('dspDelete').onclick=()=>{
  if(DSP_DELETING)return;
  const agent=dspCurrentAgent();if(!agent)return;
  const pairs=dspArchivedRows(agent)
    .filter(r=>DSP_DEL_SEL.has(agent.exe+'|'+r.id))
    .map(row=>({agent,row}));
  if(pairs.length)deleteRows(pairs);
};
$('dchatBack').onclick=closeDSession;
$('dchatRefresh').onclick=()=>{
  if(!DSCHAT)return;
  DCHAT_REFRESHING=true;
  const n=$('dchatNotice');n.hidden=false;n.textContent='正在刷新记录…';
  postObj({cmd:'sessionOpen',exe:DSCHAT.agent.exe,session:DSCHAT.row.id,
    offset:0,limit:50});
};
$('dchatSend').onclick=sendDchat;
$('dchatText').addEventListener('keydown',e=>{
  if(e.key==='Enter'){e.preventDefault();sendDchat();}
});
$('dchatList').addEventListener('scroll',()=>{
  const list=$('dchatList');
  if(list.scrollTop<80&&dchatHasMore&&!dchatLoading&&DSCHAT){
    dchatLoading=true;
    postObj({cmd:'sessionOlder',exe:DSCHAT.agent.exe,
      session:DSCHAT.row.id,offset:dchatMsgs.length,limit:50});
  }
});
document.addEventListener('keydown',e=>{
  // 快捷键只属于待办页：Qt 侧事件流是 NoSelection/NoFocus，别的页也不接。
  if(activeTab!==0 || !selected) return;
  if(STATE.chat&&STATE.chat.id) return;  // 聊天页有自己的 Enter 发送
  if(e.target && e.target.tagName==='INPUT') return;
  if(e.key==='Enter'){post(window.ZC_NOTICE?'raiseZcode':'open',selected);}
  else if(e.key==='Delete'){
    if(multiMode){multiSel.forEach(id=>post('ack',id));
      multiSel.clear();render();}
    else post('ack',selected);
  }
});
// ---------- 聊天页：打开即最新，旧记录上翻按页加载 ----------
let CHAT=null,chatMsgs=[],chatIds=new Set(),chatTotal=0,chatHasMore=false,
    chatLoading=false,chatOpenedId='',chatAtBottom=true;
function msgKey(m){return m.id||(m.at+'|'+m.role+'|'+(m.text||'').slice(0,80));}
function bubble(m){
  const d=document.createElement('div');
  d.className='bub '+(m.role==='user'?'user':'assistant');
  const who=document.createElement('span');who.className='who';
  who.textContent=m.role==='user'?'我':'Agent';
  const body=document.createElement('span');body.textContent=m.text||'';
  d.append(who,body);
  if(m.at){const t=document.createElement('span');t.className='bt';
    t.textContent=hhmm(m.at);d.append(t);}
  return d;
}
// 内容没填满视口时没有滚动事件，上翻加载永远不会触发——自动续载旧页
// 直到填满或没有更旧（高窗口实测 bug）。
// B1：待答请求只读卡片（授权/提问）。内联应答是 B2/B3，这里先展示。
function requestCard(req){
  const d=document.createElement('div');d.className='reqcard';
  d.dataset.reqId=req.id||'';   // 失败回执按 id 找卡（见 applyAnswerFailure）
  const head=document.createElement('div');head.className='reqhead';
  if(req.kind==='approval'){
    head.textContent='🔐 等你授权'+(req.toolName?'：'+req.toolName:'');
    d.append(head);
    if(req.reason){const r=document.createElement('div');r.className='reqreason';
      r.textContent=req.reason;d.append(r);}
    if(req.answerable){
      // B3：可答授权——允许/拒绝两键，提交 outcome 走 answer 动作。
      const row=document.createElement('div');row.className='reqrow';
      const mk=(label,outcome,primary)=>{const b=document.createElement('button');
        b.className='btn reqbtn'+(primary?' primary':'');b.textContent=label;
        b.onclick=()=>{row.querySelectorAll('button').forEach(x=>x.disabled=true);
          b.textContent='已提交…';
          postObj({cmd:'answer',id:chatOpenedId,requestId:req.id,outcome:outcome});};
        return b;};
      row.append(mk('允许','allowed-once',true),mk('拒绝','rejected',false));
      d.append(row);
      // 回执行：提交失败时把原因写在卡上（原来只在页顶飘 notice，长记录里
      // 滚出去就看不见，用户 2026-11-16 反馈「点了没反应」）。
      const st=document.createElement('div');st.className='reqstate';st.hidden=true;
      d.append(st);d._reqState=st;
    }
  }else if(!req.answerable){
    head.textContent='❓ 等你选方案';d.append(head);
    (req.questions||[]).forEach(q=>{
      const qd=document.createElement('div');qd.className='reqq';
      const qt=document.createElement('div');qt.className='reqqt';
      qt.textContent=q.question||q.header||'';qd.append(qt);
      (q.options||[]).forEach(o=>{const od=document.createElement('div');
        od.className='reqopt';od.textContent='· '+o.label;qd.append(od);});
      d.append(qd);
    });
  }else{
    // B2：可答——每题一列按钮，点即选；「提交回答」经 handleWebAction 排队给插件。
    head.textContent='❓ 等你选方案';d.append(head);
    const sel={};const custom={};
    (req.questions||[]).forEach(q=>{
      const qd=document.createElement('div');qd.className='reqq';
      const qt=document.createElement('div');qt.className='reqqt';
      qt.textContent=q.question||q.header||'';qd.append(qt);
      sel[q.id]=new Set();
      (q.options||[]).forEach(o=>{
        const b=document.createElement('button');b.className='reqbtn';
        b.textContent=o.label;
        if(o.description){b.title=o.description;}
        b.onclick=()=>{
          if(q.multi_select){sel[q.id].has(o.label)?sel[q.id].delete(o.label):sel[q.id].add(o.label);}
          else{sel[q.id]=new Set([o.label]);custom[q.id]='';}
          syncSel();
        };
        qd.append(b);
      });
      const ci=document.createElement('input');ci.className='reqcustom';
      ci.type='text';ci.placeholder='或自己写一个回答…';
      ci.oninput=()=>{custom[q.id]=ci.value.trim();
        if(custom[q.id]){sel[q.id].clear();syncSel();}};
      qd.append(ci);
      const syncSel=()=>{
        const btns=qd.querySelectorAll('.reqbtn');
        (q.options||[]).forEach((o,i)=>{if(btns[i])btns[i].classList.toggle('on',sel[q.id].has(o.label));});
      };
      d.append(qd);
    });
    const send=document.createElement('button');send.className='btn primary reqsend';
    send.textContent='提交回答';
    send.onclick=()=>{
      const batch=(req.questions||[]).map(q=>({id:q.id,
        selected:[...(sel[q.id]||[])],
        ...(String(custom[q.id]||'')?{custom:String(custom[q.id])}:{}),
      }));
      if(batch.some(a=>!a.selected.length&&!a.custom)){
        stateEl.textContent='每个问题都要选一项或写一句';return;}
      send.disabled=true;send.textContent='已提交，等确认…';
      postObj({cmd:'answer',id:chatOpenedId,requestId:req.id,answers:batch});
    };
    const stateEl=document.createElement('div');stateEl.className='reqstate';
    d.append(send,stateEl);
  }
  const hint=document.createElement('div');hint.className='reqhint';
  hint.textContent=!req.answerable?'暂在宿主界面应答（内联应答未开启）'
    :(req.kind==='approval'?'允许/拒绝直接生效——等于替你在电脑上点了按钮'
    :'点选项后提交——等于替你在电脑 DSH 界面里点了按钮');
  d.append(hint);
  return d;
}
// 应答失败回执：把原因写进**对应那张卡**，并恢复按钮可用（能重试）。
// 卡片每次重绘都会重建，所以由 renderChatFrame 在每帧调用，按 id 找卡。
function applyAnswerFailure(){
  const d=STATE.dispatch||{};
  if(!d.noticeRequest||d.noticeId!==chatOpenedId) return;
  const card=[...document.querySelectorAll('.reqcard')]
    .find(c=>c.dataset.reqId===d.noticeRequest);
  if(!card) return;
  const st=card._reqState;
  if(st){st.hidden=false;st.textContent=d.notice||'提交失败，请重试';
    st.style.color='var(--danger)';}
  card.querySelectorAll('.reqbtn').forEach(b=>{b.disabled=false;});
}
function maybeAutoLoad(){  const list=$('chatList');
  if(chatHasMore&&!chatLoading&&chatOpenedId&&
     list.scrollHeight<=list.clientHeight+80){
    chatLoading=true;
    const ld=$('chatLoad');if(ld)ld.textContent='正在加载更早的记录…';
    postObj({cmd:'chatOlder',id:chatOpenedId,offset:chatMsgs.length});
  }
}
// mode: init＝首帧直接定位到最新（无滚动动画）；prepend＝上翻旧页保持视觉锚点；
// append＝新消息，在底部就跟随，不在底部就亮 pill 不打断读历史。
function paintChat(mode,prevH,prevTop){
  const list=$('chatList');
  list.replaceChildren();
  if(chatHasMore){
    const ld=document.createElement('div');ld.className='chatload';ld.id='chatLoad';
    ld.textContent=chatLoading?'正在加载更早的记录…':'上翻加载更早的记录';
    list.append(ld);
  }
  if(!chatMsgs.length){
    const e=document.createElement('div');e.className='chatempty';
    e.textContent=(CHAT&&CHAT.supported===false)
      ?(CHAT.error||'暂不支持聊天记录查看'):'这条会话还没有对话记录';
    list.append(e);
  }
  chatMsgs.forEach(m=>list.append(bubble(m)));
  ((CHAT&&CHAT.requests)||[]).forEach(req=>list.append(requestCard(req)));
  if(mode==='init'){
    chatAtBottom=true;$('chatNew').hidden=true;
    // 打开即最新：同步定位 + 多帧兜底（WebView2 解除隐藏后布局可能晚落定，
    // 切换会话的边界场景实测会落空，setTimeout 重试兜住）。
    const pin=()=>{list.scrollTop=list.scrollHeight;};
    pin();
    requestAnimationFrame(pin);
    setTimeout(pin,60);setTimeout(pin,200);setTimeout(pin,500);
  }else if(mode==='prepend'){
    list.scrollTop=prevTop+(list.scrollHeight-prevH);
  }else{
    if(chatAtBottom){list.scrollTop=list.scrollHeight;$('chatNew').hidden=true;}
    else{$('chatNew').hidden=false;}
  }
  maybeAutoLoad();
}
function mergeChatPage(p,mode){
  chatTotal=p.total||0;
  const msgs=p.messages||[];
  if(mode==='init'){
    chatMsgs=msgs.slice();chatIds=new Set(chatMsgs.map(msgKey));
    paintChat('init');
  }else{
    const fresh=msgs.filter(m=>!chatIds.has(msgKey(m)));
    if(fresh.length){
      fresh.forEach(m=>{chatIds.add(msgKey(m));chatMsgs.push(m);});
      paintChat('append');
    }
  }
  // 本地算剩余：轮询推的是最新页，它的 hasMore 不代表已加载范围之外还有旧页。
  chatHasMore=chatMsgs.length<chatTotal;
  renderChatFrame();
}
function renderChatFrame(){
  if(!CHAT)return;
  $('chatTitle').textContent=CHAT.sessionTitle||CHAT.title||'会话';
  const st=$('chatStage');st.textContent=LABEL[CHAT.stage]||'';
  st.style.color=COLOR[CHAT.stage]||'var(--muted)';
  $('chatMeta').textContent=(CHAT.agentExe||'')+' · 共 '+chatTotal+' 条';
  const d=STATE.dispatch||{};
  const notice=(d.noticeId===chatOpenedId)?(d.notice||''):'';
  $('chatNotice').hidden=!notice;$('chatNotice').textContent=notice;
  // 失败原因也落到那张卡上（页顶 notice 在长记录里容易被滚出视野）。
  applyAnswerFailure();
  // 重启按钮跟着 ZCode 派发通知走（对话区不热刷新，重启才能看到全文）。
  $('chatRestart').hidden=!(notice && d.noticeExe==='ZCode.exe');
  if(d.draftId===chatOpenedId&&d.draftText&&!$('chatText').value)
    $('chatText').value=d.draftText;
  // 兜底：只要用户停在底部，每次状态推送都重新贴底——首帧定位若因布局
  // 晚落定而落空，这里最迟下一拍补上（高窗口/切换会话边界场景）。
  if(chatAtBottom){const l=$('chatList');
    if(l.scrollHeight>l.clientHeight)l.scrollTop=l.scrollHeight;}
  renderFoot();
}
// 上翻的旧页由 Qt 单独推来（chatOlder → pushChat），不进整份 state。
window.__setChatPage=function(p){
  if(!p||!p.id||p.id!==chatOpenedId)return;
  chatLoading=false;chatTotal=p.total||chatTotal;
  const fresh=(p.messages||[]).filter(m=>!chatIds.has(msgKey(m)));
  if(!fresh.length){const ld=$('chatLoad');
    if(ld)ld.textContent='上翻加载更早的记录';
    chatHasMore=chatMsgs.length<chatTotal;return;}
  const list=$('chatList');const prevH=list.scrollHeight,prevTop=list.scrollTop;
  fresh.forEach(m=>chatIds.add(msgKey(m)));
  chatMsgs=fresh.concat(chatMsgs);
  chatHasMore=chatMsgs.length<chatTotal;
  paintChat('prepend',prevH,prevTop);
};
function sendChat(){
  const t=$('chatText').value.trim();
  if(!t||!chatOpenedId)return;
  postObj({cmd:'dispatch',id:chatOpenedId,text:t});
  $('chatText').value='';
}
$('chatBack').onclick=()=>postObj({cmd:'chatBack'});
$('chatSend').onclick=sendChat;
$('chatText').addEventListener('keydown',e=>{
  if(e.key==='Enter'){e.preventDefault();sendChat();}});
$('chatNew').onclick=()=>{const l=$('chatList');l.scrollTop=l.scrollHeight;
  chatAtBottom=true;$('chatNew').hidden=true;};
$('chatList').addEventListener('scroll',()=>{
  const list=$('chatList');
  chatAtBottom=list.scrollHeight-list.scrollTop-list.clientHeight<60;
  if(chatAtBottom)$('chatNew').hidden=true;
  if(list.scrollTop<80&&chatHasMore&&!chatLoading&&chatOpenedId){
    chatLoading=true;
    const ld=$('chatLoad');if(ld)ld.textContent='正在加载更早的记录…';
    postObj({cmd:'chatOlder',id:chatOpenedId,offset:chatMsgs.length});
  }
});
function adopt(s){
  STATE=s||{};
  WORK=STATE.work||{items:[]};
  WORKERS=STATE.workers||[];
  EVENTS=STATE.events||[];
  HEADER=STATE.header||{};
  LEFT=STATE.left||{};
  SYSTEM=STATE.system||{};
  KEYMAP=STATE.keymap||{};
  APPS=STATE.apps||{};
  GUIDE=STATE.guide||{};
  now=Date.now();
  applyTheme(STATE.theme||{});
  const c=(STATE.chat&&STATE.chat.id)?STATE.chat:null;
  if(c){
    // 先 render 把聊天页显示出来，再喂数据：隐藏元素 scrollHeight=0，
    // 「打开即最新」的定位会在不可见时落空（实测 bug）。
    if(chatOpenedId!==c.id){
      chatOpenedId=c.id;selected=c.id;
      chatMsgs=[];chatIds=new Set();chatLoading=false;
      CHAT=c;render();mergeChatPage(c,'init');
    }else{
      CHAT=c;render();mergeChatPage(c,'append');
    }
  }else{
    if(chatOpenedId){chatOpenedId='';CHAT=null;chatMsgs=[];chatIds=new Set();}
    render();
  }
}
window.__setState=adopt;
window.__setTheme=applyTheme;
adopt(STATE);
</script></body></html>)HTML");
}
