"""应用页图标瀑布流验收：把主窗切到 WebView 的「应用」页签并截图。

WebView2 页签是网页里的 button（文本「应用」），UIA 树里能直接找到；
点它切页签，再 PrintWindow 抓合成层。
用法: python rc_probe/apps_page_shot.py [输出前缀]
"""
import ctypes
import ctypes.wintypes as wt
import pathlib
import sys
import time

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

sys.path.insert(0, str(pathlib.Path(__file__).parent))


def get_uia():
    import comtypes.client
    uiac = comtypes.client.GetModule("UIAutomationCore.dll")
    return uiac, comtypes.client.CreateObject(
        uiac.CUIAutomation, interface=uiac.IUIAutomation)


def pids_of_image(exe):
    kernel32 = ctypes.windll.kernel32

    class PE32(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                    ("th32ProcessID", wt.DWORD),
                    ("th32DefaultHeapID", ctypes.c_void_p),
                    ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                    ("th32ParentProcessID", wt.DWORD),
                    ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]

    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)
    entry = PE32()
    entry.dwSize = ctypes.sizeof(PE32)
    out = set()
    if kernel32.Process32FirstW(snap, ctypes.byref(entry)):
        while True:
            if entry.szExeFile.lower() == exe.lower():
                out.add(entry.th32ProcessID)
            if not kernel32.Process32NextW(snap, ctypes.byref(entry)):
                break
    kernel32.CloseHandle(snap)
    return out


def find_main_hwnd():
    pid = wt.DWORD()
    targets = pids_of_image("AgentRMCNative.exe")
    if not targets:
        return None
    wins = []
    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(h, _):
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value in targets and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            if buf.value == "AgentRMC Native":
                wins.append(h)
        return True

    user32.EnumWindows(CB(cb), 0)
    return wins[0] if wins else None


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "apps_page"
    out_dir = pathlib.Path("rc_probe")
    hwnd = find_main_hwnd()
    if hwnd is None:
        print("没有 AgentRMC Native 主窗（检查进程）")
        sys.exit(1)

    uiac, uia = get_uia()
    el = uia.ElementFromHandle(hwnd)

    # 在整棵树里找文本为「应用」的按钮（WebView 页签是 DOM 按钮，
    # UIA 把 Chrome 的 button 映射成 ControlType Button=50000）。
    walker = uia.RawViewWalker
    stack = [el]
    target = None
    visited = 0
    while stack and visited < 4000:
        node = stack.pop()
        visited += 1
        try:
            name = node.CurrentName
        except Exception:
            name = None
        if name == "应用":
            target = node
            break
        child = walker.GetFirstChildElement(node)
        while child:
            stack.append(child)
            child = walker.GetNextSiblingElement(child)
    if target is None:
        print("没找到「应用」页签按钮，先截当前页")
    else:
        # 页签按钮是 Chromium 的，Invoke 走 Win32：取 BoundingRectangle
        # 中心坐标，SendInput 发一次真实左键（不动用户鼠标，UIA 自己算位置）。
        rect = target.CurrentBoundingRectangle
        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.3)
        ctypes.windll.user32.SetCursorPos(cx, cy)
        MOUSEEVENTF_LEFTDOWN = 0x0002
        MOUSEEVENTF_LEFTUP = 0x0004
        ctypes.windll.user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        ctypes.windll.user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        print(f"已点击「应用」页签 ({cx},{cy})")
        time.sleep(1.2)

    user32.ShowWindow(hwnd, 5)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.4)
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    box = (r.left, r.top, r.right, r.bottom)
    w, h = box[2] - box[0], box[3] - box[1]

    bmp = gdi32.CreateCompatibleBitmap(user32.GetDC(0), w, h)
    dc = gdi32.CreateCompatibleDC(user32.GetDC(0))
    gdi32.SelectObject(dc, bmp)
    ok = user32.PrintWindow(hwnd, dc, 2)
    print("PrintWindow:", ok)
    if ok:
        class BI(ctypes.Structure):
            _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                        ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                        ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                        ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                        ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                        ("biClrImportant", wt.DWORD)]

        bmi = BI()
        bmi.biSize = ctypes.sizeof(BI)
        bmi.biWidth, bmi.biHeight = w, -h
        bmi.biPlanes, bmi.biBitCount, bmi.biCompression = 1, 32, 0
        buf = ctypes.create_string_buffer(w * h * 4)
        if gdi32.GetDIBits(dc, bmp, 0, h, buf, ctypes.byref(bmi), 0):
            from PIL import Image
            img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1)
            p = out_dir / f"{tag}.png"
            img.convert("RGB").save(p)
            print("saved", p)
        gdi32.DeleteDC(dc)
        gdi32.DeleteObject(bmp)


if __name__ == "__main__":
    main()
