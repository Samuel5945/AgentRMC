"""通过 preparsed data 读取 HID 按键能力（usage 范围），确认按键是否被映射成固定值。

用法: python rc_probe/hid_caps.py
"""
import ctypes as C

import hid

VID, PID = 0x000D, 0x3838
kernel32 = C.WinDLL("kernel32", use_last_error=True)
hid_dll = C.WinDLL("hid.dll")

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


class HIDP_BUTTON_CAPS(C.Structure):
    _fields_ = [
        ("UsagePage", C.c_ushort), ("ReportID", C.c_ubyte), ("IsAlias", C.c_ubyte),
        ("BitField", C.c_ushort), ("LinkCollection", C.c_ushort), ("LinkUsage", C.c_ushort),
        ("LinkUsagePage", C.c_ushort), ("IsRange", C.c_ubyte), ("IsStringRange", C.c_ubyte),
        ("IsDesignatorRange", C.c_ubyte), ("IsAbsolute", C.c_ubyte),
        ("Reserved", C.c_ulong * 10),
        ("Range_UsageMin", C.c_ushort), ("Range_UsageMax", C.c_ushort),
        ("Range_StringMin", C.c_ushort), ("Range_StringMax", C.c_ushort),
        ("Range_DesignatorMin", C.c_ushort), ("Range_DesignatorMax", C.c_ushort),
        ("Range_DataIndexMin", C.c_ushort), ("Range_DataIndexMax", C.c_ushort),
    ]


class HIDP_VALUE_CAPS(C.Structure):
    _fields_ = [
        ("UsagePage", C.c_ushort), ("ReportID", C.c_ubyte), ("IsAlias", C.c_ubyte),
        ("BitField", C.c_ushort), ("LinkCollection", C.c_ushort), ("LinkUsage", C.c_ushort),
        ("LinkUsagePage", C.c_ushort), ("IsRange", C.c_ubyte), ("IsStringRange", C.c_ubyte),
        ("IsDesignatorRange", C.c_ubyte), ("IsAbsolute", C.c_ubyte),
        ("HasNull", C.c_ubyte), ("Reserved", C.c_ubyte), ("BitSize", C.c_ushort),
        ("ReportCount", C.c_ushort), ("Reserved2", C.c_ushort * 5),
        ("UnitsExp", C.c_ulong), ("Units", C.c_ulong),
        ("LogicalMin", C.c_long), ("LogicalMax", C.c_long),
        ("PhysicalMin", C.c_long), ("PhysicalMax", C.c_long),
        ("Range_UsageMin", C.c_ushort), ("Range_UsageMax", C.c_ushort),
        ("Range_StringMin", C.c_ushort), ("Range_StringMax", C.c_ushort),
        ("Range_DesignatorMin", C.c_ushort), ("Range_DesignatorMax", C.c_ushort),
        ("Range_DataIndexMin", C.c_ushort), ("Range_DataIndexMax", C.c_ushort),
    ]


t = [d for d in hid.enumerate() if d["vendor_id"] == VID and d["product_id"] == PID][0]
path = t["path"].decode() if isinstance(t["path"], bytes) else t["path"]
print(f"path={path}\n")

h = kernel32.CreateFileW(path, 0, 3, None, 3, 0, None)
if not h or h == C.c_void_p(-1).value:
    raise SystemExit(f"CreateFileW err={C.get_last_error()}")

pp = C.c_void_p()
if not hid_dll.HidD_GetPreparsedData(C.c_void_p(h), C.byref(pp)):
    raise SystemExit(f"HidD_GetPreparsedData err={C.get_last_error()}")

caps = HIDP_CAPS()
status = hid_dll.HidP_GetCaps(pp, C.byref(caps))
print(f"HidP_GetCaps status=0x{status:08X}")
print(f"  UsagePage/Usage   : 0x{caps.UsagePage:04X}/0x{caps.Usage:04X}")
print(f"  InputReportBytes  : {caps.InputReportByteLength}")
print(f"  OutputReportBytes : {caps.OutputReportByteLength}")
print(f"  FeatureReportBytes: {caps.FeatureReportByteLength}")
print(f"  InputButtonCaps   : {caps.NumberInputButtonCaps}")
print(f"  InputValueCaps    : {caps.NumberInputValueCaps}")
print(f"  LinkCollections   : {caps.NumberLinkCollectionNodes}")

n = caps.NumberInputButtonCaps
if n:
    arr = (HIDP_BUTTON_CAPS * n)()
    st = hid_dll.HidP_GetButtonCaps(1, arr, C.byref(C.c_ushort(n)), pp)  # 1 = HidP_Input
    print(f"\nHidP_GetButtonCaps status=0x{st:08X}")
    for i, c in enumerate(arr):
        kind = "range" if c.IsRange else "list"
        if c.IsRange:
            print(f"  [{i}] page=0x{c.UsagePage:04X} reportID={c.ReportID} "
                  f"{kind} usage 0x{c.Range_UsageMin:04X}..0x{c.Range_UsageMax:04X} "
                  f"dataIdx {c.Range_DataIndexMin}..{c.Range_DataIndexMax}")
        else:
            print(f"  [{i}] page=0x{c.UsagePage:04X} reportID={c.ReportID} "
                  f"{kind} usage 0x{c.Range_UsageMin:04X} "
                  f"dataIdx {c.Range_DataIndexMin}")

hid_dll.HidD_FreePreparsedData(pp)
kernel32.CloseHandle(C.c_void_p(h))
