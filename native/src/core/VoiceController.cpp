#include "VoiceController.h"

#include "core/AppLauncher.h"
#include "core/AsrClient.h"
#include "core/AsrEngine.h"
#include "core/Profiles.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QThreadPool>
#include <QTimer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// 耳机麦克风的可说话峰值（RMS，约 -34 dBFS），高于它算还在说话。
constexpr double kMediaVoiceFloor = 0.02;

}  // namespace

namespace {

// 只要前台不是本程序，就直接粘贴（不再判断是不是输入框）。
bool foregroundIsOtherApp() {
    HWND fg = GetForegroundWindow();
    if (fg == nullptr) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid != 0 && pid != GetCurrentProcessId();
}

}  // namespace

VoiceController::VoiceController(ProfileStore *store, QObject *parent)
    : QObject(parent), store_(store) {
    tick_ = new QTimer(this);
    tick_->setInterval(50);
    connect(tick_, &QTimer::timeout, this, &VoiceController::tick);
    tick_->start();

    // 云端 ASR 路线：结果和本地路线一样汇进 deliver()。
    asr_ = new AsrClient(this);
    connect(asr_, &AsrClient::transcribed, this,
            [this](const QString &text, qint64 costMs) {
                if (alive_.load()) {
                    deliver(text, int(costMs), QString());
                }
            });
    connect(asr_, &AsrClient::failed, this,
            [this](const QString &error, qint64 costMs) {
                if (alive_.load()) {
                    deliver(QString(), int(costMs), error);
                }
            });
}

VoiceController::~VoiceController() {
    alive_.store(false);
    recorder_.close();
    QThreadPool::globalInstance()->waitForDone(10000);
    std::lock_guard<std::mutex> lock(engineMutex_);
    engine_.reset();
}

QString VoiceController::appDir() const {
    return QCoreApplication::applicationDirPath();
}

void VoiceController::updateOptions() {
    options_ = resolveVoiceOptions(store_->config(), store_->current(true).voice,
                                   appDir());
}

VoiceOptions VoiceController::options() {
    updateOptions();
    return options_;
}

QString VoiceController::backendLabel() {
    updateOptions();
    return voiceBackendLabel(options_.backend);
}

QString VoiceController::hotkeyHint() {
    updateOptions();
    const Profile &profile = store_->current(true);
    const QString combo = voiceComboText(options_.hotkeyMods, options_.hotkeyVk);
    QString text =
        voiceProfileLabel(profile.name) + QStringLiteral(" · ") + combo +
        (options_.hotkeyTrigger == QStringLiteral("toggle")
             ? QStringLiteral(" · 按一下开始/再按一次结束")
             : QStringLiteral(" · 按住说话"));
    if (profile.voice.isEmpty()) {
        text += QString::fromUtf8(" · 这个场景没配语音快捷键，应用不响应时改用本地识别");
    }
    return text;
}

bool VoiceController::modelReady(QString *detail) {
    updateOptions();
    if (options_.backend == VoiceBackend::Hotkey) {
        if (detail != nullptr) *detail = QStringLiteral("0 模型，转发给前台");
        return true;
    }
    if (options_.backend == VoiceBackend::CloudAsr) {
        // 云端路线不依赖本地运行库，key/model 配好就能用。
        const bool ready =
            !options_.asrApiKey.isEmpty() && !options_.asrApiModel.isEmpty();
        if (detail != nullptr) {
            *detail = ready
                          ? QStringLiteral("云端模型 %1 · 音频将上传")
                                .arg(options_.asrApiModel)
                          : QStringLiteral("云端 ASR 还没配置："
                                           "语音设置 → 云端 ASR 填模型和 Key");
        }
        return ready;
    }
#ifndef AGENTRMC_WITH_SHERPA
    if (detail != nullptr) {
        *detail = QStringLiteral("当前构建未包含 sherpa-onnx 运行库");
    }
    return false;
#else
    if (options_.backend == VoiceBackend::SenseVoice) {
        QString reason;
        const bool ready = senseVoiceReady(options_.senseVoiceDir, nullptr, nullptr);
        if (detail != nullptr) {
            *detail = ready ? QStringLiteral("模型就绪")
                            : QStringLiteral("还没装 SenseVoice 模型："
                                             "打开「模型设置 → 导入模型包…」装一下");
        }
        return ready;
    }
    const bool ready = qwen3Ready(options_.qwen3Dir, nullptr, nullptr, nullptr, nullptr);
    if (detail != nullptr) {
        *detail = ready ? QStringLiteral("模型就绪")
                        : QStringLiteral("还没装 Qwen3-ASR 模型："
                                         "打开「模型设置 → 导入模型包…」装一下");
    }
    return ready;
#endif
}

