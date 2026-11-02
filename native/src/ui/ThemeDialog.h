#pragma once

#include <QDialog>

class ProfileStore;
class QLabel;
class QListWidget;
class QSlider;
class QTimer;

// 主题与外观：换主题、调卡片不透明度 / 背景压暗。
// 语音设置里也有一个主题下拉，但那里是「顺手改」，这里才是专门入口——
// 换肤和「字看不清」是两件用户会专门去找的事。
class ThemeDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ThemeDialog(ProfileStore *store, QWidget *parent = nullptr);

private:
    void reloadThemes();
    void reloadSliders();
    void syncLabels();
    // 滑块拖动时别每一格都重刷整窗样式：攒 60ms 再落一次。
    void scheduleApply();

    ProfileStore *store_ = nullptr;
    QListWidget *themes_ = nullptr;
    QSlider *alpha_ = nullptr;
    QSlider *dim_ = nullptr;
    QLabel *alphaValue_ = nullptr;
    QLabel *dimValue_ = nullptr;
    QTimer *debounce_ = nullptr;
};
