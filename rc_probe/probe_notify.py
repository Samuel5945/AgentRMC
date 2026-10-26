"""监听遥控器所有 notify 特征，抓语音音频流。

用法:
    python rc_probe/probe_notify.py [MAC] [秒数]

抓到的每个通知包按 hex 打印并追加到 rc_probe/notify.jsonl，
音频包另外把 payload 写入 rc_probe/audio.bin 便于后续解码试听。
"""
import asyncio
import json
import sys
import time
from pathlib import Path

from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice
from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattClientCharacteristicConfigurationDescriptorValue,
    GattSession,
)
from winrt.windows.storage.streams import DataReader

DEFAULT_MAC = "70:FD:88:CE:60:EC"
ROOT = Path(__file__).resolve().parent

# 关心的特征：小米语音通道 / 6287 私有通道 / 电量
WATCH = {
    "0000a001-0000-1000-8000-00805f9b34fb": "MI_VOICE_TX",   # 遥控器 -> 主机 音频
    "0000a002-0000-1000-8000-00805f9b34fb": "MI_VOICE_CTL",  # 主机 -> 遥控器 控制
    "00006387-3c17-d293-8e48-14fe2e4da212": "P6287_TX",
    "00006487-3c17-d293-8e48-14fe2e4da212": "P6287_RX",
    "00002a19-0000-1000-8000-00805f9b34fb": "BATTERY",
}

start = time.time()
notify_log = open(ROOT / "notify.jsonl", "a", encoding="utf-8")
audio_fp = open(ROOT / "audio.bin", "ab")
stats = {}


def read_ibuffer(buf) -> bytes:
    if buf is None or buf.length == 0:
        return b""
    reader = DataReader.from_buffer(buf)
    return bytes(reader.read_byte() for _ in range(buf.length))


def make_handler(tag: str):
    def handler(sender, args):
        raw = read_ibuffer(args.characteristic_value)
        t = round(time.time() - start, 3)
        stats[tag] = stats.get(tag, 0) + 1
        rec = {"t": t, "ch": tag, "len": len(raw), "hex": raw.hex(" ")}
        line = json.dumps(rec, ensure_ascii=False)
        print(f"{t:8.3f} {tag:<12} len={len(raw):3d}  {raw.hex(' ')}", flush=True)
        notify_log.write(line + "\n")
        notify_log.flush()
        if tag == "MI_VOICE_TX" and len(raw) > 4:
            audio_fp.write(raw)
            audio_fp.flush()

    return handler


async def main(mac: str, duration: float):
    addr = int(mac.replace(":", "").replace("-", ""), 16)
    dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
    if dev is None:
        print("设备对象为空")
        return

    session = await GattSession.from_device_id_async(dev.bluetooth_device_id)
    session.maintain_connection = True
    print(f"已建立 GattSession: {dev.name}  status={dev.connection_status}")

    res = await dev.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
    subscribed = []
    for svc in res.services:
        cres = await svc.get_characteristics_async(BluetoothCacheMode.UNCACHED)
        if cres.status != 0:
            continue
        for ch in cres.characteristics:
            uuid = str(ch.uuid).lower()
            if uuid not in WATCH:
                continue
            tag = WATCH[uuid]
            props = ch.characteristic_properties
            if not (props & 0x10 or props & 0x20):  # NOTIFY or INDICATE
                continue
            try:
                cccd = (GattClientCharacteristicConfigurationDescriptorValue.NOTIFY
                        if props & 0x10
                        else GattClientCharacteristicConfigurationDescriptorValue.INDICATE)
                st = await ch.write_client_characteristic_configuration_descriptor_async(cccd)
                ch.add_value_changed(make_handler(tag))
                subscribed.append(tag)
                print(f"  订阅 {tag}  status={st}")
            except Exception as e:  # noqa: BLE001
                print(f"  订阅 {tag} 失败: {e}")

    print(f"\n=== 监听 {duration:.0f}s，请按遥控器语音键说话 ===\n", flush=True)
    await asyncio.sleep(duration)

    print("\n=== 统计 ===")
    for k, v in sorted(stats.items()):
        print(f"  {k:<12} {v} 包")
    if not stats:
        print("  没有收到任何通知")
    session.maintain_connection = False
    dev.close()
    notify_log.close()
    audio_fp.close()


if __name__ == "__main__":
    mac = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MAC
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    asyncio.run(main(mac, dur))
