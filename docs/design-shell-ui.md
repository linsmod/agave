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
| `message_box` | `MessageBoxA` + flags 映射 | `alert/confirm` |
| `tick_ms` | `GetTickCount64` | 看门狗提示/测量 |

选择：`AGB_SHELL_OPS=mock` 或测试里 `shellOpsSet()`。mock 不碰注册表/磁盘，
主题/DPI 由参数决定，消息框只打日志——离屏 UI 测试因此可确定复现。

**不放进 ops 的东西**：`fopen`/`FILE*`（CRT 已可移植）、GDI 绘制、窗口创建。
它们要么跨平台，要么本来就是 shell 的 Win32 实现。

## 5 组件与布局

| 组件 | 边界 | 关键交互 |
|---|---|---|
| `UiButton` | 32×32@96，圆角 hover 底，图标居中，`enabled/toggled` | 命中 → `id` |
| `UiTabStrip` | 标签条 + `+`；活动标签白底、上圆角；关闭按钮 16px 圆 | 命中标签/关闭/新标签 |
| `UiToolbar` | 后退/前进/刷新-停止/地址胶囊/Go/菜单 | 命令 ID 与旧按钮完全一致 |
| `UiFindBar` | 右对齐浮条：搜索图标 + 输入 + 上/下/关闭 | `ID_FIND_*` |
| `UiStatusBar` | 24px 状态栏 + 进度线 + 缩放胶囊 | 纯绘制 |
| `UiChrome` | 组合上述 + 内容区矩形 + 原生 EDIT 位置 | `uiChromeLayout/Paint/Mouse` |
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

原生控件只有两个 `EDIT`（地址栏、查找框），无边框、按主题设置
`WM_CTLCOLOREDIT` 背景/文字色；它们在 chrome 里只是两个矩形，离屏模式下不创建。

## 6 主题 token

浅色（Chrome 2023 风格）：tab strip `#DEE1E6`、toolbar `#FFFFFF`、
活动标签 `#FFFFFF`、地址胶囊 `#F1F3F4`、文字 `#202124`、次要 `#5F6368`、
边框 `#DADCE0`、强调 `#1A73E8`、危险 `#D93025`。

暗色：strip/toolbar `#202124`/`#292A2D`、胶囊 `#202124`、文字 `#E8EAED`、
次要 `#9AA0A6`、边框 `#5F6368`、强调 `#8AB4F8`。

`AGB_SHELL_THEME=light|dark|auto`（默认 `auto`，只影响 chrome，不改页面
`prefers-color-scheme`；页面仍由 `AGB_COLOR_SCHEME` 控制）。

字体：Segoe UI（回退 Microsoft YaHei UI / Arial），GDI+ 抗锯齿；
标签 13px、地址 14px、状态 11px（@96，随 DPI 缩放）。

## 7 无头 UI 验证（`--ui-shot`）

```powershell
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-light.png --ui-theme light
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-dark.png  --ui-theme dark
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-150.png   --ui-dpi 144
```

不建窗口、不启动引擎、不读 profile：创建 32bpp 离屏 DIB → 填样例模型
（5 个标签、进度 42%、查找栏展开、缩放 110%）→ `uiChromeLayout/Paint` →
自己写 PNG（复用探针的 `probeWritePng`）。`--ui-size 1100x760` 可换窗口尺寸。

同一进程内断言（失败 exit 5，全部打 `ui-* ok=1/0`），共 **34 项**：

1. 命中：标签/关闭/新标签/后退/菜单/地址胶囊/查找按钮/窗口 min-max-close；
2. 事件：DOWN+UP 触发 `ID_BACK`、切换/关闭/新标签、最大化/关闭窗口回调；hover 更新；
3. 布局：按钮在客户区内、地址胶囊宽度 > 120、内容区高度 > 80、标签不重叠；
4. 像素：活动标签 = surface、工具栏 = toolbar、状态栏 = surface_alt、内容区 = window；
5. 150% DPI：tabstrip 60 / toolbar 66 / content top = 126，菜单仍在窗口内。

回归 `tools/run_shell_ui_tests.ps1`：亮/暗/150% 三份并行，实测约 **1 s**（单份 ~0.85 s）。
窗口框架/NC 点击不在离屏测试里（它需要真实 HWND）；用 `AGB_SHELL_HIDDEN=1`
启动后发 `WM_NCHITTEST`/`WM_NCLBUTTONDOWN`/`WM_LBUTTONUP` 手工验证，见 §9。

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

- `tools/build_project.ps1` 增量构建 0 error；
- `run_shell_ui_tests.ps1` 全绿，亮/暗/150% PNG 目视无错位；
- `run_all_tests.ps1` 全绿（新增套件计入并计时）；
- 手工 GUI：125%/150% DPI 下标签/地址栏不重叠，hover/点击正常，
  `AGB_SHELL_THEME=dark` 标题栏与 chrome 一起变暗；
- 窗口框架：`pwsh -File tools/check_shell_frame.ps1`（`AGB_SHELL_HIDDEN=1`，不显示窗口）
  → `FRAME=PASS`：client origin y=0、`WM_NCHITTEST` min=8/max=9/close=20、
  tab=1、空白=2，`WM_NCLBUTTONDOWN + WM_LBUTTONUP` 触发最大化/还原、最小化、关闭。
  这是**手动检查**，不进默认套件（需要真实 HWND）。
