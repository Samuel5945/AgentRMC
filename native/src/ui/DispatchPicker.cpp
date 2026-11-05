#include "DispatchPicker.h"

#include "core/AgentWindow.h"
#include "core/Profiles.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QSignalBlocker>

DispatchPicker::DispatchPicker(ProfileStore *store, QWidget *parent)
    : QWidget(parent), store_(store) {
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    agent_ = new QComboBox;
    agent_->setToolTip(QString::fromUtf8(
        "语音派活交给哪个 Agent\n"
        "选定后说的话直接进它的会话，桌宠不再判断任务/闲聊，也不走自带对话模型\n"
        "自动判断 = 前台优先 → 最近发通知的宿主 → 最近派活的宿主"));
    connect(agent_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        store_->setUiString(QStringLiteral("agent_target"),
                            agent_->itemData(index).toString());
        refreshMode();
    });
    mode_ = new QComboBox;
    mode_->addItem(QString::fromUtf8("续当前会话"), QString());
    mode_->addItem(QString::fromUtf8("下次派活开新任务"), QStringLiteral("new"));
    connect(mode_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        const QString exe = agent_->currentData().toString();
        if (exe.isEmpty()) {
            return;
        }
        store_->setUiString(QStringLiteral("dispatch_mode_") + exe,
                            mode_->itemData(index).toString());
    });
    row->addWidget(agent_, 1);
    row->addWidget(mode_, 1);
    refresh();
}

void DispatchPicker::refresh() {
    const QSignalBlocker block(agent_);
    const QString pinned = store_->uiString(QStringLiteral("agent_target"));
    agent_->clear();
    agent_->addItem(QString::fromUtf8("自动判断"), QString());
    bool anyAlive = false;
    for (const QString &exe : AgentWindow::knownExes()) {
        // 进程在跑就列出；窗口收进托盘的标注「窗口隐藏」——选它语音派活
        // 不会硬注入（没有可交互窗口，粘了也是粘到别人窗口上）。
        if (!AgentWindow::processAlive(exe)) {
            continue;
        }
        anyAlive = true;
        if (AgentWindow::alive(exe)) {
            agent_->addItem(AgentWindow::displayName(exe), exe);
        } else {
            agent_->addItem(
                QString::fromUtf8("%1（窗口隐藏）")
                    .arg(AgentWindow::displayName(exe)),
                exe);
        }
    }
    if (!pinned.isEmpty() && !AgentWindow::processAlive(pinned)) {
        agent_->addItem(
            QString::fromUtf8("%1（未运行）")
                .arg(AgentWindow::displayName(pinned)),
            pinned);
    } else if (!pinned.isEmpty() && !AgentWindow::alive(pinned)) {
        // 钉住的宿主进程在但窗口收起来了：也标出来，别让用户以为能直接派。
        const int existing = agent_->findData(pinned);
        if (existing >= 0) {
            agent_->setItemText(
                existing, QString::fromUtf8("%1（窗口隐藏）")
                              .arg(AgentWindow::displayName(pinned)));
        }
    }
    int index = agent_->findData(pinned);
    if (index < 0) {
        index = 0;
    }
    agent_->setCurrentIndex(index);
    agent_->setToolTip(
        anyAlive || !pinned.isEmpty()
            ? QString::fromUtf8("语音派活交给哪个 Agent\n"
                                "自动判断 = 前台优先 → 最近发通知的宿主")
            : QString::fromUtf8("没检测到在跑的 Agent\n"
                                "语音会走自带 LLM，或提示先打开 Agent"));
    refreshMode();
}

void DispatchPicker::refreshMode() {
    const QSignalBlocker block(mode_);
    const QString exe = agent_->currentData().toString();
    mode_->setEnabled(!exe.isEmpty());
    const QString mode =
        exe.isEmpty() ? QString()
                      : store_->uiString(QStringLiteral("dispatch_mode_") + exe);
    const int index = mode_->findData(mode);
    mode_->setCurrentIndex(index >= 0 ? index : 0);
    mode_->setToolTip(
        exe.isEmpty()
            ? QString::fromUtf8("先在「交给」里选定一个 Agent，再设它的派活模式")
            : QString::fromUtf8(
                  "续当前会话（默认）= 粘进当前打开的会话输入框，读回验证\n"
                  "下次派活开新任务 = 一次性：这一句开新任务，交付后自动回到续当前会话\n"
                  "开新任务怎么开：Qoder 系走 Ctrl+N，WorkBuddy 系走 UIA\n"
                  "没登记键位的宿主：两种模式都粘当前焦点"));
}
