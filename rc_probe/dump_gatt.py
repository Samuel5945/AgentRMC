"""枚举已连接 BLE 遥控器的全部 GATT 服务/特征。

用法: python rc_probe/dump_gatt.py [MAC]
默认 MAC 70:FD:88:CE:60:EC (BLuetooth 4.0 RC, VID_000D/PID_3838)
"""
import asyncio
import sys

from bleak import BleakClient

DEFAULT_MAC = "70:FD:88:CE:60:EC"


async def main(addr: str):
    print(f"连接 {addr} ...")
    async with BleakClient(addr, timeout=20.0) as client:
        print(f"connected={client.is_connected}\n")
        for svc in client.services:
            print(f"[SERVICE] {svc.uuid}  {svc.description}")
            for ch in svc.characteristics:
                props = ",".join(ch.properties)
                print(f"    [CHAR] {ch.uuid}  handle={ch.handle}  ({props})  {ch.description}")
                for d in ch.descriptors:
                    print(f"        [DESC] {d.uuid}  handle={d.handle}")
            print()


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MAC))
