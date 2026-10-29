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

from PySide6.QtCore import QPoint, QRect, QRectF, QSettings, QSize, Qt, QTimer
from PySide6.QtGui import (QColor, QCursor, QFont, QGuiApplication, QImage,
                           QLinearGradient, QPainter, QPen, QPixmap)
from PySide6.QtWidgets import (
    QApplication, QComboBox, QFrame, QHBoxLayout, QLabel, QMainWindow, QMessageBox,
    QPushButton, QScrollArea, QSizePolicy, QTabWidget, QVBoxLayout, QWidget,
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
    background: transparent; color: {MUTED};
    padding: 5px 12px; border-radius: 8px; margin-right: 2px;
    font-size: 12px; border: none;
}}
QTabBar::tab:selected {{
    background: rgba(79,140,255,38); color: {TEXT}; font-weight: 600;
}}
QTabBar::tab:hover {{ color: {TEXT}; }}
QPushButton#ghostText {{
    background: transparent; color: {MUTED}; border: none;
    font-size: 11.5px; padding: 3px 8px; border-radius: 6px;
}}
QPushButton#ghostText:hover {{ background: rgba(255,255,255,16); color: {TEXT}; }}
QScrollArea {{ background: transparent; border: none; }}
QScrollBar:vertical {{ background: transparent; width: 8px; margin: 4px 2px; }}
QScrollBar::handle:vertical {{ background: rgba(255,255,255,26); border-radius: 4px; min-height: 24px; }}
QScrollBar::handle:vertical:hover {{ background: rgba(255,255,255,46); }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
QLabel#meta {{ color: {MUTED}; font-size: 11.5px; }}
QLabel#metaOn {{ color: #7ee2a8; font-size: 11.5px; font-weight: 600; }}
QLabel#metaOff {{ color: {MUTED}; font-size: 11.5px; font-weight: 600; }}
QLabel#metaSep {{ color: #3d4653; font-size: 11.5px; }}
QLabel#badge {{
    color: {TEXT}; font-size: 11px; font-weight: 600;
    background: rgba(79,140,255,40); border: 1px solid rgba(79,140,255,96);
    border-radius: 10px; padding: 3px 9px;
}}
QLabel#badgeIdle {{
    color: {MUTED}; font-size: 11px;
    background: rgba(255,255,255,11); border: 1px solid rgba(255,255,255,18);
    border-radius: 10px; padding: 3px 9px;
}}
QLabel#foot {{ color: #5b6472; font-size: 11px; }}
QLabel#secTitle {{ color: {TEXT}; font-size: 13px; font-weight: 700; }}
QLabel#secBody {{ color: #b6c2cf; font-size: 12px; }}
QLabel#kbd {{
    color: {TEXT}; font-size: 11px; font-weight: 600;
    background: rgba(255,255,255,16); border: 1px solid rgba(255,255,255,24);
    border-radius: 6px; padding: 2px 7px;
}}
QLabel#mapRow {{ background: rgba(255,255,255,7); border-radius: 9px; }}
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

        # 语音键两种用法：local=自己录音转写；hotkey=转给目标程序（Qoder 的 Ctrl+Shift+V）
        self.voice_mode = str(core.CFG["voice"].get("mode", "local")).lower()
        self.active_voice = core.voice_hotkey(self.profiles.current())
        self.holding = False
        self.router = core.KeyRouter(self.profiles, emit=self.emit)

    def emit(self, kind: str, **data) -> None:
        self.seq += 1
        rec = {"id": self.seq, "ts": time.strftime("%H:%M:%S"), "kind": kind, **data}
        self.events.append(rec)
        # 落盘一份，界面里看不到的细节（按键、状态机）靠这个排查
        try:
            EVENT_LOG.parent.mkdir(parents=True, exist_ok=True)
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
        core.key_events.clear()          # 丢掉停止期间积压的按键
        self.router.reset()
        self.emit("status", text="监听已启动")
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        threading.Thread(target=self._load_model, daemon=True).start()
        threading.Thread(target=self._read_battery, daemon=True).start()
        return True, ""

    def stop(self) -> None:
        self.running = False
        self._release_holding(0)
        t = self._thread
        if t is not None and t is not threading.current_thread():
            t.join(timeout=0.6)
        self._thread = None
        self.router.reset()
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
                        self.router.key_down(ev["vkey"], now)
                    else:
                        self.router.key_up(ev["vkey"], now)
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
                    prof = self.profiles.current(force=True)
                    mods, vk, trigger = core.voice_hotkey(prof)
                    self.active_voice = (mods, vk, trigger)
                    exe, title = core.foreground_info()
                    fc = core.voice_focus_click(prof)
                    focused = None
                    if fc:
                        focused = core.click_foreground(*fc)
                        if focused:
                            time.sleep(0.02)
                    if trigger == "toggle":
                        core.tap_combo(mods, vk)   # Qoder：按下开录
                    else:
                        core.hold_combo(mods, vk)
                    self.holding = True
                    core.beep(1180, 60)
                    self.emit("voice_start", mode="hotkey", trigger=trigger,
                              focus_click=focused, fg=f"{exe}|{title[:24]}",
                              tray=core.taskbar_state(), quiet=True)
                elif self.recorder.start():
                    core.beep(1180, 60)
                    self.emit("voice_start", quiet=True)
                else:
                    self.state = "idle"
        elif self.state == "recording":
            self.last_voice_event = now

    def _voice_up(self, now: float) -> None:
        if self.state == "pending":
            # 短按一下：原生 F5 已经放行，不再补发，避免闪两次
            self.state = "idle"
        elif self.state == "recording":
            self.last_voice_event = now
            if self.voice_mode == "hotkey":
                self._finish((now - self.record_started) * 1000)

    def _tick(self, now: float) -> None:
        if self.state == "pending" and (now - self.pending_since) * 1000 > self.confirm_ms:
            self.state = "idle"
        if self.state == "recording":
            if self.voice_mode == "hotkey":
                self.level = 0.0
            else:
                self.level = self.recorder.level
            if (now - self.last_voice_event) * 1000 > self.silence_ms:
                self._finish((now - self.record_started) * 1000)
        else:
            self.level = 0.0
        self.router.tick(now)

    def _release_holding(self, dur_ms: float = 0) -> None:
        if not self.holding:
            return
        mods, vk, trigger = self.active_voice
        if trigger == "toggle":
            # 目标程序是"再按一次停止"，松手必须再发一次；给它一点时间进入录音态
            wait = max(0.0, 300.0 - dur_ms) / 1000
            if wait:
                time.sleep(wait)
            core.tap_combo(mods, vk)
        else:
            core.release_combo(mods, vk)
        self.holding = False

    def _finish(self, dur_ms: float) -> None:
        self.state = "idle"
        if self.voice_mode == "hotkey":
            trigger = self.active_voice[2]
            fg = core.foreground_info()[0]
            self._release_holding(dur_ms)
            self.level = 0.0
            core.beep(760, 60)
            self.emit("voice_stop", dur=round(dur_ms / 1000, 1), mode="hotkey",
                      trigger=trigger, fg=fg, tray=core.taskbar_state())
            return
        audio = self.recorder.stop()
        self.level = 0.0
        core.beep(760, 60)
        if audio is None or dur_ms < self.min_record_ms:
            self.emit("voice_stop", dur=0, text="录音太短", mode="local", quiet=True)
            return
        self.emit("voice_stop", dur=round(dur_ms / 1000, 1), mode="local", quiet=True)
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
                self.emit("paste", text=text, quiet=True)
        except Exception as e:  # noqa: BLE001
            self.emit("error", text=f"转写失败: {str(e)[:90]}")

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
        self.hint = "按住遥控器语音键 · 松手结束"

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
            p.drawText(self.rect(), Qt.AlignCenter, self.hint)
            return

        p.setPen(QPen(QColor(255, 255, 255, 12), 1))
        p.drawLine(0, int(mid), w, int(mid))

        # hotkey 模式没有本地音频数据：用一颗来回移动的红点表示"正在录"
        if self.active and max(self.data, default=0.0) < 0.004:
            phase = (time.monotonic() * 0.75) % 1.0
            x = 10 + phase * (w - 20)
            for k in range(3):
                alpha = 230 - k * 70
                r = 4.5 - k
                p.setBrush(QColor(248, 81, 73, alpha))
                p.setPen(Qt.NoPen)
                p.drawEllipse(int(x) - int(r), int(mid) - int(r), int(r * 2), int(r * 2))
            p.setPen(QPen(QColor(248, 81, 73, 110), 1))
            p.drawLine(0, int(mid), int(x) - 8, int(mid))
            return

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
    "error": RED, "voice_start": RED, "voice_stop": GREEN,
    "status": "#30363d", "log": "#30363d", "battery": BLUE,
}


