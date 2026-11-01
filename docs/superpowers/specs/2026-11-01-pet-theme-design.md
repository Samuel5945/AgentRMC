# 桌宠虚拟形象主题系统设计（UI 美化 V1）

- 日期：2026-11-01
- 状态：设计已获口头批准，待实施
- 范围：桌宠形象动画化、主界面/语音面板背景、全局配色主题化、主题扩展接口、ComfyUI 生成管线

## 1. 背景与目标

现状：桌宠是 `VoicePet::paintEvent` 代码画的圆球+状态环；全部界面配色硬编码在
`native/resources/style.qss`（701 行）。

目标：

1. 用 ComfyUI 生成的透明 PNG 序列，把桌宠做成「鲸鱼娘」动画虚拟形象（以用户提供的
   踧裏鲸鱼娘参考图为角色锚点）；
2. 主界面与语音面板配统一生成的背景，形成一套完整主题；
3. 主题做成可扩展接口：后续加新主题形象 = 往 `themes/` 丢一个文件夹，无需重编译。

已确认的决策：

| 决策点 | 结论 |
|---|---|
| 形象来源 | 以用户参考图做角色锚点生成自有版权形象 |
| 动画档位 | 每状态 2~4 帧 PNG 序列 + 程序化浮动/倾斜 |
| V1 范围 | 桌宠全套状态 + 主界面/语音面板背景 + 全局配色；按键映射页不动 |
| 角色命名 | 名字跟主题走（`theme.json` 存 displayName/persona，换主题换名字） |

## 2. 现状盘点（关键事实）

- QSS：`native/resources/style.qss`，经 `resources/app.qrc` 嵌入 exe，
  `main.cpp` 启动时 `QFile(":/style.qss")` 加载。主窗背景走 `QWidget#root`。
- 桌宠：`VoicePet` 四态（未监听/监听/录音/转写）+ 对话模式；贴边缩条（kPeek=20px）、
  悬停卡片、气泡、右键菜单、拖拽停靠均已在位。
- 语音面板：`VoiceWindow::paintEvent` 自绘透明圆角底。
- LLM 人设：`VoicePet.cpp` 内置 `kDefaultChatSystem`（写死「语音桌宠」）。
- ComfyUI：`D:\AI_work`（`AutoLaunch_ComfyUI.bat`，端口 8188，ROCm/AMD）；
  模型仓库在 `D:\AI_Base\AImodels`。HTTP API 链路已在既往会话调通
  （`D:\AI_work\poster_test.py` 等）。
- 可用模型：`qwen_image_2.1_turbo_int8_convrot` / `qwen_image_2.1_int8_convrot`（各 6.8G）、
  `text_encoders/qwen3vl_8b_int8_convrot`、`vae/qwen_image_2.1_vae_bf16`。
  **Edit-2511 及其配套（gguf/LoRA/编码器/VAE）已全部删除，不再可用。**
- 关键能力（本机 ComfyUI 源码坐实）：
  - `TextEncodeQwenImage21` 支持最多 16 张可选参考图（`image_1..16`，以 reference
    latents 拼进文本序列）→ 角色一致性不需要 Edit 模型；
  - Qwen 2.1 VAE 按 4 通道（RGBA）编解码（`nodes_qwen.py:170` "the vae keeps all
    four"），核心 `SaveImage` 对 4 通道按 alpha 存 PNG → **提示词要透明背景即直出
    透明 PNG**，无需抠图节点。

## 3. 选型（已定）

- **A. 主题包目录 + 运行时发现（采纳）**：主题是 exe 旁 `themes/<id>/` 文件夹，
  启动扫描、即时切换、丢文件夹加新主题。
- B. 嵌入 qrc：加主题要重编译，与目标冲突，否决。
- C. Live2D/Spine：运行时与制作成本高，与「少量帧+程序动画」档位不符，否决。

## 4. 主题包规范

