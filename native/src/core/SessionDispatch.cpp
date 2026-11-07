#include "core/SessionDispatch.h"

#include "core/UiDispatch.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QUuid>

namespace {

constexpr qint64 kActivityGuardMs = 10000;
constexpr qint64 kReceiptTimeoutMs = 120000;

}  // namespace

SessionDispatch::SessionDispatch(QObject *parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &SessionDispatch::poll);
    ui_ = new UiDispatch(this);
    connect(ui_, &UiDispatch::sent, this, [this](const QString &) {
        // 焦点依赖段结束：放行下一条 UI 派发；回执继续由 poll 盯。
        uiBusy_ = false;
    });
    const auto uiFail = [this](const QString &dispatchId, const QString &reason) {
        uiBusy_ = false;
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->dispatchId == dispatchId) {
                if (it->process != nullptr) {
                    it->process->deleteLater();
                }
                pending_.erase(it);
                break;
            }
        }
        emit failed(dispatchId, reason);
    };
    connect(ui_, &UiDispatch::failed, this, uiFail);
    connect(ui_, &UiDispatch::proofFailed, this, uiFail);
}

QString SessionDispatch::configHomeFor(const QString &agentExe) {
    // 会话库位置（找目标+回执）。Qoder 系留空＝通道休眠：CLI --resume 实测
    // 送达+回执但用户 2026-11-07 否决（桌面端毫无反应，人回不去），等桌面
    // 可见方案探明再启用；WorkBuddy 系走 UiDispatch 桌面可见通道。
    if (agentExe.compare(QStringLiteral("WorkBuddy.exe"), Qt::CaseInsensitive) == 0) {
        return QDir::homePath() + QStringLiteral("/.workbuddy");
    }
    if (agentExe.compare(QStringLiteral("WorkBuddyAI.exe"), Qt::CaseInsensitive) == 0) {
        return QDir::homePath() + QStringLiteral("/.workbuddy-ai");
    }
    return QString();
}

QString SessionDispatch::cliPathFor(const QString &configHome) {
    if (configHome.endsWith(QStringLiteral("/.qoder-cn"))) {
        return configHome + QStringLiteral("/bin/qoderclicn/qoderclicn.exe");
    }
    return configHome + QStringLiteral("/bin/qodercli/qodercli.exe");
}

bool SessionDispatch::userTextOf(const QByteArray &line, QString *text) {
    const QJsonObject obj = QJsonDocument::fromJson(line).object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    const QJsonObject msg = obj.value(QStringLiteral("message")).toObject();
    // Qoder 系记 type=user、role/content 在 message 里；WorkBuddy 系记
    // type=message、role/content 在顶层，正文 part 叫 input_text。
    const QString role = msg.value(QStringLiteral("role")).toString().isEmpty()
                             ? obj.value(QStringLiteral("role")).toString()
                             : msg.value(QStringLiteral("role")).toString();
    const bool isUser = type == QStringLiteral("user") ||
                        (type == QStringLiteral("message") &&
                         role == QStringLiteral("user"));
    if (!isUser) {
        return false;
    }
    QJsonValue content = msg.value(QStringLiteral("content"));
    if (content.isUndefined() || content.isNull()) {
        content = obj.value(QStringLiteral("content"));
    }
    if (content.isString()) {
        *text = content.toString();
        return true;
    }
    if (content.isArray()) {
        QString joined;
        for (const QJsonValue &part : content.toArray()) {
            const QJsonObject o = part.toObject();
            const QString pt = o.value(QStringLiteral("type")).toString();
            if (pt == QStringLiteral("text") || pt == QStringLiteral("input_text")) {
                joined += o.value(QStringLiteral("text")).toString();
            }
        }
        *text = joined;
        return true;
    }
    return false;
}

QString SessionDispatch::lastReply(const QString &configHome,
                                   const QString &sessionId, int maxChars) {
    QString path, cwd;
    if (!findSession(configHome, sessionId, &path, &cwd)) {
        return QString();
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    QString last;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        const QString type = obj.value(QStringLiteral("type")).toString();
        const QJsonObject msg = obj.value(QStringLiteral("message")).toObject();
        const QString role = msg.value(QStringLiteral("role")).toString().isEmpty()
                                 ? obj.value(QStringLiteral("role")).toString()
                                 : msg.value(QStringLiteral("role")).toString();
        const bool isAssistant =
            type == QStringLiteral("assistant") ||
            (type == QStringLiteral("message") && role == QStringLiteral("assistant"));
        if (!isAssistant) {
            continue;
        }
        QJsonValue content = msg.value(QStringLiteral("content"));
        if (content.isUndefined() || content.isNull()) {
            content = obj.value(QStringLiteral("content"));
        }
        QString joined;
        if (content.isString()) {
            joined = content.toString();
        } else if (content.isArray()) {
            for (const QJsonValue &part : content.toArray()) {
                const QJsonObject o = part.toObject();
                const QString pt = o.value(QStringLiteral("type")).toString();
                if (pt == QStringLiteral("text") || pt == QStringLiteral("input_text") ||
                    pt == QStringLiteral("output_text")) {
                    joined += o.value(QStringLiteral("text")).toString();
                }
            }
        }
        static const QRegularExpression reminder(
            QStringLiteral("<system-reminder[\\s\\S]*?</system-reminder>"));
        static const QRegularExpression queryTag(QStringLiteral("</?user_query>"));
        joined.remove(reminder);
        joined.remove(queryTag);
        joined = joined.simplified();
        if (!joined.isEmpty()) {
            last = joined;
        }
    }
    if (last.size() > maxChars) {
        last = last.left(maxChars) + QStringLiteral("…");
    }
    return last;
}

