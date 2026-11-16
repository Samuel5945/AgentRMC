#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// 已归档会话的宿主级删除。
//
// 安全口径（用户 2026-11-16 拍板「同步删除宿主会话」）：
//   - 只接受 SessionTranscript 当前判定为 archived 的会话，活跃项目一律拒绝；
//   - 先完整复制到 AgentRMC 数据目录 session-trash/<时间戳>/，复制验证后才删原件；
//   - 文件型宿主删 jsonl + 同 id 的 sidecar 目录；DSH 删会话目录 + 投影缓存；
//   - ZCode / OpenSquilla 没有稳定的会话级物理删除接口，明确不支持，不乱删库。
class SessionCleaner {
public:
    // 目前支持 WorkBuddy 系 / Qoder 系（jsonl）与 DSH（会话目录）。
    static bool supportsHost(const QString &agentExe);

    // items: [{exe,session,project,title}]。返回：
    //   {ok, deleted, failed:[{exe,session,error}], canceled, backup}
    // homeOverride/trashOverride 仅测试用；生产传空。
    static QJsonObject removeArchived(const QJsonArray &items, QString *error,
                                      const QString &homeOverride = QString(),
                                      const QString &trashOverride = QString());
};
