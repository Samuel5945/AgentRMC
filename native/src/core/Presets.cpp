#include "Presets.h"

#include <QHash>
#include <QJsonArray>

namespace Presets {
namespace {

QJsonArray mods(std::initializer_list<const char *> items) {
    QJsonArray arr;
    for (const char *item : items) {
        arr.append(QString::fromLatin1(item));
    }
    return arr;
}

QJsonObject keySpec(int vk) {
    return QJsonObject{{"type", "key"}, {"vk", vk}};
}

const QHash<QString, QJsonObject> &specs() {
    static const QHash<QString, QJsonObject> table = [] {
        QHash<QString, QJsonObject> m;
        m["pass"] = QJsonObject{{"type", "none"},
                                {"note", QStringLiteral("原生直通")}};
        m["up"] = keySpec(0x26);
        m["down"] = keySpec(0x28);
        m["left"] = keySpec(0x25);
        m["right"] = keySpec(0x27);
        m["enter"] = keySpec(0x0D);
        m["space"] = keySpec(0x20);
        m["tab"] = keySpec(0x09);
        m["esc"] = keySpec(0x1B);
        m["backspace"] = keySpec(0x08);
        m["delete"] = keySpec(0x2E);
        m["page_up"] = keySpec(0x21);
        m["page_down"] = keySpec(0x22);
        m["scroll_up"] = QJsonObject{{"type", "scroll"}, {"delta", 360}};
        m["scroll_down"] = QJsonObject{{"type", "scroll"}, {"delta", -360}};
        m["browser_back"] = QJsonObject{{"type", "key"}, {"vk", 0x25},
                                        {"mods", mods({"alt"})}};
        m["browser_forward"] = QJsonObject{{"type", "key"}, {"vk", 0x27},
                                           {"mods", mods({"alt"})}};
        m["new_tab"] = QJsonObject{{"type", "key"}, {"vk", 0x54},
                                   {"mods", mods({"ctrl"})}};
        m["close_tab"] = QJsonObject{{"type", "key"}, {"vk", 0x57},
                                     {"mods", mods({"ctrl"})}};
        m["reload"] = QJsonObject{{"type", "key"}, {"vk", 0x52},
                                  {"mods", mods({"ctrl"})}};
        m["address"] = QJsonObject{{"type", "key"}, {"vk", 0x4C},
                                   {"mods", mods({"ctrl"})}};
        m["prev_editor"] = QJsonObject{{"type", "key"}, {"vk", 0x21},
                                       {"mods", mods({"ctrl"})}};
        m["next_editor"] = QJsonObject{{"type", "key"}, {"vk", 0x22},
                                       {"mods", mods({"ctrl"})}};
        m["prev_tab"] = QJsonObject{{"type", "key"}, {"vk", 0x09},
                                    {"mods", mods({"ctrl", "shift"})}};
        m["next_tab"] = QJsonObject{{"type", "key"}, {"vk", 0x09},
                                    {"mods", mods({"ctrl"})}};
        m["palette"] = QJsonObject{{"type", "key"}, {"vk", 0x50},
                                   {"mods", mods({"ctrl", "shift"})}};
        m["clear"] = QJsonObject{{"type", "key"}, {"vk", 0x4C},
                                 {"mods", mods({"ctrl"})}};
        m["volume_up"] = keySpec(0xAF);
        m["volume_down"] = keySpec(0xAE);
        m["mute"] = keySpec(0xAD);
        m["play"] = keySpec(0xB3);
        m["fullscreen"] = keySpec(0x7A);
        m["mouse_click"] = QJsonObject{{"type", "click"}};
        return m;
    }();
    return table;
}

const QHash<QString, QString> &labels() {
    static const QHash<QString, QString> table = {
        {"pass", QStringLiteral("原生直通")},
        {"up", QStringLiteral("↑ 上")},
        {"down", QStringLiteral("↓ 下")},
        {"left", QStringLiteral("← 左")},
        {"right", QStringLiteral("→ 右")},
        {"enter", QStringLiteral("回车")},
        {"space", QStringLiteral("空格")},
        {"tab", QStringLiteral("Tab")},
        {"esc", QStringLiteral("Esc")},
        {"backspace", QStringLiteral("退格")},
        {"delete", QStringLiteral("删除")},
        {"page_up", QStringLiteral("PageUp")},
        {"page_down", QStringLiteral("PageDown")},
        {"scroll_up", QStringLiteral("向上滚动")},
        {"scroll_down", QStringLiteral("向下滚动")},
        {"browser_back", QStringLiteral("后退")},
        {"browser_forward", QStringLiteral("前进")},
        {"new_tab", QStringLiteral("新标签页")},
        {"close_tab", QStringLiteral("关闭标签")},
        {"reload", QStringLiteral("刷新")},
        {"address", QStringLiteral("地址栏")},
        {"prev_editor", QStringLiteral("上一个编辑器")},
        {"next_editor", QStringLiteral("下一个编辑器")},
        {"prev_tab", QStringLiteral("上一个标签")},
        {"next_tab", QStringLiteral("下一个标签")},
        {"palette", QStringLiteral("命令面板")},
        {"clear", QStringLiteral("清屏")},
        {"volume_up", QStringLiteral("音量 +")},
        {"volume_down", QStringLiteral("音量 −")},
        {"mute", QStringLiteral("静音")},
        {"play", QStringLiteral("播放 / 暂停")},
        {"fullscreen", QStringLiteral("全屏")},
        {"mouse_click", QStringLiteral("鼠标左键")},
    };
    return table;
}

const QHash<QString, QString> &actionNames() {
    static const QHash<QString, QString> table = {
        {"up", QStringLiteral("上移")},
        {"down", QStringLiteral("下移")},
        {"left", QStringLiteral("左移")},
        {"right", QStringLiteral("右移")},
        {"confirm", QStringLiteral("确定")},
        {"back", QStringLiteral("返回")},
        {"menu", QStringLiteral("菜单")},
        {"home", QStringLiteral("主页")},
        {"fullscreen", QStringLiteral("全屏")},
        {"scroll_up", QStringLiteral("向上滚动")},
        {"scroll_down", QStringLiteral("向下滚动")},
        {"cursor_left", QStringLiteral("光标左移")},
        {"cursor_right", QStringLiteral("光标右移")},
        {"send", QStringLiteral("发送")},
        {"newline", QStringLiteral("换行")},
        {"chat_top", QStringLiteral("回到顶部")},
        {"prev_editor", QStringLiteral("上一个编辑器")},
        {"next_editor", QStringLiteral("下一个编辑器")},
        {"prev_tab", QStringLiteral("上一个标签")},
        {"next_tab", QStringLiteral("下一个标签")},
        {"palette", QStringLiteral("命令面板")},
        {"hist_prev", QStringLiteral("上一条命令")},
        {"hist_next", QStringLiteral("下一条命令")},
        {"word_left", QStringLiteral("按词左移")},
        {"word_right", QStringLiteral("按词右移")},
        {"clear", QStringLiteral("清屏")},
        {"enter", QStringLiteral("回车")},
        {"esc", QStringLiteral("退出")},
        {"nav_top", QStringLiteral("上翻一页")},
        {"nav_bottom", QStringLiteral("下翻一页")},
        {"nav_prev", QStringLiteral("上一个控件")},
        {"nav_next", QStringLiteral("下一个控件")},
        {"nav_exit", QStringLiteral("退出导航")},
        {"forward", QStringLiteral("前进")},
        {"new_tab", QStringLiteral("新标签页")},
    };
    return table;
}

const QHash<int, QString> &vkLabels() {
    static const QHash<int, QString> table = [] {
        QHash<int, QString> m = {
            {0x08, "BACKSPACE"}, {0x09, "TAB"}, {0x0D, "ENTER"},
            {0x1B, "ESC"}, {0x20, "SPACE"}, {0x21, "PGUP"},
            {0x22, "PGDN"}, {0x23, "END"}, {0x24, "HOME"},
            {0x25, "LEFT"}, {0x26, "UP"}, {0x27, "RIGHT"},
            {0x28, "DOWN"}, {0x2E, "DELETE"}, {0x5B, "WIN"},
            {0x5D, "APPS"}, {0xAD, "MUTE"}, {0xAE, "VOL-"},
            {0xAF, "VOL+"}, {0xB3, "PLAY"},
        };
        for (int c = 0x30; c <= 0x39; ++c) {
            m[c] = QString(QChar(c));
        }
        for (int c = 0x41; c <= 0x5A; ++c) {
            m[c] = QString(QChar(c));
        }
        for (int i = 0; i < 12; ++i) {
            m[0x70 + i] = QString("F%1").arg(i + 1);
        }
        return m;
    }();
    return table;
}

QString labelFor(const QString &kind) {
    if (kind == "mode_media") return QStringLiteral("音量/亮度模式");
    if (kind == "mode_cursor") return QStringLiteral("鼠标模式");
    if (kind == "nav_toggle") return QStringLiteral("导航模式");
    return labels().value(kind, kind);
}

QString stripNote(QString text) {
    text = text.split(QStringLiteral("（")).first();
    text = text.split(QStringLiteral("：")).first().trimmed();
    text.replace(QStringLiteral("原生 "), QString());
    text.replace(QStringLiteral("移动光标"), QStringLiteral("光标"));
    return text;
}

}  // namespace

QString actionLabel(const QString &kind) {
    return labelFor(kind);
}

QString modeLabel(const QString &kind) {
    if (kind == "mode_media") return QStringLiteral("音量/亮度模式");
    if (kind == "mode_cursor") return QStringLiteral("鼠标模式");
    if (kind == "nav_toggle") return QStringLiteral("导航模式");
    return QString();
}

QStringList doubleModeIds() {
    return {"mode_media", "mode_cursor", "nav_toggle"};
}

QStringList holdIds() {
    return {"enter", "esc", "space", "tab", "backspace", "delete",
            "page_up", "page_down", "volume_up", "volume_down",
            "mute", "play", "fullscreen", "mouse_click"};
}

QVector<QPair<QString, QStringList>> groups() {
    return {
        {QStringLiteral("导航 / 输入"),
         {"up", "down", "left", "right", "enter", "space", "tab",
          "esc", "backspace", "delete", "page_up", "page_down"}},
        {QStringLiteral("滚动"), {"scroll_up", "scroll_down"}},
        {QStringLiteral("浏览器"),
         {"browser_back", "browser_forward", "new_tab", "close_tab",
          "reload", "address"}},
        {QStringLiteral("编辑器"),
         {"prev_editor", "next_editor", "prev_tab", "next_tab",
          "palette", "clear"}},
        {QStringLiteral("系统"),
         {"volume_up", "volume_down", "mute", "play", "fullscreen"}},
        {QStringLiteral("鼠标"), {"mouse_click"}},
    };
}

QString actionCn(const QString &sem) {
    return actionNames().value(sem);
}

QString remoteKeyName(int vk) {
    switch (vk) {
    case 38: return QStringLiteral("上键");
    case 40: return QStringLiteral("下键");
    case 37: return QStringLiteral("左键");
    case 39: return QStringLiteral("右键");
    case 13: return QStringLiteral("确定键");
    case 27: return QStringLiteral("返回键");
    case 93: return QStringLiteral("菜单键");
    case 36: return QStringLiteral("主页键");
    case 116: return QStringLiteral("语音键");
    case 122: return QStringLiteral("对焦键");
    default: return vkLabel(vk);
    }
}

QString nativeLabel(int vk) {
    static const QHash<int, QString> table = {
        {38, QString::fromUtf8("↑")}, {40, QString::fromUtf8("↓")},
        {37, QString::fromUtf8("←")}, {39, QString::fromUtf8("→")},
        {13, "Enter"}, {27, "Esc"}, {93, "Apps"}, {36, "Home"},
        {116, "F5"}, {122, "F11"},
    };
    return table.value(vk, vkLabel(vk));
}

QString vkLabel(int vk) {
    if (const auto it = vkLabels().constFind(vk); it != vkLabels().constEnd()) {
        return it.value();
    }
    return QString("VK 0x%1").arg(vk, 2, 16, QChar('0')).toUpper();
}

QString comboText(const QStringList &mods, int vk) {
    QStringList parts;
    for (const QString &m : mods) {
        parts << m.toUpper();
    }
    parts << vkLabel(vk);
    return parts.join('+');
}

QJsonObject actionFor(const QString &kind, int vk, const QString &gesture) {
    if (kind == "mode_media") {
        return QJsonObject{{"type", "mode"}, {"mode", "media"},
                           {"label", QStringLiteral("音量/亮度模式")}};
    }
    if (kind == "mode_cursor") {
        return QJsonObject{{"type", "mode"}, {"mode", "cursor"},
                           {"label", QStringLiteral("鼠标模式")}};
    }
    if (kind == "nav_toggle") {
        return QJsonObject{{"type", "nav_toggle"},
                           {"label", QStringLiteral("导航模式")}};
    }
    const QJsonObject spec = specs().value(kind);
    if (spec.isEmpty()) {
        return {};
    }
    QJsonObject action = spec;
    const QString label = labels().value(kind, kind);
    action["label"] = label;
    const bool plain = action.value("mods").toArray().isEmpty();
    if (gesture == "single" && action.value("type").toString() == "key" &&
        plain && action.value("vk").toInt(-1) == vk) {
        return QJsonObject{{"type", "none"}, {"label", label},
                           {"note", QStringLiteral("原生 ") + nativeLabel(vk)}};
    }
    return action;
}

QString describeAction(const QJsonObject &act, bool holdInfo) {
    if (act.isEmpty()) {
        return QStringLiteral("无动作");
    }
    const QString kind = act.value("type").toString();
    const QString label = act.value("label").toString().trimmed();
    QString text;
    if (!label.isEmpty()) {
        text = label;
    } else if (kind == "key") {
        QStringList parts;
        for (const auto &m : act.value("mods").toArray()) {
            parts << m.toString().toUpper();
        }
        parts << vkLabel(act.value("vk").toInt());
        text = parts.join('+');
        const int rep = act.value("repeat").toInt(1);
        if (rep > 1) {
            text += QString(" ×%1").arg(rep);
        }
    } else if (kind == "scroll") {
        const int delta = act.value("delta").toInt();
        text = QStringLiteral("滚轮 %1 %2")
                   .arg(delta > 0 ? QString::fromUtf8("↑")
                                  : QString::fromUtf8("↓"))
                   .arg(qAbs(delta));
        const int rep = act.value("repeat").toInt(1);
        if (rep > 1) {
            text += QString(" ×%1").arg(rep);
        }
    } else if (kind == "run") {
        const QString cmd = act.value("cmd").toString();
        text = cmd.isEmpty() ? QStringLiteral("启动")
                             : QStringLiteral("启动 ") + cmd;
    } else if (kind == "paste") {
        text = QStringLiteral("粘贴") +
               (act.value("enter").toBool() ? QStringLiteral("并回车") : QString());
    } else if (kind == "enter") {
        text = QStringLiteral("粘贴并回车");
    } else if (kind == "click") {
        text = QStringLiteral("鼠标左键");
    } else if (kind == "none") {
        text = act.value("note").toString().trimmed();
        if (text.isEmpty()) {
            text = QStringLiteral("直通（不拦截）");
        }
    } else {
        text = kind;
    }

    if (holdInfo) {
        const QJsonObject hold = act.value("hold_action").toObject();
        if (!hold.isEmpty()) {
            text += QStringLiteral(" · 长按 ") + describeAction(hold, false);
        } else if (act.value("hold_repeat").toInt() > 0) {
            text += kind == "scroll" ? QStringLiteral(" · 按住连滚")
                                     : QStringLiteral(" · 按住连发");
        }
    }
    return text;
}

QString shortDesc(const QJsonObject &act, int keyVk) {
    if (act.isEmpty()) {
        return {};
    }
    const QString kind = act.value("type").toString();
    if (kind == "none") {
        const QString note = stripNote(act.value("note").toString().trimmed());
        return note.isEmpty() ? QStringLiteral("原生直通") : note;
    }
    QString text = describeAction(act, false);
    text = stripNote(text);
    if (kind == "key" && act.value("mods").toArray().isEmpty() &&
        act.value("repeat").toInt(1) == 1 &&
        act.value("vk").toInt(-1) == keyVk) {
        return QStringLiteral("原生按键");
    }
    return text;
}

QString nativeEffect(const QJsonObject &act) {
    const QString note = act.value("note").toString().toLower();
    const QVector<QPair<QString, QString>> tokens = {
        {"f11", "F11"}, {"enter", "Enter"}, {"esc", "Esc"},
        {"home", "Home"}, {"tab", "Tab"}, {"apps", "Apps"},
    };
    for (const auto &token : tokens) {
        if (note.contains(token.first)) {
            return token.second + QStringLiteral(" 直通");
        }
    }
    for (const QString &arrow :
         {QString::fromUtf8("←"), QString::fromUtf8("→"),
          QString::fromUtf8("↑"), QString::fromUtf8("↓")}) {
        if (note.contains(arrow)) {
            return arrow + QStringLiteral(" 直通");
        }
    }
    return QStringLiteral("原生直通");
}

QString actionEffectText(const QJsonObject &act) {
    const QString kind = act.value("type").toString();
    if (kind == "click") return QStringLiteral("鼠标左键");
    if (kind == "mode") return QStringLiteral("双击进入");
    if (kind == "nav_toggle") return QStringLiteral("双击切换");
    if (kind == "none") return QStringLiteral("无动作");
    QJsonObject copy = act;
    copy.remove("label");
    return stripNote(describeAction(copy, false));
}

QPair<QString, QString> actionCells(const QJsonObject &act, int vk,
                                    const QString &sem) {
    if (act.isEmpty()) {
        return {QStringLiteral("未映射"), {}};
    }
    const QString kind = act.value("type").toString();
    QString name = actionCn(sem);
    if (name.isEmpty()) {
        name = act.value("label").toString();
    }
    if (name.isEmpty()) {
        name = sem;
    }
    QString effect;
    if (kind == "none") {
        effect = nativeEffect(act);
        if (effect == name) {
            effect.clear();
        }
        return {name, effect};
    }
    if (sem.startsWith(QString("k%1_").arg(vk))) {
        effect = actionEffectText(act);
    } else {
        effect = shortDesc(act, vk);
    }
    if (effect == QStringLiteral("原生按键")) {
        static const QHash<int, QString> nativeNames = {
            {13, "Enter"}, {27, "Esc"}, {93, "Apps"}, {36, "Home"},
            {9, "Tab"}, {38, QString::fromUtf8("↑")},
            {40, QString::fromUtf8("↓")}, {37, QString::fromUtf8("←")},
            {39, QString::fromUtf8("→")},
        };
        const QString native = nativeNames.value(act.value("vk").toInt(-1));
        effect = native.isEmpty() ? QStringLiteral("原生直通")
                                  : native + QStringLiteral(" 直通");
    }
    if (effect == name) {
        effect.clear();
    }
    return {name, effect};
}

}  // namespace Presets
