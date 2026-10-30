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
    core.tap_key = lambda vk: KEYS.append(((), vk, 1))    # type: ignore[assignment]
    core.beep = lambda *a, **k: None                       # type: ignore[assignment]
    core.cursor_hook_start = lambda: True                 # type: ignore[assignment]
    core.cursor_hook_stop = lambda: None                  # type: ignore[assignment]
    core.time.sleep = lambda _s: None                     # type: ignore[assignment]
    return core.KeyRouter(FakeProfiles(profile()), emit=emit)


def takeover_router() -> core.KeyRouter:
    """qoder 左键：直通单键 + 双击退格 + 长按 PageUp，原生已由钩子吞掉。"""
    r = setup()
    p = r.profiles.profile
    p["keys"]["37"] = "left"
    p["actions"]["left"] = {"type": "none", "note": "原生 ←"}
    p["actions"]["backspace"] = {"type": "key", "vk": 8, "label": "退格"}
    p["double"] = {"37": "backspace"}
    r.takeover_vks = {37}
    return r


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
    assert DELTAS == [], "双击窗口内先不动作"
    r.tick(0.5)
    assert DELTAS == [9000, 9000, 9000], DELTAS
    r.tick(1.0)
    assert DELTAS == [9000, 9000, 9000], "单击不该再触发长按动作"
    assert EVENTS[-1]["detail"] == "滚到顶", EVENTS
    print("PASS 主页单击 = 滚到顶（延迟判定双击）")


def test_none_action() -> None:
    r = setup()
    r.key_down(13, 0.0)
    r.key_up(13, 0.05)
    r.key_down(27, 0.1)
    r.key_up(27, 0.15)
    r.tick(0.6)   # 返回是双击候选项，单独单击延迟执行
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

    # 双击 Home 进导航模式
    r.key_down(36, 0.0)
    r.key_up(36, 0.05)
    r.tick(0.1)
    r.key_down(36, 0.2)
    r.key_up(36, 0.25)
    assert r.nav_mode, "双击 Home 应进导航模式"
    assert DELTAS == [], "双击不该先滚到顶"
    assert any(e.get("text", "") == "导航模式 开" for e in EVENTS), EVENTS

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

    # 普通单击：延迟判定后仍滚到顶
    r.key_down(36, 3.0)
    r.key_up(36, 3.05)
    assert DELTAS == [], "双击窗口内先不动作"
    r.tick(3.4)
    assert DELTAS == [9000, 9000, 9000], "普通单击仍滚到顶"
    print("PASS 导航模式：双击开关、←→ 选择、↑↓ 翻页")


