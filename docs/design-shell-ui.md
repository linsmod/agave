# Shell UI：组件化 chrome 与平台 ops（设计记录）

> **设计记录，不是现状。** 现状见 [arch-overview.md](arch-overview.md) §1 与
> [arch-status.md](arch-status.md)；入口代码见 `examples/browser_shell/`。

## 1 要解决的问题

`examples/browser_shell/main.c` 到 2026-11 已是 6300 行单文件，同时装着：

- 窗口/标签/工具栏/查找栏/状态栏的**像素级布局与绘制**（`layoutAll`、`paintTabs`、
  `paintStatus`、`createShellWindow`）；
- 标签、会话、书签、历史、下载、单实例接力、探针和崩溃取证；
- 直接散落的 Win32/OS 调用：`GetEnvironmentVariable`、`CreateDirectoryA`、
  `RegGetValueA`、`MessageBoxA`、`GetTickCount64`、`GetDpiForWindow`、
  `DwmSetWindowAttribute`。

UI 侧的具体症状：

1. 工具栏是 7 个原生 `BUTTON`（`< > R X Go + =`）+ 1 个 `EDIT`，视觉上像 1995 年的
   对话框，和网页内容割裂；按钮文本在刷新时还会闪（`docs/plan-roadmap.md` P2）。
2. 标签栏/状态栏用 `DEFAULT_GUI_FONT` + 硬编码像素（`TAB_HEIGHT=28`、`TAB_W=172`），
   没有 DPI 缩放；系统 125%/150% 下要么太大要么挤成一团。
3. 标签宽固定 172，最多 16 个，窗口一窄就画到窗口外；没有 hover/pressed 状态，
   关闭按钮只是一个 `x` 字符。
4. 只有浅色；亮暗、强调色、聚焦环、进度条都不存在。

## 2 目标与边界

**目标**

- 视觉：Chrome 风格扁平 chrome——圆角活动标签、地址胶囊、hover/pressed、
  抗锯齿图标、亮/暗主题、Per-Monitor V2 DPI、双缓冲（无闪烁）。
- 结构：UI 拆成可独立阅读/测试的组件；`main.c` 只留壳逻辑与胶水。
- OS：所有“OS 服务”收口到 `shell_ops_t`（operations/callbacks interface），
  UI 与组件不直接读注册表/环境/系统主题/DPI/消息框。
- 可验证：无头离屏渲染 `--ui-shot` 产出 PNG + 命中/布局/事件断言，
  为 GUI-only 的壳 UI 提供自动化判据（roadmap P2 的缺口）。

**不做**

- 不抽 GDI/窗口原语（`CreateWindowEx`、`MoveWindow`、`GetClientRect`）；
  shell 就是 Win32 壳，抽象这些只会增加噪音。
- 不改引擎/控件 API，不动 `--probe` 的语义、退出码、日志通道。
- 不做标签拖拽排序、多窗口标签迁移、下载面板重做。

## 3 分层

```
┌──────────────────────────────────────────────────────────────┐
│ main.c  壳逻辑：标签/视图/会话/单实例/探针/快捷键             │
│   ├─ syncChrome()   ShellWindow → UiChromeModel（拷贝快照）   │
│   ├─ WM_* → uiChromeMouse/Layout/Paint                       │
│   └─ 回调 ← uiChromeCallbacks（命令 ID 与现有 WM_COMMAND 同源）│
├──────────────────────────────────────────────────────────────┤
│ ui_chrome.c  组件：UiToolbar / UiFindBar / UiStatusBar        │
│ ui_widgets.c 组件：UiButton / UiTabStrip                      │
│ ui_menu.c    自绘菜单：模型/布局/绘制/命中/键盘/子菜单        │
│ ui_menu_host.c 菜单宿主：分层弹出窗口 + 鼠标捕获 + 模态循环   │
├──────────────────────────────────────────────────────────────┤
│ ui_theme.c   主题 token、字体、DPI 缩放、GDI+ painter、图标    │
├──────────────────────────────────────────────────────────────┤
│ shell_ops.c  shell_ops_t：Win32 默认实现 + mock 实现           │
└──────────────────────────────────────────────────────────────┘
```

`UiChromeModel` 是**值拷贝**快照（标签标题/URL/状态/进度/缩放/查找词），
组件不认识 `ShellWindow`，因此离屏测试可以喂任意模型；`main.c` 的
`syncChrome()` 是唯一写模型的地方。

## 4 `shell_ops_t` 契约

`examples/browser_shell/shell_ops.h`：

