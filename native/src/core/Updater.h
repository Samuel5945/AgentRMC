#pragma once

#include <QString>
#include <QStringList>

struct UpdateInfo {
    bool ok = false;
    QString error;
    QString version;
    QString title;
    QString date;
    QStringList notes;
    QString site;
    QString quark;
    QString baidu;
};

// 拉取 <url> 的更新清单（curl，12 秒超时，阻塞，调用方放线程池里）。
UpdateInfo fetchUpdateInfo(const QString &url);

// 语义化版本比较：a.b.c 逐段比数字。
bool versionIsNewer(const QString &candidate, const QString &current);

QString appVersion();
