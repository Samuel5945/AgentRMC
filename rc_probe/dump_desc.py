"""dump 遥控器 HID 报告描述符 + HIDP_CAPS，判断按键报告的真实布局。

用法: python rc_probe/dump_desc.py
"""
import ctypes as C
from pathlib import Path

import hid

VID, PID = 0x000D, 0x3838
OUT = Path(__file__).resolve().parent

hid_dll = C.WinDLL("hid.dll")
kernel32 = C.WinDLL("kernel32.dll", use_last_error=True)

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3

kernel32.CreateFileW.restype = C.c_void_p
kernel32.CreateFileW.argtypes = [C.c_wchar_p, C.c_uint32, C.c_uint32, C.c_void_p,
                                 C.c_uint32, C.c_uint32, C.c_void_p]
kernel32.CloseHandle.argtypes = [C.c_void_p]


class HIDP_CAPS(C.Structure):
    _fields_ = [
        ("Usage", C.c_ushort), ("UsagePage", C.c_ushort),
        ("InputReportByteLength", C.c_ushort), ("OutputReportByteLength", C.c_ushort),
        ("FeatureReportByteLength", C.c_ushort),
        ("Reserved", C.c_ushort * 17),
        ("NumberLinkCollectionNodes", C.c_ushort),
        ("NumberInputButtonCaps", C.c_ushort), ("NumberInputValueCaps", C.c_ushort),
        ("NumberInputDataIndices", C.c_ushort),
        ("NumberOutputButtonCaps", C.c_ushort), ("NumberOutputValueCaps", C.c_ushort),
        ("NumberOutputDataIndices", C.c_ushort),
        ("NumberFeatureButtonCaps", C.c_ushort), ("NumberFeatureValueCaps", C.c_ushort),
        ("NumberFeatureDataIndices", C.c_ushort),
    ]


def dump_descriptor_bytes(data: bytes) -> None:
    """把报告描述符解析成人话（只覆盖常见 item）。"""
    i = 0
    depth = 0
    while i < len(data):
        b = data[i]
        size_code = b & 0x03
        nbytes = 4 if size_code == 3 else size_code
        item_type = (b >> 2) & 0x03
        tag = (b >> 4) & 0x0F
        val = int.from_bytes(data[i + 1:i + 1 + nbytes], "little") if nbytes else 0
        type_name = {0: "Main", 1: "Global", 2: "Local"}[item_type]
        name = ""
        if type_name == "Main":
            name = {8: "Input", 9: "Output", 10: "Collection", 11: "Feature",
                    12: "EndCollection"}.get(tag, f"Main{tag}")
            if tag == 10:
                depth += 1
            elif tag == 12:
                depth -= 1
        elif type_name == "Global":
            name = {0: "UsagePage", 1: "LogicalMin", 2: "LogicalMax", 3: "PhysicalMin",
                    4: "PhysicalMax", 5: "UnitExponent", 6: "Unit", 7: "ReportSize",
                    8: "ReportID", 9: "ReportCount", 10: "Push", 11: "Pop"}.get(tag, f"G{tag}")
        else:
            name = {0: "Usage", 1: "UsageMin", 2: "UsageMax", 3: "DesignatorIndex",
                    4: "DesignatorMin", 5: "DesignatorMax", 7: "StringIndex",
                    8: "StringMin", 9: "StringMax", 10: "Delimiter"}.get(tag, f"L{tag}")
        pad = "  " * max(depth - 1, 0)
        extra = ""
        if type_name == "Main" and tag == 8:
            flags = []
            if val & 1: flags.append("Constant")
            if val & 2: flags.append("Variable")
            if val & 4: flags.append("Relative")
            if val & 8: flags.append("Wrap")
            if val & 0x20: flags.append("NoPreferred")
            if val & 0x80: flags.append("Volatile")
            if val & 0x100: flags.append("BufferedBytes")
            extra = "  " + "|".join(flags)
        print(f"{pad}{name:<14} {val if nbytes else ''} (0x{val:0{nbytes*2}X}){extra}")
        i += 1 + nbytes


targets = [d for d in hid.enumerate() if d["vendor_id"] == VID and d["product_id"] == PID]
print(f"匹配 {len(targets)} 个 HID 集合\n")

for t in targets:
    path = t["path"]
    if isinstance(path, bytes):
        path = path.decode()
    print("=" * 70)
    print(f"path = {path}")
    print(f"usage_page=0x{t.get('usage_page', 0):04X} usage=0x{t.get('usage', 0):04X} "
          f"release={t.get('release_number')}")

    h = kernel32.CreateFileW(path, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                             OPEN_EXISTING, 0, None)
    if not h or h == C.c_void_p(-1).value:
        print(f"[!] CreateFileW 失败 err={C.get_last_error()}")
        continue

    # 报告描述符
    buf = C.create_string_buffer(4096)
    if hid_dll.HidD_GetReportDescriptor(C.c_void_p(h), buf, 4096):
        raw = buf.raw
        # 描述符到 EndCollection 结束，取到最后一个非零
        end = len(raw.rstrip(b"\x00"))
        desc = raw[:end]
        (OUT / "descriptor.bin").write_bytes(desc)
        print(f"\n--- 报告描述符 {len(desc)} 字节 (已存 descriptor.bin) ---")
        dump_descriptor_bytes(desc)
    else:
        print(f"[!] HidD_GetReportDescriptor 失败 err={C.get_last_error()}")

    # caps
    pp = C.c_void_p()
    if hid_dll.HidD_GetPreparsedData(C.c_void_p(h), C.byref(pp)):
        caps = HIDP_CAPS()
        if hid_dll.HidP_GetCaps(pp, C.byref(caps)) == 0x110000:  # HIDP_STATUS_SUCCESS
            print(f"\n--- CAPS ---")
            print(f"  UsagePage/Usage   : 0x{caps.UsagePage:04X}/0x{caps.Usage:04X}")
            print(f"  InputReportBytes  : {caps.InputReportByteLength}")
            print(f"  OutputReportBytes : {caps.OutputReportByteLength}")
            print(f"  FeatureReportBytes: {caps.FeatureReportByteLength}")
            print(f"  InputButtonCaps   : {caps.NumberInputButtonCaps}")
            print(f"  InputValueCaps    : {caps.NumberInputValueCaps}")
        else:
            print("[!] HidP_GetCaps 失败")
        hid_dll.HidD_FreePreparsedData(pp)
    kernel32.CloseHandle(C.c_void_p(h))
    print()
