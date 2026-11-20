# 手机下载契约（/m/file）

> 起因：2026-11-19 ~ 11-20 同一类 bug 反复出现三次——产物卡片点了下载，
> Chrome 存出来叫 `file.txt`（或没扩展名、名字乱码）。每次都是不同的小坑：
> ① 相对路径少拼了扩展名；② `rel` 里带了斜杠/整句中文；③ `scope=abs` 的参数
> 被 `queryMeta` 白名单静默丢掉 → 404 空响应 → Chrome 当空文件存成 `file.txt`。
>
> 结论：**不要再靠"看住每个入口"防它**。下载链路收成一条契约，三层各管一段，
> 每层都有可执行断言；新入口只准接进这条契约。

## 契约（三层，缺一不可）

### 1) 页面生成链接：只用 `fpUrl(...)`

- 三种形态，三选一，互斥：
  - `fpUrl(name)`   → inbox 里的 basename（`?name=`）
  - `fpUrl(null, rel)` → 会话工作区相对路径（`?scope=project&rel=`）
  - `fpUrl(null, null, abs)` → 项目外绝对路径（`?scope=abs&abs=`）
- 只要给了 `name/rel/abs` 任一，路由必须是 `/m/file`（列表是 `/m/files`）。
  `abs` 曾漏在这个判断外，项目外产物被拼成 `/m/files?...&scope=abs` → 404，
  页面报「文件不在」但文件就在桌面上（2026-11-20 用户实测）。
- 路径从正文里抠出来时：只认路径字符。中文句读（`，。；：、！？`）**必须**
  当分隔符，否则整句话会被当成路径（`rel=卡片层级已经拆好了…截图.png`）。
- 不允许在别处手拼 `/m/file?…` 字符串。

### 2) 点击下载：只走 `saveFile(url)`

`<a href>` 可以直接给人长按复制，但 `onclick` 必须走统一入口：

```js
a.onclick = (e) => { e.preventDefault(); saveFile(a.href); };
```

`saveFile()` 先 `HEAD` 探活：
- 200 → 建一个 `<a download>` 点击，交给浏览器按服务端给的
  `Content-Disposition` 落盘（流式，不占手机内存）；
- 404 → 页面 `toast('文件已不在电脑上…')`，**不发起下载**——404 的 body
  一旦进了下载器，就会变成 `file.txt`；
- 网络失败 → `toast('下载失败：网络不通')`。

### 3) 服务端：`/m/file` 唯一出口 `serveDownload()`

- GET 与 HEAD 同路由；HEAD 只回头（同 `Content-Length`/同 disposition），不返 body。
- 文件名只取**解析后的真实文件**（`Download.name`），不信页面传的 rel/name。
- 头：`Content-Disposition: attachment; filename="<ascii 兜底>"; filename*=UTF-8''<percent>`
  + `Content-Type`（按扩展名，缺省 `application/octet-stream`）
  + `X-Content-Type-Options: nosniff` + `Content-Length` + `Cache-Control: no-store`。
- 失败：`404` + `application/json` + `{"ok":false,"error":"file_missing"}`。
  **不允许**空 body 的 404（那正是 `file.txt` 的来源）。
- `.exe/.msi/.apk` 仍在 provider 里包一层 zip（Chrome 明文 HTTP 拦可执行文件）。

## 回归防线（改下载必须跑这三层）

| 层 | 位置 | 断言什么 |
|---|---|---|
| HTTP 契约 | `native/tests/lanweb_test.cpp`（9j/9j2/9k/9k2/9k3） | status、Content-Type、`filename*` 中文编码、`Content-Length`、HEAD 无 body、404 是 JSON |
| 真浏览器下载 | `rc_probe/mobile_page_probe.mjs flow` | `page.waitForEvent('download')` 拿到的 **suggestedFilename** 与字节；不存在文件 → 只 toast、不触发下载 |
| 手测冒烟 | 手机 `192.168.x.x:45916/m/` | inbox 文件、工作区产物（含中文名/子目录）、项目外绝对路径、已删除文件 |

`lanweb_test` 的用例是**矩阵式**的：换一种来源（inbox / project / abs）、
换一种名字（中文、空格、带 1.4.7 这种点号）都加一条，而不是只补眼前那一例。

## 新增下载入口 checklist

1. 链接由 `fpUrl` 生成，`onclick` 接 `saveFile`。
2. 服务端不需要新路由；若确需新路由，必须复用 `serveDownload`。
3. 在 `lanweb_test` 加一条对应的 HTTP 契约断言；`mobile_page_probe` 若涉及新入口，
   加一条真实 `download` 事件断言。
4. 手测四种来源各点一次（见上表）。
