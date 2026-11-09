#include "core/ZcodeDispatch.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include <algorithm>

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
// 授权/提问请求：外部派发没有代答 UI，明确拒绝并把原因回给模型/用户，
// 不能挂在 -32000「不支持」上让模型以为工具自己坏了。
const char kDenyPermissionReason[] =
    "AgentRMC 定向派发通道不代答授权；请在 ZCode 桌面端处理";
const char kDeclineInputReason[] =
    "AgentRMC 定向派发通道不代答提问；请在 ZCode 桌面端处理";

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

QString ZcodeDispatch::builtinProviderConfigFor(const QString &zcodeExe) {
    const QString path = QFileInfo(zcodeExe).absolutePath() +
                         QStringLiteral("/resources/config/provider/zcode-builtin.json");
    return QFileInfo::exists(path) ? path : QString();
}

bool ZcodeDispatch::restartDesktop(QString *error) {
    const QString exe = zcodeExeFromRegistry();
    if (exe.isEmpty() || !QFileInfo::exists(exe)) {
        if (error != nullptr) {
            *error = QStringLiteral("找不到 ZCode 桌面端（zcode:// 协议未注册）");
        }
        return false;
    }
    // 优雅关：Electron 收 WM_CLOSE 能保存状态退出；3s 不退再强杀。
    // /T 连子进程树一起：app-server / utility host 都是它的孩子，留着占端口。
    const QString image = QFileInfo(exe).fileName();
    QProcess::execute(QStringLiteral("taskkill"),
                      {QStringLiteral("/IM"), image, QStringLiteral("/T")});
    QProcess probe;
    probe.start(QStringLiteral("tasklist"),
                {QStringLiteral("/FI"),
                 QStringLiteral("IMAGENAME eq ") + image,
                 QStringLiteral("/NH")});
    probe.waitForFinished(3000);
    if (QString::fromLocal8Bit(probe.readAllStandardOutput())
            .contains(image, Qt::CaseInsensitive)) {
        QProcess::execute(QStringLiteral("taskkill"),
                          {QStringLiteral("/F"), QStringLiteral("/IM"), image,
                           QStringLiteral("/T")});
    }
    if (!QProcess::startDetached(exe, {})) {
        if (error != nullptr) {
            *error = QStringLiteral("ZCode 已关闭，但重新启动失败：%1").arg(exe);
        }
        return false;
    }
    return true;
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
    const double rawScale =
        qEnvironmentVariable("AGENTRMC_ZCODE_TIME_SCALE", QStringLiteral("1"))
            .toDouble();
    timeScale_ = std::clamp(rawScale, 0.001, 1.0);
    baselineSeq_ = 0;
    pendingTurnId_.clear();
    interactionNotified_ = false;
    sent_ = false;
    done_ = false;
    buffer_.clear();
    stderrTail_.clear();
    nextId_ = 1;
    awaitingId_ = -1;
    stage_ = Stage::Spawn;
    stageClock_.start();
    pollClock_.start();
    process_ = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ELECTRON_RUN_AS_NODE"), QStringLiteral("1"));
    // 宿主桌面端会给自己子进程注入该变量；AgentRMC 从开始菜单/自启起来时没有，
    // 不补的话 CLI 在两个死路径找不到配置直接 exit=1。已有值不覆盖。
    const QString kProviderEnv =
        QStringLiteral("ZCODE_BUILTIN_PROVIDER_CONFIG_FILE");
    if (env.value(kProviderEnv).isEmpty()) {
        const QString bundled = builtinProviderConfigFor(job_.zcodeExe);
        if (!bundled.isEmpty()) {
            env.insert(kProviderEnv, bundled);
        }
    }
    process_->setProcessEnvironment(env);
    process_->setWorkingDirectory(QDir::homePath());
    process_->setProgram(job_.zcodeExe);
    process_->setArguments({cjsPathFor(job_.zcodeExe),
                            QStringLiteral("app-server"), QStringLiteral("--stdio")});
    connect(process_, &QProcess::readyReadStandardOutput, this,
            &ZcodeDispatch::onReadyRead);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        stderrTail_ += process_->readAllStandardError();
        // 只留尾巴：诊断要看的是最后几行，别让长日志吃内存。
        if (stderrTail_.size() > 4096) {
            stderrTail_ = stderrTail_.right(4096);
        }
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (stage_ == Stage::Spawn) {
            fail(QStringLiteral("ZCode app-server 启动失败：%1")
                     .arg(process_->errorString()));
        }
    });
    connect(process_, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
        // finished 触发时管道里可能还有没读完的尾巴：先抽干再报死因。
        stderrTail_ += process_->readAllStandardError();
        if (stderrTail_.size() > 4096) {
            stderrTail_ = stderrTail_.right(4096);
        }
        if (done_) {
            return;
        }
        if (!sent_) {
            // 提前退出：把阶段、退出码和 stderr 尾巴一起报出去。
            // 2026-11-09 实测：只报「提前退出」时完全没法定位——真实死因
            // （如 Protocol input closed）全在 stderr 里被丢掉了。
            QString why = QStringLiteral("ZCode app-server 提前退出（%1阶段，exit=%2%3）")
                              .arg(stageName())
                              .arg(exitCode)
                              .arg(status == QProcess::CrashExit
                                       ? QStringLiteral("，崩溃")
                                       : QString());
            const QString tail = QString::fromUtf8(stderrTail_).trimmed();
            if (!tail.isEmpty()) {
                const QStringList lines = tail.split('\n');
                why += QStringLiteral("：%1").arg(lines.last().trimmed().left(200));
            }
            fail(why);
        } else {
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
            // 默认）。授权/提问在外部派发里没有代答 UI：按协议回明确拒绝，
            // 不能挂在 -32000「不支持」上——模型会把它当成工具自己坏了，而
            // 人也不知道这一轮其实卡在等人。拒绝原因会进模型上下文，整轮照常
            // 收尾上卡；同时发一次提醒，告诉人「要回 ZCode 桌面处理」。
            const QString method = obj.value(QStringLiteral("method")).toString();
            QJsonObject reply;
            if (method == QStringLiteral("session/requestRuntimePreferences")) {
                reply = QJsonObject{{QStringLiteral("result"),
                                     QJsonObject{{QStringLiteral(
                                         "nativeSearchEnhancementsEnabled"),
                                                  false}}}};
            } else if (method ==
                       QStringLiteral("interaction/requestPermission")) {
                reply = QJsonObject{
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("decision"),
                                  QStringLiteral("deny")},
                                 {QStringLiteral("reason"),
                                  QString::fromUtf8(kDenyPermissionReason)}}}};
                if (!interactionNotified_) {
                    interactionNotified_ = true;
                    emit interactionSkipped(job_.dispatchId,
                                            QStringLiteral("授权请求"));
                }
            } else if (method ==
                       QStringLiteral("interaction/requestUserInput")) {
                reply = QJsonObject{
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("action"),
                                  QStringLiteral("decline")},
                                 {QStringLiteral("reason"),
                                  QString::fromUtf8(kDeclineInputReason)}}}};
                if (!interactionNotified_) {
                    interactionNotified_ = true;
                    emit interactionSkipped(job_.dispatchId,
                                            QStringLiteral("提问"));
                }
            } else if (
                method ==
                QStringLiteral("interaction/requestOfficialMcpAuthHeaders")) {
                // 官方 MCP（image-search / zcode-site 等）要桌面登录态的鉴权头，
                // 外部派发实例拿不到（凭据在桌面进程里，用户裁决不破解）。
                // 按协议回明确的 ok:false + 枚举原因，服务端降级为
                // official_auth_unavailable、跳过该 MCP；回 -32000 会被当成
                // 客户端协议错误，日志里全是 auth_request_failed 噪音。
                reply = QJsonObject{
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("ok"), false},
                                 {QStringLiteral("reason"),
                                  QStringLiteral("official_auth_unavailable")}}}};
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
    // 只重置轮询节拍，不碰阶段时钟：阶段超时必须按「进入阶段」起算。
    // 每次轮询都重启阶段时钟＝超时永远不触发（2026-11-08 审查实测）。
    pollClock_.restart();
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
        // 发消息前先记事件序号基线：终态（turn.completed / turn.failed）只能
        // 从基线之后的事件里认，否则历史轮次的终态会把这一轮提前判完。
        stage_ = Stage::Baseline;
        stageClock_.restart();
        request(QStringLiteral("session/events"),
                QJsonObject{{QStringLiteral("sessionId"), job_.sessionId},
                            {QStringLiteral("limit"), 1}});
        return;
    case Stage::Baseline:
        if (!error.isEmpty() || !result.contains(QStringLiteral("events"))) {
            fail(QStringLiteral("读不到 ZCode 会话事件基线：%1")
                     .arg(error.value(QStringLiteral("message")).toString()));
            return;
        }
        for (const QJsonValue &v :
             result.value(QStringLiteral("events")).toArray()) {
            baselineSeq_ = qMax(
                baselineSeq_,
                qint64(v.toObject().value(QStringLiteral("seq")).toDouble()));
        }
        stage_ = Stage::Send;
        stageClock_.restart();
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
    case Stage::Persist: {
        if (!error.isEmpty()) {
            return;  // 读回执偶发失败：下一轮再试。
        }
        const QJsonArray messages = result.value(QStringLiteral("messages")).toArray();
        bool persisted = false;
        for (const QJsonValue &v : messages) {
            if (textOfMessage(v.toObject()).contains(job_.text)) {
                persisted = true;
                break;
            }
        }
        if (!persisted) {
            return;  // 还没落库。
        }
        sent_ = true;
        stage_ = Stage::Turn;
        stageClock_.restart();
        pollClock_.restart();
        emit sent(job_.dispatchId);
        return;
    }
    case Stage::Turn: {
        // 整轮终态只看事件流：首条 assistant 正文常常是「我先检查一下」，
        // 拿它当完成会把人话截断、还会 kill 掉还在跑的 app-server。
        if (!error.isEmpty()) {
            return;
        }
        const QJsonArray events = result.value(QStringLiteral("events")).toArray();
        for (const QJsonValue &v : events) {
            const QJsonObject ev = v.toObject();
            const QString type = ev.value(QStringLiteral("type")).toString();
            const QString turnId = ev.value(QStringLiteral("turnId")).toString();
            const QJsonObject payload = ev.value(QStringLiteral("payload")).toObject();
            if (type == QStringLiteral("turn.started")) {
                // send 在已有活跃轮次时会被拒，所以基线后的第一轮就是我们的。
                if (pendingTurnId_.isEmpty()) {
                    pendingTurnId_ = turnId;
                }
                continue;
            }
            if (pendingTurnId_.isEmpty() || turnId != pendingTurnId_) {
                continue;
            }
            if (type == QStringLiteral("turn.completed")) {
                finishTurn(payload.value(QStringLiteral("response")).toString());
                return;
            }
            if (type == QStringLiteral("turn.failed")) {
                const QString why = payload.value(QStringLiteral("error"))
                                        .toObject()
                                        .value(QStringLiteral("message"))
                                        .toString();
                fail(QStringLiteral("ZCode 轮次失败：%1（消息已送达）")
                         .arg(why.isEmpty() ? QStringLiteral("未知原因") : why));
                return;
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
        if (elapsed > scaled(kResumeTimeoutMs)) {
            fail(QStringLiteral("ZCode resume 超时"));
        }
        return;
    case Stage::Baseline:
        if (elapsed > scaled(kSendTimeoutMs)) {
            fail(QStringLiteral("读取 ZCode 会话事件基线超时"));
            return;
        }
        if (awaitingId_ < 0 && pollClock_.elapsed() >= scaled(kPersistPollMs)) {
            request(QStringLiteral("session/events"),
                    QJsonObject{{QStringLiteral("sessionId"), job_.sessionId},
                                {QStringLiteral("limit"), 1}});
        }
        return;
    case Stage::Send:
        if (elapsed > scaled(kSendTimeoutMs)) {
            fail(QStringLiteral("ZCode send 超时"));
        }
        return;
    case Stage::Persist:
        if (elapsed > scaled(kPersistTimeoutMs)) {
            fail(QStringLiteral("派发消息落库确认超时"));
            return;
        }
        if (awaitingId_ < 0 && pollClock_.elapsed() >= scaled(kPersistPollMs)) {
            request(QStringLiteral("session/messages"),
                    QJsonObject{{QStringLiteral("sessionId"), job_.sessionId}});
        }
        return;
    case Stage::Turn:
        if (elapsed > scaled(kTurnTimeoutMs)) {
            finishTurn(QString());
            return;
        }
        if (awaitingId_ < 0 && pollClock_.elapsed() >= scaled(kTurnPollMs)) {
            request(QStringLiteral("session/events"),
                    QJsonObject{{QStringLiteral("sessionId"), job_.sessionId},
                                {QStringLiteral("afterSeq"), baselineSeq_},
                                {QStringLiteral("limit"), 200}});
        }
        return;
    }
}

qint64 ZcodeDispatch::scaled(qint64 baseMs) const {
    // 测试用时间倍率（AGENTRMC_ZCODE_TIME_SCALE，默认 1）。测试取 0.02：
    // spawn 400ms 够 node 起来，persist 900ms 能跑出多次轮询再撞超时。
    return qMax<qint64>(1, qint64(double(baseMs) * timeScale_));
}

// 阶段名进失败文案：同一句「提前退出」可能死在任何一步，带上阶段才能定位。
QString ZcodeDispatch::stageName() const {
    switch (stage_) {
    case Stage::Spawn:
        return QStringLiteral("启动");
    case Stage::Ready:
        return QStringLiteral("握手");
    case Stage::Resume:
        return QStringLiteral("resume");
    case Stage::Baseline:
        return QStringLiteral("事件基线");
    case Stage::Send:
        return QStringLiteral("send");
    case Stage::Persist:
        return QStringLiteral("落库确认");
    case Stage::Turn:
        return QStringLiteral("等回复");
    }
    return QStringLiteral("?");
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
