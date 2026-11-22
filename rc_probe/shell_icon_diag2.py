"""用真 C++ 复现 ShellIcon 的两条失败路径：.lnk 的 GetImage HRESULT
和 AppsFolder 枚举绑定 HRESULT。直接定位是「GetImage 没取到位图」
还是「.lnk 解析目标失败」。
"""
import pathlib
import subprocess
import sys

src = r'''
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>    // BHID_EnumItems 在这里
#include <shellapi.h>
#include <stdio.h>
#include <wctype.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static HRESULT probe(const wchar_t *path) {
    IShellItem *item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) { wprintf(L"open 0x%08X  %ls\n", (unsigned)hr, path); return hr; }
    IShellItemImageFactory *f = nullptr;
    hr = item->QueryInterface(IID_PPV_ARGS(&f));
    if (FAILED(hr)) { wprintf(L"qint 0x%08X  %ls\n", (unsigned)hr, path); item->Release(); return hr; }
    HBITMAP hbm = nullptr;
    SIZE sz = {64, 64};
    hr = f->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm);
    BITMAP bmp{};
    int w=0, h=0, bpp=0;
    if (SUCCEEDED(hr) && hbm && GetObjectW(hbm, sizeof(bmp), &bmp)) { w=bmp.bmWidth; h=bmp.bmHeight; bpp=bmp.bmBitsPixel; }
    wprintf(L"getimage 0x%08X  %ls  size=%dx%d bpp=%d\n", (unsigned)hr, path, w, h, bpp);
    if (hbm) DeleteObject(hbm);
    f->Release(); item->Release();
    return hr;
}

int wmain(int argc, wchar_t **argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (int i = 1; i < argc; ++i) probe(argv[i]);
    // AppsFolder enumeration
    IShellItem *root = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&root));
    wprintf(L"appsfolder open 0x%08X\n", (unsigned)hr);
    if (SUCCEEDED(hr)) {
        IEnumShellItems *en = nullptr;
        hr = root->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en));
        wprintf(L"enum bind 0x%08X\n", (unsigned)hr);
        if (SUCCEEDED(hr)) {
            IShellItem *child = nullptr; int n=0;
            while (en->Next(1, &child, nullptr) == S_OK && child) {
                PWSTR nm=nullptr;
                HRESULT d = child->GetDisplayName(SIGDN_NORMALDISPLAY, &nm);
                if (SUCCEEDED(d) && nm) { if(n<8) wprintf(L"  child: %ls\n", nm); CoTaskMemFree(nm); }
                else if (n<8) wprintf(L"  child displayname 0x%08X\n", (unsigned)d);
                child->Release(); n++;
            }
            wprintf(L"child count=%d\n", n);
            en->Release();
        }
        root->Release();
    }
    return 0;
}
''';

d = pathlib.Path("tmp_diag")
d.mkdir(exist_ok=True)
(d / "diag.cpp").write_text(src, encoding="utf-8")
# find vcvars
vs = r"C:\Program Files\Microsoft Visual Studio\18\Community"
bat = f'''@echo off
call "{vs}\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
cl /nologo /EHsc /W3 "{d}\\diag.cpp" /Fe:"{d}\\diag.exe" /link shell32.lib ole32.lib gdi32.lib
'''
(d / "build.bat").write_text(bat)
r = subprocess.run(["cmd", "/c", str(d / "build.bat")], capture_output=True, text=True, encoding="utf-8", errors="replace")
print(r.stdout[-800:])
if r.returncode != 0:
    print("BUILD FAIL")
    sys.exit(1)
lnk = r"C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Google Chrome.lnk"
res = subprocess.run([str(d / "diag.exe"),
                      lnk,
                      r"D:\apps\Qoder\Qoder.exe",
                      "shell:AppsFolder\\OpenAI.Codex_26.1117.3838.0_x64__2p2nqsd0c76g0!App",
                      "shell:AppsFolder\\Codex"],
                     capture_output=True, text=True, encoding="utf-8", errors="replace")
print(res.stdout)
print(res.stderr[-300:] if res.stderr else "")
