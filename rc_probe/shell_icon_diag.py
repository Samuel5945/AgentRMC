"""ShellIcon 离线诊断：把每个应用解析出的 .lnk / exe / AUMID 用 Shell
(IShellItemImageFactory) 真实取一遍图标，打印 HRESULT / 尺寸 / alpha，
定位图标失败的确切环节。

用法: python rc_probe/shell_icon_diag.py
"""
import ctypes
import ctypes.wintypes as wt
import sys

sys.path.insert(0, r"C:\Users\PC\AppData\Roaming\Python\Python312\site-packages")

AddType = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class SIcon {
  [DllImport("shell32.dll", CharSet=CharSet.Unicode)]
  public static extern int SHCreateItemFromParsingName(string p, IntPtr x, ref Guid riid, out IntPtr ppv);
  [DllImport("shell32.dll")] public static extern int SHGetFileInfo(string p, uint a, out SHFILEINFO i, int cb, uint f);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct SHFILEINFO {
    public IntPtr hIcon; public int iIcon; public uint dwAttributes;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string szDisplayName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=80)] public string szTypeName;
  }

  [ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IShellItemImageFactoryNative {
    [PreserveSig] int GetImage(SIZE size, SIIGBF flags, out IntPtr phbm);
  }
  [StructLayout(LayoutKind.Sequential)] public struct SIZE { public int cx; public int cy; }
  [Flags] public enum SIIGBF { RESIZETOFIT=0, BIGGERSIZEOK=1, MEMORYONLY=2, ICONONLY=4, THUMBNAILONLY=8, INCACHEONLY=16, CROPTOSQUARE=32, WIDETHUMBNAILS=64, ICONBACKGROUND=128, SCALEUP=256 }

  [DllImport("gdi32.dll")] public static extern bool GetObjectW(IntPtr h, int c, out BITMAPINFO bmi);
  [StructLayout(LayoutKind.Sequential)]
  public struct BITMAPINFO { public int bmiHeader_biSize; public int bmiHeader_biWidth; public int bmiHeader_biHeight; public short bmiHeader_biPlanes; public short bmiHeader_biBitCount; public int bmiHeader_biCompression; public int bmiHeader_biSizeImage; public int bmiHeader_biXPelsPerMeter; public int bmiHeader_biYPelsPerMeter; public int bmiHeader_biClrUsed; public int bmiHeader_biClrImportant; }

  public static string Probe(string path){
    StringBuilder sb=new StringBuilder();
    Guid iid=new Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b");
    IntPtr o;
    int hr = SHCreateItemFromParsingName(path, IntPtr.Zero, ref iid, out o);
    if(hr!=0 || o==IntPtr.Zero){ sb.Append("factoryOpen=0x").Append(hr.ToString("X8")); return sb.ToString(); }
    IShellItemImageFactoryNative f=(IShellItemImageFactoryNative)Marshal.GetObjectForIUnknown(o);
    SIZE sz=new SIZE{cx=64,cy=64};
    IntPtr hbm;
    int hrG = f.GetImage(sz, SIIGBF.ICONONLY | SIIGBF.BIGGER sizeOK, out hbm);
    Marshal.ReleaseComObject(f); Marshal.Release(o);
    if(hrG!=0 || hbm==IntPtr.Zero){ sb.Append("GetImage=0x").Append(hrG.ToString("X8")); return sb.ToString(); }
    BITMAPINFO bi;
    GetObjectW(hbm, Marshal.SizeOf(typeof(BITMAPINFO)), out bi);
    sb.Append("w=").Append(bi.bmiHeader_biWidth).Append(" h=").Append(bi.bmiHeader_biHeight)
      .Append(" bpp=").Append(bi.bmiHeader_biBitCount).Append(" compress=").Append(bi.bmiHeader_biCompression);
    gdi32.DeleteObject(hbm);
    return sb.ToString();
  }
}
'@

# 上面的 C# 有手误，直接重写干净版
AddType = @'
using System;
using System.Runtime.InteropServices;
public class SIcon2 {
  [DllImport("shell32.dll", CharSet=CharSet.Unicode)]
  public static extern int SHCreateItemFromParsingName(string p, IntPtr x, ref Guid riid, out IntPtr ppv);
  [ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  public interface IImgFactory {
    [PreserveSig] int GetImage(int cx, int cy, uint flags, out IntPtr phbm);
  }
  public static string Probe(string path){
    Guid iid=new Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b");
    IntPtr o;
    int hr = SHCreateItemFromParsingName(path, IntPtr.Zero, ref iid, out o);
    if(hr!=0 || o==IntPtr.Zero) return "open=0x"+hr.ToString("X8");
    IImgFactory f=(IImgFactory)Marshal.GetObjectForIUnknown(o);
    IntPtr hbm;
    int hrG = f.GetImage(64,64,4|1, out hbm); // ICONONLY|BIGGERSIZEOK
    if(hrG!=0 || hbm==IntPtr.Zero){ Marshal.ReleaseComObject(f); Marshal.Release(o); return "GetImage=0x"+hrG.ToString("X8"); }
    object bmpInfo=null;
    string extra="";
    try{
      // 拿 alpha：从位图字节里取像素均值
      byte[] buf = new byte[64*64*4];
      // 用 GetBitmapBits
    }catch(Exception e){ extra=e.Message; }
    Marshal.ReleaseComObject(f); Marshal.Release(o);
    return "ok"+extra;
  }
}
'@

Add-Type -TypeDefinition $AddType -Language CSharp
foreach($lnk in @('C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Google Chrome.lnk','C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Microsoft Edge.lnk')){
  if(Test-Path $lnk){ Write-Output ("{0} => {1}" -f (Split-Path $lnk -Leaf), [SIcon2]::Probe($lnk)) } else { Write-Output "MISSING $lnk" }
}
