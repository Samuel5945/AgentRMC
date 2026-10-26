"""AgentRMC 图形界面。

把 agentrc.py 的核心能力（Raw Input 监听 / 录音 / 转写 / 场景 / 指令）
包成可被前端调用的服务，用 pywebview 渲染 HTML 界面。

用法:
    python gui.py
"""
from __future__ import annotations

import json
import sys
import threading
import time
import traceback
from collections import deque
from pathlib import Path

import webview

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import agentrc as core  # noqa: E402

UI_PATH = HERE / "ui" / "index.html"


class Controller:
    """业务逻辑，前端通过 js_api 调用。"""

    def __init__(self):
        self.events: deque = deque(maxlen=800)
        self.seq = 0
        self.running = False
        self._thread: threading.Thread | None = None
        self._hwnd = None

        self.recorder = core.Recorder(core.CFG["voice"])
        self.asr = core.Transcriber(core.CFG["voice"]["asr_model"],
                                    core.CFG["voice"]["asr_language"])
        self.profiles = core.ProfileManager(core.load_profiles())
        self.rules = core.RuleEngine(core.load_commands())

        vk = core.CFG["voice_key"]
        self.voice_vk = int(vk["vkey"])
        self.confirm_ms = float(vk["confirm_window_ms"])
        self.silence_ms = float(vk["silence_stop_ms"])
        self.min_record_ms = float(vk["min_record_ms"])

        self.state = "idle"
        self.pending_since = 0.0
        self.record_started = 0.0
        self.last_voice_event = 0.0
        self.repeat_count = 0
        self.level = 0.0
        self.battery = None
        self.last_text = ""
        self.model_ready = False

    # ---------------- 事件 ----------------
    def emit(self, kind: str, **data) -> None:
        self.seq += 1
        self.events.append({"id": self.seq, "ts": time.strftime("%H:%M:%S"),
                            "kind": kind, **data})

    def poll(self, since: int) -> dict:
        return {
            "events": [e for e in self.events if e["id"] > since],
            "seq": self.seq,
            "state": self.state,
            "level": round(self.level, 3),
            "profile": self.profiles.current().get("name", "?"),
            "profile_desc": self.profiles.describe(),
            "running": self.running,
            "model_ready": self.model_ready,
            "battery": self.battery,
            "last_text": self.last_text,
        }

    # ---------------- 生命周期 ----------------
    def start(self) -> dict:
        if self.running:
            return {"ok": False, "msg": "已在运行"}
        try:
            self._hwnd = core.start_listener()
        except Exception as e:  # noqa: BLE001
            self.emit("error", text=f"监听启动失败: {e}")
            return {"ok": False, "msg": str(e)}
        self.running = True
        self.emit("status", text="监听已启动")
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        threading.Thread(target=self._load_model, daemon=True).start()
        threading.Thread(target=self._read_battery, daemon=True).start()
        return {"ok": True}

    def stop(self) -> dict:
        self.running = False
        self.emit("status", text="监听已停止")
        return {"ok": True}

    def _load_model(self) -> None:
        self.emit("status", text="加载语音模型...")
        self.asr.load()
        self.model_ready = self.asr._model is not None
        self.emit("status", text="模型就绪" if self.model_ready else "模型加载失败")

    def _read_battery(self) -> None:
        try:
            import asyncio

            from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice

            async def read():
                addr = int(core.CFG.get("mac", "70:FD:88:CE:60:EC").replace(":", ""), 16)
                dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
                if dev is None:
                    return None
                res = await dev.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
                for svc in res.services:
                    if str(svc.uuid).lower().startswith("0000180f"):
                        cres = await svc.get_characteristics_async(BluetoothCacheMode.UNCACHED)
                        for ch in cres.characteristics:
                            if str(ch.uuid).lower().startswith("00002a19"):
                                v = await ch.read_value_async(BluetoothCacheMode.UNCACHED)
                                if v.status == 0 and v.value.length:
                                    from winrt.windows.storage.streams import DataReader
                                    r = DataReader.from_buffer(v.value)
                                    return r.read_byte()
                dev.close()
                return None

            self.battery = asyncio.run(read())
            self.emit("battery", value=self.battery)
        except Exception as e:  # noqa: BLE001
            self.emit("log", text=f"电量读取失败: {str(e)[:60]}")

    # ---------------- 主循环 ----------------
    def _loop(self) -> None:
        while self.running:
            try:
                core.pump()
                now = time.time()
                while core.key_events:
                    ev = core.key_events.popleft()
                    if ev["vkey"] == self.voice_vk:
                        if ev["down"]:
                            self._voice_down(now)
                        else:
                            self._voice_up(now)
                    elif ev["down"]:
                        self._on_key(ev["vkey"])
                self._tick(now)
                time.sleep(0.004)
            except Exception:  # noqa: BLE001
                self.emit("error", text=traceback.format_exc(limit=2))
                break

    # ---------------- 语音状态机 ----------------
    def _voice_down(self, now: float) -> None:
        if self.state == "idle":
            self.state = "pending"
            self.pending_since = now
            self.repeat_count = 1
        elif self.state == "pending":
            self.repeat_count += 1
            if self.repeat_count >= int(core.CFG["voice_key"]["min_repeats"]):
                self.state = "recording"
                self.record_started = now
                self.last_voice_event = now
                if self.recorder.start():
                    core.beep(1180, 60)
                    self.emit("voice_start")
                else:
                    self.state = "idle"
        elif self.state == "recording":
            self.last_voice_event = now

    def _voice_up(self, now: float) -> None:
        if self.state == "pending":
            self.state = "idle"
            core.tap_key(self.voice_vk)
        elif self.state == "recording":
            self.last_voice_event = now

    def _tick(self, now: float) -> None:
        if self.state == "pending" and (now - self.pending_since) * 1000 > self.confirm_ms:
            self.state = "idle"
            core.tap_key(self.voice_vk)
        if self.state == "recording":
            self.level = self.recorder.level
            idle_ms = (now - self.last_voice_event) * 1000
            if idle_ms > self.silence_ms:
                self._finish((now - self.record_started) * 1000)
        else:
            self.level = 0.0

    def _finish(self, dur_ms: float) -> None:
        self.state = "idle"
        audio = self.recorder.stop()
        self.level = 0.0
        core.beep(760, 60)
        if audio is None or dur_ms < self.min_record_ms:
            self.emit("voice_stop", dur=0, text="录音太短")
            return
        self.emit("voice_stop", dur=round(dur_ms / 1000, 1))
        threading.Thread(target=self._transcribe, args=(audio,), daemon=True).start()

    def _transcribe(self, audio) -> None:  # noqa: ANN001
        try:
            t0 = time.time()
            text = self.asr.transcribe(audio)
            cost = time.time() - t0
            if not text:
                self.emit("text", text="", cost=round(cost, 1), note="没有识别到内容")
                return
            self.last_text = text
            self.emit("text", text=text, cost=round(cost, 1))
            rule = self.rules.match(text)
            if rule is not None:
                self.rules.dispatch(text)
                self.emit("action", text="/".join(rule.get("match", [])[:1]),
                          act=rule.get("action", {}).get("type", "?"))
            elif core.CFG["voice"]["paste"]:
                core.paste_text(text, core.CFG["voice"]["auto_enter"])
                self.emit("paste", text=text)
        except Exception as e:  # noqa: BLE001
            self.emit("error", text=f"转写失败: {str(e)[:100]}")

    # ---------------- 按键 ----------------
    def _on_key(self, vkey: int) -> None:
        prof = self.profiles.current()
        name = prof.get("keys", {}).get(str(vkey))
        if not name:
            return
        action = prof.get("actions", {}).get(name)
        if not action:
            return
        kind = action.get("type")
        pname = prof.get("name", "?")
        try:
            if kind == "key":
                core.press_combo(action.get("mods", []), int(action["vk"]),
                                 int(action.get("repeat", 1)))
                self.emit("key", name=name, profile=pname,
                          detail=f"vk 0x{int(action['vk']):02X}")
            elif kind == "scroll":
                core.scroll(int(action["delta"]))
                self.emit("key", name=name, profile=pname, detail=f"scroll {action['delta']}")
            elif kind == "run":
                import subprocess
                subprocess.Popen(action["cmd"], shell=True)
                self.emit("key", name=name, profile=pname, detail="run")
        except Exception as e:  # noqa: BLE001
            self.emit("error", text=f"动作执行失败: {e}")


