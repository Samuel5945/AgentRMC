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

#include <algorithm>

namespace {

constexpr qint64 kStaleWorkingSecs = 1800;  // 30 分钟没动静就不再说「在跑」

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

QString WorkItemStore::kindForReport(const QString &stage) {
    const QString s = stage.trimmed().toLower();
    if (s == QStringLiteral("progress") || s == QStringLiteral("working")) {
        return QStringLiteral("progress");
    }
    if (s == QStringLiteral("blocked") || s == QStringLiteral("needs-me")) {
        // 卡住=等人，和「等你选方案」同一个注意力档位，复用 question。
        return QStringLiteral("question");
    }
    if (s == QStringLiteral("ready") || s == QStringLiteral("review")) {
        // 做完了等人验收，复用 stop 的语义（会把同 session 的旧 needs-me 归档）。
        return QStringLiteral("stop");
    }
    return QString();
}

WorkItem WorkItemStore::report(const QString &agentExe, const QString &raiseExe,
                               const QString &sessionId, const QString &project,
                               const QString &stage, const QString &title,
                               const QString &message) {
    const QString kind = kindForReport(stage);
    if (kind.isEmpty()) {
        return WorkItem{};  // 调用方据此回 400，别静默当成成功
    }
    return upsert(QStringLiteral("mcp"), agentExe, sessionId, project, kind,
                  title, message, raiseExe);
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

    // 已有活跃工单的 session，它的 stop 归那张卡的 lineage：无 session 的
    // voice 派活回执不许吸收（真机 16:29 把用户会话错标到派活卡上），
    // 而是把 voice 回执结案，注意力落在该 session 自己的卡上。
    if (kind == QStringLiteral("stop") && !sessionId.isEmpty()) {
        bool owned = false;
        for (const WorkItem &item : items_) {
            if (!item.done && item.sessionId == sessionId) {
                owned = true;
                break;
            }
        }
        if (owned) {
            const QDateTime cutoff = now.addSecs(-300);
            for (WorkItem &item : items_) {
                if (item.done || item.source != QStringLiteral("voice") ||
                    !item.sessionId.isEmpty() || item.updatedAt < cutoff) {
                    continue;
                }
                if (!agentExe.isEmpty() &&
                    item.agentExe.compare(agentExe, Qt::CaseInsensitive) != 0) {
                    continue;
                }
                item.done = true;
                item.stage = QStringLiteral("done");
                item.updatedAt = now;
            }
        }
    }

    // 同一 session 的新活动（permission/progress/…）说明上一轮已经翻篇，
    // 旧 review 直接归档；stop 还要保留本轮新 review，所以放到后面处理。
    if (kind != QStringLiteral("stop") && !sessionId.isEmpty()) {
        resolveReviews(sessionId);
    }

    // 生命周期合并：同一 session（或同一 Agent 的近期 working 项）上来了
    // permission/question/stop，就更新原工单，而不是再长一张卡片。
    if (kind == QStringLiteral("permission") ||
        kind == QStringLiteral("question") || kind == QStringLiteral("stop") ||
        kind == QStringLiteral("progress")) {
        const int windowSecs = sessionId.isEmpty() ? 300 : 1800;
        const QDateTime cutoff = now.addSecs(-windowSecs);
        WorkItem *target = nullptr;
        // 认领优先级：同 session 的卡 > 待绑定的派活回执 > 未关联的卡 >
        // 同宿主唯一一张已绑定的卡。后两档只在来件本身没有 session 时才考虑。
        WorkItem *voiceReceipt = nullptr;
        WorkItem *unbound = nullptr;
        WorkItem *soleBound = nullptr;
        int boundCandidates = 0;
        for (WorkItem &item : items_) {
            if (item.done || item.updatedAt < cutoff) {
                continue;
            }
            if (item.stage != QStringLiteral("working") &&
                item.stage != QStringLiteral("needs-me")) {
                continue;
            }
            const bool sameAgent =
                !agentExe.isEmpty() &&
                item.agentExe.compare(agentExe, Qt::CaseInsensitive) == 0;
            const bool sameTitle =
                !title.isEmpty() &&
                item.title.compare(title, Qt::CaseInsensitive) == 0;
            // 项目两边都知道又不一样 = 不是同一件事（同宿主开两个目录时，
            // 光按宿主名合并会把另一个目录的事写进这张卡）。
            const bool projectOk =
                project.isEmpty() || item.project.isEmpty() ||
                item.project.compare(project, Qt::CaseInsensitive) == 0;
            if (!sessionId.isEmpty() && item.sessionId == sessionId) {
                target = &item;
                break;
            }
            if (!item.sessionId.isEmpty()) {
                if (sessionId.isEmpty() && sameAgent && projectOk) {
                    ++boundCandidates;
                    soleBound = &item;
                }
                continue;
            }
            if (!projectOk) {
                continue;
            }
            if (sessionId.isEmpty()) {
                if (unbound == nullptr && (sameAgent || sameTitle)) {
                    unbound = &item;
                }
            } else if (voiceReceipt == nullptr && sameAgent &&
                       item.source == QStringLiteral("voice")) {
                voiceReceipt = &item;
            }
        }
        if (target == nullptr) {
            if (!sessionId.isEmpty()) {
                target = voiceReceipt;
            } else {
                target = unbound != nullptr
                             ? unbound
                             : (boundCandidates == 1 ? soleBound : nullptr);
            }
        }
        if (target != nullptr) {
            const bool wasAttention = isAttentionStage(target->stage);
            target->kind = kind;
            target->stage = stage;
            target->title = title;
            target->message = message;
            target->raiseExe = raiseExe;
            // session id 是宿主内唯一的：同一 session 报来别的宿主名只可能是
            // 先前记错了（WorkBuddy 内嵌 Qoder SDK 曾被认成 Qoder CN），纠正。
            if (!agentExe.isEmpty()) {
                target->agentExe = agentExe;
            }
            // 只认 session 之外，项目是第二道判据：来件没带项目时别把卡片原有的抹掉，
            // 否则下一次「按项目区分」就没了依据（MCP 上报常常不带 cwd）。
            if (!project.isEmpty()) {
                target->project = project;
            }
            if (target->sessionId.isEmpty() && !sessionId.isEmpty()) {
                target->sessionId = sessionId;
            }
            target->updatedAt = now;
            if (!wasAttention && isAttentionStage(stage)) {
                target->attentionSince = now;
            }
            // 一个会话只留一张活卡：状态变了，同会话的其它未结案记录就该翻篇，
            // 不然列表里同一件事挂着三条「待验收」，谁也说不清哪条是现在。
            if (!sessionId.isEmpty()) {
                closeOtherStates(sessionId, target->id);
            }
            save();
            return *target;
        }
    }

    // 没有可合并的工单：先算 dedupe key；如果是重复 stop，上面的 dedupe
    // 会直接更新旧 review，不会走到这里把它归档掉。
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
        if (!agentExe.isEmpty()) {
            item.agentExe = agentExe;
        }
        if (!project.isEmpty()) {
            item.project = project;
        }
        item.updatedAt = now;
        if (!isAttentionStage(item.stage) && isAttentionStage(stage)) {
            item.attentionSince = now;
        }
        item.stage = stage;
        save();
        return item;
    }

