# 桌宠联动桥看门狗：桥不在就拉起来，并留一行日志好查"什么时候死的"。
# 由计划任务每 5 分钟调用；也可手动跑一次。
$ErrorActionPreference = 'SilentlyContinue'

$skillDir = 'C:\Users\PC\.agents\skills\agentrmc-voice-reminder\scripts'
$bridge   = Join-Path $skillDir 'opensquilla-pet-bridge.mjs'
$vbs      = Join-Path $skillDir 'bridge-hidden.vbs'
$log      = Join-Path $env:TEMP 'pet-bridge.log'
$wdLog    = Join-Path $env:TEMP 'pet-bridge-watchdog.log'
$node     = (Get-Command node.exe).Source

function Log($msg) {
  $line = '{0} {1}' -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), $msg
  Add-Content -Path $wdLog -Value $line -Encoding UTF8
}

# 只找命令行里带桥脚本名的 node 进程，别误伤其它 node
$running = @(Get-CimInstance Win32_Process -Filter "Name='node.exe'" |
             Where-Object { $_.CommandLine -like '*opensquilla-pet-bridge.mjs*' })

if ($running.Count -ge 1) {
  # 活着就什么都不做（不打日志，避免每 5 分钟刷一条）
  exit 0
}

if (-not (Test-Path $bridge) -or -not (Test-Path $vbs)) {
  Log "MISSING_SCRIPT bridge=$bridge vbs=$vbs"
  exit 1
}

Log "DOWN detected, restarting (previous alive count=0)"
wscript.exe $vbs $node $bridge --log $log | Out-Null

Start-Sleep -Seconds 6
$after = @(Get-CimInstance Win32_Process -Filter "Name='node.exe'" |
           Where-Object { $_.CommandLine -like '*opensquilla-pet-bridge.mjs*' })
if ($after.Count -ge 1) {
  Log "UP pid=$($after[0].ProcessId)"
  exit 0
} else {
  Log "FAILED to start"
  exit 2
}
