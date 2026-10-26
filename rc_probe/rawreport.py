r"""读取遥控器原始 HID 输入报告（256 字节透传包）。

普通权限会被拒（ACCESS_DENIED），需要管理员。
用法: 双击 tools\probe_admin.bat，或在管理员终端里
    python rc_probe/rawreport.py [秒数]

输出 rc_probe/raw_reports.jsonl，每行一个事件：
    {"event":"start"/"open"/"error"/"end", ...}   状态
    {"t":1.23,"n":256,"hex":"..."}                收到的报告
"""
import ctypes as C
import json
import sys
import time
from pathlib import Path

import hid

VID, PID = 0x000D, 0x3838
OUT = Path(__file__).resolve().parent / "raw_reports.jsonl"

kernel32 = C.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.restype = C.c_void_p
kernel32.CreateFileW.argtypes = [C.c_wchar_p, C.c_uint32, C.c_uint32, C.c_void_p,
                                 C.c_uint32, C.c_uint32, C.c_void_p]
kernel32.ReadFile.argtypes = [C.c_void_p, C.c_void_p, C.c_uint32,
                              C.POINTER(C.c_uint32), C.c_void_p]
kernel32.CloseHandle.argtypes = [C.c_void_p]

GENERIC_READ = 0x80000000
BUFSZ = 1024

log = open(OUT, "w", encoding="utf-8")


def emit(obj: dict) -> None:
    log.write(json.dumps(obj, ensure_ascii=False) + "\n")
    log.flush()


def fail(msg: str, code: int = 1) -> None:
    print(f"[!] {msg}", flush=True)
    emit({"event": "error", "msg": msg})
    log.close()
    sys.exit(code)


emit({"event": "start", "ts": time.strftime("%Y-%m-%d %H:%M:%S"), "argv": sys.argv})

targets = [d for d in hid.enumerate() if d["vendor_id"] == VID and d["product_id"] == PID]
if not targets:
    fail("没找到遥控器 HID 集合")
path = targets[0]["path"]
if isinstance(path, bytes):
    path = path.decode()
emit({"event": "path", "path": path})

h = kernel32.CreateFileW(path, GENERIC_READ, 3, None, 3, 0, None)
if not h or h == C.c_void_p(-1).value:
    err = C.get_last_error()
    fail(f"CreateFileW 失败 err={err}" + (" 需要管理员权限" if err == 5 else ""))

emit({"event": "open", "ok": True})
print("已打开设备，开始读原始报告", flush=True)

duration = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
t0 = time.time()
buf = C.create_string_buffer(BUFSZ)
nread = C.c_uint32(0)
count = 0
total_bytes = 0

print("=" * 56)
print("  按住遥控器语音键说话，说 3-5 秒后松手")
print("  重复 2-3 次；中间按几下方向键做对照")
print(f"  记录 {duration:.0f} 秒")
print("=" * 56, flush=True)

try:
    while time.time() - t0 < duration:
        ok = kernel32.ReadFile(C.c_void_p(h), buf, BUFSZ, C.byref(nread), None)
        if not ok:
            err = C.get_last_error()
            emit({"event": "read_error", "err": err})
            print(f"ReadFile 失败 err={err}", flush=True)
            break
        n = nread.value
        if n == 0:
            continue
        raw = buf.raw[:n]
        count += 1
        total_bytes += n
        t = round(time.time() - t0, 3)
        emit({"t": t, "n": n, "hex": raw.hex(" ")})
        print(f"{t:8.3f} n={n:4d}  {raw[:32].hex(' ')}{' ...' if n > 32 else ''}", flush=True)
finally:
    emit({"event": "end", "count": count, "total_bytes": total_bytes,
          "duration": round(time.time() - t0, 1)})
    print(f"\n共 {count} 个报告 / {total_bytes} 字节", flush=True)
    print(f"已写入 {OUT}", flush=True)
    kernel32.CloseHandle(C.c_void_p(h))
    log.close()
    sys.stdout.flush()
    import os
    os._exit(0)
