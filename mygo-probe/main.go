// mygo-probe：AgentRMC 桌宠「生死线」探针。
//
// 用最小代码验证 MyGo 能不能承载桌宠形态，验证项：
//  1. 透明 + 无边框 + 置顶 + 不进任务栏的窗口
//  2. 透明底 PNG 立绘渲染（鲸鱼娘 idle 帧）
//  3. rgba 半透明卡片（美术主题的卡片合成路线）
//  4. 鼠标穿透 SetIgnoreMouseEvents（开窗后点不到 = 让点击落到下层）
//  5. 按住立绘拖动窗口（DragWindow）
//  6. 全局快捷键 Ctrl+Alt+T 兜底切穿透
package main

import (
	"embed"
	"fmt"
	"log"

	"github.com/egoist/mygo"
	"github.com/egoist/mygo/ui"
)

//go:embed assets/pet.png
var petFS embed.FS

type probe struct {
	win    *mygo.Window
	sprite ui.ImageSource
	pass   bool // true = 鼠标穿透已开启
}

func (p *probe) status() string {
	if p.pass {
		return "穿透:开"
	}
	return "穿透:关"
}

func (p *probe) toggle() {
	if p.win == nil {
		return
	}
	p.win.Update(func() {
		p.pass = !p.pass
		p.win.SetIgnoreMouseEvents(p.pass)
	})
}

func (p *probe) view(c *ui.Context) {
	ui.Column(c).Fill().Children(func() {
		// 立绘即拖拽手柄：按住鲸鱼娘拖动窗口
		ui.Box(c).Grow(1).Center().DragWindow().Children(func() {
			ui.Image(c, p.sprite).Width(200).Height(200)
		})
		// rgba 半透明卡片：验证美术主题的合成路线
		ui.Box(c).
			Background(ui.RGBA(18, 26, 42, 0.55)).
			Radius(10).
			Padding(10).
			Margin(10).
			Children(func() {
				ui.Row(c).Gap(10).Center().Children(func() {
					ui.Text(c, "MyGo探针").Bold()
					ui.Text(c, p.status()).FontSize(12)
					if ui.PrimaryButton(c, "切换穿透").Clicked() {
						p.toggle()
					}
				})
			})
	})
}

func main() {
	data, err := petFS.ReadFile("assets/pet.png")
	if err != nil {
		log.Fatal(err)
	}
	sprite, err := ui.DecodeImage(data)
	if err != nil {
		log.Fatal(err)
	}
	p := &probe{sprite: sprite}

	mygo.App.WhenReady(func() {
		p.win = mygo.NewWindow(mygo.WindowOptions{
			Title:         "MyGo Probe",
			Width:         240,
			Height:        300,
			Frameless:     true, // 无边框
			Transparent:   true, // 透明窗
			AlwaysOnTop:   true, // 置顶
			SkipTaskbar:   true, // 不进任务栏
			DisableShadow: true, // 透明窗不留阴影缝
			DisableResize: true,
			Content:       ui.View(p.view),
		})
		// 全局快捷键兜底：穿透开了以后按钮点不到，靠 Ctrl+Alt+T 切回来
		if err := mygo.GlobalShortcut.Register("Ctrl+Alt+T", p.toggle); err != nil {
			log.Fatal(err)
		}
		fmt.Println("probe ready: Ctrl+Alt+T 切换穿透")
	})
	if err := mygo.App.Run(); err != nil {
		log.Fatal(err)
	}
}