QString VoiceController::statusText() {
    updateOptions();
    if (options_.backend == VoiceBackend::Hotkey) {
        return hotkeyHint();
    }
    if (options_.backend == VoiceBackend::CloudAsr) {
        QString detail;
        const bool ready = modelReady(&detail);
        return QStringLiteral("云端 ASR · %1")
            .arg(ready ? options_.asrApiModel : detail);
    }
    QString detail;
    const bool ready = modelReady(&detail);
    const QString name = options_.backend == VoiceBackend::Qwen3Asr
                             ? QStringLiteral("Qwen3-ASR")
                             : QStringLiteral("SenseVoice");
    return name + QStringLiteral(" · ") + (ready ? QStringLiteral("模型就绪")
                                                  : detail);
}

void VoiceController::voiceDown() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (state_ == State::Recording) {
        lastVoiceAt_ = now;
        return;
    }
    if (state_ == State::Idle) {
        updateOptions();
        state_ = State::Pending;
        pendingAt_ = now;
        repeats_ = 1;
        return;
    }
    ++repeats_;
    updateOptions();
    if (repeats_ >= options_.minRepeats) {
        startSession(false);
    }
}

void VoiceController::voiceUp() {
    if (state_ == State::Pending) {
        setState(State::Idle, QStringLiteral("就绪"));
        return;
    }
    if (state_ == State::Recording) {
        lastVoiceAt_ = QDateTime::currentMSecsSinceEpoch();
        finishSession();
    }
}

void VoiceController::mediaToggle() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastMediaAt_ < 350) {
        return;
    }
    lastMediaAt_ = now;
    if (state_ == State::Recording) {
        finishSession();
        return;
    }
    if (state_ == State::Pending) {
        state_ = State::Idle;
    }
    startSession(true);
}

void VoiceController::tick() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (state_ == State::Pending && now - pendingAt_ > options_.confirmWindowMs) {
        setState(State::Idle, QStringLiteral("就绪"));
        return;
    }
    if (state_ != State::Recording) {
        return;
    }
    if (now - recordAt_ > options_.maxRecordMs) {
        finishSession();
        return;
    }
    if (test_) {
        return;
    }
    if (mediaStart_) {
        // 耳机只送 keydown、没有 keyup，结束时机得自己找：能拿到麦克风能量就按停嘴判定，
        // 否则（热键模式且麦克风没开成）只能按时释放组合键，免得 Ctrl+Shift 一直按住。
        const bool hasLevel = recorder_.isOpen();
        if (hasLevel) {
            if (recorder_.level() >= kMediaVoiceFloor) {
                lastVoiceAt_ = now;
            }
            if (now - lastVoiceAt_ > options_.mediaSilenceMs) {
                finishSession();
                return;
            }
        }
        if (options_.backend == VoiceBackend::Hotkey &&
            now - recordAt_ > options_.mediaAutoStopMs) {
            finishSession();
        }
        return;
    }
    if (now - lastVoiceAt_ > options_.silenceStopMs) {
        finishSession();
    }
}

