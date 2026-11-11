# Agave BrowserShell

桌面浏览器外壳，**基于 `agave_webview` 控件构建**：每个标签页就是一个
`AgaveWebView` 子窗口，shell 本身只负责 UI（标签栏、工具栏、地址栏、
查找栏、状态栏、书签/历史/会话持久化）。

引擎线程、渲染缓冲、输入转发、缩放、popup 都在控件内部；
shell 不直接调用任何 `macross_*` 接口 —— 它和第三方宿主用的是同一套 API。

```
+--------------------------------------------------------------------+
| [1 标题] [2 标题] ... [+]                    [空][_][□][X]         |
+--------------------------------------------------------------------+
| <  >  R/X  [ 锁 地址栏..................................... > ] ☰  |
+--------------------------------------------------------------------+
|                                            +---------------------+ |
|                                            | 查找栏（浮层，Ctrl+F）| |
|                    网页（引擎直接画进 DIB） | 不占页面空间，Esc 关闭 | |
|                                            +---------------------+ |
+--------------------------------------------------------------------+
| 状态栏：加载进度 / 悬停链接 / 错误 / 下载路径 / 缩放               |
+--------------------------------------------------------------------+
```

**标题栏就是标签条**：窗口客户区从左上角开始（没有原生 title bar）；标签条右侧
预留三个窗口按钮的位置，`_ □ X` 由 chrome 自绘。拖动空白区移动窗口、双击最大化、
右键系统菜单走 `HTCAPTION`；按钮区给系统 `HTMINBUTTON/HTMAXBUTTON/HTCLOSE`，
但 `WM_NCLBUTTONDOWN` 被拦截成客户区按下（Chromium `CUSTOM_DRAWN` 同路径），
避免系统再画一套 classic 按下态按钮。标签宽度不足时自动压缩（上限 220px，
无最小宽度；标题省略、favicon/关闭按宽度隐藏），永远排不进右侧按钮区。

窗口框架保留 `WS_THICKFRAME | WS_CAPTION`（DWM 阴影/缩放/Snap 语义），
`WM_NCCALCSIZE` 让客户区覆盖**整个 window rect**，`DwmExtendFrameIntoClientArea`
只扩 1px 进客户区保阴影；Win11 用 `DWMWC_ROUND` 圆角 + `DWMWA_BORDER_COLOR=NONE`，
最大化走系统默认 overscan（frame 在屏幕外），客户区在 `WM_NCCALCSIZE` 里取
monitor work area，并用 `SetWindowRgn` 把窗口区域裁到 work area（避免最大化
顶部 1px 白边）。不再强制 caption 的暗色，也不把 DWM 边框染成主题色。

## 构建

```
cmake -S . -B build-zig-native -DOPT_JS_ANT=ON -DOPT_NET_BACKEND=native -DOPT_SHELL=ON
cmake --build build-zig-native --target agave_shell
```

产物 `build-zig-native/agave_shell.exe`，需要同目录有 `libagave.dll` 和 `antjsb.dll`。

## 线程模型

UI 线程只负责窗口/消息循环；引擎线程、命令队列和回调线程契约由控件拥有，
完整说明见 [arch-embedding.md](../../docs/arch-embedding.md)。

* 输入、导航、缩放等都进一个加锁的命令队列（`ShellCommand`），UI 线程**从不**直接调用引擎 API；
* 引擎回调只把标题/URL/进度写进对应标签的字段，然后 `PostMessage(WM_SHELL_STATE)`；
* **DIB 归引擎线程所有**：UI 只记录"想要多大"，由引擎线程创建/重建缓冲区并调用
  `macross_view_resize`。否则 UI 释放缓冲区时引擎可能还在往里画（会黑屏/崩溃）。

实测：163.com 加载期间 UI 线程的 `WM_NULL` 响应在 0.1~14 ms，不再卡死。

## UI 组件与平台 ops

UI 不再由 `main.c` 里的一排原生 `BUTTON` 拼出来；`main.c` 只保留标签/会话/
单实例/探针和胶水：

