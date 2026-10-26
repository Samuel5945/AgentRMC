"""尝试绕过 Windows HID 独占，直接拿 1812(HOGP) 服务句柄并订阅 Report 特征。

思路: 系统 get_gatt_services_async() 会对 1812 返回 AccessDenied，
但用 GattDeviceService.FromIdAsync(service_device_id) 可能拿到独立句柄。

用法: python rc_probe/gatt_hid.py [秒数]
"""
import asyncio
import sys
import time
from pathlib import Path

from winrt.windows.devices.bluetooth import BluetoothCacheMode
from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattClientCharacteristicConfigurationDescriptorValue,
    GattDeviceService,
)
from winrt.windows.devices.enumeration import DeviceInformation
from winrt.windows.storage.streams import DataReader

HOGP = "00001812-0000-1000-8000-00805f9b34fb"
OUT = Path(__file__).resolve().parent / "hogp_reports.jsonl"
start = time.time()
count = 0


def read_ibuffer(buf) -> bytes:
    if buf is None or buf.length == 0:
        return b""
    r = DataReader.from_buffer(buf)
    return bytes(r.read_byte() for _ in range(buf.length))


def main_handler(sender, args):
    global count
    raw = read_ibuffer(args.characteristic_value)
    count += 1
    t = round(time.time() - start, 3)
    print(f"{t:8.3f} len={len(raw):3d}  {raw.hex(' ')}", flush=True)
    with open(OUT, "a", encoding="utf-8") as f:
        f.write(f'{{"t":{t},"len":{len(raw)},"hex":"{raw.hex(" ")}"}}\n')


async def main(duration: float):
    all_devs = await DeviceInformation.find_all_async()
    cands = [d for d in all_devs
             if d.id.lower().startswith("\\\\?\\bthledevice#")
             and HOGP in d.id.lower()
             and "01000d_pid&3838" in d.id.lower()]
    print(f"找到 {len(cands)} 个 1812 服务实例")
    for d in cands:
        print(f"  {d.id}")

    ok_svc = None
    for d in cands:
        try:
            svc = await GattDeviceService.from_id_async(d.id)
        except Exception as e:  # noqa: BLE001
            print(f"  FromIdAsync 异常: {e}")
            continue
        if svc is None:
            print("  FromIdAsync 返回 None（无访问权限）")
            continue
        print(f"  FromIdAsync OK  device_access={svc.device_access_information}")
        res = await svc.get_characteristics_async(BluetoothCacheMode.UNCACHED)
        print(f"  get_characteristics -> status={res.status} count={res.characteristics.size}")
        if res.status == 0:
            for ch in res.characteristics:
                props = ch.characteristic_properties
                print(f"    {ch.uuid} handle=0x{ch.attribute_handle:04X} props=0x{props:02X}")
                if props & 0x10:  # NOTIFY
                    st = await ch.write_client_characteristic_configuration_descriptor_async(
                        GattClientCharacteristicConfigurationDescriptorValue.NOTIFY)
                    print(f"      subscribe -> {st}")
                    if st == 0:
                        ch.add_value_changed(main_handler)
                        ok_svc = svc

    if ok_svc is None:
        print("\n[!] 无法访问 HOGP 特征")
        return
    print(f"\n=== 监听 {duration:.0f}s ===\n", flush=True)
    await asyncio.sleep(duration)
    print(f"\n共 {count} 个报告")


if __name__ == "__main__":
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    asyncio.run(main(dur))
