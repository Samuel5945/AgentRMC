#pragma once

#include <QJsonArray>
#include <QString>
#include <QVector>

// 聊天页记录读取器（只读、分页）：把待办绑定会话的对话拉出来，电脑外壳与手机页
// 共用。交互口径（用户 2026-11-13 定）：打开直接显示最新记录，旧记录往上翻时
// 按页加载——所以读取是「从末尾数 offset 条、取 limit 条」的游标分页。
// 各宿主会话库位置与记法来自 mission-control-plan 的实测：
//   WorkBuddy 系 / Qoder 系：~/.workbuddy(-ai)/.qoder(-cn)/projects/<slug>/<id>.jsonl
//   ZCode：~/.zcode/cli/db/db.sqlite 的 message+part 两表；harness 注入
//        （synthetic / visibility=model-only 的 user 行）不进聊天页
//   OpenSquilla：%APPDATA%/@opensquilla/<profile>/opensquilla/state/sessions.db
//   DSH：~/.dsh/sessions/<slug>/session-<uuid>/session.v4.jsonl.zstd，多帧拼接，
//        用 vendored libzstd（native/vendor/zstd）跨帧解压；标题取日志 session/title。
//        用户消息只取 source.kind=="user"（DSH 把时间上下文/运行时快照/skill 目录等
//        系统注入也记成 user/message，须滤掉，否则聊天页冒多余气泡）。
// 只读、带帽（单页 ≤100 条、单条 8000 字、SQLite 全量缓存 ≤500 条），不写宿主库。
// 进程内按库文件 mtime+size 缓存全量解析结果，翻页/轮询不重复解析。
class SessionTranscript {
public:
    struct Result {
        bool supported = false;
        QString error;       // 不支持/读失败的原因（给用户看）
        QString title;       // 会话标题（拿得到就给）
        // 会话的工作目录（jsonl 里的 cwd）。**无工作区的任务**（待办 item.project
        // 为空）靠它才能找到产物目录——否则「产物与文件」永远是空的，
        // agent 在会话里生成的图/文档都下不了（2026-11-20 用户报绯澜 OS8 主题）。
        QString project;
        // 会话当前在用的模型（显示名，如 "DeepSeek-V4.1-Flash" / "GLM-5.3"）。
        // 宿主记在最后一条 assistant 消息的元数据里：jsonl 系看 providerData，
        // ZCode 看 message.data.model。读不到就空——页面显示「跟随会话」。
        QString model;
        QJsonArray messages; // 按时间正序的一页 [{id, role, text, at}]
        int total = 0;       // 可用总条数（SQLite 宿主封顶 500）
        bool hasMore = false; // 还有更旧的页
        // 当前正在跑的那一轮（OpenSquilla agent_tasks.finished_at 为空）。
        // 这两个字段**每次 read 都重查**、不吃全量缓存：长轮要十几分钟才有下一条
        // transcript，缓存住就永远停在打开页面那一刻的秒数（用户 2026-11-20 报
        // 「长时间不显示进度内容」）。runningSinceMs＝0 表示没有在跑的轮。
        qint64 runningSinceMs = 0;
        QString runningText;  // 「本轮已 13 分 41 秒」
    };

    // 手机「派发」页选的会话目录项：只读元数据，不含正文。
    struct SessionInfo {
        QString id;
        QString title;
        QString project;
        QString updatedAt;  // ISO UTC
        // 宿主已移除 / 目录已不存在 / 自动临时工作区：手机端归「已归档」，
        // 不混进当前项目；只是提示性分类，不影响记录读取。
        bool archived = false;
    };

    // 宿主自动生成的临时工作区（WorkBuddy 的日期目录、Qoder Documents 工作区、
    // 远程控制沙箱、OpenSquilla 任务目录、VirtualDisplay）：单任务本来没有真项目，
    // 目录页把它们统一归「未分类任务」，不按目录名（尤其日期名）分组。
    static bool generatedWorkspace(const QString &project);

    // 最近会话目录（每宿主最多 maxSessions 条，按更新时间倒序）。
    // jsonl 宿主按文件 mtime + 首屏元数据，ZCode/OpenSquilla 查只读 SQLite，
    // DSH 读宿主投影缓存。读不出来返回空表，不抛错。
    static QVector<SessionInfo> listSessions(
        const QString &agentExe, int maxSessions = 20,
        const QString &rootOverride = QString());

    // offsetFromEnd＝从最新往旧跳过多少条（0＝最新一页）。
    // rootOverride 非空时用它当会话库根（测试注入临时库）。
    static Result read(const QString &agentExe, const QString &sessionId,
                       int limit = 50, int offsetFromEnd = 0,
                       const QString &rootOverride = QString());

    // jsonl 一行 → role+正文+时间+id（剥包装、保留换行）；不是对话行返回 false。
    static bool messageOf(const QByteArray &line, QString *role, QString *text,
                          QString *at, QString *id = nullptr);

    // 剥 system-reminder / user_query 包装，修剪首尾空白但保留中间换行。
    static QString unwrap(const QString &raw);
};