#include "core/AgentWindow.h"
#include "core/HostNewTask.h"

#include <QString>
#include <QStringList>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::fputs("FAIL: ", stderr);
        std::fputs(message, stderr);
        std::fputc(10, stderr);
    }
}

bool hasFail(const HostNewTask::Attempt &a, const QString &needle) {
    return !a.focused && a.fail.contains(needle, Qt::CaseInsensitive);
}

}  // namespace

int main() {
    const QStringList kExpected{QStringLiteral("新建任务")};

    // 1) WorkBuddy 两版走「按名激活」，且名字精确
    check(HostNewTask::controlNames(QStringLiteral("WorkBuddy.exe")) == kExpected,
          "workbuddy classic control name");
    check(HostNewTask::controlNames(QStringLiteral("WorkBuddyAI.exe")) ==
              kExpected,
          "workbuddy ai control name");
    check(HostNewTask::controlNames(QStringLiteral("workbuddyai.exe")) ==
              kExpected,
          "control names are case-insensitive on exe");

    // 2) 这两家绝不能拿 Ctrl+N 当聚焦键：实测那是「新建文件」，会弹文件框
    check(AgentWindow::chatFocusCombo(QStringLiteral("WorkBuddy.exe")).isEmpty(),
          "workbuddy has no chat focus combo");
    check(AgentWindow::chatFocusCombo(QStringLiteral("WorkBuddyAI.exe")).isEmpty(),
          "workbuddy ai has no chat focus combo");

    // 3) Qoder 家族仍然由 ctrl+n 负责，两条路不重叠（否则同一次派活按两遍）
    check(HostNewTask::controlNames(QStringLiteral("Qoder.exe")).isEmpty(),
          "qoder stays on the hotkey path");
    check(HostNewTask::controlNames(QStringLiteral("Qoder CN.exe")).isEmpty(),
          "qoder cn stays on the hotkey path");
    check(AgentWindow::chatFocusCombo(QStringLiteral("Qoder.exe")) ==
              QStringLiteral("ctrl+n"),
          "qoder ctrl+n intact");
    check(AgentWindow::chatFocusCombo(QStringLiteral("Qoder CN.exe")) ==
              QStringLiteral("ctrl+n"),
          "qoder cn ctrl+n intact");

    // 4) 没实测过的宿主一律留空——不把未验证的键位/控件名当默认值
    for (const QString &exe : {QStringLiteral("ZCode.exe"),
                               QStringLiteral("CodeBuddy.exe"),
                               QStringLiteral("OpenSquilla.exe"),
                               QStringLiteral("DeepSeek Harness.exe")}) {
        check(HostNewTask::controlNames(exe).isEmpty(),
              "unmeasured host has no control name");
    }

    // 5) 失败分支必须带判定输入，不能只回一个 false
    check(hasFail(HostNewTask::focusControl(QString(), QStringList(), 100),
                  QStringLiteral("NO-EXE")),
          "empty exe reports NO-EXE");
    const HostNewTask::Attempt noNames = HostNewTask::focusControl(
        QStringLiteral("WorkBuddy.exe"), QStringList(), 100);
    check(hasFail(noNames, QStringLiteral("NO-CONTROL-NAMES")),
          "empty names reports NO-CONTROL-NAMES");
    const HostNewTask::Attempt noWindow =
        HostNewTask::focusControl(QStringLiteral("AgentRMCNoSuchHost.exe"),
                                  kExpected, 100);
    check(hasFail(noWindow, QStringLiteral("NO-WINDOW")),
          "missing window reports NO-WINDOW");
    check(noWindow.fail.contains(QStringLiteral("AgentRMCNoSuchHost.exe")),
          "NO-WINDOW detail carries the exe it looked for");

    // 6) 验收函数在找不到宿主时必须是 false，而不是「焦点在我自己窗口里也算过」
    check(!HostNewTask::focusIsEditable(QStringLiteral("AgentRMCNoSuchHost.exe")),
          "focusIsEditable false without the host window");
    check(AgentWindow::mainWindow(QStringLiteral("AgentRMCNoSuchHost.exe")) ==
              nullptr,
          "mainWindow null for unknown exe");
    check(AgentWindow::mainWindow(QString()) == nullptr,
          "mainWindow null for empty exe");

    // 7) 读回函数在找不到宿主时给安全空读数（验证粘贴靠它判假成功）
    check(HostNewTask::composerValue(QStringLiteral("AgentRMCNoSuchHost.exe"))
              .isEmpty(),
          "composerValue empty without the host window");
    check(HostNewTask::composerValue(QString()).isEmpty(),
          "composerValue empty for empty exe");
    check(!HostNewTask::treeHasText(QStringLiteral("AgentRMCNoSuchHost.exe"),
                                   QStringLiteral("anything")),
          "treeHasText false without the host window");
    check(!HostNewTask::treeHasText(QStringLiteral("WorkBuddy.exe"), QString()),
          "treeHasText false for empty needle");

    if (failures == 0) {
        std::puts("PASS native host new-task: routing / ctrl+n exclusion / "
                  "failure readings / readback safe defaults");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
