#include "core/ZcodeDispatch.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// 各阶段超时。turn 是 LLM 节奏，给足；落库确认是秒级（异步写盘很快）。
constexpr qint64 kSpawnTimeoutMs = 20000;
constexpr qint64 kReadyTimeoutMs = 20000;
constexpr qint64 kResumeTimeoutMs = 40000;
constexpr qint64 kSendTimeoutMs = 30000;
constexpr qint64 kPersistTimeoutMs = 45000;
constexpr qint64 kTurnTimeoutMs = 600000;
constexpr qint64 kPersistPollMs = 1500;
constexpr qint64 kTurnPollMs = 3000;

QJsonObject parseLine(const QByteArray &line) {
    return QJsonDocument::fromJson(line).object();
}

}  // namespace

ZcodeDispatch::ZcodeDispatch(QObject *parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(200);
    connect(timer_, &QTimer::timeout, this, &ZcodeDispatch::tick);
}

ZcodeDispatch::~ZcodeDispatch() {
    // 事件循环可能已经不在转（应用退出路径）：同步回收，别指望 deleteLater。
    cleanup();
}

bool ZcodeDispatch::supports(const QString &exe) {
    return exe.compare(QStringLiteral("ZCode.exe"), Qt::CaseInsensitive) == 0;
}

QString ZcodeDispatch::zcodeHome() {
    const QByteArray overrideHome = qgetenv("AGENTRMC_ZCODE_HOME");
    if (!overrideHome.isEmpty()) {
        return QString::fromUtf8(overrideHome);
    }
    return QDir::homePath() + QStringLiteral("/.zcode");
}

QString ZcodeDispatch::zcodeExeFromRegistry() {
    const QByteArray overrideExe = qgetenv("AGENTRMC_ZCODE_EXE");
    if (!overrideExe.isEmpty()) {
        return QString::fromUtf8(overrideExe);
    }
    // zcode:// 协议的 handler 就是桌面端："D:\...\ZCode.exe" -- "%1"。
    // Qt 的 QSettings 读注册表默认值键名不稳定，直接走 winreg。
    char buf[1024] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueA(HKEY_CLASSES_ROOT,
                     "zcode\\shell\\open\\command", nullptr,
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return QString();
    }
    const QString cmd = QString::fromLocal8Bit(buf);
    static const QRegularExpression quoted(
        QStringLiteral("^\\s*\"([^\"]+)\""));
    const QRegularExpressionMatch m = quoted.match(cmd);
    if (m.hasMatch()) {
        return m.captured(1);
    }
    return QString();
}

QString ZcodeDispatch::cjsPathFor(const QString &zcodeExe) {
    return QFileInfo(zcodeExe).absolutePath() +
           QStringLiteral("/resources/glm/zcode.cjs");
}

bool ZcodeDispatch::sessionRecord(const QString &sessionId, QString *directory,
                                  QJsonObject *modelSelection,
                                  qint64 *timeUpdatedMs) {
    const QString dbPath =
        zcodeHome() + QStringLiteral("/cli/db/db.sqlite");
    if (!QFileInfo::exists(dbPath)) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), QStringLiteral("zcode-dispatch"));
    db.setDatabaseName(dbPath);
    // 只读：宿主进程是唯一的写者，这里毫秒级共享锁读一把就走。
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!db.open()) {
        return false;
    }
    bool ok = false;
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT directory, time_updated FROM session "
            "WHERE id = ? AND time_deleted IS NULL"));
        q.addBindValue(sessionId);
        if (q.exec() && q.next()) {
            if (directory != nullptr) {
                *directory = q.value(0).toString();
            }
            if (timeUpdatedMs != nullptr) {
                *timeUpdatedMs = q.value(1).toLongLong();
            }
            ok = true;
        }
        q.finish();
    }
    if (ok && modelSelection != nullptr) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT data FROM session_entry "
            "WHERE session_id = ? AND type = 'runtime/model_selection'"));
        q.addBindValue(sessionId);
        if (q.exec() && q.next()) {
            const QJsonObject entry = QJsonDocument::fromJson(
                q.value(0).toString().toUtf8()).object();
            const QJsonObject sel =
                entry.value(QStringLiteral("modelSelection")).toObject();
            if (!sel.isEmpty()) {
                *modelSelection = sel;
            }
        }
        q.finish();
    }
    db.close();
    QSqlDatabase::removeDatabase(QStringLiteral("zcode-dispatch"));
    return ok;
}

