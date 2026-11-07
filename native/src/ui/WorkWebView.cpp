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
    const qreal ratio = devicePixelRatioF();
    const RECT r{0, 0, static_cast<LONG>(width() * ratio),
                 static_cast<LONG>(height() * ratio)};
    d_->controller->put_IsVisible(width() > 0 && height() > 0 ? TRUE : FALSE);
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
 select{font-size:var(--f11);color:var(--text);background:var(--raised);
   border:1px solid var(--line);border-radius:8px;padding:3px 6px}
 .ghost{border:0;background:transparent;color:var(--text-dim);
   font-size:var(--f11);padding:2px 6px;cursor:default}
 .ghost:hover{color:var(--accent)}
 .health{flex:0 0 auto;color:var(--muted);font-size:var(--f11);margin-top:6px}
 .list{flex:1 1 auto;min-height:0;overflow-y:auto;margin-top:6px;
   scrollbar-width:none}
 .list::-webkit-scrollbar{width:0;height:0}
 .row{position:relative;padding:8px 12px 8px 21px;cursor:default}
 .row:hover{background:var(--w6)}
 .row.sel{background:var(--a36)}
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
 .placeholder{flex:1 1 auto;padding:40px;text-align:center;color:var(--muted);
   font-size:var(--f12)}

 /* ---------- 整窗外壳：顶栏 + 左栏（Qt chrome 藏起来后由这里画） ---------- */
 #topbar{flex:0 0 44px;display:flex;align-items:center;gap:10px;
   padding:8px 10px 4px 16px}
 .brandlogo{width:22px;height:22px;border-radius:6px;background:var(--accent);
   color:var(--on-accent);font-weight:700;font-size:var(--f12);
   display:flex;align-items:center;justify-content:center;flex:0 0 22px}
 .brand{font-size:var(--f13);font-weight:700;color:var(--text)}
 .pill{display:flex;align-items:center;gap:6px;padding:3px 11px 3px 9px;
   border-radius:13px;border:1px solid var(--w14);background:var(--w6);
   color:var(--muted);font-size:var(--f11);cursor:default}
 .pill.on{color:#3fb950;font-weight:600}
 .dot{width:6px;height:6px;border-radius:3px;background:var(--line);flex:0 0 6px}
 .dot.on{background:#3fb950}
 .hbtn{border:0;background:transparent;color:var(--text-dim);
   font-size:var(--f11);padding:2px 6px;cursor:default}
 .hbtn:hover{color:var(--accent)}
 .hbtn.boxed{border:1px solid var(--w18);border-radius:8px;padding:4px 10px}
 .hbtn.boxed:hover{background:var(--w8);color:var(--text)}
 .hbtn:disabled{color:var(--faint)}
 .winbtn{width:28px;height:24px;border:0;border-radius:6px;background:transparent;
   color:var(--text-dim);font-size:var(--f11);cursor:default;flex:0 0 28px}
 .winbtn:hover{background:var(--w10);color:var(--text)}
 .winbtn.close:hover{background:var(--danger);color:#fff}
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
   letter-spacing:1px}
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
 .wave.rec i{background:#3fb950}
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
 .vtrans{border:1px solid var(--w14);background:var(--w6);border-radius:10px;
   padding:8px;font-size:var(--f11);color:var(--text);word-break:break-word;
   cursor:default;max-height:96px;overflow:hidden;flex:0 0 auto}
 .vhist{flex:1 1 auto;min-height:0;overflow-y:auto;scrollbar-width:none;
   display:flex;flex-direction:column;gap:2px}
 .vhist::-webkit-scrollbar{width:0;height:0}
 .vh{font-size:var(--f11);color:var(--text-dim);padding:4px 6px;border-radius:6px;
   word-break:break-word;cursor:default;flex:0 0 auto}
 .vh:hover{background:var(--w6)}
 .vempty{flex:1 1 auto;display:flex;align-items:center;justify-content:center;
   color:var(--muted);font-size:var(--f11);text-align:center}
 .vdock{display:flex;flex-direction:column;gap:6px;flex:0 0 auto}
 .vrow{display:flex;gap:6px}
 .vrow input{flex:1;min-width:0;font-size:var(--f11);color:var(--text);
   background:var(--raised);border:1px solid var(--line);border-radius:8px;
   padding:5px 8px;font-family:inherit}
 .vrow input::placeholder{color:var(--muted2)}
 .btn.toggled{background:var(--a26);border-color:var(--a64);color:var(--accent)}
 #right{flex:1 1 auto;min-width:0;min-height:0;display:flex;
   flex-direction:column}
 #right .tabs{padding:0 2px}
 #right .card{margin:10px 0 0}
</style></head><body>
<div id="topbar">
  <span class="brandlogo">A</span><span class="brand">AgentRMC</span>
  <span class="spacer" style="margin-left:auto"></span>
  <button class="pill" id="listenPill" title="监听开关：点击开始 / 停止监听"><span class="dot" id="listenDot"></span><span id="listenText">未监听</span></button>
  <button class="hbtn" id="hUpdate" title="检查是否有新版本">检查更新</button>
  <button class="hbtn" id="hTheme" title="切换主题、调卡片不透明度与背景压暗">主题</button>
  <button class="hbtn boxed" id="hGuide" title="分步引导：没有遥控器怎么用、怎么配对、怎么接入 Agent">新手引导</button>
  <button class="winbtn" id="wMin">—</button>
  <button class="winbtn" id="wMax">□</button>
  <button class="winbtn close" id="wClose">✕</button>
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
    <div class="vtrans" id="vTrans" title="点击复制"></div>
    <div class="vhist" id="vHist"></div>
    <div class="vempty" id="vEmpty">说完的话会攒在这里 · 点击复制</div>
    <div class="vdock">
      <div class="vrow"><input type="text" id="vTyped" placeholder="打字说一句话，Enter 发送"><button class="btn" id="vSend">发送</button></div>
      <select id="vBackend2"></select>
      <div class="vrow"><button class="btn" id="chatBtn" title="桌宠对话模式：说话发给模型聊，回复用 TTS 念出来">对话</button><button class="btn" id="ttsBtn" title="桌宠语音反馈：Base URL / API Key / TTS 模型自填">语音反馈</button><button class="btn" id="vSettings2">模型设置</button></div>
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
    <button class="ghost" id="clear">清空已完成</button>
  </div>
  <div class="health" id="health"></div>
  <div class="list" id="list"></div>
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
<div class="foot" id="foot">
  <span class="hint" id="hint"></span>
  <button class="btn" id="raise" disabled>回到 Agent</button>
  <button class="btn primary" id="ack" disabled>标为已处理</button>
</div>
</div>
</div>
</div>
<script>
// 首帧的状态嵌在文档里；之后 Qt 只调 window.__setState(json) 换数据，不重载文档。
let STATE = )HTML")
                         + jsEscape(stateJson)
                         + QString::fromUtf8(R"HTML(;
let WORK = {items:[]}, WORKERS = [], EVENTS = [], now = Date.now();
const COLOR = {'needs-me':'#f0883e','review':'#3fb950','working':'#58a6ff'};
// 事件按 kind 上色，和 Qt 侧 appendEvent 同一套：错误红、听写/系统命令绿、
// 状态灰（muted token）、其余（按键等）用 text.dim。阶段色一样两边硬编码同值。
const EVCOLOR = {error:'#f85149',voice:'#3fb950',system:'#3fb950',status:'var(--muted)'};
let evSeen = 0;
let SYSTEM = {}, KEYMAP = {}, APPS = {}, GUIDE = {};
let HEADER = {}, LEFT = {};
const LABEL = {'needs-me':'等你处理','review':'待验收','working':'进行中'};
const TABS = ['待我处理','事件流','系统','按键映射','应用','使用引导'];
let activeTab = 0, selected = '', filterExe = '';
const $ = id => document.getElementById(id);
// CSS 变量名 → token 名，和 style.qss 里的 @token@ 同名；缺项保持 :root 的兜底值。
const TOKENS = {'--bg':'bg','--surface':'card.bg','--surface2':'card.bg2',
  '--text':'text','--text-soft':'text.soft','--text-dim':'text.dim',
  '--muted':'muted','--muted2':'muted.2','--ghost':'ghost','--faint':'faint',
  '--line':'line','--raised':'raised','--accent':'accent',
  '--accent-hover':'accent.hover','--on-accent':'on.accent','--danger':'danger',
  '--warn':'warn','--w6':'w6','--w8':'w8','--w10':'w10','--w14':'w14',
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
  return (b==='.'||b==='..'||!b)?('会话 '+String(it.session_id).slice(0,8)):b;}
function waitText(it){if(!it.attention_since) return '';
  const secs=(now-Date.parse(it.attention_since))/1000;
  if(!(secs>=600)) return '';const m=secs/60;
  if(m<60) return ' · 等 '+Math.floor(m)+' 分';
  if(m<1440) return ' · 等 '+Math.floor(m/60)+' 小时';
  return ' · 等 '+Math.floor(m/1440)+' 天';}
function isStale(it){return it.stage==='working' && it.updated_at &&
  (now-Date.parse(it.updated_at))>1800000;}
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
  const defs=[['needs-me','#f0883e','等你'],['review','#3fb950','待验收'],
              ['working','#58a6ff','进行中']];
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
  const last=(h.last_event_at||'').slice(11,16);
  $('health').innerHTML='<span style="color:'+(ok?'#3fb950':'#f0883e')+'">●</span> '+
    (ok?'桥接正常':'桥接未启动')+' · '+n+' 个 Worker'+
    (last?' · 最近 '+last:'');
}
function row(it){
  const el=document.createElement('div');
  el.className='row'+(it.id===selected?' sel':'');
  const bar=document.createElement('div');bar.className='bar';
  bar.style.background=stageColor(it);
  const l1=document.createElement('div');l1.className='l1';
  const name=document.createElement('span');name.className='name';name.textContent=it.title||'';
  const stage=document.createElement('span');stage.className='stage';
  stage.style.color=stageColor(it);stage.textContent=stageLabel(it);
  const wh=document.createElement('span');wh.className='where';wh.textContent=where(it);
  const time=document.createElement('span');time.className='time';
  time.textContent=(it.updated_at||'').slice(11,16)+waitText(it);
  l1.append(name,stage,wh,time);
  const l2=document.createElement('div');l2.className='l2';l2.textContent=it.message||'';
  el.append(bar,l1,l2);
  el.addEventListener('click',()=>{selected=it.id;render();});
  el.addEventListener('dblclick',()=>post('open',it.id));
  return el;
}
function renderFoot(){
  const selItem=(WORK.items||[]).find(i=>i.id===selected && !i.done);
  $('hint').textContent=selItem
    ? 'Enter 回到 Agent · Delete 标为已处理'
    : '选中一条待办后可操作 · 双击打开 Agent（没开着会先启动）并销账';
  $('raise').disabled=!selItem;
  $('ack').disabled=!selItem;
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
  if(!s.problemsOk) pr.style.color='#f0883e'; el.append(pr);
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
}
function renderKeymap(){
  const k=KEYMAP||{};
  // 场景行：跟随前台绿点、固定场景黄点（和 Qt refreshScene 同色）。
  $('kmDot').style.background = k.pinned ? '#d29922' : '#3fb950';
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
function render(){
  renderTop();
  renderLeft();
  renderTabs();
  const workView=activeTab===0;
  $('work').hidden=!workView;
  $('foot').hidden=!workView;
  $('events').hidden=activeTab!==1;
  $('system').hidden=activeTab!==2;
  $('keymap').hidden=activeTab!==3;
  $('apps').hidden=activeTab!==4;
  $('guide').hidden=activeTab!==5;
  $('empty').hidden=true;
  if(activeTab===1){renderEvents();return;}
  if(activeTab===2){renderSystem();return;}
  if(activeTab===3){renderKeymap();return;}
  if(activeTab===4){renderApps();return;}
  if(activeTab===5){renderGuide();return;}
  if(!workView){
    return;
  }
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
  $('listenPill').className='pill'+(on?' on':'');
  $('listenDot').className='dot'+(on?' on':'');
  $('listenText').textContent=on?'监听中':'未监听';
  $('wMax').textContent=HEADER.maximized?'❐':'□';
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
const BACKENDS=[{id:'hotkey',name:'转发热键 · 0 模型'},
  {id:'sensevoice',name:'本地 SenseVoice'},{id:'qwen3',name:'本地 Qwen3-ASR'},
  {id:'api',name:'云端 ASR（需联网）'}];
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
  const vt=$('vTrans');
  vt.textContent=W.transcript||'';
  vt.dataset.full=W.transcript||'';
  const hist=W.history||[];
  $('vHist').hidden=!hist.length;
  $('vEmpty').hidden=!!hist.length;
  const hs=JSON.stringify(hist);
  if($('vHist').dataset.sig!==hs){
    $('vHist').dataset.sig=hs;
    const keep=$('vHist').scrollTop;
    $('vHist').replaceChildren();
    hist.forEach(h=>{const d=document.createElement('div');d.className='vh';
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
$('vTrans').onclick=()=>postObj({cmd:'copy',text:$('vTrans').dataset.full||''});
$('vSend').onclick=()=>{const t=$('vTyped').value;
  if(t.trim()){postObj({cmd:'vTyped',text:t});$('vTyped').value='';}};
$('vTyped').addEventListener('keydown',e=>{
  if(e.key==='Enter'){const t=e.target.value;
    if(t.trim()){postObj({cmd:'vTyped',text:t});e.target.value='';}}});
$('chatBtn').onclick=()=>postObj({cmd:'chatToggle',on:!((LEFT.vpage||{}).chatMode)});
$('ttsBtn').onclick=()=>postObj({cmd:'tts'});
$('clear').onclick=()=>post('clear');
$('evClear').onclick=()=>post('clearLog');
$('raise').onclick=()=>post('open',selected);
$('ack').onclick=()=>post('ack',selected);
$('workGuide').onclick=()=>{activeTab=5;render();};
// 系统/映射/应用页的动作按钮：状态机全在 Qt 侧，这里只发命令。
$('sysRefresh').onclick=()=>postObj({cmd:'sysRefresh'});
$('sysBridge').onclick=()=>postObj({cmd:'sysBridge'});
$('sysUpgrade').onclick=()=>postObj({cmd:'sysUpgrade'});
$('sysOpenDir').onclick=()=>postObj({cmd:'sysOpenDir'});
$('kmProfiles').onclick=()=>postObj({cmd:'keyProfiles'});
$('kmReload').onclick=()=>postObj({cmd:'keyReload'});
$('apAdd').onclick=()=>postObj({cmd:'appAdd'});
$('apRescan').onclick=()=>postObj({cmd:'appRescan'});
document.addEventListener('keydown',e=>{
  // 快捷键只属于待办页：Qt 侧事件流是 NoSelection/NoFocus，别的页也不接。
  if(activeTab!==0 || !selected) return;
  if(e.key==='Enter'){post('open',selected);}
  else if(e.key==='Delete'){post('ack',selected);}
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
  render();
}
window.__setState=adopt;
adopt(STATE);
</script></body></html>)HTML");
}
