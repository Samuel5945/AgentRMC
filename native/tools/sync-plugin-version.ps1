param(
  [switch]$Check
)
$ErrorActionPreference = 'Stop'

# Single source of truth for the plugin version is plugins/agentrmc-bridge/VERSION.
# The five manifests below must agree with it; Qoder's plugin page, the local
# marketplace index and the zip filename all show a version, and when they drift
# you cannot tell which package is actually installed.
#
# -Check  : verify only, exit 1 on drift (package.bat runs this before packaging)
# default : rewrite the manifests to match VERSION
#
# ASCII-only on purpose: PowerShell 5 reads .ps1 as ANSI.

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$versionFile = Join-Path $root 'plugins\agentrmc-bridge\VERSION'

if (-not (Test-Path -LiteralPath $versionFile)) { throw "missing $versionFile" }
$version = ([System.IO.File]::ReadAllText($versionFile)).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "bad VERSION content: '$version'" }

$targets = @(
  'plugins\agentrmc-bridge\.qoder-plugin\plugin.json',
  'plugins\agentrmc-bridge\.zcode-plugin\plugin.json',
  'plugins\agentrmc-bridge\.codebuddy-plugin\plugin.json',
  'plugins\marketplace.json',
  'plugins\.codebuddy-plugin\marketplace.json'
)

function Read-Text([string]$path, [ref]$hadBom) {
  $bytes = [System.IO.File]::ReadAllBytes($path)
  $bom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
  $hadBom.Value = $bom
  return [System.Text.Encoding]::UTF8.GetString($bytes)
}

function Write-Text([string]$path, [string]$text, [bool]$bom) {
  [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($bom)))
}

$drift = @()
$applied = @()
foreach ($rel in $targets) {
  $path = Join-Path $root $rel
  if (-not (Test-Path -LiteralPath $path)) { throw "missing manifest: $rel" }
  $hadBom = $false
  $text = Read-Text $path ([ref]$hadBom)

  # Exactly one version field per file - if that ever changes, refuse to guess.
  # ($matches is a PowerShell automatic variable; do not reuse that name.)
  $hits = [regex]::Matches($text, '(?<q>"version"\s*:\s*")(?<ver>[^"]*)(?<tail>")')
  if ($hits.Count -ne 1) {
    throw "expected exactly 1 version field in $rel, found $($hits.Count)"
  }
  $hit = $hits[0]
  if ($hit.Groups['ver'].Value -eq $version) { continue }

  $drift += "$rel=" + $hit.Groups['ver'].Value
  if ($Check) { continue }

  $start = $hit.Groups['ver'].Index
  $updated = $text.Substring(0, $start) + $version + $text.Substring($start + $hit.Groups['ver'].Length)
  Write-Text $path $updated $hadBom

  # Assert the write landed before moving to the next file.
  $verify = Read-Text $path ([ref]$hadBom)
  $vm = [regex]::Match($verify, '"version"\s*:\s*"([^"]*)"')
  if ($vm.Groups[1].Value -ne $version) { throw "$rel still not $version after write" }
  $applied += "$rel -> $version"
}

Write-Output "plugin version source: $version"
if ($Check) {
  if ($drift.Count -gt 0) {
    Write-Output "PLUGIN VERSION DRIFT: $($drift -join ' ')  (run sync-plugin-version.ps1)"
    exit 1
  }
  Write-Output 'PLUGIN VERSION OK'
  exit 0
}
foreach ($a in $applied) { Write-Output "synced $a" }
if ($applied.Count -eq 0) { Write-Output 'all manifests already current' }
