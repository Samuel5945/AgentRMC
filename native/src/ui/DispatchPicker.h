#pragma once

#include <QJsonObject>
#include <QWidget>

class QComboBox;
class ProfileStore;

// 宿主 + 任务会话一行两下拉，不带标签（文案+tooltip 自解释）。
// 语音页与主窗语音卡共用，选择持久化 ui.agent_target / ui.dispatch_mode_<exe>。
class DispatchPicker final : public QWidget {
    Q_OBJECT

public:
    explicit DispatchPicker(ProfileStore *store, QWidget *parent = nullptr);

    // 重建宿主列表（进程存活/窗口可见会变）并带上派活模式。
    void refresh();
    // 一次性新任务被消费后：派活下拉回到默认。
    void refreshMode();
    // 整窗外壳：下拉内容/选中态序列化，选择走与 combo 相同的持久化路径。
    QJsonObject webState() const;
    void webSelectAgent(const QString &exe);
    void webSelectMode(const QString &mode);

private:
    ProfileStore *store_ = nullptr;
    QComboBox *agent_ = nullptr;
    QComboBox *mode_ = nullptr;
};
