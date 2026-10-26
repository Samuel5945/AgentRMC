import hid

VID, PID = 0x100D, 0x3838

print("=== 全部 HID 接口 ===")
targets = []
for d in hid.enumerate():
    tag = ""
    if d["vendor_id"] == VID and d["product_id"] == PID:
        tag = "  <<< 遥控器"
        targets.append(d)
    print(f"VID_{d['vendor_id']:04X} PID_{d['product_id']:04X} "
          f"UP:{d.get('usage_page',0):04X} U:{d.get('usage',0):04X} "
          f"iface={d.get('interface_number')} "
          f"| {d.get('product_string')} / {d.get('manufacturer_string')}{tag}")

print(f"\n=== 匹配到 {len(targets)} 个接口 ===")
for t in targets:
    print(f"\n--- {t['path']}")
    for k, v in t.items():
        print(f"    {k} = {v}")
