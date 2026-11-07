param([string]$Path)
Add-Type -AssemblyName System.Windows.Forms | Out-Null
Add-Type -AssemblyName System.Drawing | Out-Null
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public class DpiFixShot { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[DpiFixShot]::SetProcessDPIAware() | Out-Null
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "saved $Path $($b.Width)x$($b.Height)"