| 文件 | 职责 |
|---|---|
| `shell_ops.[ch]` | `shell_ops_t`（operations/callbacks interface）：profile 路径、下载目录、系统暗色、DPI、暗色标题栏、消息框、tick。Win32 默认实现 + mock（`AGB_SHELL_OPS=mock`） |
| `ui_theme.[ch]` | 亮/暗 token、DPI 缩放、中英同族字体（YaHei/Noto，私有注册 `fonts/`）、GDI+ painter、矢量图标 |
| `ui_widgets.[ch]` | `UiButton`、`UiTabStrip`（含 favicon chip、hover、关闭按钮、宽度自适应） |
| `ui_chrome.[ch]` | 工具栏/地址胶囊/查找栏/状态栏/窗口按钮的组合、布局、绘制、命中与鼠标状态机 |

主题：`AGB_SHELL_THEME=light|dark|auto`（默认 auto，只影响 chrome，不改页面的
`prefers-color-scheme`）；DPI 用 Per-Monitor V2，`WM_DPICHANGED` 重建字体和度量。
窗口按钮按 Chromium 的 `CUSTOM_DRAWN` 方式：自绘 glyph、`WM_NCHITTEST` 返回
`HTMINBUTTON/HTMAXBUTTON/HTCLOSE`、`WM_NCLBUTTONDOWN` 转成客户区按下 + capture。
完整设计见 [design-shell-ui.md](../../docs/design-shell-ui.md)。

无头验证（不建窗口/引擎/profile，只画离屏 DIB）：

```powershell
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-light.png --ui-theme light
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-dark.png  --ui-theme dark
build-zig-native\agave_shell.exe --ui-shot build-zig-native\ui-150.png   --ui-dpi 144 --ui-size 1650x1140
pwsh -File tools\run_shell_ui_tests.ps1
pwsh -File tools\check_shell_frame.ps1   # 手动：隐藏窗口 + 真实 WM_NCCALCSIZE/WM_NCHITTEST/NC 点击
pwsh -File tools\check_shell_menu.ps1    # 手动：真实弹出菜单（Esc/右键重开/点行选中）
pwsh -File tools\check_shell_findbar.ps1 # 手动：查找栏浮层（不占页面空间、z-order、Esc）
```

## 功能

| 操作 | 说明 |
|---|---|
| 地址栏 + Enter | 导航；不含点号/协议时按搜索词处理 |
| `+` / Ctrl+T | 新标签页 |
| 点标签 | **按下即切换**（不等抬起）；每个标签默认带 `x`（背景标签压到不足一半宽度才隐藏，**活动标签始终显示**），**中键**点标签也关闭；hover 内缩 pill、按下加深；标签之间有 2px 分隔线，活动/hover 标签两侧不画；活动标签与工具栏交界处是外翻圆角 |
| `<` `>` / Alt+←→ | 后退 / 前进 |
| `R` / F5 | 刷新（加载中同一个按钮变成停止 X） |
| `X` / Esc | 停止加载（Esc 在查找栏打开时先关查找栏；地址栏里 Esc 还原 URL） |
| `_` `□` `X`（标签条右侧） | 最小化 / 最大化-还原 / 关闭窗口；按 Chromium 的 CUSTOM_DRAWN 命中方式交给自己的按钮状态机 |
| Ctrl+L / F6 | 聚焦地址栏 |
| Ctrl+F | 打开查找栏（**浮在页面右上角，页面不重排**）并全选已有关键字；回车执行 `macross_view_search_text`；**Esc 从任何焦点处关闭**（`AGB_SHELL_FIND=1` 启动即打开，供自动化用） |
| 右键菜单 | **自绘** Chrome 风格菜单（圆角/阴影/右对齐快捷键/禁用行）：后退/前进/刷新、链接操作、复制/全选、书签、注入调试脚本；靠近屏幕边缘会自动翻转 |
| 滚轮、点击、键盘 | 全部转发给页面（中文经 `WM_CHAR` 转 UTF-8） |
| 下载 | 引擎 DownloadManager 写盘，状态栏显示进度/结果；保存到 `%USERPROFILE%\Downloads` |
| Ctrl+D | 加书签（存 `<数据目录>\bookmarks.txt`） |
| `☰` 按钮 / 菜单 | **自绘**应用菜单：书签/历史子菜单（右箭头）、**下载页（Ctrl+J）**、缩放、新建/查找/刷新、注入调试脚本、**设置（暂无页面，开 about:blank）**；打开时按钮保持高亮 |
| 主题 / DPI | `AGB_SHELL_THEME=light\|dark\|auto`；Per-Monitor V2，`WM_DPICHANGED` 自动重排 |
| 右键菜单 → 注入 vConsole | 从 exe 同目录读 `vconsole.min.js` 注入当前页（默认注入目标） |
| 右键菜单 → 注入 inspector.js | 从 exe 同目录读 `inspector.js` 注入当前页：hover 高亮元素 + 信息浮层；在页面上再右键出页内菜单，可复制元素信息（JSON）/选择器/尺寸/文本/HTML |
| 探针 `--inject <name\|path>` | `--probe` 时在 `--eval` 之前注入（`inspector` / `vconsole` / 文件路径） |
| Ctrl+= / Ctrl+- / Ctrl+0 | 放大 / 缩小 / 重置（25% 步进，状态栏显示百分比） |
| 会话恢复 | 退出时把打开的标签写 `session.txt`，下次无参启动自动恢复 |

