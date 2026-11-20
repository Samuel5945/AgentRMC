#pragma once

#include <QElapsedTimer>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

// 宿主侧真删队列（2026-11-20，对 Magpie「数据库型宿主不做半吊子文件删」的补位）：
// OpenSquilla 的会话在共享 SQLite 库里，物理删文件会毁库，但宿主网关有官方
// sessions.delete RPC（本机实测：级联干净、resolve 即拒）。通道照搬 DSH 插件
// 派发通道：native 入队 → 桥轮询取件（GET /delete/pending）→ 网关 sessions.delete
// → POST /delete/ack。桥不在（OpenSquilla 没开 / 桥过旧）60s 超时取消，绝不
// 假装删成。删的是宿主真身、不进回收站，所以调用方必须已经过「会话在库 +
// 一分钟无写入 + 人确认」三道闸（MainWindow 的 sessionDelete 分支把关）。
class SessionDeleteQueue final : public QObject {
    Q_OBJECT

public:
    explicit SessionDeleteQueue(QObject *parent = nullptr,
                                qint64 takeTimeoutMs = 60000);

    // 走官方 RPC 真删的宿主（当前只有 OpenSquilla）。
    static bool supportsHost(const QString &agentExe);

    // items: [{session,title}]（先过调用方守卫）。同会话在途不重复入队。
    // 返回接受数；idsOut 按入参顺序给出接受项的 delete_id。
    int enqueue(const QJsonArray &items, QStringList *idsOut = nullptr);

    // 桥取件：一次取走未下发项（标记 handed，等 ack/超时；重连不重发）。
    QJsonArray takePending();

    // 桥回执：{delete_id, ok, error}。id 不认识（超时兜底后晚到）返回 false。
    bool ack(const QString &deleteId, bool ok, const QString &error);

signals:
    // 单条终态：ok=宿主确认删除；失败带原因（含取件超时）。
    void finished(const QString &deleteId, const QString &session,
                  const QString &title, bool ok, const QString &error);

private:
    struct Job {
        QString id;
        QString session;
        QString title;
        QElapsedTimer clock;
        bool handed = false;
    };

    void poll();

    QList<Job> jobs_;
    QTimer *timer_ = nullptr;
    qint64 timeoutMs_ = 60000;
};
