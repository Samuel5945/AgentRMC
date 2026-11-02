#include "core/WorkItems.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHashFunctions>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

namespace {

QString iso(const QDateTime &when) {
    return when.isValid() ? when.toUTC().toString(Qt::ISODateWithMs)
                          : QString();
}

QDateTime parseDate(const QJsonValue &value) {
    const QString text = value.toString();
    if (text.isEmpty()) {
        return QDateTime();
    }
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(text, Qt::ISODate);
    }
    return parsed;
}

bool isAttentionStage(const QString &stage) {
    return stage == QStringLiteral("needs-me") ||
           stage == QStringLiteral("review");
}

}  // namespace

WorkItemStore::WorkItemStore(QString path) : path_(std::move(path)) {}

QString WorkItemStore::stageForKind(const QString &kind) {
    if (kind == QStringLiteral("permission") ||
        kind == QStringLiteral("question")) {
        return QStringLiteral("needs-me");
    }
    if (kind == QStringLiteral("stop")) {
        return QStringLiteral("review");
    }
    if (kind == QStringLiteral("progress")) {
        return QStringLiteral("working");
    }
    if (kind == QStringLiteral("voice")) {
        return QStringLiteral("working");
    }
    return QStringLiteral("info");
}

bool WorkItemStore::load(QString *error) {
    items_.clear();
    QFile file(path_);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error != nullptr) {
            *error = parseError.errorString();
        }
        return false;
    }
    QJsonArray array;
    if (doc.isObject()) {
        array = doc.object().value(QStringLiteral("items")).toArray();
    } else if (doc.isArray()) {
        array = doc.array();  // 兼容早期只有数组的格式
    }
    for (const QJsonValue &value : array) {
        if (value.isObject()) {
            items_.append(fromJson(value.toObject()));
        }
    }
    return true;
}

bool WorkItemStore::save(QString *error) const {
    QDir dir = QFileInfo(path_).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot create %1").arg(dir.absolutePath());
        }
        return false;
    }
    QJsonArray array;
    for (const WorkItem &item : items_) {
        array.append(toJson(item));
    }
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("items"), array},
    };
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

WorkItem WorkItemStore::upsert(const QString &source, const QString &agentExe,
                               const QString &sessionId,
                               const QString &project, const QString &kind,
                               const QString &title, const QString &message,
                               const QString &raiseExe,
                               const QString &eventId) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString stage = stageForKind(kind);

    // 生命周期合并：同一 session（或同一 Agent 的近期 working 项）上来了
    // permission/question/stop，就更新原工单，而不是再长一张卡片。
    if (kind == QStringLiteral("permission") ||
        kind == QStringLiteral("question") || kind == QStringLiteral("stop") ||
        kind == QStringLiteral("progress")) {
        const int windowSecs = sessionId.isEmpty() ? 300 : 1800;
        const QDateTime cutoff = now.addSecs(-windowSecs);
        WorkItem *target = nullptr;
        for (WorkItem &item : items_) {
            if (item.done || item.updatedAt < cutoff) {
                continue;
            }
            if (item.stage != QStringLiteral("working") &&
                item.stage != QStringLiteral("needs-me")) {
                continue;
            }
            const bool sameSession = !sessionId.isEmpty() &&
                                     item.sessionId == sessionId;
            const bool sameAgent =
                !agentExe.isEmpty() &&
                item.agentExe.compare(agentExe, Qt::CaseInsensitive) == 0;
            const bool sameTitle =
                !title.isEmpty() &&
                item.title.compare(title, Qt::CaseInsensitive) == 0;
            const bool loose = item.sessionId.isEmpty() || sessionId.isEmpty();
            if (sameSession || (loose && (sameAgent || sameTitle))) {
                target = &item;
                break;
            }
        }
        if (target != nullptr) {
            const bool wasAttention = isAttentionStage(target->stage);
            target->kind = kind;
            target->stage = stage;
            target->title = title;
            target->message = message;
            target->raiseExe = raiseExe;
            target->project = project;
            if (target->sessionId.isEmpty() && !sessionId.isEmpty()) {
                target->sessionId = sessionId;
            }
            target->updatedAt = now;
            if (!wasAttention && isAttentionStage(stage)) {
                target->attentionSince = now;
            }
            if (kind == QStringLiteral("stop") && !sessionId.isEmpty()) {
                resolveSession(sessionId, target->id);
            }
            save();
            return *target;
        }
    }

    // 没有可合并的工单：stop 仍然要把同 session 其它 needs-me 收掉。
    if (kind == QStringLiteral("stop") && !sessionId.isEmpty()) {
        resolveSession(sessionId);
    }

    QString key;
    if (!eventId.isEmpty()) {
        key = sessionId + QLatin1Char(':') + kind + QLatin1Char(':') + eventId;
    } else if (!sessionId.isEmpty()) {
        key = sessionId + QLatin1Char(':') + kind;
    } else {
        key = agentExe + QLatin1Char(':') + kind +
              QLatin1Char(':') + QString::number(qHash(message));
    }

    for (WorkItem &item : items_) {
        if (item.done || item.dedupeKey != key) {
            continue;
        }
        // 没有 tool_use_id 时只合并短时间内的重复投递，否则同一 session
        // 里第二次授权会被误当成同一条。
        if (eventId.isEmpty() && item.updatedAt.secsTo(now) > 8) {
            continue;
        }
        item.title = title;
        item.message = message;
        item.raiseExe = raiseExe;
        item.project = project;
        item.updatedAt = now;
        if (!isAttentionStage(item.stage) && isAttentionStage(stage)) {
            item.attentionSince = now;
        }
        item.stage = stage;
        save();
        return item;
    }

    WorkItem item;
    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.source = source;
    item.agentExe = agentExe;
    item.sessionId = sessionId;
    item.project = project;
    item.kind = kind;
    item.stage = stage;
    item.title = title;
    item.message = message;
    item.raiseExe = raiseExe;
    item.dedupeKey = key;
    item.createdAt = now;
    item.updatedAt = now;
    if (isAttentionStage(stage)) {
        item.attentionSince = now;
    }
    items_.prepend(item);
    prune();
    save();
    return item;
}

