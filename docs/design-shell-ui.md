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
| `prepare_frame` | `DwmExtendFrameIntoClientArea`（1,1,1,1）+ `DWMWA_WINDOW_CORNER_PREFERENCE=ROUND` + `DWMWA_BORDER_COLOR=NONE`（后两个 Win11 22000+，动态） | 保留阴影/缩放/Snap，同时让客户区盖满窗口、去掉白边 |
| `message_box` | `MessageBoxA` + flags 映射 | `alert/confirm` |
| `tick_ms` | `GetTickCount64` | 看门狗提示/测量 |

选择：`AGB_SHELL_OPS=mock` 或测试里 `shellOpsSet()`。mock 不碰注册表/磁盘，
主题/DPI 由参数决定，消息框只打日志——离屏 UI 测试因此可确定复现。

**不放进 ops 的东西**：`fopen`/`FILE*`（CRT 已可移植）、GDI 绘制、窗口创建。
它们要么跨平台，要么本来就是 shell 的 Win32 实现。

## 5 组件与布局

| 组件 | 边界 | 关键交互 |
|---|---|---|
| `UiButton` | 32×32@96，圆形 hover 底（Chrome 工具栏），图标盒按按钮尺寸算（上限 16px），`enabled/toggled/pressed` | 命中 → `id` |
| `UiTabStrip` | 标签条 + `+`；活动标签削底与工具栏融合，非活动标签 hover/press 是**内缩 pill**；每 tab 默认带关闭 chip（20px 圆 + 16px glyph；背景标签压到不足一半宽度才隐藏，活动标签始终保留）；tab 之间常驻分隔线；中键关闭 | 命中标签/关闭/新标签 |
| `UiToolbar` | 后退/前进/刷新-停止/地址胶囊/Go/菜单 | 命令 ID 与旧按钮完全一致 |
| `UiFindBar` | **内容区右上角的浮层卡片**（搜索图标 + 输入 + 上/下/关闭），页面不重排 | `ID_FIND_*`；Ctrl+F 打开并全选、Esc 从任何焦点处关闭 |
| `UiStatusBar` | 24px 状态栏 + 进度线 + 缩放胶囊 | 纯绘制 |
| `UiChrome` | 组合上述 + 内容区矩形 + 原生 EDIT 位置 | `uiChromeLayout/Paint/Mouse` |
| `UiMenu` | Chrome 风格圆角面板：30px 行、可选图标列、右对齐快捷键、子菜单、禁用/分隔行 | hover/键盘/点击；`uiMenuSessionMove/Key/Click` |
| `UiMenuHost` | 32bppPARGB 分层弹出窗口（自绘圆角/边框，**无阴影**），鼠标捕获 + 模态循环 | 点外关闭、Esc、Alt-Tab；返回命令 id |
| 窗口按钮 | 标签条右侧 `_ □ X`，与 tab 同一行 | `HTMINBUTTON/HTMAXBUTTON/HTCLOSE` + NC 按下拦截 |

几何（96 DPI 基准，全部经 `scale = dpi/96`）：

```
tabStripH 40   toolbarH 44   statusH 24
tab: maxW 220   h 32   gap 2   radius 8
window buttons: 46 x tabStripH, right-aligned
address: h 32  radius 16  go 28×28
content: y = tabStripH + toolbarH          （查找栏是浮层，不加高度）
         h = windowH - y - statusH
find overlay: 右上角, w = min(360+24, content.w-2*pad)  h = findFieldH + 8
```

**标题栏就是标签条**：`WM_NCCALCSIZE` 把客户区顶到窗口顶边（只保留左/右/下 resize
border），`WM_NCHITTEST` 把标签条空白处当 `HTCAPTION`（拖动、双击最大化、右键系统菜单）。
标签和右侧窗口按钮占同一行；tab 的可用宽度先扣掉 `3 * 46px` 的窗口按钮区。

