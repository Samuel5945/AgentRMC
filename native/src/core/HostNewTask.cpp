#include "core/HostNewTask.h"

#include "core/AgentWindow.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <uiautomation.h>

#include <QString>
#include <QStringList>

#include <string>

namespace {

// 窄于此宽度的是悬浮挂件而不是主面页：实测 WorkBuddy 收起时是 314×50，
// 那扇窗的 UIA 树只有 8 个节点、0 个具名，按名找必然空手而归。
constexpr LONG kMainWidthFloor = 500;
// 具名节点少于此 = 可访问性树还没建起来（Chromium 在被辅助技术查询时才建全量树）。
constexpr int kNamedFloor = 15;

HRESULT g_createHr = E_NOT_SET;

IUIAutomation *automation() {
    static IUIAutomation *ui = nullptr;
    static bool tried = false;
    if (tried) {
        return ui;
    }
    tried = true;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (init != S_OK && init != S_FALSE && init != RPC_E_CHANGED_MODE) {
        g_createHr = init;
        return nullptr;
    }
    g_createHr = CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui));
    if (FAILED(g_createHr)) {
        ui = nullptr;
    }
    return ui;
}

// 只有实测过的宿主才登记唤醒协议：workbuddy:// 与 workbuddy-ai:// 都在
// HKCR 里注册，实测能把 314×50 挂件撑成 1616×1200 / 1829×1371 主窗。
void wakeMainWindow(const QString &exe) {
    static const struct {
        const wchar_t *image;
        const wchar_t *uri;
    } kWake[] = {
        {L"WorkBuddy.exe", L"workbuddy://new-task"},
        {L"WorkBuddyAI.exe", L"workbuddy-ai://new-task"},
    };
    const std::wstring wide = exe.toStdWString();
    for (const auto &entry : kWake) {
        if (_wcsicmp(wide.c_str(), entry.image) == 0) {
            ShellExecuteW(nullptr, L"open", entry.uri, nullptr, nullptr,
                          SW_SHOWNORMAL);
            return;
        }
    }
}

struct PassResult {
    int named = 0;
    bool hit = false;
    QString hitName;
    int hitType = 0;
    IUIAutomationElement *element = nullptr;  // 命中时由调用方 Release
};

// 这版 Windows SDK 只给 UIA_*ControlTypeId 常量，没有 UIA_ControlTypeIds
// 这个类型名可用（get_CurrentControlType 编译不过），所以走属性接口取。
int controlTypeOf(IUIAutomationElement *el) {
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

PassResult scanOnce(IUIAutomation *ui, HWND hwnd, const QStringList &names) {
    PassResult out;
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
    for (int i = 0; i < count; ++i) {
        IUIAutomationElement *el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || el == nullptr) {
            continue;
        }
        BSTR raw = nullptr;
        el->get_CurrentName(&raw);
        const QString nm = raw != nullptr ? QString::fromWCharArray(raw)
                                          : QString();
        if (!nm.isEmpty()) {
            ++out.named;
        }
        bool wanted = false;
        for (const QString &cand : names) {
            // 精确等值，不用 contains：实测同窗口里有个零尺寸离屏文本节点
            // 名字里也带「新任务」，按子串匹配会挑到它。
            if (!cand.isEmpty() && nm.compare(cand, Qt::CaseInsensitive) == 0) {
                wanted = true;
                break;
            }
        }
        if (wanted && !out.hit) {
            tagRECT r{};
            if (SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) &&
                (r.right - r.left) >= 8 && (r.bottom - r.top) >= 8) {
                out.hit = true;
                out.hitName = nm;
                out.hitType = controlTypeOf(el);
                out.element = el;
                el = nullptr;  // 交给调用方释放
            }
        }
        if (raw != nullptr) {
            SysFreeString(raw);
        }
        if (el != nullptr) {
            el->Release();
        }
    }
    arr->Release();
    return out;
}