KEY_LABEL = {
    38: "↑", 40: "↓", 37: "←", 39: "→",
    13: "OK", 27: "返回", 93: "菜单", 36: "主页",
    116: "语音", 122: "变焦",
}


VK_NAME = {18: "ALT", 17: "CTRL", 16: "SHIFT", 91: "WIN", 32: "SPACE",
           13: "ENTER", 9: "TAB", 27: "ESC", 86: "V", 65: "A", 192: "`"}


def describe_voice(mods: list, vk: int, trigger: str, short: bool = False) -> str:
    """把语音键接法说成人话，比如 开关 CTRL+SHIFT+V（按下开 / 松手关）。"""
    names = [str(m).upper() for m in mods or []]
    names.append(VK_NAME.get(int(vk), f"VK 0x{int(vk):02X}"))
    combo = "+".join(names)
    if trigger == "toggle":
        return f"开关 {combo}" if short else f"开关 {combo}（按下开 / 松手关）"
    return f"按住 {combo}"


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
        self.setObjectName("evRow")
        self.setStyleSheet(
            f"QFrame#evRow {{ background: rgba(255,255,255,0.028);"
            f"border-left: 2px solid {accent};"
            f"border-top-left-radius: 0; border-bottom-left-radius: 0;"
            f"border-top-right-radius: 8px; border-bottom-right-radius: 8px; }}")
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
            if ev.get("mode") != "hotkey":
                return "开始录音"
            text = ("已发送开始录音（开关）" if ev.get("trigger") == "toggle"
                    else "已按住语音快捷键")
            if ev.get("focus_click") is not None:
                text += " · 已点聊天框" + ("✓" if ev.get("focus_click") else "✗")
            return text
        if k == "voice_stop":
            if ev.get("mode") == "hotkey" and ev.get("dur"):
                how = "已停录" if ev.get("trigger") == "toggle" else "已放开"
                return f"语音 {ev.get('dur')}s · {how}"
            return f"录音结束 {ev.get('dur')}s" if ev.get("dur") else str(ev.get("text", ""))
        if k == "battery":
            return f"电量 {ev.get('value')}%"
        return str(ev.get("text", ""))


# --------------------------------------------------------------------------
# 主窗口
# --------------------------------------------------------------------------
class ModeToast(QWidget):
    """屏幕底部居中的轻提示：进/退音量、鼠标、导航模式时显示。"""

    def __init__(self):
        super().__init__(None, Qt.FramelessWindowHint | Qt.Tool
                         | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus)
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setAttribute(Qt.WA_ShowWithoutActivating)
        self.setAttribute(Qt.WA_TransparentForMouseEvents)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        self.label = QLabel("")
        self.label.setStyleSheet(
            "background: rgba(18,23,34,235); color: #e6edf3;"
            "border: 1px solid rgba(255,255,255,30); border-radius: 10px;"
            "padding: 10px 18px; font-size: 14px;")
        lay.addWidget(self.label)
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self.hide)

    def show_text(self, text: str) -> None:
        self.label.setText(text)
        self.adjustSize()
        scr = QGuiApplication.screenAt(QCursor.pos()) or QGuiApplication.primaryScreen()
        g = scr.availableGeometry()
        self.move(g.x() + (g.width() - self.width()) // 2,
                  g.y() + g.height() - self.height() - 56)
        self.show()
        self.raise_()
        self._timer.start(1400)


