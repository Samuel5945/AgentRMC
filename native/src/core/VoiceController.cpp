#include "VoiceController.h"

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

VoiceController::VoiceController(ProfileStore *store, QObject *parent)
    : QObject(parent), store_(store) {
    tick_ = new QTimer(this);
    tick_->setInterval(50);
    connect(tick_, &QTimer::timeout, this, &VoiceController::tick);
    tick_->start();
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
    const QString combo = voiceComboText(options_.hotkeyMods, options_.hotkeyVk);
    return combo + (options_.hotkeyTrigger == QStringLiteral("toggle")
                        ? QStringLiteral(" · 按一下开始/再按一次结束")
                        : QStringLiteral(" · 按住说话"));
}

bool VoiceController::modelReady(QString *detail) {
    updateOptions();
    if (options_.backend == VoiceBackend::Hotkey) {
        if (detail != nullptr) *detail = QStringLiteral("0 模型，转发给前台");
        return true;
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
            *detail = ready ? QStringLiteral("模型就绪") : QStringLiteral("未找到 SenseVoice 模型");
        }
        return ready;
    }
    const bool ready = qwen3Ready(options_.qwen3Dir, nullptr, nullptr, nullptr, nullptr);
    if (detail != nullptr) {
        *detail = ready ? QStringLiteral("模型就绪") : QStringLiteral("未找到 Qwen3-ASR 模型");
    }
    return ready;
#endif
}

QString VoiceController::statusText() {
    updateOptions();
    if (options_.backend == VoiceBackend::Hotkey) {
        return hotkeyHint();
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
    if (!test_ && !mediaStart_ && now - lastVoiceAt_ > options_.silenceStopMs) {
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
        if (!media && activeTrigger_ == QStringLiteral("toggle")) {
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
                 holdCombo_ ? QStringLiteral("语音中 · 松手结束")
                            : QStringLiteral("语音中 · 再按一次结束"));
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

    int capturedMs = 0;
    const QByteArray pcm = recorder_.stopCapture(&capturedMs);
    state_ = State::Idle;
    beep(760, 70, !test_);
    const bool tooShort = !test_ && capturedMs < options_.minRecordMs;
    if (pcm.isEmpty() || tooShort) {
        test_ = false;
        setState(State::Idle, QStringLiteral("录音太短，已忽略"));
        return;
    }
    test_ = false;
    setState(State::Idle, QStringLiteral("转写中…"));
    emit eventReady(QStringLiteral("voice"),
                    QStringLiteral("录音 %1s · 转写中…")
                        .arg(capturedMs / 1000.0, 0, 'f', 1));
    runTranscription(pcm);
}

void VoiceController::startTest() {
    if (state_ != State::Idle || transcribing_) {
        return;
    }
    updateOptions();
    if (options_.backend == VoiceBackend::Hotkey) {
        emit eventReady(QStringLiteral("error"),
                        QStringLiteral("当前是 Qoder 热键模式；先切到本地模型再测试"));
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
    emit eventReady(QStringLiteral("voice"), QStringLiteral("测试录音 3 秒…"));
    setState(State::Recording, QStringLiteral("测试录音 3 秒…"));
    QTimer::singleShot(3000, this, [this] {
        if (test_ && state_ == State::Recording) {
            finishSession();
        }
    });
}

void VoiceController::runTranscription(const QByteArray &pcm) {
    transcribing_ = true;
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
                engineDir_ != config.modelDir) {
                engine_ = createAsrEngine(config);
                engineFamily_ = config.family;
                engineDir_ = config.modelDir;
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
                        QStringLiteral("没有识别到内容"));
        setState(State::Idle, QStringLiteral("就绪"));
        return;
    }
    lastText_ = text;
    bool pasted = false;
    if (options_.paste) {
        pasted = injector_.pasteText(text, options_.autoEnter);
    } else {
        injector_.copyText(text);
    }
    emit transcribed(text, costMs);
    setState(State::Idle,
             pasted ? QStringLiteral("已粘贴 · %1s").arg(costMs / 1000.0, 0, 'f', 1)
                    : QStringLiteral("已识别（未粘贴）"));
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