bool SessionDispatch::findSession(const QString &configHome,
                                  const QString &sessionId, QString *path,
                                  QString *cwd, QString *title,
                                  QString *altTitle) {
    QDir projects(configHome + QStringLiteral("/projects"));
    const QStringList slugs = projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &slug : slugs) {
        const QString file = projects.filePath(slug + QLatin1Char('/') + sessionId +
                                               QStringLiteral(".jsonl"));
        if (!QFileInfo::exists(file)) {
            continue;
        }
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        // 自报身份才算数：记录里的 sessionId 必须等于目标 id。
        // 头部 workspace-directories / runtime-config 有 id 没 cwd，必须继续
        // 扫到带 cwd 的记录（实测停在第一条会让 CLI 继承错误 cwd 报 rc=42）。
        bool found = false;
        QString firstUser, aiTitle;
        for (int i = 0; i < 200 && !f.atEnd(); ++i) {
            const QByteArray line = f.readLine();
            const QJsonObject obj = QJsonDocument::fromJson(line).object();
            if (obj.value(QStringLiteral("sessionId")).toString() != sessionId) {
                continue;
            }
            found = true;
            *path = file;
            const QString type = obj.value(QStringLiteral("type")).toString();
            if (cwd->isEmpty()) {
                *cwd = obj.value(QStringLiteral("cwd")).toString();
            }
            if (cwd->isEmpty() && type == QStringLiteral("workspace-directories")) {
                const QJsonArray dirs =
                    obj.value(QStringLiteral("directories")).toArray();
                if (!dirs.isEmpty()) {
                    *cwd = dirs.first().toString();
                }
            }
            if (type == QStringLiteral("ai-title")) {
                aiTitle = obj.value(QStringLiteral("aiTitle")).toString();
            } else if (firstUser.isEmpty()) {
                // userTextOf 认两种记法；WorkBuddy 正文前带 system-reminder
                // 包装，桌面标题/气泡显示的是剥掉包装后的正文。
                QString raw;
                if (userTextOf(line, &raw)) {
                    static const QRegularExpression reminder(
                        QStringLiteral("<system-reminder[\\s\\S]*?</system-reminder>"));
                    static const QRegularExpression queryTag(
                        QStringLiteral("</?user_query>"));
                    raw.remove(reminder);
                    raw.remove(queryTag);
                    firstUser = raw.trimmed();
                }
            }
        }
        if (found && !cwd->isEmpty()) {
            if (title != nullptr) {
                *title = aiTitle.isEmpty() ? firstUser : aiTitle;
            }
            if (altTitle != nullptr) {
                *altTitle = aiTitle.isEmpty() ? QString() : firstUser;
            }
            return true;
        }
        if (found) {
            // 有身份没工作目录：证明不了该在哪个项目里操作，不发。
            path->clear();
            cwd->clear();
        }
        return false;
    }
    return false;
}