void VoiceController::startSession(bool media) {
    updateOptions();
    mediaStart_ = media;
    test_ = false;
    toggleCombo_ = false;
    holdCombo_ = false;
    recordAt_ = QDateTime::currentMSecsSinceEpoch();
    lastVoiceAt_ = recordAt_;
    activeMods_ = options_.hotkeyMods;
    activeVk_ = options_.hotkeyVk;
    activeTrigger_ = options_.hotkeyTrigger;

    if (options_.backend == VoiceBackend::Hotkey) {
        if (activeTrigger_ == QStringLiteral("toggle")) {
            injector_.key(activeMods_, activeVk_);
            toggleCombo_ = true;
        } else {
            injector_.keyDown(activeMods_, activeVk_);
            holdCombo_ = true;
        }
        state_ = State::Recording;
        const QString combo = voiceComboText(activeMods_, activeVk_);
        emit eventReady(QStringLiteral("voice"),
                        QStringLiteral("语音快捷键 %1（%2）")
                            .arg(combo,
                                 holdCombo_ ? QStringLiteral("按住")
                                            : QStringLiteral("开关")));
        beep(1180, 60, !test_);
        setState(State::Recording,
                 mediaStart_ && options_.backend != VoiceBackend::Hotkey
                     ? QStringLiteral("语音中 · 再按一次或停嘴结束")
                     : (holdCombo_ ? QStringLiteral("语音中 · 松手结束")
                                   : QStringLiteral("语音中 · 再按一次结束")));
        return;
    }

    QString error;
    if (!modelReady(&error)) {
        emit eventReady(QStringLiteral("error"), error);
        setState(State::Idle, QStringLiteral("就绪"));
        return;
    }
    if (!recorder_.isOpen() && !recorder_.open(&error)) {
        emit eventReady(QStringLiteral("error"), error);
        setState(State::Idle, QStringLiteral("就绪"));
        return;
    }
    recorder_.startCapture();
    state_ = State::Recording;
    beep(1180, 70, !test_);
    setState(State::Recording, QStringLiteral("录音中… 松手转写"));
}

void VoiceController::finishSession() {
    if (state_ != State::Recording) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int durationMs = int(now - recordAt_);

    if (options_.backend == VoiceBackend::Hotkey) {
        if (toggleCombo_) {
            const int delay = qMax(0, 300 - durationMs);
            const QStringList mods = activeMods_;
            const int vk = activeVk_;
            QTimer::singleShot(delay, this, [this, mods, vk] {
                if (alive_.load()) {
                    injector_.key(mods, vk);
                }
            });
        } else if (holdCombo_) {
            injector_.keyUp(activeMods_, activeVk_);
        }
        toggleCombo_ = false;
        holdCombo_ = false;
        state_ = State::Idle;
        beep(760, 70, !test_);
        setState(State::Idle, QStringLiteral("语音快捷键已结束"));
        return;
    }

    const QByteArray pcm = recorder_.stopCapture();
    state_ = State::Idle;
    beep(760, 70, !test_);
    // 录音等按键重复（约 500ms）才开始记，按住的前一段靠 preroll 补回来，
    // 所以判定和展示都用真实音频时长，不能用墙钟——不然正常的快说一句
    // 会被误报「采样太短」。
    const int audioMs = int(pcm.size()) / 32;  // 16k 单声道 16bit = 32 字节/毫秒
    const bool tooShort = !test_ && audioMs < options_.minRecordMs;
    if (pcm.isEmpty() || tooShort) {
        test_ = false;
        setState(State::Idle, QStringLiteral("采样太短，按住久一点再说"));
        return;
    }
    test_ = false;
    setState(State::Idle, QStringLiteral("转写中…"));
    emit eventReady(QStringLiteral("voice"),
                    QStringLiteral("录音 %1s · 转写中…")
                        .arg(audioMs / 1000.0, 0, 'f', 1));
    runTranscription(pcm);
}

