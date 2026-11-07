"""只读：dump WorkBuddy 系主窗 UIA 树（限深/限量），找会话行、composer、新建任务按钮。

用法: python rc_probe/wb_session_dump.py [WorkBuddy.exe|WorkBuddyAI.exe] [depth]
"""
import pathlib
import sys

import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen.UIAutomationClient import CUIAutomation, IUIAutomation  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from qoder_ctrl_n_focus import pids_for, top_window  # noqa: E402

exe = sys.argv[1] if len(sys.argv) > 1 else "WorkBuddy.exe"
depth = int(sys.argv[2]) if len(sys.argv) > 2 else 7

ui = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
hit = top_window(pids_for(exe))
if hit is None:
    print("no visible window for", exe)
    sys.exit(1)
hwnd, title, size = hit
print(f"window hwnd={hwnd:#x} title={title!r} size={size}")
root = ui.ElementFromHandle(hwnd)
walker = ui.ControlViewWalker

TYPES = {50000: "Button", 50004: "Edit", 50007: "ListItem", 50024: "TreeItem",
         50030: "Document", 50020: "Text", 50011: "MenuItem", 50026: "TabItem",
         50033: "Pane", 50021: "ToolBar", 50002: "CheckBox", 50003: "ComboBox",
         50005: "Hyperlink", 50008: "List", 50019: "Table", 50025: "Tree",
         50031: "Group", 50032: "Window", 50010: "Menu", 50009: "MenuBar",
         50018: "StatusBar", 50016: "ProgressBar", 50017: "RadioButton",
         50015: "ScrollBar", 50014: "Slider", 50013: "Spinner", 50012: "Image",
         50006: "Image?", 50022: "ToolTip", 50023: "InfoBar", 50027: "Separator",
         50028: "SemanticZoom", 50029: "AppBar", 50034: "Custom", 50035: "TitleBar"}

count = 0


def walk(el, d):
    global count
    if d > depth or count > 260:
        return
    try:
        ct = el.GetCurrentPropertyValue(30003)
        name = el.GetCurrentPropertyValue(30005) or ""
        aid = el.GetCurrentPropertyValue(30011) or ""
        cls = el.GetCurrentPropertyValue(30012) or ""
        r = el.GetCurrentPropertyValue(30001)
        rect = tuple(r) if isinstance(r, (tuple, list)) else (0, 0, 0, 0)
        off = el.GetCurrentPropertyValue(30022)
    except Exception:
        return
    count += 1
    label = TYPES.get(ct, str(ct))
    if label in ("Pane", "Group", "Custom", "Image", "Image?") and not name and not aid:
        pass
    else:
        print(f"{'  ' * d}{label} name={name[:44]!r} aid={aid[:24]!r} "
              f"cls={cls[:20]!r} rect={tuple(int(v) for v in rect)} off={off}")
    child = walker.GetFirstChildElement(el)
    while child and count <= 260:
        walk(child, d + 1)
        child = walker.GetNextSiblingElement(child)


walk(root, 0)
print("nodes", count)