数据目录优先 `%APPDATA%\AgaveShell`，不可写时退回 exe 同目录的
`AgaveShellData`（便携模式）。

## 注入调试脚本

右键菜单和 `=` 菜单各有两个入口：`注入 vConsole`（默认）和 `注入 inspector.js`。
两者都是**把 exe 同目录下的文件读出来，再 `agave_webview_eval` 到当前页**，所以：

- 改脚本不用重编：改 `build-zig-native\inspector.js`，再点一次菜单即可（等价于 prelude 的热改）；
- 文件缺失时状态栏会提示 `Cannot read ...`，不是静默失败；
- `AGB_INSPECTOR=<path>` 可以换 `inspector.js` 的读取路径（探针/多份副本用）；
- 脚本长度不再受 8192 字节限制：控件队列对超长 payload 走堆分配（`AgwCommand.heapText`）。
- 探针复现：`agave_shell.exe --probe <url> --inject inspector --eval "..."`；
  `--inject inspector|vconsole` 走的就是菜单那两个函数，所以连实例化步骤一起覆盖。
- vConsole 是 UMD：本页 realm 有 prelude 的 AMD `define`，bundle 会注册成模块
  `VConsole` 而不是 `window.VConsole`。外壳把 `require('VConsole')` 的实例化语句
  **拼在同一个 eval 的末尾**（`injectScriptAsset` 的 `suffix`），注册与实例化之间
  不依赖命令队列的时序。

`inspector.js` 的行为：

- hover 时在 `<html>` 下放一个绝对定位的框 + 半透明信息浮层（`z-index` 最大），
  **不参与文档流**；Esc 退出（菜单开着时先关菜单）；再次注入等于关闭。
- 它靠 `elementFromPoint` 命中元素，命中前把自己的浮层 `display:none`
  （本引擎的 HTML 不认 `pointer-events`，否则浮层会挡住 hover）。
- **页内右键菜单**（不是 Win32 菜单，也不是外壳菜单）：右键先 `preventDefault`，引擎的默认 handler
  被跳过，所以外壳的右键菜单不会同时弹出（`[probe] context menu at …` 不会出现）。
  菜单项：`Copy element info`、`Copy selector`、`Copy bounds`、`Copy text`、
  `Copy HTML`、`Select parent`、`Exit inspector`；↑/↓ + Enter 也能选。
- `Copy element info` 是一行 JSON，用来把"是哪个元素、放在哪儿"交给别人（或 agent）：

  ```json
  {"kind":"agave-element","version":1,
   "page":{"url":"https://example.com/","title":"…",
           "viewport":{"width":1100,"height":800},"scroll":{"x":0,"y":100}},
   "element":{"tag":"div","id":"target","class":"","selector":"div#target",
              "text":"target","outerHTML":"<div …>target</div>","inViewport":true,
              "getBoundingClientRect":{"left":100,"top":20,"width":240,"height":80},
              "getBoundingClientRectInPage":{"left":100,"top":120,"width":240,"height":80},
              "style":{"display":"block","position":"absolute","font-size":"14px",…}}}
  ```

  字段名就是取它的函数：`getBoundingClientRect` 是 `getBoundingClientRect()` 的原样返回
  （R82 起是**视口**坐标，截图对得上），`getBoundingClientRectInPage` 是同一个矩形加上
  `page.scroll`（页面坐标，滚动时不变，布局/CSS 对得上）；`selector` 是短 CSS 路径。
