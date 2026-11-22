"""离线复现 ShellIcon：GetImage 返回的 HBITMAP 按 ShellIcon.h 的方式
转成 44x44 RGBA（含 alpha=0 兜底），存 PNG 看像素到底什么样。"""
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

static void dumpBmp(HBITMAP hbm, const wchar_t *outPath) {
    BITMAP bmp{};
    if (!GetObjectW(hbm, sizeof(bmp), &bmp)) { wprintf(L"GetObject fail\n"); return; }
    wprintf(L"  bm %dx%d bpp=%d\n", bmp.bmWidth, bmp.bmHeight, bmp.bmBitsPixel);
    HDC dc = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = bmp.bmWidth;
    bi.bmiHeader.biHeight = -bmp.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC ddc = CreateCompatibleDC(dc);
    SelectObject(ddc, dib);
    HDC mdc = CreateCompatibleDC(dc);
    SelectObject(mdc, hbm);
    BitBlt(ddc, 0, 0, bmp.bmWidth, bmp.bmHeight, mdc, 0, 0, SRCCOPY);
    unsigned char *p = (unsigned char*)bits;
    int nonblack=0, colored=0, aNz=0;
    for (int k = 0; k < bmp.bmWidth*bmp.bmHeight; ++k) {
        unsigned char B=p[k*4], G=p[k*4+1], R=p[k*4+2], A=p[k*4+3];
        if (R+G+B > 60) nonblack++;
        if (A>0) aNz++;
        int mx = R>G?(R>B?R:B):(G>B?G:B);
        int mn = R<G?(R<B?R:B):(G<B?G:B);
        if (mx > 90 && mx - mn > 45) colored++;
    }
    wprintf(L"  nonblack=%d colored=%d alphaNz=%d\n", nonblack, colored, aNz);
    // 存 BMP（负高度=自上而下）
    BITMAPFILEHEADER bf{};
    DWORD imgSize = (DWORD)(bmp.bmWidth * bmp.bmHeight * 4);
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bf.bfSize = bf.bfOffBits + imgSize;
    HANDLE f = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(f, &bf, sizeof(bf), &w, nullptr);
        WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &w, nullptr);
        WriteFile(f, bits, imgSize, &w, nullptr);
        CloseHandle(f);
        wprintf(L"  saved %ls\n", outPath);
    }
    DeleteDC(mdc); DeleteDC(ddc); DeleteObject(dib);
}

int wmain(int argc, wchar_t **argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int idx = 0;
    for (int i = 1; i < argc; ++i) {
        IShellItem *item = nullptr;
        if (FAILED(SHCreateItemFromParsingName(argv[i], nullptr, IID_PPV_ARGS(&item))) || !item) {
            wprintf(L"%ls open fail\n", argv[i]); continue;
        }
        IShellItemImageFactory *f = nullptr;
        item->QueryInterface(IID_PPV_ARGS(&f));
        HBITMAP hbm = nullptr; SIZE sz = {64, 64};
        HRESULT hr = f->GetImage(sz, SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &hbm);
        wprintf(L"GetImage 0x%08X  %ls\n", (unsigned)hr, argv[i]);
        if (SUCCEEDED(hr) && hbm) {
            wchar_t out[260];
            wsprintfW(out, L"tmp_diag\\probe_%d.bmp", idx++);
            dumpBmp(hbm, out);
            DeleteObject(hbm);
        }
        f->Release(); item->Release();
    }
    return 0;
}
'''
import pathlib, subprocess
d = pathlib.Path("tmp_diag"); d.mkdir(exist_ok=True)
(d/"diag5.cpp").write_text(src, encoding="utf-8")
bat = f'''@echo off
call "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
cl /nologo /EHsc "{d}\\diag5.cpp" /Fe:"{d}\\diag5.exe" /link shell32.lib ole32.lib gdi32.lib user32.lib
'''
(d/"b5.bat").write_text(bat)
r=subprocess.run(["cmd","/c",str(d/"b5.bat")],capture_output=True,text=True,encoding="utf-8",errors="replace")
if r.returncode!=0:
    print(r.stdout[-700:]); print(r.stderr[-300:]); raise SystemExit(1)
out=subprocess.run([str(d/"diag5.exe"),
    r"C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Google Chrome.lnk",
    r"D:\apps\Qoder\Qoder.exe",
    "shell:AppsFolder\\OpenSquilla"], capture_output=True, text=True, encoding="utf-8", errors="replace")
print(out.stdout)
from PIL import Image
for i in range(3):
    p = d / f"probe_{i}.bmp"
    if p.exists():
        im = Image.open(p)
        print(p.name, im.size, im.mode)
        im.convert("RGBA").save(d/f"probe_{i}.png")