def test_mode_media_and_cursor() -> None:
    r = setup()
    r.profiles.profile["keys"]["93"] = "menu"
    r.profiles.profile["actions"]["menu"] = {"type": "none", "note": "原生菜单"}
    bright: list[int] = []
    moves: list[tuple[int, int]] = []
    core.brightness = lambda d: bright.append(d) or True   # type: ignore[assignment]
    core.mouse_move = lambda dx, dy: moves.append((dx, dy))  # type: ignore[assignment]

    # 双击返回 → 音量/亮度模式
    r.key_down(27, 0.0)
    r.key_up(27, 0.05)
    r.key_down(27, 0.2)
    r.key_up(27, 0.25)
    assert r.mode == "media", r.mode
    assert any(e.get("text", "").startswith("音量/亮度模式 开") for e in EVENTS), EVENTS

    r.key_down(38, 1.0)
    r.key_up(38, 1.05)
    r.key_down(40, 1.1)
    r.key_up(40, 1.15)
    assert KEYS[-2:] == [((), 0xAF, 1), ((), 0xAE, 1)], KEYS
    r.key_down(37, 1.2)
    r.key_up(37, 1.25)
    r.key_down(39, 1.3)
    r.key_up(39, 1.35)
    assert bright == [-10, 10], bright
    assert DELTAS == [], "模式里 ↑↓ 不应再滚轮"

    # 单击返回退出，且不再执行返回键本身
    n_events = len(EVENTS)
    r.key_down(27, 2.0)
    r.key_up(27, 2.1)
    assert r.mode is None
    assert len(EVENTS) == n_events + 1, "退出只记一条状态"

    # 单击菜单：0.35s 后执行原动作
    r.key_down(93, 3.0)
    r.key_up(93, 3.05)
    r.tick(3.4)
    assert EVENTS[-1]["detail"] == "原生菜单", EVENTS

    # 双击菜单 → 鼠标模式
    r.key_down(93, 4.0)
    r.key_up(93, 4.05)
    r.key_down(93, 4.2)
    r.key_up(93, 4.25)
    assert r.mode == "cursor", r.mode
    r.key_down(39, 5.0)
    r.key_up(39, 5.05)
    assert moves == [], "鼠标模式下原生方向键交给钩子，Raw 不再移动"

    clicks: list[int] = []
    core.mouse_click = lambda: clicks.append(1)   # type: ignore[assignment]
    r.key_down(13, 5.1)
    r.key_up(13, 5.15)
    assert clicks == [], "OK 也交给钩子，Raw 不再点击"

    # 钩子直接驱动鼠标并吞掉事件
    import ctypes

    kb = core._KBDLLHOOKSTRUCT(vkCode=39, scanCode=0, flags=0, time=0,
                               dwExtraInfo=0)
    ret = core._cursor_hook_cb(0, core.WM_KEYDOWN, ctypes.addressof(kb))
    assert ret == 1 and moves[-1] == (core.CURSOR_STEP, 0), (ret, moves)
    kb = core._KBDLLHOOKSTRUCT(vkCode=13, scanCode=0, flags=0, time=0,
                               dwExtraInfo=0)
    ret = core._cursor_hook_cb(0, core.WM_KEYDOWN, ctypes.addressof(kb))
    assert ret == 1 and clicks == [1], (ret, clicks)
    kb = core._KBDLLHOOKSTRUCT(vkCode=39, scanCode=0, flags=0x10, time=0,
                               dwExtraInfo=0)
    core._cursor_hook_cb(0, core.WM_KEYDOWN, ctypes.addressof(kb))
    assert moves[-1] == (core.CURSOR_STEP, 0), "注入事件不该被钩子处理"
    # 空闲超时自动退出（钩子活动时间清掉，走 tick 时钟）
    core._cursor_last = 0.0
    r.tick(6.0)
    r.tick(6.0 + core.MODE_IDLE_S + 1)
    assert r.mode is None
    print("PASS 长按返回=音量/亮度、双击菜单=鼠标，单击/超时退出")


def test_mode_enter_repeat_keeps_mode() -> None:
    """双击进模式那一击按得稍久，设备连发的 keydown 不应立刻退出模式
    （否则弹窗「开→关」闪一下就没了）。"""
    r = setup()
    r.profiles.profile["keys"]["93"] = "menu"
    r.profiles.profile["actions"]["menu"] = {"type": "none", "note": "原生菜单"}

    # 双击返回 → 音量/亮度模式；第二击按住期间连发两次
    r.key_down(27, 0.0)
    r.key_up(27, 0.05)
    r.key_down(27, 0.2)
    assert r.mode == "media", r.mode
    r.key_down(27, 0.4)            # 设备连发
    r.key_down(27, 0.6)            # 设备连发
    assert r.mode == "media", "进模式那一击的连发不应退出模式"
    assert sum("音量/亮度模式 开" in e.get("text", "") for e in EVENTS) == 1
    assert not any("音量/亮度模式 关" in e.get("text", "") for e in EVENTS), EVENTS
    r.key_up(27, 0.7)              # 松手后恢复退出键语义
    r.key_down(27, 1.5)
    assert r.mode is None, "松手后的单击应正常退出模式"

    # 双击主页 → 导航模式，同样吞掉连发
    r.key_down(36, 2.0)
    r.key_up(36, 2.05)
    r.key_down(36, 2.2)
    r.key_up(36, 2.25)
    assert r.nav_mode, r.nav_mode
    print("PASS 双击进模式：连发 keydown 不再导致立刻退出")


