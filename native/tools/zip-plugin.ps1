param(
  [Parameter(Mandatory = $true)][string]$Src,
  [Parameter(Mandatory = $true)][string]$Out
)

# Build a Qoder-installable plugin ZIP. ASCII-only on purpose: PowerShell 5
# reads .ps1 as ANSI, so a UTF-8 file with Chinese comments breaks parsing.
#
# Entry names MUST use forward slashes - Compress-Archive writes backslashes and
# Qoder then can't find .qoder-plugin/plugin.json (EXTENSION_PLUGIN_VALIDATION_FAILED).
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# GetFullPath normalizes ".." segments; Resolve-Path may keep them verbatim on
# PowerShell 5, which makes the Substring below produce an empty entry name.
$root = [System.IO.Path]::GetFullPath($Src).TrimEnd('\')
if (-not (Test-Path -LiteralPath $root)) {
  throw "plugin dir not found: $root"
}
$outFull = [System.IO.Path]::GetFullPath($Out)
$parent = Split-Path $outFull
if (-not (Test-Path -LiteralPath $parent)) {
  New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
if (Test-Path -LiteralPath $outFull) {
  Remove-Item -LiteralPath $outFull -Force
}

$zip = [System.IO.Compression.ZipFile]::Open($outFull, [System.IO.Compression.ZipArchiveMode]::Create)
try {
  Get-ChildItem -Path $root -Recurse -Force -File | ForEach-Object {
    # Case-insensitive names: never use $src / $out here, they collide with $Src / $Out
    # and the typed param coerces the stream into a string.
    $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
      $zip, $_.FullName, $rel,
      [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
  }
} finally {
  $zip.Dispose()
}
Write-Output "zipped -> $outFull"
