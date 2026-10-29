"""AgentRMC 图形界面（Qt / PySide6 版）。

不依赖 WebView2，打包成 exe 更稳。

用法:
    python agentrc_gui.py
"""
from __future__ import annotations

import html
import json
import os
import sys
import threading
import time
import traceback
from collections import deque
from pathlib import Path

from PySide6.QtCore import (Property, QEasingCurve, QPoint, QPropertyAnimation,
                            QRect, QRectF, QSettings, QSize, Qt, QTimer, Signal)
from PySide6.QtGui import (QColor, QCursor, QFont, QGuiApplication, QIcon,
                           QImage, QLinearGradient, QPainter, QPen, QPixmap)
from PySide6.QtWidgets import (
    QApplication, QComboBox, QFrame, QHBoxLayout, QLabel, QMainWindow, QMenu,
    QMessageBox, QPushButton, QScrollArea, QSizePolicy, QSystemTrayIcon,
    QTabWidget, QVBoxLayout, QWidget,
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

# (显示名, faster-whisper 模型名)。large 在 faster-whisper 里就是 large-v3，
# 这里统一存 large-v3，跟 models/faster-whisper-large-v3 目录对得上。
MODEL_CHOICES = [("small", "small"), ("medium", "medium"), ("large", "large-v3")]


def normalize_model_size(size) -> str:
    size = str(size or "small")
    return "large-v3" if size == "large" else size

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
    color: {TEXT}; font-size: 12px; background: rgba(255,255,255,9);
    border: 1px solid rgba(255,255,255,18); border-radius: 10px; padding: 6px 9px;
}}
QLabel#result[idle="true"] {{ color: #5b6472; font-size: 11px; }}
QLabel#result[live="true"] {{
    color: #ffb4b0; background: rgba(248,81,73,18);
    border: 1px solid rgba(248,81,73,115);
}}
QLabel#result[done="true"] {{
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
QPushButton#voicePrimary {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {BLUE}, stop:1 {PURPLE});
    color: white; border: none; border-radius: 8px;
    font-size: 11.5px; font-weight: 600; padding: 0 10px;
}}
QPushButton#voicePrimary:hover {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #6299ff, stop:1 #8d70ff);
}}
QPushButton#voicePrimary[stopping="true"] {{
    background: rgba(248,81,73,34); color: #ffb4b0;
    border: 1px solid rgba(248,81,73,110);
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
QComboBox#combo[compact="true"] {{ min-width: 64px; padding: 4px 8px; }}
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
QPushButton#guideLink {{
    background: rgba(79,140,255,14); color: #9cc0ff;
    border: 1px solid rgba(79,140,255,72); border-radius: 8px;
    font-size: 11.5px; font-weight: 600; padding: 6px 12px;
}}
QPushButton#guideLink:hover {{
    background: rgba(79,140,255,32); color: #cfe0ff;
    border-color: rgba(79,140,255,120);
}}
QScrollArea {{ background: transparent; border: none; }}
QScrollBar:vertical {{ background: transparent; width: 8px; margin: 4px 2px; }}
QScrollBar::handle:vertical {{ background: rgba(255,255,255,26); border-radius: 4px; min-height: 24px; }}
QScrollBar::handle:vertical:hover {{ background: rgba(255,255,255,46); }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
QScrollBar:horizontal {{ background: transparent; height: 8px; margin: 2px 4px; }}
QScrollBar::handle:horizontal {{ background: rgba(255,255,255,26); border-radius: 4px; min-width: 24px; }}
QScrollBar::handle:horizontal:hover {{ background: rgba(255,255,255,46); }}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {{ width: 0; }}
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
QLabel#sceneKicker {{
    color: {MUTED}; font-size: 10px; font-weight: 600; letter-spacing: 1px;
}}
QLabel#sceneName {{ color: {TEXT}; font-size: 12.5px; font-weight: 600; }}
QLabel#sceneRule {{ color: #5b6472; font-size: 10.5px; }}
QLabel#mapHead {{
    color: #5b6472; font-size: 10px; font-weight: 600; letter-spacing: 1px;
}}
QLabel#mapGroup {{
    color: {MUTED}; font-size: 10.5px; font-weight: 600; letter-spacing: 1px;
}}
QFrame#mapHair {{ background: rgba(255,255,255,10); border: none; }}
QFrame#mapKeyRow {{
    background: transparent; border: 1px solid transparent; border-radius: 9px;
}}
QFrame#mapKeyRow:hover {{ background: rgba(255,255,255,10); }}
QFrame#mapKeyRow[sel="true"] {{
    background: rgba(79,140,255,24);
    border: 1px solid rgba(79,140,255,72);
}}
QLabel#mapChip {{
    color: {TEXT}; font-size: 9.5px; font-weight: 700;
    background: rgba(255,255,255,14); border: 1px solid rgba(255,255,255,20);
    border-radius: 6px;
}}
QLabel#mapKeyName {{ color: #dbe4ee; font-size: 12px; font-weight: 600; }}
QFrame#mapCell {{
    background: transparent; border: 1px solid transparent; border-radius: 7px;
}}
QFrame#mapCell:hover {{
    background: rgba(255,255,255,9);
    border: 1px solid rgba(255,255,255,16);
}}
QLabel#mapCellTitle {{ color: #dbe4ee; font-size: 11px; font-weight: 600; }}
QLabel#mapCellEffect {{ color: #5b6472; font-size: 9.5px; }}
QLabel#mapCellDash {{ color: #3d4653; font-size: 11px; }}
QLabel#mapCellChev {{ color: #3d4653; font-size: 10px; }}
QFrame#mapCell:hover QLabel#mapCellChev {{ color: #8b949e; }}
QFrame#mapKeyRow[sel="true"] QLabel#mapCellEffect {{ color: #8b949e; }}
QFrame#mapKeyRow[sel="true"] QLabel#mapCellChev {{ color: #8b949e; }}
QLabel#mapNavNote {{ font-size: 9.5px; }}
QLabel#mapEmpty {{ color: {MUTED}; font-size: 11px; padding: 14px 6px; }}
QLabel#mapFoot {{ color: #5b6472; font-size: 10px; }}
QMenu {{
    background: #161d2a; color: {TEXT}; border: 1px solid rgba(255,255,255,26);
    border-radius: 8px; padding: 5px;
}}
QMenu::item {{
    padding: 6px 24px 6px 12px; border-radius: 6px; font-size: 11.5px;
}}
QMenu::item:selected {{ background: rgba(79,140,255,70); }}
QMenu::item:disabled {{ color: #5b6472; }}
QMenu::separator {{
    height: 1px; background: rgba(255,255,255,14); margin: 4px 8px;
}}
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
        self.asr = core.Transcriber(normalize_model_size(core.CFG["voice"]["asr_model"]),
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
        self.media_voice = False     # 当前语音会话由耳机播放键发起（无 keyup，只能 toggle）
        self.last_media_voice = 0.0
        self.level = 0.0
        self.battery = None
        self.last_text = ""
        self.model_ready = False
        self.model_size = normalize_model_size(core.CFG["voice"]["asr_model"])
        self.model_state = "idle"      # idle | loading | ready | error | skip
        self.model_error = ""
        self._model_gen = 0

        # 语音键两种用法：local=自己录音转写；hotkey=转给目标程序（Qoder 的 Ctrl+Shift+V）
        self.voice_mode = str(core.CFG["voice"].get("mode", "local")).lower()
        if self.voice_mode != "local":
            self.model_state = "skip"
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
        core.media_events.clear()
        self.media_voice = False
        self.router.reset()
        self.emit("status", text="监听已启动")
        self._sync_media_hook()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        self._start_model_load()
        threading.Thread(target=self._read_battery, daemon=True).start()
        return True, ""

    def _sync_media_hook(self) -> None:
        """耳机播放键只在 hotkey + 监听中接管，切模式时跟着装/卸。"""
        want = (self.running
                and (core.CFG.get("media_voice") or {}).get("enabled", True)
                and self.voice_mode == "hotkey")
        if not want:
            core.media_hook_stop()
            return
        if core.media_hook_start(lambda: core.media_voice_gate(self.profiles,
                                                              self.voice_mode)):
            self.emit("log", text="耳机播放键：Qoder/WorkBuddy 前台时开关语音")
        else:
            self.emit("log", text="耳机播放键接管失败（低层钩子没装上）")

    def stop(self) -> None:
        self.running = False
        self._release_holding(0)
        core.media_hook_stop()
        self.media_voice = False
        t = self._thread
        if t is not None and t is not threading.current_thread():
            t.join(timeout=0.6)
        self._thread = None
        self.router.reset()
        self.emit("status", text="监听已停止")

    def _start_model_load(self) -> None:
        """按当前开关决定是否加载；加载放后台，界面靠 model_state 轮询。"""
        if self.voice_mode != "local":
            # 语音键交给 Agent 自己识别，本地模型就不用加载了（省 462MB 内存 + 1s 启动）
            self.model_state = "skip"
            self.model_ready = False
            self.emit("status", text="hotkey 模式，跳过本地模型")
            return
        if self.asr._model is not None:
            self.model_ready = True
            self.model_state = "ready"
            return
        if self.asr._failed:              # 上次失败过，手动再开一次就重试
            self.asr = core.Transcriber(self.model_size, core.CFG["voice"]["asr_language"])
        self._model_gen += 1
        gen = self._model_gen
        self.model_error = ""
        self.model_state = "loading"
        local_dir = HERE / "models" / f"faster-whisper-{self.model_size}"
        tail = "" if (local_dir / "model.bin").exists() else "（models/ 里没有，走缓存或联网）"
        self.emit("status", text=f"加载语音模型 {self.model_size}…{tail}")
        threading.Thread(target=self._load_model_worker, args=(gen,), daemon=True).start()

    def _load_model_worker(self, gen: int) -> None:
        asr = self.asr
        try:
            model = asr.load()
        except Exception:  # noqa: BLE001
            model = None
        if gen != self._model_gen:        # 加载期间又切换过模型/开关，这结果作废
            return
        self.model_ready = model is not None
        self.model_state = "ready" if model else "error"
        self.model_error = "" if model else (asr.error or "加载失败")
        self.emit("status", text="模型就绪" if model else "模型加载失败")

    def _unload_model(self) -> None:
        """释放模型内存：换一个干净的 Transcriber，旧对象由 GC 回收。"""
        self._model_gen += 1
        self.asr = core.Transcriber(self.model_size, core.CFG["voice"]["asr_language"])
        self.model_ready = False
        self.model_error = ""
        self.model_state = "skip"

    def _cancel_voice_session(self) -> None:
        """切模式前把手上的录音/已按下的热键收尾，避免状态机卡在 recording。"""
        if self.state == "recording":
            if self.voice_mode == "hotkey" and self.holding:
                self._release_holding((time.time() - self.record_started) * 1000)
            else:
                self.recorder.stop()
        self.state = "idle"
        self.holding = False
        self.media_voice = False
        self.level = 0.0

    def set_local_model(self, on: bool) -> None:
        want = "local" if on else "hotkey"
        if self.voice_mode == want:
            if on:
                self._start_model_load()
            return
        self._cancel_voice_session()
        self.voice_mode = want
        self._sync_media_hook()
        if not core.save_voice_config(mode=want):
            self.emit("error", text="写 config.json 失败，本次设置只在当前运行有效")
        if on:
            self.emit("status", text="本地模型已开启")
            self._start_model_load()
        else:
            self._unload_model()
            self.emit("status", text="本地模型已关闭 · 语音交给目标程序")

    def set_model_size(self, size: str) -> None:
        if not size or size == self.model_size:
            return
        self.model_size = size
        if not core.save_voice_config(asr_model=size):
            self.emit("error", text="写 config.json 失败，本次设置只在当前运行有效")
        self._cancel_voice_session()
        self._model_gen += 1
        self.asr = core.Transcriber(size, core.CFG["voice"]["asr_language"])
        self.model_ready = False
        self.model_error = ""
        if self.voice_mode == "local":
            self.emit("status", text=f"本地模型切换为 {size}")
            self._start_model_load()
        else:
            self.model_state = "skip"
            self.emit("status", text=f"本地模型已选 {size}（开关关闭，暂不加载）")

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
                while core.media_events:
                    core.media_events.popleft()
                    self._media_voice_press(now)
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

    def _media_voice_press(self, now: float) -> None:
        """耳机播放键：按一下开、再按一下关；没有 keyup，不能走按住逻辑。"""
        if not core.media_voice_gate(self.profiles, self.voice_mode):
            return
        if now - self.last_media_voice < 0.35:      # 双击手势别开又关
            return
        self.last_media_voice = now
        if self.state == "recording":
            if self.media_voice:
                self._finish((now - self.record_started) * 1000)
            return
        if self.state == "pending":
            self.state = "idle"
        prof = self.profiles.current(force=True)
        mods, vk, trigger = core.voice_hotkey(prof)
        if trigger != "toggle":
            return
        exe, title = core.foreground_info()
        fc = core.voice_focus_click(prof)
        focused = None
        if fc:
            focused = core.click_foreground(*fc)
            if focused:
                time.sleep(0.02)
        core.tap_combo(mods, vk)
        self.active_voice = (mods, vk, trigger)
        self.holding = True
        self.media_voice = True
        self.state = "recording"
        self.record_started = now
        self.last_voice_event = now
        core.beep(1180, 60)
        self.emit("voice_start", mode="hotkey", trigger=trigger, source="headset",
                  focus_click=focused, fg=f"{exe}|{title[:24]}",
                  tray=core.taskbar_state())

    def _tick(self, now: float) -> None:
        if self.state == "pending" and (now - self.pending_since) * 1000 > self.confirm_ms:
            self.state = "idle"
        if self.state == "recording":
            if self.voice_mode == "hotkey":
                self.level = 0.0
            else:
                self.level = self.recorder.level
            if not self.media_voice and (now - self.last_voice_event) * 1000 > self.silence_ms:
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
        was_media = self.media_voice
        self.state = "idle"
        self.media_voice = False
        if self.voice_mode == "hotkey":
            trigger = self.active_voice[2]
            fg = core.foreground_info()[0]
            self._release_holding(dur_ms)
            self.level = 0.0
            core.beep(760, 60)
            self.emit("voice_stop", dur=round(dur_ms / 1000, 1), mode="hotkey",
                      trigger=trigger, source="headset" if was_media else "",
                      fg=fg, tray=core.taskbar_state())
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
        # 开关关着时麦克风测试也要转写一次，用完把模型释放掉，别破坏「关=不占内存」
        temp_load = self.voice_mode != "local"
        try:
            import sounddevice as sd
            if temp_load:
                self.emit("status", text=f"测试麦克风：加载本地模型 {self.model_size}…")
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
        finally:
            if temp_load and self.voice_mode != "local":
                self._unload_model()


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

        # 左栏紧凑模式：一条电平/录音脉冲，不画大波形也不放提示字
        if h <= 20:
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(255, 255, 255, 14))
            p.drawRoundedRect(QRectF(0, mid - 2, w, 4), 2, 2)
            level = self.data[-1] if self.data else 0.0
            if self.active:
                if max(self.data, default=0.0) < 0.004:
                    phase = (time.monotonic() * 0.9) % 1.0
                    seg = max(24.0, w * 0.26)
                    x = phase * max(0.0, w - seg)
                    g = QLinearGradient(x, 0, x + seg, 0)
                    g.setColorAt(0.0, QColor(248, 81, 73, 40))
                    g.setColorAt(0.5, QColor(248, 81, 73, 235))
                    g.setColorAt(1.0, QColor(248, 81, 73, 40))
                    p.setBrush(g)
                    p.drawRoundedRect(QRectF(x, mid - 2, seg, 4), 2, 2)
                else:
                    fill = min(w, max(4.0, w * min(1.0, level * 4)))
                    p.setBrush(QColor(248, 81, 73, 220))
                    p.drawRoundedRect(QRectF(0, mid - 2, fill, 4), 2, 2)
            elif level > 0.01:
                fill = min(w, max(2.0, w * min(1.0, level * 4)))
                p.setBrush(QColor(79, 140, 255, 150))
                p.drawRoundedRect(QRectF(0, mid - 2, fill, 4), 2, 2)
            return

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


VK_NAME = {18: "ALT", 17: "CTRL", 16: "SHIFT", 91: "WIN", 32: "SPACE",
           13: "ENTER", 9: "TAB", 27: "ESC", 86: "V", 65: "A", 192: "`"}


def vk_name(vk: int) -> str:
    """单键名：修饰键用大写名，字母/数字回落到 core 的表。"""
    vk = int(vk)
    return VK_NAME.get(vk) or core.VK_LABEL.get(vk) or f"VK 0x{vk:02X}"


def describe_voice(mods: list, vk: int, trigger: str, short: bool = False) -> str:
    """把语音键接法说成人话，比如 开关 CTRL+SHIFT+V（按下开 / 松手关）。"""
    names = [str(m).upper() for m in mods or []]
    names.append(vk_name(vk))
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


class ToggleSwitch(QWidget):
    """自绘开关：跟卡片同一套配色，不跟着系统 QCheckBox 主题跑。"""

    toggled = Signal(bool)

    def __init__(self, checked: bool = False, parent=None):
        super().__init__(parent)
        self._checked = bool(checked)
        self._offset = 1.0 if checked else 0.0
        self._hover = False
        self.setFixedSize(36, 21)
        self.setCursor(Qt.PointingHandCursor)
        self.setFocusPolicy(Qt.StrongFocus)
        self._anim = QPropertyAnimation(self, b"offset", self)
        self._anim.setDuration(130)
        self._anim.setEasingCurve(QEasingCurve.OutCubic)

    def _get_offset(self) -> float:
        return self._offset

    def _set_offset(self, v: float) -> None:
        self._offset = float(v)
        self.update()

    offset = Property(float, _get_offset, _set_offset)

    def isChecked(self) -> bool:
        return self._checked

    def setChecked(self, on: bool, animate: bool = False) -> None:
        on = bool(on)
        if on == self._checked and not animate:
            return
        self._checked = on
        self._anim.stop()
        end = 1.0 if on else 0.0
        if animate:
            self._anim.setStartValue(self._offset)
            self._anim.setEndValue(end)
            self._anim.start()
        else:
            self._set_offset(end)

    def _flip(self) -> None:
        self.setChecked(not self._checked, animate=True)
        self.toggled.emit(self._checked)

    def mouseReleaseEvent(self, e) -> None:  # noqa: N802
        if e.button() == Qt.LeftButton and self.rect().contains(e.position().toPoint()):
            self._flip()
        super().mouseReleaseEvent(e)

    def keyPressEvent(self, e) -> None:  # noqa: N802
        if e.key() in (Qt.Key_Space, Qt.Key_Return, Qt.Key_Enter):
            self._flip()
            return
        super().keyPressEvent(e)

    def enterEvent(self, e) -> None:  # noqa: N802
        self._hover = True
        self.update()
        super().enterEvent(e)

    def leaveEvent(self, e) -> None:  # noqa: N802
        self._hover = False
        self.update()
        super().leaveEvent(e)

    def focusInEvent(self, e) -> None:  # noqa: N802
        self.update()
        super().focusInEvent(e)

    def focusOutEvent(self, e) -> None:  # noqa: N802
        self.update()
        super().focusOutEvent(e)

    def paintEvent(self, _e) -> None:  # noqa: N802
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = float(self.width()), float(self.height())
        r = h / 2
        track = QRectF(0.5, 0.5, w - 1, h - 1)
        if self._checked:
            g = QLinearGradient(0, 0, w, 0)
            g.setColorAt(0, QColor(BLUE))
            g.setColorAt(1, QColor(PURPLE))
            p.setBrush(g)
            p.setPen(QPen(QColor(255, 255, 255, 46), 1))
        else:
            p.setBrush(QColor(255, 255, 255, 36 if self._hover else 22))
            p.setPen(QPen(QColor(255, 255, 255, 40 if self._hover else 28), 1))
        p.drawRoundedRect(track, r, r)

        knob = h - 6
        x = 3 + (w - 6 - knob) * self._offset
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(255, 255, 255, 235 if self._checked else 200))
        p.drawEllipse(QRectF(x, 3, knob, knob))

        if self.hasFocus():
            p.setBrush(Qt.NoBrush)
            p.setPen(QPen(QColor(79, 140, 255, 150), 1))
            p.drawRoundedRect(track.adjusted(-1, -1, 1, 1), r + 1, r + 1)


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
            if ev.get("source") == "headset":
                return "耳机播放键 · 已开语音（再按一次停止）"
            if ev.get("mode") != "hotkey":
                return "开始录音"
            text = ("已发送开始录音（开关）" if ev.get("trigger") == "toggle"
                    else "已按住语音快捷键")
            if ev.get("focus_click") is not None:
                text += " · 已点聊天框" + ("✓" if ev.get("focus_click") else "✗")
            return text
        if k == "voice_stop":
            if ev.get("source") == "headset" and ev.get("dur"):
                return f"耳机语音 {ev.get('dur')}s · 已停"
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
        self.setMinimumSize(238, 190)
        self._lit: dict[int, float] = {}
        self.mode_keys: set[int] = set()
        self.link_vk: int | None = None

    def flash(self, vk: int | None) -> None:
        if vk:
            self._lit[int(vk)] = time.monotonic()
            self.update()

    def set_mode_keys(self, vks) -> None:
        self.mode_keys = set(vks)
        self.update()

    def set_link(self, vk: int | None) -> None:
        """映射页选中/悬停的键：画一圈蓝环，跟左侧键位图联动。"""
        vk = int(vk) if vk else None
        if vk == self.link_vk:
            return
        self.link_vk = vk
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
        linked = vk is not None and vk == self.link_vk
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
        if linked and enabled and not lit:
            p.setBrush(Qt.NoBrush)
            p.setPen(QPen(QColor(79, 140, 255, 210), 1.5))
            p.drawEllipse(QRectF(x - r - 3, y - r - 3, (r + 3) * 2, (r + 3) * 2))
        if ringed and not lit:
            p.setBrush(Qt.NoBrush)
            p.setPen(QPen(QColor(124, 92, 255, 200), 1.6))
            rr = r + (6 if linked else 3)
            p.drawEllipse(QRectF(x - rr, y - rr, rr * 2, rr * 2))
        glyph = self.GLYPH.get(vk or -1, "")
        if glyph:
            f = p.font()
            f.setPointSizeF(8.4 if r < 12 else 9.2)
            p.setFont(f)
            p.setPen(QPen(QColor(230, 237, 243) if lit or enabled
                          else QColor(90, 100, 114)))
            p.drawText(QRectF(x - r - 6, y - r, (r + 6) * 2, r * 2),
                       Qt.AlignCenter, glyph)


class ScaledImageLabel(QLabel):
    """按控件实际物理像素重绘原图。

    不能先缩成 px 逻辑像素再交给 QLabel：高 DPI 屏会把那张小图再放大一次，
    细线就糊了。这里保留原图，paintEvent 里按控件物理尺寸做一次平滑缩放。
    """

    def __init__(self, path: Path | None = None, parent=None):
        super().__init__(parent)
        self._pm = QPixmap(str(path)) if path is not None and path.exists() else QPixmap()
        self.setAlignment(Qt.AlignCenter)

    def paintEvent(self, e) -> None:  # noqa: N802
        super().paintEvent(e)             # QSS 背景/圆角边框
        if self._pm.isNull():
            return
        r = self.rect().adjusted(7, 7, -7, -7)
        if r.width() <= 0 or r.height() <= 0:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.SmoothPixmapTransform)
        size = self._pm.size().scaled(r.size(), Qt.KeepAspectRatio)
        p.drawPixmap(
            QRect(r.x() + (r.width() - size.width()) // 2,
                  r.y() + (r.height() - size.height()) // 2,
                  size.width(), size.height()),
            self._pm,
        )


