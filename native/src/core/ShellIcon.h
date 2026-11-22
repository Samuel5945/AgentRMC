#pragma once

#include <QGuiApplication>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace ShellIcon {

// 普通文件图标（exe / .lnk）：内部走 SHCreateItemFromParsingName + GetImage。
inline QIcon fromPath(const QString &path);

// 商店应用图标（AUMID，如 OpenAI.Codex_xxx!App）：先查内存缓存，
// 再试枚举匹配；拿不到返回空 QIcon。
inline QIcon fromAumid(const QString &aumid);

}  // namespace ShellIcon

// —— 实现（头文件内联，Qt Widgets 与 Core-only 测试两种形态共用）——

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>  // IShellItem / IShellItemImageFactory
#include <shlobj.h>    // BHID_EnumItems
#include <shellapi.h>
#include <objbase.h>

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

namespace shell_icon_detail {

inline constexpr SIZE kIconSize = {64, 64};  // 卡片 44px，取大一点留高 DPI 余量

// thread_local 包在函数里：inline 变量要 C++17 且跨 TU 单例，函数静态更稳。
inline void ensureComOnThread() {
    thread_local bool done = false;
    if (done) {
        return;
    }
    done = true;
    // RPC_E_CHANGED_MODE（线程已被别处做成 MTA）不算致命，shell 接口照样能用。
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
}

inline QString lastSegment(const QString &aumid) {
    const int bang = aumid.indexOf(QLatin1Char('!'));
    return bang > 0 ? aumid.left(bang) : aumid;
}

// AUMID 家族前缀：剥掉版本号 _<digits>.<digits>.<digits>.<digits>。
// OpenAI.Codex_26.1117.3838.0_x64__2p2nqsd0c76g0 → OpenAI.Codex
inline QString familyPrefix(const QString &aumid) {
    QString seg = lastSegment(aumid);
    static const QRegularExpression version(
        QStringLiteral("_\\d+\\.\\d+\\.\\d+\\.\\d+$"));
    seg.remove(version);
    return seg;
}

// Shell 解析路径 + IShellItemImageFactory 取位图 → QIcon。
inline HBITMAP getBitmapForPath(const QString &path) {
    ensureComOnThread();
    IShellItem *item = nullptr;
    const HRESULT hr = SHCreateItemFromParsingName(
        reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) || item == nullptr) {
        return nullptr;
    }
    HBITMAP hbm = nullptr;
    IShellItemImageFactory *factory = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&factory))) &&
        factory != nullptr) {
        factory->GetImage(kIconSize, SIIGBF_RESIZETOFIT, &hbm);
        factory->Release();
    }
    item->Release();
    return hbm;
}

inline QIcon iconFromBitmap(HBITMAP hbm) {
    if (hbm == nullptr) {
        return {};
    }
    BITMAP bmp{};
    const bool ok = GetObjectW(hbm, sizeof(bmp), &bmp) != 0;
    QImage img;
    if (ok && bmp.bmPlanes == 1 && bmp.bmBitsPixel == 32 &&
        bmp.bmBits != nullptr) {
        // 32 位 DIB 的字节序是 BGRA，Qt 的 ARGB32 在内存里正是 BGRA。
        img = QImage(static_cast<const uchar *>(bmp.bmBits), bmp.bmWidth,
                     bmp.bmHeight, bmp.bmWidth * 4,
                     QImage::Format_ARGB32);
        img = img.copy();  // 离开 bmBits 的生命周期
        DeleteObject(hbm);
    } else if (ok && bmp.bmBitsPixel == 24 && bmp.bmBits != nullptr) {
        // 24 位不透明位图（系统图标常见）：逐行按 4 字节对齐拷贝。
        const int rowBytes = ((bmp.bmWidth * 3 + 3) / 4) * 4;
        img = QImage(bmp.bmWidth, bmp.bmHeight, QImage::Format_ARGB32);
        const uchar *src = static_cast<const uchar *>(bmp.bmBits);
        for (int y = 0; y < bmp.bmHeight; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
            const uchar *s = src + (bmp.bmHeight - 1 - y) * rowBytes;
            for (int x = 0; x < bmp.bmWidth; ++x) {
                line[x] = qRgb(s[x * 3 + 2], s[x * 3 + 1], s[x * 3]);
            }
        }
        DeleteObject(hbm);
    } else {
        img = QImage::fromHBITMAP(hbm);  // 成功即接管所有权
        if (img.isNull()) {
            DeleteObject(hbm);
        }
    }
    if (img.isNull()) {
        return {};
    }
    // 全透明兜底：32 位 DIB 的 alpha 可能整个是 0（图标画进了 RGB，
    // alpha 没人填），这种位图按不透明处理，否则 QIcon 会把它当空图。
    if (img.hasAlphaChannel()) {
        bool anyOpaque = false;
        for (int y = 0; y < img.height() && !anyOpaque; ++y) {
            const uchar *line = img.constScanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                if (line[x * 4 + 3] > 128) {
                    anyOpaque = true;
                    break;
                }
            }
        }
        if (!anyOpaque) {
            img = img.convertToFormat(QImage::Format_RGB32);
        }
    }
    return QIcon(QPixmap::fromImage(img));
}

inline QMutex &cacheMutex() {
    static QMutex mutex;
    return mutex;
}

struct AppsFolderCache {
    QStringList names;                     // 枚举到的显示名（小写）
    QStringList urls;                      // 解析名（shell:AppsFolder\ 后可用段）
    bool enumerated = false;
    QHash<QString, QIcon> resolved;        // 小写解析键 → QIcon（空也记）
};

