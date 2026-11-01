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
#include "ui/TtsSettingsDialog.h"
#include "ui/VoicePet.h"
#include "ui/VoiceSettingsDialog.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("AgentRMCNative");
    QApplication::setOrganizationName("LIG");
    QApplication::setApplicationVersion(AGENTRMC_VERSION);
    // QSS 由 ThemeManager 渲染 @token@ 后再 setStyleSheet（init 时应用），
    // 这里绝不能直接加载 :/style.qss——那是带占位符的模板。

    QString shot;
    QString pin;
    QString settingsShot;
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
                          !guideShot.isEmpty() || !petShot.isEmpty() ||
                          !ttsTest.isEmpty() || !ttsShot.isEmpty() ||
                          !asrTest.isEmpty() || !llmTest.isEmpty();
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
        pet.expandCard();
        pet.speak(QString::fromUtf8("你好，我是你的语音桌宠"));
        QTimer::singleShot(450, &pet, [&pet, petShot] {
            QPixmap canvas(320, 420);
            canvas.fill(Qt::transparent);
            QPainter painter(&canvas);
            painter.drawPixmap(30, 8, pet.grab());
            QWidget *card = pet.findChild<QWidget *>(QStringLiteral("petCard"));
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
        VoiceController voice(&store);
        const VoiceOptions options = voice.options();
        TtsClient tts;
        tts.setOptions(options.ttsEnabled, options.ttsBaseUrl, options.ttsApiKey,
                       options.ttsModel, options.ttsVoice, options.ttsStyle,
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
    win.resize(1000, shotTab >= 0 ? 1400 : 640);

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