标签宽度随窗口自适应：`slot = (W - pad*2 - plusW - windowButtons)/n`，上限 220px，
**没有最小宽度下限**——不够时继续压缩，但**只压标题**：favicon 永远在（Chrome 同），
关闭 chip 按 `w * 2 < tab_max_w`（不足一半自然宽度）隐藏——**活动标签例外，任何宽度
都留着它**（关闭"正在看的这个 tab"不该被迫走菜单）。隐藏时 `close[i]` 是零矩形，
于是"看不见"和"点不到"由同一个数据决定；唯一仍为零矩形的活动标签是比 chip 本身还窄
（`w <= 24+2`）的退化情况。`+` 永远排在最后一个 tab 与窗口按钮之间，不会压到按钮下面。

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

**窗口外框：让 DWM 保留框架，但客户区盖满窗口**。只把 `WM_NCCALCSIZE` 做成
“顶部 0、左右下减边框”会留下非客户区的 1px 白边；正确的组合是：

- `WS_THICKFRAME | WS_CAPTION` 保持不变（否则 DWM 不画阴影/系统动效，Snap 也退化）；
- `DwmExtendFrameIntoClientArea(hwnd, {1,1,1,1})`：把 frame 往客户区extend 1px，
  阴影保留；
- `WM_NCCALCSIZE` 让客户区覆盖**整个 window rect**（最大化时用 monitor work area，
  避免盖任务栏）；
- Win11 圆角 `DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND`；
- `DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE`，不跟主题色，也不让系统 1px 白框露出来；
- `WM_NCHITTEST` 自己按 DPI 缩放后的 8px 热区返回 `HTLEFT/HTTOP/...`，
  空白标题栏 `HTCAPTION`，按钮 `HTMINBUTTON/HTMAXBUTTON/HTCLOSE`；
- `WM_GETMINMAXINFO` 只设最小尺寸，**不要**把 `ptMaxSize/ptMaxPosition` 钉成
  work area：标准最大化会让 window rect 向屏幕外多出 frame 厚度，客户区再取
  work area，DWM 的 1px 外框就被推到屏幕外；钉死后那 1px 会露在顶部成白边。
- 最大化时再 `SetWindowRgn` 把窗口区域裁到 monitor work area（Chromium 的
  `ResetWindowRegion`），把 invisible frame overscan 完全挡在可见表面外；还原时
  清空 region。`WM_SIZE` / `WM_DPICHANGED` 各更新一次。

这套只在 `shell_ops_t::prepare_frame` 里做 OS 调用；**不再强制 caption 的暗色**
（`DWMWA_USE_IMMERSIVE_DARK_MODE` 已移除），边框色也不再取 `theme->chrome/border`，
chrome 自身的亮暗 token 不受影响。

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
- **分层弹窗**（`ui_menu_host.c`）：窗口覆盖所有可见面板的并集 + 2px 边距，
  像素来自 32bppPARGB 的 GDI+ 位图（`GdipCreateBitmapFromScan0` 直接指向 DIB），
  圆角、文字抗锯齿的 alpha 都保留，面板之间是真正的透明。面板以外填
  alpha=1 的黑，不改变观感但让整块弹窗接住鼠标——点外面只会关菜单，不会穿透
  到页面。窗口是 `WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`。
- **没有投影**：整块 surface 每次 hover 变化都要重画，10 层嵌套圆角填充的阴影
  是那次重画里最贵的一项；面板用 1px 边框把边界交代清楚。要恢复就先给阴影做
  位图缓存，别回到"每次 paint 都重新叠 10 层"。
- **模态语义**：进入时 `SetCapture` + `SetFocus`，跑自己的 `GetMessage` 循环
  （和 `TrackPopupMenu` 一样），Esc 关、点行选、点外关、滚轮/中键关、Alt-Tab 关；
  结束后把焦点还给打开前的控件。`WM_KILLFOCUS` **不**作为关闭条件：弹窗是
  `SW_SHOWNOACTIVATE`，隐藏窗口的测试里也拿不到焦点，靠 capture 已经足够。
- **右键 = 关掉并在点击处重开**（页面菜单）：popup 有 capture，第二个右键根本到不了
  页面，早先只是把菜单关掉，体验是 "show + hide"。现在宿主把这次点击的屏幕坐标
  交回（`uiMenuHostRun(..., POINT* reopen_at)`，返回值 `UI_MENU_REOPEN`），调用方把
  `WM_RBUTTONDOWN/UP` 按该坐标重放给 webview，由引擎重新判定 link/image/selection。
  应用菜单没有这个语义（Chrome 同），右键只关。
