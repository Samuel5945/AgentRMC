#pragma once

#include <QJsonArray>
#include <QString>

// 聊天页记录读取器（只读、分页）：把待办绑定会话的对话拉出来，电脑外壳与手机页
// 共用。交互口径（用户 2026-11-13 定）：打开直接显示最新记录，旧记录往上翻时
// 按页加载——所以读取是「从末尾数 offset 条、取 limit 条」的游标分页。
// 各宿主会话库位置与记法来自 mission-control-plan 的实测：
//   WorkBuddy 系 / Qoder 系：~/.workbuddy(-ai)/.qoder(-cn)/projects/<slug>/<id>.jsonl
//   ZCode：~/.zcode/cli/db/db.sqlite 的 message+part 两表
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
        QJsonArray messages; // 按时间正序的一页 [{id, role, text, at}]
        int total = 0;       // 可用总条数（SQLite 宿主封顶 500）
        bool hasMore = false; // 还有更旧的页
    };

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