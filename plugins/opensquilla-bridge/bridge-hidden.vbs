' Hidden-window launcher: wscript bridge-hidden.vbs <program> <args...>
' Used by scheduled task / autostart; stdout is dropped (the bridge writes --log itself).
'
' IMPORTANT: keep this file pure ASCII. wscript parses .vbs as ANSI (GBK on zh-CN
' Windows); UTF-8 Chinese comments eat LF line endings via dangling GBK lead bytes,
' which merges following code lines into comments ("Object required: 'sh'").
' See also ensure-bridge-hidden.vbs: same .vbs parsing rules apply there.
'
' Also: VBScript syntax only (CreateObject / Set / & concatenation) -- no JScript
' (var / new ActiveXObject), or wscript dies with 800A0401 "Statement expected".
' WScript.Arguments is a collection object: assignment requires Set, or 800A01C2.
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

sh.Run cmd, 0, False ' 0 = hidden window