- **行模型**：分隔行、禁用行不参与 hover/键盘选择；子菜单在 hover 父项时立即
  打开（无需延时），键盘 Right/Left 进出；面板有图标时整列缩进（Chrome 的 Lens
  行就是这个效果）。应用菜单锚在 `☰` 按钮正下方、右边缘对齐；页面右键锚在
  指针处，靠近屏幕边缘时向左/上翻转，并夹在显示器 work area 内。
- **不做假的可用性**：引擎没有的能力（Lens、查看源代码、打印/另存）保持禁用并
  在标签里写 `(not implemented)`，与 R75 的“死项要说明”约定一致。

### 5.3 交互状态、脏矩形与光标

chrome 全部落在 `HTCLIENT` 里（只有标签条空白处是 `HTCAPTION`），所以 Windows
**不会**因为鼠标移动让窗口失效：`WM_MOUSEMOVE` 只更新 `UiChrome` 里的 hover/press
状态时必须由壳自己 `InvalidateRect`，否则 hover 反馈只在别的东西碰巧重绘时闪一下。

- `uiChromeMouse()` 记录"事件前/后命中到哪个 part（以及哪个标签）"，
  用 `partRect()` 把两者并成 `chrome.dirty`；**同一 part 内移动不产生脏矩形**，
  真的一次都不重绘。`uiChromeTakeDirty()` 取走并清空。
- `WM_MOUSEMOVE` / `WM_NCMOUSEMOVE` / `WM_MOUSELEAVE` 只 invalidate 这个矩形；
  按下/抬起（press 可能发生在指针已经离开的 part 上）整窗重绘，次数本来就少。
- 三种状态都有像素：`tab_hover` / `tab_pressed`（内缩 pill，底部留 1~2px 让 strip
  透出来）、`tab_close_hover` / `tab_close_pressed`、`button_hover` /
  `button_pressed`、`window_close_hover`。活动标签按下不额外变色（Chrome 同）。
- 光标：`IDC_IBEAM` 只给地址胶囊与查找框，其余 part 一律 `IDC_ARROW`——chrome 不是
  网页，工具栏按钮不该出现手型。
- **tab 分隔线**（`paintTabSeparators`）：相邻两个 tab 之间画 1px 竖线（高 16px 居中），
  位置在布局 gap 的中间，所以它**始终有自己的一份空间**，不会吃 tab 宽度。两侧任一
  tab 处于"与 strip 融合"状态（活动 / hover / 按下）时整条不画——这就是"活动标签两侧
  不显示、hover 的 tab 两侧不显示"。颜色 `tab_sep`：暗色深灰 `#3C4043`、亮色浅白
  `#F4F6F8`（对 `#DEE1E6` 的 strip 是提亮而不是压暗）。

### 5.4 查找栏浮层

Chrome 的 find bar 浮在内容区右上角，**页面不重排**。本仓库原先把它做成 chrome 里的
一整行（`content_top += findH`），打开查找时整页往下跳 40px，页面高度/滚动位置都变。

Win32 下页面是 shell 的子窗口，想让它"浮在页面上"只能是**另一个子窗口**：

- `AgaveShellFindBar` 子窗口（`WS_CHILD | WS_CLIPSIBLINGS`）放在
  `chrome->findbar.rect`，画一张圆角卡片（`uiChromePaintFindOverlay`），窗口区域用
  `SetWindowRgn` 裁成胶囊，四角露出页面；
- 原生 `EDIT` 仍是 **shell 的子窗口**（不是浮层的子窗口），这样 `WM_CTLCOLOREDIT`
  还发给 shell，主题色/焦点底色不用重写；它被 `SetWindowPos(..., HWND_TOP)` 抬到浮层之上；
- 页面控件带 `WS_CLIPSIBLINGS`（`AgaveWebView.c`）：没有它，页面每次重绘都会盖掉浮层；
- 鼠标事件在浮层窗口里换成 shell 客户区坐标后仍走 `uiChromeMouse`，所以 hover /
  按下 / `ID_FIND_*` 命令与别处共用一套状态机和脏矩形；