def test_generic_double() -> None:
    r = setup()
    p = r.profiles.profile
    p["double"] = {"13": "dbl_esc"}
    p["actions"]["dbl_esc"] = {"type": "key", "vk": 27, "label": "双击 Esc"}

    # 第一次按下：等双击窗口，不执行单击
    r.key_down(13, 0.0)
    r.key_up(13, 0.05)
    assert KEYS == [], "双击等待期间不该先执行 Enter"
    # 窗口内第二次按下：执行双击动作，取消单击
    r.key_down(13, 0.2)
    r.key_up(13, 0.25)
    assert KEYS == [((), 27, 1)], KEYS
    assert EVENTS[-1]["detail"].startswith("双击 · "), EVENTS
    r.tick(1.0)
    assert len(EVENTS) == 1, f"双击后不该再补单击: {EVENTS}"
    print("PASS 通用双击：第二击执行预设并取消单击")


def test_double_none_disables_builtin() -> None:
    r = setup()
    r.profiles.profile["double"] = {"27": "none"}

    # 不再等双击窗口，单击立即执行；连按两次也不进音量模式
    r.key_down(27, 0.0)
    r.key_up(27, 0.05)
    assert EVENTS[-1]["detail"] == "原生 Esc 停止/返回", EVENTS
    r.key_down(27, 0.2)
    r.key_up(27, 0.25)
    assert r.mode is None
    assert not any("音量/亮度模式" in e.get("text", "") for e in EVENTS), EVENTS
    print("PASS 双击设为 none：关掉内置模式，单击不再延迟")


def test_hold_spec_action() -> None:
    r = setup()
    p = r.profiles.profile
    p["hold"] = {"13": "hold_mute"}
    p["actions"]["hold_mute"] = {"type": "key", "vk": 0xAD, "label": "静音"}

    # 短按不触发长按动作
    r.key_down(13, 0.0)
    r.key_up(13, 0.05)
    r.tick(1.0)
    assert KEYS == [], KEYS
    # 按住 0.6s 触发静音
    r.key_down(13, 2.0)
    r.tick(2.7)
    assert KEYS == [((), 0xAD, 1)], KEYS
    r.key_up(13, 2.8)
    assert EVENTS[-1]["detail"].startswith("长按 · 静音"), EVENTS
    print("PASS 长按动作预设：按住 0.6s 执行，短按不触发")


def test_hold_repeat_spec() -> None:
    r = setup()
    p = r.profiles.profile
    p["hold"] = {"13": "repeat"}
    p["keys"]["13"] = "send_key"
    p["actions"]["send_key"] = {"type": "key", "vk": 0x41, "label": "A"}

    r.key_down(13, 0.0)
    assert KEYS == [((), 0x41, 1)], KEYS
    r.tick(0.15)
    assert len(KEYS) == 2, f"按住应重复触发: {KEYS}"
    r.key_up(13, 0.2)
    print("PASS 长按重复预设：按住重复当前动作")


def test_hold_none_overrides_legacy() -> None:
    r = setup()
    r.profiles.profile["hold"] = {"38": "none"}
    r.key_down(38, 0.0)
    r.tick(0.5)
    r.key_up(38, 0.6)
    assert DELTAS == [300], f"none 应覆盖动作自带的 hold_repeat: {DELTAS}"
    print("PASS 长按 none：覆盖旧的 hold_repeat")


def test_double_with_hold() -> None:
    r = setup()
    p = r.profiles.profile
    p["double"] = {"13": "dbl_esc"}
    p["hold"] = {"13": "hold_mute"}
    p["actions"]["dbl_esc"] = {"type": "key", "vk": 27, "label": "双击 Esc"}
    p["actions"]["hold_mute"] = {"type": "key", "vk": 0xAD, "label": "静音"}

    # 单击窗口过后：先执行单击，再进入长按计时；双击不触发
    r.key_down(13, 0.0)
    r.tick(0.4)
    assert not any(k[1] == 27 for k in KEYS), "还没到双击，不该执行 Esc"
    r.tick(0.7)
    assert KEYS == [((), 0xAD, 1)], KEYS
    r.key_up(13, 0.8)
    print("PASS 双击键也能长按：单击窗口后接长按动作")