class RemotePanel(QWidget):
    """遥控器键位图：按下实时高亮，红外键标灰，模式内可用键描边。"""

    W, H = 258, 316
    # (vkey, x偏移(相对机身中心), y, 半径, 可用)
    KEYS = [(None, 0, 26, 8.5, False), (122, 0, 52, 8.5, True),
            (116, 0, 78, 9.5, True),
            (38, 0, 104, 9.5, True), (37, -26, 132, 9.5, True),
            (13, 0, 132, 13, True), (39, 26, 132, 9.5, True),
            (40, 0, 160, 9.5, True),
            (27, -28, 194, 9, True), (93, 0, 194, 9, True),
            (36, 28, 194, 9, True)]
    GLYPH = {38: "↑", 40: "↓", 37: "←", 39: "→", 13: "OK", 116: "🎙",
             27: "↩", 93: "☰", 36: "⌂", 122: "◎"}
    LABELS = [(26, "电源（红外）"), (52, "对焦 · 全屏"), (78, "语音 · 按住说话"),
              (132, "方向 · 滚动 / OK 发送"), (194, "返回 · 菜单 · 主页"),
              (236, "音量（红外）")]

    def __init__(self):
        super().__init__()
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.setMinimumSize(238, 280)
        self._lit: dict[int, float] = {}
        self.mode_keys: set[int] = set()

    def flash(self, vk: int | None) -> None:
        if vk:
            self._lit[int(vk)] = time.monotonic()
            self.update()

    def set_mode_keys(self, vks) -> None:
        self.mode_keys = set(vks)
        self.update()

    def refresh(self) -> None:
        if self._lit and time.monotonic() - max(self._lit.values()) > 0.32:
            self._lit.clear()
            self.update()

    def paintEvent(self, _e) -> None:  # noqa: N802
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        s = min(self.width() / self.W, self.height() / self.H)
        p.translate((self.width() - self.W * s) / 2,
                    (self.height() - self.H * s) / 2)
        p.scale(s, s)
        bx, by, bw, bh = 14.0, 6.0, 96.0, float(self.H - 12)
        cx = bx + bw / 2
        p.setPen(QPen(QColor(255, 255, 255, 26), 1))
        p.setBrush(QColor(20, 25, 35))
        p.drawRoundedRect(QRectF(bx, by, bw, bh), 22, 22)

        p.setPen(QPen(QColor(255, 255, 255, 22), 1))
        p.setBrush(QColor(255, 255, 255, 9))
        p.drawEllipse(QRectF(cx - 36, 96, 72, 72))

        for vk, dx, y, r, ok in self.KEYS:
            self._key(p, cx + dx, y, r, vk, ok)

        p.setPen(QPen(QColor(255, 255, 255, 22), 1, Qt.DashLine))
        p.setBrush(QColor(255, 255, 255, 8))
        p.drawRoundedRect(QRectF(cx - 9, 214, 18, 46), 9, 9)

        f = p.font()
        f.setPointSizeF(8.6)
        p.setFont(f)
        p.setPen(QPen(QColor(139, 148, 158)))
        for y, text in self.LABELS:
            p.drawText(QRectF(124, y - 9, self.W - 128, 18),
                       Qt.AlignVCenter | Qt.AlignLeft, text)

    def _key(self, p: QPainter, x: float, y: float, r: float,
             vk: int | None, enabled: bool, glyph_only: bool = True) -> None:
        lit = vk is not None and (time.monotonic() - self._lit.get(vk, -9)) < 0.32
        ringed = vk is not None and vk in self.mode_keys
        if lit:
            p.setBrush(QColor(79, 140, 255))
            p.setPen(QPen(QColor(120, 170, 255), 1))
        elif not enabled:
            p.setBrush(QColor(255, 255, 255, 8))
            p.setPen(QPen(QColor(255, 255, 255, 20), 1, Qt.DashLine))
        else:
            p.setBrush(QColor(255, 255, 255, 18))
            p.setPen(QPen(QColor(255, 255, 255, 28), 1))
        p.drawEllipse(QRectF(x - r, y - r, r * 2, r * 2))
        if ringed and not lit:
            p.setBrush(Qt.NoBrush)
            p.setPen(QPen(QColor(124, 92, 255, 200), 1.6))
            p.drawEllipse(QRectF(x - r - 3, y - r - 3, (r + 3) * 2, (r + 3) * 2))
        glyph = self.GLYPH.get(vk or -1, "")
        if glyph:
            f = p.font()
            f.setPointSizeF(8.4 if r < 12 else 9.2)
            p.setFont(f)
            p.setPen(QPen(QColor(230, 237, 243) if lit or enabled
                          else QColor(90, 100, 114)))
            p.drawText(QRectF(x - r - 6, y - r, (r + 6) * 2, r * 2),
                       Qt.AlignCenter, glyph)


def image_tile(name: str, px: int, caption: str, py: int | None = None) -> QWidget:
    """产品图/示意图：按图片自身明暗配深浅“纸片”，避免深底上刺眼。"""
    box = QWidget()
    lay = QVBoxLayout(box)
    lay.setContentsMargins(0, 0, 0, 0)
    lay.setSpacing(6)
    ph = py or px
    pic = QLabel()
    pic.setFixedSize(px, ph)
    pic.setAlignment(Qt.AlignCenter)
    path = HERE / "assets" / "remote" / name
    paper = "#f2f4f7"
    if path.exists():
        img = QImage(str(path))
        c = img.pixelColor(3, 3)
        if c.red() * 0.3 + c.green() * 0.6 + c.blue() * 0.1 < 90:
            paper = "#0f1420"          # 深色底图配深色纸片，不刺眼
    pic.setStyleSheet(f"background: {paper}; border: 1px solid rgba(255,255,255,26);"
                      "border-radius: 12px;")
    if path.exists():
        pm = QPixmap(str(path))
        pic.setPixmap(pm.scaled(px - 14, ph - 14, Qt.KeepAspectRatio,
                                Qt.SmoothTransformation))
    else:
        pic.setText("缺图\n" + name)
        pic.setStyleSheet(pic.styleSheet() + f"color: {MUTED};")
    cap = QLabel(caption)
    cap.setObjectName("meta")
    cap.setAlignment(Qt.AlignHCenter | Qt.AlignTop)
    cap.setWordWrap(True)
    lay.addWidget(pic)
    lay.addWidget(cap)
    return box


