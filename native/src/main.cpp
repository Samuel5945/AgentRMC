#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QPainter>
#include <QTabWidget>
#include <QTimer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MainWindow.h"
#include "core/AsrClient.h"
#include "core/LlmClient.h"
#include "core/Profiles.h"
#include "core/ThemeManager.h"
#include "core/TtsClient.h"
#include "core/VoiceController.h"
#include "ui/GuideDialog.h"
#include "ui/PopupShape.h"
#include "ui/ThemeDialog.h"
#include "ui/TtsSettingsDialog.h"
#include "ui/VoicePet.h"
#include "ui/VoiceSettingsDialog.h"

namespace {

// 宿主会话环境隔离：AgentRMC 可能是从某个宿主的会话里被拉起来的（比如
// 用户在 Qoder CN 里让 agent 起桌宠，或 DSH 的 agent 静默装完顺手启动），
// 进程环境会带着那套会话变量（QODER_PRODUCT_ID / QODER_CONFIG_DIR /
// QODER_SEARCH_HOST_PID、ELECTRON_RUN_AS_NODE 等）。之后从这里启动的每一个
// 应用都原样继承——国际版 Qoder 拿到 qoder-cn 身份会在主进程直接拒绝启动
// （「产物内联的产品是 qoder，当前运行身份是 qoder-cn」弹窗）；
// ELECTRON_RUN_AS_NODE=1 更隐蔽：Electron 宿主（WorkBuddy/Qoder/ZCode/DSH）
// 会「当成裸 Node」启动即静默退出（exit 0，无窗口无报错），ShellExecute 还
// 返回成功，待办双击「启动」看着没反应甚至被销账（2026-11-13 实测：
// WorkBuddy 双击启动失灵，根因就是 AgentRMC 从 DSH shell 启动继承了它）。
// 开始菜单能点是因为 Explorer 的环境是干净的。所以进程一起来先把这些变量
// 从自己的环境里摘掉，之后的子进程（待办启动、语音启动、注入 cmd）一并干净。
// 前缀列表大小写不敏感；新宿主再出现同类冲突时往 kPrefixes/kExact 里加。
void scrubHostSessionEnv() {
    static const QStringList kPrefixes = {QStringLiteral("QODER"),
                                          QStringLiteral("__QODER_")};
    // 精确名列表：只摘这一个变量，不动 ELECTRON_* 其它成员
    // （ELECTRON_FORCE_RENDERER_ACCESSIBILITY 之类对宿主无害）。
    static const QStringList kExact = {QStringLiteral("ELECTRON_RUN_AS_NODE")};
    wchar_t *block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return;
    }
    QStringList victims;
    for (const wchar_t *p = block; *p != L'\0'; p += wcslen(p) + 1) {
        const wchar_t *eq = wcschr(p, L'=');
        if (eq == nullptr) {
            continue;
        }
        const QString upper =
            QString::fromWCharArray(p, int(eq - p)).toUpper();
        bool victim = kExact.contains(upper);
        for (const QString &prefix : kPrefixes) {
            if (upper.startsWith(prefix)) {
                victim = true;
                break;
            }
        }
        if (victim) {
            victims.append(QString::fromWCharArray(p, int(eq - p)));
        }
    }
    FreeEnvironmentStringsW(block);
    // 值传 nullptr = 从进程环境块删除；子进程继承的就是洗过的这份。
    for (const QString &name : victims) {
        SetEnvironmentVariableW(reinterpret_cast<const wchar_t *>(name.utf16()),
                                nullptr);
    }
}

}  // namespace

