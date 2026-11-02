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
    check("a different tool_use_id creates a new item",
          second.id != first.id && store.items().size() == 2 &&
              store.attentionCount() == 2);

    check("stop resolves the session's pending needs-me items",
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
          reloaded.load() && reloaded.items().size() == 3 &&
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

    std::printf(failures == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