def image_tile(name: str, px: int, caption: str, py: int | None = None) -> QWidget:
    """产品图/示意图：按图片自身明暗配深浅“纸片”，避免深底上刺眼。"""
    box = QWidget()
    lay = QVBoxLayout(box)
    lay.setContentsMargins(0, 0, 0, 0)
    lay.setSpacing(6)
    ph = py or px
    path = HERE / "assets" / "remote" / name
    paper = "#f2f4f7"
    if path.exists():
        img = QImage(str(path))
        c = img.pixelColor(3, 3)
        if c.red() * 0.3 + c.green() * 0.6 + c.blue() * 0.1 < 90:
            paper = "#0f1420"          # 深色底图配深色纸片，不刺眼
    pic = ScaledImageLabel(path)
    pic.setFixedSize(px, ph)
    pic.setStyleSheet(f"background: {paper}; border: 1px solid rgba(255,255,255,26);"
                      "border-radius: 12px;")
    if not path.exists():
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
        row.addWidget(image_tile("pairing-earbuds.png", 300,
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
        for k, v in [("耳机播放", "Qoder / WorkBuddy 前台时开 / 关语音"),
                     ("语音键", "按住说话、松手结束"),
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

        btn_bt = QPushButton("打开蓝牙设置  ↗")
        btn_bt.setObjectName("guideLink")
        btn_bt.setCursor(Qt.PointingHandCursor)
        btn_bt.clicked.connect(core.open_bluetooth_settings)
        rlay.addStretch(1)               # 按钮贴右列底部，避免下方留一大块空
        foot = QHBoxLayout()
        foot.setContentsMargins(0, 0, 0, 0)
        foot.addStretch(1)
        foot.addWidget(btn_bt)
        rlay.addLayout(foot)
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
    "nav_exit": "退出导航", "forward": "前进", "new_tab": "新标签页",
}

# 遥控器物理键：顺序就是映射页的分组顺序
MAP_GLYPH = {38: "↑", 40: "↓", 37: "←", 39: "→", 13: "OK",
             27: "↩", 93: "☰", 36: "⌂", 116: "◉", 122: "◎"}
MAP_NAME = {38: "上键", 40: "下键", 37: "左键", 39: "右键", 13: "确定键",
            27: "返回键", 93: "菜单键", 36: "主页键", 116: "语音键", 122: "对焦键"}
MAP_GROUPS = [("方向键", [38, 40, 37, 39]),
              ("功能键", [13, 27, 93, 36]),
              ("语音与直通", [116, 122])]
# 双击手势由 router 统一接管（不写在 profile 里）
DOUBLE_GESTURE = {
    27: ("音量/亮度模式", "双击进入音量 / 亮度模式：↑↓ 音量、←→ 屏幕亮度；"
                       "再双击返回或 12 秒无操作退出"),
    93: ("鼠标模式", "双击进入鼠标模式：方向键移动、OK 左键；单击菜单退出"),
    36: ("导航模式", "双击切换导航模式：↑↓ 翻页、←→ 切换控件；单击主页退出"),
}

# --------------------------------------------------------------------------
# 常用动作预设：映射页单元格点开后直接选，不再手改 JSON
# --------------------------------------------------------------------------
NATIVE_LABEL = {38: "↑", 40: "↓", 37: "←", 39: "→", 13: "Enter",
                27: "Esc", 93: "Apps", 36: "Home", 116: "F5", 122: "F11"}

PRESET_LABELS = {
    "pass": "原生直通",
    "up": "↑ 上", "down": "↓ 下", "left": "← 左", "right": "→ 右",
    "enter": "回车", "space": "空格", "tab": "Tab", "esc": "Esc",
    "backspace": "退格", "delete": "删除",
    "page_up": "PageUp", "page_down": "PageDown",
    "scroll_up": "向上滚动", "scroll_down": "向下滚动",
    "browser_back": "后退", "browser_forward": "前进",
    "new_tab": "新标签页", "close_tab": "关闭标签",
    "reload": "刷新", "address": "地址栏",
    "prev_editor": "上一个编辑器", "next_editor": "下一个编辑器",
    "prev_tab": "上一个标签", "next_tab": "下一个标签",
    "palette": "命令面板", "clear": "清屏",
    "volume_up": "音量 +", "volume_down": "音量 −",
    "mute": "静音", "play": "播放 / 暂停", "fullscreen": "全屏",
    "mouse_click": "鼠标左键",
}

PRESET_SPECS = {
    "pass": {"type": "none", "note": "原生直通"},
    "up": {"type": "key", "vk": 0x26},
    "down": {"type": "key", "vk": 0x28},
    "left": {"type": "key", "vk": 0x25},
    "right": {"type": "key", "vk": 0x27},
    "enter": {"type": "key", "vk": 0x0D},
    "space": {"type": "key", "vk": 0x20},
    "tab": {"type": "key", "vk": 0x09},
    "esc": {"type": "key", "vk": 0x1B},
    "backspace": {"type": "key", "vk": 0x08},
    "delete": {"type": "key", "vk": 0x2E},
    "page_up": {"type": "key", "vk": 0x21},
    "page_down": {"type": "key", "vk": 0x22},
    "scroll_up": {"type": "scroll", "delta": 360},
    "scroll_down": {"type": "scroll", "delta": -360},
    "browser_back": {"type": "key", "vk": 0x25, "mods": ["alt"]},
    "browser_forward": {"type": "key", "vk": 0x27, "mods": ["alt"]},
    "new_tab": {"type": "key", "vk": 0x54, "mods": ["ctrl"]},
    "close_tab": {"type": "key", "vk": 0x57, "mods": ["ctrl"]},
    "reload": {"type": "key", "vk": 0x52, "mods": ["ctrl"]},
    "address": {"type": "key", "vk": 0x4C, "mods": ["ctrl"]},
    "prev_editor": {"type": "key", "vk": 0x21, "mods": ["ctrl"]},
    "next_editor": {"type": "key", "vk": 0x22, "mods": ["ctrl"]},
    "prev_tab": {"type": "key", "vk": 0x09, "mods": ["ctrl", "shift"]},
    "next_tab": {"type": "key", "vk": 0x09, "mods": ["ctrl"]},
    "palette": {"type": "key", "vk": 0x50, "mods": ["ctrl", "shift"]},
    "clear": {"type": "key", "vk": 0x4C, "mods": ["ctrl"]},
    "volume_up": {"type": "key", "vk": 0xAF},
    "volume_down": {"type": "key", "vk": 0xAE},
    "mute": {"type": "key", "vk": 0xAD},
    "play": {"type": "key", "vk": 0xB3},
    "fullscreen": {"type": "key", "vk": 0x7A},
    "mouse_click": {"type": "click"},
}

PRESET_GROUPS = [
    ("导航 / 输入", ["up", "down", "left", "right", "enter", "space", "tab",
                    "esc", "backspace", "delete", "page_up", "page_down"]),
    ("滚动", ["scroll_up", "scroll_down"]),
    ("浏览器", ["browser_back", "browser_forward", "new_tab", "close_tab",
              "reload", "address"]),
    ("编辑器", ["prev_editor", "next_editor", "prev_tab", "next_tab",
              "palette", "clear"]),
    ("系统", ["volume_up", "volume_down", "mute", "play", "fullscreen"]),
    ("鼠标", ["mouse_click"]),
]

HOLD_PRESET_IDS = ["enter", "esc", "space", "tab", "backspace", "delete",
                   "page_up", "page_down", "volume_up", "volume_down",
                   "mute", "play", "fullscreen", "mouse_click"]

DOUBLE_MODE_PRESETS = {
    "mode_media": {"type": "mode", "mode": "media", "label": "音量/亮度模式"},
    "mode_cursor": {"type": "mode", "mode": "cursor", "label": "鼠标模式"},
    "nav_toggle": {"type": "nav_toggle", "label": "导航模式"},
}

GESTURE_CN = {"single": "单击", "double": "双击", "hold": "长按"}


def preset_action(pid: str, vk: int, gesture: str = "single") -> dict | None:
    """生成一个预设动作；单击时若目标键就是物理键，自动改成原生直通。"""
    if pid in DOUBLE_MODE_PRESETS:
        return dict(DOUBLE_MODE_PRESETS[pid])
    spec = PRESET_SPECS.get(pid)
    if spec is None:
        return None
    action = dict(spec)
    if isinstance(action.get("mods"), list):
        action["mods"] = list(action["mods"])
    action["label"] = PRESET_LABELS.get(pid, pid)
    if (gesture == "single" and action.get("type") == "key"
            and not (action.get("mods") or [])
            and int(action.get("vk", -1)) == int(vk)):
        return {"type": "none", "label": action["label"],
                "note": "原生 " + NATIVE_LABEL.get(int(vk), vk_name(int(vk)))}
    return action


def apply_mapping_preset(raw: dict, profile_name: str, vk: int,
                         gesture: str, kind: str) -> bool:
    """把单元格预设写进原始 profiles 数据；不改磁盘。返回是否改动。"""
    profile = next((p for p in (raw.get("profiles") or [])
                    if p.get("name") == profile_name), None)
    if profile is None:
        return False
    actions = profile.get("actions")
    if not isinstance(actions, dict):
        actions = profile["actions"] = {}
    keys = profile.get("keys")
    if not isinstance(keys, dict):
        keys = profile["keys"] = {}

    if gesture == "single":
        action = preset_action(kind, vk, "single")
        if action is None:
            return False
        sem = f"k{vk}_{kind}"
        keys[str(vk)] = sem
        actions[sem] = action
        return True

    double = profile.get("double")
    if not isinstance(double, dict):
        double = profile["double"] = {}
    hold = profile.get("hold")
    if not isinstance(hold, dict):
        hold = profile["hold"] = {}

    if gesture == "double":
        if kind == "none":
            double[str(vk)] = "none"
            return True
        action = preset_action(kind, vk, "double")
        if action is None:
            return False
        sem = f"k{vk}_d_{kind}"
        double[str(vk)] = sem
        actions[sem] = action
        return True

    if gesture == "hold":
        if kind in ("none", "repeat", "repeat_accel"):
            hold[str(vk)] = kind
            return True
        action = preset_action(kind, vk, "hold")
        if action is None:
            return False
        sem = f"k{vk}_h_{kind}"
        hold[str(vk)] = sem
        actions[sem] = action
        return True
    return False

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


def native_effect(act: dict) -> str:
    """type=none 的动作说清楚是哪颗键在直通。"""
    note = str(act.get("note", "")).lower()
    for token, label in (("f11", "F11"), ("enter", "Enter"), ("esc", "Esc"),
                         ("home", "Home"), ("tab", "Tab"), ("apps", "Apps")):
        if token in note:
            return label + " 直通"
    for arrow in ("←", "→", "↑", "↓"):
        if arrow in note:
            return arrow + " 直通"
    return "原生直通"


def action_effect_text(act: dict) -> str:
    """动作的实际效果；忽略 label，避免预设标题把真实按键吞掉。"""
    kind = act.get("type")
    if kind == "click":
        return "鼠标左键"
    if kind == "mode":
        return "双击进入"
    if kind == "nav_toggle":
        return "双击切换"
    if kind == "none":
        return "无动作"
    a = dict(act)
    a.pop("label", None)
    text = core.describe_action(a, hold_info=False)
    return text.split("（")[0].split("：")[0].strip()


def action_cells(act: dict, vk: int, sem: str) -> tuple[str, str]:
    """映射表“单击”格：人话动作名 + 实际按键/效果。"""
    if not act:
        return "未映射", ""
    kind = act.get("type")
    name = ACTION_CN.get(sem) or str(act.get("label") or sem)
    if kind == "none":
        effect = native_effect(act)
        return name, ("" if effect == name else effect)
    if sem.startswith(f"k{vk}_"):
        effect = action_effect_text(act)
    else:
        effect = short_desc(act, vk)
    if effect == "原生按键":
        native = {13: "Enter", 27: "Esc", 93: "Apps", 36: "Home", 9: "Tab",
                  38: "↑", 40: "↓", 37: "←", 39: "→"}.get(
            int(act.get("vk", -1)), core.VK_LABEL.get(int(act.get("vk", -1)), ""))
        effect = (native + " 直通") if native else "原生直通"
    if effect == name:
        effect = ""
    return name, effect


class ElidedLabel(QLabel):
    """单行、超宽省略号 + 完整 tooltip；窗口拉窄时不把布局撑爆。"""

    def __init__(self, text: str = "", parent=None):
        super().__init__(parent)
        self._full = ""
        self.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        self.setText(text)

    def setText(self, text: str) -> None:  # noqa: N802
        self._full = str(text or "")
        self.setToolTip(self._full)
        self._apply_elide()

    def resizeEvent(self, e) -> None:  # noqa: N802
        self._apply_elide()
        super().resizeEvent(e)

    def minimumSizeHint(self) -> QSize:  # noqa: N802
        # 允许被布局压窄：QLabel 默认按全文宽度顶住布局
        return QSize(0, super().minimumSizeHint().height())

    def _apply_elide(self) -> None:
        w = self.width()
        if w > 8 and self._full:
            super().setText(self.fontMetrics().elidedText(
                self._full, Qt.ElideRight, max(0, w - 1)))
        else:
            super().setText(self._full)


def map_section(title: str) -> QWidget:
    """映射表分组标题：小字 + 细横线。"""
    box = QWidget()
    box.setFixedHeight(22)
    lay = QHBoxLayout(box)
    lay.setContentsMargins(2, 5, 0, 0)
    lay.setSpacing(8)
    lab = QLabel(title)
    lab.setObjectName("mapGroup")
    hair = QFrame()
    hair.setObjectName("mapHair")
    hair.setFixedHeight(1)
    hair.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
    lay.addWidget(lab)
    lay.addWidget(hair, 1, Qt.AlignVCenter)
    return box


class MapCell(QFrame):
    """映射表里的一个手势格：显示当前动作，点开是常用预设菜单。"""

    clicked = Signal(int, str)
    hovered = Signal(int, bool)

    def __init__(self, vk: int, gesture: str, title: str, effect: str,
                 editable: bool = True, tip: str = "", parent=None):
        super().__init__(parent)
        self.vk = int(vk)
        self.gesture = gesture
        self.title_text = title
        self.effect_text = effect
        self.editable = bool(editable)
        self.setObjectName("mapCell")
        self.setFixedHeight(33)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.setCursor(Qt.PointingHandCursor if editable else Qt.ArrowCursor)

        lay = QHBoxLayout(self)
        lay.setContentsMargins(7, 1, 5, 1)
        lay.setSpacing(4)
        col = QVBoxLayout()
        col.setContentsMargins(0, 0, 0, 0)
        col.setSpacing(0)
        col.addStretch(1)
        title_lab = ElidedLabel(title)
        title_lab.setObjectName("mapCellDash" if title == "—" else "mapCellTitle")
        title_lab.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Fixed)
        col.addWidget(title_lab)
        if effect:
            eff = ElidedLabel(effect)
            eff.setObjectName("mapCellEffect")
            eff.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Fixed)
            col.addWidget(eff)
        col.addStretch(1)
        chev = QLabel("⌄")
        chev.setObjectName("mapCellChev")
        chev.setVisible(self.editable)
        lay.addLayout(col, 1)
        lay.addWidget(chev, 0, Qt.AlignVCenter)

        for w in (title_lab, chev):
            w.setAttribute(Qt.WA_TransparentForMouseEvents)
        tip_text = tip or (f"{title}" + (f" · {effect}" if effect else ""))
        if editable:
            tip_text += "\n点击选择常用动作"
        self.setToolTip(tip_text)

    def sizeHint(self) -> QSize:  # noqa: N802
        return QSize(120, 33)

    def minimumSizeHint(self) -> QSize:  # noqa: N802
        return QSize(0, 33)

    def mouseReleaseEvent(self, e) -> None:  # noqa: N802
        if (self.editable and e.button() == Qt.LeftButton
                and self.rect().contains(e.position().toPoint())):
            self.clicked.emit(self.vk, self.gesture)
        super().mouseReleaseEvent(e)

    def enterEvent(self, e) -> None:  # noqa: N802
        self.hovered.emit(self.vk, True)
        super().enterEvent(e)

    def leaveEvent(self, e) -> None:  # noqa: N802
        self.hovered.emit(self.vk, False)
        super().leaveEvent(e)