inline AppsFolderCache &cache() {
    static AppsFolderCache c;
    return c;
}

inline void enumerateAppsFolderLocked() {
    AppsFolderCache &c = cache();
    if (c.enumerated) {
        return;
    }
    c.enumerated = true;  // 只试一次，失败不重试（不拖住应用页刷新）
    IShellItem *root = nullptr;
    const HRESULT hrOpen = SHCreateItemFromParsingName(
        L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&root));
    if (FAILED(hrOpen) || root == nullptr) {
        return;
    }
    IEnumShellItems *en = nullptr;
    const HRESULT hrEnum = root->BindToHandler(
        nullptr, BHID_EnumItems, IID_PPV_ARGS(&en));
    root->Release();
    if (FAILED(hrEnum) || en == nullptr) {
        return;
    }
    // 同时记下显示名（小写）和 child 的解析尝试线索：SIGDN 全格式。
    IShellItem *child = nullptr;
    while (en->Next(1, &child, nullptr) == S_OK && child != nullptr) {
        PWSTR name = nullptr;
        if (SUCCEEDED(child->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
            name != nullptr) {
            c.names.append(QString::fromWCharArray(name).toLower());
            CoTaskMemFree(name);
        }
        name = nullptr;
        // SIGDN_DESKTOPABSOLUTEPARSING：AppsFolder child 给的是
        // 「shell:AppsFolder\{可解析段}」，取最后一段当解析键。
        if (SUCCEEDED(child->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,
                                            &name)) &&
            name != nullptr) {
            QString url = QString::fromWCharArray(name);
            CoTaskMemFree(name);
            const int slash = url.lastIndexOf(QLatin1Char('\\'));
            if (slash >= 0 && slash < url.size() - 1) {
                c.urls.append(url.mid(slash + 1));
            }
        }
        child->Release();
        child = nullptr;
    }
    en->Release();
}

inline QIcon tryAppsFolderNameLocked(const QString &name) {
    AppsFolderCache &c = cache();
    const QString key = name.toLower();
    const auto hit = c.resolved.constFind(key);
    if (hit != c.resolved.constEnd()) {
        return hit.value();
    }
    QIcon icon;
    // ① 显示名解析：AppsFolder child 的 NORMALDISPLAY 名就是可解析段
    //    （探针实测：shell:AppsFolder\OpenSquilla → GetImage 成功）。
    // ② 整段 AUMID（含 !App）：部分包的注册激活名走这条路。
    for (int pass = 0; pass < 2 && icon.isNull(); ++pass) {
        QString candidate = name;
        const int slash = candidate.lastIndexOf(QLatin1Char('\\'));
        if (slash >= 0) {
            candidate = candidate.mid(slash + 1);
        }
        if (pass == 1 && !candidate.contains(QLatin1Char('!'))) {
            candidate += QStringLiteral("!App");
        }
        icon = iconFromBitmap(
            getBitmapForPath(QStringLiteral("shell:AppsFolder\\") + candidate));
    }
    c.resolved.insert(key, icon);  // 空结果也存，避免反复解析
    return icon;
}

}  // namespace shell_icon_detail

namespace ShellIcon {

inline QIcon fromPath(const QString &path) {
    return shell_icon_detail::iconFromBitmap(
        shell_icon_detail::getBitmapForPath(path));
}

inline QIcon fromAumid(const QString &aumid) {
    if (aumid.isEmpty()) {
        return {};
    }
    using namespace shell_icon_detail;
    AppsFolderCache &c = cache();
    const QString full = lastSegment(aumid);       // PackageId!AppId
    const QString pkgId = full.section(QLatin1Char('!'), 0, 0);
    const QString family = familyPrefix(aumid);    // 去版本号：OpenAI.Codex
    QMutexLocker locker(&cacheMutex());
    enumerateAppsFolderLocked();
    // ① 解析名精确等于 AUMID 的 PackageId!AppId（child 的 URL 段常带 !）
    // ② 解析名以 PackageId 开头（OpenAI.Codex_26.1117...!App 命中
    //    PackageId=OpenAI.Codex_26.1117... 的整段）
    // ③ 显示名 / 家族前缀兜底。
    QStringList tries;
    tries << full << pkgId << family;
    for (const QString &want : tries) {
        if (want.isEmpty()) {
            continue;
        }
        const QString wantLow = want.toLower();
        for (const QString &u : c.urls) {
            const QString uLow = u.toLower();
            if (uLow == wantLow || uLow.startsWith(wantLow + QLatin1Char('!')) ||
                uLow.startsWith(wantLow + QLatin1Char('_'))) {
                const QIcon icon = tryAppsFolderNameLocked(u);
                if (!icon.isNull()) {
                    return icon;
                }
            }
        }
        // 显示名命中（Codex / ChatGPT 这类）
        for (const QString &name : c.names) {
            if (name == wantLow) {
                const QIcon icon = tryAppsFolderNameLocked(name);
                if (!icon.isNull()) {
                    return icon;
                }
            }
        }
    }
    // 家族前缀以显示名开头（OpenAI.Codex → Codex）
    const QString familyLow = family.toLower();
    for (const QString &name : c.names) {
        if (!name.isEmpty() && familyLow.startsWith(name)) {
            const QIcon icon = tryAppsFolderNameLocked(name);
            if (!icon.isNull()) {
                return icon;
            }
        }
    }
    return tryAppsFolderNameLocked(family);
}

}  // namespace ShellIcon