void ZcodeDispatch::start(const Job &job) {
    job_ = job;
    stage_ = Stage::Spawn;
    stageClock_.start();
    pollClock_.start();
    process_ = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ELECTRON_RUN_AS_NODE"), QStringLiteral("1"));
    process_->setProcessEnvironment(env);
    process_->setWorkingDirectory(QDir::homePath());
    process_->setProgram(job_.zcodeExe);
    process_->setArguments({cjsPathFor(job_.zcodeExe),
                            QStringLiteral("app-server"), QStringLiteral("--stdio")});
    connect(process_, &QProcess::readyReadStandardOutput, this,
            &ZcodeDispatch::onReadyRead);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (stage_ == Stage::Spawn) {
            fail(QStringLiteral("ZCode app-server 启动失败：%1")
                     .arg(process_->errorString()));
        }
    });
    connect(process_, &QProcess::finished, this, [this]() {
        if (!done_ && !sent_) {
            fail(QStringLiteral("ZCode app-server 提前退出"));
        } else if (!done_) {
            finishTurn(QString());
        }
    });
    process_->start();
    timer_->start();
}

void ZcodeDispatch::onReadyRead() {
    buffer_ += process_->readAllStandardOutput();
    for (;;) {
        const int nl = buffer_.indexOf('\n');
        if (nl < 0) {
            break;
        }
        const QByteArray line = buffer_.left(nl);
        buffer_.remove(0, nl + 1);
        if (!line.trimmed().isEmpty()) {
            handleLine(line);
        }
    }
}

void ZcodeDispatch::handleLine(const QByteArray &line) {
    const QJsonObject obj = parseLine(line);
    if (obj.isEmpty()) {
        return;
    }
    if (obj.contains(QStringLiteral("method"))) {
        if (obj.contains(QStringLiteral("id"))) {
            // server→client 请求：偏好请求必须成功应答（只填必填字段，其余走
            // 默认），其它一律回错误（桌面端超时路径同样是回错误，服务端容忍）。
            const QString method = obj.value(QStringLiteral("method")).toString();
            QJsonObject reply;
            if (method == QStringLiteral("session/requestRuntimePreferences")) {
                reply = QJsonObject{{QStringLiteral("result"),
                                     QJsonObject{{QStringLiteral(
                                         "nativeSearchEnhancementsEnabled"),
                                                  false}}}};
            } else {
                reply = QJsonObject{
                    {QStringLiteral("error"),
                     QJsonObject{{QStringLiteral("code"), -32000},
                                 {QStringLiteral("message"),
                                  QStringLiteral(
                                      "agentrmc: unsupported client request")}}}};
            }
            reply.insert(QStringLiteral("id"), obj.value(QStringLiteral("id")));
            process_->write(
                (QJsonDocument(reply).toJson(QJsonDocument::Compact) + "\n"));
        } else if (
            stage_ == Stage::Spawn &&
            obj.value(QStringLiteral("method")).toString() ==
                QStringLiteral("startup/storageState") &&
            obj.value(QStringLiteral("params")).toObject()
                    .value(QStringLiteral("phase")).toString() ==
                QStringLiteral("ready")) {
            // 会话库就绪：直接进 resume（免 workspace，会话 id 即定位）。
            stage_ = Stage::Resume;
            stageClock_.restart();
            request(QStringLiteral("session/resume"),
                    QJsonObject{{QStringLiteral("sessionId"), job_.sessionId}});
        }
        return;
    }
    if (obj.contains(QStringLiteral("id")) && awaitingId_ >= 0 &&
        obj.value(QStringLiteral("id")).toInt() == awaitingId_) {
        awaitingId_ = -1;
        evaluate(obj);
    }
}

void ZcodeDispatch::request(const QString &method, const QJsonObject &params) {
    QJsonObject frame;
    const int id = nextId_++;
    frame.insert(QStringLiteral("id"), id);
    frame.insert(QStringLiteral("method"), method);
    frame.insert(QStringLiteral("params"), params);
    awaitingId_ = id;
    stageClock_.restart();
    process_->write((QJsonDocument(frame).toJson(QJsonDocument::Compact) + "\n"));
}

