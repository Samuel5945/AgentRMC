# Prints one path: an executable that can run Node scripts. Prints nothing if none.
# Used by agentrmc-launch.cmd as the last-resort step (see that file for the order).
# 1) The nearest non-shell ancestor that is a known Electron host: the host that
#    spawned this hook is the most accurate answer and needs no configuration.
# 2) The host's install root from the uninstall registry (DisplayIcon / UninstallString).
# 3) Any running node.exe.
$ErrorActionPreference = 'SilentlyContinue'

$shells = @('cmd.exe', 'powershell.exe', 'pwsh.exe', 'conhost.exe', 'bash.exe', 'sh.exe',
            'git-bash.exe', 'OpenConsole.exe', 'WindowsTerminal.exe', 'node.exe')
$hosts = @('Qoder.exe', 'Qoder CN.exe', 'ZCode.exe', 'WorkBuddy.exe', 'WorkBuddyAI.exe',
           'CodeBuddy.exe', 'DeepSeek Harness.exe', 'OpenSquilla.exe')

$id = $PID
for ($i = 0; $i -lt 10; $i++) {
    $p = Get-CimInstance Win32_Process -Filter "ProcessId=$id"
    if (-not $p) { break }
    if ($hosts -contains $p.Name -and $p.ExecutablePath) { $p.ExecutablePath; exit 0 }
    if ($shells -notcontains $p.Name -and $p.ExecutablePath) { break }
    $id = $p.ParentProcessId
    if ($id -le 0) { break }
}

$uninstall = @(
    'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
)
foreach ($root in $uninstall) {
    $entries = Get-ChildItem $root | Get-ItemProperty |
        Where-Object { $_.DisplayName -match 'Qoder|ZCode|WorkBuddy|CodeBuddy|DeepSeek|OpenSquilla' }
    foreach ($entry in $entries) {
        $dir = $null
        if ($entry.DisplayIcon) { $dir = Split-Path ($entry.DisplayIcon -replace ',\d+$', '') }
        elseif ($entry.UninstallString) { $dir = Split-Path ($entry.UninstallString -replace '^"([^"]+)".*$', '$1') }
        if (-not $dir) { continue }
        foreach ($h in $hosts) {
            $candidate = Join-Path $dir $h
            if (Test-Path -LiteralPath $candidate) { $candidate; exit 0 }
        }
        # Versioned installs keep the real binary one level down (.qoder-versions/<ver>/).
        $versioned = Get-ChildItem (Join-Path $dir '.qoder-versions') -Directory |
            Sort-Object Name -Descending
        foreach ($v in $versioned) {
            foreach ($h in $hosts) {
                $candidate = Join-Path $v.FullName $h
                if (Test-Path -LiteralPath $candidate) { $candidate; exit 0 }
            }
        }
    }
}

$node = Get-CimInstance Win32_Process -Filter "Name='node.exe'" | Select-Object -First 1
if ($node -and $node.ExecutablePath) { $node.ExecutablePath }
exit 0
