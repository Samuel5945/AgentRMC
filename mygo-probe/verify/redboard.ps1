param([int]$X, [int]$Y, [int]$W, [int]$H, [int]$Ms = 60000)
Add-Type -AssemblyName System.Windows.Forms | Out-Null
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public class DpiFixBoard { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[DpiFixBoard]::SetProcessDPIAware() | Out-Null
$f = New-Object System.Windows.Forms.Form
$f.StartPosition = 'Manual'
$f.Location = New-Object System.Drawing.Point($X, $Y)
$f.Size = New-Object System.Drawing.Size($W, $H)
$f.BackColor = [System.Drawing.Color]::Red
$f.FormBorderStyle = 'None'
$f.TopMost = $true
$f.Text = 'RED-BOARD'
$t = New-Object System.Windows.Forms.Timer
$t.Interval = $Ms
$t.Add_Tick({ $f.Close() })
$t.Start()
[void]$f.ShowDialog()
