#include "VoiceController.h"

#include "core/AppLauncher.h"
#include "core/AsrClient.h"
#include "core/AsrEngine.h"
#include "core/Profiles.h"
#include "core/SystemControl.h"

#include <QJsonArray>
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

// 二次确认窗口。要把 TTS 念确认口令那一两秒算进去，否则用户还没听完就超时了。
constexpr qint64 kConfirmWindowMs = 20000;

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

// 非蓝牙麦克风常开，换取 preroll 补回按键判定期间的开头几百毫秒；
// 蓝牙麦克风绝不常开（会占死耳机通道），startSession 里有「没开就临时开」的分支。
// 判的是 chooseMicDevice 这次真要打开的那台设备（用户点名的，或默认通信录音
// 端点），不是 MME 默认录制设备——两者在装了 Steam 虚拟麦的机器上会不一致；
// 判据和实际打开必须走同一个裁决，否则会出现「以为常开安全、开的却是耳机麦」。
bool VoiceController::shouldHoldMic() {
    const MicChoice choice = AudioRecorder::chooseMicDevice(
        pinnedMicName(), AudioRecorder::communicationsDeviceName(),
        AudioRecorder::waveInNames());
    return !choice.bluetooth;
}

QString VoiceController::pinnedMicName() const {
    const QString byName = options_.micDevice.trimmed();
    if (!byName.isEmpty()) {
        return byName;
    }
    // 老配置的数字序号形态：按当前设备列表换成名字，后面判蓝牙、开麦都只认名字。
    if (options_.micDeviceIndex >= 0) {
        const QStringList names = AudioRecorder::waveInNames();
        if (options_.micDeviceIndex < names.size()) {
            return names.at(options_.micDeviceIndex).trimmed();
        }
    }
    return {};
}

bool VoiceController::openMic(QString *error) {
    const QString wanted = pinnedMicName();
    // 已经开着、但开的是上一台：先放掉再按当前配置开——否则在设置里换了设备
    // 却还在从旧设备收声，看起来就是「选了没用」。
    if (recorder_.isOpen() && recorder_.requestedDeviceName() != wanted) {
        recorder_.close();
    }
    recorder_.setDeviceName(wanted);
    if (recorder_.isOpen()) {
        return true;  // 同一台常开着，不重开（重开有 0.5-1s 的空窗）
    }
    if (!recorder_.open(error)) {
        return false;
    }
    // 点名没用上（设备拔了/被独占）要说出来：不然用户以为自己选的是耳机麦，
    // 实际在从系统默认那台收声。
    const QString note = recorder_.openedDeviceNote();
    if (!note.isEmpty()) {
        emit eventReady(QStringLiteral("voice"),
                        QString::fromUtf8("麦克风：%1").arg(note));
    }
    return true;
}

QJsonArray VoiceController::micDeviceList() {
    QJsonArray out;
    for (const QString &name : AudioRecorder::waveInNames()) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty()) {
            out.append(trimmed);
        }
    }
    return out;
}

