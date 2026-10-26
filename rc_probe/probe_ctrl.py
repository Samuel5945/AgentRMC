"""探测 D1FF/A002 控制通道：往它写候选命令，看 A001 是否有音频/状态回应。

假设：遥控器按住语音键只是通知主机，真正开麦需要主机下发命令。
用法: python rc_probe/probe_ctrl.py [MAC]
"""
import asyncio
import sys
import time
from pathlib import Path

from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice
from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattClientCharacteristicConfigurationDescriptorValue,
    GattSession,
    GattWriteOption,
)
from winrt.windows.storage.streams import DataReader, DataWriter

DEFAULT_MAC = "70:FD:88:CE:60:EC"
SVC_D1FF = "0000d1ff-3c17-d293-8e48-14fe2e4da212"
CH_A001 = "0000a001-0000-1000-8000-00805f9b34fb"
CH_A002 = "0000a002-0000-1000-8000-00805f9b34fb"

OUT = Path(__file__).resolve().parent / "ctrl_probe.jsonl"
start = time.time()
notify_count = 0


def read_ibuffer(buf) -> bytes:
    if buf is None or buf.length == 0:
        return b""
    r = DataReader.from_buffer(buf)
    return bytes(r.read_byte() for _ in range(buf.length))


def make_handler(tag: str):
    def handler(sender, args):
        global notify_count
        raw = read_ibuffer(args.characteristic_value)
        notify_count += 1
        t = round(time.time() - start, 3)
        print(f"    << {tag} {t:7.3f} len={len(raw):3d}  {raw.hex(' ')}", flush=True)
        with open(OUT, "a", encoding="utf-8") as f:
            f.write(f'{{"t":{t},"ch":"{tag}","len":{len(raw)},"hex":"{raw.hex(" ")}"}}\n')

    return handler


async def write_cmd(ch, data: bytes, label: str) -> bool:
    w = DataWriter()
    w.write_bytes(data)
    buf = w.detach_buffer()
    try:
        await ch.write_value_async(buf, GattWriteOption.WRITE_WITH_RESPONSE)
        print(f"  -> 写入 [{label}] {data.hex(' ')}", flush=True)
        return True
    except Exception as e:  # noqa: BLE001
        print(f"  -> 写入 [{label}] 失败: {str(e)[:80]}", flush=True)
        return False


# 候选命令：ATVV 风格 + 常见开关
CANDIDATES = [
    ("audio_start_04", bytes([0x04])),
    ("on_01", bytes([0x01])),
    ("off_00", bytes([0x00])),
    ("start_08", bytes([0x08])),
    ("sync_0a", bytes([0x0a])),
    ("len16_01", bytes([0x01]) + bytes(15)),
    ("len16_04", bytes([0x04]) + bytes(15)),
]


async def main(mac: str):
    addr = int(mac.replace(":", "").replace("-", ""), 16)
    dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
    if dev is None:
        print("设备对象为空")
        return
    session = await GattSession.from_device_id_async(dev.bluetooth_device_id)
    session.maintain_connection = True
    print(f"已连接 {dev.name}")

    res = await dev.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
    svc = None
    for s in res.services:
        if str(s.uuid).lower() == SVC_D1FF:
            svc = s
            break
    if svc is None:
        print("没找到 D1FF 服务")
        return

    cres = await svc.get_characteristics_async(BluetoothCacheMode.UNCACHED)
    ch_a001 = ch_a002 = None
    for ch in cres.characteristics:
        u = str(ch.uuid).lower()
        if u == CH_A001:
            ch_a001 = ch
        elif u == CH_A002:
            ch_a002 = ch
    print(f"A001={'ok' if ch_a001 else 'missing'}  A002={'ok' if ch_a002 else 'missing'}")
    if ch_a001 is None or ch_a002 is None:
        return

    st = await ch_a001.write_client_characteristic_configuration_descriptor_async(
        GattClientCharacteristicConfigurationDescriptorValue.NOTIFY)
    print(f"订阅 A001 -> {st}")
    ch_a001.add_value_changed(make_handler("A001"))

    print("\n=== 依次写入候选命令，观察 A001 回应 ===")
    for label, data in CANDIDATES:
        before = notify_count
        await write_cmd(ch_a002, data, label)
        await asyncio.sleep(1.2)
        if notify_count > before:
            print(f"    ^ 收到 {notify_count - before} 个通知！")
        else:
            print("    (无回应)")

    print("\n=== 再写一轮，同时请按住语音键说话 ===")
    print("    你有 20 秒")
    for label, data in CANDIDATES:
        await write_cmd(ch_a002, data, label)
        await asyncio.sleep(2.0)

    print(f"\n共收到 {notify_count} 个通知")
    session.maintain_connection = False
    dev.close()


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MAC))
