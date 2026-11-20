#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// 会话的宿主级删除（回收站）。2026-11-20 对齐 Magpie（yetone/magpie
// internal/sessions/manage.go）的「回收站而非即删」口径：
//   - 删除 = 整条目**移动**进本地回收站（session-trash/entries/<宿主>/<时间>-<id>/），
//     宿主侧条目随即消失；不再是「复制一份再删原件」——manifest 随移动增量落盘，
//     中途失败把已移动的搬回去，会话保持完整（旧实现有「备份成功、原件没删净」
//     的两不脏状态）；
//   - 回收站可列（listTrash）、可恢复（restore：任一原位已被占用就整体拒绝，
//     绝不半恢复）、可彻底删（purge / purgeAll）；
//   - 永不自动过期、自动清空：Magpie 原则「只有人开口才删」。
//   - 任意会话都能删，不要求归档（对齐 Magpie：Delete 只拦非文件型宿主、
//     只读行与活跃窗口；进了回收站全能恢复）；会话必须真在该宿主会话库
//     里；任一文件 60 秒内有过写入视为活跃，拒绝动手（防止宿主还在写）；
//   - 会话 id 与回收站 key 都走字符白名单，防路径穿越；
//   - DSH 删会话目录 + 投影缓存；ZCode / OpenSquilla 没有稳定的会话级物理
//     删除接口，明确拒绝，不乱删库。
class SessionCleaner {
public:
    // 目前支持 WorkBuddy 系 / Qoder 系（jsonl + sidecar）与 DSH（会话目录）。
    static bool supportsHost(const QString &agentExe);

    // 删除入口（页面契约不变）。items: [{exe,session,project,title}]。返回：
    //   {ok, deleted, failed:[{exe,session,error}], entries:[key...], backup}
    // homeOverride/trashOverride 仅测试用；生产传空。
    static QJsonObject removeSessions(const QJsonArray &items, QString *error,
                                      const QString &homeOverride = QString(),
                                      const QString &trashOverride = QString());

    // 回收站列表（最新在前）：[{key,exe,session,title,project,deleted,size,damaged}]。
    static QJsonArray listTrash(const QString &trashOverride = QString());

    // 恢复：先全量检查落点未被占用才动手，失败回滚，绝不半恢复。
    // 返回 {ok, exe, session, title}，拒绝原因写 *error。
    static QJsonObject restore(const QString &key, QString *error,
                               const QString &homeOverride = QString(),
                               const QString &trashOverride = QString());

    // 彻底删除单条 / 清空全部。只做用户明确点名的，永不自动跑。
    static QJsonObject purge(const QString &key, QString *error,
                             const QString &trashOverride = QString());
    static QJsonObject purgeAll(QString *error,
                                const QString &trashOverride = QString());
};