QJsonObject VoiceController::micStatus() const {
    // 读数分两半：pick* = 「现在说话会用哪台」（不依赖麦克风开没开，热键档下
    // 麦克风平时是关的，只报 open/device 就全是空，看不出选错了档）；
    // device/source = 「这一刻真在录的那台」。config 留原值，方便对账手改的配置。
    const QString wanted = pinnedMicName();
    const QString commsName = AudioRecorder::communicationsDeviceName();
    const MicChoice pick = AudioRecorder::chooseMicDevice(
        wanted, commsName, AudioRecorder::waveInNames());
    QString configValue = options_.micDevice.trimmed();
    if (configValue.isEmpty() && options_.micDeviceIndex >= 0) {
        configValue = QString::number(options_.micDeviceIndex);
    }
    // stale = 「开着的那台」和「现在该用的那台」不是同一台。这次就是靠它抓出
    // 默认档被抢走后麦克风还挂在旧设备上；正常情况应该一直是 false。
    MicChoice opened;
    opened.name = recorder_.openedDeviceName();
    opened.index = recorder_.openedDeviceIndex();
    opened.source = recorder_.openedDeviceSource();
    const bool stale = recorder_.isOpen() &&
                       !AudioRecorder::sameDevice(opened, pick);
    return QJsonObject{
        {QStringLiteral("config"),
         configValue.isEmpty() ? QStringLiteral("auto") : configValue},
        {QStringLiteral("pick"), pick.name},
        {QStringLiteral("pick_source"), pick.source},
        {QStringLiteral("pick_bluetooth"), pick.bluetooth},
        // pick_source=="prefer" 时说明自动档躲开了被占的端点：这里给出它躲开了谁。
        {QStringLiteral("comms_device"), commsName},
        // 点名的设备现在不在列表里：蓝牙档不会预开麦克风，下面的 note 要等
        // 真开过一次才有值，所以前瞻性原因单独报一行，别让「选了没用」查不到。
        {QStringLiteral("pick_note"), pick.note},
        {QStringLiteral("hold"), micAlwaysOn_},
        {QStringLiteral("open"), recorder_.isOpen()},
        {QStringLiteral("device"), recorder_.openedDeviceName()},
        {QStringLiteral("source"), recorder_.openedDeviceSource()},
        {QStringLiteral("stale"), stale},
        // level = 麦克风当前 RMS（0~1）。只有麦开着的时候有意义：说话时一直是
        // 0.0 就说明这台设备根本没收到声音（幽灵端点/麦被别的程序独占/人不在
        // 这台设备前），比「听没听到」这种主观判断好对账。
        {QStringLiteral("level"), qRound(recorder_.level() * 1000.0) / 1000.0},
        {QStringLiteral("devices"), micDeviceList()},
        {QStringLiteral("note"), recorder_.openedDeviceNote()},
    };
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
    expireConfirm(now);
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
    if (!recorder_.isOpen() && !openMic(&error)) {
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
    if (!micAlwaysOn_) {
        // 蓝牙麦克风：说完立刻归还设备，耳机马上回音乐通道。
        recorder_.close();
    }
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
        if (!openMic(&error)) {
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
    emit chatModeChanged(on);
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
    // 系统控制（音量/亮度/媒体/电源）优先于听写和对话：命中就地执行。
    // 二次确认的口令也在这里拦，绝不能被当成听写贴进前台。
    if (deliverSystem(text, costMs, false)) {
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

void VoiceController::setSystemControl(ISystemControl *control) {
    system_ = control;
}

void VoiceController::expireConfirm(qint64 now) {
    if (!confirm_.pending()) {
        return;
    }
    SystemCommand waiting;
    waiting.kind = confirm_.kind();
    const QString name = SystemCommandText::label(waiting);
    if (!confirm_.expire(now)) {
        return;
    }
    emit eventReady(QStringLiteral("system"),
                    name + QStringLiteral("：超时未确认，已取消"));
    emit spokenFeedback(name + QStringLiteral("取消了，没执行"));
    setState(State::Idle, QStringLiteral("就绪"));
}

bool VoiceController::deliverSystem(const QString &text, int costMs, bool typed) {
    if (system_ == nullptr) {
        return false;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    expireConfirm(now);
    const SystemCommand command = SystemCommandText::parse(text);
    const SystemConfirm::Verdict verdict = confirm_.offer(command, now);
    if (verdict == SystemConfirm::Verdict::Confirmed) {
        const SystemCommand pending = confirm_.take();
        emit transcribed(text, costMs);
        dispatchSystem(pending, true, typed);
        return true;
    }
    if (verdict == SystemConfirm::Verdict::Cancelled) {
        emit eventReady(QStringLiteral("system"), QStringLiteral("已取消，没执行"));
    }
    // 没有待确认命令时，「确认关机」这类口令不算命令，照原样听写下去。
    if (!command.matched || command.confirmed ||
        !SystemControlSettings::enabled(store_, command.kind)) {
        return false;
    }
    // 打字只能调音量亮度这类可逆的；关机/睡眠必须真的说出口再过确认。
    if (typed && SystemCommandText::needsConfirm(command.kind)) {
        emit transcribed(text, costMs);
        emit eventReady(QStringLiteral("system"),
                        QStringLiteral("关机/睡眠这类动作只能语音触发：按住语音键说「%1」")
                            .arg(SystemCommandText::label(command)));
        setState(State::Idle, QStringLiteral("这类动作只能语音触发"));
        return true;
    }
    emit transcribed(text, costMs);
    if (SystemCommandText::needsConfirm(command.kind)) {
        confirm_.arm(command, now, kConfirmWindowMs);
        const QString prompt =
            QStringLiteral("要%1吗？%2 秒内说「%3」执行，说别的就取消")
                .arg(SystemCommandText::label(command))
                .arg(int(kConfirmWindowMs / 1000))
                .arg(SystemCommandText::confirmPhrase(command.kind));
        emit eventReady(QStringLiteral("system"), prompt);
        if (!typed) {
            emit spokenFeedback(prompt);
        }
        setState(State::Idle, prompt);
        return true;
    }
    dispatchSystem(command, false, typed);
    return true;
}

void VoiceController::dispatchSystem(const SystemCommand &command, bool confirmed,
                                     bool typed) {
    if (system_ == nullptr) {
        return;
    }
    if (systemBusy_) {
        setState(State::Idle, QStringLiteral("上一条系统命令还没执行完"));
        return;
    }
    const QString name = SystemCommandText::label(command);
    systemBusy_ = true;
    setState(State::Idle, name + QStringLiteral("…"));
    ISystemControl *control = system_;
    // DDC/CI 一次要几百毫秒，不能在主线程做；结果回主线程再报状态。
    QThreadPool::globalInstance()->start([this, control, command, confirmed, name,
                                          typed] {
        const SystemResult result = control->execute(command, confirmed);
        QMetaObject::invokeMethod(
            this,
            [this, result, name, typed] {
                if (!alive_.load()) {
                    return;
                }
                systemBusy_ = false;
                emit eventReady(result.ok ? QStringLiteral("system")
                                          : QStringLiteral("error"),
                                name + QStringLiteral("：") + result.text);
                if (!typed) {
                    emit spokenFeedback(result.text);
                }
                setState(State::Idle, result.text);
            },
            Qt::QueuedConnection);
    });
}

bool VoiceController::injectText(const QString &text, bool autoEnter) {
    if (text.isEmpty()) {
        return false;
    }
    return injector_.pasteText(text, autoEnter);
}

bool VoiceController::sendHotkey(const QStringList &mods, int vk) {
    return injector_.sendHotkey(mods, vk);
}

void VoiceController::submitTyped(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    if (deliverSystem(trimmed, 0, true)) {
        return;
    }
    emit transcribed(trimmed, 0);
    emit chatRequested(trimmed);
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
    confirm_.cancel();
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        engine_.reset();
        engineFamily_.clear();
        engineDir_.clear();
    }
    micAlwaysOn_ = listening_ && options_.backend != VoiceBackend::Hotkey &&
                   shouldHoldMic();
    if (micAlwaysOn_) {
        // 走 openMic：里面会比对配置，换了设备先放掉旧的再开。
        QString error;
        if (!openMic(&error)) {
            emit eventReady(QStringLiteral("error"), error);
        }
    } else {
        // 蓝牙麦克风长期占用会把耳机整个切进通话通道（A2DP 挂起），
        // 所以蓝牙一律不预开：开始说话时再开，说完立刻关。
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
        micAlwaysOn_ = false;
        recorder_.close();
        confirm_.cancel();
        return;
    }
    micAlwaysOn_ = options_.backend != VoiceBackend::Hotkey && shouldHoldMic();
    if (micAlwaysOn_) {
        QString error;
        if (!openMic(&error)) {
            emit eventReady(QStringLiteral("error"), error);
        }
    }
}