void VoiceController::startTest() {
    if (state_ != State::Idle || transcribing_) {
        return;
    }
    updateOptions();
    if (options_.backend == VoiceBackend::Hotkey) {
        emit eventReady(QStringLiteral("error"),
                        QStringLiteral("现在是转发模式，没有本地模型可测：切到本地识别后再试"));
        return;
    }
    QString error;
    if (!modelReady(&error)) {
        emit eventReady(QStringLiteral("error"), error);
        return;
    }
    if (!recorder_.isOpen()) {
        if (!recorder_.open(&error)) {
            emit eventReady(QStringLiteral("error"), error);
            return;
        }
        emit eventReady(QStringLiteral("voice"),
                        QStringLiteral("麦克风启动中，约 1 秒后开始录音…"));
        setState(State::Idle, QStringLiteral("麦克风启动中…"));
        QTimer::singleShot(900, this, [this] {
            if (alive_.load()) {
                beginTestCapture();
            }
        });
        return;
    }
    beginTestCapture();
}

void VoiceController::beginTestCapture() {
    if (state_ != State::Idle || transcribing_) {
        return;
    }
    updateOptions();
    if (options_.backend == VoiceBackend::Hotkey || !recorder_.isOpen()) {
        return;
    }
    test_ = true;
    mediaStart_ = true;
    recordAt_ = QDateTime::currentMSecsSinceEpoch();
    lastVoiceAt_ = recordAt_;
    recorder_.startCapture();
    state_ = State::Recording;
    emit eventReady(QStringLiteral("voice"), QStringLiteral("试一句：现在说句话（录 3 秒）"));
    setState(State::Recording, QStringLiteral("试一句：现在说句话（录 3 秒）"));
    QTimer::singleShot(3000, this, [this] {
        if (test_ && state_ == State::Recording) {
            finishSession();
        }
    });
}

void VoiceController::runTranscription(const QByteArray &pcm) {
    transcribing_ = true;
    updateOptions();
    if (options_.backend == VoiceBackend::CloudAsr) {
        runCloudTranscription(pcm);
        return;
    }
    AsrConfig config;
    config.family = options_.backend == VoiceBackend::Qwen3Asr
                        ? QStringLiteral("qwen3")
                        : QStringLiteral("sensevoice");
    config.modelDir = options_.backend == VoiceBackend::Qwen3Asr
                          ? options_.qwen3Dir
                          : options_.senseVoiceDir;
    config.language = options_.language;
    config.threads = options_.threads;

    QThreadPool::globalInstance()->start([this, pcm, config] {
        QElapsedTimer timer;
        timer.start();
        QString text;
        QString error;
        {
            std::lock_guard<std::mutex> lock(engineMutex_);
            if (!engine_ || engineFamily_ != config.family ||
                engineDir_ != config.modelDir ||
                engineLang_ != config.language) {
                engine_ = createAsrEngine(config);
                engineFamily_ = config.family;
                engineDir_ = config.modelDir;
                engineLang_ = config.language;
            }
            text = engine_->transcribe(pcm, 16000, &error);
        }
        const int costMs = int(timer.elapsed());
        QMetaObject::invokeMethod(
            this,
            [this, text, error, costMs] {
                if (alive_.load()) {
                    deliver(text, costMs, error);
                }
            },
            Qt::QueuedConnection);
    });
}

void VoiceController::runCloudTranscription(const QByteArray &pcm) {
    asr_->setOptions(options_.asrApiBaseUrl, options_.asrApiKey,
                     options_.asrApiModel, options_.language,
                     options_.asrApiStyle);
    if (!asr_->ready()) {
        transcribing_ = false;
        emit eventReady(QStringLiteral("error"),
                        QStringLiteral("云端 ASR 还没配置："
                                       "语音设置 → 云端 ASR 填模型和 Key"));
        setState(State::Idle, QStringLiteral("就绪"));
        return;
    }
    const QByteArray wav = wavFromPcm16(pcm, options_.sampleRate);
    emit eventReady(QStringLiteral("voice"),
                    QStringLiteral("已上传 %1 KB · 云端转写中…")
                        .arg(AsrClient::sizeHint(wav)));
    asr_->transcribe(wav);
}

void VoiceController::setChatMode(bool on) {
    chatMode_ = on;
    if (!on && state_ == State::Idle) {
        setState(State::Idle, QStringLiteral("就绪"));
    }
}