| 成员 | 默认 Win32 实现 | 用途 |
|---|---|---|
| `get_env` | `getenv`（保持 `--userdata` 用 `putenv` 写的 CRT 环境可见） | profile / 主题 / 调试开关 |
| `profile_root` | `AGB_USERDATA` → `%APPDATA%\AgaveShell` → `%LOCALAPPDATA%\AgaveShellData` → exe 旁 | 书签/历史/会话根 |
| `downloads_dir` | `%USERPROFILE%\Downloads` | 下载面板落点 |
| `system_is_dark` | `HKCU\…\Themes\Personalize\AppsUseLightTheme==0` | chrome 暗色主题 |
| `dpi_for_window` | `GetDpiForWindow`（动态解析；回退 `GetDeviceCaps`） | 每窗口 DPI |
| `enable_dpi_awareness` | `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`（回退 `SetProcessDPIAware`） | 启动时一次性 |
| `set_dark_titlebar` | `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)`（动态） | 暗色标题栏 |
| `set_window_border` | `DwmSetWindowAttribute(DWMWA_BORDER_COLOR=34)`（Win11 22000+，返回是否接受） | 圆角窗体 1px 外框跟随主题色 |
| `message_box` | `MessageBoxA` + flags 映射 | `alert/confirm` |
| `tick_ms` | `GetTickCount64` | 看门狗提示/测量 |

选择：`AGB_SHELL_OPS=mock` 或测试里 `shellOpsSet()`。mock 不碰注册表/磁盘，
主题/DPI 由参数决定，消息框只打日志——离屏 UI 测试因此可确定复现。

**不放进 ops 的东西**：`fopen`/`FILE*`（CRT 已可移植）、GDI 绘制、窗口创建。
它们要么跨平台，要么本来就是 shell 的 Win32 实现。

## 5 组件与布局

| 组件 | 边界 | 关键交互 |
|---|---|---|
| `UiButton` | 32×32@96，圆形 hover 底（Chrome 工具栏），图标居中，`enabled/toggled` | 命中 → `id` |
| `UiTabStrip` | 标签条 + `+`；活动标签与工具栏同色、上圆角，非活动标签 hover 用 `tab_hover` | 命中标签/关闭/新标签 |
| `UiToolbar` | 后退/前进/刷新-停止/地址胶囊/Go/菜单 | 命令 ID 与旧按钮完全一致 |
| `UiFindBar` | 右对齐浮条：搜索图标 + 输入 + 上/下/关闭 | `ID_FIND_*` |
| `UiStatusBar` | 24px 状态栏 + 进度线 + 缩放胶囊 | 纯绘制 |
| `UiChrome` | 组合上述 + 内容区矩形 + 原生 EDIT 位置 | `uiChromeLayout/Paint/Mouse` |
| `UiMenu` | Chrome 风格圆角面板：30px 行、可选图标列、右对齐快捷键、子菜单、禁用/分隔行 | hover/键盘/点击；`uiMenuSessionMove/Key/Click` |
| `UiMenuHost` | 32bppPARGB 分层弹出窗口（自绘阴影/圆角），鼠标捕获 + 模态循环 | 点外关闭、Esc、Alt-Tab；返回命令 id |
| 窗口按钮 | 标签条右侧 `_ □ X`，与 tab 同一行 | `HTMINBUTTON/HTMAXBUTTON/HTCLOSE` + NC 按下拦截 |

几何（96 DPI 基准，全部经 `scale = dpi/96`）：

```
tabStripH 40   toolbarH 44   findH 40   statusH 24
tab: maxW 220   h 32   gap 2   radius 8
window buttons: 46 x tabStripH, right-aligned
address: h 32  radius 16  go 28×28
content: y = tabStripH + toolbarH + (findVisible ? findH : 0)
         h = windowH - y - statusH
```

**标题栏就是标签条**：`WM_NCCALCSIZE` 把客户区顶到窗口顶边（只保留左/右/下 resize
border），`WM_NCHITTEST` 把标签条空白处当 `HTCAPTION`（拖动、双击最大化、右键系统菜单）。
标签和右侧窗口按钮占同一行；tab 的可用宽度先扣掉 `3 * 46px` 的窗口按钮区。

标签宽度随窗口自适应：`slot = (W - pad*2 - plusW - windowButtons)/n`，上限 220px，
**没有最小宽度下限**——不够时继续压缩，标题省略、favicon/关闭按钮按宽度隐藏；
`+` 永远排在最后一个 tab 与窗口按钮之间，不会压到按钮下面。

### 5.1 窗口按钮：Chromium 的 CUSTOM_DRAWN 做法