- 浮层随 `layoutAll()` 定位（内容区右上角，pad 边距），`Ctrl+F` 打开、`Esc` 关闭
  （Esc 在主消息循环里，任何焦点都能关）。`AGB_SHELL_FIND=1` 让启动即打开，供
  `tools/check_shell_findbar.ps1` 用真实窗口验证 z-order / 不重排 / Esc。

## 6 主题 token

浅色（Chrome 2023 风格）：tab strip `#DEE1E6`、toolbar `#FFFFFF`、
活动标签 `#FFFFFF`、非活动标签 hover `#D3D6DB` / 按下 `#C2C6CC`、tab 分隔线 `#F4F6F8`、
地址胶囊 `#F1F3F4`（focus 时
`#FFFFFF` + 2px `#1A73E8` 焦点环）、工具栏图标 hover `#F1F3F4`、文字 `#202124`、
次要 `#5F6368`、边框 `#DADCE0`、强调 `#1A73E8`、窗口关闭 hover `#E81123`。

暗色（对齐 Chrome 无痕窗口的实际截图，2026-11-11）：frame/tab strip `#1F2020`、
toolbar/活动标签/content `#3C3C3C`、地址胶囊 `#282828`（focus 时 `#3C3C3C`）、
非活动标签 hover `#474747` / 按下 `#505050`、tab 分隔线 `#3C4043`、
工具栏图标 hover `#505050`、焦点环 `#A8C7FA`、
文字 `#E8EAED`、次要 `#9AA0A6`、图标 `#C4C7C5`、边框 `#373737`、状态栏 `#1F1F1F`、
窗口关闭 hover `#C42B1C`。**层次关系**是这组值的重点：strip 比 toolbar 暗、胶囊比
toolbar 更暗；`--ui-shot` 用亮度关系断言它们（`dark-strip-darker` /
`dark-pill-darker` / `dark-hover-lighter`）。

自绘菜单 token：面板 `#1F1F1F`（浅色 `#FFFFFF`）、hover 行 `#3C4043`
（浅色 `#F1F3F4`）、分隔线/边框 `#373737`（浅色 `#E8EAED`/`#DADCE0`）、
禁用文字 `#7A7A7A`、快捷键 `#9AA0A6`、图标 `#C8C8C8`。菜单**不画投影**
（`menu_shadow` token 保留但不再使用，理由见 5.2）。
菜单几何 @96：行高 30、分隔行 9、左右 padding 12、上下 padding 8、面板圆角 8、
行高亮圆角 4、图标列 26、宽度 clamp 180..460。

`AGB_SHELL_THEME=light|dark|auto`（默认 `auto`，只影响 chrome，不改页面
`prefers-color-scheme`；页面仍由 `AGB_COLOR_SCHEME` 控制）。

字体：**一个 family 同时覆盖中英**——按 `Microsoft YaHei UI → Microsoft YaHei →
Noto Sans SC → Segoe UI → Arial` 取第一个装得上的。GDI+ 的 `DrawString` 不做跨
脚本 family 链接，指定 Segoe UI 画中文会掉到系统替换字体，同一行里英文一个样、
中文另一个样；YaHei 和随构建拷贝的 Noto Sans SC 都是全字库。`uiSpaceInit()` 还会
私有注册 `<exe>\fonts\*.ttf`（和引擎 `FontCachePS.cpp` 同一批文件），所以
`--ui-shot`（不启引擎）和没装中文字体的系统也一致。GDI（`EDIT` 控件）与 GDI+ 都用
灰度抗锯齿（`ANTIALIASED_QUALITY` / `AntiAliasGridFit`）。
标签 13px、地址 14px、状态 11px（@96，随 DPI 缩放）。

## 7 无头 UI 验证（`--ui-shot`）

```powershell
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-light.png --ui-theme light
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-dark.png  --ui-theme dark
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-150.png   --ui-theme light --ui-dpi 144 --ui-size 1650x1140
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-menu.png  --ui-theme dark --ui-menu
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-focus.png --ui-theme dark --ui-focus
```

