' 无窗口启动桥：wscript bridge-hidden.vbs <程序> <参数...>
' 用于计划任务/开机自启，stdout 不落地（桥自带 --log 写文件）。
' 注意：本文件必须是 VBScript 语法（CreateObject / Set / 字符串用 & 拼接），
' 不能写 var / new ActiveXObject(...) 这类 JScript —— .vbs 扩展名会让 wscript
' 用 VBScript 引擎解析，混进 JScript 会在第 3 行报 800A0401「语句未结束」并弹窗。
' WScript.Arguments 是集合对象，赋值必须带 Set，否则 800A01C2。
Set sh = CreateObject("WScript.Shell")
Set args = WScript.Arguments
Dim cmd, i, a

If args.Count < 1 Then WScript.Quit 2

cmd = """" & args(0) & """"
For i = 1 To args.Count - 1
  a = args(i)
  If InStr(a, " ") > 0 And Left(a, 1) <> """" Then
    cmd = cmd & " """ & a & """"
  Else
    cmd = cmd & " " & a
  End If
Next

sh.Run cmd, 0, False ' 0 = 隐藏窗口
