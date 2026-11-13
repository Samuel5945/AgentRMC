#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QTcpServer;
class QTcpSocket;

// 局域网手机遥控口（移动端 M1）：监听 0.0.0.0:45916，**默认关闭**，系统页
// 「手机访问」显式打开。与 AgentBridge（127.0.0.1:45915）是双端口双信任区：
// 桥是无鉴权本机 IPC（hooks/插件专用，保持回环不动）；这个口暴露给局域网，
// 除静态页与配对外全路由要设备令牌。方案见 docs/mobile-lan-web-plan.md。
//   GET  /m/            → 手机页（静态 HTML，免鉴权；数据全走下面的路由）
//   POST /m/pair        {"code":"配对码","name":"设备名"} → 换设备令牌（Set-Cookie）
//   GET  /m/state       → 裁剪版外壳状态 JSON（work/health/events/theme/dispatch/meta）
//   POST /m/action      {"cmd":"ack|dispatch|open|raiseZcode|restartZcode|readLast",…}
//                       → 白名单校验后转主窗 handleWebAction（与桌面外壳同一入口，
//                         页面里不另起状态机；做完桌面外壳自动跟着刷新）
//   GET  /m/theme/<rel> → 主题素材（仅 png/jpg/jpeg/webp/gif，禁路径越界）
// 安全：配对码 128-bit 随机、10 分钟有效；设备令牌存 SHA-256 哈希、可吊销；
// 每 IP 配对失败 5 次锁 15 分钟；POST 一律要自定义头（CSRF 闸）；不开 CORS；
// 令牌/配对码不落日志。桥的 agent 侧路由（/notify /command /dispatch/* …）
// 永不出现在这个口上。
class LanWebServer final : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kPort = 45916;

    explicit LanWebServer(QObject *parent = nullptr);

    // 起监听（0.0.0.0）；失败（端口被占等）返回 false。port=0 自动选空闲口（测试用）。
    bool start(quint16 port = kPort);
    void stop();
    bool isRunning() const { return server_ != nullptr; }
    // 实际监听端口（start(0) 时有意义）。
    quint16 port() const;

    // GET /m/state 的应答来源（主窗接 lanShellState）；缺省回 "{}"。
    void setStateProvider(std::function<QString()> provider);
    // GET /m/transcript?id=&offset=&limit= 的应答来源（聊天页分页记录）；
    // 返回空串＝待办不存在，服务端回 404。
    void setTranscriptProvider(
        std::function<QString(const QString &id, int offset, int limit)> provider);
    // POST /m/action 的处理器（白名单已在服务端验过）：
    // 返回 {"ok":bool,"code":200|400|403|409}，code 直接当 HTTP 状态。
    void setActionHandler(
        std::function<QJsonObject(const QJsonObject &)> handler);
    // 主题素材根目录（ThemeManager::themesRoot()）。
    void setThemeRoot(const QString &root);
    // 恢复持久化的设备（含 tokenHash）；变化时发 devicesChanged 给主窗落盘。
    void setDevices(const QJsonArray &devices);
    // 配对码有效期；默认 10 分钟，测试可缩短。
    void setPairingTtlMs(qint64 ms) { pairingTtlMs_ = ms; }

    // 生成新配对码（旧的立即作废），返回码本身；主窗拿去画二维码。
    QString beginPairing();
    void endPairing();
    // {active, code, expiresAt}——code 只给桌面 UI 显示，不下发手机。
    QJsonObject pairingInfo() const;
    // 给 UI 的设备列表（不含哈希）：[{id,name,pairedAt,lastSeen}]。
    QJsonArray devices() const;
    bool revokeDevice(const QString &id);

    // 本机局域网 IPv4 → http://<ip>:<port>/m/（192.168 段优先，多网卡都列出来）。
    static QStringList lanUrls(quint16 port);
    // 手机可发的动作白名单（桌面专用命令如 lanToggle/sysUpgrade 永不放行）。
    static bool isAllowedAction(const QString &cmd);

signals:
    // 全量设备（含 tokenHash），主窗写进 store 持久化。
    void devicesChanged(const QJsonArray &devices);

private:
    void onConnected();
    void onReadable(QTcpSocket *socket);
    void route(QTcpSocket *socket, const QByteArray &method,
               const QByteArray &path, const QByteArray &query,
               const QList<QByteArray> &headerLines, const QByteArray &body);
    void routePair(QTcpSocket *socket, const QList<QByteArray> &headerLines,
                   const QByteArray &body);
    void routeTheme(QTcpSocket *socket, const QByteArray &path);
    // 返回设备下标；-1 = 未鉴权。顺带更新 lastSeen（落盘节流 30s）。
    int authDevice(const QList<QByteArray> &headerLines);
    void finish(QTcpSocket *socket, const char *status,
                const QByteArray &body = QByteArray(),
                const char *contentType = nullptr,
                const QByteArray &extraHeaders = QByteArray(),
                const char *cacheControl = "no-store");
    void dropSocket(QTcpSocket *socket);
    QString mobileHtml() const;

    QTcpServer *server_ = nullptr;
    QHash<QTcpSocket *, QByteArray> buffers_;
    // 超大 body：先排干再回 413。收缓冲里还压着没读的数据就关连接，Windows
    // 会发 RST 而不是 FIN，客户端连已收到的响应都会丢（lanweb_test 实测）。
    QHash<QTcpSocket *, qint64> drain_;
    std::function<QString()> stateProvider_;
    std::function<QString(const QString &, int, int)> transcriptProvider_;
    std::function<QJsonObject(const QJsonObject &)> actionHandler_;
    QString themeRoot_;
    QJsonArray devices_;  // {id,name,tokenHash,pairedAt,lastSeen}
    QString pairingCode_;
    QDateTime pairingExpires_;
    qint64 pairingTtlMs_ = 10 * 60 * 1000;
    struct PairFail {
        int count = 0;
        QDateTime lockedUntil;
    };
    QHash<QString, PairFail> pairFails_;
    QDateTime lastPersist_;
};