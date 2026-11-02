' OpenSquilla 桌宠联动桥 —— 登录自启（隐藏窗口）。
' 放这里：shell:startup。要停：任务管理器杀 node，或删本文件。
' 免打扰：New-Item "$env:TEMP\opensquilla-pet-bridge.pause"；恢复：删掉它。
Set sh = CreateObject("WScript.Shell")
Dim node, bridge, log
node   = "node.exe"
bridge = "C:\Users\PC\.agents\skills\agentrmc-voice-reminder\scripts\opensquilla-pet-bridge.mjs"
log    = "C:\Users\PC\AppData\Local\Temp\pet-bridge.log"
sh.Run """" & node & """ """ & bridge & """ --log """ & log & """", 0, False
