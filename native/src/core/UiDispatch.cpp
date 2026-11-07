#include "core/UiDispatch.h"

#include "core/AgentWindow.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <uiautomation.h>

#include <QPoint>
#include <QRegularExpression>
#include <QTimer>

#include <cstring>
#include <string>
#include <vector>

namespace {

IUIAutomation *automation() {
    static IUIAutomation *ui = nullptr;
    static bool tried = false;
    if (tried) {
        return ui;
    }
    tried = true;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (init != S_OK && init != S_FALSE && init != RPC_E_CHANGED_MODE) {
        return nullptr;
    }
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui)))) {
        ui = nullptr;
    }
    return ui;
}

QString nameOf(IUIAutomationElement *el) {
    BSTR raw = nullptr;
    el->get_CurrentName(&raw);
    const QString out = raw != nullptr ? QString::fromWCharArray(raw) : QString();
    if (raw != nullptr) {
        SysFreeString(raw);
    }
    return out;
}

int typeOf(IUIAutomationElement *el) {
    VARIANT v{};
    VariantInit(&v);
    int out = 0;
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_ControlTypePropertyId, &v)) &&
        v.vt == VT_I4) {
        out = int(v.lVal);
    }
    VariantClear(&v);
    return out;
}

QString valueOf(IUIAutomationElement *el) {
    VARIANT v{};
    VariantInit(&v);
    QString out;
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &v)) &&
        v.vt == VT_BSTR && v.bstrVal != nullptr) {
        out = QString::fromWCharArray(v.bstrVal);
    }
    VariantClear(&v);
    return out;
}

struct Rel {
    long x = 0, y = 0, w = 0, h = 0;
};

struct Node {
    int type = 0;
    QString name;
    Rel rel;
    IUIAutomationElement *el = nullptr;
};

struct Snapshot {
    std::vector<Node> nodes;
    ~Snapshot() {
        for (Node &n : nodes) {
            if (n.el != nullptr) {
                n.el->Release();
            }
        }
    }
};

Snapshot snap(IUIAutomation *ui, HWND hwnd, const QPoint &origin) {
    Snapshot out;
    IUIAutomationElement *root = nullptr;
    if (FAILED(ui->ElementFromHandle(hwnd, &root)) || root == nullptr) {
        return out;
    }
    IUIAutomationCondition *cond = nullptr;
    if (FAILED(ui->CreateTrueCondition(&cond)) || cond == nullptr) {
        root->Release();
        return out;
    }
    IUIAutomationElementArray *arr = nullptr;
    const HRESULT hr = root->FindAll(TreeScope_Descendants, cond, &arr);
    cond->Release();
    root->Release();
    if (FAILED(hr) || arr == nullptr) {
        return out;
    }
    int count = 0;
    arr->get_Length(&count);
    for (int i = 0; i < count && i < 4000; ++i) {
        IUIAutomationElement *el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || el == nullptr) {
            continue;
        }
        Node n;
        n.el = el;
        n.type = typeOf(el);
        n.name = nameOf(el);
        tagRECT r{};
        el->get_CurrentBoundingRectangle(&r);
        n.rel = {r.left - origin.x(), r.top - origin.y(), r.right - r.left,
                 r.bottom - r.top};
        out.nodes.push_back(n);
    }
    arr->Release();
    return out;
}

bool wideEnough(const Node &n, LONG w) {
    return n.rel.w >= w;
}

void clickAt(const QPoint &p) {
    SetCursorPos(p.x(), p.y());
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
}

void pressEnter() {
    keybd_event(VK_RETURN, 0, 0, 0);
    keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
}

void pressCtrlV() {
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event('V', 0, 0, 0);
    keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
}


