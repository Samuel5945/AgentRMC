import ctypes, sys, time
u = ctypes.windll.user32
x, y = int(sys.argv[1]), int(sys.argv[2])
u.SetCursorPos(x, y)
time.sleep(0.15)
u.mouse_event(0x0002, 0, 0, 0, 0)   # left down
time.sleep(0.05)
u.mouse_event(0x0004, 0, 0, 0, 0)   # left up
print(f"clicked {x},{y}")
