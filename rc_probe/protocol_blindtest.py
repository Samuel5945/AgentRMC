"""Protocol blind test: does <scheme>://new-task actually do anything?

One shot per installed version, run separately so the on-screen reaction can be
attributed to one app.

Objective markers, because these hosts are opaque to UIA/MSAA:
  1. window inventory before/after (new top-level window, title change, resize)
  2. PrintWindow capture diff on the largest window (PW_RENDERFULLCONTENT, so an
     in-window route switch shows up as pixels even with no new window)

Capture self-checks for blankness: a GPU-composited window can PrintWindow to an
all-transparent/all-black bitmap, and a zero-filled pair would diff to 0.0000 and
read as "nothing happened". Such shots are reported as BLANK and the ratio is
marked unusable.

The pixel ratio is a *sensor*, not a verdict: caret blink and animated avatars
also change pixels, so --dry measures that baseline noise first.

usage:
  python protocol_blindtest.py --list=true --exe=WorkBuddy.exe
  python protocol_blindtest.py --dry=true --uri=x://y --exe=WorkBuddy.exe
  python protocol_blindtest.py --uri=workbuddy://new-task --exe=WorkBuddy.exe
"""
import sys
import time
import ctypes
import ctypes.wintypes
import hashlib
import os
from ctypes import byref, create_unicode_buffer

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
import winreg  # noqa: E402

PW_RENDERFULLCONTENT = 0x00000002
DIB_RGB_COLORS = 0

user32.SetProcessDPIAware()
user32.IsWindowVisible.restype = ctypes.wintypes.BOOL
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowThreadProcessId.restype = ctypes.wintypes.DWORD
user32.GetWindowDC.restype = ctypes.wintypes.HDC
user32.GetWindowDC.argtypes = [ctypes.wintypes.HWND]
user32.ReleaseDC.argtypes = [ctypes.wintypes.HWND, ctypes.wintypes.HDC]
user32.ReleaseDC.restype = ctypes.c_int
user32.PrintWindow.restype = ctypes.wintypes.BOOL
user32.PrintWindow.argtypes = [ctypes.wintypes.HWND, ctypes.wintypes.HDC,
                               ctypes.c_uint]
user32.GetWindowRect.argtypes = [ctypes.wintypes.HWND,
                                 ctypes.POINTER(ctypes.wintypes.RECT)]
user32.GetClassNameW.restype = ctypes.c_int
gdi32.CreateCompatibleDC.restype = ctypes.wintypes.HDC
gdi32.CreateCompatibleDC.argtypes = [ctypes.wintypes.HDC]
gdi32.CreateCompatibleBitmap.restype = ctypes.wintypes.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [ctypes.wintypes.HDC, ctypes.c_int,
                                         ctypes.c_int]
gdi32.SelectObject.restype = ctypes.wintypes.HGDIOBJ
gdi32.SelectObject.argtypes = [ctypes.wintypes.HDC, ctypes.wintypes.HGDIOBJ]
gdi32.DeleteObject.argtypes = [ctypes.wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [ctypes.wintypes.HDC]
gdi32.GetDIBits.restype = ctypes.c_int
gdi32.GetDIBits.argtypes = [ctypes.wintypes.HDC, ctypes.wintypes.HBITMAP,
                            ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p,
                            ctypes.c_void_p, ctypes.c_uint]

EnumWindowsProc = ctypes.WINFUNCTYPE(
    ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)


def arg(name, default=None):
    for a in sys.argv[1:]:
        if a.startswith("--%s=" % name):
            return a.split("=", 1)[1]
    return default


def pids_for(exe):
    import subprocess

    res = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq %s" % exe, "/FO", "CSV", "/NH"],
        capture_output=True, text=True, encoding="mbcs", errors="ignore")
    out = []
    for line in (res.stdout or "").splitlines():
        parts = line.strip().split('","')
        if len(parts) >= 2 and parts[0].strip('"').lower() == exe.lower():
            try:
                out.append(int(parts[1].strip('"')))
            except ValueError:
                pass
    return out


def windows_for(pids, min_width=0, include_invisible=False):
    """All titled top-level windows of these pids, keyed by hwnd."""
    hits = {}

    def cb(hwnd, _l):
        got = ctypes.wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, byref(got))
        if got.value not in pids:
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if not n:
            return True
        buf = create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        r = ctypes.wintypes.RECT()
        user32.GetWindowRect(hwnd, byref(r))
        cls = create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        w, h = r.right - r.left, r.bottom - r.top
        vis = bool(user32.IsWindowVisible(hwnd))
        if not include_invisible and not vis:
            return True
        if w < min_width:
            return True
        hits[int(hwnd)] = {"hwnd": int(hwnd), "title": buf.value,
                           "pid": got.value, "class": cls.value,
                           "rect": (r.left, r.top, r.right, r.bottom),
                           "w": w, "h": h, "area": max(0, w * h), "vis": vis}
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return hits


class BI(ctypes.Structure):
    _fields_ = [("biSize", ctypes.wintypes.DWORD),
                ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long),
                ("biPlanes", ctypes.wintypes.WORD),
                ("biBitCount", ctypes.wintypes.WORD),
                ("biCompression", ctypes.wintypes.DWORD)]


