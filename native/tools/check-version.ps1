$ErrorActionPreference = 'Stop'

# FR-07: single source of truth is native/CMakeLists.txt project(VERSION x.y.z).
# This script proves the generated NSIS include and the built exe agree with it.
# ASCII-only on purpose: PowerShell 5 reads .ps1 as ANSI.

$native = Split-Path -Parent $PSScriptRoot
$cmakeLists = Join-Path $native 'CMakeLists.txt'
$exe = Join-Path $native 'build\Release\AgentRMCNative.exe'
$nsi = Join-Path $native 'build\generated\version.nsi'

$m = Select-String -Path $cmakeLists -Pattern 'project\s*\(\s*AgentRMCNative\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' |
  Select-Object -First 1
if (-not $m) { throw "cannot find project version in $cmakeLists" }
$cmakeVer = $m.Matches[0].Groups[1].Value

if (-not (Test-Path -LiteralPath $exe)) { throw "missing exe: $exe" }
$info = (Get-Item -LiteralPath $exe).VersionInfo
$exeVer = $info.FileVersion
$prodVer = $info.ProductVersion

if (-not (Test-Path -LiteralPath $nsi)) { throw "missing generated nsi: $nsi" }
$n = Select-String -Path $nsi -Pattern 'APPVERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"' | Select-Object -First 1
if (-not $n) { throw "cannot find APPVERSION in $nsi" }
$nsiVer = $n.Matches[0].Groups[1].Value

Write-Output "cmake=$cmakeVer nsi=$nsiVer exe.FileVersion=$exeVer exe.ProductVersion=$prodVer"
if ($cmakeVer -ne $nsiVer -or $cmakeVer -ne $exeVer -or $cmakeVer -ne $prodVer) {
  Write-Error "version mismatch: CMake / NSIS / exe must all be $cmakeVer"
  exit 1
}

# The plugin has its own version line (plugins/agentrmc-bridge/VERSION) and its
# own release channel (the zip the hosts install). It is deliberately not required
# to match Native; it IS required to match the five manifests, otherwise the
# installed plugin page and the zip filename disagree about what version is what.
& (Join-Path $PSScriptRoot 'sync-plugin-version.ps1') -Check
if ($LASTEXITCODE -ne 0) {
  Write-Error "plugin manifests disagree with plugins/agentrmc-bridge/VERSION"
  exit 1
}

Write-Output 'VERSION OK'