我们最初试过“让 OS 自己画”：Win11 + DWM 实测三条路都不行——

1. 原生标题栏 + `WM_NCPAINT` 叠加：`PrintWindow` 能看到，但真实屏幕像素仍是系统
   caption 颜色（`WM_NCPAINT` 的绘制在 DWM 下不合成）；
2. `WM_NCCALCSIZE` 把客户区顶到 0：原生按钮直接消失；
3. `DwmExtendFrameIntoClientArea` 顶边 40px：DWM 把原生按钮画在客户区上方，但
   同一块区域盖住我们的 tab。

Chromium 源码给出实际做法（`chrome/browser/ui/views/frame/` 与
`ui/views/win/hwnd_message_handler.cc`）：

- `BrowserCaptionButtonContainer` 里是四个自绘 `WindowsCaptionButton`
  （min/max/restore/close）；`NonClientHitTest` 返回
  `HTMINBUTTON/HTMAXBUTTON/HTCLOSE`。
- `HWNDMessageHandler::OnNCLButtonDown` 在 `FrameMode::CUSTOM_DRAWN` 下**不调用
  DefWindowProc**：把 NC 按下转成 client 按下并 `SetCapture`，等 `WM_LBUTTONUP`
  走按钮自己的回调。源码注释写明原因——交给 DefWindowProc 会在 classic 样式里
  **再画一个按下态按钮盖在 view 上**（正是我们截图里的偏移白色方块）。
- `DwmDefWindowProc` 只在系统画框架（`HasSystemFrame()`）时调用；CUSTOM_DRAWN 不调。

本仓库对应实现：

```
uiChrome.win_min / win_max / win_close   （自绘 glyph + hover/pressed）
WM_NCHITTEST          → HTMINBUTTON / HTMAXBUTTON / HTCLOSE
WM_NCLBUTTONDOWN      → uiChromeMouse(DOWN) + SetCapture，return 0
WM_LBUTTONUP          → uiChromeMouse(UP) → UiChromeCallbacks.command
WM_NCMOUSEMOVE/LEAVE  → 只更新 hover
```

`HTMAXBUTTON` 保留 Win11 Snap Layouts 的命中语义；点击本身由我们自己的按钮状态机
完成，不会再出现系统 classic 按下态。**原生 OS 画按钮与“tab 同一行”在纯 Win32/GDI
下不可兼得**；Chrome 也是自绘，这是对齐 Chrome 的唯一可行路径。

**窗口外框也要跟主题走**：Win11 会给圆角顶层窗口画一圈 1px 边框，这圈边框默认
跟系统 app 模式（浅色）而不是我们的 chrome 主题，所以深色窗口最外圈会露出一条
白边（左上圆角尤其明显）。`applyWindowFrame()` 在窗口创建、换主题、`WM_DPICHANGED`
时调用 `set_window_border`，暗色传 `theme->chrome`（`#1F2020`）、亮色传
`theme->border`；属性不被系统接受（Win10）时只是保持系统默认，不影响其他行为。

原生控件只有两个 `EDIT`（地址栏、查找框），无边框、按主题设置
`WM_CTLCOLOREDIT` 背景/文字色；它们在 chrome 里只是两个矩形，离屏模式下不创建。

### 5.2 自绘菜单：Chrome 的 context menu 形状

`TrackPopupMenu` 在 Win32 下只能画系统菜单（亮色、直角、无图标、ANSI 标签，
中文曾不得不走 `AppendMenuW`），和 Chrome 的圆角深色菜单完全对不上。现在的做法：

- **模型与宿主分离**。`ui_menu.c` 只有值语义的 `UiMenu`（行/快捷键/图标/禁用/
  子菜单）和 `UiMenuSession`（面板、可见性、hover/键盘）：`uiMenuSessionBegin`
  一次算好根面板与所有后代面板的位置（屏幕坐标，贴边时反向翻转），
  `uiMenuSessionMove/Key/Click` 只改状态，`uiMenuSessionPaint` 用 `UiPainter`
  画。因此 `--ui-shot --ui-menu` 能在 DIB 里复现击中/键盘/像素，不需要窗口。
- **分层弹窗**（`ui_menu_host.c`）：窗口覆盖所有可见面板的并集 + 16px 阴影边距，
  像素来自 32bppPARGB 的 GDI+ 位图（`GdipCreateBitmapFromScan0` 直接指向 DIB），
  圆角、阴影、文字抗锯齿的 alpha 都保留，面板之间是真正的透明。面板以外填
  alpha=1 的黑，不改变观感但让整块弹窗接住鼠标——点外面只会关菜单，不会穿透
  到页面。窗口是 `WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`。
