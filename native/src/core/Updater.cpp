#include "Updater.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

namespace {

QString systemCurl() {
    const QString path =
        qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/curl.exe");
    return QFileInfo::exists(path) ? QDir::toNativeSeparators(path)
                                   : QStringLiteral("curl");
}

QStringList stringList(const QJsonValue &value) {
    QStringList out;
    for (const QJsonValue &item : value.toArray()) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty()) {
            out.append(text);
        }
    }
    return out;
}

}  // namespace

QString appVersion() {
#ifdef AGENTRMC_VERSION
    return QStringLiteral(AGENTRMC_VERSION);
#else
    return QStringLiteral("0.0.0");
#endif
}

UpdateInfo fetchUpdateInfo(const QString &url) {
    UpdateInfo info;
    if (url.trimmed().isEmpty()) {
        info.error = QStringLiteral("没有配置更新地址");
        return info;
    }

    QProcess curl;
    curl.start(systemCurl(),
               {QStringLiteral("-sS"), QStringLiteral("--max-time"),
                QStringLiteral("12"), QStringLiteral("-L"), url});
    if (!curl.waitForFinished(15000) || curl.exitStatus() != QProcess::NormalExit ||
        curl.exitCode() != 0) {
        const QString err = QString::fromLocal8Bit(curl.readAllStandardError()).trimmed();
        info.error = err.isEmpty() ? QStringLiteral("连不上更新服务器") : err;
        return info;
    }

    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(curl.readAllStandardOutput(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        info.error = QStringLiteral("更新清单格式不对");
        return info;
    }
    const QJsonObject root = doc.object();
    info.version = root.value("version").toString().trimmed();
    if (info.version.isEmpty()) {
        info.error = QStringLiteral("更新清单里没有版本号");
        return info;
    }
    info.title = root.value("title").toString();
    info.date = root.value("releaseDate").toString();
    info.notes = stringList(root.value("notes"));
    const QJsonObject downloads = root.value("downloads").toObject();
    info.site = downloads.value("site").toString();
    info.quark = downloads.value("quark").toString();
    info.baidu = downloads.value("baidu").toString();
    info.ok = true;
    return info;
}

bool versionIsNewer(const QString &candidate, const QString &current) {
    const QStringList a = candidate.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    const QStringList b = current.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    const int count = qMax(a.size(), b.size());
    for (int i = 0; i < count; ++i) {
        const int left = i < a.size() ? a.at(i).toInt() : 0;
        const int right = i < b.size() ? b.at(i).toInt() : 0;
        if (left != right) {
            return left > right;
        }
    }
    return false;
}
