#include "core/SessionDeleteQueue.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

namespace {
constexpr qint64 kPollMs = 1000;
}

SessionDeleteQueue::SessionDeleteQueue(QObject *parent, qint64 takeTimeoutMs)
    : QObject(parent), timeoutMs_(takeTimeoutMs) {
    timer_ = new QTimer(this);
    // 生产超时 60s，1s 轮询足够；测试把超时缩小时让间隔跟着缩，否则 poll
    // 触发不到（间隔不超过超时的 1/4，两边都不空转）。
    timer_->setInterval(timeoutMs_ < kPollMs * 4 ? qMax<qint64>(25, timeoutMs_ / 4)
                                                 : kPollMs);
    connect(timer_, &QTimer::timeout, this, &SessionDeleteQueue::poll);
}

bool SessionDeleteQueue::supportsHost(const QString &agentExe) {
    return agentExe.compare(QStringLiteral("OpenSquilla.exe"),
                            Qt::CaseInsensitive) == 0;
}

int SessionDeleteQueue::enqueue(const QJsonArray &items, QStringList *idsOut) {
    int accepted = 0;
    for (const QJsonValue &value : items) {
        const QJsonObject obj = value.toObject();
        const QString session = obj.value(QStringLiteral("session")).toString();
        const QString title = obj.value(QStringLiteral("title")).toString();
        if (session.isEmpty()) {
            continue;
        }
        // 同会话在途不重复排队：桥还没回执前再点删只是叠一条一样的真删。
        bool inflight = false;
        for (const Job &job : jobs_) {
            if (job.session == session) {
                inflight = true;
                break;
            }
        }
        if (inflight) {
            continue;
        }
        Job job;
        job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        job.session = session;
        job.title = title;
        job.clock.start();
        jobs_.append(job);
        if (idsOut != nullptr) {
            idsOut->append(job.id);
        }
        ++accepted;
    }
    if (accepted > 0 && !timer_->isActive()) {
        timer_->start();
    }
    return accepted;
}

QJsonArray SessionDeleteQueue::takePending() {
    QJsonArray out;
    for (Job &job : jobs_) {
        if (job.handed) {
            continue;
        }
        // 一次取件标记一次：ack 或超时前不重复下发，防桥重连后双删。
        job.handed = true;
        out.append(QJsonObject{{QStringLiteral("delete_id"), job.id},
                               {QStringLiteral("session"), job.session},
                               {QStringLiteral("title"), job.title}});
    }
    return out;
}

bool SessionDeleteQueue::ack(const QString &deleteId, bool ok,
                             const QString &error) {
    for (int i = 0; i < jobs_.size(); ++i) {
        if (jobs_.at(i).id != deleteId) {
            continue;
        }
        const Job job = jobs_.takeAt(i);
        emit finished(job.id, job.session, job.title, ok, error);
        if (jobs_.isEmpty() && timer_->isActive()) {
            timer_->stop();
        }
        return true;
    }
    return false;
}

void SessionDeleteQueue::poll() {
    for (int i = jobs_.size() - 1; i >= 0; --i) {
        if (jobs_.at(i).clock.isValid() &&
            jobs_.at(i).clock.elapsed() > timeoutMs_) {
            const Job job = jobs_.takeAt(i);
            emit finished(job.id, job.session, job.title, false,
                          QStringLiteral("桥没来取件（OpenSquilla 没开或桥过旧），"
                                         "未删除"));
        }
    }
    if (jobs_.isEmpty() && timer_->isActive()) {
        timer_->stop();
    }
}