class Api:
    """暴露给 JS 的接口。"""

    def __init__(self):
        self.ctl = Controller()
        self.window = None

    def start(self):
        return self.ctl.start()

    def stop(self):
        return self.ctl.stop()

    def poll(self, since):
        return self.ctl.poll(int(since))

    def snapshot(self):
        return {
            "device": core.CFG.get("device_mark", ""),
            "voice_vk": self.ctl.voice_vk,
            "model": core.CFG["voice"]["asr_model"],
            "profiles": [p.get("name") for p in self.ctl.profiles.profiles],
            "rules": len(self.ctl.rules.rules),
            "mics": self._mics(),
        }

    def _mics(self):
        try:
            import sounddevice as sd
            return [d["name"] for d in sd.query_devices() if d["max_input_channels"] > 0]
        except Exception:  # noqa: BLE001
            return []

    def set_mic(self, index):
        try:
            core.CFG["voice"]["input_device"] = int(index)
            self.ctl.recorder.device = int(index)
            self.ctl.emit("status", text=f"麦克风已切换到 #{index}")
            return {"ok": True}
        except Exception as e:  # noqa: BLE001
            return {"ok": False, "msg": str(e)}

    def test_mic(self, seconds=4.0):
        threading.Thread(target=self._test_mic_worker, args=(float(seconds),),
                         daemon=True).start()
        return {"ok": True}

    def _test_mic_worker(self, seconds: float):
        try:
            import numpy as np
            import sounddevice as sd
            self.ctl.emit("status", text=f"录音测试 {seconds:.0f}s...")
            rec = sd.rec(int(seconds * 16000), samplerate=16000, channels=1,
                         dtype="float32", device=core.CFG["voice"]["input_device"])
            sd.wait()
            audio = rec.flatten()
            self.ctl.emit("status", text="转写中...")
            text = self.ctl.asr.transcribe(audio)
            self.ctl.emit("text", text=text or "", cost=0, note="麦克风测试")
            self.ctl.last_text = text
        except Exception as e:  # noqa: BLE001
            self.ctl.emit("error", text=f"麦克风测试失败: {e}")

    def minimize(self):
        if self.window:
            self.window.minimize()

    def close(self):
        self.ctl.stop()
        if self.window:
            self.window.destroy()