def test_extend_double_hold() -> None:
    raw = [
        {"name": "base", "keys": {"13": "a"},
         "actions": {"a": {"type": "key", "vk": 65},
                     "d": {"type": "key", "vk": 66},
                     "h": {"type": "key", "vk": 67}},
         "double": {"13": "d"}, "hold": {"13": "h"}},
        {"name": "child", "extends": "base",
         "double": {"27": "none"}, "hold": {"27": "repeat"}},
    ]
    by = {p["name"]: p for p in core.resolve_profiles(raw)}
    child = by["child"]
    assert child["double"] == {"13": "d", "27": "none"}, child["double"]
    assert child["hold"] == {"13": "h", "27": "repeat"}, child["hold"]
    print("PASS extends 合并 double/hold")


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
    assert "hold_action" not in q["actions"]["chat_top"], "Home 不再用长按"
    a = by["agent"]
    assert a["nav"]["keys"] == q["nav"]["keys"], "WorkBuddy 与 Qoder 按键一致"
    assert "hold_action" not in a["actions"]["chat_top"]
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
    assert core.describe_action({"type": "click"}) == "鼠标左键"
    assert core.describe_action({"type": "key", "vk": 0x7A}) == "F11"
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


def test_takeover_pass_single() -> None:
    r = takeover_router()
    r.key_down(37, 0.0)
    assert KEYS == [], "双击窗口还没判定，不能先补 ←"
    r.key_up(37, 0.05)
    r.tick(0.31)
    assert KEYS == [((), 37, 1)], f"单击落地后应补一次 ←: {KEYS}"
    print("PASS 原生接管：直通单击在双击窗口后补发源键")


def test_takeover_pass_double() -> None:
    r = takeover_router()
    r.key_down(37, 0.0)
    r.key_up(37, 0.05)
    r.key_down(37, 0.20)
    assert KEYS == [((), 8, 1)], f"双击只该发退格: {KEYS}"
    assert not any(vk == 37 for _, vk, _ in KEYS), "双击不该再补 ←"
    print("PASS 原生接管：双击只发退格，不再补源键")


def test_takeover_repeat() -> None:
    r = takeover_router()
    r.key_down(37, 0.0)
    r.tick(0.31)             # 单击落地，键仍按着
    r.key_down(37, 0.35)     # 遥控器设备连发
    assert KEYS == [((), 37, 1), ((), 37, 1)], f"连发应逐次补源键: {KEYS}"
    r.key_up(37, 0.4)
    print("PASS 原生接管：按住连发用源键 tap 还原")


def test_native_guard_predicate() -> None:
    core.native_guard_set(True, {37})
    assert core.native_guard_should_swallow(37, False)
    assert not core.native_guard_should_swallow(37, True), "自己注入的键不能被吞"
    assert not core.native_guard_should_swallow(38, False)
    core.native_guard_set(False, {37})
    assert not core.native_guard_should_swallow(37, False)
    core.native_guard_set(True, ())
    assert not core.native_guard_should_swallow(37, False)
    # 鼠标模式：cursor hook 在时，方向键/Enter 必须放行
    core._cursor_hook = 1
    core.native_guard_set(True, {37, 0x0D})
    assert not core.native_guard_should_swallow(37, False)
    assert not core.native_guard_should_swallow(0x0D, False)
    core._cursor_hook = None
    core.native_guard_set(False, ())
    print("PASS 接管判定：只吞启用/非注入/映射集合内的键，鼠标模式放行方向键")


