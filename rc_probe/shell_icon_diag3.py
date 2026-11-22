"""诊断：.lnk 走 GetImage 后像素到底有没有真图标（写 PNG + 统计 alpha/彩色）。"""
src = r'''
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")

int wmain(int argc, wchar_t **argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (int i = 1; i < argc; ++i) {
        IShellItem *item = nullptr;
        if (FAILED(SHCreateItemFromParsingName(argv[i], nullptr, IID_PPV_ARGS(&item))) || !item) {
            wprintf(L"open fail %ls\n", argv[i]); continue;
        }
        IShellItemImageFactory *f = nullptr;
        if (FAILED(item->QueryInterface(IID_PPV_ARGS(&f))) || !f) { item->Release(); wprintf(L"qint fail\n"); continue; }
        HBITMAP hbm = nullptr; SIZE sz = {64, 64};
        HRESULT hr = f->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm);
        if (FAILED(hr) || !hbm) { wprintf(L"getimage fail 0x%08X %ls\n", (unsigned)hr, argv[i]); f->Release(); item->Release(); continue; }
        BITMAP bmp{}; GetObjectW(hbm, sizeof(bmp), &bmp);
        // 读像素
        HDC dc = GetDC(nullptr);
        void *bits = nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bmp.bmWidth; bi.bmiHeader.biHeight = -bmp.bmHeight;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC ddc = CreateCompatibleDC(dc);
        SelectObject(ddc, dib);
        HDC mdc = CreateCompatibleDC(dc);
        SelectObject(mdc, hbm);
        BitBlt(ddc, 0, 0, bmp.bmWidth, bmp.bmHeight, mdc, 0, 0, SRCCOPY);
        unsigned char *p = (unsigned char*)bits;
        int nonblack=0, colored=0, nonzeroA=0;
        for (int k = 0; k < bmp.bmWidth*bmp.bmHeight; ++k) {
            unsigned char B=p[k*4], G=p[k*4+1], R=p[k*4+2], A=p[k*4+3];
            if (A>0) nonzeroA++;
            if (R+G+B > 60) nonblack++;
            if ((R>G+45&&R>B+45)||(G>R+45&&G>B+45)||(B>R+45&&B>G+45)) colored++;
        }
        wprintf(L"%ls size=%dx%d bpp=%d nonblack=%d colored=%d alphaNz=%d\n", argv[i], bmp.bmWidth, bmp.bmHeight, bmp.bmBitsPixel, nonblack, colored, nonzeroA);
        DeleteDC(mdc); DeleteObject(dib); DeleteObject(hbm);
        f->Release(); item->Release();
    }
    return 0;
}
'''
import pathlib, subprocess
d = pathlib.Path("tmp_diag"); d.mkdir(exist_ok=True)
(d/"diag2.cpp").write_text(src, encoding="utf-8")
bat = f'''@echo off
call "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
cl /nologo /EHsc "{d}\\diag2.cpp" /Fe:"{d}\\diag2.exe" /link shell32.lib ole32.lib gdi32.lib user32.lib
'''
(d/"b2.bat").write_text(bat)
r=subprocess.run(["cmd","/c",str(d/"b2.bat")],capture_output=True,text=True,encoding="utf-8",errors="replace")
if r.returncode!=0: print(r.stdout[-500:]); raise SystemExit(1)
for t in [r"C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Google Chrome.lnk",
          r"C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Microsoft Edge.lnk",
          r"D:\apps\Qoder\Qoder.exe",
          "shell:AppsFolder\\OpenSquilla"]:
    out=subprocess.run([str(d/"diag2.exe"),t],capture_output=True,text=True,encoding="utf-8",errors="replace")
    print(out.stdout.strip())
