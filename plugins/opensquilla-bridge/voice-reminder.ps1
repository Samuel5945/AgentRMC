# AgentRMC 语音提醒：对 127.0.0.1:45915 发 /speak 或 /notify。
# 关键点：PowerShell 5.1 的 curl.exe 传中文参数会按 GBK 码页转，桌宠收到的 JSON
# 就乱码。所以这里不走 curl，用 UTF-8 字节体 + Invoke-WebRequest 直发。
#
# 用法：
#   voice-reminder.ps1 -Message "该睡了"                     # 出声（气泡+TTS）
#   voice-reminder.ps1 -Notify -Message "等你确认" -Raise "ZCode.exe"   # 不出声+拉前台
#   voice-reminder.ps1 -Check                                # 只看 /state
#
# 退出码：0 成功；1 桌宠没在跑/端点不通；2 参数错；3 服务端回了非 204。
[CmdletBinding()]
param(
    [string]$Message = "",
    [switch]$Notify,
    [string]$Event = "message",
    [string]$Title = "OpenSquilla",
    [string]$Raise = "",
    [switch]$Check,
    [string]$Base = "http://127.0.0.1:45915"
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Invoke-Bridge($Method, $Path, $Obj) {
    $url = "$Base$Path"
    if ($Method -eq "GET") {
        return (Invoke-WebRequest -Uri $url -Method Get -UseBasicParsing -TimeoutSec 3)
    }
    $json = ($Obj | ConvertTo-Json -Compress)
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    return (Invoke-WebRequest -Uri $url -Method Post -Body $bytes `
                -ContentType "application/json; charset=utf-8" -UseBasicParsing -TimeoutSec 5)
}

try {
    $stateResp = Invoke-Bridge GET "/state" $null
} catch {
    Write-Host "AgentRMC 没在跑或端口 45915 不通：$($_.Exception.Message)"
    Write-Host "先启动 AgentRMCNative.exe（装机位置见注册表 HKCU\Software\AgentRMC\InstallDir），再重试。"
    exit 1
}

$state = $null
try { $state = $stateResp.Content | ConvertFrom-Json } catch { }
Write-Host ("STATE  pet_visible={0} tts_ready={1} pet_enabled={2}" -f `
    $state.pet_visible, $state.tts_ready, $state.pet_enabled)

if ($Check) { Write-Host $stateResp.Content; exit 0 }

if ([string]::IsNullOrWhiteSpace($Message)) {
    Write-Host "缺少 -Message（提醒文案）。"
    exit 2
}
if ($Message.Length -gt 2000) {
    Write-Host "文案过长（>2000 字），TTS 体验差，先精简。"
    exit 2
}

try {
    if ($Notify) {
        $obj = [ordered]@{ event = $Event; title = $Title; message = $Message }
        if ($Raise) { $obj.raise = $Raise }
        $resp = Invoke-Bridge POST "/notify" $obj
        Write-Host "NOTIFY OK（气泡，不出声）"
    } else {
        $resp = Invoke-Bridge POST "/speak" ([ordered]@{ message = $Message })
        Write-Host "SPEAK OK（气泡+TTS）"
    }
    if ($resp.StatusCode -ne 204) {
        Write-Host "服务端返回 $($resp.StatusCode)，不是 204，检查一下文案/端点。"
        exit 3
    }
    if (-not $Notify -and -not $state.tts_ready) {
        Write-Host "注意：tts_ready=false，气泡会出现但念不出声（本地模型/音色未就绪）。"
    }
    if (-not $Notify -and -not $state.pet_visible) {
        Write-Host "注意：桌宠当前收起；/speak 仍会朗读，但气泡看不到。"
    }
    exit 0
} catch {
    Write-Host "发送失败：$($_.Exception.Message)"
    exit 3
}
