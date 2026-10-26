"""AgentRMC 图形界面（Qt / PySide6 版）。

不依赖 WebView2，打包成 exe 更稳。

用法:
    python agentrc_gui.py
"""
from __future__ import annotations

import json
import sys
import threading
import time
import traceback
from collections import deque
from pathlib import Path

from PySide6.QtCore import QPoint, Qt, QTimer
from PySide6.QtGui import QColor, QFont, QLinearGradient, QPainter, QPen
from PySide6.QtWidgets import (
    QApplication, QComboBox, QFrame, QHBoxLayout, QLabel, QMainWindow, QPushButton,
    QScrollArea, QSizePolicy, QTabWidget, QVBoxLayout, QWidget,
)

if getattr(sys, "frozen", False):
    HERE = Path(sys.executable).resolve().parent
else:
    HERE = Path(__file__).resolve().parent
EVENT_LOG = HERE / "rc_probe" / "gui_events.log"
sys.path.insert(0, str(HERE))

import agentrc as core  # noqa: E402

# --------------------------------------------------------------------------
# 配色
# --------------------------------------------------------------------------
BG = "#0b0e14"
CARD_TOP = "#121722"
CARD_BOT = "#161d2a"
LINE = "rgba(255,255,255,18)"
TEXT = "#e6edf3"
MUTED = "#8b949e"
BLUE = "#4f8cff"
PURPLE = "#7c5cff"
GREEN = "#3fb950"
ORANGE = "#d29922"
RED = "#f85149"

QSS = f"""
QWidget#root {{ background: {BG}; }}
QFrame#card {{
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 {CARD_TOP}, stop:1 {CARD_BOT});
    border: 1px solid rgba(255,255,255,18);
    border-radius: 16px;
}}
QLabel#cardTitle {{
    color: {MUTED}; font-size: 11px; font-weight: 600; letter-spacing: 1px;
}}
QLabel#brand {{ color: {TEXT}; font-size: 14px; font-weight: 700; }}
QLabel#result {{
    color: {TEXT}; font-size: 16px; background: rgba(255,255,255,9);
    border: 1px solid rgba(255,255,255,18); border-radius: 12px; padding: 14px 16px;
}}
QLabel#result[idle] {{ color: #5b6472; font-size: 13px; }}
QLabel#result[live] {{
    color: #ffb4b0; background: rgba(248,81,73,18);
    border: 1px solid rgba(248,81,73,115);
}}
QLabel#result[done] {{
    color: #b9f6c6; background: rgba(63,185,80,18);
    border: 1px solid rgba(63,185,80,89);
}}
QLabel#meta {{ color: {MUTED}; font-size: 11px; }}
QLabel#pill {{
    color: {MUTED}; font-size: 11px; background: rgba(255,255,255,11);
    border: 1px solid rgba(255,255,255,18); border-radius: 11px; padding: 4px 10px;
}}
QLabel#pillOn {{
    color: #b9f6c6; font-size: 11px; background: rgba(63,185,80,31);
    border: 1px solid rgba(63,185,80,89); border-radius: 11px; padding: 4px 10px;
}}
QLabel#rowKey {{ color: {MUTED}; font-size: 12px; }}
QLabel#rowVal {{ color: #c9d1d9; font-size: 12px; }}
QPushButton#primary {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {BLUE}, stop:1 {PURPLE});
    color: white; border: none; border-radius: 12px;
    font-size: 13px; font-weight: 600; padding: 11px 0;
}}
QPushButton#primary:hover {{ background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #6299ff, stop:1 #8d70ff); }}
QPushButton#primary[stopping="true"] {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #e5484d, stop:1 #b4232a);
}}
QPushButton#ghost {{
    background: rgba(255,255,255,13); color: {TEXT};
    border: 1px solid rgba(255,255,255,18); border-radius: 12px;
    font-size: 13px; font-weight: 600; padding: 11px 0;
}}
QPushButton#ghost:hover {{ background: rgba(255,255,255,23); }}
QPushButton#winBtn {{
    background: transparent; color: {MUTED}; border: none;
    border-radius: 7px; font-size: 13px;
}}
QPushButton#winBtn:hover {{ background: rgba(255,255,255,20); color: {TEXT}; }}
QPushButton#winBtn[close="true"]:hover {{ background: {RED}; color: white; }}
QPushButton#ghostSmall {{
    background: rgba(255,255,255,13); color: {TEXT};
    border: 1px solid rgba(255,255,255,18); border-radius: 8px;
    font-size: 11.5px; padding: 5px 12px;
}}
QPushButton#ghostSmall:hover {{ background: rgba(255,255,255,26); }}
QComboBox#combo {{
    background: rgba(255,255,255,13); color: {TEXT};
    border: 1px solid rgba(255,255,255,18); border-radius: 8px;
    font-size: 11.5px; padding: 4px 10px; min-width: 104px;
}}
QComboBox#combo:hover {{ background: rgba(255,255,255,26); }}
QComboBox#combo QAbstractItemView {{
    background: #161d2a; color: {TEXT}; border: 1px solid rgba(255,255,255,26);
    selection-background-color: rgba(79,140,255,72); outline: none;
}}
QTabWidget::pane {{ border: none; background: transparent; }}
QTabBar::tab {{
    background: rgba(255,255,255,13); color: {MUTED};
    padding: 6px 15px; border-radius: 9px; margin-right: 6px;
    font-size: 12px; border: 1px solid transparent;
}}
QTabBar::tab:selected {{
    background: rgba(79,140,255,46); color: {TEXT};
    border: 1px solid rgba(79,140,255,96);
}}
QTabBar::tab:hover {{ color: {TEXT}; }}
QScrollArea {{ background: transparent; border: none; }}
QScrollBar:vertical {{ background: transparent; width: 8px; margin: 4px 2px; }}
QScrollBar::handle:vertical {{ background: rgba(255,255,255,26); border-radius: 4px; min-height: 24px; }}
QScrollBar::handle:vertical:hover {{ background: rgba(255,255,255,46); }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
"""


