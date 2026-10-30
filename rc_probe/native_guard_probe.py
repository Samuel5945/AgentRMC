import sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import agentrc as core

print("start:", core.native_guard_start())
core.native_guard_set(True, {0x25, 0x0D})
print("swallow left:", core.native_guard_should_swallow(0x25, False))
print("swallow injected:", core.native_guard_should_swallow(0x25, True))
print("swallow right:", core.native_guard_should_swallow(0x27, False))
time.sleep(0.15)
core.native_guard_stop()
print("swallow after stop:", core.native_guard_should_swallow(0x25, False))
print("OK")