void fillFocus(IUIAutomation *ui, HostNewTask::Attempt *attempt) {
    IUIAutomationElement *el = nullptr;
    if (FAILED(ui->GetFocusedElement(&el)) || el == nullptr) {
        attempt->landed = QStringLiteral("<none>");
        return;
    }
    BSTR raw = nullptr;
    el->get_CurrentName(&raw);
    attempt->landed = raw != nullptr ? QString::fromWCharArray(raw) : QString();
    attempt->landedType = controlTypeOf(el);
    if (raw != nullptr) {
        SysFreeString(raw);
    }
    el->Release();
}

}  // namespace

namespace HostNewTask {

QStringList controlNames(const QString &exe) {
    // 实测登记（2026-11-03 P2）：WorkBuddy 两版共用「新建任务」（TabItem，
    // SetFocus+Enter）；DSH「新建会话」、OpenSquilla「新建任务」为 Button，
    // 同样 SetFocus+Enter 落空输入框实测通过。其它宿主一律留空：没实测过
    // 就不塞默认值（ZCode 的新任务是逐项目行按钮，无单前项目入口，见文档）。
    if (exe.compare(QStringLiteral("WorkBuddy.exe"), Qt::CaseInsensitive) == 0 ||
        exe.compare(QStringLiteral("WorkBuddyAI.exe"), Qt::CaseInsensitive) == 0 ||
        exe.compare(QStringLiteral("OpenSquilla.exe"), Qt::CaseInsensitive) == 0) {
        return {QStringLiteral("新建任务")};
    }
    if (exe.compare(QStringLiteral("DeepSeek Harness.exe"),
                    Qt::CaseInsensitive) == 0) {
        return {QStringLiteral("新建会话")};
    }
    return {};
}

Attempt focusControl(const QString &exe, const QStringList &names,
                     int timeoutMs) {
    Attempt attempt;
    if (exe.isEmpty()) {
        attempt.fail = QStringLiteral("NO-EXE");
        return attempt;
    }
    if (names.isEmpty()) {
        attempt.fail = QStringLiteral("NO-CONTROL-NAMES(exe=%1)").arg(exe);
        return attempt;
    }
    HWND hwnd = static_cast<HWND>(AgentWindow::mainWindow(exe));
    if (hwnd == nullptr) {
        attempt.fail = QStringLiteral("NO-WINDOW(exe=%1)").arg(exe);
        return attempt;
    }
    tagRECT r{};
    GetWindowRect(hwnd, &r);
    if ((r.right - r.left) < kMainWidthFloor) {
        wakeMainWindow(exe);
        Sleep(1200);
        hwnd = static_cast<HWND>(AgentWindow::mainWindow(exe));
        if (hwnd == nullptr) {
            attempt.fail = QStringLiteral("WAKE-LOST-WINDOW(%1x%2)")
                               .arg(r.right - r.left)
                               .arg(r.bottom - r.top);
            return attempt;
        }
        GetWindowRect(hwnd, &r);
        if ((r.right - r.left) < kMainWidthFloor) {
            attempt.fail = QStringLiteral("MINI-WINDOW(%1x%2 after wake)")
                               .arg(r.right - r.left)
                               .arg(r.bottom - r.top);
            return attempt;
        }
    }
    IUIAutomation *ui = automation();
    if (ui == nullptr) {
        attempt.fail = QStringLiteral("UIA-UNAVAILABLE(hr=0x%1)")
                           .arg(quint32(g_createHr), 8, 16, QLatin1Char('0'));
        return attempt;
    }

    const ULONGLONG deadline = GetTickCount64() + quint64(timeoutMs);
    PassResult pass;
    int waits = 0;
    while (true) {
        pass = scanOnce(ui, hwnd, names);
        if (pass.hit || pass.named >= kNamedFloor ||
            GetTickCount64() >= deadline) {
            break;
        }
        waits += 100;
        Sleep(100);
    }
    attempt.namedNodes = pass.named;
    if (!pass.hit) {
        if (pass.named < kNamedFloor) {
            attempt.fail = QStringLiteral("TREE-THIN(named=%1 waited=%2ms)")
                               .arg(pass.named)
                               .arg(waits);
        } else {
            attempt.fail = QStringLiteral("NO-CONTROL(candidates=%1 named=%2 "
                                          "waited=%3ms)")
                               .arg(names.join(QLatin1Char(',')))
                               .arg(pass.named)
                               .arg(waits);
        }
        if (pass.element != nullptr) {
            pass.element->Release();
        }
        return attempt;
    }

    const HRESULT sf = pass.element->SetFocus();
    fillFocus(ui, &attempt);
    pass.element->Release();
    if (FAILED(sf)) {
        attempt.fail = QStringLiteral("SETFOCUS-FAILED(hr=0x%1 want=%2 got=%3)")
                           .arg(quint32(sf), 8, 16, QLatin1Char('0'))
                           .arg(pass.hitName, attempt.landed);
        return attempt;
    }
    // 焦点真落到目标控件才算数：实测焦点可能还停在标题栏「最小化」按钮上。
    if (attempt.landed.compare(pass.hitName, Qt::CaseInsensitive) != 0) {
        attempt.fail = QStringLiteral("FOCUS-NOT-LANDED(want=%1 got=%2 "
                                      "type=%3)")
                           .arg(pass.hitName, attempt.landed)
                           .arg(attempt.landedType);
        return attempt;
    }
    attempt.focused = true;
    return attempt;
}

bool focusIsEditable(const QString &exe) {
    HWND hwnd = static_cast<HWND>(AgentWindow::mainWindow(exe));
    if (hwnd == nullptr) {
        return false;
    }
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    IUIAutomation *ui = automation();
    if (ui == nullptr) {
        return false;
    }
    IUIAutomationElement *el = nullptr;
    if (FAILED(ui->GetFocusedElement(&el)) || el == nullptr) {
        return false;
    }
    int focusedPid = 0;
    el->get_CurrentProcessId(&focusedPid);
    const int focusedType = controlTypeOf(el);
    el->Release();
    // 按进程限定：不限定的话焦点还在自己 App 里也会被读成「输入框就绪」。
    // 实测这两家主窗与焦点控件同 pid；真出现不同（子进程报 pid）就是假失败，
    // 走调用方的剪贴板兜底，方向是安全的。
    return quint32(focusedPid) == windowPid && focusedType == UIA_EditControlTypeId;
}

QString composerValue(const QString &exe) {
    HWND hwnd = static_cast<HWND>(AgentWindow::mainWindow(exe));
    if (hwnd == nullptr) {
        return QString();
    }
    IUIAutomation *ui = automation();
    if (ui == nullptr) {
        return QString();
    }
    IUIAutomationElement *root = nullptr;
    if (FAILED(ui->ElementFromHandle(hwnd, &root)) || root == nullptr) {
        return QString();
    }
    IUIAutomationCondition *cond = nullptr;
    QString out;
    VARIANT typeVar{};
    VariantInit(&typeVar);
    typeVar.vt = VT_I4;
    typeVar.lVal = UIA_EditControlTypeId;
    if (SUCCEEDED(ui->CreatePropertyCondition(UIA_ControlTypePropertyId,
                                              typeVar, &cond)) &&
        cond != nullptr) {
        IUIAutomationElement *el = nullptr;
        if (SUCCEEDED(root->FindFirst(TreeScope_Descendants, cond, &el)) &&
            el != nullptr) {
            IUIAutomationValuePattern *vp = nullptr;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId,
                                                  IID_PPV_ARGS(&vp))) &&
                vp != nullptr) {
                BSTR raw = nullptr;
                if (SUCCEEDED(vp->get_CurrentValue(&raw)) && raw != nullptr) {
                    out = QString::fromWCharArray(raw);
                    SysFreeString(raw);
                }
                vp->Release();
            }
            el->Release();
        }
        cond->Release();
    }
    root->Release();
    return out;
}

