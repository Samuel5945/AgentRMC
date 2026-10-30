#pragma once

#include <QFrame>
#include <QString>

struct MapCellData {
    QString title;
    QString effect;
    QString tip;
    bool editable = true;
};

class MapCell final : public QFrame {
    Q_OBJECT

public:
    MapCell(int vk, QString gesture, MapCellData data, QWidget *parent = nullptr);

    int vk() const { return vk_; }
    QString gesture() const { return gesture_; }
    QString titleText() const { return title_; }
    QString effectText() const { return effect_; }

signals:
    void clicked(int vk, const QString &gesture);
    void hovered(int vk, bool on);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    int vk_ = 0;
    QString gesture_;
    QString title_;
    QString effect_;
    bool editable_ = true;
};

class MapKeyRow final : public QFrame {
    Q_OBJECT

public:
    MapKeyRow(int vk, QString glyph, QString name, MapCellData single,
              MapCellData doubleCell, MapCellData hold, bool selected,
              QWidget *parent = nullptr);

    int vk() const { return vk_; }
    void setSelected(bool on);

signals:
    void picked(int vk);
    void cellClicked(int vk, const QString &gesture);
    void hovered(int vk, bool on);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    int vk_ = 0;
};