    // 没有可合并的卡 = 这个会话新一轮：之前那张活卡翻篇（同会话只留一张）。
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
    if (!sessionId.isEmpty()) {
        closeOtherStates(sessionId, item.id);
    }
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

qint64 WorkItem::waitedSecs(const QDateTime &now) const {
    if (done || !isAttentionStage(stage) || !attentionSince.isValid()) {
        return -1;
    }
    return attentionSince.secsTo(now);
}

bool WorkItem::staleWorking(const QDateTime &now) const {
    return !done && stage == QStringLiteral("working") && updatedAt.isValid() &&
           updatedAt.secsTo(now) > kStaleWorkingSecs;
}

QVector<WorkItem> WorkItemStore::displayOrder() const {
    QVector<WorkItem> out;
    out.reserve(items_.size());
    for (const WorkItem &item : items_) {
        if (!item.done) {
            out.append(item);
        }
    }
    const auto rank = [](const QString &stage) {
        if (stage == QStringLiteral("needs-me")) {
            return 0;
        }
        if (stage == QStringLiteral("review")) {
            return 1;
        }
        if (stage == QStringLiteral("working")) {
            return 2;
        }
        return 3;
    };
    std::stable_sort(out.begin(), out.end(), [&rank](const WorkItem &a,
                                                     const WorkItem &b) {
        const int ra = rank(a.stage);
        const int rb = rank(b.stage);
        if (ra != rb) {
            return ra < rb;
        }
        if (ra <= 1) {
            // 等人这件事按「开始等」的先后排队：新到的进度不该把老账挤下去。
            const QDateTime wa = a.attentionSince.isValid() ? a.attentionSince
                                                           : a.updatedAt;
            const QDateTime wb = b.attentionSince.isValid() ? b.attentionSince
                                                           : b.updatedAt;
            if (wa != wb) {
                return wa < wb;
            }
        }
        return a.updatedAt > b.updatedAt;
    });
    return out;
}

bool WorkItemStore::closeOtherStates(const QString &sessionId,
                                     const QString &keepId) {
    if (sessionId.isEmpty()) {
        return false;
    }
    bool changed = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (WorkItem &item : items_) {
        if (!item.done && item.id != keepId && item.sessionId == sessionId) {
            item.done = true;
            item.stage = QStringLiteral("done");
            item.updatedAt = now;
            changed = true;
        }
    }
    return changed;
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
                                   const QString &keepId,
                                   bool includeReview) {
    if (sessionId.isEmpty()) {
        return false;
    }
    bool changed = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (WorkItem &item : items_) {
        const bool attention =
            item.stage == QStringLiteral("needs-me") ||
            (includeReview && item.stage == QStringLiteral("review"));
        if (!item.done && item.id != keepId && item.sessionId == sessionId &&
            attention) {
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

bool WorkItemStore::resolveReviews(const QString &sessionId,
                                   const QString &keepId) {
    if (sessionId.isEmpty()) {
        return false;
    }
    bool changed = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (WorkItem &item : items_) {
        if (!item.done && item.id != keepId && item.sessionId == sessionId &&
            item.stage == QStringLiteral("review")) {
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
    // 对外按面板同一套顺序回（displayOrder），别让人对着 JSON 数一遍、
    // 对着界面又数一遍——两边顺序不一样时，谁都不知道哪个是真相。
    // 已结案的一条不丢：排在后面，历史还查得到。
    QJsonArray array;
    for (const WorkItem &item : displayOrder()) {
        array.append(toJson(item));
    }
    for (const WorkItem &item : items_) {
        if (item.done) {
            array.append(toJson(item));
        }
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
