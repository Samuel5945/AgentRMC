"""按 ATVV 协议探测遥控器语音通道（Google Android TV Voice over BLE）。

参考实现（均为实测跑通的项目）：
  flydog555/xiaomi-remote-voice-assistant   Windows / WinRT
  godarrenw/mi_remote_control               macOS / CoreBluetooth
  dailiuyi/xiaomi-remote-voice-linux        Linux / BlueZ

协议：
  ATVV 服务 AB5E0001-5A21-4F05-BC7D-AF01F617B664
    AB5E0002 = TX  主机写命令
    AB5E0003 = AUDIO 遥控器 notify（IMA-ADPCM 音频帧）
    AB5E0004 = CTL 遥控器 notify（状态帧）

  时序：
    订阅 0003 / 0004
    TX  GET_CAPS       0A 01 00 00 03 03
    CTL 0x0B 能力帧    （codec / 帧长）
    CTL 0x08 遥控器请求开麦（用户按下语音键时主动上报）
    TX  MIC_OPEN       0C 00
    CTL 0x04 流开始
    AUDIO 连续 ADPCM 帧
    CTL 0x0A 同步帧（重置 predictor / step_index）
    CTL 0x00 流结束（用户松手）
    TX  MIC_CLOSE      0D

极米这台（Realtek B951C）实测服务列表里**没有 AB5E**，D1FF/A001+A002 结构同形
但只在订阅瞬间回一个 16 字节全零包。本脚本先找 AB5E，找不到就退化试 D1FF，
把结果写进 atvv_raw.jsonl，方便换遥控器后复测。

用法:
    python rc_probe/atvv_probe.py                  # 默认 15 秒
    python rc_probe/atvv_probe.py --seconds 40
"""
from __future__ import annotations

import argparse
import asyncio
import json
import time
import wave
from pathlib import Path

from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice
from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattClientCharacteristicConfigurationDescriptorValue,
    GattSession,
    GattWriteOption,
)
from winrt.windows.storage.streams import DataReader, DataWriter

HERE = Path(__file__).resolve().parent
DEFAULT_MAC = "70:FD:88:CE:60:EC"

ATVV_SVC, ATVV_TX, ATVV_RX, ATVV_CTL = "ab5e0001", "ab5e0002", "ab5e0003", "ab5e0004"
D1FF_SVC, D1FF_A001, D1FF_A002 = "0000d1ff", "0000a001", "0000a002"

GET_CAPS_V1 = bytes([0x0A, 0x01, 0x00, 0x00, 0x03, 0x03])
GET_CAPS_V0 = bytes([0x0A, 0x00, 0x04, 0x00, 0x03])
MIC_OPEN = bytes([0x0C, 0x00])
MIC_CLOSE = bytes([0x0D])

STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8]

CTL_NAME = {
    0x00: "AUDIO_STOP 流结束",
    0x04: "AUDIO_START 流开始",
    0x08: "MIC_REQUEST 遥控器请求开麦",
    0x0A: "SYNC 同步帧",
    0x0B: "CAPS 能力帧",
}


class Adpcm:
    """跨包连续的 IMA-ADPCM 解码器，遇到 0x0A 同步帧要 reset。"""

    def __init__(self) -> None:
        self.predictor = 0
        self.step_index = 0

    def reset(self, predictor: int = 0, step_index: int = 0) -> None:
        self.predictor = predictor
        self.step_index = step_index

    def nibble(self, n: int) -> int:
        step = STEP_TABLE[self.step_index]
        diff = step >> 3
        if n & 1:
            diff += step >> 2
        if n & 2:
            diff += step >> 1
        if n & 4:
            diff += step
        pred = self.predictor - diff if n & 8 else self.predictor + diff
        self.predictor = max(-32768, min(32767, pred))
        self.step_index = max(0, min(88, self.step_index + INDEX_TABLE[n & 7]))
        return self.predictor

    def decode(self, data: bytes, high_first: bool = True) -> list[int]:
        out: list[int] = []
        for b in data:
            if high_first:
                out.append(self.nibble((b >> 4) & 0x0F))
                out.append(self.nibble(b & 0x0F))
            else:
                out.append(self.nibble(b & 0x0F))
                out.append(self.nibble((b >> 4) & 0x0F))
        return out


