#pragma once

#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class RemotePanel final : public QWidget {
public:
    struct Entry {
        int vk = 0;
        QString glyph;
        QString label;
        QString action;
        bool infrared = false;
    };

    explicit RemotePanel(QWidget *parent = nullptr);

    void setEntries(const QVector<Entry> &entries);
    void setModeKeys(const QSet<int> &keys);
    void setLinkedVk(int vk);
    void flash(int vk);
    QString actionFor(int vk) const;
    QString labelFor(int vk) const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<Entry> entries_;
    QSet<int> modeKeys_;
    int linkedVk_ = 0;
    int litVk_ = -1;
    qint64 litUntil_ = 0;
};