QString clipboardText() {
    if (!OpenClipboard(nullptr)) {
        return QString();
    }
    QString out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h != nullptr) {
        const wchar_t *p = static_cast<const wchar_t *>(GlobalLock(h));
        if (p != nullptr) {
            out = QString::fromWCharArray(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

bool setClipboardText(const QString &text) {
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();
    const std::wstring wide = text.toStdWString();
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h != nullptr) {
        wchar_t *p = static_cast<wchar_t *>(GlobalLock(h));
        if (p != nullptr) {
            memcpy(p, wide.c_str(), bytes);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) {
            GlobalFree(h);
        }
    }
    CloseClipboard();
    return ok;
}

}  // namespace

UiDispatch::UiDispatch(QObject *parent) : QObject(parent) {}

bool UiDispatch::supports(const QString &exe) {
    return exe.compare(QStringLiteral("WorkBuddy.exe"), Qt::CaseInsensitive) == 0 ||
           exe.compare(QStringLiteral("WorkBuddyAI.exe"), Qt::CaseInsensitive) == 0;
}

QString UiDispatch::cleanRowName(const QString &name) {
    static const QRegularExpression re(
        QStringLiteral("\\s+(刚刚|\\d+\\s*(分钟|小时|天|月)前|更多)$"));
    QString out = name;
    out.remove(re);
    return out.trimmed();
}

bool UiDispatch::rowMatches(const QString &rowName, const QString &title) {
    const QString row = cleanRowName(rowName);
    if (row.isEmpty() || title.isEmpty()) {
        return false;
    }
    if (row.compare(title, Qt::CaseInsensitive) == 0) {
        return true;
    }
    const int n = qMin(12, qMin(row.size(), title.size()));
    return row.left(n).compare(title.left(n), Qt::CaseInsensitive) == 0;
}

void UiDispatch::start(const Job &job) {
    job_ = job;
    stage_ = 0;
    tries_ = 0;
    scrolled_ = false;
    expandTries_ = 0;
    busy_ = true;
    hasSavedClipboard_ = false;
    QTimer::singleShot(0, this, [this] { step(); });
}

void UiDispatch::restoreClipboard() {
    if (hasSavedClipboard_) {
        setClipboardText(savedClipboard_);
        hasSavedClipboard_ = false;
    }
}

void UiDispatch::finishFail(const QString &reason, bool proof) {
    restoreClipboard();
    busy_ = false;
    if (proof) {
        emit proofFailed(job_.dispatchId, reason);
    } else {
        emit failed(job_.dispatchId, reason);
    }
}

void UiDispatch::step() {
    IUIAutomation *ui = automation();
    if (ui == nullptr) {
        finishFail(QStringLiteral("UIA 不可用"), false);
        return;
    }
    HWND hwnd = static_cast<HWND>(AgentWindow::mainWindow(job_.exe));
    if (hwnd == nullptr) {
        finishFail(QStringLiteral("宿主主窗不在（%1）").arg(job_.exe), false);
        return;
    }
    tagRECT wr{};
    GetWindowRect(hwnd, &wr);
    const QPoint origin(wr.left, wr.top);
    if ((wr.right - wr.left) < 500) {
        // 收进挂件：实测 deep link 能把主窗撑起来。
        const wchar_t *uri =
            job_.exe.compare(QStringLiteral("WorkBuddyAI.exe"),
                             Qt::CaseInsensitive) == 0
                ? L"workbuddy-ai://new-task"
                : L"workbuddy://new-task";
        ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
        if (++tries_ > 4) {
            finishFail(QStringLiteral("主窗唤醒失败（仍是挂件）"), false);
            return;
        }
        QTimer::singleShot(1200, this, [this] { step(); });
        return;
    }
    hwnd_ = hwnd;
    SetForegroundWindow(hwnd);
    Snapshot s = snap(ui, hwnd, origin);
    // Chromium 在被辅助技术查询且窗口在前台后才建全量树：树太薄就等一拍再扫，
    // 别拿空树去滚侧栏/点分组（实测空转三轮后误报找不到行）。
    int named = 0;
    for (const Node &n : s.nodes) {
        if (!n.name.isEmpty()) {
            ++named;
        }
    }
    if (named < 15) {
        if (++tries_ > 12) {
            finishFail(QStringLiteral("可访问性树没建起来（named=%1）").arg(named),
                       false);
            return;
        }
        QTimer::singleShot(400, this, [this] { step(); });
        return;
    }

    const auto rows = [&] {
        std::vector<const Node *> out;
        for (const Node &n : s.nodes) {
            if (n.type == UIA_ButtonControlTypeId && !n.name.isEmpty() &&
                n.rel.x >= 0 && n.rel.x < 600 && n.rel.y > 200 &&
                wideEnough(n, 300)) {
                out.push_back(&n);
            }
        }
        return out;
    };
    const auto headers = [&] {
        QStringList out;
        for (const Node &n : s.nodes) {
            // 标题实测落在 rel y≈97（菜单栏 61px 之下），区域下界别卡 100；
            // 多轮会话停在底部时首句/最新气泡也会进这片区域，靠匹配区分。
            if (n.type == UIA_TextControlTypeId && !n.name.isEmpty() &&
                n.rel.x > 500 && n.rel.x < 1400 && n.rel.y > 60 &&
                n.rel.y < 300 && wideEnough(n, 150)) {
                out.append(n.name);
            }
        }
        return out;
    };
    const auto edits = [&] {
        std::vector<const Node *> out;
        for (const Node &n : s.nodes) {
            if (n.type == UIA_EditControlTypeId && wideEnough(n, 200) &&
                n.rel.h >= 40) {
                out.push_back(&n);
            }
        }
        return out;
    };

    switch (stage_) {
    case 0: {
        const auto rs = rows();
        const Node *hit = nullptr;
        for (const Node *n : rs) {
            if (rowMatches(n->name, job_.title)) {
                hit = n;
                break;
            }
        }
        if (hit != nullptr) {
            IUIAutomationInvokePattern *inv = nullptr;
            if (SUCCEEDED(hit->el->GetCurrentPatternAs(
                    UIA_InvokePatternId, IID_PPV_ARGS(&inv))) &&
                inv != nullptr) {
                inv->Invoke();
                inv->Release();
            } else {
                hit->el->SetFocus();
                pressEnter();
            }
            stage_ = 4;
            tries_ = 0;
            QTimer::singleShot(1200, this, [this] { step(); });
            return;
        }
        if (!scrolled_) {
            // 虚拟化侧栏：滚出视口的行整棵子树不存在，先滚顶。
            POINT save{};
            GetCursorPos(&save);
            SetCursorPos(origin.x() + 280, origin.y() + 500);
            mouse_event(MOUSEEVENTF_WHEEL, 0, 0, 720, 0);
            SetCursorPos(save.x, save.y);
            scrolled_ = true;
            QTimer::singleShot(700, this, [this] { step(); });
            return;
        }
        // 分组开关是 toggle：不知道当前展开还是折叠，点一次找一遍，再点回来
        // 找一遍（实测当前会话所在组默认展开，盲点会把它折叠掉）。
        if (expandTries_ < 2) {
            const Node *group = nullptr;
            for (const Node &n : s.nodes) {
                if (n.type == UIA_TextControlTypeId &&
                    n.rel.x >= 0 && n.rel.x < 600 && wideEnough(n, 40) &&
                    (n.name.startsWith(QStringLiteral("任务")) ||
                     (!job_.groupHint.isEmpty() &&
                      n.name.startsWith(job_.groupHint)))) {
                    group = &n;
                    if (n.name.startsWith(job_.groupHint)) {
                        break;
                    }
                }
            }
            expandTries_ += 1;
            if (group != nullptr) {
                POINT save{};
                GetCursorPos(&save);
                clickAt(QPoint(origin.x() + group->rel.x + group->rel.w / 2,
                               origin.y() + group->rel.y + group->rel.h / 2));
                SetCursorPos(save.x, save.y);
                QTimer::singleShot(700, this, [this] { step(); });
                return;
            }
        }
        if (++tries_ < 3) {
            QTimer::singleShot(700, this, [this] { step(); });
            return;
        }
        finishFail(QStringLiteral("侧栏找不到会话行：%1（可见行 %2）")
                       .arg(job_.title)
                       .arg(rs.size()),
                   false);
        return;
    }
    case 4: {
        const QStringList hs = headers();
        QString proofHit;
        for (const QString &h : hs) {
            if (rowMatches(h, job_.title) ||
                (!job_.altTitle.isEmpty() && rowMatches(h, job_.altTitle))) {
                proofHit = h;
                break;
            }
        }
        if (!proofHit.isEmpty()) {
            stage_ = 5;
            QTimer::singleShot(200, this, [this] { step(); });
            return;
        }
        if (++tries_ < 3) {
            QTimer::singleShot(800, this, [this] { step(); });
            return;
        }
        finishFail(QStringLiteral("聊天区标题读回不符（got=%1 want=%2），证明不了归属")
                       .arg(hs.value(0), job_.title),
                   true);
        return;
    }
    case 5: {
        const auto es = edits();
        if (es.empty()) {
            finishFail(QStringLiteral("找不到输入框"), false);
            return;
        }
        if (FAILED(es.front()->el->SetFocus())) {
            finishFail(QStringLiteral("输入框聚焦失败"), false);
            return;
        }
        savedClipboard_ = clipboardText();
        hasSavedClipboard_ = true;
        if (!setClipboardText(job_.text)) {
            finishFail(QStringLiteral("剪贴板被占用，未发"), false);
            return;
        }
        pressCtrlV();
        stage_ = 6;
        QTimer::singleShot(500, this, [this] { step(); });
        return;
    }
    case 6: {
        const auto es = edits();
        if (es.empty() || !valueOf(es.front()->el).contains(job_.text)) {
            finishFail(QStringLiteral("粘贴读回失败，未发"), false);
            return;
        }
        pressEnter();
        stage_ = 7;
        QTimer::singleShot(300, this, [this] { step(); });
        return;
    }
    case 7:
    default:
        restoreClipboard();
        busy_ = false;
        emit sent(job_.dispatchId);
        return;
    }
}