bool SessionDispatch::dispatch(const QString &agentExe, const QString &sessionId,
                               const QString &project, const QString &text,
                               QString *reason, QString *dispatchIdOut) {
    const auto refuse = [reason](const QString &why) {
        *reason = why;
        return false;
    };
    if (sessionId.trimmed().isEmpty()) {
        return refuse(QStringLiteral("待办未关联会话，不能定向派发"));
    }
    const QString home = configHomeFor(agentExe);
    if (home.isEmpty()) {
        return refuse(QStringLiteral("该宿主未接定向派发（当前仅 WorkBuddy 系）"));
    }
    QString path, cwd, title, altTitle;
    if (!findSession(home, sessionId, &path, &cwd, &title, &altTitle)) {
        return refuse(QStringLiteral("目标会话不在该宿主会话库，证明不了归属，未发"));
    }
    if (pending_.contains(sessionId)) {
        return refuse(QStringLiteral("该会话已有派发在途，等回执后再派"));
    }
    const qint64 idle = QFileInfo(path).lastModified().msecsTo(
        QDateTime::currentDateTime());
    if (idle < kActivityGuardMs) {
        return refuse(QStringLiteral("目标会话 %1 秒内仍有写入，可能正在跑，稍后再派")
                          .arg(kActivityGuardMs / 1000));
    }

    Pending p;
    p.dispatchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.agentExe = agentExe;
    p.sessionId = sessionId;
    p.project = project;
    p.text = text;
    p.path = path;
    p.offset = QFileInfo(path).size();
    p.clock.start();
    if (dispatchIdOut != nullptr) {
        *dispatchIdOut = p.dispatchId;
    }

    if (UiDispatch::supports(agentExe)) {
        if (uiBusy_ || ui_->busy()) {
            return refuse(QStringLiteral("已有 UI 派发在途（依赖前台，全局串行）"));
        }
        uiBusy_ = true;
        pending_.insert(sessionId, p);
        if (!timer_->isActive()) {
            timer_->start();
        }
        UiDispatch::Job job;
        job.dispatchId = p.dispatchId;
        job.exe = agentExe;
        job.sessionId = sessionId;
        job.title = title;
        job.altTitle = altTitle;
        job.groupHint = cwd.isEmpty() ? QString() : QDir(cwd).dirName();
        job.text = text;
        ui_->start(job);
        return true;
    }

    const QString cli = cliPathFor(home);
    if (!QFileInfo::exists(cli)) {
        return refuse(QStringLiteral("找不到 Qoder CLI：%1").arg(cli));
    }
    p.process = new QProcess(this);
    // 桌面派生的环境带 SDK entrypoint 会让 CLI 拒启动（sdk_invalid_args）。
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char *key : {"QODER_AGENT_SDK_ENTRYPOINT", "QODER_AGENT_SDK_DAEMON",
                            "QODER_AGENT_SDK_VERSION"}) {
        env.remove(QString::fromLatin1(key));
    }
    p.process->setProcessEnvironment(env);
    if (!cwd.isEmpty()) {
        p.process->setWorkingDirectory(cwd);
    }
    p.process->setProgram(cli);
    // --tools 是变长参数，prompt 必须跟在 -p 后面才不会被吞（实测 rc=42）。
    p.process->setArguments({QStringLiteral("--resume"), sessionId,
                             QStringLiteral("-p"), text});
    const QString sid = sessionId;
    connect(p.process, &QProcess::finished, this,
            [this, sid](int code, QProcess::ExitStatus status) {
                auto it = pending_.find(sid);
                if (it == pending_.end()) {
                    return;
                }
                if (status == QProcess::CrashExit || code != 0) {
                    const QString err = QString::fromUtf8(
                        it->process->readAllStandardError()).right(200);
                    const QString id = it->dispatchId;
                    it->process->deleteLater();
                    pending_.erase(it);
                    emit failed(id, QStringLiteral("CLI 退出码 %1：%2").arg(code).arg(err));
                }
            });
    p.process->start();
    if (!p.process->waitForStarted(3000)) {
        p.process->deleteLater();
        return refuse(QStringLiteral("CLI 启动失败：%1").arg(p.process->errorString()));
    }
    pending_.insert(sessionId, p);
    if (!timer_->isActive()) {
        timer_->start();
    }
    return true;
}

void SessionDispatch::poll() {
    const QStringList keys = pending_.keys();
    for (const QString &sid : keys) {
        auto it = pending_.find(sid);
        if (it == pending_.end()) {
            continue;
        }
        Pending p = it.value();
        bool got = false;
        bool timedOut = false;
        QFile f(p.path);
        if (f.open(QIODevice::ReadOnly)) {
            f.seek(p.offset);
            const QByteArray chunk = f.readAll();
            const qsizetype cut = chunk.lastIndexOf('\n');
            if (cut >= 0) {
                // 只推进到完整行边界，半行留到下一轮。
                it->offset += cut + 1;
                for (const QByteArray &line : chunk.left(cut).split('\n')) {
                    QString text;
                    if (userTextOf(line, &text) && text.contains(p.text)) {
                        got = true;
                        break;
                    }
                }
            }
        }
        if (!got && p.clock.elapsed() > kReceiptTimeoutMs) {
            timedOut = true;
        }
        if (got || timedOut) {
            if (p.process != nullptr) {
                p.process->deleteLater();
            }
            pending_.erase(it);
            if (got) {
                emit receipt(p.dispatchId, p.agentExe, p.sessionId, p.project, p.text);
            } else {
                emit failed(p.dispatchId,
                            QStringLiteral("回执超时（CLI 进程可能仍在跑）"));
            }
        }
    }
    if (pending_.isEmpty()) {
        timer_->stop();
    }
}