void VoiceController::deliver(const QString &text, int costMs,
                              const QString &error) {
    transcribing_ = false;
    updateOptions();
    if (!error.isEmpty()) {
        emit eventReady(QStringLiteral("error"),
                        QStringLiteral("转写失败：") + error);
        setState(State::Idle, QStringLiteral("就绪"));
        return;
    }
    if (text.isEmpty()) {
        emit eventReady(QStringLiteral("voice"),
                        QStringLiteral("没听清：按住说完整一句再松手"));
        setState(State::Idle, QStringLiteral("就绪"));
        return;
    }
    // 桌宠对话模式：「打开 xx」照样直接启动（省一次 LLM 往返），确认话术
    // 通过状态文案让桌宠念出来；其余的话才交给 LLM 聊。
    if (chatMode_) {
        emit transcribed(text, costMs);
        const AppLaunchResult chatApp = store_->uiBool("voice_launch", true)
                                            ? AppLauncher::runVoice(text)
                                            : AppLaunchResult{};
        if (chatApp.matched) {
            setState(State::Idle, chatApp.ok ? QStringLiteral("已打开 ") + chatApp.name
                                             : chatApp.error);
            return;
        }
        emit chatRequested(text);
        setState(State::Idle, QStringLiteral("桌宠对话中…"));
        return;
    }
    lastText_ = text;
    // 「打开剪映」这类话先当启动指令用，命中就不再往输入框里贴字。
    const AppLaunchResult app = store_->uiBool("voice_launch", true)
                                    ? AppLauncher::runVoice(text)
                                    : AppLaunchResult{};
    if (app.matched) {
        emit transcribed(text, costMs);
        if (app.ok) {
            emit eventReady(QStringLiteral("voice"),
                            QStringLiteral("已打开 ") + app.name);
            setState(State::Idle, QStringLiteral("已打开 ") + app.name);
        } else {
            emit eventReady(QStringLiteral("error"), app.error);
            setState(State::Idle, app.error);
        }
        return;
    }
    // 先无条件写进剪贴板；只有前台正落在输入框里才自动粘贴。
    injector_.copyText(text);
    bool pasted = false;
    if (options_.paste && foregroundIsOtherApp()) {
        pasted = injector_.pasteText(text, options_.autoEnter);
    }
    emit transcribed(text, costMs);
    setState(State::Idle,
             pasted ? QStringLiteral("已粘贴 · %1s").arg(costMs / 1000.0, 0, 'f', 1)
                    : QStringLiteral("已复制到剪贴板 · %1s")
                          .arg(costMs / 1000.0, 0, 'f', 1));
}

void VoiceController::setState(State state, const QString &detail) {
    state_ = state;
    const QString name = state == State::Recording
                             ? QStringLiteral("recording")
                             : (state == State::Pending ? QStringLiteral("pending")
                                                        : QStringLiteral("idle"));
    emit stateChanged(name, detail);
}

void VoiceController::beep(int frequency, int durationMs, bool enabled) {
    if (enabled) {
        Beep(DWORD(frequency), DWORD(durationMs));
    }
}

void VoiceController::reload() {
    updateOptions();
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        engine_.reset();
        engineFamily_.clear();
        engineDir_.clear();
    }
    if (listening_ && options_.backend != VoiceBackend::Hotkey) {
        if (!recorder_.isOpen()) {
            QString error;
            if (!recorder_.open(&error)) {
                emit eventReady(QStringLiteral("error"), error);
            }
        }
    } else {
        recorder_.close();
    }
    setState(State::Idle, QStringLiteral("就绪"));
    emit optionsChanged();
}

void VoiceController::setListening(bool listening) {
    listening_ = listening;
    updateOptions();
    if (!listening) {
        if (state_ == State::Recording) {
            test_ = false;
            setState(State::Idle, QStringLiteral("已停止监听"));
        }
        recorder_.close();
        return;
    }
    if (options_.backend != VoiceBackend::Hotkey && !recorder_.isOpen()) {
        QString error;
        if (!recorder_.open(&error)) {
            emit eventReady(QStringLiteral("error"), error);
        }
    }
}
