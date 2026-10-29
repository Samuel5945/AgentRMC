"""KeyRouter / profile extends / 语音接法的无头测试。

不碰真实键盘、不注入任何按键：
    .venv\\Scripts\\python.exe tests\\test_keyrouter.py
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import agentrc as core  # noqa: E402

DELTAS: list[int] = []
KEYS: list[tuple] = []
EVENTS: list[dict] = []


class FakeProfiles:
    def __init__(self, profile: dict):
        self.profile = profile

    def current(self, force: bool = False) -> dict:
        return self.profile


def profile() -> dict:
    return {
        "name": "test",
        "keys": {"38": "up", "40": "down", "36": "home", "13": "send", "27": "back"},
        "actions": {
            "up": {"type": "scroll", "delta": 300, "hold_repeat": 100, "hold_accel": True},
            "down": {"type": "scroll", "delta": -300},
            "home": {
                "type": "scroll", "delta": 9000, "repeat": 3, "label": "滚到顶",
                "hold_action": {"type": "scroll", "delta": -9000, "repeat": 3,
                                "label": "滚到底"},
                "hold_ms": 450, "hold_defer": True,
            },
            "send": {"type": "none", "note": "原生 Enter 发送"},
            "back": {"type": "none", "note": "原生 Esc 停止/返回"},
        },
    }


def emit(kind: str, **data) -> None:
    EVENTS.append({"kind": kind, **data})


def reset() -> None:
    DELTAS.clear()
    KEYS.clear()
    EVENTS.clear()


def setup() -> core.KeyRouter:
    reset()
    core.scroll = lambda d: DELTAS.append(d)              # type: ignore[assignment]
    core.press_combo = lambda mods, vk, rep=1: KEYS.append((tuple(mods), vk, rep))  # type: ignore[assignment]
    core.time.sleep = lambda _s: None                     # type: ignore[assignment]
    return core.KeyRouter(FakeProfiles(profile()), emit=emit)


def test_hold_scroll() -> None:
    r = setup()
    r.key_down(38, 0.0)
    assert DELTAS == [300], DELTAS
    r.tick(0.05)
    assert DELTAS == [300], "还没到 100ms 不该重复"
    r.tick(0.10)
    assert DELTAS == [300, 300], DELTAS
    r.tick(0.19)   # 加速后的第二次
    assert len(DELTAS) == 3, DELTAS
    r.key_up(38, 0.2)
    assert len(EVENTS) == 1, f"一次长按只该有一条事件: {EVENTS}"
    assert EVENTS[0]["detail"] == "长按 0.2s · 滚轮 ↑ 300 ×3", EVENTS
    r.tick(1.0)
    assert len(DELTAS) == 3, "松手后不应继续滚"
    assert len(EVENTS) == 1, "松手后不该再补事件"
    print("PASS 长按连滚：只在松手时记一条")


def test_tap_home() -> None:
    r = setup()
    r.key_down(36, 0.0)
    assert DELTAS == [], "defer 动作按下时不该立即执行"
    r.key_up(36, 0.15)
    assert DELTAS == [9000, 9000, 9000], DELTAS
    r.tick(1.0)
    assert DELTAS == [9000, 9000, 9000], "单击不该再触发长按动作"
    assert EVENTS[-1]["detail"] == "滚到顶", EVENTS
    print("PASS 主页单击 = 滚到顶")


def test_hold_home() -> None:
    r = setup()
    r.key_down(36, 0.0)
    r.tick(0.44)
    assert DELTAS == [], "450ms 前不该触发"
    r.tick(0.46)
    assert DELTAS == [-9000, -9000, -9000], DELTAS
    r.key_up(36, 0.6)
    r.tick(1.0)
    assert DELTAS == [-9000, -9000, -9000], "长按已触发，松手不该再补单击"
    assert EVENTS[-1]["detail"].startswith("长按"), EVENTS
    print("PASS 主页长按 = 滚到底")


def test_none_action() -> None:
    r = setup()
    r.key_down(13, 0.0)
    r.key_up(13, 0.05)
    r.key_down(27, 0.1)
    r.key_up(27, 0.15)
    assert KEYS == [], "none 动作不应注入任何按键"
    assert [e["detail"] for e in EVENTS] == ["原生 Enter 发送", "原生 Esc 停止/返回"], EVENTS
    print("PASS 原生透传动作不注入")


def test_repeat_keydown_ignored() -> None:
    r = setup()
    r.key_down(38, 0.0)
    r.key_down(38, 0.03)   # 设备连发
    assert DELTAS == [300], "连发 keydown 不该立刻多滚一次"
    r.key_up(38, 0.04)
    assert len(EVENTS) == 1, EVENTS
    assert "×1" not in EVENTS[0]["detail"], EVENTS
    print("PASS 设备连发 keydown 去重，只记一条")


def test_plain_hold_one_event() -> None:
    r = setup()
    r.key_down(13, 0.0)
    r.tick(0.5)
    r.key_up(13, 0.52)
    assert len(EVENTS) == 1, EVENTS
    assert EVENTS[0]["detail"] == "长按 0.5s · 原生 Enter 发送", EVENTS
    print("PASS 普通键长按也只记一条（带时长）")


def test_quick_tap_one_event() -> None:
    r = setup()
    r.key_down(38, 0.0)
    r.key_up(38, 0.06)
    assert len(EVENTS) == 1, EVENTS
    assert EVENTS[0]["detail"] == "滚轮 ↑ 300", EVENTS
    print("PASS 轻点只记一条，不带长按前缀")


def test_nav_mode() -> None:
    r = setup()
    r.profiles.profile["nav"] = {"exit_key": 36,
                                 "keys": {"38": "nav_top", "39": "nav_next"}}
    r.profiles.profile["actions"].update({
        "nav_top": {"type": "key", "vk": 33, "label": "上翻一页"},
        "nav_next": {"type": "key", "vk": 9, "label": "下一个控件"},
        "nav_exit": {"type": "nav_exit", "label": "退出导航"},
    })
    r.profiles.profile["actions"]["home"]["hold_action"] = {
        "type": "nav_toggle", "label": "导航模式"}

    r.key_down(36, 0.0)
    r.tick(0.46)
    r.key_up(36, 0.6)
    assert r.nav_mode and DELTAS == [], "长按 Home 应进导航模式，不再滚到底"
    assert EVENTS[-2]["kind"] == "status" and EVENTS[-2]["text"] == "导航模式 开", EVENTS

    r.key_down(39, 1.0)
    r.key_up(39, 1.1)
    assert KEYS == [((), 9, 1)], KEYS
    r.key_down(38, 1.2)
    r.key_up(38, 1.3)
    assert KEYS[-1] == ((), 33, 1), "导航态 ↑ 应翻页，不滚轮、不跳顶"
    assert DELTAS == [], DELTAS

    r.key_down(36, 2.0)
    r.key_up(36, 2.1)
    assert not r.nav_mode, "导航中单击 Home 应退出"
    assert DELTAS == [], "退出不该补滚到顶"

    r.key_down(36, 3.0)
    r.key_up(36, 3.1)
    assert DELTAS == [9000, 9000, 9000], "普通单击仍滚到顶"
    print("PASS 导航模式：长按开关、←→ 选择、↑↓ 翻页")


def test_profiles_extend() -> None:
    import json

    data = json.loads((ROOT / "profiles.json").read_text(encoding="utf-8"))
    profs = core.resolve_profiles(data["profiles"])
    by = {p["name"]: p for p in profs}
    q = by["qoder"]
    assert set(q["keys"]) == set(by["agent"]["keys"]), "agent 应继承 qoder 的按键"
    assert q["actions"]["send"]["type"] == "none"
    assert q["nav"]["keys"]["39"] == "nav_next"
    assert q["actions"]["nav_next"]["vk"] == 9
    assert q["actions"]["chat_top"]["hold_action"]["type"] == "nav_toggle"
    a = by["agent"]
    assert a["nav"]["keys"] == q["nav"]["keys"], "WorkBuddy 与 Qoder 按键一致"
    assert a["actions"]["chat_top"]["hold_action"]["type"] == "nav_toggle"
    assert "workbuddy.exe" in a["match"]["process"]
    assert core.voice_hotkey(q) == (["ctrl", "shift"], 86, "toggle")
    assert core.voice_hotkey(a) == (["ctrl"], 68, "toggle"), \
        "WorkBuddy 语音 = Ctrl+D 开关式"
    print("PASS qoder extends agent + 语音接法（toggle）")


def test_describe() -> None:
    a = {"type": "scroll", "delta": 300, "hold_repeat": 100}
    assert core.describe_action(a) == "滚轮 ↑ 300 · 按住连滚", core.describe_action(a)
    h = profile()["actions"]["home"]
    assert core.describe_action(h) == "滚到顶 · 长按 滚到底", core.describe_action(h)
    assert core.describe_action({"type": "none", "note": "原生 Enter 发送"}) == "原生 Enter 发送"
    print("PASS 映射页文案")


def test_tap_combo() -> None:
    seq: list[tuple[int, bool]] = []
    orig_send = core._send_key
    core._send_key = lambda vk, up=False, extra=0: seq.append((vk, up))  # type: ignore[assignment]
    core.time.sleep = lambda _s: None
    try:
        core.tap_combo(["ctrl", "shift"], 0x56)
    finally:
        core._send_key = orig_send
    expected = [(0x11, False), (0x10, False), (0x56, False),
                (0x56, True), (0x10, True), (0x11, True)]
    assert seq == expected, seq
    print("PASS tap_combo = 两次独立 keydown 的基础顺序")


def test_voice_toggle() -> None:
    """Qoder：按下发一次、松手再发一次；不是按住不放。"""
    calls = {"tap": 0, "hold": 0, "release": 0}
    orig = (core.tap_combo, core.hold_combo, core.release_combo, core.beep)
    core.tap_combo = lambda m, v: calls.__setitem__("tap", calls["tap"] + 1)  # type: ignore[assignment]
    core.hold_combo = lambda m, v: calls.__setitem__("hold", calls["hold"] + 1)  # type: ignore[assignment]
    core.release_combo = lambda m, v: calls.__setitem__("release", calls["release"] + 1)  # type: ignore[assignment]
    core.beep = lambda *a, **k: None  # type: ignore[assignment]
    try:
        app = core.App()
        app.voice_mode = "hotkey"
        prof = {"name": "qoder", "voice": {"trigger": "toggle"},
                "keys": {}, "actions": {}}
        app.profiles = FakeProfiles(prof)
        app.router.profiles = app.profiles
        app.on_voice_down(1.000)
        app.on_voice_down(1.031)       # 第二次连发 → 开录
        app.on_voice_up(1.500)         # 松手 → 停止
        assert calls == {"tap": 2, "hold": 0, "release": 0}, calls
        assert app.state == "idle"

        # 快速轻点：不发快捷键（原生 F5 已经放行）
        calls.update(tap=0, hold=0, release=0)
        app.on_voice_down(2.0)
        app.on_voice_up(2.05)
        assert calls == {"tap": 0, "hold": 0, "release": 0}, calls
    finally:
        core.tap_combo, core.hold_combo, core.release_combo, core.beep = orig
    print("PASS Qoder 开关式：按下开、松手关，轻点不动作")


def test_voice_hold() -> None:
    calls = {"tap": 0, "hold": 0, "release": 0}
    orig = (core.tap_combo, core.hold_combo, core.release_combo, core.beep)
    core.tap_combo = lambda m, v: calls.__setitem__("tap", calls["tap"] + 1)  # type: ignore[assignment]
    core.hold_combo = lambda m, v: calls.__setitem__("hold", calls["hold"] + 1)  # type: ignore[assignment]
    core.release_combo = lambda m, v: calls.__setitem__("release", calls["release"] + 1)  # type: ignore[assignment]
    core.beep = lambda *a, **k: None  # type: ignore[assignment]
    try:
        app = core.App()
        app.voice_mode = "hotkey"
        prof = {"name": "agent", "voice": {"trigger": "hold"},
                "keys": {}, "actions": {}}
        app.profiles = FakeProfiles(prof)
        app.router.profiles = app.profiles
        app.on_voice_down(1.000)
        app.on_voice_down(1.031)
        app.on_voice_up(1.500)
        assert calls == {"tap": 0, "hold": 1, "release": 1}, calls
    finally:
        core.tap_combo, core.hold_combo, core.release_combo, core.beep = orig
    print("PASS WorkBuddy 按住式：按下按住、松手放开")


if __name__ == "__main__":
    test_hold_scroll()
    test_tap_home()
    test_hold_home()
    test_none_action()
    test_repeat_keydown_ignored()
    test_plain_hold_one_event()
    test_quick_tap_one_event()
    test_nav_mode()
    test_profiles_extend()
    test_describe()
    test_tap_combo()
    test_voice_toggle()
    test_voice_hold()
    print("\n全部通过")