class MapKeyRow(QFrame):
    """一行四个部分：键帽 + 单击/双击/长按三个可编辑手势格。"""

    picked = Signal(int)
    cell_clicked = Signal(int, str)
    hovered = Signal(int, bool)

    def __init__(self, vk: int, cells: dict[str, tuple], selected: bool = False,
                 parent=None):
        super().__init__(parent)
        self.vk = int(vk)
        self.setObjectName("mapKeyRow")
        self.setFixedHeight(35)
        self.setCursor(Qt.PointingHandCursor)
        self.setProperty("sel", "true" if selected else "false")

        lay = QHBoxLayout(self)
        lay.setContentsMargins(6, 2, 10, 2)
        lay.setSpacing(8)

        key_box = QWidget()
        key_box.setFixedWidth(88)
        klay = QHBoxLayout(key_box)
        klay.setContentsMargins(0, 0, 0, 0)
        klay.setSpacing(8)
        chip = QLabel(MAP_GLYPH.get(vk, "?"))
        chip.setObjectName("mapChip")
        chip.setFixedSize(24, 24)
        chip.setAlignment(Qt.AlignCenter)
        name = QLabel(MAP_NAME.get(vk, f"0x{vk:02X}"))
        name.setObjectName("mapKeyName")
        name.setFixedWidth(50)
        klay.addWidget(chip)
        klay.addWidget(name)
        key_box.setAttribute(Qt.WA_TransparentForMouseEvents)
        for w in (chip, name):
            w.setAttribute(Qt.WA_TransparentForMouseEvents)
        lay.addWidget(key_box)

        self.cells: dict[str, MapCell] = {}
        for gesture in ("single", "double", "hold"):
            title, effect, editable, tip = cells[gesture]
            cell = MapCell(vk, gesture, title, effect, editable, tip)
            cell.clicked.connect(self.cell_clicked)
            cell.hovered.connect(self.hovered)
            self.cells[gesture] = cell
            lay.addWidget(cell, 1)

        self.setToolTip(f"{MAP_NAME.get(vk, vk)} · 点键帽选中，点手势格改键")

    def set_selected(self, on: bool) -> None:
        self.setProperty("sel", "true" if on else "false")
        self.style().unpolish(self)
        self.style().polish(self)

    def mouseReleaseEvent(self, e) -> None:  # noqa: N802
        if e.button() == Qt.LeftButton and self.rect().contains(e.position().toPoint()):
            self.picked.emit(self.vk)
        super().mouseReleaseEvent(e)

    def enterEvent(self, e) -> None:  # noqa: N802
        self.hovered.emit(self.vk, True)
        super().enterEvent(e)

    def leaveEvent(self, e) -> None:  # noqa: N802
        self.hovered.emit(self.vk, False)
        super().leaveEvent(e)