def test_native_guard_feed() -> None:
    import ctypes
    core.key_events.clear()
    core.native_guard_set(True, {37})
    kb = core._KBDLLHOOKSTRUCT()
    kb.vkCode = 37
    kb.scanCode = 75
    kb.flags = 0
    lp = ctypes.addressof(kb)
    rc = core._native_guard_cb(0, core.WM_KEYDOWN, lp)
    assert rc == 1, rc
    ev = core.key_events[-1]
    assert ev["vkey"] == 37 and ev["down"] and ev.get("native"), ev
    rc = core._native_guard_cb(0, core.WM_KEYUP, lp)
    assert rc == 1 and not core.key_events[-1]["down"]
    before = len(core.key_events)
    kb.flags = core.LLKHF_INJECTED
    rc = core._native_guard_cb(0, core.WM_KEYDOWN, lp)
    assert rc != 1, "注入键不能吞"
    assert len(core.key_events) == before
    assert (37, True) in core._native_fed_at
    core.native_guard_set(False, ())
    core.key_events.clear()
    print("PASS 原生接管：钩子吞键后主动喂给 Router，注入键不喂")


def test_guard_cursor_handles() -> None:
    import ctypes
    moves: list[tuple] = []
    clicks: list[int] = []
    orig = (core.mouse_move, core.mouse_click)
    core.mouse_move = lambda dx, dy: moves.append((dx, dy))  # type: ignore[assignment]
    core.mouse_click = lambda: clicks.append(1)              # type: ignore[assignment]
    try:
        core._cursor_hook = 1
        core.native_guard_set(True, {37, 0x0D})
        kb = core._KBDLLHOOKSTRUCT()
        kb.vkCode = 37
        kb.flags = 0
        rc = core._native_guard_cb(0, core.WM_KEYDOWN, ctypes.addressof(kb))
        assert rc == 1 and moves[-1] == (-core.CURSOR_STEP, 0), (rc, moves)
        kb.vkCode = 0x0D
        rc = core._native_guard_cb(0, core.WM_KEYDOWN, ctypes.addressof(kb))
        assert rc == 1 and clicks, (rc, clicks)
    finally:
        core._cursor_hook = None
        core.native_guard_set(False, ())
        core.mouse_move, core.mouse_click = orig
    print("PASS 接管钩子也能在鼠标模式驱动鼠标（不依赖钩子链序）")


def test_cursor_accel() -> None:
    moves: list[tuple] = []
    orig = core.mouse_move
    core.mouse_move = lambda dx, dy: moves.append((dx, dy))  # type: ignore[assignment]
    try:
        core._cursor_hold_vk = 0
        core._cursor_hold_count = 0
        core._cursor_hold_t = 0.0
        for _ in range(5):
            core._cursor_handle(37, True)
        assert moves == [(-8, 0), (-8, 0), (-10, 0), (-10, 0), (-12, 0)], moves
        core._cursor_handle(37, False)
        core._cursor_handle(37, True)
        assert moves[-1] == (-core.CURSOR_STEP, 0), "松手再按要回到 8px"
    finally:
        core.mouse_move = orig
        core._cursor_hold_vk = 0
        core._cursor_hold_count = 0
        core._cursor_hold_t = 0.0
    print("PASS 鼠标模式：短按 8px，长按逐级加速")


if __name__ == "__main__":
    test_hold_scroll()
    test_tap_home()
    test_none_action()
    test_repeat_keydown_ignored()
    test_plain_hold_one_event()
    test_quick_tap_one_event()
    test_nav_mode()
    test_mode_media_and_cursor()
    test_mode_enter_repeat_keeps_mode()
    test_generic_double()
    test_double_none_disables_builtin()
    test_hold_spec_action()
    test_hold_repeat_spec()
    test_hold_none_overrides_legacy()
    test_double_with_hold()
    test_extend_double_hold()
    test_profiles_extend()
    test_describe()
    test_tap_combo()
    test_voice_toggle()
    test_voice_hold()
    test_takeover_pass_single()
    test_takeover_pass_double()
    test_takeover_repeat()
    test_native_guard_predicate()
    test_native_guard_feed()
    test_guard_cursor_handles()
    test_cursor_accel()
    print("\n全部通过")