```
themes/
  classic/                # 内置默认主题：零美术资源，palette = 现有配色（兜底必存）
    theme.json
  xiaoai-whale/           # 第一个美术主题：鲸鱼娘「小爱」
    theme.json
    preview.png           # 主题选择器缩略图（自拼或生成）
    pet/                  # 入库 512×512 RGBA（1024 生成后缩），每状态 2 帧
      idle_0.png idle_1.png               # 站立呼吸
      listening_0.png listening_1.png     # 微微前倾
      recording_0.png recording_1.png     # 张嘴/举手
      transcribing_0.png transcribing_1.png  # 歪头思考
      chat_0.png chat_1.png               # 对话模式（可选，缺省用 idle）
    backgrounds/
      main.png            # 主界面背景：暗色低对比 + 暗角，字符区留净
      panel.png           # 语音面板背景
```

`theme.json` schema：

```json
{
  "id": "xiaoai-whale",
  "displayName": "小爱 · 鲸鱼娘",
  "author": "",
  "character": {
    "name": "小爱",
    "persona": "你是 AgentRMC 的语音桌宠小爱……（替代内置 kDefaultChatSystem）"
  },
  "palette": { "<token>": "#rrggbb | rgba(...)" },
  "pet": {
    "canvas": 512,
    "display_size": 96,
    "ring": true,
    "states": {
      "idle": { "frames": ["pet/idle_0.png", "pet/idle_1.png"],
                "fps": 2, "bob": 3, "tilt": 2 }
    }
  },
  "backgrounds": { "main": "backgrounds/main.png", "panel": "backgrounds/panel.png",
                   "dim": 0.55 }
}
```

- palette 语义 token（从现有 style.qss 提取，约 15 个）：`root.bg`、`card.bg`、
  `card.border`、`accent`、`accent.soft`、`accent.border`、`text`、`muted`、`ok`、
  `warn`、`danger`、`input.bg`、`input.border`、`tab.active`、`menu.bg` 等，
  提取时以 classic 逐项等于现值为准，允许实现期微调集合。
- 加载规则：缺 `id`/`displayName` 或清单解析失败 → 跳过该目录（选择器里置灰并
  在主窗事件流提示）；`ui.theme` 指向的主题缺失/损坏 → 回落 classic。
- 可选字段缺省行为：无 `pet` → 圆球回落；无 `backgrounds` → 纯色底；无
  `character` → 内置人设与名字。

## 5. 运行时接口

### 5.1 ThemeManager（`native/src/core/ThemeManager.{h,cpp}`，单例）

```cpp
class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager &instance();
    void rescan();                       // 扫描 exeDir/themes/
    QList<ThemeInfo> themes() const;
    Theme current() const;               // 解析后的主题（palette/精灵/背景/character）
    void setTheme(const QString &id);    // 写 ui.theme + applyQss + emit changed
signals:
    void themeChanged(const Theme &theme);
};
```

- `Theme` 提供现成数据：`paletteValue(token)`、`petFrames(state)`、
  `background(key)`、`character()`；图片懒加载并缓存 QPixmap。
- QSS 渲染：`style.qss` 改为含 `@token@` 占位符的模板（qrc 内置），启动/切换时
  替换后 `qApp->setStyleSheet`。主界面背景以
  `QWidget#root { background-image: url(<主题绝对路径>); }` 片段追加。
- 所有现有 UI（MainWindow/VoiceWindow/VoicePet/对话框）通过 `themeChanged` 或
  重绘响应切换；VoicePet 另需重读 character 人设。

### 5.2 PetSprite（`native/src/ui/PetSprite.{h,cpp}`）

- 输入：`Theme::petStates()`；内部 QTimer 按 fps 换帧，paintEvent 画当前帧 +
  程序化 bob（正弦上下 `bob` px）与 `tilt` 度倾斜（复用现有 `phase_` 相位思路）。
- 状态解析沿用 VoicePet 现有布尔（listening/recording/transcribing/chatMode），
  由 VoicePet 调 `PetSprite::setState()`。
- 贴边缩条：整只等比缩到 kPeek 可见宽度，不裁切。
- VoicePet::paintEvent 改为：主题有精灵 → 交给 PetSprite；无 → 保留现有圆球
  绘制（classic 永远可用）。`ring: true` 时在精灵外圈叠加现有状态环画法。
- 卡片/气泡/右键菜单/拖拽停靠逻辑不动。

### 5.3 背景接入

