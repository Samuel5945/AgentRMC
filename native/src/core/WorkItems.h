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
    WorkItem upsert(const QString &source, const QString &agentExe,
                    const QString &sessionId, const QString &project,
                    const QString &kind, const QString &title,
                    const QString &message, const QString &raiseExe,
                    const QString &eventId = QString());

    const QVector<WorkItem> &items() const { return items_; }
    int attentionCount() const;
    int openCount() const;

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
    static QJsonObject toJson(const WorkItem &item);
    static WorkItem fromJson(const QJsonObject &obj);

    QString path_;
    QVector<WorkItem> items_;
};
