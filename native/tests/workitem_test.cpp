#include "core/WorkItems.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(const char *name, bool ok) {
    if (ok) {
        std::printf("PASS %s\n", name);
    } else {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::printf("FAIL temp dir\n");
        return 1;
    }
    const QString path = dir.path() + "/work-items.json";
    WorkItemStore store(path);

    const WorkItem first = store.upsert(
        "hook", "WorkBuddyAI.exe", "s1", "D:/repo", "permission",
        "WorkBuddy AI", "等你确认 Bash", "WorkBuddyAI.exe", "tool-1");
    check("permission creates a needs-me item",
          first.stage == "needs-me" && store.attentionCount() == 1 &&
              !first.id.isEmpty());

    const WorkItem again = store.upsert(
        "hook", "WorkBuddyAI.exe", "s1", "D:/repo", "permission",
        "WorkBuddy AI", "等你确认 Bash", "WorkBuddyAI.exe", "tool-1");
    check("same tool_use_id updates instead of duplicating",
          again.id == first.id && store.items().size() == 1);

    const WorkItem second = store.upsert(
        "hook", "WorkBuddyAI.exe", "s1", "D:/repo", "permission",
        "WorkBuddy AI", "等你确认 Write", "WorkBuddyAI.exe", "tool-2");
    check("a second permission merges into the session's live item",
          second.id == first.id && store.items().size() == 1 &&
              second.message == "等你确认 Write" &&
              store.attentionCount() == 1);

    check("resolveSession closes the live needs-me item",
          store.resolveSession("s1") && store.attentionCount() == 0);

    const WorkItem stop = store.upsert(
        "hook", "WorkBuddyAI.exe", "s1", "D:/repo", "stop", "WorkBuddy AI",
        "WorkBuddy AI：改好了", "WorkBuddyAI.exe", "");
    check("stop creates a review item", stop.stage == "review" &&
                                           store.attentionCount() == 1);

    check("ack removes it from attention",
          store.ack(stop.id) && store.attentionCount() == 0);

    WorkItemStore reloaded(path);
    check("reload keeps items and state",
          reloaded.load() && reloaded.items().size() == 2 &&
              reloaded.attentionCount() == 0);

    check("clearFinished removes done items",
          reloaded.clearFinished() && reloaded.items().isEmpty());

    WorkItemStore dedupe(dir.path() + "/dedupe.json");
    const WorkItem d1 = dedupe.upsert("hook", "WorkBuddyAI.exe", "s2", "",
                                      "permission", "WorkBuddy AI", "确认 A",
                                      "WorkBuddyAI.exe");
    const WorkItem d2 = dedupe.upsert("hook", "WorkBuddyAI.exe", "s2", "",
                                      "permission", "WorkBuddy AI", "确认 A",
                                      "WorkBuddyAI.exe");
    check("no event id dedupes a quick retry",
          d1.id == d2.id && dedupe.items().size() == 1);

    WorkItemStore lifecycle(dir.path() + "/lifecycle.json");
    const WorkItem voice = lifecycle.upsert(
        "voice", "WorkBuddyAI.exe", "", "", "voice", "WorkBuddy AI",
        "帮我改一下这个报错", "WorkBuddyAI.exe");
    check("voice handoff creates a working item",
          voice.stage == "working" && lifecycle.attentionCount() == 0);

    const WorkItem perm = lifecycle.upsert(
        "hook", "WorkBuddyAI.exe", "s9", "D:/repo", "permission",
        "WorkBuddy AI", "等你确认 Bash", "WorkBuddyAI.exe", "p1");
    check("permission merges into the voice working item",
          perm.id == voice.id && perm.stage == "needs-me" &&
              perm.sessionId == "s9" && lifecycle.attentionCount() == 1);

    const WorkItem done = lifecycle.upsert(
        "hook", "WorkBuddyAI.exe", "s9", "D:/repo", "stop", "WorkBuddy AI",
        "WorkBuddy AI：改好了", "WorkBuddyAI.exe", "");
    check("stop merges the same item into review",
          done.id == voice.id && done.stage == "review" &&
              lifecycle.attentionCount() == 1);

    WorkItemStore progress(dir.path() + "/progress.json");
    const WorkItem p1 = progress.upsert(
        "hook", "WorkBuddyAI.exe", "s10", "D:/repo", "progress",
        "WorkBuddy AI", "正在跑测试", "WorkBuddyAI.exe", "");
    const WorkItem p2 = progress.upsert(
        "hook", "WorkBuddyAI.exe", "s10", "D:/repo", "progress",
        "WorkBuddy AI", "正在打包", "WorkBuddyAI.exe", "");
    check("progress updates one working item per session",
          p1.id == p2.id && p2.stage == "working" &&
              p2.message == "正在打包" && progress.items().size() == 1);

    const WorkItem pstop = progress.upsert(
        "hook", "WorkBuddyAI.exe", "s10", "D:/repo", "stop", "WorkBuddy AI",
        "WorkBuddy AI：完成", "WorkBuddyAI.exe", "");
    check("stop closes the progress item as review",
          pstop.id == p1.id && pstop.stage == "review" &&
              progress.attentionCount() == 1);

    WorkItemStore reviews(dir.path() + "/reviews.json");
    const WorkItem oldReview = reviews.upsert(
        "hook", "WorkBuddyAI.exe", "s11", "D:/repo", "stop", "WorkBuddy AI",
        "old review", "WorkBuddyAI.exe", "");
    const WorkItem next = reviews.upsert(
        "hook", "WorkBuddyAI.exe", "s11", "D:/repo", "progress",
        "WorkBuddy AI", "new turn", "WorkBuddyAI.exe", "");
    check("new activity archives the session's old review",
          oldReview.stage == "review" && next.stage == "working" &&
              reviews.attentionCount() == 0);

    const WorkItem dupStop1 = reviews.upsert(
        "hook", "WorkBuddyAI.exe", "s12", "D:/repo", "stop", "WorkBuddy AI",
        "done once", "WorkBuddyAI.exe", "");
    const WorkItem dupStop2 = reviews.upsert(
        "hook", "WorkBuddyAI.exe", "s12", "D:/repo", "stop", "WorkBuddy AI",
        "done once", "WorkBuddyAI.exe", "");
    check("duplicate stop keeps one review item",
          dupStop1.id == dupStop2.id && dupStop2.stage == "review" &&
              reviews.items().size() == 3);

    check("turn-start archives the session's review",
          reviews.resolveReviews("s12") && reviews.attentionCount() == 0);

    // MCP 的 pet_work_update：stage 映射到既有 kind，走同一条合并逻辑。
    WorkItemStore reported(dir.path() + "/reported.json");
    const WorkItem rProgress = reported.report(
        "WorkBuddyAI.exe", "WorkBuddyAI.exe", "m1", "D:/repo", "progress",
        "WorkBuddy AI", "在改 MainWindow");
    check("report progress lands as working",
          rProgress.stage == "working" && rProgress.source == "mcp" &&
              reported.attentionCount() == 0);

    const WorkItem rBlocked = reported.report(
        "WorkBuddyAI.exe", "WorkBuddyAI.exe", "m1", "D:/repo", "blocked",
        "WorkBuddy AI", "缺一个 token");
    check("report blocked turns the same item into needs-me",
          rBlocked.id == rProgress.id && rBlocked.stage == "needs-me" &&
              reported.attentionCount() == 1);

    const WorkItem rReady = reported.report(
        "WorkBuddyAI.exe", "WorkBuddyAI.exe", "m1", "D:/repo", "ready",
        "WorkBuddy AI", "改完了");
    check("report ready turns it into review",
          rReady.id == rProgress.id && rReady.stage == "review" &&
              reported.items().size() == 1);

    // voice 回执与 stop 的归属（真机 16:29 错标复核）：
    // 1) session 无活跃卡 → stop 吸收进 voice 卡（续聊/新任务首轮）；
    // 2) session 已有活跃卡 → stop 归该 session 的卡，voice 回执结案不吸收。
    WorkItemStore own(dir.path() + "/voice-owned.json");
    const WorkItem v1 = own.upsert("voice", "WorkBuddy.exe", "", "", "voice",
                                   "WorkBuddy", "派活句一", "WorkBuddy.exe");
    const WorkItem s1 = own.upsert("hook", "WorkBuddy.exe", "sA", "D:/repo",
                                   "stop", "WorkBuddy", "收到",
                                   "WorkBuddy.exe", "");
    check("untracked session stop merges into the voice receipt",
          s1.id == v1.id && s1.stage == "review" && s1.sessionId == "sA");
    check("ack the merged review", own.ack(s1.id));

    const WorkItem v2 = own.upsert("voice", "WorkBuddy.exe", "", "", "voice",
                                   "WorkBuddy", "派活句二", "WorkBuddy.exe");
    const WorkItem s2 = own.upsert("hook", "WorkBuddy.exe", "sA", "D:/repo",
                                   "stop", "WorkBuddy", "收到二",
                                   "WorkBuddy.exe", "");
    check("acked session is untracked again: stop merges into new receipt",
          s2.id == v2.id && s2.stage == "review");

    const WorkItem foreign = own.upsert("hook", "WorkBuddy.exe", "sB",
                                        "D:/repo", "stop", "WorkBuddy",
                                        "用户自己的会话跑完了",
                                        "WorkBuddy.exe", "");
    check("foreign session gets its own review",
          foreign.stage == "review" && foreign.id != v2.id);
    const WorkItem v3 = own.upsert("voice", "WorkBuddy.exe", "", "", "voice",
                                   "WorkBuddy", "派活句三", "WorkBuddy.exe");
    const WorkItem sB2 = own.upsert("hook", "WorkBuddy.exe", "sB", "D:/repo",
                                    "stop", "WorkBuddy", "用户会话又跑完了",
                                    "WorkBuddy.exe", "");
    bool v3done = false;
    QString v3msg;
    for (const WorkItem &item : own.items()) {
        if (item.id == v3.id) {
            v3done = item.done;
            v3msg = item.message;
        }
    }
    check("tracked session stop does not relabel the voice receipt",
          v3done && v3msg == QStringLiteral("派活句三") &&
              sB2.sessionId == "sB" && sB2.id != v3.id);

    const WorkItem rBogus = reported.report(
        "WorkBuddyAI.exe", "", "m1", "D:/repo", "done-ish", "WorkBuddy AI", "");
    check("unknown stage is rejected instead of silently accepted",
          rBogus.id.isEmpty() && reported.items().size() == 1);

    // 会话身份优先于宿主身份：同一个宿主并行开两个会话时不许串单。
    WorkItemStore parallel(dir.path() + "/parallel.json");
    const WorkItem pa = parallel.upsert("hook", "Qoder.exe", "qa", "D:/repo",
                                        "progress", "Qoder", "在改 WorkItems",
                                        "Qoder.exe", "");
    const WorkItem pb = parallel.upsert("hook", "Qoder.exe", "qb", "D:/repo",
                                        "permission", "Qoder", "等你确认 Bash",
                                        "Qoder.exe", "t1");
    check("two sessions of one host keep separate items",
          pa.id != pb.id && parallel.items().size() == 2);
    const WorkItem pa2 = parallel.upsert("hook", "Qoder.exe", "qa", "D:/repo",
                                         "stop", "Qoder", "改完了",
                                         "Qoder.exe", "");
    bool qbIntact = false;
    for (const WorkItem &item : parallel.items()) {
        if (item.sessionId == "qb" && item.stage == "needs-me" &&
            item.message == QStringLiteral("等你确认 Bash")) {
            qbIntact = true;
        }
    }
    check("a session's stop updates its own card, not the other one",
          pa2.id == pa.id && pa2.stage == "review" && qbIntact &&
              parallel.items().size() == 2);

    // 不带会话身份的上报（老宿主、MCP 拿不到 session）：只有一个候选才认领。
    WorkItemStore solo(dir.path() + "/solo.json");
    const WorkItem bound1 = solo.upsert("hook", "Qoder.exe", "q1", "D:/repo",
                                        "progress", "Qoder", "在改桥接",
                                        "Qoder.exe", "");
    const WorkItem adopted = solo.report("Qoder.exe", "Qoder.exe", "", "D:/repo",
                                         "blocked", "Qoder", "缺一个 token");
    check("session-less report updates the only live card of that host",
          adopted.id == bound1.id && adopted.stage == "needs-me" &&
              solo.items().size() == 1);
    const WorkItem bound2 = solo.upsert("hook", "Qoder.exe", "q2", "D:/repo",
                                        "progress", "Qoder", "另一个会话在跑",
                                        "Qoder.exe", "");
    const WorkItem orphan = solo.report("Qoder.exe", "Qoder.exe", "", "D:/repo",
                                        "progress", "Qoder", "说不清是谁的进度");
    check("two live sessions leave the unattributed report unbound",
          orphan.id != bound1.id && orphan.id != bound2.id &&
              orphan.sessionId.isEmpty() && orphan.stage == "working" &&
              solo.items().size() == 3);

    WorkItemStore projects(dir.path() + "/projects.json");
    const WorkItem other = projects.upsert(
        "voice", "Qoder.exe", "", "D:/other", "voice", "Qoder",
        "另一个仓库的派活", "Qoder.exe");
    const WorkItem here = projects.report("Qoder.exe", "Qoder.exe", "",
                                          "D:/repo", "progress", "Qoder",
                                          "这个仓库的进度");
    check("a different project is not the same work item",
          here.id != other.id && projects.items().size() == 2);

    // 认领之后还要留得住判据：不带项目的上报不许把卡片的项目抹成空。
    WorkItemStore keep(dir.path() + "/keep-project.json");
    const WorkItem k1 = keep.upsert("hook", "Qoder.exe", "k1", "D:/repo",
                                    "progress", "Qoder", "在改桥接",
                                    "Qoder.exe", "");
    const WorkItem k2 = keep.report("Qoder.exe", "Qoder.exe", "", "", "progress",
                                    "Qoder", "没带项目的进度");
    check("a report without a project keeps the card's project",
          k2.id == k1.id && k2.project == "D:/repo" && keep.items().size() == 1);

    // 列表顺序与「等了多久」：一条新进度不该把等人的老账挤下去。
    {
        const QString fixture = dir.path() + "/order.json";
        QFile file(fixture);
        if (!file.open(QIODevice::WriteOnly)) {
            check("order fixture writable", false);
        } else {
            static const char *kJson = R"JSON({
  "version": 1,
  "items": [
    {"id":"A","source":"hook","agent_exe":"Qoder.exe","session_id":"sa","project":"D:/repo","kind":"permission","stage":"needs-me","title":"Qoder","message":"msg A","raise":"Qoder.exe","dedupe_key":"ka","created_at":"2026-11-06T09:00:00.000Z","updated_at":"2026-11-06T09:59:55.000Z","attention_since":"2026-11-06T09:30:00.000Z","done":false},
    {"id":"B","source":"hook","agent_exe":"Qoder.exe","session_id":"sb","project":"D:/repo","kind":"question","stage":"needs-me","title":"Qoder","message":"msg B","raise":"Qoder.exe","dedupe_key":"kb","created_at":"2026-11-06T06:00:00.000Z","updated_at":"2026-11-06T09:50:00.000Z","attention_since":"2026-11-06T07:00:00.000Z","done":false},
    {"id":"C","source":"hook","agent_exe":"ZCode.exe","session_id":"sc","project":"D:/repo","kind":"stop","stage":"review","title":"ZCode","message":"msg C","raise":"ZCode.exe","dedupe_key":"kc","created_at":"2026-11-06T09:55:00.000Z","updated_at":"2026-11-06T09:59:00.000Z","attention_since":"2026-11-06T09:59:00.000Z","done":false},
    {"id":"D","source":"hook","agent_exe":"Qoder.exe","session_id":"sd","project":"D:/repo","kind":"progress","stage":"working","title":"Qoder","message":"msg D","raise":"Qoder.exe","dedupe_key":"kd","created_at":"2026-11-06T08:00:00.000Z","updated_at":"2026-11-06T09:00:00.000Z","attention_since":"","done":false},
    {"id":"E","source":"hook","agent_exe":"Qoder.exe","session_id":"se","project":"D:/repo","kind":"progress","stage":"working","title":"Qoder","message":"msg E","raise":"Qoder.exe","dedupe_key":"ke","created_at":"2026-11-06T09:50:00.000Z","updated_at":"2026-11-06T09:59:50.000Z","attention_since":"","done":false},
    {"id":"F","source":"hook","agent_exe":"Qoder.exe","session_id":"sf","project":"D:/repo","kind":"permission","stage":"done","title":"Qoder","message":"msg F","raise":"Qoder.exe","dedupe_key":"kf","created_at":"2026-11-06T05:00:00.000Z","updated_at":"2026-11-06T05:30:00.000Z","attention_since":"","done":true}
  ]
})JSON";
            file.write(kJson);
            file.close();
            WorkItemStore ordered(fixture);
            const bool loaded = ordered.load();
            const QDateTime now = QDateTime::fromString(
                QStringLiteral("2026-11-06T10:00:00.000Z"), Qt::ISODateWithMs);
            const QVector<WorkItem> view = ordered.displayOrder();
            QStringList ids;
            for (const WorkItem &item : view) {
                ids << item.id;
            }
            const bool shaped = loaded && now.isValid() && view.size() == 5 &&
                                ids == QStringList{"B", "A", "C", "E", "D"};
            check("display order skips closed items and queues by wait start",
                  shaped);
            // 后面三条按 id 取，不靠位置：顺序一错就把它们一起跳过的话，
            // 「没报错」就只是「没跑到」。
            const auto byId = [&view](const char *id) {
                for (const WorkItem &item : view) {
                    if (item.id == QLatin1String(id)) {
                        return item;
                    }
                }
                return WorkItem{};
            };
            const WorkItem a = byId("A");
            const WorkItem d = byId("D");
            const WorkItem e = byId("E");
            check("waited time reads from the attention clock, not the last event",
                  !a.id.isEmpty() && a.waitedSecs(now) == 1800 &&
                      !e.id.isEmpty() && e.waitedSecs(now) == -1);
            check("a working card silent for 30+ minutes is stale",
                  !d.id.isEmpty() && d.staleWorking(now) &&
                      !e.staleWorking(now) && !a.staleWorking(now));
            // JSON 队列要和面板同序，已结案的不丢、排在后面。
            const QJsonArray api = QJsonDocument::fromJson(ordered.toJson().toUtf8())
                                       .object()
                                       .value(QStringLiteral("items"))
                                       .toArray();
            QStringList apiIds;
            for (const QJsonValue &value : api) {
                apiIds << value.toObject().value(QStringLiteral("id")).toString();
            }
            check("the JSON queue reads in the panel's order and keeps history",
                  apiIds == QStringList{"B", "A", "C", "E", "D", "F"});
        }
    }

    // 同一会话只留一张活卡：状态变了要把旧账翻篇，包括历史里已经攒下的重复卡。
    {
        const QString dupFile = dir.path() + "/dup-session.json";
        QFile file(dupFile);
        if (!file.open(QIODevice::WriteOnly)) {
            check("dup fixture writable", false);
        } else {
            // 时间相对当前生成：合并窗口只有 30 分钟，写死日期会在第二天
            // 变成「两张都太旧」而误报（2026-11-07 实测）。
            const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
            const QString fresh = nowUtc.addSecs(-60).toString(Qt::ISODateWithMs);
            const QString older = nowUtc.addSecs(-3600).toString(Qt::ISODateWithMs);
            static const char *kDupTmpl = R"JSON({"version":1,"items":[
{"id":"N","source":"hook","agent_exe":"Qoder.exe","session_id":"dup","project":"D:/repo","kind":"progress","stage":"needs-me","title":"Qoder","message":"新的那条","raise":"Qoder.exe","dedupe_key":"kn","created_at":"__FRESH__","updated_at":"__FRESH__","attention_since":"__FRESH__","done":false},
{"id":"O","source":"hook","agent_exe":"Qoder.exe","session_id":"dup","project":"D:/repo","kind":"permission","stage":"needs-me","title":"Qoder","message":"老的那条","raise":"Qoder.exe","dedupe_key":"ko","created_at":"__OLDER__","updated_at":"__OLDER__","attention_since":"__OLDER__","done":false}
]})JSON";
            QString kDup = QString::fromUtf8(kDupTmpl);
            kDup.replace(QStringLiteral("__FRESH__"), fresh);
            kDup.replace(QStringLiteral("__OLDER__"), older);
            file.write(kDup.toUtf8());
            file.close();
            WorkItemStore dup(dupFile);
            const bool loaded = dup.load();
            const WorkItem refreshed = dup.upsert(
                "hook", "Qoder.exe", "dup", "D:/repo", "progress", "Qoder",
                "最新状态", "Qoder.exe", "");
            QStringList openIds;
            for (const WorkItem &item : dup.displayOrder()) {
                openIds << item.id;
            }
            check("one session keeps exactly one open card, the latest state",
                  loaded && refreshed.id == QStringLiteral("N") &&
                      refreshed.message == QStringLiteral("最新状态") &&
                      openIds == QStringList{"N"} && dup.openCount() == 1);

            // 新建那张卡也要守同一条不变式：同会话还挂着一条不在合并窗口里的
            // 旧状态（info），新状态上来要把它翻篇。
            const QString infoFile = dir.path() + "/dup-info.json";
            QFile infoFileHandle(infoFile);
            if (!infoFileHandle.open(QIODevice::WriteOnly)) {
                check("info fixture writable", false);
            } else {
                static const char *kInfo = R"JSON({"version":1,"items":[
{"id":"I","source":"mcp","agent_exe":"Qoder.exe","session_id":"inf1","project":"D:/repo","kind":"message","stage":"info","title":"Qoder","message":"一条旧提示","raise":"Qoder.exe","dedupe_key":"ki","created_at":"2026-11-06T09:50:00.000Z","updated_at":"2026-11-06T09:50:00.000Z","attention_since":"","done":false}
]})JSON";
                infoFileHandle.write(kInfo);
                infoFileHandle.close();
                WorkItemStore infoStore(infoFile);
                const bool infoLoaded = infoStore.load();
                const WorkItem infoFresh = infoStore.upsert(
                    "hook", "Qoder.exe", "inf1", "D:/repo", "permission",
                    "Qoder", "等你确认 Write", "Qoder.exe", "t-1");
                QStringList infoOpen;
                for (const WorkItem &item : infoStore.displayOrder()) {
                    infoOpen << item.id;
                }
                check("a new card for the same session closes the stale state",
                      infoLoaded &&
                          infoFresh.stage == QStringLiteral("needs-me") &&
                          infoOpen.size() == 1 && !infoOpen.contains("I") &&
                          infoStore.openCount() == 1);
            }
        }
    }

    {
        // 2026-11-07：WorkBuddy 内嵌 Qoder CN SDK 曾被 host.js 认成 Qoder CN；
        // 判据修好后同一 session 再报，活卡必须跟着改宿主，不能留在错名下。
        WorkItemStore fix(dir.path() + "/reattribute.json");
        const WorkItem wrong = fix.upsert(
            "hook", "Qoder CN.exe", "wb1", "D:/repo", "progress",
            "Qoder CN", "在跑", "Qoder CN.exe", "");
        const WorkItem right = fix.upsert(
            "hook", "WorkBuddy.exe", "wb1", "D:/repo", "stop",
            "WorkBuddy", "做完了", "WorkBuddy.exe", "");
        check("same session re-reported by another host corrects agent_exe",
              right.id == wrong.id &&
                  right.agentExe == QStringLiteral("WorkBuddy.exe") &&
                  fix.items().size() == 1);
    }

    {
        // 2026-11-08 审查复现：派发轮晚到的 turnFinished 若走 upsert(progress)，
        // 会把 hook 已经推到「待验收」的卡降回进行中（注意力列表里消失）。
        // 晚到回复只能补正文，档位不动。
        WorkItemStore late(dir.path() + "/late-reply.json");
        const WorkItem review = late.upsert("hook", "ZCode.exe", "zlate", "D:/repo",
                                            "stop", "ZCode", "做完了", "ZCode.exe");
        const bool patched = late.updateLiveMessage("zlate", "本轮正文：收到");
        check("late dispatch reply patches the review card without demoting it",
              patched && late.items().size() == 1 &&
                  late.items().first().stage == QStringLiteral("review") &&
                  late.items().first().message == QStringLiteral("本轮正文：收到") &&
                  late.attentionCount() == 1);
        check("late dispatch reply for an unknown session is a no-op",
              !late.updateLiveMessage("nobody", "x"));
    }

    std::printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