def read_buf(buf) -> bytes:  # noqa: ANN001
    if buf is None or buf.length == 0:
        return b""
    r = DataReader.from_buffer(buf)
    return bytes(r.read_byte() for _ in range(buf.length))


class Probe:
    def __init__(self, seconds: float) -> None:
        self.seconds = seconds
        self.start = time.time()
        self.audio: list[bytes] = []
        self.ctl: list[bytes] = []
        self.mic_requested = False
        self.stream_started = False
        self.raw_path = HERE / "atvv_raw.jsonl"
        self.loop: asyncio.AbstractEventLoop | None = None
        self.tx = None
        self.dec = Adpcm()
        self.last_sync = b""

    def t(self) -> float:
        return round(time.time() - self.start, 3)

    def _log(self, kind: str, raw: bytes) -> None:
        with open(self.raw_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"t": self.t(), "ch": kind, "len": len(raw),
                                "hex": raw.hex(" ")}) + "\n")

    def on_audio(self, _sender, args) -> None:  # noqa: ANN001
        raw = read_buf(args.characteristic_value)
        if not raw:
            return
        self.audio.append(raw)
        self._log("audio", raw)
        if len(self.audio) <= 5 or len(self.audio) % 50 == 0:
            print(f"    << AUDIO #{len(self.audio):4d} t={self.t():7.3f} "
                  f"len={len(raw):3d} {raw.hex(' ')[:56]}", flush=True)

    def on_ctl(self, _sender, args) -> None:  # noqa: ANN001
        raw = read_buf(args.characteristic_value)
        if not raw:
            return
        self.ctl.append(raw)
        self._log("ctl", raw)
        op = raw[0]
        print(f"    << CTL t={self.t():7.3f} {raw.hex(' ')}  "
              f"→ {CTL_NAME.get(op, f'0x{op:02X} 未知')}", flush=True)
        if op == 0x08:
            self.mic_requested = True
        elif op == 0x04:
            self.stream_started = True
        elif op == 0x0A:
            self.last_sync = raw

    def snapshot(self) -> str:
        return (f"音频包={len(self.audio)} 控制包={len(self.ctl)} "
                f"mic_requested={self.mic_requested} stream_started={self.stream_started}")


async def write(ch, data: bytes, label: str) -> bool:  # noqa: ANN001
    w = DataWriter()
    w.write_bytes(data)
    try:
        await ch.write_value_async(w.detach_buffer(), GattWriteOption.WRITE_WITH_RESPONSE)
        print(f"  -> TX [{label}] {data.hex(' ')}", flush=True)
        return True
    except Exception as e:  # noqa: BLE001
        print(f"  -> TX [{label}] 失败: {str(e)[:90]}", flush=True)
        return False


def save_wav(samples: list[int], rate: int, path: Path) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(int(s).to_bytes(2, "little", signed=True) for s in samples))


async def pick_target(dev):  # noqa: ANN001
    res = await dev.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
    services = {str(s.uuid).lower(): s for s in res.services}
    print(f"服务数 {len(services)}: {', '.join(sorted(u[:8] for u in services))}")

    tx = rx = ctl = None
    for u, s in services.items():
        if u.startswith(ATVV_SVC):
            cres = await s.get_characteristics_async(BluetoothCacheMode.UNCACHED)
            for ch in cres.characteristics:
                cu = str(ch.uuid).lower()
                if cu.startswith(ATVV_TX):
                    tx = ch
                elif cu.startswith(ATVV_RX):
                    rx = ch
                elif cu.startswith(ATVV_CTL):
                    ctl = ch
            return "ATVV (AB5E)", tx, rx, ctl

    for u, s in services.items():
        if u.startswith(D1FF_SVC):
            cres = await s.get_characteristics_async(BluetoothCacheMode.UNCACHED)
            for ch in cres.characteristics:
                cu = str(ch.uuid).lower()
                if cu.startswith(D1FF_A001):
                    rx = ch
                elif cu.startswith(D1FF_A002):
                    tx = ch
            return "D1FF 厂商私有（ATVV 同形，未验证）", tx, rx, rx
    return "", None, None, None


