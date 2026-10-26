"""读取遥控器所有可读特征（设备信息 + 小米私有服务）。

用法: python rc_probe/read_all.py [MAC]
"""
import asyncio
import sys

from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice
from winrt.windows.storage.streams import DataReader

DEFAULT_MAC = "70:FD:88:CE:60:EC"

FRIENDLY = {
    "00002a29": "ManufacturerName",
    "00002a24": "ModelNumber",
    "00002a25": "SerialNumber",
    "00002a27": "HardwareRevision",
    "00002a26": "FirmwareRevision",
    "00002a28": "SoftwareRevision",
    "00002a23": "SystemId",
    "00002a2a": "IEEE11073",
    "00002a50": "PnPId",
    "00002a00": "DeviceName",
    "00002a01": "Appearance",
    "00002a04": "PeripheralPrefConnParams",
    "00002aa6": "CentralAddrResolution",
    "00002a19": "BatteryLevel",
}


async def read_char(ch):
    try:
        res = await ch.read_value_async(BluetoothCacheMode.UNCACHED)
    except Exception as e:  # noqa: BLE001
        return f"<异常 {e}>"
    if res.status != 0:
        return f"<status={res.status}>"
    buf = res.value
    if buf is None or buf.length == 0:
        return "<空>"
    reader = DataReader.from_buffer(buf)
    raw = bytes(reader.read_byte() for _ in range(buf.length))
    short = str(ch.uuid).split("-")[0].lower()
    label = FRIENDLY.get(short, "")
    try:
        txt = raw.decode("utf-8")
        printable = all(32 <= b < 127 or b in (9, 10, 13) for b in raw)
    except UnicodeDecodeError:
        printable = False
    hexs = raw.hex(" ")
    if printable:
        return f"{label:<24} hex=[{hexs}]  ascii={txt!r}"
    return f"{label:<24} hex=[{hexs}]  int={int.from_bytes(raw, 'little')}"


async def main(mac: str):
    addr = int(mac.replace(":", ""), 16)
    dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
    res = await dev.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
    for svc in res.services:
        cres = await svc.get_characteristics_async(BluetoothCacheMode.UNCACHED)
        readable = []
        for ch in cres.characteristics:
            if ch.characteristic_properties & 0x02:  # READ
                readable.append(ch)
        if not readable:
            continue
        print(f"[{svc.uuid}]")
        for ch in readable:
            print("   ", await read_char(ch))
        print()
    dev.close()


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MAC))
