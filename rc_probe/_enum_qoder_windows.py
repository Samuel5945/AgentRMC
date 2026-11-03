import ctypes, ctypes.wintypes as wt, subprocess, sys
from ctypes import byref, create_unicode_buffer
u = ctypes.WinDLL('user32'); u.SetProcessDPIAware()
exe = sys.argv[1] if len(sys.argv) > 1 else 'Qoder.exe'
res = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq %s' % exe, '/FO', 'CSV', '/NH'],
                     capture_output=True, text=True, encoding='mbcs', errors='ignore')
pids = set()
for line in res.stdout.splitlines():
    p = line.strip().split('","')
    if len(p) >= 2 and p[0].strip('"').lower() == exe.lower():
        pids.add(int(p[1].strip('"')))
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
def cb(h, l):
    pid = wt.DWORD(); u.GetWindowThreadProcessId(h, byref(pid))
    if pid.value in pids and u.IsWindowVisible(h):
        n = u.GetWindowTextLengthW(h); b = create_unicode_buffer(n + 1); u.GetWindowTextW(h, b, n + 1)
        c = create_unicode_buffer(256); u.GetClassNameW(h, c, 256)
        r = wt.RECT(); u.GetWindowRect(h, byref(r))
        print(h, repr(b.value), c.value, (r.left, r.top, r.right, r.bottom))
    return True
u.EnumWindows(CB(cb), 0)
import comtypes.client
comtypes.client.GetModule('UIAutomationCore.dll')
from comtypes.gen.UIAutomationClient import IUIAutomationElement
names = [m for m in dir(IUIAutomationElement) if 'attern' in m or 'Focus' in m]
print('METHODS:', names)