bool treeHasText(const QString &exe, const QString &text) {
    if (text.isEmpty()) {
        return false;
    }
    HWND hwnd = static_cast<HWND>(AgentWindow::mainWindow(exe));
    if (hwnd == nullptr) {
        return false;
    }
    IUIAutomation *ui = automation();
    if (ui == nullptr) {
        return false;
    }
    IUIAutomationElement *root = nullptr;
    if (FAILED(ui->ElementFromHandle(hwnd, &root)) || root == nullptr) {
        return false;
    }
    IUIAutomationCondition *cond = nullptr;
    bool hit = false;
    const std::wstring wide = text.toStdWString();
    VARIANT nameVar{};
    VariantInit(&nameVar);
    nameVar.vt = VT_BSTR;
    nameVar.bstrVal = SysAllocString(wide.c_str());
    const HRESULT condHr = ui->CreatePropertyCondition(UIA_NamePropertyId,
                                                       nameVar, &cond);
    VariantClear(&nameVar);
    if (SUCCEEDED(condHr) && cond != nullptr) {
        IUIAutomationElement *el = nullptr;
        hit = SUCCEEDED(root->FindFirst(TreeScope_Descendants, cond, &el)) &&
              el != nullptr;
        if (el != nullptr) {
            el->Release();
        }
        cond->Release();
    }
    root->Release();
    return hit;
}

