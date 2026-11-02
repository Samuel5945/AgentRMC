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

    std::printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