- 复制走 `navigator.clipboard.writeText`（prelude polyfill）→ 宿主全局
  `__agbClipboardSetText` → 控件的 `cb_clipboard_set_data`（`CF_UNICODETEXT`，和
  Ctrl+C 同一条路）。本引擎没有 `document.execCommand('copy')`。
- 自动化钩子：`window.__agaveInspector` 暴露 `inspectAt(x,y)` / `menuAt(x,y)` /
  `menuSelect(label)` / `elementInfo()`；回归
  `pwsh -File tools/run_inspector_tests.ps1`（进程内断言 + 真实右键/左键，26 项）。
- 已知限制：监听挂在 `document` 上、按引擎的冒泡顺序执行；如果页面自己在更内层
  `stopPropagation()` 了 `contextmenu`（自定义右键菜单的站点），页内菜单不会出现
  （此时外壳右键菜单也不会被压低）。

## 踩过的坑

1. **地址栏 EN_CHANGE 死循环**：`SetWindowText` 会立刻回调，若在 `WM_COMMAND`
   里导航就无限递归爆栈。地址栏改为子类化，只处理 `VK_RETURN`。
2. **重复创建 view 崩溃**：`WM_SIZE` 与 `WinMain` 各建一次。现在尺寸未变直接返回。
3. **缓冲区所有权**：早期 UI 线程重建 DIB，引擎还在画旧指针 → 内容全黑。
   现在 DIB 只在引擎线程创建/销毁。
4. **新建标签必须走 UI 线程**：标签数组是 UI 状态，命令只负责让引擎建 view。
5. **`WM_NCCALCSIZE` 有两种形态**：带 `NCCALCSIZE_PARAMS`（wParam=TRUE）和不带
   （wParam=FALSE）。只处理 TRUE 时真实窗口仍保留原生标题栏，表现为“标题栏 +
   第一行 chrome”两行。两种都要把客户区顶边设到 `window.top`。
6. **窗口按钮不能交给 DefWindowProc 画按下态**：`WM_NCHITTEST` 对 min/max/close
   返回系统命中码（Chromium 同款，保留 Snap Layouts 语义）之后，必须在
   `WM_NCLBUTTONDOWN` 里转成 client 按下 + `SetCapture`；否则 DefWindowProc 会在
   classic 样式里再画一个偏移的按下态按钮盖住自绘 glyph。Chromium 的
   `HWNDMessageHandler::OnNCLButtonDown` 注释写明了这一点。
7. **加/删 tab 后要重排 chrome**：tab 条槽位矩形只在 `uiChromeLayout` 里更新；
   新建第一个 tab 后若不 `layoutAll()`，绘制和 `WM_NCHITTEST` 都看不到 tab，
   空白区会被当成 `HTCAPTION`（点击变拖窗口）。
8. **`WM_NCPAINT` 叠加在 DWM 下不可见**：Win11 实测真实屏幕仍是系统 caption
   颜色；`PrintWindow` 能看到只是因为走了 WM_PRINT/classic 路径。不要在 NC 里
   画 tab。
9. **白边不是靠改边框颜色解决的**：`DWMWA_BORDER_COLOR` 跟主题色只是把 1px
   系统边框染成相近颜色，换个主题/高对比度还会露。正确组合是保留
   `WS_THICKFRAME|WS_CAPTION` + `WM_NCCALCSIZE` 客户区盖满 + `DwmExtendFrameIntoClientArea
   (1,1,1,1)` 保阴影 + `DWMWA_BORDER_COLOR=NONE`；最大化**不要**钉
   `ptMaxSize`，让系统 overscan 把 frame 推离屏幕。

## 与 examples/touchweb 的关系

`examples/touchweb` 是完整手机浏览器（标签页/历史/收藏/设置/自动填表），
已在 `-DOPT_EXAMPLES=ON` 下能编译通过；它的 UI 是触摸式、按 480x503 设计。
本目录是同一引擎上的桌面外壳。
