r"""映射页预设的无头测试：动作生成 + 原始 profiles 写入/extends 合并。

    .venv\Scripts\python.exe tests\test_presets.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import agentrc as core  # noqa: E402
import agentrc_gui as gui  # noqa: E402


def test_native_single() -> None:
    a = gui.preset_action("enter", 13, "single")
    assert a["type"] == "none" and "Enter" in a["note"], a
    assert a["label"] == "回车"
    b = gui.preset_action("enter", 38, "single")
    assert b["type"] == "key" and b["vk"] == 0x0D, b
    # 双击/长按即使同名键也要注入，不能当原生直通
    c = gui.preset_action("enter", 13, "double")
    assert c["type"] == "key" and c["vk"] == 0x0D, c
    print("PASS 预设动作：单击同键自动原生，双击/长按仍注入")


def test_preset_catalog() -> None:
    s = gui.preset_action("scroll_up", 38, "single")
    assert s["type"] == "scroll" and s["delta"] == 360
    m = gui.preset_action("mode_media", 27, "double")
    assert m["type"] == "mode" and m["mode"] == "media"
    c = gui.preset_action("mouse_click", 13, "single")
    assert c["type"] == "click" and c["label"] == "鼠标左键"
    assert gui.preset_action("no_such", 13, "single") is None
    print("PASS 预设目录：滚动/模式/鼠标/未知项")


def test_apply_preset_to_raw() -> None:
    data = json.loads((ROOT / "profiles.json").read_text(encoding="utf-8"))
    q = next(p for p in data["profiles"] if p["name"] == "qoder")
    assert gui.apply_mapping_preset(data, "qoder", 38, "single", "pass")
    assert q["keys"]["38"] == "k38_pass"
    assert q["actions"]["k38_pass"]["type"] == "none"
    assert q["actions"]["k38_pass"]["label"] == "原生直通"

    assert gui.apply_mapping_preset(data, "qoder", 13, "double", "mode_media")
    assert q["double"]["13"] == "k13_d_mode_media"
    assert q["actions"]["k13_d_mode_media"]["mode"] == "media"

    assert gui.apply_mapping_preset(data, "qoder", 38, "hold", "repeat_accel")
    assert q["hold"]["38"] == "repeat_accel"

    assert gui.apply_mapping_preset(data, "qoder", 27, "double", "none")
    assert q["double"]["27"] == "none"

    # extends 继承新字段
    by = {p["name"]: p for p in core.resolve_profiles(data["profiles"])}
    assert by["qoder"]["double"]["13"] == "k13_d_mode_media"
    assert by["agent"]["double"]["13"] == "k13_d_mode_media"
    assert by["agent"]["hold"]["38"] == "repeat_accel"
    assert by["qoder"]["keys"]["38"] == "k38_pass"
    print("PASS 写回原始 profiles：单击/双击/长按 + extends 合并")


def test_apply_unknown() -> None:
    data = json.loads((ROOT / "profiles.json").read_text(encoding="utf-8"))
    before = (ROOT / "profiles.json").read_text(encoding="utf-8")
    assert not gui.apply_mapping_preset(data, "no_such", 13, "single", "enter")
    assert not gui.apply_mapping_preset(data, "qoder", 13, "single", "no_such")
    after = (ROOT / "profiles.json").read_text(encoding="utf-8")
    assert after == before, "纯函数不应碰磁盘"
    print("PASS 未知场景/预设安全返回")


if __name__ == "__main__":
    test_native_single()
    test_preset_catalog()
    test_apply_preset_to_raw()
    test_apply_unknown()
    print("\n全部通过")