void ZcodeDispatch::evaluate(const QJsonObject &response) {
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    const QJsonObject error = response.value(QStringLiteral("error")).toObject();
    switch (stage_) {
    case Stage::Resume:
        if (!error.isEmpty() || result.isEmpty()) {
            fail(QStringLiteral("ZCode resume 失败：%1")
                     .arg(error.value(QStringLiteral("message")).toString()));
            return;
        }
        stage_ = Stage::Send;
        request(QStringLiteral("session/send"),
                QJsonObject{{QStringLiteral("sessionId"), job_.sessionId},
                            {QStringLiteral("content"), job_.text},
                            {QStringLiteral("modelSelection"), job_.modelSelection}});
        return;
    case Stage::Send:
        if (!error.isEmpty()) {
            const QString msg = error.value(QStringLiteral("message")).toString();
            fail(msg.contains(QStringLiteral("already running"))
                     ? QStringLiteral("该会话正在跑，稍后再派")
                     : QStringLiteral("ZCode send 失败：%1").arg(msg));
            return;
        }
        stage_ = Stage::Persist;
        pollClock_.restart();
        return;
    case Stage::Persist:
    case Stage::Turn: {
        if (!error.isEmpty()) {
            return;  // 读回执偶发失败：下一轮再试。
        }
        const QJsonArray messages = result.value(QStringLiteral("messages")).toArray();
        int ourIndex = -1;
        for (int i = messages.size() - 1; i >= 0; --i) {
            const QJsonObject msg = messages.at(i).toObject();
            if (textOfMessage(msg).contains(job_.text)) {
                ourIndex = i;
                break;
            }
        }
        if (ourIndex < 0) {
            return;  // 还没落库。
        }
        if (!sent_) {
            sent_ = true;
            stage_ = Stage::Turn;
            pollClock_.restart();
            emit sent(job_.dispatchId);
        }
        if (stage_ == Stage::Turn) {
            for (int i = ourIndex + 1; i < messages.size(); ++i) {
                const QJsonObject msg = messages.at(i).toObject();
                if ((msg.value(QStringLiteral("info")).toObject()
                         .value(QStringLiteral("role")).toString() ==
                     QStringLiteral("assistant"))) {
                    const QString text = textOfMessage(msg);
                    if (!text.trimmed().isEmpty()) {
                        finishTurn(text);
                        return;
                    }
                }
            }
        }
        return;
    }
    case Stage::Spawn:
    case Stage::Ready:
        break;
    }
}

void ZcodeDispatch::tick() {
    if (done_ || process_ == nullptr) {
        return;
    }
    const qint64 elapsed = stageClock_.elapsed();
    switch (stage_) {
    case Stage::Spawn:
        if (elapsed > kSpawnTimeoutMs) {
            fail(QStringLiteral("ZCode app-server 启动超时"));
        }
        return;
    case Stage::Ready:
        if (elapsed > kReadyTimeoutMs) {
            fail(QStringLiteral("ZCode app-server 握手超时"));
        }
        return;
    case Stage::Resume:
        if (elapsed > kResumeTimeoutMs) {
            fail(QStringLiteral("ZCode resume 超时"));
        }
        return;
    case Stage::Send:
        if (elapsed > kSendTimeoutMs) {
            fail(QStringLiteral("ZCode send 超时"));
        }
        return;
    case Stage::Persist:
        if (elapsed > kPersistTimeoutMs) {
            fail(QStringLiteral("派发消息落库确认超时"));
            return;
        }
        if (awaitingId_ < 0 && pollClock_.elapsed() >= kPersistPollMs) {
            request(QStringLiteral("session/messages"),
                    QJsonObject{{QStringLiteral("sessionId"), job_.sessionId}});
        }
        return;
    case Stage::Turn:
        if (elapsed > kTurnTimeoutMs) {
            finishTurn(QString());
            return;
        }
        if (awaitingId_ < 0 && pollClock_.elapsed() >= kTurnPollMs) {
            request(QStringLiteral("session/messages"),
                    QJsonObject{{QStringLiteral("sessionId"), job_.sessionId}});
        }
        return;
    }
}

void ZcodeDispatch::fail(const QString &reason) {
    if (done_) {
        return;
    }
    done_ = true;
    const QString id = job_.dispatchId;
    cleanup();
    emit failed(id, reason);
}

void ZcodeDispatch::finishTurn(const QString &reply) {
    if (done_) {
        return;
    }
    done_ = true;
    const QString id = job_.dispatchId;
    cleanup();
    emit turnFinished(id, reply);
}

void ZcodeDispatch::cleanup() {
    done_ = true;
    if (timer_ != nullptr) {
        timer_->stop();
    }
    if (process_ != nullptr) {
        QProcess *p = process_;
        process_ = nullptr;
        p->disconnect(this);
        p->kill();
        // 等它死透再删：kill 后最多 1 秒，避免析构链里跟子进程赛跑。
        p->waitForFinished(1000);
        p->deleteLater();
    }
}

QString ZcodeDispatch::textOfMessage(const QJsonObject &message) {
    QString joined;
    for (const QJsonValue &part : message.value(QStringLiteral("parts")).toArray()) {
        const QJsonObject o = part.toObject();
        if (o.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
            joined += o.value(QStringLiteral("text")).toString();
        }
    }
    return joined;
}
