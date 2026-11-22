#pragma once

#include <QHash>
#include <QString>

// DSH 会话记录的只读索引（Magpie 式「补标题与项目」，不写宿主数据库）。
//
// 会话正文在 ~/.dsh/sessions/<项目 slug>/session-<uuid>/session.v4.jsonl.zstd，
// zstd 多帧压缩 native 不读；但宿主同时把每个会话的投影缓存在
// ~/.dsh/storages/session_projcache/sessions/<session-id>.json ——  plain JSON，
// 里面有 AI 标题（rows.title.val）、首条用户正文（rows.titleInput.val.first.text）
// 与工作目录（identity.cwd = 原始项目，rows.workingDirectory.val = 当前目录）。
// 待办侧只读这份投影给卡片补会话身份：同项目并行两条会话时，短 id 换成看得懂的标题。
//
// 只从主线程调用（UI 刷新与 AgentBridge 都在主线程），缓存不加锁。
class DshSessions {
public:
    struct Info {
        bool valid = false;  // 投影文件存在且解出了 record
        // 投影文件不存在（宿主删除会话时连投影一起清）。与 valid=false
        // 的区别：坏 JSON / 超大文件 / id 不合法都只是「读不出」，会话可能
        // 还活着；gone 只在确认「这份投影没了」时置位，工单对账据此结案。
        // id 不合法的早退不置位——那是调用方的错，不是会话没了。
        bool gone = false;
        // turnBoundary.openTurnStartSeq 的三态：1=有开着回合 0=回合已关
        // -1=投影里没这份账（老格式/形状变了），不表态。模型出错/用户停止
        // 的收尾宿主不发 turn-stopping，插件那侧没有事件可收——这份投影是
        // 「这轮到底结束了没」的硬账。
        int turnOpen = -1;
        QString title;       // AI 标题；没有就退首条用户正文（压空白、截断）
        // 会话的**原始项目目录**（投影 identity.cwd = 会话 header 的 cwd，也是
        // 宿主工作区登记的那条路径）。这是「这条会话属于哪个项目」的唯一身份，
        // 会话中途 /cd 也不会变。分组、行身份、「在哪儿」一律用它。
        QString cwd;
        // 会话**当前生效的工作目录**（投影 rows.workingDirectory.val）。
        // 用户 /cd 或宿主自动恢复都会改它，所以它可能指向项目里的子目录
        // （2026-11-22 用户截图：Agentrmc 的会话因 cd 进 native\build\Release，
        // 整条被归到「Release」项目组）。只用于「现在在哪个目录干活」这类读数，
        // 绝不能拿来当项目身份。
        QString workingDir;
        // 会话当前模型（投影 rows.modelSelection.val 的 pending ?? lastUsed）。
        // 只是身份提示：聊天页顶栏的真实读数读会话日志（投影有写节流）。
        QString provider;
        QString model;
        QString effort;      // reasoningEffort；空＝跟随适配器默认
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