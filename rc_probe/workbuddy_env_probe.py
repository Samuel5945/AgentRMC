"""只读： dump 指定进程环境块里与宿主判定有关的变量（QODER*/WORKBUDDY*/CLAUDE* 等）。

用法: python rc_probe/workbuddy_env_probe.py WorkBuddy.exe
通过 PEB->ProcessParameters->Environment 读 UTF-16 双 null 结尾的环境块。
"""
import ctypes
import ctypes.wintypes as wt
import sys

ctypes.windll.shcore.SetProcessDpiAwareness(2)
k = ctypes.windll.kernel32
nt = ctypes.windll.ntdll
k.CreateToolhelp32Snapshot.restype = wt.HANDLE
k.OpenProcess.restype = wt.HANDLE
k.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
nt.NtQueryInformationProcess.argtypes = [wt.HANDLE, ctypes.c_ulong, ctypes.c_void_p,
                                         ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]

name = sys.argv[1] if len(sys.argv) > 1 else "WorkBuddy.exe"


class PE32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


if name.isdigit():
    pids = [int(name)]
else:
    import subprocess
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-Process -Name '" + name.replace(".exe", "") +
         "').Id -join ','"],
        capture_output=True, text=True).stdout.strip()
    pids = [int(x) for x in out.split(",") if x.strip().isdigit()]
print("pids", pids)


class PBI(ctypes.Structure):
    _fields_ = [("Reserved1", ctypes.c_void_p), ("PebBaseAddress", ctypes.c_void_p),
                ("Reserved2", ctypes.c_void_p * 2), ("UniqueProcessId", ctypes.c_void_p),
                ("Reserved3", ctypes.c_void_p)]


def read_mem(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


for pid in pids[:1]:
    h = k.OpenProcess(0x0410, False, pid)  # QUERY_INFORMATION | VM_READ
    if not h:
        print("open failed", ctypes.GetLastError())
        continue
    pbi = PBI()
    ret = ctypes.c_ulong()
    st = nt.NtQueryInformationProcess(h, 0, ctypes.byref(pbi), ctypes.sizeof(pbi),
                                      ctypes.byref(ret))
    if st != 0:
        print("NtQueryInformationProcess status", hex(st & 0xFFFFFFFF))
        continue
    peb = read_mem(h, pbi.PebBaseAddress, 0x40)
    params = int.from_bytes(peb[0x20:0x28], "little")
    pp = read_mem(h, params, 0x90)
    env = int.from_bytes(pp[0x80:0x88], "little")
    raw = read_mem(h, env, 128 * 1024)
    k.CloseHandle(h)
    if raw is None:
        print("env read failed")
        continue
    text = raw.decode("utf-16-le", errors="replace")
    for entry in text.split("\0"):
        if not entry:
            break
        key = entry.split("=", 1)[0]
        if key.upper().startswith(("QODER", "WORKBUDDY", "CLAUDE", "CODEBUDDY",
                                   "CLIENT_INFO", "AGENTRMC")):
            print(entry)
