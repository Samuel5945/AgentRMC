"""只读探测：遥控器在 Windows 蓝牙栈里的可见状态（asyncio 版）。

用法: .venv\\Scripts\\python.exe rc_probe/bt_status.py
结果逐行写入 rc_probe/bt_status.txt。
"""
from __future__ import annotations

import asyncio
from pathlib import Path

from winrt.windows.devices.enumeration import (
    DeviceInformation, DeviceInformationKind)

PROPS = [
    "System.Devices.Aep.DeviceAddress",
    "System.Devices.Aep.IsConnected",
    "System.Devices.Aep.ProtocolId",
    "System.Devices.Aep.Bluetooth.Le.IsConnectable",
    "System.Devices.Connected",
    "System.Devices.Paired",
    "System.Devices.ContainerId",
]
BTLE_SVC = "{e0cbf8cb-74ae-4909-87ba-e1f1cecc02e9}"
OUT = Path(__file__).with_name("bt_status.txt")


def line(text: str) -> None:
    with OUT.open("a", encoding="utf-8") as f:
        f.write(text + "\n")


def dump(infos, limit: int = 40) -> None:
    line(f"  共 {len(infos)} 个")
    for it in list(infos)[:limit]:
        p = it.properties
        line("  - " + " | ".join([
            f"name={it.name!r}",
            f"paired={p.get('System.Devices.Paired')}",
            f"connected={p.get('System.Devices.Connected')}",
            f"aepConn={p.get('System.Devices.Aep.IsConnected')}",
            f"leConn={p.get('System.Devices.Aep.Bluetooth.Le.IsConnectable')}",
            f"addr={p.get('System.Devices.Aep.DeviceAddress')}",
            f"id={it.id[:60]}",
        ]))


async def step(title: str, aqs: str, kind, seconds: float = 25.0) -> None:
    line(f"===== {title}")
    try:
        op = DeviceInformation.find_all_async(aqs, PROPS, kind)
        infos = await asyncio.wait_for(op, timeout=seconds)
        dump(infos)
    except asyncio.TimeoutError:
        line("  超时（该查询在本机不可用或过慢）")
    except Exception as e:  # noqa: BLE001
        line(f"  失败: {type(e).__name__}: {e}")


async def main() -> None:
    OUT.write_text("", encoding="utf-8")
    await step("已安装设备（含已配对）", "(System.Devices.Paired:=true)",
               DeviceInformationKind.DEVICE)
    await step("BLE 关联端点（含未配对、可发现）",
               f"(System.Devices.AepService.ProtocolId:=\"{BTLE_SVC}\")",
               DeviceInformationKind.ASSOCIATION_ENDPOINT_SERVICE)
    line("done")


if __name__ == "__main__":
    asyncio.run(main())
    print(OUT.read_text(encoding="utf-8")[:400])
