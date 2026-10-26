"""用 WinRT 原生 API 直接枚举已连接 BLE 设备的 GATT 表（走系统缓存，不重新建链）。

bleak 的 connect() 需要设备在广播，而遥控器配对后只在按键时短暂广播，
所以这里用 BluetoothLEDevice.FromBluetoothAddressAsync + GetGattServicesAsync(Uncached)。

用法: python rc_probe/dump_gatt_winrt.py [MAC]
"""
import asyncio
import sys

from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice
from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattCharacteristicProperties,
)

DEFAULT_MAC = "70:FD:88:CE:60:EC"


def mac_to_int(mac: str) -> int:
    return int(mac.replace(":", "").replace("-", ""), 16)


def props_of(ch) -> str:
    p = ch.characteristic_properties
    names = []
    for name in ("read", "write", "write_without_response", "notify", "indicate",
                 "broadcast", "authenticated_signed_writes", "extended_properties"):
        if p & getattr(GattCharacteristicProperties, name.upper()):
            names.append(name)
    return ",".join(names)


async def main(mac: str):
    addr = mac_to_int(mac)
    print(f"BluetoothLEDevice.FromBluetoothAddressAsync(0x{addr:012X}) ...")
    dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
    if dev is None:
        print("设备对象为空（未配对 / 蓝牙未开）")
        return
    print(f"name={dev.name!r}  connection_status={dev.connection_status}\n")

    result = await dev.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
    print(f"GetGattServicesAsync -> status={result.status} count={result.services.size}\n")

    for svc in result.services:
        print(f"[SERVICE] {svc.uuid}  handle=0x{svc.attribute_handle:04X}")
        cres = await svc.get_characteristics_async(BluetoothCacheMode.UNCACHED)
        if cres.status != 0:
            print(f"    (chars status={cres.status})")
        for ch in cres.characteristics:
            print(f"    [CHAR] {ch.uuid}  handle=0x{ch.attribute_handle:04X}  ({props_of(ch)})")
            dres = await ch.get_descriptors_async(BluetoothCacheMode.UNCACHED)
            for d in dres.descriptors:
                print(f"        [DESC] {d.uuid}  handle=0x{d.attribute_handle:04X}")
        print()
    dev.close()


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MAC))