- **模态语义**：进入时 `SetCapture` + `SetFocus`，跑自己的 `GetMessage` 循环
  （和 `TrackPopupMenu` 一样），Esc 关、点行选、点外关、滚轮/右键关、Alt-Tab 关；
  结束后把焦点还给打开前的控件。`WM_KILLFOCUS` **不**作为关闭条件：弹窗是
  `SW_SHOWNOACTIVATE`，隐藏窗口的测试里也拿不到焦点，靠 capture 已经足够。
- **行模型**：分隔行、禁用行不参与 hover/键盘选择；子菜单在 hover 父项时立即
  打开（无需延时），键盘 Right/Left 进出；面板有图标时整列缩进（Chrome 的 Lens
  行就是这个效果）。应用菜单锚在 `☰` 按钮正下方、右边缘对齐；页面右键锚在
  指针处，靠近屏幕边缘时向左/上翻转，并夹在显示器 work area 内。
- **不做假的可用性**：引擎没有的能力（Lens、查看源代码、打印/另存）保持禁用并
  在标签里写 `(not implemented)`，与 R75 的“死项要说明”约定一致。

## 6 主题 token

浅色（Chrome 2023 风格）：tab strip `#DEE1E6`、toolbar `#FFFFFF`、
活动标签 `#FFFFFF`、非活动标签 hover `#D3D6DB`、地址胶囊 `#F1F3F4`（focus 时
`#FFFFFF` + 2px `#1A73E8` 焦点环）、工具栏图标 hover `#F1F3F4`、文字 `#202124`、
次要 `#5F6368`、边框 `#DADCE0`、强调 `#1A73E8`、窗口关闭 hover `#E81123`。

暗色（对齐 Chrome 无痕窗口的实际截图，2026-11-11）：frame/tab strip `#1F2020`、
toolbar/活动标签/content `#3C3C3C`、地址胶囊 `#282828`（focus 时 `#3C3C3C`）、
非活动标签 hover `#474747`、工具栏图标 hover `#505050`、焦点环 `#A8C7FA`、
文字 `#E8EAED`、次要 `#9AA0A6`、图标 `#C4C7C5`、边框 `#373737`、状态栏 `#1F1F1F`、
窗口关闭 hover `#C42B1C`。**层次关系**是这组值的重点：strip 比 toolbar 暗、胶囊比
toolbar 更暗；`--ui-shot` 用亮度关系断言它们（`dark-strip-darker` /
`dark-pill-darker` / `dark-hover-lighter`）。

自绘菜单 token：面板 `#1F1F1F`（浅色 `#FFFFFF`）、hover 行 `#3C4043`
（浅色 `#F1F3F4`）、分隔线/边框 `#373737`（浅色 `#E8EAED`/`#DADCE0`）、
禁用文字 `#7A7A7A`、快捷键 `#9AA0A6`、图标 `#C8C8C8`、阴影 `0x66000000`。
菜单几何 @96：行高 30、分隔行 9、左右 padding 12、上下 padding 8、面板圆角 8、
行高亮圆角 4、图标列 26、宽度 clamp 180..460。

`AGB_SHELL_THEME=light|dark|auto`（默认 `auto`，只影响 chrome，不改页面
`prefers-color-scheme`；页面仍由 `AGB_COLOR_SCHEME` 控制）。

字体：Segoe UI（回退 Microsoft YaHei UI / Arial），GDI+ 抗锯齿；
标签 13px、地址 14px、状态 11px（@96，随 DPI 缩放）。

## 7 无头 UI 验证（`--ui-shot`）

```powershell
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-light.png --ui-theme light
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-dark.png  --ui-theme dark
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-150.png   --ui-theme light --ui-dpi 144
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-menu.png  --ui-theme dark --ui-menu
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-focus.png --ui-theme dark --ui-focus
```

不建窗口、不启动引擎、不读 profile：创建 32bpp 离屏 DIB → 填样例模型
（5 个标签、进度 42%、查找栏展开、缩放 110%）→ `uiChromeLayout/Paint` →
自己写 PNG（复用探针的 `probeWritePng`）。`--ui-size 1100x760` 可换窗口尺寸。
`--ui-menu` 额外在菜单按钮下画一份 Chrome 形状的应用菜单（Bookmarks 子菜单
打开、hover 停在 Bookmarks、Reload 按钮显示圆形 hover），`--ui-focus` 把地址
胶囊置为聚焦态。

同一进程内断言（失败 exit 5，全部打 `ui-* ok=1/0`）：