async def run(mac: str, seconds: float) -> int:
    probe = Probe(seconds)
    probe.raw_path.write_text("", encoding="utf-8")

    addr = int(mac.replace(":", "").replace("-", ""), 16)
    dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
    if dev is None:
        print("设备对象为空：遥控器没配对或没唤醒")
        return 2
    session = await GattSession.from_device_id_async(dev.bluetooth_device_id)
    session.maintain_connection = True
    print(f"已连接 {dev.name}\n")

    kind, tx, rx, ctl = await pick_target(dev)
    if tx is None or rx is None:
        print("\n既没有 AB5E 也没有 D1FF —— 这台遥控器没有可用的语音 GATT 通道。")
        session.maintain_connection = False
        dev.close()
        return 3

    print(f"\n目标: {kind}")
    print(f"  TX={tx.uuid}\n  RX={rx.uuid}\n  CTL={getattr(ctl, 'uuid', None)}")
    probe.tx = tx

    for ch, tag in ((rx, "RX"), (ctl, "CTL")):
        if ch is None:
            continue
        try:
            st = await ch.write_client_characteristic_configuration_descriptor_async(
                GattClientCharacteristicConfigurationDescriptorValue.NOTIFY)
            print(f"  订阅 {tag} -> status={st}")
        except Exception as e:  # noqa: BLE001
            print(f"  订阅 {tag} 失败: {str(e)[:70]}")

    rx.add_value_changed(probe.on_audio)
    if ctl is not None and ctl is not rx:
        ctl.add_value_changed(probe.on_ctl)
    elif ctl is rx:
        rx.add_value_changed(probe.on_ctl)

    print("\n[1] 查询能力 GET_CAPS")
    await write(tx, GET_CAPS_V1, "GET_CAPS v1")
    await asyncio.sleep(1.0)
    if not probe.ctl:
        await write(tx, GET_CAPS_V0, "GET_CAPS v0")
        await asyncio.sleep(1.0)
    print(f"    {probe.snapshot()}")

    print(f"\n[2] 采集 {seconds:.0f}s —— 现在按住遥控器语音键说话，松手后再按一次")
    deadline = time.time() + seconds
    opened = False
    while time.time() < deadline:
        # 遥控器按下语音键会上报 0x08，此时主机回 MIC_OPEN
        if probe.mic_requested and not opened:
            opened = True
            print(f"    收到开麦请求，回 MIC_OPEN (t={probe.t()})")
            await write(tx, MIC_OPEN, "MIC_OPEN")
        await asyncio.sleep(0.15)

    print("\n[3] 关麦")
    await write(tx, MIC_CLOSE, "MIC_CLOSE")
    await asyncio.sleep(0.4)

    session.maintain_connection = False
    dev.close()

    print(f"\n结果: {probe.snapshot()}")
    nonzero = [b for b in probe.audio if any(b)]
    print(f"  非全零音频包 {len(nonzero)}/{len(probe.audio)}")
    if not nonzero:
        print("  没有拿到音频。三种可能：")
        print("   · 遥控器不跑 ATVV（这台就是）—— 麦克风只对极米主机开放")
        print("   · 需要用户按下语音键才有 0x08 开麦请求（上面看 CTL 有没有 0x08）")
        print("   · 私有服务命令字不同，得抓极米主机与遥控器的空口包")
        return 1

    dec = Adpcm()
    samples: list[int] = []
    for pkt in nonzero:
        samples.extend(dec.decode(pkt))
    peak = max((abs(s) for s in samples), default=0)
    print(f"  解码 {len(samples)} 采样，峰值 {peak}")
    save_wav(samples, 16000, HERE / "atvv_16k.wav")
    print("  已保存 rc_probe/atvv_16k.wav（ATVV 标准是 16kHz 单声道）")
    return 0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mac", default=DEFAULT_MAC)
    ap.add_argument("--seconds", type=float, default=15.0)
    a = ap.parse_args()
    try:
        code = asyncio.run(run(a.mac, a.seconds))
    except KeyboardInterrupt:
        code = 130
    print(f"\n退出码 {code}")
    try:
        input("按回车关闭…")
    except (EOFError, OSError):
        pass


if __name__ == "__main__":
    main()