class GuideDialog(QWidget):
    """图文版使用引导：配对示意 + 实物键位，独立弹窗，不抢焦点也不挡操作。"""

    STEPS = [("1", "遥控器靠近要连接的设备"),
             ("2", "同时长按「返回」+「主页」约 3 秒"),
             ("3", "指示灯快闪后，在蓝牙列表里点遥控器"),
             ("4", "卡片状态变绿「已连接 · HID 在位」即可用")]

    def __init__(self, parent=None):
        super().__init__(parent, Qt.Window | Qt.FramelessWindowHint
                         | Qt.WindowStaysOnTopHint)
        self.setObjectName("guideRoot")
        self.setAttribute(Qt.WA_TranslucentBackground)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)

        card = QFrame()
        card.setObjectName("card")
        outer.addWidget(card)
        lay = QVBoxLayout(card)
        lay.setContentsMargins(20, 16, 20, 18)
        lay.setSpacing(14)

        head = QHBoxLayout()
        title = QLabel("使用引导")
        title.setStyleSheet(f"color: {TEXT}; font-size: 15px; font-weight: 700;")
        sub = QLabel("配对 · 常用手势")
        sub.setObjectName("meta")
        btn_close = QPushButton("✕  关闭")
        btn_close.setObjectName("ghostText")
        btn_close.setCursor(Qt.PointingHandCursor)
        btn_close.clicked.connect(self.close)
        head.addWidget(title)
        head.addSpacing(8)
        head.addWidget(sub)
        head.addStretch(1)
        head.addWidget(btn_close)
        lay.addLayout(head)

        row = QHBoxLayout()
        row.setSpacing(20)
        row.addWidget(image_tile("pairing.png", 300,
                                 "长按「返回」+「主页」进入配对", py=358),
                      0, Qt.AlignTop)

        right = QWidget()
        rlay = QVBoxLayout(right)
        rlay.setContentsMargins(0, 0, 0, 0)
        rlay.setSpacing(16)

        steps = QWidget()
        slay = QVBoxLayout(steps)
        slay.setContentsMargins(0, 0, 0, 0)
        slay.setSpacing(9)
        cap = QLabel("配对步骤")
        cap.setObjectName("secTitle")
        slay.addWidget(cap)
        for num, text in self.STEPS:
            line = QHBoxLayout()
            line.setSpacing(9)
            k = QLabel(num)
            k.setObjectName("kbd")
            k.setFixedWidth(26)
            k.setAlignment(Qt.AlignCenter)
            v = QLabel(text)
            v.setObjectName("secBody")
            v.setWordWrap(True)
            line.addWidget(k, 0, Qt.AlignTop)
            line.addWidget(v, 1)
            slay.addLayout(line)
        rlay.addWidget(steps)

        tips = QWidget()
        tlay = QVBoxLayout(tips)
        tlay.setContentsMargins(0, 0, 0, 0)
        tlay.setSpacing(9)
        cap2 = QLabel("常用手势")
        cap2.setObjectName("secTitle")
        tlay.addWidget(cap2)
        for k, v in [("语音键", "按住说话、松手结束"),
                     ("方向 / OK", "浏览与发送；导航模式内翻页"),
                     ("双击", "返回=音量·亮度 菜单=鼠标 主页=导航"),
                     ("红外键", "电源与音量拨杆电脑收不到")]:
            line = QHBoxLayout()
            line.setSpacing(9)
            kk = QLabel(k)
            kk.setObjectName("kbd")
            kk.setMinimumWidth(84)
            kk.setAlignment(Qt.AlignCenter)
            vv = QLabel(v)
            vv.setObjectName("secBody")
            vv.setWordWrap(True)
            line.addWidget(kk, 0, Qt.AlignTop)
            line.addWidget(vv, 1)
            tlay.addLayout(line)
        rlay.addWidget(tips)

        btn_bt = QPushButton("打开蓝牙设置")
        btn_bt.setObjectName("ghost")
        btn_bt.setCursor(Qt.PointingHandCursor)
        btn_bt.clicked.connect(core.open_bluetooth_settings)
        rlay.addWidget(btn_bt, 0, Qt.AlignLeft)
        rlay.addStretch(1)
        row.addWidget(right, 1)
        lay.addLayout(row)

        hint = self.sizeHint()
        self.setFixedSize(max(680, hint.width()), hint.height())

    def keyPressEvent(self, e) -> None:  # noqa: N802
        if e.key() == Qt.Key_Escape:
            self.close()
            return
        super().keyPressEvent(e)

    def mousePressEvent(self, e) -> None:  # noqa: N802
        if e.button() == Qt.LeftButton:
            wh = self.windowHandle()
            if wh is not None:
                wh.startSystemMove()
        super().mousePressEvent(e)


def guide_section(title: str, lines: list[tuple[str, str]]) -> QWidget:
    box = QFrame()
    lay = QVBoxLayout(box)
    lay.setContentsMargins(0, 0, 0, 0)
    lay.setSpacing(7)
    t = QLabel(title)
    t.setObjectName("secTitle")
    lay.addWidget(t)
    for key, text in lines:
        row = QHBoxLayout()
        row.setSpacing(9)
        k = QLabel(key)
        k.setObjectName("kbd")
        k.setMinimumWidth(86)
        k.setAlignment(Qt.AlignCenter)
        v = QLabel(text)
        v.setObjectName("secBody")
        v.setWordWrap(True)
        row.addWidget(k, 0, Qt.AlignTop)
        row.addWidget(v, 1)
        lay.addLayout(row)
    return box


ACTION_CN = {
    "up": "上移", "down": "下移", "left": "左移", "right": "右移",
    "confirm": "确定", "back": "返回", "menu": "菜单", "home": "主页",
    "fullscreen": "全屏", "scroll_up": "向上滚动", "scroll_down": "向下滚动",
    "cursor_left": "光标左移", "cursor_right": "光标右移", "send": "发送",
    "newline": "换行", "chat_top": "回到顶部", "prev_editor": "上一个编辑器",
    "next_editor": "下一个编辑器", "prev_tab": "上一个标签", "next_tab": "下一个标签",
    "palette": "命令面板", "hist_prev": "上一条命令", "hist_next": "下一条命令",
    "word_left": "按词左移", "word_right": "按词右移", "clear": "清屏",
    "enter": "回车", "esc": "退出", "nav_top": "上翻一页",
    "nav_bottom": "下翻一页", "nav_prev": "上一个控件", "nav_next": "下一个控件",
    "nav_exit": "退出导航",
}

