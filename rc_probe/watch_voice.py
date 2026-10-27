"""一次性诊断：语音开始时抓一张全屏，确认所谓“任务栏”是谁的。

用法（后台运行，按一次遥控器语音键就会出图）：
    python rc_probe/watch_voice.py 180
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import agentrc as core  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402

LOG = ROOT / "release" / "AgentRMC" / "rc_probe" / "gui_events.log"
OUT = ROOT / "rc_probe"


def grab(name: str) -> None:
    app = QApplication.instance() or QApplication([])
    ppm = QGuiApplication.primaryScreen().grabWindow(0)
    path = OUT / name
    ppm.save(str(path))
    exe, title = core.foreground_info()
    (OUT / f"{name}.txt").write_text(
        f"{time.strftime('%H:%M:%S')} fg={exe}|{title} tray={core.taskbar_state()}",
        encoding="utf-8")
    print(f"[capture] {path} fg={exe}|{title} tray={core.taskbar_state()}", flush=True)


def main() -> None:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 180.0
    offset = LOG.stat().st_size if LOG.exists() else 0
    print(f"watching {LOG} from offset {offset}", flush=True)
    deadline = time.time() + seconds
    while time.time() < deadline:
        if LOG.exists() and LOG.stat().st_size > offset:
            with LOG.open("r", encoding="utf-8", errors="replace") as f:
                f.seek(offset)
                new = f.read()
                offset = f.tell()
            for line in new.splitlines():
                try:
                    ev = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if ev.get("kind") == "voice_start":
                    time.sleep(0.7)
                    grab("voice_start.png")
                    time.sleep(1.0)
                    grab("voice_start_1s.png")
                    return
        time.sleep(0.15)
    print("timeout, no voice_start seen", flush=True)


if __name__ == "__main__":
    main()