int WorkItemStore::attentionCount() const {
    int count = 0;
    for (const WorkItem &item : items_) {
        if (!item.done && isAttentionStage(item.stage)) {
            ++count;
        }
    }
    return count;
}

int WorkItemStore::openCount() const {
    int count = 0;
    for (const WorkItem &item : items_) {
        if (!item.done) {
            ++count;
        }
    }
    return count;
}

bool WorkItemStore::ack(const QString &id) {
    for (WorkItem &item : items_) {
        if (item.id != id) {
            continue;
        }
        item.done = true;
        item.stage = QStringLiteral("done");
        item.updatedAt = QDateTime::currentDateTimeUtc();
        save();
        return true;
    }
    return false;
}

bool WorkItemStore::resolveSession(const QString &sessionId,
                                   const QString &keepId) {
    if (sessionId.isEmpty()) {
        return false;
    }
    bool changed = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (WorkItem &item : items_) {
        if (!item.done && item.id != keepId && item.sessionId == sessionId &&
            item.stage == QStringLiteral("needs-me")) {
            item.done = true;
            item.stage = QStringLiteral("done");
            item.updatedAt = now;
            changed = true;
        }
    }
    if (changed) {
        save();
    }
    return changed;
}

bool WorkItemStore::clearFinished() {
    const int before = items_.size();
    for (int i = items_.size() - 1; i >= 0; --i) {
        if (items_.at(i).done) {
            items_.removeAt(i);
        }
    }
    if (items_.size() == before) {
        return false;
    }
    save();
    return true;
}

QString WorkItemStore::toJson() const {
    QJsonArray array;
    for (const WorkItem &item : items_) {
        array.append(toJson(item));
    }
    const QJsonObject root{
        {QStringLiteral("attention_count"), attentionCount()},
        {QStringLiteral("open_count"), openCount()},
        {QStringLiteral("items"), array},
    };
    return QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QJsonObject WorkItemStore::toJson(const WorkItem &item) {
    return QJsonObject{
        {QStringLiteral("id"), item.id},
        {QStringLiteral("source"), item.source},
        {QStringLiteral("agent_exe"), item.agentExe},
        {QStringLiteral("session_id"), item.sessionId},
        {QStringLiteral("project"), item.project},
        {QStringLiteral("kind"), item.kind},
        {QStringLiteral("stage"), item.stage},
        {QStringLiteral("title"), item.title},
        {QStringLiteral("message"), item.message},
        {QStringLiteral("raise"), item.raiseExe},
        {QStringLiteral("dedupe_key"), item.dedupeKey},
        {QStringLiteral("created_at"), iso(item.createdAt)},
        {QStringLiteral("updated_at"), iso(item.updatedAt)},
        {QStringLiteral("attention_since"), iso(item.attentionSince)},
        {QStringLiteral("done"), item.done},
    };
}

WorkItem WorkItemStore::fromJson(const QJsonObject &obj) {
    WorkItem item;
    item.id = obj.value(QStringLiteral("id")).toString();
    item.source = obj.value(QStringLiteral("source")).toString();
    item.agentExe = obj.value(QStringLiteral("agent_exe")).toString();
    item.sessionId = obj.value(QStringLiteral("session_id")).toString();
    item.project = obj.value(QStringLiteral("project")).toString();
    item.kind = obj.value(QStringLiteral("kind")).toString();
    item.stage = obj.value(QStringLiteral("stage")).toString();
    item.title = obj.value(QStringLiteral("title")).toString();
    item.message = obj.value(QStringLiteral("message")).toString();
    item.raiseExe = obj.value(QStringLiteral("raise")).toString();
    item.dedupeKey = obj.value(QStringLiteral("dedupe_key")).toString();
    item.createdAt = parseDate(obj.value(QStringLiteral("created_at")));
    item.updatedAt = parseDate(obj.value(QStringLiteral("updated_at")));
    item.attentionSince =
        parseDate(obj.value(QStringLiteral("attention_since")));
    item.done = obj.value(QStringLiteral("done")).toBool(false);
    if (item.id.isEmpty()) {
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    return item;
}

void WorkItemStore::prune() {
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-7);
    for (int i = items_.size() - 1; i >= 0; --i) {
        if (items_.at(i).done && items_.at(i).updatedAt.isValid() &&
            items_.at(i).updatedAt < cutoff) {
            items_.removeAt(i);
        }
    }
    while (items_.size() > 200) {
        int removed = -1;
        for (int i = items_.size() - 1; i >= 0; --i) {
            if (items_.at(i).done) {
                removed = i;
                break;
            }
        }
        items_.removeAt(removed >= 0 ? removed : items_.size() - 1);
    }
}
