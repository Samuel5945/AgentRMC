#pragma once

#include <QJsonObject>
#include <QWidget>

#include <functional>

// 原型：把「待我处理」这一页交给 WebView2 渲染，验证 Chromium 的字形合成
// 是不是用户要的那个「清楚」。Qt 侧继续管窗口、托盘、语音、桥接——这一页
// 坏了摘掉就行，不动现有实现。
//
// 只走动态加载（LoadLibrary + CreateCoreWebView2EnvironmentWithOptions），
// 所以工程不链接 WebView2Loader.lib，也不把那枚 DLL 铺进 build/Release
// ——installer.nsi 会把 Release 目录里的多余文件打进安装包。
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
    void syncBounds();  // WebView2 的边界是物理像素，且要等布局出来
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
