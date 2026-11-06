#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QString>
#include <QVector>

// 一件需要跟踪的宿主 Agent 工作。不是 Agent 运行时，只是桌宠侧的
// 「谁在干什么、卡在哪、是否需要人」索引。
struct WorkItem {
    QString id;
    QString source;      // hook / mcp / voice
    QString agentExe;    // WorkBuddyAI.exe ...
    QString sessionId;   // 宿主会话 id；老宿主可能没有
    QString project;     // 工作目录；没有就空
    QString kind;        // permission / question / stop / progress
    QString stage;       // needs-me / review / working / done
    QString title;       // 气泡标题（宿主名）
    QString message;
    QString raiseExe;
    QString dedupeKey;
    QDateTime createdAt;
    QDateTime updatedAt;
    QDateTime attentionSince;
    bool done = false;

    // 从进入「要人」那一刻起等了多久（秒）；不在注意力档返回 -1。
    // 排序和「等了多久」都要看这个，不能看 updatedAt——进度一直在动，
    // 人却还没被叫回来。
    qint64 waitedSecs(const QDateTime &now) const;
    // 挂着「进行中」但很久没有任何动静：宿主可能早就悄悄结束或被关了，
    // 继续显示成「在跑」是把未知说成已知。
    bool staleWorking(const QDateTime &now) const;
};

// JSON 文件持久化 + 去重。先做 MVP：一个文件、最多 200 条，够桌面单用户用；
// 以后要换 SQLite 时 UI/桥接接口不用变。
class WorkItemStore {
public:
    explicit WorkItemStore(QString path);

    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr) const;

    // 同一 session + kind（有 tool_use_id 时再加它）在 8 秒内视为同一条，
    // 避免 hook 重投/重复触发把列表刷成两份。stop 会顺手结束同 session 的
    // 旧 needs-me 项，避免授权提醒永远挂着。
    //
    // 认领老工单时，会话身份优先于宿主身份：来件带 session_id 就只认同一个
    // session；不带才退回按宿主＋项目认，且同宿主只有一个候选时才认领——
    // 并行开两个会话时，「按宿主名猜」会把 A 的进度写到 B 的卡上。
    WorkItem upsert(const QString &source, const QString &agentExe,
                    const QString &sessionId, const QString &project,
                    const QString &kind, const QString &title,
                    const QString &message, const QString &raiseExe,
                    const QString &eventId = QString());

    const QVector<WorkItem> &items() const { return items_; }
    int attentionCount() const;
    int openCount() const;

    // 列表顺序：等你处理 > 待验收 > 进行中；同一档里「先开始等人的」在前，
    // 这样一条进度不会把整个列表洗一遍牌。只回未结案的。
    QVector<WorkItem> displayOrder() const;

    // 宿主 Agent 主动汇报（MCP 工具 pet_work_update）：
    //   progress → working / blocked → needs-me / ready → review。
    // 复用 upsert 的去重与「同 session 生命周期合并」，不另起一套状态机。
    // stage 认不出来时返回 id 为空的 WorkItem，调用方据此回 400。
    static QString kindForReport(const QString &stage);
    WorkItem report(const QString &agentExe, const QString &raiseExe,
                    const QString &sessionId, const QString &project,
                    const QString &stage, const QString &title,
                    const QString &message);

    bool ack(const QString &id);
    bool resolveSession(const QString &sessionId,
                        const QString &keepId = QString(),
                        bool includeReview = false);
    bool resolveReviews(const QString &sessionId,
                        const QString &keepId = QString());
    bool clearFinished();

    QString toJson() const;
    QString path() const { return path_; }

    static QString stageForKind(const QString &kind);

private:
    void prune();
    // 同一会话只留一张活卡：把该 session 其它未结案记录翻篇（不分档位——
    // 一条旧提示、一次验收、一个进度，状态变了就不该并排挂着）。
    bool closeOtherStates(const QString &sessionId, const QString &keepId);
    static QJsonObject toJson(const WorkItem &item);
    static WorkItem fromJson(const QJsonObject &obj);

    QString path_;
    QVector<WorkItem> items_;
};
