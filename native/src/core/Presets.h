#pragma once

#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Presets {

QString actionLabel(const QString &kind);
QJsonObject actionFor(const QString &kind, int vk, const QString &gesture);
QVector<QPair<QString, QStringList>> groups();
QStringList holdIds();
QStringList doubleModeIds();
QString actionCn(const QString &sem);
QString nativeLabel(int vk);
QString remoteKeyName(int vk);
QString vkLabel(int vk);
QString comboText(const QStringList &mods, int vk);
QString modeLabel(const QString &kind);
QString describeAction(const QJsonObject &act, bool holdInfo = true);
QString shortDesc(const QJsonObject &act, int keyVk);
QString nativeEffect(const QJsonObject &act);
QString actionEffectText(const QJsonObject &act);
QPair<QString, QString> actionCells(const QJsonObject &act, int vk,
                                    const QString &sem);

}  // namespace Presets