- 主界面：`QWidget#root` 的 background-image 主题化，卡片 `card.bg` 加 alpha 微透。
- 语音面板：`VoiceWindow::paintEvent` 先铺 `backgrounds/panel.png`（圆角裁剪逻辑
  保留），再叠 `dim` 不透明度遮罩保证文字可读。

## 6. 生成管线 `tools/theme/gen_theme.py`

打 ComfyUI HTTP API（`http://127.0.0.1:8188`，需先跑 `AutoLaunch_ComfyUI.bat`），
图结构沿用已验证的 `poster_test.py`（UNETLoader + CLIPLoader qwen3vl_8b +
VAELoader qwen_image_2.1_vae_bf16 + TextEncodeQwenImage21 + KSampler turbo 8 步
cfg 1.0 euler/simple），唯一区别：`TextEncodeQwenImage21` 挂 `image1` 参考锚点。

1. **锚点**：用户参考 JPEG 直接作为 `image1`（RGB 即可；输入尺寸取整 32 倍数）。
2. **状态帧**：EmptySD3LatentImage 1024×1024 + 姿势提示词（张嘴/举手/歪头等）+
   「透明背景、纯透明底」描述 → KSampler（denoise 1.0，参考图引导）每状态固定
   seed 出 4~6 张候选 → **RGBA PNG 直出**，人工挑选 2 帧。
3. **规整**：Pillow 裁内容包围盒、缩放 512、边缘杂色清理（可选 `--rembg` 开关，
   默认不用）。
4. **背景**：同图结构文生图（不挂参考、不透明），1728×1080 级别，暗色低对比；
   后处理压对比 + 暗角。
5. **收尾**：自动写 `theme.json` 脚手架（含 character/palette/pet/backgrounds）、
   拼 preview.png，输出到候选目录，人工确认后拷入 `themes/<id>/`。

零下载、零新增 Python 依赖（Pillow 已有；rembg 仅可选）。

## 7. UI 入口与持久化

- 语音设置对话框顶部加「主题」下拉：列出 `themes()` 显示名 + 「打开主题目录」
  按钮（explorer 打开，方便丢新主题）。
- 持久化：`ui.theme`（ProfileStore::setUiBool 同款机制，新增 setUiString）。
- 切换即时生效：applyQss + themeChanged 广播；桌宠、面板、主窗各自刷新。

## 8. 测试与验收

- 新增 `AgentRMCThemeTest`（native 控制台测试，仿 AgentRMCVoiceTest）：
  清单解析、损坏主题跳过、classic 兜底、QSS 令牌渲染结果断言。
- 既有 `--shot-pet` / `--shot` 截图模式出 PNG，走 vision 工具做视觉验收
  （遵守全局规则：不直接 Read 图片）。
- classic 零回归：M1 完成后与改动前截图逐张对比。
- 里程碑（每步可独立编译验收）：
  - M1 主题接口落地：ThemeManager + QSS 令牌化 + classic + 选择器（零美术）；
  - M2 精灵桌宠：PetSprite + 圆球回落 + xiaoai-whale 精灵接入（生成管线首次跑通）；
  - M3 背景与配色：main/panel 背景 + whale palette 调优；
  - M4 扩展性验证：用 gen_theme.py 造第二个主题（仅换锚点/配色）走完全流程。

## 9. 风险与对策

| 风险 | 对策 |
|---|---|
| 参考图生成帧间角色漂移 | 每状态多出候选人工挑 2 帧；姿势指令保持小幅度；锚点尺寸规整 |
| 背景干扰文字可读性 | 暗色低对比提示词 + `dim` 遮罩 + 截图 vision 验收 |
| QSS 令牌化回归 | classic palette 逐项等于现值 + M1 截图对比兜底 |
| AI 边缘偶发白边/杂色 | Pillow alpha 清理；必要时 `--rembg` 开关 |
| ROCm 出图速度未知（本机首次跑小图） | M2 先单张计时，慢则降 512 或换 turbo 参数 |

## 10. 明确不做（V1 边界）

- 按键映射页、托盘图标、对话框逐个换皮；
- Live2D/骨骼动画；
- 一套主题多角色混排；
- 主题市场/在线下载。