class MainWindow(QMainWindow):
    def __init__(self, with_tray: bool = True):
        super().__init__()
        self.ctl = Controller()
        self.toast = ModeToast()
        self._seq = 0
        self._drag = None
        self._rows: list = []
        self._last_profile = ""
        self._bt_tick = 0
        self._seen_remote = False
        self._model_sig = None
        self._last_voice_mode = ""
        self._map_sel_vk = 38
        self.map_rows: dict[int, MapKeyRow] = {}
        self.profiles_path = HERE / "profiles.json"

        self.setWindowTitle("AgentRMC")
        self.resize(1000, 640)
        self.setMinimumSize(880, 560)
        self.setWindowFlags(Qt.Window | Qt.FramelessWindowHint)
        self._settings = QSettings("AgentRMC", "AgentRMC")
        self._screen_hooked = False
        self._build()
        self._refresh_info()
        self._restore_geometry()

        # 关窗 = 隐藏到托盘继续监听；真正退出走托盘菜单。系统没有托盘时
        # 退回老行为（关窗即退出），否则窗口就再也叫不回来了。
        self.close_to_tray = bool((core.CFG.get("ui") or {}).get("close_to_tray", True))
        self._really_quit = False
        self._tray_hinted = False
        self._tray = self._build_tray() if with_tray else None

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(70)
        QTimer.singleShot(400, self._refresh_bt_state)
        self.guide_first = str(self._settings.value("guide/seen", "") or "") != "1"
        self.tabs.currentChanged.connect(self._tab_changed)

    # ---------- 托盘 ----------
    def _build_tray(self) -> QSystemTrayIcon | None:
        """常驻托盘：窗口关掉后继续在后台监听，点托盘图标叫回界面。"""
        if not QSystemTrayIcon.isSystemTrayAvailable():
            return None
        ico = HERE / "assets" / "icon.ico"
        if not ico.exists():
            ico = HERE / "assets" / "icon.png"
        icon = QIcon(str(ico))
        if icon.isNull():
            icon = self.windowIcon()
        tray = QSystemTrayIcon(icon, self)
        menu = QMenu()
        self.tray_show = menu.addAction("显示主界面", self._tray_reveal)
        self.tray_hide = menu.addAction("隐藏到托盘", self.hide)
        menu.addSeparator()
        self.tray_listen = menu.addAction("停止监听", self._toggle)
        menu.addSeparator()
        menu.addAction("退出 AgentRMC", self._quit_app)
        tray.setContextMenu(menu)
        tray.setToolTip("AgentRMC")
        tray.activated.connect(self._on_tray_click)
        tray.show()
        # 窗口隐藏后不能靠“最后一个窗口关闭”退出，否则一关窗进程就没了
        QApplication.instance().setQuitOnLastWindowClosed(False)
        self._update_tray()
        return tray

    def tray_ready(self) -> bool:
        return getattr(self, "_tray", None) is not None

    def _on_tray_click(self, reason) -> None:
        if reason in (QSystemTrayIcon.Trigger, QSystemTrayIcon.DoubleClick):
            self._tray_reveal()

    def _tray_reveal(self) -> None:
        self.showNormal()
        self.raise_()
        self.activateWindow()

    def _quit_app(self) -> None:
        """托盘菜单里的真退出。"""
        self._really_quit = True
        if not self.isVisible():
            # 隐藏状态下 close() 不会走 closeEvent，这里自己收尾
            self.ctl.stop()
            if self._tray is not None:
                self._tray.hide()
            QApplication.instance().quit()
            return
        self.close()

    def _update_tray(self) -> None:
        tray = getattr(self, "_tray", None)
        if tray is None:
            return
        try:
            name = str((self.ctl.profiles.current() or {}).get("name", ""))
        except Exception:  # noqa: BLE001
            name = ""
        state = "监听中" if self.ctl.running else "已停止"
        tray.setToolTip("AgentRMC · " + " · ".join(x for x in (state, name) if x))
        self.tray_listen.setText("停止监听" if self.ctl.running else "开始监听")
        self.tray_hide.setEnabled(self.isVisible())

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
            self._settings.sync()

    def _tab_changed(self, index: int) -> None:
        if index != 2:
            self._mark_guide_seen()
        if hasattr(self, "panel"):
            self.panel.set_link(self._map_sel_vk if index == 1 else None)
        if index == 1:
            # 页面刚变成可见时重建一次，避免“建在隐藏页上”的行没被绘制
            QTimer.singleShot(0, self._refresh_keymap)

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
        grid.addWidget(self._build_left_column())
        grid.addWidget(self._build_right(), 1)

        outer.addWidget(body, 1)

    def _build_left_column(self) -> QWidget:
        col = QWidget()
        col.setFixedWidth(286)
        col.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Expanding)
        lay = QVBoxLayout(col)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(12)
        lay.addWidget(self._build_remote_card(), 1)
        lay.addWidget(self._build_voice_card(), 0)
        return col

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
        card.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Expanding)
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
        self.panel.setToolTip("双击 返回 / 菜单 / 主页 进入对应模式\n"
                              "12 秒无操作自动退出")
        lay.addWidget(self.panel, 1)

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

    def _build_voice_card(self) -> QWidget:
        card = QFrame()
        card.setObjectName("card")
        self.voice_card = card
        card.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        lay = QVBoxLayout(card)
        lay.setContentsMargins(14, 10, 14, 10)
        lay.setSpacing(5)

        head = QHBoxLayout()
        head.setSpacing(6)
        title = QLabel("语音")
        title.setObjectName("cardTitle")
        self.voice_title = title
        self.rec_label = QLabel("")
        self.rec_label.setStyleSheet(
            f"color: {RED}; font-size: 10.5px; font-weight: 700;")
        self.btn_test = QPushButton("试麦")
        self.btn_test.setObjectName("ghostText")
        self.btn_test.setToolTip("录 4 秒并转写，验证麦克风")
        self.btn_test.setCursor(Qt.PointingHandCursor)
        self.btn_test.clicked.connect(self._test_mic)
        head.addWidget(title)
        head.addStretch(1)
        head.addWidget(self.rec_label)
        head.addWidget(self.btn_test)
        lay.addLayout(head)

        self.wave = Waveform()
        self.wave.setFixedHeight(10)
        self.wave.setToolTip("录音电平")
        lay.addWidget(self.wave)

        self.result = QLabel("按住语音键说话")
        self.result.setObjectName("result")
        self.result.setProperty("idle", "true")
        self.result.setWordWrap(True)
        self.result.setFixedHeight(48)
        self.result.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Fixed)
        self.result.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        lay.addWidget(self.result)

        controls = QHBoxLayout()
        controls.setSpacing(8)
        self.btn_toggle = QPushButton("开始监听")
        self.btn_toggle.setObjectName("voicePrimary")
        self.btn_toggle.setFixedHeight(28)
        self.btn_toggle.setCursor(Qt.PointingHandCursor)
        self.btn_toggle.clicked.connect(self._toggle)

        controls.addWidget(self.btn_toggle, 1)
        lay.addLayout(controls)

        lay.addWidget(self._build_model_row())
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

    def _build_model_row(self) -> QWidget:
        row = QWidget()
        lay = QHBoxLayout(row)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(7)

        self.model_switch = ToggleSwitch(self.ctl.voice_mode == "local")
        self.model_switch.setToolTip(
            "开 = 本地 whisper 自己转写（加载模型）\n"
            "关 = 语音键交给 Qoder / WorkBuddy 识别（不加载模型）")
        self.model_switch.toggled.connect(self._on_model_toggle)

        self.model_combo = QComboBox()
        self.model_combo.setObjectName("combo")
        self.model_combo.setProperty("compact", "true")
        self.model_combo.setCursor(Qt.PointingHandCursor)
        self.model_combo.setFixedWidth(84)
        cur = normalize_model_size(self.ctl.model_size)
        for label, value in MODEL_CHOICES:
            self.model_combo.addItem(label, value)
        if self.model_combo.findData(cur) < 0:      # 配置里写了其它尺寸也照实显示
            self.model_combo.addItem(cur, cur)
        idx = self.model_combo.findData(cur)
        if idx >= 0:
            self.model_combo.blockSignals(True)
            self.model_combo.setCurrentIndex(idx)
            self.model_combo.blockSignals(False)
        self.model_combo.currentIndexChanged.connect(self._on_model_size)

        self.model_status = QLabel("")
        self.model_status.setFixedWidth(44)
        self.model_status.setAlignment(Qt.AlignRight | Qt.AlignVCenter)

        label = QLabel("本地模型")
        label.setStyleSheet(f"color: {MUTED}; font-size: 10.5px;")
        lay.addWidget(label)
        lay.addWidget(self.model_switch)
        lay.addStretch(1)
        lay.addWidget(self.model_combo)
        lay.addWidget(self.model_status)

        self._sync_model_ui()
        return row

    def _sync_model_ui(self) -> None:
        c = self.ctl
        local = c.voice_mode == "local"
        self.model_switch.setChecked(local)
        idx = self.model_combo.findData(normalize_model_size(c.model_size))
        if idx >= 0 and idx != self.model_combo.currentIndex():
            self.model_combo.blockSignals(True)
            self.model_combo.setCurrentIndex(idx)
            self.model_combo.blockSignals(False)
        if not local:
            text, color = "未启用", MUTED
        elif c.model_state == "loading":
            text, color = "加载中…", ORANGE
        elif c.model_state == "ready":
            text, color = "就绪", "#7ee2a8"
        elif c.model_state == "error":
            text, color = "失败", RED
        else:
            text, color = "待加载", MUTED
        self.model_status.setText(text)
        self.model_status.setStyleSheet(f"color: {color}; font-size: 11px;")
        self.model_status.setToolTip(c.model_error if c.model_state == "error" else text)

        marks = []
        for name, value in MODEL_CHOICES:
            has = (HERE / "models" / f"faster-whisper-{value}" / "model.bin").exists()
            marks.append(f"{name} {'已下载' if has else '未下载'}")
        self.model_combo.setToolTip(
            "切尺寸立刻生效：开关开着会重新加载，关着只记下选择\n" + " · ".join(marks))

    def _on_model_toggle(self, on: bool) -> None:
        self.ctl.set_local_model(on)
        self._sync_model_ui()
        self._refresh_info()
        self._refresh_keymap()

    def _on_model_size(self, _index: int) -> None:
        size = self.model_combo.currentData()
        if size:
            self.ctl.set_model_size(str(size))
        self._sync_model_ui()
        self._refresh_info()

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
        lay.setContentsMargins(14, 11, 12, 11)
        lay.setSpacing(8)

        # 场景：当前前台命中谁，一眼可见
        head = QHBoxLayout()
        head.setSpacing(8)
        left = QVBoxLayout()
        left.setSpacing(1)
        self.map_kicker = QLabel("当前场景")
        self.map_kicker.setObjectName("sceneKicker")
        self.map_kicker.setVisible(False)
        state = QHBoxLayout()
        state.setSpacing(6)
        self.map_scene_dot = QLabel()
        self.map_scene_dot.setFixedSize(6, 6)
        self.map_scene_dot.setStyleSheet(f"background: {GREEN}; border-radius: 3px;")
        self.map_scene_name = QLabel("—")
        self.map_scene_name.setObjectName("sceneName")
        self.map_scene_rule = ElidedLabel("")
        self.map_scene_rule.setObjectName("sceneRule")
        state.addWidget(self.map_scene_dot, 0, Qt.AlignVCenter)
        state.addWidget(self.map_scene_name)
        state.addSpacing(2)
        state.addWidget(self.map_scene_rule, 1)
        left.addWidget(self.map_kicker)
        left.addLayout(state)

        self.map_combo = QComboBox()
        self.map_combo.setObjectName("combo")
        self.map_combo.setCursor(Qt.PointingHandCursor)
        self.map_combo.setFixedWidth(104)
        self.map_combo.setToolTip(
            "跟随前台：切窗口自动换映射\n选具体场景：固定预览这个场景")
        self.map_combo.addItem("跟随前台", None)
        for p in self.ctl.profiles.profiles:
            self.map_combo.addItem(p.get("name", "?"), p.get("name"))
        self.map_combo.currentIndexChanged.connect(self._refresh_keymap)
        head.addLayout(left, 1)
        head.addWidget(self.map_combo, 0, Qt.AlignTop)
        lay.addLayout(head)

        # 表头也放进滚动区：和行共用同一列宽，不会被滚动条挤出对齐
        header = QWidget()
        hv = QVBoxLayout(header)
        hv.setContentsMargins(0, 0, 0, 0)
        hv.setSpacing(2)
        hlay = QHBoxLayout()
        hlay.setContentsMargins(6, 1, 10, 0)
        hlay.setSpacing(8)
        for text, width, stretch in (("键位", 88, 0), ("单击", 0, 1),
                                     ("双击", 0, 1), ("长按", 0, 1)):
            lab = QLabel(text)
            lab.setObjectName("mapHead")
            if width:
                lab.setFixedWidth(width)
            hlay.addWidget(lab, stretch)
        hv.addLayout(hlay)
        hair = QFrame()
        hair.setObjectName("mapHair")
        hair.setFixedHeight(1)
        hv.addWidget(hair)

        self.map_scroll = QScrollArea()
        self.map_scroll.setWidgetResizable(True)
        # 窗口被夹到小屏时宁可横滚，也不让三列手势被裁掉
        self.map_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        holder = QWidget()
        holder.setStyleSheet("background: transparent;")
        holder.setMinimumWidth(500)
        self.map_grid = QVBoxLayout(holder)
        self.map_grid.setContentsMargins(0, 0, 4, 0)
        self.map_grid.setSpacing(0)
        self.map_grid.addWidget(header)
        self.map_scroll.setWidget(holder)
        lay.addWidget(self.map_scroll, 1)

        foot = QHBoxLayout()
        foot.setSpacing(8)
        self.map_foot_hint = ElidedLabel("点击手势格改键 · 自动保存")
        self.map_foot_hint.setObjectName("mapFoot")
        self.map_foot_hint.setStyleSheet("QLabel#mapFoot { color: #5b6472; font-size: 10px; }")
        self.map_foot_hint.setToolTip(
            "点单击/双击/长按单元格选择常用动作\n"
            "写回 profiles.json，并保留上一版 profiles.json.bak")
        self._map_hint_timer = QTimer(self)
        self._map_hint_timer.setSingleShot(True)
        self._map_hint_timer.timeout.connect(self._reset_map_hint)

        btn_open = QPushButton("高级配置")
        btn_open.setObjectName("ghostSmall")
        btn_open.setToolTip("直接编辑 profiles.json（匹配规则、语音接法等）")
        btn_open.setCursor(Qt.PointingHandCursor)
        btn_open.clicked.connect(self._open_profiles)
        btn_reload = QPushButton("重新加载")
        btn_reload.setObjectName("ghostSmall")
        btn_reload.setCursor(Qt.PointingHandCursor)
        btn_reload.clicked.connect(self._reload_profiles)
        foot.addWidget(self.map_foot_hint, 1)
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

    @staticmethod
    def _match_summary(prof: dict) -> tuple[str, str]:
        match = prof.get("match", {}) or {}
        procs = [str(x) for x in (match.get("process") or [])]
        titles = [str(x) for x in (match.get("title") or [])]

        def few(items: list[str]) -> str:
            if not items:
                return ""
            if len(items) == 1:
                return items[0]
            joined = " / ".join(items)
            if len(items) == 2 and len(joined) <= 26:
                return joined
            return f"{items[0]} 等 {len(items)} 个"

        bits = []
        if procs:
            bits.append(few(procs))
        elif titles:
            bits.append("标题 " + few(titles))
        short = " · ".join(bits) or "兜底 · 无匹配条件"
        full = []
        if procs:
            full.append("进程：" + " / ".join(procs))
        if titles:
            full.append("标题：" + " / ".join(titles))
        return short, "\n".join(full) or "没有匹配条件，作为兜底场景"

    def _voice_cells(self, prof: dict) -> tuple[str, str, str]:
        """语音键不走 keys 表，接法由 profile.voice + 本地模型开关决定。"""
        if self.ctl.voice_mode == "hotkey":
            mods, hk_vk, trigger = core.voice_hotkey(prof)
            desc = describe_voice(mods, hk_vk, trigger, short=True)
            combo = desc.split(" ", 1)[-1]      # “开关 CTRL+SHIFT+V” → 只留组合键
            if trigger == "toggle":
                tip = "开关式：按下开始、松手结束，交给 Qoder / WorkBuddy"
            else:
                tip = "按住式：按住说话、松手结束，交给 Qoder / WorkBuddy"
            return "语音输入", combo, tip
        return "语音输入", "本地 whisper", "按住遥控器语音键说话，松手后自动转写"

    @staticmethod
    def _action_effect(act: dict) -> str:
        return action_effect_text(act)

    @staticmethod
    def _double_cells(vk: int, prof: dict, actions: dict,
                      has_nav: bool) -> tuple[str, str, bool, str]:
        spec = (prof.get("double") or {}).get(str(vk))
        if spec == "none":
            return ("—", "已关闭", True,
                    "双击已关闭；单击不再等双击窗口")
        if spec:
            act = actions.get(str(spec), {}) or {}
            title = (act.get("label") or ACTION_CN.get(str(spec))
                     or str(spec))
            effect = MainWindow._action_effect(act)
            if effect == title:
                effect = ""
            tip = f"双击 {title}" + (f" · {effect}" if effect else "")
            return (title, effect, True, tip)
        if vk in DOUBLE_GESTURE and (vk != 36 or has_nav):
            label, tip = DOUBLE_GESTURE[vk]
            return (label, "双击切换" if vk == 36 else "双击进入", True, tip)
        return ("—", "", True, "双击无动作，点击选择常用动作")

    @staticmethod
    def _hold_cells(vk: int, prof: dict, actions: dict,
                    act: dict) -> tuple[str, str, bool, str]:
        spec = (prof.get("hold") or {}).get(str(vk))
        if spec == "none":
            return ("—", "已关闭", True, "长按已关闭")
        if spec in ("repeat", "repeat_accel") or (
                not spec and act.get("hold_repeat")):
            accel = (spec == "repeat_accel"
                     or (not spec and bool(act.get("hold_accel"))))
            if act.get("type") == "scroll":
                return ("连滚加速" if accel else "连续滚动",
                        "越按越快" if accel else "按住重复", True,
                        "按住重复当前滚动" + ("，越按越快" if accel else ""))
            return ("连发加速" if accel else "连续触发",
                    "越按越快" if accel else "按住重复", True,
                    "按住重复当前动作" + ("，越按越快" if accel else ""))
        if spec:
            hold_act = actions.get(str(spec), {}) or {}
            title = (hold_act.get("label")
                     or ACTION_CN.get(str(spec)) or str(spec))
            return (title, "按住触发", True,
                    f"长按 {title} · 按住 0.6 秒后触发")
        if act.get("hold_action"):
            ha = act["hold_action"]
            title = ha.get("label") or core.describe_action(ha, hold_info=False)
            return (title, "按住触发", True,
                    f"长按 {title} · 按住 0.6 秒后触发")
        return ("—", "", True, "长按无动作，点击选择常用动作")

    def _refresh_keymap(self) -> None:
        if not hasattr(self, "map_grid"):
            return
        # 第 0 项是常驻表头，只清掉它后面的内容
        while self.map_grid.count() > 1:
            item = self.map_grid.takeAt(1)
            w = item.widget()
            if w is not None:
                w.hide()
                w.setParent(None)
                w.deleteLater()
        self.map_rows: dict[int, MapKeyRow] = {}

        prof, auto = self._current_map_profile()
        self.map_kicker.setText("当前场景" if auto else "预览场景")
        self.map_scene_dot.setStyleSheet(
            f"background: {GREEN if auto else ORANGE}; border-radius: 3px;")
        self.map_scene_name.setText(str(prof.get("name", "?")))
        # 同行右侧的规则说明是可压缩的，布局会把场景名挤到比 sizeHint 还窄
        # （"default" 变成 "defaul"），按当前字体锁一下最小宽度。
        self.map_scene_name.setMinimumWidth(self.map_scene_name.sizeHint().width())
        rule_short, rule_full = self._match_summary(prof)
        self.map_scene_rule.setText("· " + rule_short)
        self.map_scene_rule.setToolTip(rule_full)

        keys = prof.get("keys", {}) or {}
        actions = prof.get("actions", {}) or {}
        nav_keys = (prof.get("nav") or {}).get("keys") or {}
        has_nav = bool(nav_keys)

        for gname, vks in MAP_GROUPS:
            self.map_grid.addWidget(map_section(gname))
            for vk in vks:
                if vk == 116:
                    vtitle, veffect, vtip = self._voice_cells(prof)
                    cells = {
                        "single": (vtitle, veffect, False, vtip),
                        "double": ("—", "", False, "语音键不支持双击"),
                        "hold": ("按住说话", "", False, vtip),
                    }
                elif vk == 122:
                    sem = keys.get("122", "")
                    act = actions.get(sem, {}) if sem else {}
                    title, effect = action_cells(act, vk, sem)
                    cells = {
                        "single": (title, effect, False,
                                   "F11 原生直通，不能改为其它动作"),
                        "double": ("—", "", False, "对焦键不支持双击"),
                        "hold": ("—", "", False, "对焦键不支持长按"),
                    }
                else:
                    sem = keys.get(str(vk), "")
                    act = actions.get(sem, {}) if sem else {}
                    title, effect = action_cells(act, vk, sem)
                    single_tip = f"{title}" + (f" · {effect}" if effect else "")
                    cells = {
                        "single": (title, effect, True, single_tip),
                        "double": self._double_cells(vk, prof, actions, has_nav),
                        "hold": self._hold_cells(vk, prof, actions, act),
                    }
                row = MapKeyRow(vk, cells, selected=(vk == self._map_sel_vk))
                row.picked.connect(self._select_map_key)
                row.cell_clicked.connect(self._open_preset_menu)
                row.hovered.connect(self._hover_map_key)
                self.map_rows[vk] = row
                self.map_grid.addWidget(row)

        if not keys:
            hint = QLabel("这个场景没有配置按键，物理键仍按原生行为工作")
            hint.setObjectName("mapEmpty")
            hint.setWordWrap(True)
            self.map_grid.addWidget(hint)
        self.map_grid.addStretch(1)

        # 行和分组标题都是「先无父控件创建、再进布局」，窗口已经显示时 Qt 不会
        # 自动把它们置为可见，必须显式 show，否则表体只剩空白。
        for i in range(self.map_grid.count()):
            w = self.map_grid.itemAt(i).widget()
            if w is not None:
                w.show()

        # 后台/刚启动时重建也要立刻分配宽度，否则新行在 grab/曝光前是 0 宽
        self.map_grid.activate()
        if self.map_grid.parentWidget() is not None:
            self.map_grid.parentWidget().updateGeometry()

        # 映射页总是显示选中键的蓝环；离开页签时清掉
        if self.tabs.currentIndex() == 1:
            self.panel.set_link(self._map_sel_vk)

    def _select_map_key(self, vk: int) -> None:
        self._map_sel_vk = int(vk)
        for v, row in self.map_rows.items():
            row.set_selected(v == self._map_sel_vk)
        self.panel.set_link(self._map_sel_vk)

    def _hover_map_key(self, vk: int, on: bool) -> None:
        self.panel.set_link(vk if on else self._map_sel_vk)

    def _reset_map_hint(self) -> None:
        if not hasattr(self, "map_foot_hint"):
            return
        self.map_foot_hint.setText("点击手势格改键 · 自动保存")
        self.map_foot_hint.setStyleSheet(
            "QLabel#mapFoot { color: #5b6472; font-size: 10px; }")

    def _flash_map_hint(self, text: str, color: str) -> None:
        if not hasattr(self, "map_foot_hint"):
            return
        self.map_foot_hint.setText(text)
        self.map_foot_hint.setStyleSheet(
            f"QLabel#mapFoot {{ color: {color}; font-size: 10px; }}")
        self._map_hint_timer.start(2600)

    @staticmethod
    def _gesture_label(gesture: str, kind: str) -> str:
        if kind == "none":
            return "已关闭"
        if kind == "repeat":
            return "连续触发"
        if kind == "repeat_accel":
            return "连发加速"
        if kind in DOUBLE_MODE_PRESETS:
            return str(DOUBLE_MODE_PRESETS[kind].get("label") or kind)
        return PRESET_LABELS.get(kind, kind)

    def _add_preset_menu_groups(self, menu: QMenu, pname: str, vk: int,
                                gesture: str) -> None:
        for gname, ids in PRESET_GROUPS:
            sub = menu.addMenu(gname)
            for pid in ids:
                act = sub.addAction(PRESET_LABELS[pid])
                act.setToolTip(PRESET_LABELS[pid])
                act.triggered.connect(
                    lambda _=False, p=pid: self._apply_mapping(
                        pname, vk, gesture, p))

    def _build_preset_menu(self, vk: int, gesture: str) -> QMenu | None:
        prof, _auto = self._current_map_profile()
        pname = str(prof.get("name", ""))
        row = self.map_rows.get(vk)
        cell = row.cells.get(gesture) if row else None
        if cell is None or not cell.editable:
            return None
        menu = QMenu(self)
        cur = cell.title_text
        if cell.effect_text:
            cur += " · " + cell.effect_text
        head = menu.addAction(f"当前：{cur}")
        head.setEnabled(False)
        menu.addSeparator()

        if gesture == "single":
            act = menu.addAction("原生直通")
            act.triggered.connect(
                lambda _=False: self._apply_mapping(pname, vk, "single", "pass"))
            menu.addSeparator()
            self._add_preset_menu_groups(menu, pname, vk, "single")
        elif gesture == "double":
            act = menu.addAction("关闭双击")
            act.triggered.connect(
                lambda _=False: self._apply_mapping(pname, vk, "double", "none"))
            modes = menu.addMenu("模式")
            for mid, label in (("mode_media", "音量/亮度模式"),
                               ("mode_cursor", "鼠标模式")):
                a = modes.addAction(label)
                a.triggered.connect(
                    lambda _=False, m=mid: self._apply_mapping(
                        pname, vk, "double", m))
            if (prof.get("nav") or {}).get("keys"):
                a = modes.addAction("导航模式")
                a.triggered.connect(
                    lambda _=False: self._apply_mapping(
                        pname, vk, "double", "nav_toggle"))
            menu.addSeparator()
            self._add_preset_menu_groups(menu, pname, vk, "double")
        else:
            act = menu.addAction("关闭长按")
            act.triggered.connect(
                lambda _=False: self._apply_mapping(pname, vk, "hold", "none"))
            act = menu.addAction("连续触发（按住重复）")
            act.triggered.connect(
                lambda _=False: self._apply_mapping(pname, vk, "hold", "repeat"))
            act = menu.addAction("连续加速（按住越来越快）")
            act.triggered.connect(
                lambda _=False: self._apply_mapping(
                    pname, vk, "hold", "repeat_accel"))
            menu.addSeparator()
            sub = menu.addMenu("按住动作")
            for pid in HOLD_PRESET_IDS:
                a = sub.addAction(PRESET_LABELS[pid])
                a.triggered.connect(
                    lambda _=False, p=pid: self._apply_mapping(
                        pname, vk, "hold", p))
        return menu

    def _open_preset_menu(self, vk: int, gesture: str) -> None:
        self._select_map_key(vk)
        menu = self._build_preset_menu(vk, gesture)
        if menu is None:
            return
        row = self.map_rows.get(vk)
        cell = row.cells.get(gesture) if row else None
        if cell is None:
            return
        menu.exec(cell.mapToGlobal(QPoint(0, cell.height())))

    def _write_raw_profiles(self, path: Path, raw: dict) -> None:
        text = json.dumps(raw, ensure_ascii=False, indent=2) + "\n"
        tmp = path.with_name(path.name + ".tmp")
        bak = path.with_name(path.name + ".bak")
        tmp.write_text(text, encoding="utf-8")
        if path.exists():
            bak.write_bytes(path.read_bytes())
        os.replace(tmp, path)

    def _apply_mapping(self, pname: str, vk: int, gesture: str,
                       kind: str) -> None:
        path = self.profiles_path
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(raw, dict) or not isinstance(raw.get("profiles"), list):
                raise ValueError("profiles.json 结构不对")
            if not apply_mapping_preset(raw, pname, vk, gesture, kind):
                raise ValueError("没有找到这个场景或预设")
            self._write_raw_profiles(path, raw)
        except Exception as e:  # noqa: BLE001
            self._flash_map_hint(f"保存失败 · {e}", RED)
            QMessageBox.warning(self, "保存失败", f"{e}\n\n{path}")
            return

        self.ctl.profiles = core.ProfileManager(core.load_profiles())
        self.ctl.router.profiles = self.ctl.profiles
        self.ctl.router.reset()
        self._refresh_info()
        self._refresh_keymap()
        label = self._gesture_label(gesture, kind)
        text = (f"已保存 · {pname} · {MAP_NAME.get(vk, vk)} "
                f"{GESTURE_CN.get(gesture, gesture)} → {label}")
        self._flash_map_hint(text, GREEN)
        self._append({"kind": "status", "text": text,
                      "ts": time.strftime("%H:%M:%S")})

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
        path = self.profiles_path
        try:
            os.startfile(str(path))  # noqa: S606
            self._append({"kind": "status",
                          "text": "已打开 profiles.json，改完点『重新加载』",
                          "ts": time.strftime("%H:%M:%S")})
        except Exception as e:  # noqa: BLE001
            self._append({"kind": "error", "text": f"打开失败: {e}",
                          "ts": time.strftime("%H:%M:%S")})

    def _reload_profiles(self) -> None:
        try:
            pinned = self.map_combo.currentData()
            self.ctl.profiles = core.ProfileManager(core.load_profiles())
            self.ctl.router.profiles = self.ctl.profiles
            self.ctl.router.reset()
            self.map_combo.blockSignals(True)
            self.map_combo.clear()
            self.map_combo.addItem("跟随前台", None)
            for p in self.ctl.profiles.profiles:
                self.map_combo.addItem(p.get("name", "?"), p.get("name"))
            if pinned is not None:
                idx = self.map_combo.findData(pinned)
                if idx >= 0:
                    self.map_combo.setCurrentIndex(idx)
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
        else:
            wire = "本地 whisper"
        media_hint = (" · 耳机播放键开/关"
                      if (core.CFG.get("media_voice") or {}).get("enabled", True)
                      and c.voice_mode == "hotkey" else "")
        if hasattr(self, "voice_title"):
            self.voice_title.setToolTip(
                f"语音接法 {wire}{media_hint}\n"
                f"指令 {len(c.rules.rules)} 条 · 设备 {core.CFG.get('device_mark', '-')}")
        self._update_tray()

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
        self._update_tray()

    def _set_result(self, state: str, text: str, meta: str) -> None:
        main = html.escape(str(text or ""))
        self.result.setText(f'<div style="font-size:12px;">{main}</div>')
        self.result.setToolTip(str(text or "") + (f"\n{meta}" if meta else ""))
        for p in ("idle", "live", "done"):
            self.result.setProperty(p, "true" if p == state else "false")
        self.result.style().unpolish(self.result)
        self.result.style().polish(self.result)

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
                if ev.get("source") == "headset":
                    self._set_result("live", "正在聆听…", "再按耳机播放键停止")
                elif ev.get("mode") == "hotkey":
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
        if c.state == "recording":
            held = max(0.0, time.time() - c.record_started)
            self.rec_label.setText(f"● REC {held:.0f}s")
        pname = c.profiles.current().get("name", "?")
        self.pill_profile.setText(pname)
        if pname != self._last_profile:
            self._last_profile = pname
            self._refresh_info()
            if self.map_combo.currentData() is None:   # 跟随前台时才自动刷
                self._refresh_keymap()
        sig = (c.voice_mode, c.model_state, c.model_size, c.model_error)
        if sig != self._model_sig:
            changed_mode = c.voice_mode != self._last_voice_mode
            self._model_sig = sig
            self._last_voice_mode = c.voice_mode
            self._sync_model_ui()
            self._refresh_info()
            if changed_mode:
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
        self._update_tray()
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
            self._settings.sync()
        except Exception:  # noqa: BLE001
            pass
        if self._tray is not None and self.close_to_tray and not self._really_quit:
            # 关窗 = 收进托盘，监听照跑
            e.ignore()
            self.hide()
            if not self._tray_hinted:
                self._tray_hinted = True
                self._tray.showMessage(
                    "AgentRMC 仍在后台监听",
                    "点托盘图标叫回界面；要真退出用托盘菜单的「退出 AgentRMC」。",
                    QSystemTrayIcon.Information, 4000)
            return
        self.ctl.stop()
        if self._tray is not None:
            self._tray.hide()
        super().closeEvent(e)
        QApplication.instance().quit()


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

    # 截图/自测不建托盘图标，免得验收时右下角反复闪
    win = MainWindow(with_tray=not shot)
    start_minimized = bool((core.CFG.get("ui") or {}).get("start_minimized"))
    first_run = win.guide_first
    if shot and "--onscreen" not in sys.argv:
        # 验收截图不该在桌面上闪一个窗口出来：走完整显示/布局流程但不建原生窗口
        win.setAttribute(Qt.WA_DontShowOnScreen)
        win.show()
    elif start_minimized and not first_run and not shot and "--selftest" not in sys.argv:
        if win.tray_ready():
            # 无边框窗口 showMinimized() 会先在屏幕上闪一个 137x51 的最小化残影；
            # 有托盘就干脆不显示，点托盘图标再叫出来。
            pass
        else:
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
