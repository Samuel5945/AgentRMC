#pragma once

#include <QFrame>
#include <QHash>
#include <QString>

#include "core/Profiles.h"
#include "ui/MapWidgets.h"

class QComboBox;
class ToggleSwitch;
class QLabel;
class QScrollArea;
class QTimer;
class QVBoxLayout;

class KeymapPage final : public QFrame {
    Q_OBJECT

public:
    explicit KeymapPage(ProfileStore *store, QWidget *parent = nullptr);
    void refreshAll();

private:
    void buildUi();
    void rebuildCombo();
    void refreshScene();
    void rebuildTable();
    void selectRow(int vk);
    void openMenu(int vk, const QString &gesture);
    void applyPreset(int vk, const QString &gesture, const QString &kind);
    void captureCombo(int vk, const QString &gesture);
    void flashHint(const QString &text, const QString &color);
    void syncTakeoverUi();
    void resetHint();
    void openProfiles();
    void reloadProfiles();

    MapCellData singleCell(int vk, const Profile &profile) const;
    MapCellData doubleCell(int vk, const Profile &profile) const;
    MapCellData holdCell(int vk, const Profile &profile) const;
    QWidget *buildHeaderRow();
    QWidget *buildSection(const QString &title);
    bool hasNav(const Profile &profile) const;

    const Profile &currentProfile();
    QString keyName(int vk) const;
    QString gestureLabel(const QString &gesture) const;

    ProfileStore *store_ = nullptr;
    QComboBox *combo_ = nullptr;
    QLabel *sceneDot_ = nullptr;
    QLabel *sceneName_ = nullptr;
    QLabel *sceneRule_ = nullptr;
    QLabel *footHint_ = nullptr;
    QLabel *takeoverLabel_ = nullptr;
    ToggleSwitch *takeoverSwitch_ = nullptr;
    QScrollArea *scroll_ = nullptr;
    QWidget *holder_ = nullptr;
    QVBoxLayout *grid_ = nullptr;
    QHash<int, MapKeyRow *> rows_;
    int selectedVk_ = 38;
    QString shownName_;
    QTimer *tick_ = nullptr;
    QTimer *hintTimer_ = nullptr;
};
