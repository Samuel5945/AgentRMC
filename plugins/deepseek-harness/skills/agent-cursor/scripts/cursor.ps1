# Agent 专用虚拟指针 · 客户端
#
# AgentRMC 常驻进程里有一根「Agent 自己的」指针：Agent 把要指/要点的地方画在它
# 上面，用户自己的鼠标一动不动——人机可以同时用鼠标。这个脚本是它的通用入口，
# 任何 Agent（DSH / Codex / WorkBuddy / ZCode / Qoder / 你手打的）都能调。
#
#   走的是 AgentRMC 本机桥 127.0.0.1:45915（纯回环、无鉴权），AgentRMC 没在跑就
#   直接失败——不静默、不假装成功。
[CmdletBinding()]
param(
    # move | click | state | hide | show
    [Parameter(Position = 0)]
    [ValidateSet('move', 'click', 'state', 'hide', 'show')]
    [string]$Action = 'state',

    # 目标坐标（物理像素，与截图/GetCursorPos 同一套数）
    [Parameter(Position = 1)][int]$X,
    [Parameter(Position = 2)][int]$Y,

    [ValidateSet('left', 'right', 'middle', 'double')][string]$Button = 'left',
    [ValidateSet('auto', 'uia', 'post')][string]$Backend = 'auto',
    # 指针飞过去的时长（毫秒）；0 = 直接落在目标点上
    [int]$DurationMs = 240,
    # 署名：决定指针颜色和标签，让用户一眼看出是哪家 Agent 在动
    [string]$Agent,
    # 这么久没有新动作就自动淡出；0 = 一直留着
    [int]$HoldMs = 8000,
    [int]$FadeMs = 260,
    [string]$Bridge = 'http://127.0.0.1:45915',
    [switch]$Json
)

$ErrorActionPreference = 'Stop'

function Get-CallerName {
    # 零配置也要能区分「谁在动」：往上找第一个不是 shell 的父进程名。
    try {
        $pid_ = $PID
        for ($i = 0; $i -lt 3; $i++) {
            $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$pid_" -ErrorAction Stop
            if ($null -eq $proc) { break }
            $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$($proc.ParentProcessId)" -ErrorAction Stop
            if ($null -eq $parent) { break }
            $name = [IO.Path]::GetFileNameWithoutExtension($parent.Name)
            if ($name -notmatch '^(pwsh|powershell|cmd|bash|sh|wsl|conhost|WindowsTerminal)$') {
                return $name
            }
            $pid_ = $parent.ProcessId
        }
    } catch {
        # 拿不到就退回默认名，不影响功能
    }
    return 'Agent'
}

if (-not $Agent) { $Agent = Get-CallerName }

$payload = @{ agent = $Agent }
switch ($Action) {
    'move' {
        if ($PSBoundParameters.ContainsKey('X')) { $payload.x = $X }
        if ($PSBoundParameters.ContainsKey('Y')) { $payload.y = $Y }
        $payload.duration_ms = $DurationMs
        $payload.hold_ms = $HoldMs
    }
    'click' {
        if ($PSBoundParameters.ContainsKey('X')) { $payload.x = $X }
        if ($PSBoundParameters.ContainsKey('Y')) { $payload.y = $Y }
        $payload.duration_ms = $DurationMs
        $payload.hold_ms = $HoldMs
        $payload.button = $Button
        $payload.backend = $Backend
    }
    'hide' {
        $payload.fade_ms = $FadeMs
    }
}

$uri = "$Bridge/cursor/$Action"
try {
    if ($Action -eq 'state' -or $Action -eq 'show' -or $Action -eq 'hide') {
        if ($Action -eq 'state') {
            $response = Invoke-RestMethod -Uri $uri -Method Get -TimeoutSec 5
        } else {
            $response = Invoke-RestMethod -Uri $uri -Method Post -TimeoutSec 5 `
                -ContentType 'application/json' `
                -Body ([Text.Encoding]::UTF8.GetBytes(($payload | ConvertTo-Json -Compress)))
        }
    } else {
        $response = Invoke-RestMethod -Uri $uri -Method Post -TimeoutSec 5 `
            -ContentType 'application/json' `
            -Body ([Text.Encoding]::UTF8.GetBytes(($payload | ConvertTo-Json -Compress)))
    }
} catch {
    [Console]::Error.WriteLine("agent-cursor: 连不上 AgentRMC 的桥（$Bridge）——桌宠/原生端没在跑？`n$($_.Exception.Message)")
    exit 1
}

if ($Json) {
    $response | ConvertTo-Json -Depth 6 -Compress
} elseif ($Action -eq 'state') {
    "visible={0} agent={1} x={2} y={3}" -f $response.visible, $response.agent, $response.x, $response.y
} elseif ($Action -eq 'click') {
    # 点击是异步的：接口立刻回 200，真实结局要看 /cursor/state 的 last_click。
    # 这里顺手查一次，把结果摊给调用方，省得每个 Agent 自己再写一遍轮询。
    $final = $null
    for ($i = 0; $i -lt 8; $i++) {
        Start-Sleep -Milliseconds 120
        try {
            $state = Invoke-RestMethod "$Bridge/cursor/state" -TimeoutSec 3
            if ($state.last_click -and $state.last_click.at_ms -gt 0) { $final = $state.last_click; break }
        } catch { break }
    }
    if ($final) {
        "clicked={0} backend={1} verified={2} target={3}" -f `
            $final.ok, $final.backend, $final.verified, $final.target
        if (-not $final.ok) {
            [Console]::Error.WriteLine("agent-cursor: 点击没生效：$($final.error)")
            exit 2
        }
    } else {
        "clicked=dispatched backend=async（没读到回执，可查 /cursor/state 的 last_click）"
    }
} else {
    "ok={0}" -f $response.ok
}

# 点击没落下去（比如目标窗不认合成消息）必须让调用方看得见：退出码不为 0，
# Agent 才知道「这一步没成」，而不是以为点到了接着往下跑。
if ($response.PSObject.Properties.Name -contains 'ok' -and -not $response.ok) {
    [Console]::Error.WriteLine("agent-cursor: $Action 没成功：$($response.error)")
    exit 2
}
exit 0