# --------------------------------------------------------------------------
# 业务控制器（在后台线程跑 Raw Input 循环）
# --------------------------------------------------------------------------
class Controller:
    def __init__(self):
        self.events: deque = deque(maxlen=400)
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

        # 语音键两种用法：local=自己录音转写；hotkey=长按期间按住目标程序的"按住说话"键
        self.voice_mode = str(core.CFG["voice"].get("mode", "local")).lower()
        hk = core.CFG["voice"].get("hotkey") or {}
        self.hk_vk = int(hk.get("vk", 0x20))
        self.hk_mods = list(hk.get("mods", []))
        self.holding = False

    def emit(self, kind: str, **data) -> None:
        self.seq += 1
        rec = {"id": self.seq, "ts": time.strftime("%H:%M:%S"), "kind": kind, **data}
        self.events.append(rec)
        # 落盘一份，界面里看不到的细节（按键、状态机）靠这个排查
        try:
            with open(EVENT_LOG, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        except OSError:
            pass

    def drain(self, since: int) -> list:
        return [e for e in self.events if e["id"] > since]

    def start(self) -> tuple[bool, str]:
        if self.running:
            return False, "已在运行"
        try:
            self._hwnd = core.start_listener()
        except Exception as e:  # noqa: BLE001
            self.emit("error", text=f"监听启动失败: {e}")
            return False, str(e)
        self.running = True
        self.emit("status", text="监听已启动")
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        threading.Thread(target=self._load_model, daemon=True).start()
        threading.Thread(target=self._read_battery, daemon=True).start()
        return True, ""

    def stop(self) -> None:
        self.running = False
        self.emit("status", text="监听已停止")

    def _load_model(self) -> None:
        if self.voice_mode == "hotkey":
            # 语音键交给 Agent 自己识别，本地模型就不用加载了（省 462MB 内存 + 1s 启动）
            self.emit("status", text="hotkey 模式，跳过本地模型")
            return
        self.emit("status", text="加载语音模型…")
        self.asr.load()
        self.model_ready = self.asr._model is not None
        self.emit("status", text="模型就绪" if self.model_ready else "模型加载失败")

    def _read_battery(self) -> None:
        try:
            import asyncio

            from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice
            from winrt.windows.storage.streams import DataReader

            async def read():
                addr = int("70FD88CE60EC", 16)
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
                                    r = DataReader.from_buffer(v.value)
                                    return r.read_byte()
                dev.close()
                return None

            self.battery = asyncio.run(read())
            self.emit("battery", value=self.battery)
        except Exception as e:  # noqa: BLE001
            self.emit("log", text=f"电量读取失败: {str(e)[:50]}")

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
                if self.voice_mode == "hotkey":
                    core.hold_combo(self.hk_mods, self.hk_vk)
                    self.holding = True
                    core.beep(1180, 60)
                    self.emit("voice_start", mode="hotkey")
                elif self.recorder.start():
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
            if self.voice_mode == "hotkey":
                self._finish((now - self.record_started) * 1000)

    def _tick(self, now: float) -> None:
        if self.state == "pending" and (now - self.pending_since) * 1000 > self.confirm_ms:
            self.state = "idle"
            core.tap_key(self.voice_vk)
        if self.state == "recording":
            self.level = self.recorder.level
            if (now - self.last_voice_event) * 1000 > self.silence_ms:
                self._finish((now - self.record_started) * 1000)
        else:
            self.level = 0.0

    def _finish(self, dur_ms: float) -> None:
        self.state = "idle"
        if self.voice_mode == "hotkey":
            if self.holding:
                core.release_combo(self.hk_mods, self.hk_vk)
                self.holding = False
            self.level = 0.0
            core.beep(760, 60)
            self.emit("voice_stop", dur=round(dur_ms / 1000, 1), mode="hotkey")
            return
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
            self.emit("error", text=f"转写失败: {str(e)[:90]}")

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
                self.emit("key", name=name, profile=pname, detail=f"vk 0x{int(action['vk']):02X}")
            elif kind == "scroll":
                core.scroll(int(action["delta"]))
                self.emit("key", name=name, profile=pname, detail=f"scroll {action['delta']}")
            elif kind == "run":
                import subprocess
                subprocess.Popen(action["cmd"], shell=True)
                self.emit("key", name=name, profile=pname, detail="run")
        except Exception as e:  # noqa: BLE001
            self.emit("error", text=f"动作执行失败: {e}")

    def test_mic(self, seconds: float = 4.0) -> None:
        threading.Thread(target=self._test_mic_worker, args=(seconds,), daemon=True).start()

    def _test_mic_worker(self, seconds: float) -> None:
        try:
            import sounddevice as sd
            self.emit("status", text=f"录音测试 {seconds:.0f}s…")
            rec = sd.rec(int(seconds * 16000), samplerate=16000, channels=1,
                         dtype="float32", device=core.CFG["voice"]["input_device"])
            sd.wait()
            self.emit("status", text="转写中…")
            text = self.asr.transcribe(rec.flatten())
            self.emit("text", text=text or "", cost=0, note="麦克风测试")
            self.last_text = text
        except Exception as e:  # noqa: BLE001
            self.emit("error", text=f"麦克风测试失败: {e}")


# --------------------------------------------------------------------------
# 波形控件
# --------------------------------------------------------------------------
class Waveform(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(112)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.data = deque([0.0] * 72, maxlen=72)
        self.active = False

    def push(self, v: float, active: bool) -> None:
        self.data.append(v)
        self.active = active
        self.update()

    def paintEvent(self, _e) -> None:  # noqa: N802
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        mid = h / 2

        if not self.active and max(self.data, default=0.0) < 0.004:
            p.setPen(QPen(QColor(91, 100, 114, 200), 1))
            f = p.font()
            f.setPointSizeF(9.5)
            p.setFont(f)
            p.drawText(self.rect(), Qt.AlignCenter, "按住遥控器语音键 · 松手出字")
            return

        p.setPen(QPen(QColor(255, 255, 255, 12), 1))
        p.drawLine(0, int(mid), w, int(mid))

        n = len(self.data)
        gap = 2
        bw = max(2.0, (w - gap * (n - 1)) / n)
        for i, v in enumerate(self.data):
            amp = min(1.0, v * 9)
            if amp < 0.004:          # 静音时只留中线，不画成一排小点
                continue
            bh = max(1.5, amp * h * 0.42)
            x = i * (bw + gap)
            g = QLinearGradient(x, mid - bh, x, mid + bh)
            if self.active:
                g.setColorAt(0, QColor(248, 81, 73, 240))
                g.setColorAt(1, QColor(248, 81, 73, 90))
            else:
                g.setColorAt(0, QColor(79, 140, 255, 140))
                g.setColorAt(1, QColor(124, 92, 255, 45))
            p.setBrush(g)
            p.setPen(Qt.NoPen)
            p.drawRoundedRect(int(x), int(mid - bh), int(bw), int(bh * 2), bw / 2, bw / 2)


# --------------------------------------------------------------------------
# 事件条
# --------------------------------------------------------------------------
ACCENT = {
    "key": BLUE, "text": GREEN, "action": ORANGE, "paste": PURPLE,
    "error": RED, "voice_start": RED, "voice_stop": "#30363d",
    "status": "#30363d", "log": "#30363d", "battery": BLUE,
}


KEY_LABEL = {
    38: "↑", 40: "↓", 37: "←", 39: "→",
    13: "OK", 27: "返回", 93: "菜单", 36: "主页",
    116: "语音", 122: "变焦",
}


VK_NAME = {18: "ALT", 17: "CTRL", 16: "SHIFT", 91: "WIN", 32: "SPACE",
           13: "ENTER", 9: "TAB", 27: "ESC", 86: "V", 65: "A", 192: "`"}


def describe_hotkey(mods: list, vk: int) -> str:
    """把 (mods, vk) 说成人话，比如 长按 ALT / 长按 CTRL+SHIFT+V。"""
    names = [str(m).upper() for m in mods or []]
    names.append(VK_NAME.get(int(vk), f"VK 0x{int(vk):02X}"))
    return "长按 " + "+".join(names)


def describe_action(act: dict) -> str:
    """把动作描述成人话，给按键映射表用。"""
    if not act:
        return "无动作"
    kind = act.get("type")
    if kind == "key":
        mods = "+".join(str(m).upper() for m in act.get("mods", []) or [])
        vk = int(act.get("vk", 0))
        text = f"{mods}+" if mods else ""
        text += f"VK 0x{vk:02X}"
        rep = int(act.get("repeat", 1) or 1)
        if rep > 1:
            text += f" ×{rep}"
        return text
    if kind == "scroll":
        d = int(act.get("delta", 0))
        return f"滚轮{'↑' if d > 0 else '↓'} {abs(d)}"
    if kind == "run":
        cmd = str(act.get("cmd", ""))
        return f"启动 {cmd}" if cmd else "启动"
    if kind == "paste":
        return "粘贴" + ("并回车" if act.get("enter") else "")
    if kind == "enter":
        return "粘贴并回车"
    if kind == "none":
        note = str(act.get("note", "")).strip()
        return note or "直通（不拦截）"
    return str(kind)


class Dot(QLabel):
    """事件条左侧小圆点。用背景色画，不依赖字体里有没有 emoji 字形。"""

    def __init__(self, color: str, parent=None):
        super().__init__(parent)
        self.setFixedSize(7, 7)
        self.setStyleSheet(f"background: {color}; border-radius: 3px;")


class EventRow(QFrame):
    def __init__(self, ev: dict, parent=None):
        super().__init__(parent)
        kind = ev.get("kind", "log")
        accent = ACCENT.get(kind, "#30363d")
        self.setStyleSheet(
            f"background: rgba(255,255,255,0.028); border-left: 2px solid {accent};"
            f"border-radius: 8px;")
        lay = QHBoxLayout(self)
        lay.setContentsMargins(10, 7, 10, 7)
        lay.setSpacing(10)

        ico = Dot(accent)
        ico.setToolTip(kind)

        body = QLabel(self._text(ev))
        body.setStyleSheet("color: #c9d1d9; font-size: 12.5px; background: transparent;")
        body.setTextInteractionFlags(Qt.TextSelectableByMouse)

        ts = QLabel(ev.get("ts", ""))
        ts.setStyleSheet("color: #5b6472; font-size: 11px; background: transparent;")
        ts.setFixedWidth(58)
        ts.setAlignment(Qt.AlignRight | Qt.AlignVCenter)

        lay.addWidget(ico)
        lay.addWidget(body, 1)
        lay.addWidget(ts)

    @staticmethod
    def _text(ev: dict) -> str:
        k = ev.get("kind")
        if k == "key":
            return f"{ev.get('name')}  <span style='color:{MUTED}'>{ev.get('profile')} · {ev.get('detail','')}</span>"
        if k == "text":
            return (f"「{ev.get('text')}」  <span style='color:{MUTED}'>{ev.get('cost')}s</span>"
                    if ev.get("text") else f"<span style='color:{MUTED}'>{ev.get('note','空')}</span>")
        if k == "action":
            return f"执行指令 {ev.get('text')}  <span style='color:{MUTED}'>{ev.get('act')}</span>"
        if k == "paste":
            return "已粘贴到前台窗口"
        if k == "voice_start":
            return "已按住语音快捷键" if ev.get("mode") == "hotkey" else "开始录音"
        if k == "voice_stop":
            return f"录音结束 {ev.get('dur')}s" if ev.get("dur") else str(ev.get("text", ""))
        if k == "battery":
            return f"电量 {ev.get('value')}%"
        return str(ev.get("text", ""))


# --------------------------------------------------------------------------
# 主窗口
# --------------------------------------------------------------------------
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.ctl = Controller()
        self._seq = 0
        self._drag = None
        self._rows: list = []
        self._last_profile = ""

        self.setWindowTitle("AgentRMC")
        self.resize(1020, 700)
        self.setMinimumSize(900, 620)
        self.setWindowFlags(Qt.Window | Qt.FramelessWindowHint)
        self._build()
        self._refresh_info()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(70)

    # ---------- 构建 ----------
    def _build(self) -> None:
        root = QWidget()
        root.setObjectName("root")
        self.setCentralWidget(root)
        outer = QVBoxLayout(root)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        outer.addWidget(self._build_header())

        body = QWidget()
        grid = QHBoxLayout(body)
        grid.setContentsMargins(14, 12, 14, 14)
        grid.setSpacing(12)

        left = QVBoxLayout()
        left.setSpacing(10)
        left.addWidget(self._build_voice_card(), 1)
        left.addLayout(self._build_controls())
        left.addWidget(self._build_info_card())

        leftw = QWidget()
        leftw.setLayout(left)
        leftw.setFixedWidth(400)
        grid.addWidget(leftw)
        grid.addWidget(self._build_right(), 1)

        outer.addWidget(body, 1)

    def _build_right(self) -> QWidget:
        self.tabs = QTabWidget()
        self.tabs.setDocumentMode(True)
        self.tabs.addTab(self._build_log_card(), "事件流")
        self.tabs.addTab(self._build_keymap_card(), "按键映射")
        return self.tabs

    def _build_header(self) -> QWidget:
        bar = QFrame()
        bar.setFixedHeight(52)
        bar.setStyleSheet(
            f"background: rgba(13,17,26,230); border-bottom: 1px solid {LINE};")
        lay = QHBoxLayout(bar)
        lay.setContentsMargins(18, 0, 8, 0)
        lay.setSpacing(10)

        logo = QLabel()
        logo.setFixedSize(18, 18)
        logo.setStyleSheet(
            f"background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 {BLUE}, stop:1 {PURPLE});"
            f"border-radius: 6px;")
        brand = QLabel("AgentRMC")
        brand.setObjectName("brand")

        self.pill_status = QLabel("● 已停止")
        self.pill_status.setObjectName("pill")
        self.pill_battery = QLabel("--%")
        self.pill_battery.setObjectName("pill")
        self.pill_profile = QLabel("default")
        self.pill_profile.setObjectName("pill")

        lay.addWidget(logo)
        lay.addWidget(brand)
        lay.addStretch(1)
        lay.addWidget(self.pill_status)
        lay.addWidget(self.pill_battery)
        lay.addWidget(self.pill_profile)

        btn_min = QPushButton("─")
        btn_min.setObjectName("winBtn")
        btn_min.setFixedSize(34, 30)
        btn_min.clicked.connect(self.showMinimized)
        btn_close = QPushButton("✕")
        btn_close.setObjectName("winBtn")
        btn_close.setProperty("close", "true")
        btn_close.setFixedSize(34, 30)
        btn_close.clicked.connect(self.close)
        lay.addWidget(btn_min)
        lay.addWidget(btn_close)
        return bar

    def _build_voice_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(16, 13, 16, 14)
        lay.setSpacing(10)

        head = QHBoxLayout()
        title = QLabel("语音")
        title.setObjectName("cardTitle")
        self.rec_label = QLabel("")
        self.rec_label.setStyleSheet(f"color: {RED}; font-size: 11px; font-weight: 700;")
        head.addWidget(title)
        head.addStretch(1)
        head.addWidget(self.rec_label)
        lay.addLayout(head)

        self.wave = Waveform()
        lay.addWidget(self.wave, 1)

        self.result = QLabel("识别结果会显示在这里")
        self.result.setObjectName("result")
        self.result.setProperty("idle", "true")
        self.result.setWordWrap(True)
        self.result.setMinimumHeight(72)
        self.result.setMaximumHeight(128)
        self.result.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        self.result.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        lay.addWidget(self.result)

        self.meta = QLabel("")
        self.meta.setObjectName("meta")
        lay.addWidget(self.meta)
        return card

    def _build_controls(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(9)
        self.btn_toggle = QPushButton("开始监听")
        self.btn_toggle.setObjectName("primary")
        self.btn_toggle.setCursor(Qt.PointingHandCursor)
        self.btn_toggle.clicked.connect(self._toggle)

        self.btn_test = QPushButton("测试麦克风")
        self.btn_test.setObjectName("ghost")
        self.btn_test.setCursor(Qt.PointingHandCursor)
        self.btn_test.clicked.connect(self._test_mic)

        row.addWidget(self.btn_toggle, 1)
        row.addWidget(self.btn_test, 1)
        return row

    def _build_info_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(16, 12, 16, 12)
        lay.setSpacing(6)

        self.info_vals = {}
        for key, label in [("device", "设备"), ("vk", "语音键"),
                           ("vmode", "语音模式"), ("model", "模型"), ("rules", "指令")]:
            row = QHBoxLayout()
            k = QLabel(label)
            k.setObjectName("rowKey")
            v = QLabel("-")
            v.setObjectName("rowVal")
            v.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
            row.addWidget(k)
            row.addStretch(1)
            row.addWidget(v)
            lay.addLayout(row)
            self.info_vals[key] = v
        return card

    def _build_log_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(14, 12, 10, 12)
        lay.setSpacing(8)

        head = QHBoxLayout()
        title = QLabel("事件流")
        title.setObjectName("cardTitle")
        self.log_count = QLabel("")
        self.log_count.setStyleSheet(f"color: {MUTED}; font-size: 11px;")
        head.addWidget(title)
        head.addStretch(1)
        head.addWidget(self.log_count)
        lay.addLayout(head)

        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        holder = QWidget()
        holder.setStyleSheet("background: transparent;")
        self.log_lay = QVBoxLayout(holder)
        self.log_lay.setContentsMargins(0, 0, 4, 0)
        self.log_lay.setSpacing(5)
        self.log_lay.addStretch(1)
        self.scroll.setWidget(holder)
        lay.addWidget(self.scroll, 1)

        self.empty_hint = QLabel("等待事件…")
        self.empty_hint.setAlignment(Qt.AlignCenter)
        self.empty_hint.setStyleSheet("color: #4a5568; font-size: 12px; padding: 30px;")
        self.log_lay.insertWidget(0, self.empty_hint)
        return card

    def _build_keymap_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(14, 12, 14, 12)
        lay.setSpacing(9)

        head = QHBoxLayout()
        title = QLabel("按键映射")
        title.setObjectName("cardTitle")
        self.map_combo = QComboBox()
        self.map_combo.setObjectName("combo")
        self.map_combo.setCursor(Qt.PointingHandCursor)
        self.map_combo.addItem("跟随前台", None)
        for p in self.ctl.profiles.profiles:
            self.map_combo.addItem(p.get("name", "?"), p.get("name"))
        self.map_combo.currentIndexChanged.connect(self._refresh_keymap)
        head.addWidget(title)
        head.addStretch(1)
        head.addWidget(self.map_combo)
        lay.addLayout(head)

        self.map_match = QLabel("")
        self.map_match.setWordWrap(True)
        self.map_match.setStyleSheet(f"color: {MUTED}; font-size: 11.5px;")
        lay.addWidget(self.map_match)

        self.map_scroll = QScrollArea()
        self.map_scroll.setWidgetResizable(True)
        holder = QWidget()
        holder.setStyleSheet("background: transparent;")
        self.map_grid = QVBoxLayout(holder)
        self.map_grid.setContentsMargins(0, 0, 4, 0)
        self.map_grid.setSpacing(5)
        self.map_scroll.setWidget(holder)
        lay.addWidget(self.map_scroll, 1)

        foot = QHBoxLayout()
        foot.setSpacing(8)
        path = QLabel("profiles.json · 顺序即优先级")
        path.setStyleSheet("color: #5b6472; font-size: 11px;")
        btn_open = QPushButton("打开配置")
        btn_open.setObjectName("ghostSmall")
        btn_open.setCursor(Qt.PointingHandCursor)
        btn_open.clicked.connect(self._open_profiles)
        btn_reload = QPushButton("重新加载")
        btn_reload.setObjectName("ghostSmall")
        btn_reload.setCursor(Qt.PointingHandCursor)
        btn_reload.clicked.connect(self._reload_profiles)
        foot.addWidget(path)
        foot.addStretch(1)
        foot.addWidget(btn_open)
        foot.addWidget(btn_reload)
        lay.addLayout(foot)

        self._refresh_keymap()
        return card

    # ---------- 按键映射 ----------
    def _current_map_profile(self) -> tuple[dict, bool]:
        name = self.map_combo.currentData()
        if name is None:
            return self.ctl.profiles.current(), True
        for p in self.ctl.profiles.profiles:
            if p.get("name") == name:
                return p, False
        return self.ctl.profiles.current(), True

    def _refresh_keymap(self) -> None:
        while self.map_grid.count():
            item = self.map_grid.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()

        prof, auto = self._current_map_profile()
        match = prof.get("match", {}) or {}
        bits = []
        if match.get("process"):
            bits.append("进程 " + " / ".join(match["process"]))
        if match.get("title"):
            bits.append("标题含 " + " / ".join(match["title"]))
        rule = " · ".join(bits) if bits else "兜底，无匹配条件"
        self.map_match.setText(
            f"{'跟随前台 → ' if auto else '预览 '}{prof.get('name', '?')}　{rule}")

        keys = prof.get("keys", {}) or {}
        actions = prof.get("actions", {}) or {}
        if not keys:
            hint = QLabel("这个 profile 没有配置按键")
            hint.setStyleSheet(f"color: {MUTED}; font-size: 12px; padding: 16px;")
            self.map_grid.addWidget(hint)
            self.map_grid.addStretch(1)
            return
        for vk_s, sem in keys.items():
            try:
                vk = int(vk_s)
            except (TypeError, ValueError):
                continue
            self.map_grid.addWidget(self._map_row(vk, sem, actions.get(sem, {})))
        self.map_grid.addStretch(1)

    def _map_row(self, vk: int, sem: str, act: dict) -> QWidget:
        row = QFrame()
        row.setStyleSheet("background: rgba(255,255,255,7); border-radius: 8px;")
        lay = QHBoxLayout(row)
        lay.setContentsMargins(9, 6, 10, 6)
        lay.setSpacing(10)

        chip = QLabel(KEY_LABEL.get(vk, f"0x{vk:02X}"))
        chip.setFixedWidth(46)
        chip.setAlignment(Qt.AlignCenter)
        chip.setStyleSheet("color: #e6edf3; font-size: 11.5px; font-weight: 600;"
                           "background: rgba(255,255,255,18); border-radius: 6px;"
                           "padding: 3px 0;")

        name = QLabel(str(sem))
        name.setStyleSheet("color: #c9d1d9; font-size: 12.5px;")

        desc = QLabel(describe_action(act))
        desc.setStyleSheet(f"color: {MUTED}; font-size: 11.5px;")
        desc.setAlignment(Qt.AlignRight | Qt.AlignVCenter)

        lay.addWidget(chip)
        lay.addWidget(name)
        lay.addStretch(1)
        lay.addWidget(desc)
        return row

    def _open_profiles(self) -> None:
        path = HERE / "profiles.json"
        try:
            import os

            os.startfile(str(path))  # noqa: S606
            self._append({"kind": "status",
                          "text": "已打开 profiles.json，改完点『重新加载』",
                          "ts": time.strftime("%H:%M:%S")})
        except Exception as e:  # noqa: BLE001
            self._append({"kind": "error", "text": f"打开失败: {e}",
                          "ts": time.strftime("%H:%M:%S")})

    def _reload_profiles(self) -> None:
        try:
            self.ctl.profiles = core.ProfileManager(core.load_profiles())
            self.map_combo.blockSignals(True)
            self.map_combo.clear()
            self.map_combo.addItem("跟随前台", None)
            for p in self.ctl.profiles.profiles:
                self.map_combo.addItem(p.get("name", "?"), p.get("name"))
            self.map_combo.blockSignals(False)
            self._refresh_keymap()
            self._append({"kind": "status", "text": "按键映射已重新加载",
                          "ts": time.strftime("%H:%M:%S")})
        except Exception as e:  # noqa: BLE001
            self._append({"kind": "error", "text": f"重载失败: {e}",
                          "ts": time.strftime("%H:%M:%S")})

    # ---------- 行为 ----------
    def _refresh_info(self) -> None:
        c = self.ctl
        self.info_vals["device"].setText(core.CFG.get("device_mark", "-"))
        self.info_vals["vk"].setText(f"0x{c.voice_vk:02X}")
        mode = c.voice_mode
        if mode == "hotkey":
            self.info_vals["vmode"].setText(describe_hotkey(c.hk_mods, c.hk_vk))
        else:
            self.info_vals["vmode"].setText("本地 whisper")
        if c.voice_mode == "hotkey":
            self.info_vals["model"].setText("不加载（hotkey）")
        else:
            self.info_vals["model"].setText("whisper " + core.CFG["voice"]["asr_model"])
        self.info_vals["rules"].setText(f"{len(c.rules.rules)} 条")

    def _toggle(self) -> None:
        if self.ctl.running:
            self.ctl.stop()
            self._set_running(False)
        else:
            ok, msg = self.ctl.start()
            if ok:
                self._set_running(True)
            else:
                self._append({"kind": "error", "text": msg,
                              "ts": time.strftime("%H:%M:%S")})

    def _test_mic(self) -> None:
        self.ctl.test_mic(4)
        self._set_result("idle", "录音测试中，请对着麦克风说话…", "")

    def _set_running(self, on: bool) -> None:
        self.pill_status.setText("● 监听中" if on else "● 已停止")
        self.pill_status.setObjectName("pillOn" if on else "pill")
        self.pill_status.style().unpolish(self.pill_status)
        self.pill_status.style().polish(self.pill_status)
        self.btn_toggle.setText("停止监听" if on else "开始监听")
        self.btn_toggle.setProperty("stopping", "true" if on else "false")
        self.btn_toggle.style().unpolish(self.btn_toggle)
        self.btn_toggle.style().polish(self.btn_toggle)

    def _set_result(self, state: str, text: str, meta: str) -> None:
        self.result.setText(text)
        for p in ("idle", "live", "done"):
            self.result.setProperty(p, "true" if p == state else "false")
        self.result.style().unpolish(self.result)
        self.result.style().polish(self.result)
        self.meta.setText(meta)

    def _append(self, ev: dict) -> None:
        if self.empty_hint.isVisible():
            self.empty_hint.hide()
        row = EventRow(ev)
        self.log_lay.insertWidget(self.log_lay.count() - 1, row)
        self._rows.append(row)
        while len(self._rows) > 200:
            old = self._rows.pop(0)
            self.log_lay.removeWidget(old)
            old.deleteLater()
        self.log_count.setText(f"{len(self._rows)} 条")
        bar = self.scroll.verticalScrollBar()
        bar.setValue(bar.maximum())

    def _tick(self) -> None:
        c = self.ctl
        for ev in c.drain(self._seq):
            self._seq = ev["id"]
            self._append(ev)
            k = ev.get("kind")
            if k == "text" and ev.get("text"):
                self._set_result("done", ev["text"], f"识别耗时 {ev.get('cost')}s")
                self.rec_label.setText("")
            elif k == "voice_start":
                self._set_result("live", "正在聆听…", "松手后自动转写")
                self.rec_label.setText("● REC")
            elif k == "voice_stop":
                self.rec_label.setText("")
            elif k == "error":
                self._set_result("live", ev.get("text", "出错了"), "")

        self.wave.push(c.level, c.state == "recording")
        pname = c.profiles.current().get("name", "?")
        self.pill_profile.setText(pname)
        if pname != self._last_profile:
            self._last_profile = pname
            if self.map_combo.currentData() is None:   # 跟随前台时才自动刷
                self._refresh_keymap()
        if c.battery is not None:
            self.pill_battery.setText(f"{c.battery}%")
        if c.running != (self.btn_toggle.text() == "停止监听"):
            self._set_running(c.running)

    # ---------- 无边框拖动 ----------
    def mousePressEvent(self, e) -> None:  # noqa: N802
        if e.button() == Qt.LeftButton and e.position().y() < 52:
            self._drag = e.globalPosition().toPoint() - self.frameGeometry().topLeft()
        super().mousePressEvent(e)

    def mouseMoveEvent(self, e) -> None:  # noqa: N802
        if self._drag and e.buttons() & Qt.LeftButton:
            self.move(e.globalPosition().toPoint() - self._drag)
        super().mouseMoveEvent(e)

    def mouseReleaseEvent(self, e) -> None:  # noqa: N802
        self._drag = None
        super().mouseReleaseEvent(e)

    def closeEvent(self, e) -> None:  # noqa: N802
        self.ctl.stop()
        super().closeEvent(e)


def main() -> None:
    app = QApplication(sys.argv)
    app.setStyleSheet(QSS)
    win = MainWindow()
    win.show()

    def _arg(name: str) -> str | None:
        for i, a in enumerate(sys.argv):
            if a == name and i + 1 < len(sys.argv):
                return sys.argv[i + 1]
        return None

    # --shot 路径：渲染一帧存 PNG 后退出，用于无人值守验收界面
    shot = _arg("--shot")
    tab = _arg("--tab")
    if tab is not None:
        try:
            win.tabs.setCurrentIndex(1 if tab == "keymap" else int(tab))
        except (ValueError, IndexError):
            pass
    if shot:
        def _grab() -> None:
            win.grab().save(shot)
            print(f"[shot] saved {shot}", flush=True)
            app.quit()

        QTimer.singleShot(1500, _grab)

    # --autostart：启动就自动开始监听（不想每次手点按钮时用）
    if "--autostart" in sys.argv:
        QTimer.singleShot(500, win._toggle)

    # --selftest：真的拉起监听、等模型加载、读电量，然后截图退出
    if "--selftest" in sys.argv:
        out = shot or "rc_probe/qt_selftest.png"

        def _boot() -> None:
            ok, msg = win.ctl.start()
            print(f"[selftest] start -> ok={ok} msg={msg}", flush=True)

        def _report() -> None:
            c = win.ctl
            print(f"[selftest] running={c.running} state={c.state} "
                  f"model_ready={c.model_ready} battery={c.battery} "
                  f"events={c.seq} profile={c.profiles.current().get('name')} "
                  f"mics_ok={c.recorder is not None}", flush=True)
            for e in list(c.events)[-8:]:
                print(f"    {e}", flush=True)
            win.grab().save(out)
            print(f"[selftest] shot -> {out}", flush=True)
            c.stop()
            app.quit()

        QTimer.singleShot(400, _boot)
        QTimer.singleShot(9000, _report)

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
