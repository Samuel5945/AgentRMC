#include "core/Profiles.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool check(bool condition, const char *message) {
    if (!condition) {
        std::fputs("FAIL: ", stderr);
        std::fputs(message, stderr);
        std::fputc(10, stderr);
    }
    return condition;
}

QJsonObject qoderProfile() {
    return QJsonObject{
        {"name", "qoder"},
        {"match", QJsonObject{{"process", QJsonArray{"qoder.exe"}}}},
        {"keys", QJsonObject{{"37", "cursor_left"}}},
        {"actions",
         QJsonObject{{"cursor_left",
                      QJsonObject{{"type", "none"},
                                  {"note", QStringLiteral("原生 ← 移动光标")}}}}},
    };
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!check(dir.isValid(), "temp dir")) {
        return 1;
    }
    const QString path = dir.filePath("profiles.json");
    QJsonObject root{
        {"profiles",
         QJsonArray{qoderProfile(),
                    QJsonObject{{"name", "agent"},
                                {"extends", "qoder"},
                                {"match", QJsonObject{{"process",
                                                       QJsonArray{"workbuddy.exe"}}}}},
                    QJsonObject{{"name", "default"},
                                {"keys", QJsonObject{}},
                                {"actions", QJsonObject{}}}}}};
    {
        QFile file(path);
        if (!check(file.open(QIODevice::WriteOnly), "write temp profiles")) {
            return 1;
        }
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }

    ProfileStore store(path);
    QString error;
    bool ok = true;
    ok &= check(store.load(&error), qPrintable(error));
    ok &= check(store.find("qoder") != nullptr, "qoder exists");
    ok &= check(store.find("agent") != nullptr, "agent resolves");

    ok &= check(store.apply("qoder", 37, "double", "backspace"), "apply double");
    ok &= check(store.apply("qoder", 37, "hold", "page_up"), "apply hold");
    ok &= check(store.apply("qoder", 36, "single", "pass"), "apply pass");
    ok &= check(store.apply("qoder", 38, "single", "up"), "apply same-key");
    QJsonObject custom{
        {"type", "key"},
        {"mods", QJsonArray{"ctrl"}},
        {"vk", 0x54},
        {"label", "CTRL+T"},
    };
    ok &= check(store.apply("qoder", 39, "single", "custom", custom),
                "apply custom");

    {
        QFile file(path);
        if (!check(file.open(QIODevice::ReadOnly), "read back")) {
            return 1;
        }
        const QJsonObject raw = QJsonDocument::fromJson(file.readAll()).object();
        QJsonObject qoder;
        for (const auto &value : raw.value("profiles").toArray()) {
            const QJsonObject object = value.toObject();
            if (object.value("name").toString() == "qoder") {
                qoder = object;
                break;
            }
        }
        const QJsonObject actions = qoder.value("actions").toObject();
        ok &= check(qoder.value("double").toObject().value("37").toString() ==
                        "k37_d_backspace",
                    "double sem");
        ok &= check(actions.value("k37_d_backspace").toObject().value("vk").toInt() == 8,
                    "double vk=8");
        ok &= check(qoder.value("hold").toObject().value("37").toString() ==
                        "k37_h_page_up",
                    "hold sem");
        ok &= check(actions.value("k37_h_page_up").toObject().value("vk").toInt() == 0x21,
                    "hold PageUp");
        ok &= check(qoder.value("keys").toObject().value("36").toString() ==
                        "k36_pass",
                    "pass sem");
        ok &= check(actions.value("k36_pass").toObject().value("type").toString() ==
                        "none",
                    "pass type");
        ok &= check(qoder.value("keys").toObject().value("38").toString() ==
                        "k38_up",
                    "same-key sem");
        ok &= check(actions.value("k38_up").toObject().value("type").toString() ==
                        "none",
                    "same-key pass");
        ok &= check(qoder.value("keys").toObject().value("39").toString() ==
                        "k39_custom",
                    "custom sem");
        ok &= check(actions.value("k39_custom").toObject().value("label").toString() ==
                        "CTRL+T",
                    "custom label");
    }
    ok &= check(QFile::exists(path + ".bak"), "bak exists");

    ok &= check(store.load(&error), "reload");
    const Profile *qoder = store.find("qoder");
    const Profile *agent = store.find("agent");
    ok &= check(qoder != nullptr && agent != nullptr, "profiles after reload");
    if (qoder && agent) {
        ok &= check(qoder->doubleMap.value("37").toString() ==
                        "k37_d_backspace",
                    "resolved qoder double");
        ok &= check(agent->doubleMap.value("37").toString() ==
                        "k37_d_backspace",
                    "extends double");
        ok &= check(agent->actions.value("k37_h_page_up").toObject().value("vk").toInt() ==
                        0x21,
                    "extends action");
        ok &= check(agent->keys.value("36").toString() == "k36_pass",
                    "extends keys");
    }

    if (!ok) {
        return 1;
    }
    std::fputs("PASS native ProfileStore: apply / bak / extends / same-key",
               stdout);
    std::fputc(10, stdout);
    return 0;
}