int main(int argc, char *argv[]) {
    scrubHostSessionEnv();
    QApplication app(argc, argv);
    // 启动时照常取系统字体/调色板，之后不再跟桌面设置联动：配色全由
    // ThemeManager 管，而 Qt 自己收到深浅色变更（注册表通知/WM_SETTINGCHANGE）
    // 会拿全局 QSS repolish 整窗，实测卡 UI 线程 0.6~2 秒，跟随系统反被它拖慢。
    QApplication::setDesktopSettingsAware(false);
    QApplication::setApplicationName("AgentRMCNative");
    QApplication::setOrganizationName("LIG");
    QApplication::setApplicationVersion(AGENTRMC_VERSION);
    // 下拉弹层 / 右键菜单是独立 popup 窗口：启动时装一次圆角+表面色，
    // QSS 的圆角才不会被方形窗口底顶出黑角（见 PopupShape.h）。
    PopupShape::install();
    // QSS 由 ThemeManager 渲染 @token@ 后再 setStyleSheet（init 时应用），
    // 这里绝不能直接加载 :/style.qss——那是带占位符的模板。

    QString shot;
    QString pin;
    QString settingsShot;
    QString themeShot;
    QString guideShot;
    QString petShot;
    QString ttsTest;
    QString ttsSave;
    QString ttsShot;
    QString asrTest;
    QString llmTest;
    QString llmSave;
    int shotTab = -1;
    int guideStep = -1;
    const QStringList args = app.arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args.at(i) == "--shot") {
            shot = args.at(i + 1);
        } else if (args.at(i) == "--pin") {
            pin = args.at(i + 1);
        } else if (args.at(i) == "--shot-settings") {
            settingsShot = args.at(i + 1);
        } else if (args.at(i) == "--shot-theme") {
            themeShot = args.at(i + 1);
        } else if (args.at(i) == "--shot-guide") {
            guideShot = args.at(i + 1);
        } else if (args.at(i) == "--shot-pet") {
            petShot = args.at(i + 1);
        } else if (args.at(i) == "--tts-test") {
            ttsTest = args.at(i + 1);
        } else if (args.at(i) == "--tts-save") {
            ttsSave = args.at(i + 1);
        } else if (args.at(i) == "--asr-test") {
            asrTest = args.at(i + 1);
        } else if (args.at(i) == "--llm-test") {
            llmTest = args.at(i + 1);
        } else if (args.at(i) == "--llm-save") {
            llmSave = args.at(i + 1);
        } else if (args.at(i) == "--shot-tts") {
            ttsShot = args.at(i + 1);
        } else if (args.at(i) == "--tab") {
            shotTab = args.at(i + 1).toInt();
        } else if (args.at(i) == "--guide-step") {
            guideStep = args.at(i + 1).toInt();
        }
    }

    // 开机自启带 --tray：只留托盘；手动双击图标时不带，直接开窗口。
    const bool trayStart = app.arguments().contains(QStringLiteral("--tray"));
    const QString ipcName = QStringLiteral("AgentRMC.Native.Wake");
    const bool shotMode = !shot.isEmpty() || !settingsShot.isEmpty() ||
                          !themeShot.isEmpty() || !guideShot.isEmpty() ||
                          !petShot.isEmpty() || !ttsTest.isEmpty() ||
                          !ttsShot.isEmpty() || !asrTest.isEmpty() ||
                          !llmTest.isEmpty();
    if (!shotMode) {
        if (HWND existing = FindWindowW(nullptr, L"AgentRMC Native")) {
            if (!trayStart) {
                PostMessageW(existing,
                             RegisterWindowMessageW(L"AgentRMC.Native.Wake"), 0, 0);
            }
            return 0;
        }
    }

    if (!settingsShot.isEmpty()) {
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        ThemeManager::instance().init(&store);
        VoiceSettingsDialog dialog(&store);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        QTimer::singleShot(350, &dialog, [&dialog, settingsShot] {
            const bool ok = dialog.grab().save(settingsShot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed")
                              << settingsShot;
            QCoreApplication::quit();
        });
        return app.exec();
    }

    if (!themeShot.isEmpty()) {
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        ThemeManager::instance().init(&store);
        ThemeDialog dialog(&store);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        QTimer::singleShot(350, &dialog, [&dialog, themeShot] {
            const bool ok = dialog.grab().save(themeShot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed")
                              << themeShot;
            QCoreApplication::quit();
        });
        return app.exec();
    }

    if (!guideShot.isEmpty()) {
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        ThemeManager::instance().init(&store);
        GuideDialog dialog;
        if (guideStep >= 0) {
            dialog.showStep(guideStep);
        }
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        QTimer::singleShot(350, &dialog, [&dialog, guideShot] {
            const bool ok = dialog.grab().save(guideShot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed")
                              << guideShot;
            QCoreApplication::quit();
        });
        return app.exec();
    }

    if (!petShot.isEmpty()) {
        // 桌宠验收图：球体 + 悬停卡片 + 气泡拼一张。
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        ThemeManager::instance().init(&store);
        VoiceController voice(&store);
        VoicePet pet(&store, &voice);
        pet.setAttribute(Qt::WA_DontShowOnScreen);
        pet.setVoiceFeedbackMuted(true);  // 截图别真发 TTS 请求
        pet.present();
        // 状态行文案是「事后」才出现的：先塞一条超长的，验证卡片不会被它顶开、
        // 下拉行不会被挤掉半截（expandCard 里的 adjustSize 就在这一步之后跑）。
        // AGENTRMC_PET_LONG_DETAIL=1 时启用。
        if (qEnvironmentVariableIsSet("AGENTRMC_PET_LONG_DETAIL")) {
            pet.setStatusDetailForShot(
                QString::fromUtf8("亮度 50% · 光标所在屏 · 这条状态文案故意写得很长"));
        }
        // 转写区空态只占一行，有正文才长高：这里塞一段，验「字没有被切掉」。
        // AGENTRMC_PET_SHOT_TRANSCRIPT=1 时启用。
        if (qEnvironmentVariableIsSet("AGENTRMC_PET_SHOT_TRANSCRIPT")) {
            pet.setTranscriptForShot(QString::fromUtf8(
                "把这句话写进桌宠卡片的转写区，看看有正文的时候盒子会不会自己长高、"
                "文字有没有被切掉半截。"));
        }
        // 强制状态拍状态环/外圈弧：AGENTRMC_PET_SHOT_STATE=transcribing 等。
        const QString shotState = qEnvironmentVariable("AGENTRMC_PET_SHOT_STATE");
        if (!shotState.isEmpty()) {
            pet.setStateForShot(shotState);
        }
        pet.expandCard();
        pet.speak(QString::fromUtf8("你好，我是你的语音桌宠"));
        QTimer::singleShot(450, &pet, [&pet, petShot] {
            // 画布按最长的一块算：卡片 300 + 左边距，别把卡片右边裁掉。
            int cardWidth = 300;
            QWidget *card = pet.findChild<QWidget *>(QStringLiteral("petCard"));
            if (card != nullptr) {
                cardWidth = qMax(cardWidth, card->width());
            }
            QPixmap canvas(cardWidth + 60, 480);
            canvas.fill(Qt::transparent);
            QPainter painter(&canvas);
            painter.drawPixmap(30, 8, pet.grab());
            if (card != nullptr) {
                painter.drawPixmap(30, 110, card->grab());
            }
            QWidget *bubble =
                pet.findChild<QWidget *>(QStringLiteral("petBubble"));
            if (bubble != nullptr) {
                painter.drawPixmap(30, 340, bubble->grab());
            }
            const bool ok = canvas.save(petShot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed")
                              << petShot;
            QCoreApplication::quit();
        });
        return app.exec();
    }

    if (!ttsTest.isEmpty()) {
        // TTS 自检：--tts-test "你好" 合成并播放，打印字节数/错误。
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        ThemeManager::instance().init(&store);  // 跟随主题解析要拿到当前主题
        VoiceController voice(&store);
        const VoiceOptions options = voice.options();
        // 跟随主题：与 VoicePet 同规则解析（自检跑在主题扫描之后，能拿到当前主题）。
        QString ttsVoice = options.ttsVoice;
        QString ttsStyle = options.ttsStyle;
        if (isFollowThemeVoice(ttsVoice)) {
            const Theme &theme = ThemeManager::instance().current();
            ttsVoice = theme.voiceName.isEmpty() ? QStringLiteral("Chloe")
                                                 : theme.voiceName;
            if (ttsStyle.isEmpty()) {
                ttsStyle = theme.voiceStyle;
            }
        }
        TtsClient tts;
        tts.setOptions(options.ttsEnabled, options.ttsBaseUrl, options.ttsApiKey,
                       options.ttsModel, ttsVoice, ttsStyle,
                       options.ttsOutputDevice);
        if (!tts.ready()) {
            qWarning().noquote()
                << "[tts] 没有 API Key：在语音设置的「TTS Key」里自填，"
                   "或设环境变量 MIMO_API_KEY";
            return 2;
        }
        QObject::connect(&tts, &TtsClient::audioReady, &app,
                         [&ttsTest, &ttsSave](const QByteArray &wav) {
                             qInfo().noquote() << "[tts] ok" << wav.size()
                                               << "bytes，播放中：" << ttsTest;
                             if (!ttsSave.isEmpty()) {
                                 QFile out(ttsSave);
                                 if (out.open(QIODevice::WriteOnly)) {
                                     out.write(wav);
                                 }
                             }
                             // 16k 单声道 16bit ≈ 32 字节/毫秒，播完退出
                             QTimer::singleShot(wav.size() / 32 + 800, [] {
                                 QCoreApplication::exit(0);
                             });
                         });
        QObject::connect(
            &tts, &TtsClient::failed, &app, [](const QString &error) {
                qWarning().noquote() << "[tts] failed:" << error;
                QCoreApplication::exit(1);
            });
        tts.speak(ttsTest);
        QTimer::singleShot(30000, &app, [] { QCoreApplication::exit(3); });
        return app.exec();
    }

    if (!ttsShot.isEmpty()) {
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        ThemeManager::instance().init(&store);
        TtsSettingsDialog dialog(&store);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        QTimer::singleShot(350, &dialog, [&dialog, ttsShot] {
            const bool ok = dialog.grab().save(ttsShot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed") << ttsShot;
            QCoreApplication::quit();
        });
        return app.exec();
    }

    if (!asrTest.isEmpty()) {
        // 云端 ASR 自检：--asr-test file.wav → 上传转写，打印文本。
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        VoiceController voice(&store);
        const VoiceOptions options = voice.options();
        AsrClient asr;
        asr.setOptions(options.asrApiBaseUrl, options.asrApiKey,
                       options.asrApiModel, options.language,
                       options.asrApiStyle);
        if (!asr.ready()) {
            qWarning().noquote()
                << "[asr] 没配置：语音设置 → 云端 ASR 填模型，"
                   "Key 可沿用语音反馈的";
            return 2;
        }
        QFile wavFile(asrTest);
        if (!wavFile.open(QIODevice::ReadOnly)) {
            qWarning().noquote() << "[asr] 打不开音频文件" << asrTest;
            return 2;
        }
        QObject::connect(&asr, &AsrClient::transcribed, &app,
                         [](const QString &text, qint64 costMs) {
                             qInfo().noquote() << "[asr] ok" << costMs << "ms"
                                               << text;
                             QCoreApplication::exit(0);
                         });
        QObject::connect(&asr, &AsrClient::failed, &app,
                         [](const QString &error, qint64) {
                             qWarning().noquote() << "[asr] failed:" << error;
                             QCoreApplication::exit(1);
                         });
        asr.transcribe(wavFile.readAll());
        QTimer::singleShot(45000, &app, [] { QCoreApplication::exit(3); });
        return app.exec();
    }

    if (!llmTest.isEmpty()) {
        // 对话闭环自检：--llm-test "问题" → 打印模型回复（可 --llm-save 落盘）。
        ProfileStore store(QCoreApplication::applicationDirPath() +
                           "/profiles.json");
        store.load();
        store.loadConfig(QCoreApplication::applicationDirPath() + "/config.json");
        VoiceController voice(&store);
        const VoiceOptions options = voice.options();
        LlmClient llm;
        llm.setOptions(options.llmApiBaseUrl, options.llmApiKey,
                       options.llmApiModel, options.llmApiSystem);
        if (!llm.ready()) {
            qWarning().noquote()
                << "[llm] 没配置：对话设置里填模型，Key 可沿用语音反馈的";
            return 2;
        }
        QJsonArray messages{
            QJsonObject{{"role", "system"},
                        {"content", options.llmApiSystem.isEmpty()
                                        ? QString::fromUtf8(
                                              "你是语音桌宠，用中文口语简短回答。")
                                        : options.llmApiSystem}},
            QJsonObject{{"role", "user"}, {"content", llmTest}}};
        QObject::connect(
            &llm, &LlmClient::replied, &app,
            [&llmSave](const QString &text, qint64 costMs) {
                qInfo().noquote() << "[llm] ok" << costMs << "ms" << text;
                if (!llmSave.isEmpty()) {
                    QFile out(llmSave);
                    if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        out.write(text.toUtf8());
                    }
                }
                QCoreApplication::exit(0);
            });
        QObject::connect(
            &llm, &LlmClient::failed, &app, [](const QString &error, qint64) {
                qWarning().noquote() << "[llm] failed:" << error;
                QCoreApplication::exit(1);
            });
        llm.chat(messages);
        QTimer::singleShot(60000, &app, [] { QCoreApplication::exit(3); });
        return app.exec();
    }

    MainWindow win(shot.isEmpty(), pin, shot.isEmpty());
    // 页签截图要看到整页内容，离屏时把窗口拉高。
    // 主窗默认给足高度：左栏语音 + 遥控器两张卡片、右侧待办列表都要站得下。
    win.resize(1060, shotTab >= 0 ? 1400 : 720);

    if (!shotMode) {
        // 托盘常驻：窗口隐藏/关闭不该让进程退出。
        QApplication::setQuitOnLastWindowClosed(false);
    }

    if (!shot.isEmpty()) {
        if (shotTab >= 0) {
            if (auto *tabs = win.findChild<QTabWidget *>()) {
                tabs->setCurrentIndex(shotTab);
            }
        }
        win.setAttribute(Qt::WA_DontShowOnScreen);
        win.show();
        QTimer::singleShot(450, &win, [&win, shot] {
            const bool ok = win.grab().save(shot);
            qInfo().noquote() << "[shot]" << (ok ? "saved" : "failed") << shot;
            QCoreApplication::quit();
        });
    } else {
        win.showOnStartup(trayStart);
    }
    return app.exec();
}
