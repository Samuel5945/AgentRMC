#pragma once

#include <QJsonObject>
#include <QWidget>

#include <functional>

// 整窗外壳：顶栏、左栏两形态、页签页面都由 WebView2 画（Qt 混渲染时 backing
// store 发虚，用户实测），Qt 侧继续管窗口、托盘、语音、桥接与原生对话框。
// AGENTRMC_WORKWEB=0 或运行库缺失时主窗回退老 Qt 界面。
//
// 只走动态加载（LoadLibrary + CreateCoreWebView2EnvironmentWithOptions），
// 工程不链接 WebView2Loader.lib；DLL 由 build.bat 铺进 build/Release 随包走。
class WorkWebView : public QWidget {
    Q_OBJECT

public:
    // 整份界面状态（待办队列 + Worker 列表 + 健康行 + 主题 token + 事件流），
    // 一次推给页面。
    using StateProvider = std::function<QString()>;
    // 页面上的一次动作：{"cmd":"open|ack|clear|clearLog", "id":"..."} 原样交回主窗，
    // 由它走既有的 openWorkItem / ack / clearFinished / clearLog，不在页面里另起一套。
    using ActionHandler = std::function<void(const QJsonObject &action)>;

    explicit WorkWebView(QWidget *parent = nullptr);
    ~WorkWebView() override;

    void setSources(StateProvider provider, ActionHandler action);
    // 主题素材目录（themes/）映射成虚拟域名，页面里的背景图走
    // https://agentrmc.theme/<主题>/backgrounds/main.png 读，不用把图 base64 塞进状态。
    void setResourceRoot(const QString &folder);
    void refresh();  // 重新取一次 JSON 并重画
    // 高频小状态（麦克风电平/录音态/按键灯）：只 ExecuteScript 更新对应节点，
    // 不走 refresh 的整页重排，20Hz 也扛得住。
    void pushLive(const QJsonObject &live);
    // 换主题/配色模式只推 theme 对象：整份 state 序列化太贵，换色用不着。
    void pushTheme(const QJsonObject &theme);
    // 聊天页上翻加载的更旧一页：单独推 window.__setChatPage，不重排整份 state。
    void pushChat(const QJsonObject &page);
    // WebView2 的边界是物理像素：跨屏换 DPI 时主窗拿原生消息催一次重喂。
    void syncBounds();
    bool ready() const { return webviewReady(); }

    // loader 在、且这台机器装了 WebView2 运行时？装不上就退回 Qt 页，
    // 不给用户留一块白板。
    static bool runtimeAvailable();

    // 给 COM 回调用（WebView2 的接口类型不能出现在头文件里，所以收 void*）。
    void applyWebviewRaw(void *controller, void *webview);
    void handleAction(const QJsonObject &action);
    void handleNavigationCompleted(bool success);

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void create();
    // 首帧整页 NavigateToString；文档好了以后只 ExecuteScript 喂数据——
    // 每刷一次就重载整页，选中态和滚动都会被冲掉（实测），事件一密还闪。
    void pushState(const QString &stateJson);
    void executeState(const QString &stateJson);
    QString htmlFor(const QString &itemsJson) const;
    bool webviewReady() const;

    struct Private;
    Private *d_ = nullptr;
    StateProvider provider_;
    ActionHandler action_;
    QString resourceRoot_;
    bool creating_ = false;
    bool pendingRefresh_ = false;
    bool navigating_ = false;
    bool documentReady_ = false;
    QString pendingState_;
};