MODE_BADGE = {"导航模式": ("导航模式", [37, 38, 39, 40]),
              "音量/亮度模式": ("音量 · 亮度", [37, 38, 39, 40]),
              "鼠标模式": ("鼠标模式", [37, 38, 39, 40, 13])}


def short_desc(act: dict, key_vk: int | None = None) -> str:
    """映射页右侧说明：去掉实现注释和与键帽重复的内容。"""
    if not act:
        return ""
    kind = act.get("type")
    if kind == "none":
        note = str(act.get("note", "")).strip()
        note = note.split("（")[0].split("：")[0].strip()
        return note.replace("原生 ", "").replace("移动光标", "光标") or "原生直通"
    text = core.describe_action(act, hold_info=False)
    text = text.split("（")[0].split("：")[0].strip()
    text = text.replace("原生 ", "").replace("移动光标", "光标")
    if (kind == "key" and not (act.get("mods") or [])
            and int(act.get("repeat", 1) or 1) == 1
            and key_vk is not None and int(act.get("vk", -1)) == int(key_vk)):
        return "原生按键"
    return text


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.ctl = Controller()
        self.toast = ModeToast()
        self._seq = 0
        self._drag = None
        self._rows: list = []
        self._last_profile = ""
        self._bt_tick = 0
        self._seen_remote = False

        self.setWindowTitle("AgentRMC")
        self.resize(1000, 640)
        self.setMinimumSize(880, 560)
        self.setWindowFlags(Qt.Window | Qt.FramelessWindowHint)
        self._settings = QSettings("AgentRMC", "AgentRMC")
        self._screen_hooked = False
        self._build()
        self._refresh_info()
        self._restore_geometry()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(70)
        QTimer.singleShot(400, self._refresh_bt_state)
        self.guide_first = str(self._settings.value("guide/seen", "") or "") != "1"
        self.tabs.currentChanged.connect(self._tab_changed)

    def _show_guide(self) -> None:
        self.showNormal()
        self.raise_()
        self.activateWindow()
        if getattr(self, "_guide_dlg", None) is None:
            self._guide_dlg = GuideDialog()
        dlg = self._guide_dlg
        scr = (QGuiApplication.screenAt(QCursor.pos())
               or QGuiApplication.primaryScreen())
        g = scr.availableGeometry()
        dlg.move(g.x() + (g.width() - dlg.width()) // 2,
                 g.y() + (g.height() - dlg.height()) // 2)
        dlg.show()
        dlg.raise_()
        self._mark_guide_seen()

    def _mark_guide_seen(self) -> None:
        if self.guide_first:
            self.guide_first = False
            self._settings.setValue("guide/seen", "1")

    def _tab_changed(self, index: int) -> None:
        if index != 2:
            self._mark_guide_seen()

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
        grid.addWidget(self._build_remote_card())
        grid.addWidget(self._build_voice_column())
        grid.addWidget(self._build_right(), 1)

        outer.addWidget(body, 1)

    def _build_right(self) -> QWidget:
        self.tabs = QTabWidget()
        self.tabs.setDocumentMode(False)
        self.tabs.addTab(self._build_log_card(), "事件流")
        self.tabs.addTab(self._build_keymap_card(), "按键映射")
        self.tabs.addTab(self._build_guide_card(), "使用引导")
        return self.tabs

    def _build_remote_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        card.setFixedWidth(286)
        lay = QVBoxLayout(card)
        lay.setContentsMargins(16, 13, 16, 13)
        lay.setSpacing(8)

        head = QHBoxLayout()
        title = QLabel("遥控器")
        title.setObjectName("cardTitle")
        self.mode_badge = QLabel("待机")
        self.mode_badge.setObjectName("badgeIdle")
        head.addWidget(title)
        head.addStretch(1)
        head.addWidget(self.mode_badge)
        lay.addLayout(head)

        self.panel = RemotePanel()
        lay.addWidget(self.panel, 1)

        foot = QLabel("双击 返回 / 菜单 / 主页 进入对应模式\n"
                      "12 秒无操作自动退出")
        foot.setObjectName("foot")
        foot.setWordWrap(True)
        lay.addWidget(foot)

        row = QHBoxLayout()
        row.setSpacing(8)
        self.bt_dot = Dot("#4a5568")
        self.bt_label = QLabel("检测中…")
        self.bt_label.setStyleSheet(f"color: {MUTED}; font-size: 11.5px;")
        btn_bt = QPushButton("蓝牙设置")
        btn_bt.setObjectName("ghostSmall")
        btn_bt.setCursor(Qt.PointingHandCursor)
        btn_bt.clicked.connect(self._open_bluetooth)
        row.addWidget(self.bt_dot)
        row.addWidget(self.bt_label, 1)
        row.addWidget(btn_bt)
        lay.addLayout(row)
        lay.addStretch(1)
        return card

    def _open_bluetooth(self) -> None:
        if not core.open_bluetooth_settings():
            self._append({"kind": "error", "text": "打不开系统蓝牙设置",
                          "ts": time.strftime("%H:%M:%S")})

    def _refresh_bt_state(self) -> None:
        """三态：在位=已连接；不在位但读到过电量=链路断开；从未见过=未配对。"""
        if core.remote_present():
            self.bt_dot.setStyleSheet(f"background: {GREEN}; border-radius: 3px;")
            self.bt_label.setText("已连接 · HID 在位")
            self._seen_remote = True
        elif getattr(self, "_seen_remote", False) or self.ctl.battery is not None:
            self.bt_dot.setStyleSheet(f"background: {ORANGE}; border-radius: 3px;")
            self.bt_label.setText("未连接 · 按一下遥控器任意键回连")
        else:
            self.bt_dot.setStyleSheet("background: #4a5568; border-radius: 3px;")
            self.bt_label.setText("未发现遥控器 · 长按 返回+主页 配对")

    def _build_voice_column(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        card.setFixedWidth(300)
        lay = QVBoxLayout(card)
        lay.setContentsMargins(16, 13, 16, 13)
        lay.setSpacing(9)

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
        self.wave.setMinimumHeight(88)
        self.wave.setMaximumHeight(88)
        lay.addWidget(self.wave)

        self.result = QLabel("识别结果会显示在这里")
        self.result.setObjectName("result")
        self.result.setProperty("idle", "true")
        self.result.setWordWrap(True)
        self.result.setMinimumHeight(56)
        self.result.setMaximumHeight(124)
        self.result.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        self.result.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        lay.addWidget(self.result)

        self.meta = QLabel("")
        self.meta.setObjectName("meta")
        lay.addWidget(self.meta)

        lay.addLayout(self._build_controls())

        self.info_line = QLabel("")
        self.info_line.setObjectName("foot")
        self.info_line.setWordWrap(True)
        lay.addWidget(self.info_line)
        lay.addStretch(1)
        return card

    def _build_header(self) -> QWidget:
        bar = QFrame()
        bar.setFixedHeight(48)
        bar.setStyleSheet("background: rgba(13,17,26,230);")
        lay = QHBoxLayout(bar)
        lay.setContentsMargins(18, 0, 8, 0)
        lay.setSpacing(8)

        logo = QLabel()
        logo.setFixedSize(18, 18)
        logo.setStyleSheet(
            f"background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 {BLUE}, stop:1 {PURPLE});"
            f"border-radius: 6px;")
        brand = QLabel("AgentRMC")
        brand.setObjectName("brand")

        self.pill_status = QLabel("● 已停止")
        self.pill_status.setObjectName("metaOff")
        self.pill_battery = QLabel("")
        self.pill_battery.setObjectName("meta")
        self.pill_battery.setVisible(False)
        self.pill_profile = QLabel("default")
        self.pill_profile.setObjectName("meta")
        self.pill_profile.setToolTip("当前前台场景 profile，决定按键映射")
        self.meta_sep1 = QLabel("·")
        self.meta_sep1.setObjectName("metaSep")
        self.meta_sep1.setVisible(False)
        self.meta_sep2 = QLabel("·")
        self.meta_sep2.setObjectName("metaSep")

        lay.addWidget(logo)
        lay.addWidget(brand)
        lay.addStretch(1)
        lay.addWidget(self.pill_status)
        lay.addWidget(self.meta_sep1)
        lay.addWidget(self.pill_battery)
        lay.addWidget(self.meta_sep2)
        lay.addWidget(self.pill_profile)
        lay.addSpacing(10)
        btn_guide = QPushButton("使用引导")
        btn_guide.setObjectName("ghostText")
        btn_guide.setCursor(Qt.PointingHandCursor)
        btn_guide.clicked.connect(self._show_guide)
        lay.addWidget(btn_guide)
        lay.addSpacing(6)

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

    def _build_log_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(14, 11, 10, 12)
        lay.setSpacing(8)

        head = QHBoxLayout()
        self.log_count = QLabel("0 条")
        self.log_count.setStyleSheet(f"color: {MUTED}; font-size: 11px;")
        btn_clear = QPushButton("清空")
        btn_clear.setObjectName("ghostText")
        btn_clear.setCursor(Qt.PointingHandCursor)
        btn_clear.clicked.connect(self._clear_log)
        head.addWidget(self.log_count)
        head.addStretch(1)
        head.addWidget(btn_clear)
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
        hint = QLabel("按当前前台场景显示")
        hint.setStyleSheet(f"color: {MUTED}; font-size: 11.5px;")
        self.map_combo = QComboBox()
        self.map_combo.setObjectName("combo")
        self.map_combo.setCursor(Qt.PointingHandCursor)
        self.map_combo.addItem("跟随前台", None)
        for p in self.ctl.profiles.profiles:
            self.map_combo.addItem(p.get("name", "?"), p.get("name"))
        self.map_combo.currentIndexChanged.connect(self._refresh_keymap)
        head.addWidget(hint)
        head.addStretch(1)
        head.addWidget(self.map_combo)
        lay.addLayout(head)

        self.map_match = QLabel("")
        self.map_match.setWordWrap(True)
        self.map_match.setStyleSheet(f"color: {MUTED}; font-size: 11.5px;")
        lay.addWidget(self.map_match)

        self.map_scroll = QScrollArea()
        self.map_scroll.setWidgetResizable(True)
        self.map_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
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

        # 语音键先单独一行：它不走 keys 表，接法由 profile.voice 决定
        if self.ctl.voice_mode == "hotkey":
            mods, hk_vk, trigger = core.voice_hotkey(prof)
            voice_desc = describe_voice(mods, hk_vk, trigger, short=True)
        else:
            voice_desc = "本地 whisper · 按住录音"
        self.map_grid.addWidget(self._map_row(116, "语音输入", {}, voice_desc))

        nav = prof.get("nav") or {}
        if nav.get("keys"):
            for vk_s, sem in nav["keys"].items():
                try:
                    vk = int(vk_s)
                except (TypeError, ValueError):
                    continue
                self.map_grid.addWidget(self._map_row(
                    vk, "导航中", actions.get(sem, {}),
                    desc_override=short_desc(actions.get(sem, {}))))

        if not keys:
            hint = QLabel("这个 profile 没有配置其他按键")
            hint.setStyleSheet(f"color: {MUTED}; font-size: 12px; padding: 16px;")
            self.map_grid.addWidget(hint)
            self.map_grid.addStretch(1)
            return
        for vk_s, sem in keys.items():
            try:
                vk = int(vk_s)
            except (TypeError, ValueError):
                continue
            act = actions.get(sem, {})
            desc = short_desc(act, vk)
            note = {27: "双击=音量/亮度", 36: "双击=导航",
                    93: "双击=鼠标"}.get(vk)
            if note:
                desc = f"{desc} · {note}" if desc else note
            self.map_grid.addWidget(self._map_row(vk, ACTION_CN.get(sem, sem),
                                                  act, desc_override=desc))
        self.map_grid.addStretch(1)

    def _map_row(self, vk: int | str, sem: str, act: dict,
                 desc_override: str | None = None,
                 chip_text: str | None = None) -> QWidget:
        row = QFrame()
        row.setObjectName("mapRow")
        row.setStyleSheet("QFrame#mapRow { background: rgba(255,255,255,7);"
                          " border-radius: 9px; }")
        lay = QHBoxLayout(row)
        lay.setContentsMargins(9, 5, 10, 5)
        lay.setSpacing(10)

        chip = QLabel(chip_text or (KEY_LABEL.get(vk, f"0x{vk:02X}")
                                    if isinstance(vk, int) else "组合"))
        chip.setFixedWidth(46)
        chip.setAlignment(Qt.AlignCenter)
        chip.setStyleSheet("color: #e6edf3; font-size: 11.5px; font-weight: 600;"
                           "background: rgba(255,255,255,18); border-radius: 6px;"
                           "padding: 3px 0;")

        name = QLabel(str(sem))
        name.setStyleSheet("color: #c9d1d9; font-size: 12.5px;")
        name.setMinimumWidth(84)

        desc = QLabel(desc_override if desc_override is not None
                      else short_desc(act))
        desc.setStyleSheet(f"color: {MUTED}; font-size: 11.5px;")
        desc.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        desc.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)

        lay.addWidget(chip)
        lay.addWidget(name)
        lay.addWidget(desc, 1)
        return row

    def _build_guide_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        lay = QVBoxLayout(card)
        lay.setContentsMargins(14, 12, 10, 12)
        lay.setSpacing(0)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        holder = QWidget()
        holder.setStyleSheet("background: transparent;")
        box = QVBoxLayout(holder)
        box.setContentsMargins(0, 0, 6, 0)
        box.setSpacing(16)

        box.addWidget(guide_section("第一次使用：配对遥控器", [
            ("长按 3 秒", "同时按住「返回」+「主页」，直到遥控器指示灯快速闪烁"),
            ("蓝牙设置", "点遥控器卡片右侧的「蓝牙设置」，在列表里连接遥控器"),
            ("看状态", "卡片底部状态变绿「已连接 · HID 在位」即可使用"),
            ("以后", "配对过一次后无需再配对：按一下任意键就会自动回连"),
        ]))
        box.addWidget(guide_section("手势速查", [
            ("语音键", "按住说话、松手结束（Qoder / WorkBuddy 自动切换接法）"),
            ("方向键", "浏览内容；导航模式内改为翻页"),
            ("OK", "发送消息；鼠标模式内 = 鼠标左键"),
            ("返回", "停止 / 返回；双击进入音量·亮度模式"),
            ("菜单", "换行；双击进入鼠标模式"),
            ("主页", "回到顶部；双击进入导航模式"),
            ("对焦", "全屏（走系统原生 F11）"),
        ]))

        box.addWidget(guide_section("模式怎么用", [
            ("进入", "双击对应键，屏幕底部会提示「xx模式 开」，并有一声提示音"),
            ("操作", "音量/亮度：↑↓ 音量、←→ 屏幕亮度；鼠标：方向键移动、OK 点击"),
            ("退出", "再单击「返回」或「菜单」；12 秒不操作自动退出"),
            ("注意", "鼠标模式期间方向键与 Enter 被接管，物理键盘也会一起受影响"),
        ]))
        box.addWidget(guide_section("已知限制", [
            ("电源 / 音量", "这两个键走红外，电脑收不到，无法映射"),
            ("原生按键", "遥控器的按键会先原生送达前台，用户模式无法拦截"),
            ("任务栏闪", "开始/结束录音时副屏任务栏闪一下，是 Windows 麦克风指示器"),
        ]))
        box.addStretch(1)
        scroll.setWidget(holder)
        lay.addWidget(scroll)
        return card

    def _clear_log(self) -> None:
        for row in self._rows:
            self.log_lay.removeWidget(row)
            row.deleteLater()
        self._rows.clear()
        self.log_count.setText("0 条")
        self.empty_hint.show()

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
            self.ctl.router.profiles = self.ctl.profiles
            self.ctl.router.reset()
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
        if c.voice_mode == "hotkey":
            mods, hk_vk, trigger = core.voice_hotkey(c.profiles.current())
            wire = describe_voice(mods, hk_vk, trigger, short=True)
            self.wave.hint = ("按住遥控器语音键 · 松手停录" if trigger == "toggle"
                              else "按住遥控器语音键 · 松手结束")
            model = "不加载（走目标程序识别）"
        else:
            wire = "本地 whisper"
            self.wave.hint = "按住遥控器语音键 · 松手出字"
            model = "whisper " + core.CFG["voice"]["asr_model"]
        self.info_line.setText(
            f"语音接法 {wire} · 模型 {model} · 指令 {len(c.rules.rules)} 条\n"
            f"设备 {core.CFG.get('device_mark', '-')}",
        )

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

    def _set_badge(self, text: str, keys: list[int]) -> None:
        self.mode_badge.setText(text)
        self.mode_badge.setObjectName("badge" if keys else "badgeIdle")
        self.mode_badge.style().unpolish(self.mode_badge)
        self.mode_badge.style().polish(self.mode_badge)
        self.panel.set_mode_keys(keys)

    def _set_running(self, on: bool) -> None:
        self.pill_status.setText("● 监听中" if on else "● 已停止")
        self.pill_status.setObjectName("metaOn" if on else "metaOff")
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
            if not ev.get("quiet"):
                self._append(ev)
            k = ev.get("kind")
            if k == "key":
                self._seen_remote = True
                self.panel.flash(ev.get("vk"))
            elif k == "battery":
                val = ev.get("value")
                if val is not None:
                    self.pill_battery.setText(f"{val}%")
                    self.pill_battery.setVisible(True)
                    self.meta_sep1.setVisible(True)
            elif k == "status":
                text = str(ev.get("text", ""))
                if "模式 开" in text or "模式 关" in text:
                    self.toast.show_text(text.split("（")[0])
                    name = text.split("模式")[0] + "模式"
                    if name in MODE_BADGE:
                        badge, keys = MODE_BADGE[name]
                        on = "开" in text
                        self._set_badge(badge if on else "待机",
                                        keys if on else [])
            if k == "text":
                if ev.get("text"):
                    self._set_result("done", ev["text"], f"识别耗时 {ev.get('cost')}s")
                else:
                    self._set_result("done", ev.get("note") or "没有识别到内容", "")
                self.rec_label.setText("")
            elif k == "voice_start":
                self.panel.flash(116)
                if ev.get("mode") == "hotkey":
                    meta = ("松手停录 · Qoder 开关式" if ev.get("trigger") == "toggle"
                            else "松手结束")
                    self._set_result("live", "正在聆听…", meta)
                else:
                    self._set_result("live", "正在聆听…", "松手后自动转写")
                self.rec_label.setText("● REC")
            elif k == "voice_stop":
                self.rec_label.setText("")
                if ev.get("mode") == "hotkey":
                    self._set_result("done", "已停止 · 等 Agent 出字",
                                     f"本次 {ev.get('dur')}s")
                elif ev.get("text") == "录音太短":
                    self._set_result("done", "录音太短", "")
                else:
                    self._set_result("live", "转写中…", f"录音 {ev.get('dur')}s")
            elif k == "error":
                self._set_result("live", ev.get("text", "出错了"), "")

        self.wave.push(c.level, c.state == "recording")
        self.panel.refresh()
        self._bt_tick += 1
        if self._bt_tick >= 60:        # 约 4 秒复查一次连接状态
            self._bt_tick = 0
            self._refresh_bt_state()
        if c.state == "recording" and c.voice_mode == "hotkey":
            held = max(0.0, time.time() - c.record_started)
            self.meta.setText(f"已录 {held:.1f}s · 松手结束")
        pname = c.profiles.current().get("name", "?")
        self.pill_profile.setText(pname)
        if pname != self._last_profile:
            self._last_profile = pname
            self._refresh_info()
            if self.map_combo.currentData() is None:   # 跟随前台时才自动刷
                self._refresh_keymap()
        if c.battery is not None:
            self.pill_battery.setText(f"{c.battery}%")
        if c.running != (self.btn_toggle.text() == "停止监听"):
            self._set_running(c.running)

    # ---------- 跨屏 / 缩放 ----------
    @staticmethod
    def fit_rect(frame: QRect, avail: QRect) -> QRect:
        """把 frame 收进 avail：放不下就缩到屏幕大小，再贴边。"""
        w = min(frame.width(), avail.width())
        h = min(frame.height(), avail.height())
        if w >= avail.width():
            x = avail.left()
        else:
            x = min(max(frame.left(), avail.left()), avail.right() - w + 1)
        if h >= avail.height():
            y = avail.top()
        else:
            y = min(max(frame.top(), avail.top()), avail.bottom() - h + 1)
        return QRect(x, y, w, h)

    def _restore_geometry(self) -> None:
        raw = self._settings.value("window/geometry")
        if raw:
            self.restoreGeometry(raw)
        else:
            scr = QGuiApplication.primaryScreen()
            if scr is not None:
                avail = scr.availableGeometry()
                self.resize(min(1020, avail.width()), min(700, avail.height()))
                self.move(avail.center().x() - self.width() // 2,
                          avail.center().y() - self.height() // 2)
        self._fit_to_screen()

    def _fit_to_screen(self) -> None:
        scr = QGuiApplication.screenAt(self.frameGeometry().center())
        if scr is None:
            scr = QGuiApplication.primaryScreen()
        if scr is None:
            return
        avail = scr.availableGeometry()
        target = self.fit_rect(self.frameGeometry(), avail)
        if target.size() != self.size():
            self.resize(target.size())
            target = self.fit_rect(self.frameGeometry(), avail)
        if target.topLeft() != self.pos():
            self.move(target.topLeft())

    def _on_screen_changed(self, _screen=None) -> None:
        QTimer.singleShot(250, self._fit_to_screen)

    def showEvent(self, e) -> None:  # noqa: N802
        super().showEvent(e)
        if not self._screen_hooked:
            wh = self.windowHandle()
            if wh is not None:
                wh.screenChanged.connect(self._on_screen_changed)
                self._screen_hooked = True

    # ---------- 无边框拖动 ----------
    def mousePressEvent(self, e) -> None:  # noqa: N802
        if e.button() == Qt.LeftButton and e.position().y() < 52:
            wh = self.windowHandle()
            if wh is not None and wh.startSystemMove():
                QTimer.singleShot(250, self._fit_to_screen)
                return
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
        try:
            self._settings.setValue("window/geometry", self.saveGeometry())
            self._settings.setValue("guide/seen", "1")
        except Exception:  # noqa: BLE001
            pass
        self.ctl.stop()
        super().closeEvent(e)


def main() -> None:
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
    app = QApplication(sys.argv)
    app.setStyleSheet(QSS)

    def _arg(name: str) -> str | None:
        for i, a in enumerate(sys.argv):
            if a == name and i + 1 < len(sys.argv):
                return sys.argv[i + 1]
        return None

    shot = _arg("--shot")
    # 截图 / 诊断可以绕过单实例；正常启动必须唯一，否则每个键会注入两次
    if not shot and "--allow-multi" not in sys.argv:
        if not core.acquire_single_instance():
            QMessageBox.information(
                None, "AgentRMC",
                "AgentRMC 已经在运行。\n\n同一时间只允许一个实例，避免按键被注入两次。")
            return

    win = MainWindow()
    start_minimized = bool((core.CFG.get("ui") or {}).get("start_minimized"))
    first_run = win.guide_first
    if start_minimized and not first_run and not shot and "--selftest" not in sys.argv:
        win.showMinimized()
    else:
        win.show()
        if first_run:
            QTimer.singleShot(350, win._show_guide)

    # --pin qoder：固定看某个 profile 的按键映射（配合 --shot 做验收）
    pin = _arg("--pin")
    if pin:
        idx = win.map_combo.findData(pin)
        if idx >= 0:
            win.map_combo.setCurrentIndex(idx)
            win.tabs.setCurrentIndex(1)

    # --shot 路径：渲染一帧存 PNG 后退出，用于无人值守验收界面
    tab = _arg("--tab")
    if tab is not None:
        try:
            win.tabs.setCurrentIndex(1 if tab == "keymap" else int(tab))
        except (ValueError, IndexError):
            pass
    if shot and "--selftest" not in sys.argv:
        def _grab() -> None:
            win.grab().save(shot)
            print(f"[shot] saved {shot}", flush=True)
            app.quit()

        QTimer.singleShot(1500, _grab)

    # 默认启动即监听（config.json 的 ui.auto_listen）；截图/自测默认不自动开，
    # 但显式 --autostart 仍然生效（用于验收"启动即监听"这条路径）
    auto_listen = bool((core.CFG.get("ui") or {}).get("auto_listen"))
    if ("--autostart" in sys.argv
            or (not shot and "--selftest" not in sys.argv and auto_listen)):
        def _auto_start() -> None:
            if not win.ctl.running:
                win._toggle()

        QTimer.singleShot(450, _auto_start)

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
