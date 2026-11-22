"""导出每张卡的图标采样块拼成一张大图，便于人工核对采样位置与内容。
用法: python rc_probe/apps_icon_strip.py
"""
import pathlib
import sys
import time
import ctypes
import ctypes.wintypes as wt

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import apps_page_shot as aps  # noqa: E402
from apps_icon_probe import grab  # noqa: E402

from PIL import Image, ImageDraw  # noqa: E402

APP_NAMES = ["浏览器", "Chrome", "Edge", "Qoder", "Qoder CN",
             "WorkBuddy AI", "WorkBuddy", "DeepSeek", "Codex", "ZCode",
             "剪映", "Photoshop", "Premiere Pro", "Illustrator",
             "OpenSquilla"]

user32 = ctypes.windll.user32


def main():
    hwnd = aps.find_main_hwnd()
    user32.ShowWindow(hwnd, 9)
    user32.ShowWindow(hwnd, 5)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.6)
    uiac, uia = aps.get_uia()
    root = uia.ElementFromHandle(hwnd)
    walker = uia.RawViewWalker
    stack = [root]; tab = None
    while stack and tab is None:
        n = stack.pop()
        try: nm = n.CurrentName
        except Exception: nm = None
        if nm == "应用": tab = n
        c = walker.GetFirstChildElement(n)
        while c: stack.append(c); c = walker.GetNextSiblingElement(c)
    if tab is not None:
        b = tab.CurrentBoundingRectangle
        user32.SetCursorPos((b.left+b.right)//2, (b.top+b.bottom)//2)
        user32.mouse_event(2,0,0,0,0); user32.mouse_event(4,0,0,0,0)
        time.sleep(1.2)

    boxes = {}
    def visit(node, depth):
        if depth > 60 or node is None: return
        try: name = node.CurrentName
        except Exception: name = None
        if name in APP_NAMES and name not in boxes:
            rr = node.CurrentBoundingRectangle
            if rr.right > rr.left: boxes[name] = (rr.left, rr.top, rr.right, rr.bottom)
        child = walker.GetFirstChildElement(node)
        while child:
            visit(child, depth+1)
            child = walker.GetNextSiblingElement(child)
    visit(root, 0)

    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    win = (r.left, r.top)
    img = grab(hwnd).convert("RGB")

    cols = 5
    cell = 60
    sheet = Image.new("RGB", (cols*cell, ((len(APP_NAMES)+cols-1)//cols)*cell), (30,30,30))
    d = ImageDraw.Draw(sheet)
    for i, nm in enumerate(APP_NAMES):
        if nm not in boxes: continue
        l,t,rr,b = boxes[nm]
        cx = (l+rr)//2 - win[0]
        top = t - win[1]
        tile = img.crop((cx-28, top-118, cx+28, top-58))
        x0 = (i%cols)*cell; y0 = (i//cols)*cell
        sheet.paste(tile.resize((56,56)), (x0+2, y0+2))
        d.text((x0+4, y0+4), nm[:6], fill=(255,255,0))
    sheet.save("rc_probe/icon_sheet.png")
    print("saved rc_probe/icon_sheet.png", sheet.size)


if __name__ == "__main__":
    main()