def capture(hwnd):
    r = ctypes.wintypes.RECT()
    user32.GetWindowRect(hwnd, byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None
    hwnd_dc = user32.GetWindowDC(hwnd)
    if not hwnd_dc:
        return None
    mem_dc = gdi32.CreateCompatibleDC(hwnd_dc)
    bmp = gdi32.CreateCompatibleBitmap(hwnd_dc, w, h)
    old = gdi32.SelectObject(mem_dc, bmp)
    printed = user32.PrintWindow(hwnd, mem_dc, PW_RENDERFULLCONTENT)
    bmi = BI()
    bmi.biSize = ctypes.sizeof(BI)
    bmi.biWidth, bmi.biHeight = w, h
    bmi.biPlanes, bmi.biBitCount, bmi.biCompression = 1, 32, 0
    buf = ctypes.create_string_buffer(w * h * 4)
    lines = gdi32.GetDIBits(mem_dc, bmp, 0, h, buf, byref(bmi), DIB_RGB_COLORS)
    gdi32.SelectObject(mem_dc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem_dc)
    user32.ReleaseDC(hwnd, hwnd_dc)
    if not lines:
        return {"error": "GetDIBits=0", "printed": printed}
    raw = buf.raw
    # distinct-colour probe: one colour means nothing was painted into it
    seen = set()
    step = 4 * 37
    for i in range(0, len(raw) - 3, step):
        seen.add(raw[i:i + 4])
        if len(seen) > 40:
            break
    opaque = sum(1 for i in range(3, len(raw), step) if raw[i] > 8)
    sampled = len(range(3, len(raw), step))
    blank = len(seen) <= 1 or (sampled and opaque / sampled < 0.02)
    return {"w": w, "h": h, "printed": printed, "colors": len(seen),
            "opaque_frac": round(opaque / max(sampled, 1), 3),
            "sha": hashlib.sha256(raw).hexdigest()[:12], "raw": raw,
            "blank": blank}


def diff_ratio(a, b):
    if not a or not b or "raw" not in a or "raw" not in b:
        return None, "no-shot"
    if a["blank"] or b["blank"]:
        return None, "blank-shot(colors=%d/%d, opaque=%s/%s)" % (
            a["colors"], b["colors"], a["opaque_frac"], b["opaque_frac"])
    if (a["w"], a["h"]) != (b["w"], b["h"]):
        return None, "resized %dx%d->%dx%d" % (a["w"], a["h"], b["w"], b["h"])
    sa, sb = a["raw"], b["raw"]
    step = 4 * 7
    changed = total = 0
    for i in range(0, len(sa) - 3, step):
        total += 1
        if sa[i:i + 4] != sb[i:i + 4]:
            changed += 1
    return (changed / max(total, 1)), "%d sampled" % total


def url_protocol(scheme):
    for hive, base in ((winreg.HKEY_CLASSES_ROOT, None),
                       (winreg.HKEY_CURRENT_USER, r"Software\Classes")):
        key = (base + "\\" + scheme) if base else scheme
        try:
            k = winreg.OpenKey(hive, key)
        except OSError:
            continue
        out = {}
        try:
            out["URLProtocol"] = winreg.QueryValueEx(k, "URL Protocol")[0]
        except OSError:
            out["URLProtocol"] = None
        try:
            out["command"] = winreg.QueryValue(
                winreg.OpenKey(k, r"shell\open\command"), "")
        except OSError:
            out["command"] = None
        return out
    return None


def show(wins, shots, label):
    print("  %s: %d window(s)" % (label, len(wins)))
    for v in sorted(wins.values(), key=lambda x: -x["area"]):
        s = shots.get(v["hwnd"])
        state = ("BLANK c=%s" % s["colors"]) if (s and s.get("blank")) else (
            "ok sha=%s c=%d op=%s" % (s["sha"], s["colors"], s["opaque_frac"])
            if s and "raw" in s else "FAIL(%s)" % (s or "none"))
        print("    hwnd=%-8d pid=%-6d %-10s %4dx%-4d vis=%d title=%r shot=%s"
              % (v["hwnd"], v["pid"], v["class"][:10], v["w"], v["h"],
                 1 if v["vis"] else 0, v["title"][:34], state))


def main():
    uri = arg("uri")
    exe = arg("exe")
    settle = float(arg("settle", "3.0"))
    if not exe:
        print(__doc__)
        return
    pids = set(pids_for(exe))
    if not pids:
        print("== %s: NOT RUNNING" % exe)
        return
    if arg("list"):
        wins = windows_for(pids, include_invisible=True)
        show(wins, {}, "inventory (incl. invisible)")
        return

    scheme = (uri or "").split("://", 1)[0]
    print("== %s  ->  %s  (registry[%s]=%s)" % (uri, exe, scheme,
                                                url_protocol(scheme)))
    before = windows_for(pids, min_width=200)
    shots = {k: capture(v["hwnd"]) for k, v in before.items()}
    show(before, shots, "before")

    if arg("dry"):
        print("  fire: DRY RUN (nothing sent, measuring baseline noise)")
    else:
        try:
            os.startfile(uri)
            print("  fire: os.startfile(%r) ok" % uri)
        except OSError as e:
            print("  fire: startfile raised %r" % e)
        time.sleep(settle)

    after = windows_for(pids, min_width=200)
    after_shots = {k: capture(v["hwnd"]) for k, v in after.items()}
    show(after, after_shots, "after")
    new = [v for k, v in after.items() if k not in before]
    gone = [v for k, v in before.items() if k not in after]
    for k, v in after.items():
        if k in before:
            d, note = diff_ratio(shots[k], after_shots[k])
            print("    diff hwnd=%d title_changed=%s pixel=%s (%s)"
                  % (v["hwnd"], before[k]["title"] != v["title"],
                     "n/a" if d is None else "%.4f" % d, note))
    if new:
        print("VERDICT: NEW-WINDOW %s" % [n["title"][:30] for n in new])
    elif gone:
        print("VERDICT: WINDOW-CLOSED %s" % [g["title"][:30] for g in gone])
    else:
        print("VERDICT: NO-NEW-WINDOW — read the pixel column; if it is n/a "
              "for blank shots, only the watching human can call this")


if __name__ == "__main__":
    main()