def main() -> None:
    api = Api()
    html = UI_PATH.read_text(encoding="utf-8")
    window = webview.create_window(
        "AgentRMC", html=html, js_api=api,
        width=980, height=680, min_size=(820, 560),
        background_color="#0b0e14",
    )
    api.window = window

    def bring_to_front() -> int:
        """pywebview 建窗后不一定在前台，主动找出来提到最前。"""
        import ctypes
        from ctypes import wintypes as wt

        u = ctypes.WinDLL("user32")
        found: list = []
        CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def cb(h, _l):
            n = u.GetWindowTextLengthW(h)
            if n:
                b = ctypes.create_unicode_buffer(n + 1)
                u.GetWindowTextW(h, b, n + 1)
                if b.value == "AgentRMC":
                    r = wt.RECT()
                    u.GetWindowRect(h, ctypes.byref(r))
                    if r.left > -1000:
                        found.append(h)
            return True

        u.EnumWindows(CB(cb), 0)
        for h in found:
            u.ShowWindow(h, 9)
            u.SetForegroundWindow(h)
        return len(found)

    def selftest():
        time.sleep(2.5)
        try:
            n = bring_to_front()
            print(f"[selftest] 窗口前置 {n} 个", flush=True)
        except Exception as e:  # noqa: BLE001
            print(f"[selftest] 前置失败: {e}", flush=True)
        time.sleep(1.2)
        try:
            state = window.evaluate_js(
                "JSON.stringify({seq:seq,running:running,"
                "nodes:document.querySelectorAll('.ev').length,"
                "dev:document.getElementById('i-device').textContent,"
                "prof:document.getElementById('pill-profile').textContent})")
            print(f"[selftest] JS 状态 {state}", flush=True)
        except Exception as e:  # noqa: BLE001
            print(f"[selftest] JS 失败: {e}", flush=True)

    webview.start(func=selftest, debug=False)


if __name__ == "__main__":
    main()
