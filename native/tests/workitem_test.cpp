#include "core/WorkItems.h"

#include <QCoreApplication>
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

    std::printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
