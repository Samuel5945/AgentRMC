"""离线复现 ShellIcon：.lnk GetImage → QImage → PNG，检查像素是否完好；
同时复现 Qt QFileIconProvider 路径对比。"""
src = r'''
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// 手工 PNG 太麻烦：直接 GdipSaveImageToFile（gdiplus）
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

int wmain(int argc, wchar_t **argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG tok; Gdiplus::GdiplusStartup(&tok, nullptr, nullptr);
    for (int i = 1; i < argc; ++i) {
        IShellItem *item = nullptr;
        if (FAILED(SHCreateItemFromParsingName(argv[i], nullptr, IID_PPV_ARGS(&item))) || !item) {
            wprintf(L"open fail %ls\n", argv[i]); continue;
        }
        IShellItemImageFactory *f = nullptr;
        item->QueryInterface(IID_PPV_ARGS(&f));
        HBITMAP hbm = nullptr; SIZE sz = {64, 64};
        HRESULT hr = f->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm);
        wprintf(L"%ls -> 0x%08X hbm=%p\n", argv[i], (unsigned)hr, (void*)hbm);
        if (SUCCEEDED(hr) && hbm) {
            Gdiplus::Bitmap bmp(hbm, nullptr);
            // 存 PNG（带 alpha 通道的原图）
            Gdiplus::CLSID pngClsid;
            GetImageEncClsid(pngClsid);
            UINT cnt = 0; Gdiplus::GetImageEncodersSize(&cnt, nullptr);
            // 简单：用 Bitmap::Save 直接存 BMP
            wchar_t out[512];
            wsprintfW(out, L"rc_probe\\shell_probe_%d.png", i);
            int st = bmp.Save(out, nullptr);
            wprintf(L"  save=%d -> %ls\n", st, out);
            DeleteObject(hbm);
        }
        f->Release(); item->Release();
    }
    return 0;
}
'''
import pathlib, subprocess
d = pathlib.Path("tmp_diag"); d.mkdir(exist_ok=True)
# 修正：用 GetImageEncoders 拿 PNG clsid 太啰嗦，直接 BMP
src2 = src.replace(
    '''            Gdiplus::CLSID pngClsid;
            GetImageEncClsid(pngClsid);
            UINT cnt = 0; Gdiplus::GetImageEncodersSize(&cnt, nullptr);
            // 简单：用 Bitmap::Save 直接存 BMP
            wchar_t out[512];
            wsprintfW(out, L"rc_probe\\shell_probe_%d.png", i);
            int st = bmp.Save(out, nullptr);''',
    '''            wchar_t out[512];
            wsprintfW(out, L"rc_probe\\shell_probe_%d.bmp", i);
            int st = bmp.Save(out, &ImageFormatBMP);''')
src2 = src2.replace("#include <gdiplus.h>", "#include <gdiplus.h>\nusing namespace Gdiplus;")
(d/"diag4.cpp").write_text(src2, encoding="utf-8")
bat = f'''@echo off
call "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
cl /nologo /EHsc "{d}\\diag4.cpp" /Fe:"{d}\\diag4.exe" /link shell32.lib ole32.lib gdi32.lib user32.lib gdiplus.lib
'''
(d/"b4.bat").write_text(bat)
r=subprocess.run(["cmd","/c",str(d/"b4.bat")],capture_output=True,text=True,encoding="utf-8",errors="replace")
if r.returncode!=0:
    print(r.stdout[-600:]); print(r.stderr[-300:]); raise SystemExit(1)
out=subprocess.run([str(d/"diag4.exe"),
    r"C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Google Chrome.lnk",
    r"C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Microsoft Edge.lnk",
    r"D:\apps\Qoder\Qoder.exe"], capture_output=True, text=True, encoding="utf-8", errors="replace")
print(out.stdout)
# 读 BMP 统计
from PIL import Image
for i in (1,2,3):
    p = pathlib.Path(f"rc_probe/shell_probe_{i}.bmp")
    if p.exists():
        im=Image.open(p).convert("RGB")
        px=list(im.getdata())
        colored=sum(1 for (r_,g_,b_) in px if max(r_,g_,b_)>90 and (max(r_,g_,b_)-min(r_,g_,b_))>45)
        nonblack=sum(1 for (r_,g_,b_) in px if r_+g_+b_>60)
        print(f"{p.name} {im.size} colored={colored} nonblack={nonblack}")
    else:
        print(p, "missing")
