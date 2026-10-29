"""无头验收：本地模型开关 + 尺寸切换。

用临时 config.json 跑真实 MainWindow：
1. 关：不加载模型，写回 hotkey；
2. 开：后台加载 small，状态变成「就绪」，写回 local；
3. 关：释放模型；
4. 关着改 medium：只记配置，不加载；改回 small。
同时输出开/关两张截图。
"""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import agentrc as core  # noqa: E402

tmpdir = Path(tempfile.mkdtemp(prefix="agentrmc_cfg_"))
cfg_path = tmpdir / "config.json"
shutil.copy(ROOT / "config.json", cfg_path)
data = json.loads(cfg_path.read_text(encoding="utf-8"))
data.setdefault("voice", {})["mode"] = "hotkey"
data["voice"]["asr_model"] = "small"
cfg_path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
core.CONFIG_PATH = cfg_path

from PySide6.QtWidgets import QApplication  # noqa: E402

import agentrc_gui as gui  # noqa: E402


def pump(app, seconds: float) -> None:
    end = time.time() + seconds
    while time.time() < end:
        app.processEvents()
        time.sleep(0.03)


def wait_state(app, win, states: tuple[str, ...], timeout: float = 25.0) -> str:
    end = time.time() + timeout
    while time.time() < end:
        app.processEvents()
        if win.ctl.model_state in states:
            break
        time.sleep(0.03)
    return win.ctl.model_state


def cfg() -> dict:
    return json.loads(cfg_path.read_text(encoding="utf-8"))


def main() -> None:
    app = QApplication(sys.argv)
    app.setStyleSheet(gui.QSS)
    win = gui.MainWindow()
    win.resize(1000, 640)
    win.show()
    pump(app, 0.8)

    print("initial", {
        "mode": win.ctl.voice_mode,
        "size": win.ctl.model_size,
        "state": win.ctl.model_state,
        "switch": win.model_switch.isChecked(),
        "combo": win.model_combo.currentData(),
        "status": win.model_status.text(),
    })
    win.grab().save(str(ROOT / "rc_probe" / "model_switch_off.png"))

    # 开
    win.model_switch._flip()
    state = wait_state(app, win, ("ready", "error"))
    pump(app, 0.4)
    print("after_on", {
        "mode": win.ctl.voice_mode,
        "size": win.ctl.model_size,
        "state": state,
        "ready": win.ctl.model_ready,
        "switch": win.model_switch.isChecked(),
        "status": win.model_status.text(),
        "info": win.info_line.text().replace("\n", " | "),
        "cfg_mode": cfg()["voice"].get("mode"),
        "cfg_model": cfg()["voice"].get("asr_model"),
    })
    win.grab().save(str(ROOT / "rc_probe" / "model_switch_on.png"))

    # 关
    win.model_switch._flip()
    pump(app, 0.5)
    print("after_off", {
        "mode": win.ctl.voice_mode,
        "state": win.ctl.model_state,
        "ready": win.ctl.model_ready,
        "model_obj": win.ctl.asr._model is not None,
        "switch": win.model_switch.isChecked(),
        "status": win.model_status.text(),
        "cfg_mode": cfg()["voice"].get("mode"),
    })

    # 关着改 medium：只写配置，不加载
    idx = win.model_combo.findData("medium")
    win.model_combo.setCurrentIndex(idx)
    pump(app, 0.4)
    print("size_medium_off", {
        "size": win.ctl.model_size,
        "state": win.ctl.model_state,
        "model_obj": win.ctl.asr._model is not None,
        "cfg_model": cfg()["voice"].get("asr_model"),
        "status": win.model_status.text(),
    })

    # 改回 small
    win.model_combo.setCurrentIndex(win.model_combo.findData("small"))
    pump(app, 0.2)

    # 运行中切模式：耳机播放键钩子要跟着装/卸
    win.ctl.running = True
    win.ctl.set_local_model(True)
    pump(app, 0.4)
    hook_local = getattr(core, "_media_thread", None) is not None
    win.ctl.set_local_model(False)
    pump(app, 0.4)
    hook_hotkey = getattr(core, "_media_thread", None) is not None
    win.ctl.running = False
    core.media_hook_stop()
    print("media_hook", {"local_has_hook": hook_local, "hotkey_has_hook": hook_hotkey})

    ok = (
        cfg()["voice"].get("mode") == "hotkey"
        and cfg()["voice"].get("asr_model") == "small"
        and win.ctl.voice_mode == "hotkey"
        and not win.ctl.model_ready
        and not hook_local
        and hook_hotkey
    )
    print("PASS" if ok else "FAIL")
    shutil.rmtree(tmpdir, ignore_errors=True)
    app.quit()


if __name__ == "__main__":
    main()
