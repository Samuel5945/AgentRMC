"""遥控器语音通道探测：ATVV 协议 + 全通道嗅探 + 按键对照。

参考实现（均为实测跑通的项目）：
  flydog555/xiaomi-remote-voice-assistant   Windows / WinRT
  godarrenw/mi_remote_control               macOS / CoreBluetooth
  dailiuyi/xiaomi-remote-voice-linux        Linux / BlueZ

ATVV 协议：
  服务 AB5E0001-5A21-4F05-BC7D-AF01F617B664
    AB5E0002 TX    主机写命令
    AB5E0003 AUDIO 遥控器 notify，IMA-ADPCM 音频帧
    AB5E0004 CTL   遥控器 notify，状态帧
  时序：
    订阅 0003 / 0004
    TX  GET_CAPS       0A 01 00 00 03 03
    CTL 0x0B 能力帧
    （用户按下语音键）CTL 0x08 请求开麦
    TX  MIC_OPEN       0C 00
    CTL 0x04 流开始
    AUDIO 连续 ADPCM 帧
    CTL 0x0A 同步帧（重置 predictor / step_index）
    CTL 0x00 流结束（松手）
    TX  MIC_CLOSE      0D

本脚本做三件事：
  1. 按 ATVV 时序打一遍（找不到 AB5E 就退化试 D1FF 私有服务）
  2. 把**所有**能 notify/indicate 的特征全订阅上，看按住语音键时 BLE 上到底有没有
     任何字节流动 —— 这是判断"遥控器只是开关"最直接的证据
  3. 同时用 Raw Input 监听遥控器按键，把按键时刻和 BLE 帧对上，区分
     "没按" / "按了但 BLE 完全静默" / "按了有数据"

用法:
    python rc_probe/atvv_probe.py                  # 默认 20 秒
    python rc_probe/atvv_probe.py --seconds 40
    python rc_probe/atvv_probe.py --no-keys        # 不启动按键监听
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
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
ROOT = HERE.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

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

    def decode(self, data: bytes) -> list[int]:
        out: list[int] = []
        for b in data:
            out.append(self.nibble((b >> 4) & 0x0F))
            out.append(self.nibble(b & 0x0F))
        return out


def read_buf(buf) -> bytes:  # noqa: ANN001
    if buf is None or buf.length == 0:
        return b""
    r = DataReader.from_buffer(buf)
    return bytes(r.read_byte() for _ in range(buf.length))


class KeyWatch:
    """Raw Input 监听遥控器按键，用来区分"没按"和"按了但 BLE 没反应"。"""

    def __init__(self) -> None:
        self.hits: list[tuple[float, bool]] = []
        self.ready = False
        self.err = ""
        self._start = time.time()

    def start(self) -> None:
        import threading

        try:
            import agentrc as core
        except Exception as e:  # noqa: BLE001
            self.err = f"导入 agentrc 失败: {e}"
            return
        vk = int(core.CFG["voice_key"]["vkey"])
        downs = 0

        def loop() -> None:
            # 建窗和 pump 必须在同一个线程，否则 PeekMessage 取不到这个窗口的 WM_INPUT
            nonlocal downs
            try:
                core.start_listener()
            except Exception as e:  # noqa: BLE001
                self.err = f"Raw Input 启动失败: {e}"
                return
            self.ready = True
            print("[i] 按键监听已启动（语音键按下会打印 [KEY]）", flush=True)
            while True:
                core.pump()
                while core.key_events:
                    ev = core.key_events.popleft()
                    if ev["vkey"] != vk:
                        continue
                    t = round(time.time() - self._start, 2)
                    self.hits.append((t, ev["down"]))
                    if ev["down"]:
                        downs += 1
                        if downs <= 3 or downs % 20 == 0:
                            print(f"    [KEY] 语音键按下 #{downs} t={t}", flush=True)
                    else:
                        print(f"    [KEY] 语音键松开 t={t}", flush=True)
                time.sleep(0.004)

        threading.Thread(target=loop, daemon=True).start()
        for _ in range(60):
            if self.ready or self.err:
                break
            time.sleep(0.05)

    @property
    def presses(self) -> int:
        return sum(1 for _, d in self.hits if d)


class Probe:
    def __init__(self) -> None:
        self.start = time.time()
        self.frames: dict[str, list[bytes]] = {}
        self.mic_requested = False
        self.stream_started = False
        self.raw_path = HERE / "atvv_raw.jsonl"

    def t(self) -> float:
        return round(time.time() - self.start, 3)

    def _log(self, tag: str, raw: bytes) -> None:
        with open(self.raw_path, "a", encoding="utf-8") as f:
            f.write(json.dumps({"t": self.t(), "ch": tag, "len": len(raw),
                                "hex": raw.hex(" ")}) + "\n")

    def handler(self, tag: str, loud: bool = True):  # noqa: ANN201
        def cb(_sender, args) -> None:  # noqa: ANN001
            raw = read_buf(args.characteristic_value)
            if not raw:
                return
            bucket = self.frames.setdefault(tag, [])
            bucket.append(raw)
            self._log(tag, raw)
            if tag == "AUDIO" and raw[0] in CTL_NAME:
                op = raw[0]
                print(f"    << CTL t={self.t():7.3f} {raw.hex(' ')} "
                      f"→ {CTL_NAME[op]}", flush=True)
                if op == 0x08:
                    self.mic_requested = True
                elif op == 0x04:
                    self.stream_started = True
                return
            n = len(bucket)
            if loud and (n <= 6 or n % 40 == 0):
                print(f"    << {tag} #{n:4d} t={self.t():7.3f} "
                      f"len={len(raw):3d} {raw.hex(' ')[:52]}", flush=True)

        return cb

    def total(self) -> int:
        return sum(len(v) for v in self.frames.values())

    def snapshot(self) -> str:
        detail = " ".join(f"{k}={len(v)}" for k, v in self.frames.items()) or "无"
        return (f"帧数={self.total()}（{detail}） "
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


async def subscribe_all(dev, probe: Probe) -> list[tuple[str, object]]:
    """把所有 notify/indicate 特征全订阅，返回 [(tag, char)]。"""
    res = await dev.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
    print(f"\n[0] 全通道嗅探：发现 {len(res.services)} 个服务")
    subs: list[tuple[str, object]] = []
    for svc in res.services:
        su = str(svc.uuid).lower()
        try:
            cres = await svc.get_characteristics_async(BluetoothCacheMode.UNCACHED)
        except Exception as e:  # noqa: BLE001
            print(f"    {su[:8]} 取特征失败: {str(e)[:50]}")
            continue
        for ch in cres.characteristics:
            cu = str(ch.uuid).lower()
            try:
                props = int(ch.characteristic_properties)
            except Exception:  # noqa: BLE001
                props = 0
            if not props & 0x30:          # NOTIFY(0x10) | INDICATE(0x20)
                continue
            tag = cu[:8]
            kind = "notify" if props & 0x10 else "indicate"
            try:
                st = await ch.write_client_characteristic_configuration_descriptor_async(
                    GattClientCharacteristicConfigurationDescriptorValue.NOTIFY
                    if props & 0x10 else
                    GattClientCharacteristicConfigurationDescriptorValue.INDICATION)
                ch.add_value_changed(probe.handler(tag))
                subs.append((tag, ch))
                print(f"    订阅 {su[:8]}/{tag} ({kind}) -> status={st}")
            except Exception as e:  # noqa: BLE001
                print(f"    订阅 {su[:8]}/{tag} 失败: {str(e)[:60]}")
    return subs


async def pick_atvv(dev):  # noqa: ANN001
    """找 ATVV 或 D1FF 的 TX/RX，用来打协议时序。"""
    res = await dev.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
    services = {str(s.uuid).lower(): s for s in res.services}

    for u, s in services.items():
        if u.startswith(ATVV_SVC):
            tx = rx = ctl = None
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
            tx = rx = None
            cres = await s.get_characteristics_async(BluetoothCacheMode.UNCACHED)
            for ch in cres.characteristics:
                cu = str(ch.uuid).lower()
                if cu.startswith(D1FF_A001):
                    rx = ch
                elif cu.startswith(D1FF_A002):
                    tx = ch
            return "D1FF 厂商私有（ATVV 同形，未验证）", tx, rx, rx
    return "", None, None, None


async def run(mac: str, seconds: float, with_keys: bool) -> int:
    probe = Probe()
    probe.raw_path.write_text("", encoding="utf-8")

    keys = KeyWatch()
    if with_keys:
        keys.start()
        if keys.ready:
            print("[i] 按键监听已启动（会同时打印语音键按下/松开）")
        else:
            print(f"[!] 按键监听未启动: {keys.err}")

    addr = int(mac.replace(":", "").replace("-", ""), 16)
    dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
    if dev is None:
        print("设备对象为空：遥控器没配对或没唤醒")
        return 2
    session = await GattSession.from_device_id_async(dev.bluetooth_device_id)
    session.maintain_connection = True
    print(f"已连接 {dev.name}")

    await subscribe_all(dev, probe)

    kind, tx, rx, ctl = await pick_atvv(dev)
    if tx is None:
        print("\n没有 AB5E 也没有 D1FF，跳过协议时序，只做全通道嗅探。")
    else:
        print(f"\n[1] ATVV 时序目标: {kind}")
        print(f"    TX={tx.uuid}  RX={rx.uuid}")
        print("    查询能力 GET_CAPS")
        await write(tx, GET_CAPS_V1, "GET_CAPS v1")
        await asyncio.sleep(1.0)
        if not probe.total():
            await write(tx, GET_CAPS_V0, "GET_CAPS v0")
            await asyncio.sleep(1.0)
        print(f"    {probe.snapshot()}")

    print(f"\n[2] 采集 {seconds:.0f}s —— 现在按住遥控器语音键说句话，松手，再按一次")
    deadline = time.time() + seconds
    opened = False
    while time.time() < deadline:
        if probe.mic_requested and not opened and tx is not None:
            opened = True
            print(f"    收到开麦请求，回 MIC_OPEN (t={probe.t()})")
            await write(tx, MIC_OPEN, "MIC_OPEN")
        await asyncio.sleep(0.15)

    if tx is not None:
        print("\n[3] 关麦")
        await write(tx, MIC_CLOSE, "MIC_CLOSE")
        await asyncio.sleep(0.3)

    session.maintain_connection = False
    dev.close()

    # ---------------- 汇总 ----------------
    print("\n================ 结果 ================")
    print(f"语音键按下次数: {keys.presses if with_keys else '（未监听）'}")
    print(f"BLE 帧总数: {probe.total()}")
    for tag, frames in sorted(probe.frames.items()):
        nz = sum(1 for b in frames if any(b))
        print(f"  {tag}: {len(frames)} 帧，其中非全零 {nz} 帧")

    audio = probe.frames.get("AUDIO", [])
    if not audio:
        for tag, frames in probe.frames.items():
            if len(frames) > 3:
                audio = frames
                break

    nonzero = [b for b in audio if any(b)]
    if not nonzero:
        print("\n没拿到音频。结合上面的按键次数判断：")
        if with_keys and keys.presses == 0:
            print("  · 这次没检测到按键 —— 测试无效，重新跑并在提示后按住语音键")
        elif with_keys and keys.presses > 0 and probe.total() == 0:
            print("  · 按了键，但 BLE 上一个字节都没流 —— 遥控器确实只把按键事件报给主机，")
            print("    音频不在 BLE 上走（录音在投影仪/云端，遥控器只是开关），或者这条")
            print("    通道只对极米主机开放")
        elif with_keys and keys.presses > 0:
            print("  · 按了键也有 BLE 帧，但都不像音频 —— 需要看 atvv_raw.jsonl 里的原始帧")
        else:
            print("  · 没检测到音频，换个 --seconds 更长的窗口再试")
        return 1

    dec = Adpcm()
    samples: list[int] = []
    for pkt in nonzero:
        samples.extend(dec.decode(pkt))
    peak = max((abs(s) for s in samples), default=0)
    print(f"\n解码 {len(samples)} 采样，峰值 {peak}")
    save_wav(samples, 16000, HERE / "atvv_16k.wav")
    print("已保存 rc_probe/atvv_16k.wav")
    return 0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mac", default=DEFAULT_MAC)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--no-keys", action="store_true")
    a = ap.parse_args()
    try:
        code = asyncio.run(run(a.mac, a.seconds, not a.no_keys))
    except KeyboardInterrupt:
        code = 130
    print(f"\n退出码 {code}")
    try:
        input("按回车关闭…")
    except (EOFError, OSError):
        pass


if __name__ == "__main__":
    main()
