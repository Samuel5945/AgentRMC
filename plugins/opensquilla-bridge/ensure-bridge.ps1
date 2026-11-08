# 桌宠联动桥看门狗：桥不在就拉起来，桥卡死就杀掉重启，留一行日志好查"什么时候死的"。
# 由计划任务每 5 分钟调用；也可手动跑一次。
# 路径按脚本自身目录（$PSScriptRoot）取，整个目录挪走/拷给别人都能直接用。
$ErrorActionPreference = 'SilentlyContinue'

$skillDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$bridge   = Join-Path $skillDir 'opensquilla-pet-bridge.mjs'
$vbs      = Join-Path $skillDir 'bridge-hidden.vbs'
$log      = Join-Path $env:TEMP 'pet-bridge.log'
$wdLog    = Join-Path $env:TEMP 'pet-bridge-watchdog.log'
$hb       = Join-Path $env:TEMP 'pet-bridge.heartbeat'
$node     = (Get-Command node.exe).Source

function Log($msg) {
  $line = '{0} {1}' -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), $msg
  Add-Content -Path $wdLog -Value $line -Encoding UTF8
}

# 只找命令行里带桥脚本名的 node 进程，别误伤其它 node
$running = @(Get-CimInstance Win32_Process -Filter "Name='node.exe'" |
             Where-Object { $_.CommandLine -like '*opensquilla-pet-bridge.mjs*' })

if ($running.Count -ge 1) {
  # 活着还要查卡死：桥每 30s 写一拍心跳（网关断了在重连循环里也照写），
  # 超 3 分钟没更新 = 漏 6 拍 = 事件循环死了，杀掉重启。
  # 心跳文件不在（旧版桥/刚启动还没写）时保守放行，不误杀。
  if (Test-Path $hb) {
    $age = ((Get-Date) - (Get-Item $hb).LastWriteTime).TotalMinutes
    if ($age -le 3) { exit 0 }
    Log "WEDGED detected (heartbeat stale $([math]::Round($age, 1)) min), killing pid=$($running[0].ProcessId)"
    $running | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    Start-Sleep -Seconds 1
  } else {
    exit 0
  }
}

if (-not (Test-Path $bridge) -or -not (Test-Path $vbs)) {
  Log "MISSING_SCRIPT bridge=$bridge vbs=$vbs"
  exit 1
}

Log "DOWN detected, restarting (previous alive count=$($running.Count))"
wscript.exe $vbs $node $bridge --relay --log $log | Out-Null

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
