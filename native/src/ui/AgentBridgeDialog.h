#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QString>

class QLabel;
class QPushButton;
class QVBoxLayout;

// 「接入 Agent（桌宠提醒）」：一个弹窗，一个按钮。
//
// 为什么要它：关掉这个功能的成本太高了——以前要去「使用引导」那一页翻到第 6 节、
// 读一屏对照表、再滚到页底找「复制安装话术」。对绝大多数人来说这就等于「不会装」。
// 这里把动作收敛成：看一眼各家装没装 → 点「复制安装话术」→ 粘给正在干活的 Agent。
//
// 一句话话术本身就覆盖了所有宿主（Qoder / Qoder CN / ZCode / WorkBuddy /
// DeepSeek Harness），Agent 读到自己那一行就照做；OpenSquilla 不走插件，单独标出来。
class AgentBridgeDialog final : public QDialog {
    Q_OBJECT

public:
    explicit AgentBridgeDialog(QWidget *parent = nullptr);

    // health 直接吃 MainWindow::pluginHealth()；sentence 是那句话；
    // pluginsDir 是安装目录里的 plugins（手动装的人只需要认这一个落点）。
    void setData(const QJsonObject &health, const QString &sentence,
                 const QString &pluginsDir);

signals:
    // 「打开插件目录」：把安装目录里的 plugins 文件夹打开。
    void openFolderRequested();
    // 「详细说明」：跳到「使用引导 → 接入 AI Agent」那一节。
    void detailsRequested();

private:
    void clearRows();
    void addRow(const QString &name, const QString &detail, bool ok);

    QVBoxLayout *statusLay_ = nullptr;
    QPushButton *copy_ = nullptr;
    QLabel *hint_ = nullptr;
    QLabel *path_ = nullptr;
    QString sentence_;
};