`--ui-size` 是**设备像素**：高 DPI 要配同样逻辑尺寸（1100×760@96 ≈ 1650×1140@144），
否则拿到的是一个很窄的逻辑窗口（tab 会合理地隐藏关闭 chip），断言口径就变了。

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
5. 交互状态（5.3）：hover 到非活动标签产生的脏矩形落在标签条内且覆盖该标签、
   同一 part 内移动**不产生**脏矩形、hover pill 与底部透出的 strip 像素、
   按下 = `tab_pressed`、press 整窗脏、关闭 chip 20px 圆与 16px glyph（对角线像素）、
   `IDC_IBEAM` 的两个 part；
6. 标签条（5）：每个 tab 默认有 chip（按"w 是否过半"的规则断言，任意窗口尺寸都成立）、
   **活动标签任何宽度都有 chip**、tab 间分隔线 = `tab_sep`、活动标签两侧与 hover
   标签两侧的分隔线 = strip 色、压缩后背景标签 chip 为零矩形且 favicon 仍在；
7. 150% DPI：tabstrip 60 / toolbar 66 / content top = **126**（查找栏不再占一行），
   菜单仍在窗口内；
8. 暗色层次：strip < toolbar、胶囊 < toolbar、hover > 活动标签、焦点环 =
   `#A8C7FA`；
9. 查找栏浮层（5.4）：卡片在内容区内、右上角对齐、EDIT 在卡片内、卡片在工具栏下方，
   并把卡片画进独立 DIB 校验底色；
10. `--ui-menu`（87 项）：菜单宽度/锚定、子菜单打开与反向翻转、行/分隔行命中、
    Down 跳过分隔行与禁用行、Right/Left 进出子菜单、点击选中/点外取消、
    hover/背景像素、圆形工具栏 hover；
11. `--ui-focus`（69 项）：焦点字段、胶囊填充色、焦点环左右/上边像素与 token。

各项计数（2026-11-11）：亮 60 / 暗 64 / 亮-150% 60 / 暗-菜单 87 / 暗-焦点 69。
窄窗口（如 `--ui-size 700x460`，背景标签不足一半宽度）会少 2 条 chip 像素断言：
那里背景标签本来就没有 chip（活动标签的 chip 仍在，命中/点击断言照常跑）。

回归 `tools/run_shell_ui_tests.ps1`：亮/暗/亮-150%/暗-菜单/暗-焦点五份并行，
实测约 **1.5 s**（单份 ~1.2–1.5 s，含进程启动）。
**分层弹出窗口本身**（真实 HWND、鼠标捕获、模态循环、点外关闭）不在离屏测试里：
`pwsh -File tools/check_shell_menu.ps1` 用 `AGB_SHELL_HIDDEN=1` 起真实壳，点菜单
按钮/发右键，断言弹出窗口存在（WS_EX_LAYERED/TOOLWINDOW）、首行点击生效、
右键关掉并在点击处重开（`UI_MENU_REOPEN` + 重放点击）、Esc 关闭上下文菜单，
然后 `WM_CLOSE` 退出。
**查找栏浮层**同理：`pwsh -File tools/check_shell_findbar.ps1`（`AGB_SHELL_FIND=1`）
断言浮层是 shell 的子窗口、EDIT 在浮层内且 z-order 在其上、页面矩形没被压小、
Esc 关掉。两者都是手动检查，不进默认套件；它们都按 pid 找窗口，开发者自己开着的
shell 不会被脚本动。

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
  并检查 `[shell] window frame extended=1`（DWM 1px frame 扩展成功）。这是
  **手动检查**，不进默认套件（需要真实 HWND）。
- 菜单宿主：`pwsh -File tools/check_shell_menu.ps1`（`AGB_SHELL_HIDDEN=1`，不显示窗口）
  → `MENU-HOST=PASS`：应用菜单弹窗存在且是 layered/toolwindow、首行点击生效，
  页面右键上下文菜单能打开、Esc 能关，最后 `WM_CLOSE` 干净退出。同样是
  **手动检查**，不进默认套件。
