// webprobe：MyGo Web 模式（WebView2）透明验证探针。
//
// 原生 UI 模式已实锤白底（见根 README），本探针验证 Web 模式是否透明：
// 静态页纯 Go 嵌入（mygo.SetFrontend，无 Bun），页面背景 transparent，
// 同样的红板三段采样判定。窗口选项与原生探针一致。
package main

import (
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"log"

	"github.com/egoist/mygo"
)

//go:embed dist
var distFS embed.FS

func main() {
	web, err := fs.Sub(distFS, "dist")
	if err != nil {
		log.Fatal(err)
	}
	mygo.SetFrontend(web)

	x := flag.Int("x", -1, "window X (DIP), -1 = default position")
	y := flag.Int("y", -1, "window Y (DIP), -1 = default position")
	tr := flag.Bool("transparent", true, "Transparent window option")
	fl := flag.Bool("frameless", true, "Frameless window option")
	flag.Parse()

	mygo.App.WhenReady(func() {
		opts := mygo.WindowOptions{
			Title:         "MyGo Probe",
			Width:         240,
			Height:        300,
			URL:           "/",      // 解析到 mygo://localhost/，即嵌入的 dist
			Frameless:     *fl,      // 无边框
			Transparent:   *tr,      // 透明窗（本探针要验证的关键项）
			AlwaysOnTop:   true,     // 置顶
			SkipTaskbar:   true,     // 不进任务栏
			DisableShadow: true,     // 透明窗不留阴影缝
			DisableResize: true,
		}
		if *x >= 0 {
			opts.X = *x
		}
		if *y >= 0 {
			opts.Y = *y
		}
		win := mygo.NewWindow(opts)

		// 全局快捷键切穿透（Go 侧直接调，不走 IPC）
		pass := false
		if err := mygo.GlobalShortcut.Register("Ctrl+Alt+T", func() {
			pass = !pass
			win.SetIgnoreMouseEvents(pass)
		}); err != nil {
			log.Fatal(err)
		}
		fmt.Println("webprobe ready: Ctrl+Alt+T 切换穿透")
	})
	if err := mygo.App.Run(); err != nil {
		log.Fatal(err)
	}
}