1. 命中：标签/关闭/新标签/后退/菜单/地址胶囊/查找按钮/窗口 min-max-close；
2. 事件：DOWN+UP 触发 `ID_BACK`、切换/关闭/新标签、最大化/关闭窗口回调、hover 更新；
3. 布局：按钮在客户区内、地址胶囊宽度 > 120、内容区高度 > 80、标签不重叠；
4. 像素：活动标签 = surface、工具栏 = toolbar、状态栏 = surface_alt、内容区 = window；
5. 150% DPI：tabstrip 60 / toolbar 66 / content top = 126，菜单仍在窗口内；
6. 暗色层次：strip < toolbar、胶囊 < toolbar、hover > 活动标签、焦点环 =
   `#A8C7FA`；
7. `--ui-menu`（61 项）：菜单宽度/锚定、子菜单打开与反向翻转、行/分隔行命中、
   Down 跳过分隔行与禁用行、Right/Left 进出子菜单、点击选中/点外取消、
   hover/背景像素、圆形工具栏 hover；
8. `--ui-focus`（43 项）：焦点字段、胶囊填充色、焦点环左右/上边像素与 token。

回归 `tools/run_shell_ui_tests.ps1`：亮/暗/亮-150%/暗-菜单/暗-焦点五份并行，
实测约 **1.5 s**（单份 ~1.2–1.5 s，含进程启动）。
**分层弹出窗口本身**（真实 HWND、鼠标捕获、模态循环、点外关闭）不在离屏测试里：
`pwsh -File tools/check_shell_menu.ps1` 用 `AGB_SHELL_HIDDEN=1` 起真实壳，点菜单
按钮/发右键，断言弹出窗口存在（WS_EX_LAYERED/TOOLWINDOW）、首行点击生效、
Esc 关闭上下文菜单，然后 `WM_CLOSE` 退出。这是手动检查，不进默认套件。

## 8 兼容性

- 命令 ID（`ID_BACK`…`ID_FIND_CLOSE`）不变，日志来源字符串沿用
  `button`/`enter`/`ctrl+r` 等，`docs/dev-debugging.md` 的判据继续有效。
- `--probe` 路径不建 chrome、不初始化 GDI+；探针退出码/回调/线程检查不变。
- **DPI awareness 只在 GUI 分支调用**。探针/`--ui-shot` 保持 DPI-unaware：`send_ui_events.ps1`
  是从另一个（DPI-unaware）PowerShell 进程 `PostMessage` 的，目标进程一旦改成
  Per-Monitor V2，`WM_RBUTTONDOWN` 的坐标会被系统缩放（120,140 → 150,175），
  inspector 套件断言的是前者。
- 单实例接力的隐藏窗口标题、`WM_COPYDATA` 协议、session/bookmarks 文件格式不变。
- 会话恢复仍是“一个窗口 + 所有标签”（[design-shell-singleton.md](design-shell-singleton.md)）。

## 9 验收

- `tools/build_project.ps1` 增量构建 0 error / 0 warning；
- `run_shell_ui_tests.ps1` 全绿（亮/暗/亮-150%/暗-菜单/暗-焦点五份），
  五张 PNG 目视无错位，暗色层次和焦点环符合 §6；
- `run_all_tests.ps1` 全绿（新增 case 计入并计时）；
- 手工 GUI：125%/150% DPI 下标签/地址栏不重叠，hover/点击正常，
  `AGB_SHELL_THEME=dark` 标题栏与 chrome 一起变暗；点 `☰` / 页面右键看到的
  是自绘菜单（圆角/阴影/快捷键/子菜单），点外面和 Esc 都能关；
- 窗口框架：`pwsh -File tools/check_shell_frame.ps1`（`AGB_SHELL_HIDDEN=1`，不显示窗口）
  → `FRAME=PASS`：client origin y=0、`WM_NCHITTEST` min=8/max=9/close=20、
  tab=1、空白=2，`WM_NCLBUTTONDOWN + WM_LBUTTONUP` 触发最大化/还原、最小化、关闭；
  并检查 `[shell] window frame … applied=1`（Win11 的 DWM 边框色被接受，
  深色窗口不再有白边）。这是**手动检查**，不进默认套件（需要真实 HWND）。
- 菜单宿主：`pwsh -File tools/check_shell_menu.ps1`（`AGB_SHELL_HIDDEN=1`，不显示窗口）
  → `MENU-HOST=PASS`：应用菜单弹窗存在且是 layered/toolwindow、首行点击生效，
  页面右键上下文菜单能打开、Esc 能关，最后 `WM_CLOSE` 干净退出。同样是
  **手动检查**，不进默认套件。
