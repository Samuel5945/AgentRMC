"""WinEvent 级抓小窗：SHOW 即截图，记类名/标题/owner/样式。

    .venv\\Scripts\\pythonw.exe rc_probe\\ghost_watch.py [seconds]

只关心屏幕上的小窗（<= 500x250），命中立即 BitBlt 那块区域存 PNG，
用来确认「137x51 小窗」到底是什么内容。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import sys
import time
from pathlib import Path

import win32gui
import win32process

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "rc_probe" / "ghost_hits"

u32 = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
try:
    u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:  # noqa: BLE001
    pass

EVENT_OBJECT_CREATE = 0x8000
EVENT_OBJECT_DESTROY = 0x8001
EVENT_OBJECT_SHOW = 0x8002
EVENT_OBJECT_HIDE = 0x8003
OBJID_WINDOW = 0
WINEVENT_OUTOFCONTEXT = 0x0000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
u32.GetWindow.argtypes = [wt.HWND, wt.UINT]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.GetParent.argtypes = [wt.HWND]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR,
                                           ctypes.POINTER(wt.DWORD)]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.CloseHandle.restype = wt.BOOL

WINEVENTPROC = ctypes.WINFUNCTYPE(None, wt.HANDLE, wt.DWORD, wt.HWND,
                                  ctypes.c_long, ctypes.c_long, wt.DWORD,
                                  wt.DWORD)
_exe_cache: dict[int, str] = {}
_hits = 0
_start = time.monotonic()
_watch_pid = None


def exe_name(pid: int) -> str:
    if pid in _exe_cache:
        return _exe_cache[pid]
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    name = "?"
    if h:
        buf = ctypes.create_unicode_buffer(512)
        n = wt.DWORD(512)
        if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            name = Path(buf.value).name
        k32.CloseHandle(h)
    _exe_cache[pid] = name
    return name


def class_title(hwnd):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, c, 256)
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, t, 256)
    return c.value, t.value


def grab(x, y, w, h, path: Path) -> None:
    hdc_screen = u32.GetDC(None)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    gdi32.SelectObject(hdc_mem, hbmp)
    gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, x, y, 0x00CC0020)

    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long),
                    ("biHeight", ctypes.c_long), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                    ("biYPelsPerMeter", ctypes.c_long),
                    ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]

    bi = BMIH()
    bi.biSize = ctypes.sizeof(BMIH)
    bi.biWidth = w
    bi.biHeight = -h
    bi.biPlanes = 1
    bi.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    lines = gdi32.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bi), 0)
    ok = lines == h
    if ok:
        from PySide6.QtGui import QImage
        img = QImage(bytes(buf.raw), w, h, w * 4, QImage.Format_RGB32)
        img.save(str(path))
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    u32.ReleaseDC(None, hdc_screen)
    if not ok:
        print("  grab failed", flush=True)


def handler(hook, event, hwnd, id_obj, id_child, thread, ms) -> None:
    global _hits
    if event != EVENT_OBJECT_SHOW or id_obj != OBJID_WINDOW or id_child != 0:
        return
    if not hwnd or not u32.IsWindowVisible(hwnd):
        return
    pid = wt.DWORD(0)
    u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if _watch_pid and pid.value != _watch_pid:
        return
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if not (20 <= w <= 500 and 20 <= h <= 250):
        return
    if r.left <= -14000 or r.top <= -14000:
        return
    name = exe_name(pid.value)
    cls, title = class_title(hwnd)
    if cls.startswith("tooltips_class32"):
        return
    now = time.monotonic() - _start
    owner = u32.GetWindow(hwnd, 4)  # GW_OWNER
    parent = u32.GetParent(hwnd)
    ex = u32.GetWindowLongW(hwnd, -20)
    style = u32.GetWindowLongW(hwnd, -16)
    _hits += 1
    print(f"{now:7.3f}s SHOW hwnd=0x{hwnd:X} {w}x{h}@{r.left},{r.top} "
          f"pid={pid.value} {name} owner=0x{owner:X} parent=0x{parent:X} "
          f"ex=0x{ex & 0xFFFFFFFF:X} style=0x{style & 0xFFFFFFFF:X} | {cls} | {title}",
          flush=True)
    OUT.mkdir(exist_ok=True)
    grab(max(0, r.left - 4), max(0, r.top - 4), min(w + 8, 500), min(h + 8, 250),
         OUT / f"ev_{_hits:03d}_{name}_{w}x{h}.png")


def main() -> None:
    global _watch_pid
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    if len(sys.argv) > 2:
        _watch_pid = int(sys.argv[2])
    proc = WINEVENTPROC(handler)
    hooks = []
    for ev in (EVENT_OBJECT_SHOW,):
        hooks.append(u32.SetWinEventHook(ev, ev, None, proc, 0, 0,
                                         WINEVENT_OUTOFCONTEXT))
    print(f"watch {secs}s pid={_watch_pid} -> {OUT}", flush=True)
    msg = wt.MSG()
    end = time.monotonic() + secs
    while time.monotonic() < end:
        while u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            u32.TranslateMessage(ctypes.byref(msg))
            u32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.002)
    for h in hooks:
        if h:
            u32.UnhookWinEvent(h)
    print(f"done {_hits} hits", flush=True)


if __name__ == "__main__":
    main()
