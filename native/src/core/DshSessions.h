#pragma once

#include <QHash>
#include <QString>

// DSH 会话记录的只读索引（Magpie 式「补标题与项目」，不写宿主数据库）。
//
// 会话正文在 ~/.dsh/sessions/<项目 slug>/session-<uuid>/session.v4.jsonl.zstd，
// zstd 多帧压缩 native 不读；但宿主同时把每个会话的投影缓存在
// ~/.dsh/storages/session_projcache/sessions/<session-id>.json ——  plain JSON，
// 里面有 AI 标题（rows.title.val）、首条用户正文（rows.titleInput.val.first.text）
// 与工作目录（rows.workingDirectory.val / identity.cwd）。待办侧只读这份投影
// 给卡片补会话身份：同项目并行两条会话时，短 id 换成看得懂的标题。
//
// 只从主线程调用（UI 刷新与 AgentBridge 都在主线程），缓存不加锁。
class DshSessions {
public:
    struct Info {
        bool valid = false;  // 投影文件存在且解出了 record
        QString title;       // AI 标题；没有就退首条用户正文（压空白、截断）
        QString cwd;         // 工作目录；可能为空
    };

    // 会话 id 必须是 session-<uuid> 形态（与派发哨兵同口径）；文件缺失、
    // 解析失败一律 valid=false，绝不抛错拖垮面板刷新。
    static Info find(const QString &sessionId);

    // ~/.dsh；AGENTRMC_DSH_HOME 可覆盖（测试/便携）。
    static QString home();

private:
    struct CacheEntry {
        qint64 mtimeMs = 0;
        qint64 size = -1;
        Info info;
    };
    // 按文件 mtime+size 缓存：面板 5 秒一刷，不能每次都读盘解 JSON。
    static QHash<QString, CacheEntry> cache_;
};