bool focusComposer(const QString &exe) {
    HWND hwnd = static_cast<HWND>(AgentWindow::mainWindow(exe));
    if (hwnd == nullptr) {
        return false;
    }
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    IUIAutomation *ui = automation();
    if (ui == nullptr) {
        return false;
    }
    IUIAutomationElement *root = nullptr;
    if (FAILED(ui->ElementFromHandle(hwnd, &root)) || root == nullptr) {
        return false;
    }
    IUIAutomationCondition *cond = nullptr;
    VARIANT typeVar{};
    VariantInit(&typeVar);
    typeVar.vt = VT_I4;
    typeVar.lVal = UIA_EditControlTypeId;
    const HRESULT condHr =
        ui->CreatePropertyCondition(UIA_ControlTypePropertyId, typeVar, &cond);
    VariantClear(&typeVar);
    IUIAutomationElement *el = nullptr;
    if (SUCCEEDED(condHr) && cond != nullptr) {
        root->FindFirst(TreeScope_Descendants, cond, &el);
        cond->Release();
    }
    root->Release();
    if (el == nullptr) {
        return false;
    }
    tagRECT r{};
    const bool visible = SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) &&
                         (r.right - r.left) >= 8 && (r.bottom - r.top) >= 8;
    int elPid = 0;
    el->get_CurrentProcessId(&elPid);
    const HRESULT sf = visible && quint32(elPid) == windowPid
                           ? el->SetFocus()
                           : E_FAIL;
    el->Release();
    if (FAILED(sf)) {
        return false;
    }
    // 复查：焦点真在宿主进程内的 Edit 上才算落稳（SetFocus 返回 S_OK 不算）。
    IUIAutomationElement *focused = nullptr;
    if (FAILED(ui->GetFocusedElement(&focused)) || focused == nullptr) {
        return false;
    }
    int focusedPid = 0;
    focused->get_CurrentProcessId(&focusedPid);
    const bool ok = quint32(focusedPid) == windowPid &&
                    controlTypeOf(focused) == UIA_EditControlTypeId;
    focused->Release();
    return ok;
}

bool measuredChatHost(const QString &exe) {
    // 显式白名单：当前会话输入框定位（focusComposer）实测过的宿主才走默认
    // 续聊。不能由 controlNames 推导——登记了新任务按钮不代表输入框定位
    // 实测过（IDE 类第一个 Edit 可能是代码编辑器）。
    return exe.compare(QStringLiteral("WorkBuddy.exe"), Qt::CaseInsensitive) == 0 ||
           exe.compare(QStringLiteral("WorkBuddyAI.exe"), Qt::CaseInsensitive) == 0 ||
           exe.compare(QStringLiteral("Qoder.exe"), Qt::CaseInsensitive) == 0 ||
           exe.compare(QStringLiteral("Qoder CN.exe"), Qt::CaseInsensitive) == 0;
}

}  // namespace HostNewTask
