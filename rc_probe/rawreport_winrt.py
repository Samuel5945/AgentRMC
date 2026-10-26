"""用 WinRT HID API 读遥控器原始输入报告（不需要管理员）。

用法: python rc_probe/rawreport_winrt.py [秒数]
"""
import asyncio
import json
import sys
import time
from pathlib import Path

from winrt.windows.devices.enumeration import DeviceInformation
from winrt.windows.devices.humaninterfacedevice import HidDevice
from winrt.windows.storage import FileAccessMode
from winrt.windows.storage.streams import DataReader

VID, PID = 0x000D, 0x3838
OUT = Path(__file__).resolve().parent / "raw_reports.jsonl"

start = time.time()
log = open(OUT, "a", encoding="utf-8")
count = 0


def read_ibuffer(buf) -> bytes:
    if buf is None or buf.length == 0:
        return b""
    reader = DataReader.from_buffer(buf)
    return bytes(reader.read_byte() for _ in range(buf.length))


def on_report(sender, args):
    global count
    raw = read_ibuffer(args.data)
    count += 1
    t = round(time.time() - start, 3)
    rid = args.report_id
    print(f"{t:8.3f} report_id={rid:3d} len={len(raw):3d}  {raw.hex(' ')}", flush=True)
    log.write(json.dumps({"t": t, "report_id": rid, "len": len(raw),
                          "hex": raw.hex(" ")}) + "\n")
    log.flush()


async def main(duration: float):
    all_devs = await DeviceInformation.find_all_async()
    devs = [d for d in all_devs
            if d.id.lower().startswith("\\\\?\\hid#") and "01000d_pid&3838" in d.id.lower()]
    print(f"找到 {len(devs)} 个遥控器 HID 接口")
    for d in devs:
        print(f"  {d.name}  {d.id}")

    opened = None
    for d in devs:
        try:
            hd = await HidDevice.from_id_async(d.id, FileAccessMode.READ)
        except Exception as e:  # noqa: BLE001
            print(f"  from_id_async 失败 {d.id[:60]}: {e}")
            continue
        if hd is None:
            print(f"  from_id_async 返回 None: {d.id[:60]}")
            continue
        try:
            ok = await hd.open_async()
        except Exception as e:  # noqa: BLE001
            print(f"  open_async 异常: {e}")
            continue
        print(f"  打开成功={ok}  usage=0x{hd.usage_page:04X}/0x{hd.usage_id:04X}")
        if ok:
            hd.input_report_received += on_report
            opened = hd

    if opened is None:
        print("\n[!] 所有接口都无法打开")
        return

    print(f"\n=== 监听 {duration:.0f}s，请按键/说话 ===\n", flush=True)
    await asyncio.sleep(duration)
    print(f"\n共 {count} 个报告")
    log.close()


if __name__ == "__main__":
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    asyncio.run(main(dur))
