# 调试与定位：探针、默认日志、环境变量、cdb、噪音对照

## 1. 无头探针（主力度量工具）

```powershell
agave_shell.exe --probe <url> [--eval <js>] [--wait ms] [--settle ms] [--timeout ms] [--reload n] [--offline] [--resize WxH] [--zoom percent]
```

- **eval 时机** = 页面 `window load` 事件 + `--wait`；`on_progress finished` 只是长延迟兜底
  （**加载失败也会报 finished**，别拿它当成功判据）。
- `--settle` 从 **eval 脚本返回**开始计时，不是从投递开始：脚本比 settle 慢时探针会等它跑完
  （之前按投递计时，长脚本会在运行中被销毁视图 → `0xC0000005`，见 R61）。
  `--timeout` 仍是硬上限（中途销毁视图是安全的，命令持引用、销毁推迟到引擎线程）。
- 页面 JS 调 `probeExit(result)` 带结果退出；页面 `console.*` 进 stderr（console guard 自动节流重复警告）。
- `--reload n` 语义：eval → reload → 轮询新文档就绪 → 再 eval，共 n+1 轮。
- `--offline`：模块只从磁盘缓存取，未命中立刻失败；先在线跑一次暖缓存，再确定性重放模块图。
- `--keys "<text>"`：eval **之前**把文字打进页面，走真实的 `WM_KEYDOWN → WM_CHAR → WM_KEYUP`
  （PostMessage 到控件窗口，由宿主循环的 `TranslateMessage` 生成字符；一个 tick 一个键，
  保证字符在下一个键之前处理完）。页面自己负责聚焦（`autofocus` / `el.focus()`）。
  `{backspace} {enter} {tab} {esc} {delete} {left} {right} {up} {down} {home} {end}` 表示功能键；
  非 ASCII 直接以 `WM_CHAR` 发送。窗口始终隐藏，不打扰用户（铁律 7）。
  `--pre-eval "<js>"` 在第一个键之前跑一次短脚本（聚焦控件、关掉弹层）：
  `--pre-eval "document.querySelector('input').focus()" --keys "13800138000"` 就能在真实站点上验证输入。
  `--ime "<text>"` 走**输入法**那条路：每个 UTF-16 码元发一条 `WM_IME_CHAR`（星平面字符是两个，
  即代理对），用来复测中文/emoji 输入；`--keys` 则走 `WM_KEYDOWN → TranslateMessage → WM_CHAR`。
  回归：`pwsh -File tools/run_typing_tests.ps1`（8 个用例，页面 `tools/pages/cap_typing.html`）。
- `--click <x,y>`：eval 之前在该视口坐标点一下，走真实的
  `WM_MOUSEMOVE → WM_LBUTTONDOWN → WM_LBUTTONUP`（PostMessage 到控件窗口，和用户点击同一条
  WndProc → 引擎命令路径），一个 tick 一次；可以给多次（`--click 750,228 --click 750,280`
  = 点开 `<select>` 再点第二项）。这是唯一能复测"点开下拉/点中某一项/点空白关闭"的方式
  （`new MouseEvent()` 还是空壳，见 [arch-capability-gaps.md](arch-capability-gaps.md) §1）。
  回归：`tools/pages/form_select.html`（`run_layout_tests.ps1` 的 select 两组）。
- `--resize WxH`：页面 settle 后把控件（连同隐藏的父窗口）改成新尺寸，等 400ms 再跑 eval ——
  复测"窗口变化时页面怎么反应"（重排、百分比、`innerWidth`、`vw/vh`、`@media` 断点、`resize` 事件）。
  这正是拖窗口走的 `WM_SIZE → AGW_CMD_RESIZE → macross_view_resize` 路径，只是窗口从不显示。
  页面与复测命令：`tools/pages/resize/resize_probe.html` +
  `--eval-file tools/pages/resize/check_resize.js`（现状见
  [arch-capability-gaps.md](arch-capability-gaps.md) §2「窗口 resize」）。
- `--zoom <percent>`：eval 前调 `agave_webview_set_zoom`（10..500）。和 `--resize` 可叠加；
  用来复测 zoom 下的视口语义（`100vw == innerWidth`，两者都是 CSS px；R70）。
- URL 参数走**地址栏同一套规则**（`normalizeUrl`），所以可以直接传本地路径 / 文件 URL。

**退出码（先看它，再翻日志）**

| 码 | 含义 |
|---|---|
| 0 | OK（`probeExit` 跑到） |
| 1 | usage（参数错，含 `--probe` 缺 URL） |
| 2 | 超时：页面从未加载完成 |
| 3 | 超时：加载完成但没调 `probeExit` |
| 4 | 没调 `probeExit`（重页面把 `--settle` 调大，或检查 eval 是否抛错） |
| 5 | 加载错误 |
| 6 | 空文档：`body` 无子节点 = 页面从未真正解析 |

**无头语义**：`--probe` 在创建任何窗口之前解析命令行，命中就直奔 `probeRun()`——不建浏览器主窗口、
不建工具栏、不读/写 session/bookmark 文件。唯一的窗口是 `WS_POPUP|WS_EX_TOOLWINDOW` 的**隐藏**父窗口。
stderr 会打一行 `[probe] headless window hidden, AGB_PROBE_SHOW=1 to show (pid <pid>)`。
`--shot <png|bmp>` 抓的是控件自己的 mirror 缓冲（引擎线程每帧写的那份），**不需要**显示窗口。
扩展名决定编码：`.png` 由壳自己用 libpng 写（默认，不用再转图），`.bmp` 是 32 位 bottom-up
原始 DIB（`pixel_check.py` 一直读的那个），其它扩展名直接报错而不是写错格式。
`--shot-rect x,y,w,h` 把捕获裁到一条矩形（**CSS 像素**，原点在视口左上角）——直接抓单个元素：
`--eval` 里 `scrollIntoView()` 后把 `getBoundingClientRect()` 的四个值传进去即可（`--zoom` 下会自动
换算成设备像素，所以文件尺寸是 `rect × zoom`）；超出视口的部分会被裁掉，整块都在视口外则报错退出
（不会留一张看起来像渲染 bug 的空白图）。
--shot 可以和 `--eval`/`--eval-file` 同用：探针先跑脚本、等 `--settle`，再等一帧才抓（否则拿到的是
eval 之前的像素，滚动/改 DOM 后的效果看不到，R82）。典型用法是「脚本给元素画框/上色 → 截图」，
R119 的 SVG 对位就是这么量出来的。
`AGB_PROBE_SHOW=1` 才 ShowWindow，只在调试「窗口可见时行为不同」的路径时才用。
验证"真的没窗口"：`powershell -File tools/check_window_visible.ps1 -ProcessId <pid>`
（exit 0 = 无可见窗口，`-Json` 出明细）。要看真实 GUI：`agave_shell.exe <url>`（不带 `--probe`）。

**profile 隔离**：`--probe` 有自己的 profile 根，默认 `%LOCALAPPDATA%\Agave\probe`
（cookies / localStorage / module + HTTP 缓存 / shell state 全在里面），跑完不会动 GUI 会话的数据；
stderr 的 `[probe] userdata=<路径>` 是实际生效的根。要隔离到别处（并行跑多个探针）：

```powershell
agave_shell.exe --probe <url> --userdata E:\tmp\probe-a ...   # 也可用环境变量 AGB_USERDATA
```

注意 `--offline` 重放用的是**探针自己**的缓存：先在线跑一次暖它，不要指望 GUI 会话的缓存。

**本地测试页**：直接 `file://`（模块管线对 `file:` URL 本地直读，不用起服务）。
需要 http 语义（相对 URL、worker、fetch）时起一个静态服务：

```powershell
Start-Process -WindowStyle Hidden python -ArgumentList '-m','http.server','8971','--directory','e:\mybrowsers\agave\debug'
```

**流式响应 / 入口模块等待期**：`python tools/slow_module_server.py 8974` 再
`--probe http://127.0.0.1:8974/` —— 页面分两块送达（第一块 20 KiB 垫片，让
`WinHttpReadData` 的 16 KiB 缓冲立刻返回），模块脚本后 0.5 s 的第二块含 inline `<script>`，
模块体延迟 1.5 s。复测"解析器等入口模块时到达的数据不能提前 tokenize"（R50）：
修复前 exit 139 + `ASSERTION FAILED: !scriptNode`，修复后 exit 0。

## 2. 默认打开的日志（`[evt]` / `[nav]` / `[ui]` / `[shell]` / `[probe]` / `[flag]`）

这些日志**不需要任何环境变量**：一次运行应该能自己解释发生了什么。全部经由
[`src/webview/AgbLog.h`](../src/webview/AgbLog.h) 的同一个收口函数输出，格式统一为

```
[<进程内相对时间>ms] [通道] 正文
```

时间 = 距**本进程第一条日志**的毫秒数；原点放在按 pid 命名的共享内存区里，所以 libagave、
antjsb.dll、控件 DLL、宿主壳打出来的行在**同一条时间轴**上（各 TU 自己取原点会差 ~15ms，
把相邻事件排反）。

```
[      0.0ms] [shell] window ready hwnd=00000000005C157E
[      0.3ms] [flag] set js=000001EA69D93CD0 on=1 -> 1
[      0.9ms] [ui] resize 800x400
[      1.1ms] [ui] resize 1084x677
[      1.2ms] [ui] focus
[      1.3ms] [shell] entering message loop
[    312.4ms] [nav] open url=https://chat.deepseek.com/
[    312.6ms] [evt] realm start gen=2 engine=... (view context)
[    312.9ms] [nav] request main type=other method=GET url=https://chat.deepseek.com/
[    313.1ms] [evt] bindDocument ... (bound=0) url=about:blank
[    313.4ms] [evt] documentWillUnload doc=... listeners=0
[    313.5ms] [evt] window event pagehide listeners=0
[    313.6ms] [evt] window event unload listeners=0
[    313.8ms] [evt] realm stop gen=2 url=about:blank
[    314.1ms] [evt] realm start gen=3 engine=...
[    400.2ms] [evt] commit https://chat.deepseek.com/
[    401.0ms] [evt] bindDocument ... (bound=1) url=https://chat.deepseek.com/
[    402.3ms] [evt] window event DOMContentLoaded listeners=3
[    455.7ms] [evt] window event load listeners=5
[    900.0ms] [nav] request main type=link method=GET url=https://chat.deepseek.com/sign_in
[    901.2ms] [ui] mouse down left at 320,180
[    901.3ms] [ui] mouse up left at 320,180
[    901.4ms] [ui] wheel up at 320,180
[    901.5ms] [ui] key down vk=0x41
[    901.5ms] [ui] char 'a'
[    901.6ms] [ui] blur
[   2986.8ms] [con-error] Error:
```

| 通道 | 内容 | 关掉 | 更详细 |
|---|---|---|---|
| `[evt]` 生命周期 | commit → realm start/stop → bindDocument → pagehide/unload → DOMContentLoaded → load | `AGB_EVT_LOG=0` | `AGB_EVT_DEBUG=1`（evaluate 深度、listener 记账、JS `dispatchEvent`） |
| `[nav]` 导航动作 | 宿主 `open`/`back`/`forward`/`reload`/`stop`；`request`（type=link/form/back-forward/reload/other + method + URL）；`submit`；`new-window`/`popup create`；`context menu at x,y link=…`（右键，控件侧，**引擎线程**）；`client-redirect`（meta refresh、JS 定时跳转）；`fail provisional`/`fail load`（主资源失败，带 `_mc_error` 码）/`fail resource`（子资源失败）/`cannot implement`（引擎拒绝显示）/`blocked` | `AGB_NAV_LOG=0` | 无（每条只有一行） |
| `[ui]` 界面事件 | resize / focus / blur / mouse down/up / wheel / key down/up / char | `AGB_UI_LOG=0` | `AGB_UI_DEBUG=1` 连 `mouse move` 也打（默认不打，太吵） |
| `[shell]` 宿主 | window ready / 进入与退出消息循环 / `CreateWindowEx` 失败 / `atexit` 模块基址；**用户操作**：`argv …`（启动参数汇总）、`reload (button\|F5\|ctrl+r\|menu\|context menu)`、`back/forward (…) can=0/1`、`stop (…)`、`tab create/activate/close/reopen/switch`（含无空槽、view 创建失败）、`find bar on/off`、`find text='…' forward=N`、`zoom 110% -> 135% (ctrl+plus)`、`alert/confirm/prompt`（含答复）、`bookmark add/duplicate`、`bookmarks loaded: N`、`history loaded: N`、`session restored/saved tabs=N`、`download dir=…`、`context menu … choice=N`（右键，**UI 线程**，菜单关掉后才打）、`clipboard copy link ok=0/1` | — | — |
| `[probe]` 探针 | userdata、headless 状态、加载错误、对话框、退出码 | — | — |
| `[flag]` 引擎开关 | 桥接开关（`uncaught_nonfatal` 等），来自 antjsb.dll | — | — |
| `[style]` 渲染树 | 指定元素的 attach / detach / recalcStyle（是否拿到 renderer、computed display）。**opt-in** | 默认关 | `AGB_STYLE_LOG=1` 打开，`AGB_STYLE_FILTER=<子串>` 按 tag/id/class 过滤 |
| `[cache]` 资源缓存 | 内存缓存 hit/miss、HTTP 缓存 hit/miss、图片 `data`/`notify` 生命周期、绘制时用到的帧状态（`FrameComplete`/`rows=n/m`） | `AGB_CACHE_LOG=0` | — |
| `[dl]` 下载 | `start`/`state`（控制层派发给宿主）/`finish`/`fail`/`cancel`，带 id、字节数、落盘路径 | `AGB_DL_LOG=0` | — |

注意：
- `[ui]` 主要是 GUI 模式的输入（focus/blur/mouse/wheel/key/char）：输入事件在控件窗口过程里产生。
  `--probe` 也会打**控件创建时的 `[ui] resize`**（控件窗口真实存在，只是父窗口 `WS_POPUP` 隐藏）；
  `agave_view_host` 不走控件，所以完全没有 `[ui]`。
- `[ui]` 是**窗口过程里同步打**的（`agbLog` 直接 `fprintf(stderr)`，见 `src/webview/AgbLog.h`），
  在 `agwPost()` 把事件交给引擎线程**之前**；不经过 JS/引擎，所以页面脚本卡死时 `[ui]` 照样出。
- `[ui]` 只记**原始输入**；"用户点了什么"（tab/查找/缩放/书签/对话框/右键菜单）在 `[shell]` 里，
  因为这些动作只发生在 GUI 壳里，探针覆盖不到 —— 出问题时的日志判据就是这两条线拼起来。
- 右键菜单是唯一**跨两个通道两行**的动作：`[nav] context menu`（引擎线程，紧跟 `[ui] mouse up right`）
  → 控制层把数据 `PostMessage` 到 UI 线程 → `[shell] context menu … choice=N`（菜单关闭后才打，
  `choice=0` = 菜单被取消或没打开）。两行都在、只有前一行 = 宿主没接住；两行都没有 = 浏览器没收到右键。
- **刷新/前进后退/停止的触发源只在 `[shell]` 里**：`reload (F5)` / `reload (button)` / `reload (menu)` /
  `reload (context menu)` / `reload (ctrl+r)` 是壳打的，紧跟其后的 `[nav] reload` 是引擎打的 ——
  后者只说"发生了 reload"，前者才回答"用户按的哪一个"。探针的连刷是 `[probe] reload (--reload) round=k of n`
  和 `[probe] reload (hammer) n left`。⚠ **键盘快捷键只能用真键盘触发**（消息循环读 `GetKeyState`，
  `PostMessage` 伪造的按键没有 ctrl/alt 状态），自动化只能走按钮/菜单的 `WM_COMMAND`。
- `[evt]` / `[nav]` 在 `--probe` 里照样有（与窗口无关）。
- **探针的 stdout**（`[probe] running eval`、`[exit] ...`、`[probe] main url=`）也改用同一个时间格式
  （`[ 1234.5ms]`），但它走 stdout，和上面的 stderr 时间线分开重定向。
- `[nav] request` 的 `type=` 来自 `NavigationAction`：`link`=链接默认动作、`form`=表单提交、
  `back-forward`=历史前进/后退、`reload`=重载、`other`=脚本 `location`/meta refresh/宿主打开。
  **重载时 `type=` 可能仍是上一次动作的类型**（WebCore 复用 `triggeringAction`），宿主发起的重载以
  `[nav] reload` 这行为准。
- **301/302 会出现 `[nav] redirect`**：native 后端不再让 WinHTTP 自己跟随
  （`WINHTTP_OPTION_REDIRECT_POLICY_NEVER`），3xx 交给 `FrameLoader::willSendRequest`：先打
  `[nav] redirect from=... to=...`，再按新 URL 打一条 `[nav] request`；地址栏和 `[probe] main url`
  随之变成最终 URL。同一 URL 的自重定向（循环）不会打 redirect 行（`DocumentLoader` 只在 URL
  变化时通知），20 跳后以 `[nav] fail provisional code=-3`（too many redirects）结束。
- 页面自己的 `console.*` 输出由 polyfills 的控制台守卫收口到 **`[con-<level>]`** 通道
  （`__agbConsoleLineEx(level, text)` → `agbLogConsole`），级别就是 tag：
  `[con-log]`/`[con-info]`/`[con-warn]`/`[con-error]`/`[con-debug]`，未知级别回落 `con-log`；
  和 `[evt]`/`[nav]` 一样带 `[ 123.4ms]` 时间前缀、走 stderr；所有级别共用一个开关
  `AGB_CON_LOG=0`。`%c` 之类的样式参数按原样文本输出；单行超过 ~2KB 会被行缓冲截断。
  控制台守卫安装之前（页面脚本开跑的头几毫秒）的调用仍走 ant 原生 console，没有前缀/时间。
  只想看页面报错时 `grep '\[con-error\]'` 即可。
- `[nav] download mime=... url=...` 在顶层导航被判定为下载时打一行（`Content-Disposition: attachment`，
  或 MIME 不能显示）；任务本身的 start/progress/finish/fail/cancel 走 `[dl]`（见 [arch-downloads.md](arch-downloads.md)）。
- 这些日志是**正常输出**，不是错误（见 §6 噪音对照）。
- 可选调试通道（`[netlog]`、`[modfetch]`、`[snippet]`、`[call]`、`[ctx]`、`[loop]` …）**没有**走这个
  收口：它们各自有开关，`[netlog]` 还有自己的文件 sink 和时间原点。要拼总时间线时以 `[evt]`/`[nav]`
  为准。

## 3. 环境变量全表

| 组 | 变量 | 作用 |
|---|---|---|
| 日志 | `AGB_GAP_LOG=0` | 关掉 `[gap-jsdom]`（默认开启：每个已知缺口只在**第一次被访问**时打一行） |
| 日志 | `AGB_DL_LOG=0` | 关掉 `[dl]`（下载任务的 start/progress/finish/fail/cancel） |
| 日志 | `AGB_CON_LOG=0` | 关掉页面 `console.*` 的收口（`[con-*]`，默认开启；级别在 tag 里） |
| 探针 | `AGB_PROBE_SHOW=1` | 显示探针窗口（调试可见性相关的差异；截图不需要它） |
| 探针 | `AGB_PROBE_ALLOW_UNLOAD=1` | beforeunload 守卫直接放行（默认拒绝） |
| 日志 | `AGB_EVT_LOG=0` | **关掉默认开启的页面生命周期日志**（`[evt]`） |
| 日志 | `AGB_EVT_DEBUG=1` | 生命周期的详细版：evaluate 深度、listener 记账、JS `dispatchEvent` |
| 日志 | `AGB_NAV_LOG=0` | **关掉默认开启的导航动作日志**（`[nav]`：open/request/submit/redirect/popup/download） |
| 日志 | `AGB_UI_LOG=0` | **关掉默认开启的用户输入日志**（`[ui]`，仅 GUI 模式） |
| 日志 | `AGB_UI_DEBUG=1` | 连 `mouse move` 也打（默认只打 down/up/wheel/key/char/focus/resize） |
| 探针宿主 | `AGB_WAIT_MS` / `AGB_MAX_FRAMES` / `AGB_IDLE_FRAMES` / `AGB_FRAME_LOG` | `agave_view_host` 的等待与帧循环（出图用） |
| JS 错误 | `AGB_ERR_SNIPPET=1` | 引擎抛内部错误时打 `[snippet]` 源码片段（ant 侧） |
| JS 调用 | `AGB_JS_DEBUG=1` / `AGB_JS_TRACE=1` / `AGB_CALL_DEBUG=1` | JS 求值 / 每次 JS→C 调用（`[call] name nargs`）/ 桥调用细节 |
| JS 绑定 | `AGB_DOM_DEBUG=1` / `AGB_STYLE_DEBUG=1` / `AGB_EVT_DEBUG=1` | DOM 绑定 / style 访问 / realm·teardown 事件 |
| 事件派发链 | `AGB_EVT_TRACE=1` | 每次派发沿冒泡链打印 `[evt-trace] <type> node=<ptr> listeners=<n>`：看某个监听器到底在不在链上、有没有被调用（R45 就是靠它确认 React 的委托监听器确实执行了） |
| 崩溃 | `AGB_TRACE_CRASH=1` | 记录最近 256 条内部 trace（崩溃后能回看走到哪） |
| 渲染树/层树 | `AGB_LAYER_TRACE=1` | 每层出生/挂接/摘链/释放各打一行（`[lay] birth id=… kind=… why=…`、`LINK`、`UNLINK`、`free`）。Debug 构建里 `RenderLayer::destroy()` 发现层还挂在链上时**无条件**打 `[lay] DESTROY-WHILE-LINKED`（带 id/kind/why/parent/first）——看到它基本就是层树拆除的 use-after-free（R134），配 `tools/pages/fuzz_layer.html` 复现 |
| 图片解码 | `AGB_IMG_DECODE_MS=12` | 每帧（一次 paint）所有图片共享的解码时间预算，默认 **12ms**；`0` = 不限（旧行为，一帧解完整张图）。超出的行留到后续帧，图像逐行出现（R136） |
| 图片解码 | `AGB_IMG_DECODE_ROWS=256` | 单次 `frameBufferAtIndex` 的行上限（JPEG 按行；PNG 按 64KB 字节块计时间预算）。与 `AGB_IMG_DECODE_MS` 共同决定切片粒度 |
| 图片解码 | `AGB_IMG_SAMPLE_MAX=4` | 缩放绘制的盒式滤波每轴采样上限（大图缩小时精确盒=100 taps/像素）。**完整帧**用这个值，未完成帧固定 2；`0` = 精确盒（参考截图/像素对拍用） |
| 图片解码 | `AGB_IMG_NO_SCALE=1` | 关掉 L3 的"按显示尺寸解码"（JPEG 用 libjpeg IDCT scaling 只解 1/2、1/4、1/8）。参考截图、像素对拍、怀疑缩放解码画质时用（R137） |
| 图片解码 | `AGB_IMG_SCALE_DEBUG=1` | 打 `[img-scale] jpeg natural=WxH display=WxH denom=N out=WxH`：确认 hint 传到了解码器、libjpeg 真的缩小了输出 |
| 图片解码 | `[perf] ... decode=Nms` | `AGB_PERF_LOG=1` 时 `[perf]` 行里的 `decode=` 是本帧花在 libjpeg/libpng 里的时间；一帧只要解过码就会打（渐进解码会连续出现多帧） |
| 性能 | `AGB_PROFILE=1` / `AGB_PERF_LOG=1` | RIP 采样直方图（退出时 dump `[profjs]`，用 `tools/cdb_stack.ps1 -Symbolize` 前先把 `module+0x` 聚合成 `#NN` 行）/ 帧耗时（layout+paint > 100ms 才打，**带 `rect=WxH@x,y` 与 `decode=`**：脏矩形等于视口 = 正常，等于整篇文档 = 失效路径在放大重绘（R135），`decode=` 接近 `paint=` 时是图片解码跑在 paint 里（[design-async-image-decode.md](design-async-image-decode.md)）） |
| 性能 | `AGB_LAYOUT_DEBUG=1` | 每 5 次 layout 打 `[layout] calls= total= ms`：区分"真的在反复布局"和"只是在重绘"。整页 layout 在长文档上是 60–100ms 量级（R135） |
| 性能 | 页面内 rAF 计时 | 探针里跑 `debug/fps_paint.js` 那种脚本（每帧改一个内联样式 + 数 rAF）能量出"每帧 JS+样式+布局+绘制"的真实帧率；只数 rAF 不改样式会把布局/绘制全跳过，数字虚高 |
| 性能 | `AGB_PUMP_BREAKDOWN=1` | 每帧 `pumpAsyncRequests()` 分段计时（`[pumpbd] xhr=/module=/arrived=/vm=`，单帧 >2ms 才打）：`vm=` 大 = `antb_pump` 里的页面脚本/React 调度器；`xhr=` 大 = XHR 回调；用于区分"网络慢"和"JS 执行慢" |
| 页面脚本 | `AGB_PAGE_EVAL=eval` | 页面脚本回到 direct-eval 语义（**默认 classic script**，顶层 `var`/`function` 进 `window`，R66）；A/B 与 `run_script_mode_tests.ps1` 用 |
| GC | `AGB_GC_OFF=1` | 关掉引擎 GC；"关掉就好"是"对象被提前回收"的决定性判据（R60/R63/R67） |
| Realm | `AGB_PER_FRAME_REALM=0` | 关掉"每 frame 一个 realm"（[design-per-frame-realm.md](design-per-frame-realm.md) S1/S2，**默认开启**）：回到一 view 一 realm + per-frame window 代理的旧模型，用于 A/B。`run_realm_tests.ps1` 覆盖两条路径 |
| Realm 绑定 | `AGB_REALM_CHECK=1` | 写入类 API（`document.title`/`document.write`/`location`/`history.pushState` 等）的 self 文档与当前 binding 不一致时打一行 `[evt] realm-mismatch <api> self=<url> current=<url>`（每个 api 一次）。正常应为 0；`run_realm_tests.ps1` 已强制开启并把它当作失败 |
| 脚本编码 | `AGB_SCRIPT_CHARSET=1` | 每个脚本打印 `[script-charset] <编码> doc=<文档编码>`（排查乱码/重复日志） |
| 绘制诊断 | `AGB_PAINT_FULL=1` | 每帧整屏重绘（忽略脏矩形）；用来区分"失效/裁剪错"和"布局错"（见 §4） |
| 页面 | `AGB_TITLE_DEBUG=1` / `AGB_SCRIPT_DEBUG=1` | `document.title` 轮询 / tokenizer 脚本加载 |
| CSS/布局 | `AGB_CSS_DEBUG=1` / `AGB_LAYOUT_DEBUG=1` / `AGB_BG_DEBUG=1` | 未实现 CSS 属性日志 / 布局 / 背景层 |
| flex | `AGB_FLEX_DEBUG=1` | 每个 `RenderFlexibleBox` 的 `[flex] lines`（inner/wrap/行数）、每子项 `base/min/max/final/extra`，以及 `block-end` 的 `w=/over=`（两者不一致 = 子项被当普通块重排过，R81） |
| 字体 | `AGB_FONT_DEBUG=1` | 每个 `FontData::platformInit` 一行 `[fontdbg]`：size / ascent / descent / leading / unitsEm / xHeight（行盒 = asc+desc+leading，各独立 round，R123） |
| 字体 | `AGB_FONT_NO_SUBST=1` | **临时对照开关**：不把 arial/times/segoe ui 等家族替换成 Microsoft YaHei（R123 的缺口诊断用；关掉后西文与 Chrome 逐字体一致，但混排行盒只有主字体度量） |
| sticky | `AGB_STICKY_DEBUG=1` | 每次 `RenderBox::stickyPositionOffset()`：`top/bottom`、box/scrollport/containing block 的矩形与最终 `d=(dx,dy)`（R82）。滚动后没有这行 = 没人重算（没重绘） |
| CSS/视口 | `AGB_VIEWPORT_DEBUG=1` | 打印 vw/vh 变量注册（`-agb-vwunit-…`）与每个变量在当前视口下的 px 值，以及含 vw/vh 的 `calc` 模板代入结果（R69） |
| 配色方案 | `AGB_COLOR_SCHEME=dark` / `light` / `auto` | `prefers-color-scheme` 的答案。**默认 light**（不跟随系统）；`auto` 才读注册表 `AppsUseLightTheme`；`dark`/`light` 钉死（R86） |
| 模块 | `AGB_MODULE_DEBUG=1` | 模块抓取/求值/预取（`[modfetch]`/`[modeval]`） |
| 模块 | `AGB_MODULE_CACHE=0` / `AGB_MODULE_CACHE_TTL` / `AGB_MODULE_CONCURRENCY=n` | 关磁盘缓存 / TTL / 并发槽（默认 12） |
| 模块 | `AGB_OFFLINE=1` | 只从缓存取（`--offline` 就是设它） |
| 网络 | `AGB_NET_LOG=1` | 导航与请求时间线（`net.stage`：crack/connected/openrequest/sending/sent） |
| 网络 | `AGB_NET_DEBUG=1` / `AGB_NET_TIMEOUT_MS` / `AGB_NO_PROXY=1` | 请求细节 / 超时（**默认 0 = 系统默认**：WinHTTP 的 connect 60 s、send/receive 30 s；设了才用，注意 WinHTTP 的精度约 4 s 一档）/ 绕开代理解析 |
| 模块 | `AGB_MODULE_WAIT_MS=<ms>` | 模块体同步等待的上限（默认跟随网络超时，系统默认时 30 s）；`debug/xhr_timeout.html` 不涉及它 |
| 网络 | `AGB_HTTP_CACHE=0\|1` / `AGB_HTTP_CACHE_MB=n` | HTTP 缓存开关（默认开）/ 内存上限（默认 64MB） |
| 网络 | `AGB_SKIPRES=<file>` | 资源跳过表（默认读 `skipres.txt`） |
| 字体 | `AGB_FONT_LOG=1` | `[fontsel] src[i] ... supported=0/1` 与 `[font] beginLoad/data` |
| 控件 | `AGB_LOOP_DEBUG=1` / `AWG_CTX_DEBUG=1` | 控件帧循环 / 引擎上下文切换 |
| JS 文件 | `AGB_PRELUDE` / `AGB_POLYFILLS` | 指向别处的 `prelude.js` / `polyfills.js` 副本 |
| 注入脚本 | `AGB_INSPECTOR` | 覆盖外壳「注入 inspector.js」读取的路径（默认 exe 同目录的 `inspector.js`）。vConsole 固定读 exe 旁的 `vconsole.min.js` |
| profile | `AGB_USERDATA` | 整个 profile 根（cookies/localStorage/模块缓存/HTTP 缓存/shell state）。`--probe` 默认 `%LOCALAPPDATA%\Agave\probe`，`--userdata` 等价 |

## 4. 定位手段

- **想在页面里看 console / 网络 / 元素**：**F12 或 Ctrl+Shift+I**（inspector.js 二次注入
  即自毁，所以同一个键开关），或右键菜单 / `=` 菜单 → `注入 vConsole`
  （默认）或 `注入 inspector.js`（hover 高亮 + 半透明信息浮层，Esc 退出；在页面上
  再右键出页内菜单，复制元素信息 JSON / 选择器 / 尺寸 / 文本 / HTML）。脚本从
  exe 同目录读，改完再点一次即可；`AGB_INSPECTOR` 换 inspector.js 路径。探针等价：
  `--probe <url> --inject vconsole|inspector`。注入 + 页内菜单的回归：
  `pwsh -File tools/run_inspector_tests.ps1`。细节见
  [browser_shell/README.md](../examples/browser_shell/README.md#注入调试脚本)。
- **未捕获/被页面吞掉的错误**：`AGB_ERR_SNIPPET=1`（ant `src/errors.c`）。引擎创建内部错误
  （`js_mkerr_typed`）时，若消息含 `length` / `not a function`，往 stderr 打
  `[snippet] line=<行> col=<列> msg=<消息> | <出错点前 60 字符起的 130 字符源码>`
  —— 不用栈就能把错误对回 bundle 偏移（React/lottie 把异常吞进 `console.error` 时特别有用）。注意：
  ①行号恒为 2（页面脚本被 `try{\n…\n}catch` 包过），**列号才是真偏移**；
  ②两半标记之间是**出错前的上下文**，真正的 callee 在片段末尾附近；
  ③core-js/modernizr 的特性探测会故意抛错并自己 catch，这类 `[snippet]` 是**正常噪音**。
- **DOM 节点属性读出 undefined**：先查桥（`wrapNode` / 类属性表 / catch-all getter），再怀疑页面。
- **Vue "missing template or render function"** ≈ `container.innerHTML` 拿到 undefined。
- **元素高度在加载后跳变**：先判断**会不会回落**——骨架屏/图片/字体会让内容短暂变高（DeepSeek 登录页的二维码块就是：骨架屏层与 iframe 层并存约 1.2 s，块 302 → 526 → 302）。回落 = 页面自己的过渡，**不回落**才是布局 bug（R113：auto 高度的 flex 容器把上一次布局的高度当成了可用高度）。量法：`--inject inspector` 后分别在加载中、稳定后各点一次 `Copy element info` 对比 `getBoundingClientRectInPage`。
- **`<template>` 内容惰性**（HTML5）：`Node::createRendererIfNeeded` 对 `localName=="template"` 返回，
  子树没有 renderer、不渲染；children 仍在 DOM（`innerHTML`/script 访问不变）。**别删**。
- **`on_progress finished=1` 不代表加载成功**：用 `agave_webview_url` 比对 host 判断是否真提交
  （加载卡死时 FrameLoader 会以 `about:blank` 提交）。
- **一条命令跑探针**：`pwsh -File tools/probe.ps1 -Url <url> -ScriptFile <js>`
  —— 探针自己解析命令行、把 `"` 和 `\` 当语法，内联 `--eval` 里带 JS 字符串字面量会被拆坏；
  这个 helper 用 `--eval-file` 传文件，顺带设好 `--wait/--settle/--timeout/--userdata`。
  退出码仍是权威判据。
- **查选择器命中**：`AGB_QS_DEBUG='#app'` 时每次 `querySelector` 只要选择器包含该子串就打印
  `[qs] sel=... found=0/1`。定位“框架说挂载目标为 null / 找不到容器”这类问题先用它，
  比在页面里重放选择器可靠（能抓到页面脚本自己的调用时机）。
- **未处理 Promise 拒绝**：`window.onunhandledrejection` 的 polyfill 会打印
  stack → `name: message` → 对象前几个自有属性，不再是 `[object Object]`；
  CLI 侧 ant 的 `print_error_value` 对普通对象也走同一套渲染。
- **卡死/假死**：先 `pwsh -File tools/js_stack.ps1 -Pid <pid>` —— 不 attach、不重启，直接抓当前 JS 栈
  （见下面"卡住的窗口抓 JS 栈"）。要**从启动就开始看**：设 `AGB_HANG_WATCH=1`（`AGB_HANG_SECONDS` 默认 5），
  引擎线程连续 N 秒满核就打印 `[hang]` + JS 栈（`file:line`）；窗口无响应但 CPU 不高则是 UI 线程/等待问题，
  用 `tools/cdb_stack.ps1 -Attach <pid>` 看全线程。栈里 `intern_find/hash_key` / `j (vendor-common…js)`
  说明是属性/数组访问热点（脚本 VM），不是死锁。
- **`0xC0000005` 且只跟"时长"有关**（长循环、大 eval、注入脚本）：先排除**视图生命周期**，
  不要先怀疑 JIT/优化器。判据：①崩溃点是否固定在某个墙钟时间而不是迭代次数
  （把循环体加 `console.log`，看最后一条进度；R61 里 5M 和 1M 在同一个时间点崩）；
  ②失败帧最后一行是不是 `[probe] main url=...`（探针打断消息循环 → `agave_webview_destroy()`）；
  ③把 `--settle` 调大就"好了" —— 这三条基本就是"引擎线程还在跑 EVAL，宿主把视图销毁了"。
  回归：`pwsh -File tools/run_jit_loop_tests.ps1`（含"超时路径"在 eval 中途销毁视图的用例）。
  要证明与浏览器无关：`tools/ant_loop_repro.c` 直接 `antb_engine_create + antb_eval` 跑同一脚本
  （编译/运行见文件头），它在 `f68ba03c` 和 HEAD 上都 exit 0。
- **`HEAP: Free Heap block ... modified ... after it was freed`**（调试器下）：这是**堆破坏**，
  不是当前栈的错 —— NT 堆在**下一次分配**时才发现某块释放后被写，faulting stack 只是"发现点"。
  别顺着那条栈去修（R62 的 `PNGImageDecoder`/`WinHttpSession` 栈就是这样把人带偏的）。两条工具：
  ①`AGB_GUARD_MALLOC=<all|512-4096|0>` —— WTF 分配器守卫（`src/wtf/FastMalloc.cpp`）：页对齐 +
  尾部保护页，`free` 后整块 `VirtualFree`，于是"释放后写/读"和"越界写"立刻在**肇事指令**上 AV，
  栈就是写入者（会被 `tools/cdb_stack.ps1` 符号化）；②`tools/cdb_heap_probe.ps1 -Url <url>` ——
  cdb **启动**（堆校验才生效），从管道里读 HEAP 消息的地址，现场 `db/dq/dps/ln` dump 被写坏的块
  和它的 vtable，用来判断"这块是什么"。
- **只在非调试器下复现的 AV**：先用 `AGB_TEST_CRASH=1` 跑一次
  （`agave_shell.exe --probe about:blank`）确认取证链路没坏，然后正常复现。壳里装了两道：
  ①`AddVectoredExceptionHandler` 在**任何** SEH/filter 之前抓首次异常，写 `agb_crash.txt`
  （`[crash-firstchance] thread/code/addr/info0/info1` + `#NN module+0x...` 帧，原始 Win32 I/O，
  CRT 挂了也能写）；②`shellCrashFilter`（`SetUnhandledExceptionFilter`）再打一遍 `[crash]` 到 stderr。
  VEH 拿到帧后会**动态调用 antjsb 的 `antb_print_last_js_stack()`**，所以日志里能直接看到
  `at <anonymous> (https://…/x.js:10:606395)` —— 没有这一条时，JIT 里的崩溃只会显示 `?+0x…`。
  取到文件后：`pwsh -File tools/cdb_stack.ps1 -Symbolize build-zig-native/agb_crash.txt`。
  注意 ant 自己也装了 crash handler（`src/crash.c`），它只往 previous filter 链一次、且会用
  StackWalk64，二次异常时会静默吞掉整个进程 —— 这就是"cdb 一挂就不崩、不挂又没输出"的形状。
- **`cdb -p <pid> -c "…"` 里任何一条命令解析失败 = 杀掉被调试进程**：cdb 把 `-c` 的命令表当脚本，
  遇到错误就中断，后面的命令（**包括 `qd`**）都不执行，cdb 退出时按默认行为 `TerminateProcess`。
  实测（2026-11-06）：对一个 `ping` 进程跑 `cdb -p <pid> -c "x nosuchmodule!sym; qd"` → ping 直接消失；
  同一形态在真的浏览器窗口上踩过（想 `.call` 打 JS 栈，命令没解析成功，窗口没了）。
  所以对**正在用的窗口**做 attach 实验时：①先用只读命令验证符号能解析（`x antjsb!antb_print_js_stack`）；
  ②命令表最后必须是 `qd`，表达式不许打错。
- **卡住的窗口抓 JS 栈：`tools/js_stack.ps1 -Pid <pid>`**（不 attach、不重启、进程继续跑）。
  它在目标进程里 `CreateRemoteThread` 调 `antjsb!antb_dump_last_js_stack_to_file(path)`（arg 就是路径，
  正好是该导出唯一的参数），引擎自己把栈写到文件。**不要用 cdb `.call` 走这条路**：ant 用
  `__declspec(thread)`，`.call` 的调用上下文里 TLS 数组不是 antjsb 初始化过的那份，第一条
  `_tls_index` 加载就 AV（实测 `Attempt to read from address 0x50`），而且上面那条"命令失败 → 杀进程"
  会让窗口直接消失；`.call` 还必须补一个 `g` 才真正执行（`.call` 只做 "set up"，不打 `g` 就 detach
  会把线程留在半搭好的调用帧上，进程同样会死）。另外 `js_print_stack_trace_vm()` 写 `stderr`，
  调试器读不到，重定向（`freopen`/`_dup2`）本身又是一串容易打错字面量（cdb 的 MASM 解析器不允许
  `.call` 里带字符串字面量）。
  产物形如 `at j (https://vfiles.gtimg.cn/…/vendor-common-*.js:1:52943)` × N —— v.qq 的卡死能直接看到
  字节码 VM 的递归分发帧，比看门狗的单帧信息多。
### 引擎/桥接侧的开关（在 ant 仓库里读，不是 agave 读）

这些 `AGB_*` 由 `antjsb.dll`（bridge 或引擎）自己 `getenv`，agave 只负责透传环境变量。
**读的时机不一样**：带「启动时」的只在建引擎/首次命中时读一次，改了必须重启进程。

| 开关 | 读处 | 作用 | 备注 |
|---|---|---|---|
| `AGB_JIT_DEBUG=1` | `bridge/AntBridge.c:187` | 打开 ant 的 JIT 诊断（`SV_DEBUG_JIT_WARN`：不合法操作、OSR 编译结果、bailout） | 建引擎时读 |
| `AGB_JS_TRACE=1` | `bridge/AntBridge.c:2127` | 每次 JS→C 派发打印 `[call] <name> nargs=N` | 首次命中时读一次 |
| `AGB_CALL_DEBUG=1` | `bridge/AntBridge.c:2333,2338` | `antb_call` 进出打印 `[call] enter/leave fn=…` | 每次调用都 `getenv`（热路径，只在调试时开） |
| `AGB_ERR_SNIPPET=1` | `src/errors.c:394,430` | 内部错误附 `[snippet]` 源码片段 | 页面关掉 `stackTraceLimit` 时定位用 |
| `AGB_GC_DEBUG=1` | `src/gc/gc.c:379` | 退出时 dump GC 统计 | 启动时读 |
| `AGB_GC_OFF=1` | `src/gc/gc.c:380` | **完全关闭 GC**（诊断） | "关掉就不崩" = 对象被提前回收的判据（R60/R63/R67） |
| `AGB_PROXY_DEBUG=1` | `src/ant.c:19779,19792` | Proxy `get` 不变量失败时打印 `[proxy] key=… result=… target=…` | 命中即读 |
| `ANT_GC_STRESS=1` | `src/verify.c` | 每次分配都触发 GC（测试用） | meson `put-elem-gc` 用它跑 |
| `AGB_NAV_ABORT_MS=<ms>` | `src/webview_control/AgaveWebView.c`（`agwWatchdogStart`） | 导航排队兜底：导航命令在队首等这么久、且有 JS 在跑，就中断一次（正常路径在 `agwPost()` 里**投递时就中断**，这里只在"投递时引擎还没进 JS、随后才进"的情况下兜底），默认 100；触发时**不弹提示框** | 看门狗启动时读一次 |
| `AGB_TEARDOWN_MS=<ms>` | 同上 | 收尾预算：旧文档的 `pagehide`/`visibilitychange`/`unload` 任何一步跑这么久还没返回就中断它（下一个事件照常发），默认 100。导航时旧页被立即中断、收尾按流程走，就靠这一档保证收尾不会拖住导航 | 调小可复现：`debug/teardown_hang.html`（pagehide 死循环）+ `--nav-after 800` |
| `AGB_USER_ABORT_MS=<ms>` | 同上 | 不换文档的用户动作（停止 / 查找）被页面脚本挡住的容忍时间，默认 1500（导航走上面的 100） | 看门狗启动时读一次 |
| `AGB_UNRESPONSIVE_MS=<ms>` | 同上 | JS 连续在跑多久算"页面无响应"，默认 12000（`AGB_STUCK_EXEC_MS` 是它的旧名，仍然认） | 调小可复现：`AGB_UNRESPONSIVE_MS=2000` + 20 s 死循环 → `[evt] watchdog: aborting an unresponsive page script` |
| `AGB_NATIVE_STALL_MS=<ms>` | 同上 | 没有 JS、命令却多久不 drain 算"引擎在 JS 之外卡住"，默认 10000；这一档**不中断**，只记日志 | — |
| `AGB_WD_DEBUG=1` | 同上 | 每个 tick 打印 `wd js=… jsbusy=… busy=… queue=… head=…` | 调阈值、判断"真卡/慢"的第一手数据 |
| `AGB_PARSE_LOG=1` | `src/webcore/html/HTMLTokenizer.cpp` / `HTMLScriptElement.cpp` / `AntRuntime.cpp` | HTML 解析轨迹（默认关）：`tag` 行=解析器在每个 `<script>`/`<style>`/`<link>` 起标签被 tokenize 处打一行（**全部属性**按作者写法原样打印，空值属性只打名字，超长值截断 `(N bytes)`；`src=`/`href=` 后跟 `url=<解析后的绝对地址>`，和作者写的不同才打）；执行行=脚本真正的时刻与形态（inline/external、classic/module、flush 封口**前后**分别打 `queued for parse end` / `running now (parse already finished)`）。排查"脚本为什么在这个时机跑"从这里开始 | 见下例 |
| `AGB_CSS_PROFILE=1` | `src/webcore/css/CSSParser.cpp`（`parseSheet`） | 每张样式表解析前打印 8 个改写 pass（`hoistPageRules`/`rewriteViewportUnits`/`foldCalc*`…）与 `cssyyparse` 的耗时（默认关）。大样式表排版卡顿时先看这里：先确认哪个 pass 慢，再怀疑语法解析 | 页面带大 `<style>`：用 `debug/stall_baidu_css.html`（百度全部内联 CSS 的本地复现页，~316KB） |
| `AGB_HANG_POPUP=0` | `examples/browser_shell/main.c`（`onUnresponsive`） | 关掉"页面无响应"提示框（无人值守/自动化时用） | — |

ant 自身 CLI / 基础设施的开关（`ANT_MANIFEST_URL`、`ANT_SANDBOX_*`、`ANT_NO_CRASH_HANDLER` 等）
在 `H:\ant-v16` 里读，**和 agave 的运行无关**，排查浏览器问题时不用管。

- **解析轨迹（`AGB_PARSE_LOG=1`，默认关）**：`debug/parse_log.html` 的实测输出（链接、内联样式、
  经典/模块/defer/async 各一条）：

  ```
  [parse] tag link rel=stylesheet href=data:text/css,a%7Bcolor:red%7D media=screen
  [parse] tag style (no attributes)
  [parse] tag script (no attributes)
  [parse] run classic script url=(inline) len=25
  [parse] tag script type=module
  [parse] script (module) queued for parse end url=... len=24
  [parse] tag script defer src=data:text/javascript,window.deferred=1
  [parse] tag script async src=data:text/javascript,window.asyncOne=1
  [parse] script (defer) queued for parse end url=... len=17
  [parse] script (async/dynamic) evaluating url=... len=17
  [parse] deferred module running url=... len=24
  [parse] deferred classic script running url=... len=17
  ```

  语义约定：`tag` 行 = 分词器看到这个标签的时刻（元素尚未建好/入树）；`run ...` / `script (...)`
  = 执行点的**决策结果**（queued / running now / evaluating），日志都打在做出决策的地方，
  所以"只见 queued 不见 running"不会出现——flush 封口后才到的 defer/module 打
  `running now (parse already finished)`。动态脚本先进引擎的那一步打
  `evaluating`（`AntRuntime::evaluate()` 可能仍把 module 形态的 body 改道进队列，改道后由队列打真实结果）。
  标签行走 `HTMLTokenizer::processToken` 前（属性全量、按作者写法），执行行走
  `HTMLTokenizer::scriptExecution` / `HTMLScriptElement::evaluateScript` /
  `AntRuntime::queueDeferredScript` / `queueModule`（tokenizer 对 module 只在
  `queueDeferredModule` 处报）/ 解析后 flush。

- **卡死的页面怎么恢复（不用杀进程）**：刷新/导航在控件里是排队命令，脚本不返回时队列永不 drain，
  所以窗口“活着但没反应”。中断机制：引擎在下一个 safepoint（解释器调用点 / JIT 循环回边）抛终止错误，
  脚本退出 → 队列继续 → reload 拿到新 realm；标志被中断一次后即被消费（`js_interrupt_raise()`），
  新文档照常执行。锚点：`agave_webview_interrupt()` / `agave_webview_busy()` /
  `antb_interrupt_all_engines()`。
- **怎么区分"真卡"和"慢"**（R90/R92，判据在 `agwWatchdogThread()`）：单看时长永远分不出来，所以看两件事 ——
  **VM 里有没有 JS 在栈上**（`antb_engines_js_depth()`，ant 自己用它判 job 边界）和**谁在等**（队首命令类型）：

  | 现象 | 判定 | 动作 |
  |---|---|---|
  | 导航（navigate/刷新/前进后退/load-html）投递时 JS 正在跑 | 用户要离开，页面马上被替换 | **投递即中断**（`agwPost()`，无等待），静默 |
  | 导航命令在队首 ≥100 ms 且有 JS 在跑 | 投递时引擎还没进 JS（原生解析），随后才进 | 中断兜底（`AGB_NAV_ABORT_MS`），静默 |
  | 旧文档收尾（pagehide/visibilitychange/unload）≥100 ms 未返回 | 收尾 handler 卡住 | 中断它，**下一个事件照常发**，静默（`AGB_TEARDOWN_MS`） |
  | 其它用户动作（停止/查找）被脚本挡住 ≥1.5 s | 用户要操作，但页面留在屏幕上 | 中断并提示（`AGB_USER_ABORT_MS`） |
  | JS 连续在跑 ≥12 s，没有人在等 | 页面无响应（Chrome 同量级） | 中断并弹提示（`AGB_UNRESPONSIVE_MS`） |
  | 没有 JS、命令却 ≥10 s 不 drain | 引擎在 JS 之外卡住（模态/死锁/原生长活） | **不中断**（没人轮询标志），只记日志（`AGB_NATIVE_STALL_MS`） |

  页面脚本在 `macross_event_dispatch()` 里跑（load 事件、timer），**不在命令里** —— 所以"队首等了 1.5 s"
  不能当成卡死（css3test 的 2.9 s onload 就是这样被误杀成 0% 的）。看门狗每 50 ms 采样一次（导航预算只有
  100 ms，200 ms 的 tick 会让实际反应变成 100–300 ms），
  `AGB_WD_DEBUG=1` 打印每个 tick 的四元组（`js/jsbusy/busy/queue/head`），是调这几个阈值的第一手数据。
  收尾预算的起点来自引擎：`documentWillUnload()` 写 `g_teardownStart`，控件通过
  `agb_teardown_started_ms()`（`src/webview/AgbRuntime.h`）轮询——commit 是异步的，控件自己看不到收尾从哪一刻开始。
  中断只对 A 类（失控 JS）有效；B 类（原生阻塞）中断标志挂在那里没人看，所以第三档只记录不动作。
  每次判定同时写一行 `build-zig-native\agb_watchdog.txt`（pid / kind / jsdepth / 队首命令 / busy / queue）——
  GUI 里没有 stderr，"探针好好的、窗口里是 0%" 这种问题就靠它。
  回归：`pwsh -File tools/run_hang_recovery_tests.ps1`（8 条：中断、导航中断、空闲误中断、慢页连刷、
  timer 长脚本不被杀、卡死页刷新恢复、HTTP 连刷、reload 链）。
  验证手段：`--probe <卡死页> --interrupt-after 2000`（到点中断，页面继续加载）、
  `--nav-after 2000`（模拟"卡着时重新导航"）、`debug/teardown_hang.html`（pagehide 死循环，
  收尾被切断且 unload 仍执行）、`--hide-after/--show-after`（Page Visibility，不经 GUI 切 tab）。
  页面生命周期（事件顺序、Chrome 对拍工具）：`tools/chrome_lifecycle.py`。
  卡死时先 `tools/js_stack.ps1 -Pid <pid>` 留证据再点刷新。`AGB_CRASH_POPUP=0` 可关崩溃弹框
  （弹框会给出 `agb_crash.txt` 的绝对路径与符号化命令）。
- **一条命令跑站点冒烟**：`pwsh -File tools/run_site_smoke.ps1`
  —— deepseek / baidu / v.qq 各跑一次探针，输出退出码、耗时、元素数与 title；退出码 0 且元素数
  达到下限才算过。**需要网络**：exit 5/6 先确认站点可达再当引擎问题查。v.qq 目前带
  `AGB_GC_OFF=1`（R68/P0-h 未修），修掉后去掉。改了 realm/loader/DCL/GC 之后跑它。
- **一条命令跑脚本语义/性能回归**：`pwsh -File tools/run_script_mode_tests.ps1`
  —— `ant_pin_repro.exe`（C 侧 `antb_retain` 住一个函数后强制 GC 再调用，R67）、
  `cap_script_semantics.html`（`window.pageVar=number`）、`AGB_PAGE_EVAL=eval` 回退、
  `bench_hot` 的 ns 上限。翻 `AGB_PAGE_EVAL` 默认或动 GC 根集合后必须过。
- **一条命令跑布局回归**：`pwsh -File tools/run_layout_tests.ps1`  —— 对 `tools/pages/layout/*.html` 逐个跑 `layout_diff.py`（几何对拍 Chrome），再对
  `transform_repaint.html` 做像素断言、对 `scrollbar_hidden.html` 断言 `::-webkit-scrollbar`
  隐藏后的视口宽度，输出 `LAYOUT=PASS`。加几何页不用改脚本（滚动条页没有 `data-case`，
  由专用断言覆盖）。
- **像素断言**：`python tools/pixel_check.py <png|bmp> 250,150,ff0000 450,150,ffffff [--tol 8]`
  —— 读探针 `--shot` 的图（PNG 8 位 RGB/RGBA 或 BMP 32/24 位，两种都直接读），`x,y,RRGGBB`
  是截图坐标（左上原点）。绘制类回归（裁剪、变换、placeholder 消失）用它，几何用 `layout_diff.py`。
  图是从 `--shot-rect` 裁出来的，坐标就相对那块矩形。
- **点查 Chrome 的固定套路（`tools/chrome_probe.py`）**：几何之外的"Chrome 到底怎么算的"
  一律走这一条命令，别再手写 CDP 脚本（手写的容易连到用户自己的浏览器、漏 `Emulation`、
  忘了 enable 需要的域）：

  ```powershell
  # 同一段 JS 在两边跑：脚本末尾 console.log('RESULT ' + JSON.stringify(...))
  python tools/chrome_probe.py --url <url> --js-file debug/x.js --agave --wait-load

  # 哪条 CSS 规则真的命中、值是什么（CDP CSS.getMatchedStylesForNode）
  python tools/chrome_probe.py --url <url> --matched-styles .ds-input --props margin

  # 实际用的是哪个字体家族（CDP CSS.getPlatformFontsForNode）
  python tools/chrome_probe.py --url <url> --platform-fonts '#a'

  # Chrome 参考截图（和探针 --shot 对拍）
  python tools/chrome_probe.py --url file:///E:/... --screenshot build-zig-native/chrome.png
  ```

  内部固定：`--headless=new` + 临时 `--user-data-dir` + 独立 `--remote-debugging-port`
  （不碰用户会话）、`Page/DOM/CSS/Runtime.enable`、`Emulation.setDeviceMetricsOverride`
  （默认 1084×800，与 `layout_diff.py` 一致）。agave 侧复用 `--probe --eval-file` 并只解析
  `[con-log]` 行，所以同一个 JS 文件两边都能跑。
  ⚠ 视口对齐：agave 探针默认 1100×800（16px 经典滚动条），要对 Chrome 的 1084 就用
  `--agave-resize 1084x800`；本地页面用 `file:///E:/...`（正斜杠）。
  ⚠ 字体：Chrome 本机（zh-CN）默认字体是 **Noto Sans SC**，agave 默认 Times 且会把
  arial/times/segoe ui 整族替换成 Microsoft YaHei（R123，`AGB_FONT_NO_SUBST=1` 可对照），
  字体相关的差异先看这一条再怀疑布局。
  ⚠ **agave 侧的 `RESULT` 会被截断**：`[con-log]` 单行大约 2 KiB 封顶，一次
  `JSON.stringify` 整页元素的长结果会腰斩（Chrome 侧完整，看着像 agave 少了一半字段）。
  只打关心的字段（`{left,top,width,height}` 通常就够），或按 800 字符分段打 `R0:`/`R1:` 再拼；
  回归脚本里更稳的是每条断言打一行短文本（`a.margin=ok`），见 `tools/pages/check_computed.js`。
  ⚠ **光标 / `:focus` 在 headless 里默认看不到**：Chrome 侧加 `--focus-emulation`
  （`Emulation.setFocusEmulationEnabled`，在脚本之前打开）让 `:focus` 与 `focus()` 真正生效；
  但 headless 的 `Page.captureScreenshot` 不一定画光标（R127 实测没抓到），要量 Chrome 的
  光标得看真浏览器。agave 侧加 `AGB_CARET_DEBUG=1`：它无视焦点和闪烁相位把光标画成**红色**，
  `python tools/caret_check.py <png>` 量出 `x/y/height`。例：
  `agave_shell.exe --probe <pg> --eval "document.getElementById('a').focus()" --shot build-zig-native/c.png --shot-rect 0,0,240,60`
  （**别**在 `--eval` 里 `probeExit`，探针会先退出、截图不落盘）。
- **布局对拍 Chrome**：`python tools/layout_diff.py tools/pages/layout/<page>.html [--tol 1]`
  —— 同一份 `tools/pages/layout/lib_measure.js` 在 agave 探针和 headless Chrome 里各跑一遍，
  两边视口都固定成 1084×800（agave = 1100 窗口减 16px 经典滚动条；Chrome = `--hide-scrollbars`
  + `Emulation.setDeviceMetricsOverride`），按 `data-case="名字"` 输出
  `名字:left,top,width,height|prop=computed`，逐行比 rect/计算值。用例写在 HTML 里即可
  （`data-props` 指定要读的计算属性）。加页面：`tools/pages/layout/*.html`，一套跑多个：
  `python tools/layout_diff.py tools/pages/layout/*.html`。
- **一条命令截图**：`pwsh -File tools/screenshot.ps1 -Url <url> -Out <png> -Wait 12000`
  —— 内部就是 `agave_shell.exe --probe <url> --shot <png>`：探针窗口是隐藏的 `WS_POPUP`，
  从不 `ShowWindow`，抓的是控件的 mirror 缓冲（`agave_webview_paint_to`），所以屏幕上什么都不会出现。
  `-Out` 写 `.png`/`.bmp` 时探针直接落盘（PNG 由壳编码，**没有** BMP 中转），`.jpg/.gif/.tif` 才在
  脚本里转一次。
  只要页面上某个元素：`-Rect x,y,w,h` 透传给 `--shot-rect`（CSS 像素，可直接用
  `getBoundingClientRect()`；输出按 `--zoom` 换算成设备像素），输出的图就是那块，不必事后裁。例：
  `pwsh -File tools/screenshot.ps1 -Url file:///E:/mybrowsers/agave/debug/shot_page.html -Out build-zig-native/box.png -Rect 0,0,440,200`
  不要启动 GUI 壳截图、不要 `SetForegroundWindow`、不要抓屏（会抓到任务视图/别的窗口，也会打扰用户）。
  需要像素时优先用它；能只看数字就别截图。
- **画面"少了一块"（文字/图标不见了）先分清是布局还是绘制**：三步定死，别猜。
  ①**同一次运行里两处取样**：`--wait 6500` 与 `--wait 8000` 各截一张，比较同一像素区域
  （例如 deepseek 登录页 `+86` 在 `(265..305, 100..125)`）的亮像素数量。数字从有到无 = 画上去后**被擦掉**，
  两次都没有 = 从来没画（布局/样式问题）。
  ②**抓"哪一帧擦的"**：在 `WebView::paintView()` 的 `paint()` 之后，直接读 `d->m_buffer`
  （BGRA，pitch=`m_width*4`）采样同一像素并打时间戳，就能把"擦除"归到某一帧，同时把该帧的
  `d->m_drawRect` 打出来；再在 `GraphicsContext::fillRect` / `clip` 里打请求矩形与当前 clip，
  对照"请求超出 clip 却落笔了"就能判定是**裁剪丢失**（R40 就是这么抓到的：`[fill] r=262,103 336x35`
  的 clip 只有 `309,103 3x22`）。
  ③**`AGB_PAINT_FULL=1` 反向验证**：它把每帧的脏矩形强制成整个视口（`WebView::paintView`）。
  开了之后画面正常 → 引擎的**失效/裁剪**有问题（脏区太小或 clip 被丢），**不是布局**；
  开了还是不对 → 回头查布局/样式。这个开关只用于诊断，别留在默认路径上。
  另注：本地复现页很容易"看起来正常"，因为整屏重绘时脏区永远够大 —— 要复现这类 bug 必须制造**局部重绘**
  （光标闪烁、iframe 局部刷新、hover 变色都行）。
- **"这个元素为什么是 0×0"**：先 `__agbLayoutInfo(node)`（不强制布局）——
  `renderer=0` 是"没有渲染树对象"（样式在最后一次重算时是 `display:none`），
  `renderer=1` 但 `w=0` 是"挂上了但从没布局"，两者的修法相反。配合 `[style]` 通道看时序。
- **页面踩了哪些 JS/DOM 缺口**：`[gap-jsdom]` 默认开着，每个缺口**每个进程只打一次**（不是每次访问），
  例如 DeepSeek 登录页：
  ```
  [    236.0ms] [gap-jsdom] name=querySelector[attr] area=css effect="attribute selectors ([data-x]) match nothing"
  [   1258.0ms] [gap-jsdom] name=document.hidden area=dom effect="undefined; document.hidden is never set, el.hidden not exposed"
  [   1487.0ms] [gap-jsdom] name=history.state area=history effect="undefined; pushState only rewrites the URL"
  [   2318.1ms] [gap-jsdom] name=document.activeElement area=doc effect="undefined; no focus tracking in script"
  ```
  `name=<接收者>.<属性>`（元素给 `tag#id`），`area` 分组，`effect` 是页面实际得到的值 + 替代写法。
  名单在 `src/javascript/ant_backend/AgbGapJsdom.h`（= `docs/arch-capability-gaps.md` §1 的机器可读版），
  `AGB_GAP_LOG=0` 关掉。全局对象上的缺口（`window.getSelection`、`IntersectionObserver` 等）打不出来，
  原因见该头文件注释（ant 的标识符查找不走全局对象的 exotic getter）。
- **iframe 里到底是什么样**：`__agbActivateSlot(i)` 切换共享 window 绑定的 frame 文档
  （返回上一个槽位），读 `document.*` 之后切回去；`__agbFrameSlot()` 返回当前槽位。
- **一条命令抓带符号栈（首选）**：`pwsh -File tools/cdb_stack.ps1 -Url <url> -ScriptFile <js>`
  —— 自动带 `-y "build-zig-native;H:\ant-v16\build-zig"`、`.lines`、`sxe av/0xC0000409/0x80000003`，
  命中后 dump `kb 60` + `~*kb 30` + `lm`，日志落 `debug/cdb_stack_<ts>.log`；退出时还会自动符号化
  `[shell] atexit` 的 `#NN exe|libagave|antjsb+0x...` 帧。挂起用 `-Attach <pid>` 或
  `-Hang -Url <url> -HangAfter 8`；已有日志用 `-Symbolize <log>`。
  `antjsb.pdb` 和 DLL 一起安装（H:/ant-v16/build-zig、3rdparty/ant/bin、build-zig-native），
  所以 cdb 直接就有符号；`pwsh -File tools/build_antjsb.ps1 -Force` 重编并重装。
  下面两条是**需要自定义 cdb 命令时**的手工后备。
- **抓挂起线程栈**：`tools\cdb.exe -p <pid> -c '~*k 30; qd'`（探针有超时会秒退，attach 要趁早）。
- **断在首次异常**：写命令文件 `sxe av` / `g` / `~*kb 20` / `q`，再
  `tools\cdb.exe -y "<build-zig-native>;H:\ant-v16\build-zig" -cf cmds.txt build-zig-native\agave_shell.exe --probe ...`
  —— **不要用 `--` 分隔**（这版 cdb 报 0x80070057），也别用 Start-Process 传 `-c "长命令"`（引号被吞）。
  `antjsb.dll` 旁边有匹配的 `antjsb.pdb`，`-y` 指过去就能解出 `process.c @ 2355` 这种帧。
- **CPU 热点**（连 ant 内部符号一起解）：`AGB_PROFILE=1` 跑探针拿 `[profjs]` 直方图；antjsb 和
  libant 都是带调试信息的 release（`-O2 -g` / `-O3 -g`），`cdb -y <dll目录> -z antjsb.dll -c "ln antjsb+0x<rva>; qd"`
  出函数/源码行；在线抓栈用 `cdb -y <dll目录> -p <pid> -c "~*k 30; qd"`。
- **地址栏输入 → URL**：`examples/browser_shell/main.c` 的 `normalizeUrl()` 是唯一入口（地址栏/Go/收藏夹/
  `--probe` 都走它）。本地路径 → `fileUrlFromLocalPath()`（`GetFullPathNameW` + `\`→`/` + 逐字节百分号转义）；
  `file:` URL 在引擎侧只能**单次转义**（`WebView::loadUrl` 会对整串调 `KURL::encode_string`，它必须幂等）。
  调试入口：`[probe] main url=` 就是引擎最终 URL；运行时的导航决策看 `[nav]`（见下一条）。
- **导航去了别处 / 没发生**：按 `[nav] open`（宿主发起）→ `[nav] request type=... method=...`（引擎分类）
  → `[evt] commit`（真正提交）的顺序找断点。`popup create`/`blocked` 是弹窗，`client-redirect` 是
  meta refresh / JS 定时跳转；`[nav] back/forward/reload/stop` 是宿主的历史与重载。301/302 现在会以
  `[nav] redirect from=... to=...` 出现（§2），要查更细的 3xx 交互再用 `AGB_NET_LOG=1` 或抓包。
- **worker**：独立线程 + 独立 isolate，崩溃栈在**非主线程**（`~*kb` 里找 `worker_thread_main`）；
  worker 入口在 ant 侧走 ESM/CJS，栈帧名对 data: URL 折成 `data:script/<hash>`（看到它=worker 内的代码）。
  回归：`pwsh -File tools\run_worker_tests.ps1`。
- **网络卡点**：`AGB_NET_LOG=1` 看卡在 crack/connected/sending 哪一步；WinHTTP `SendRequest` 偶发长滞是
  WPAD/PAC 环境抖动，同刻 GUI 正常≠代码回归，先重试。
- **字体**：`AGB_FONT_LOG=1`；回归页 `tools/pages/iconfont_render.html`，
  `agave_view_host.exe <file-url> out.bmp` 出图核对字形，链路见 [arch-web-font.md](arch-web-font.md)。

## 5. 宿主/控件硬规则（违反会串消息或野指针）

- 宿主窗口消息循环处理 `WM_TIMER` 必须按 `msg.hwnd` 过滤——webview 控件内部有自己的 id=1 帧泵定时器，
  `wParam` 相同会串；引擎线程回调里**禁止 SetTimer**（窗口属主线程，跨线程未定义行为），走 `PostMessage`。
- `WM_DESTROY` 里 `PostQuitMessage`（`GetMessage` 只认 `WM_QUIT`，不会因为窗口没了就返回 0）。
- 调 `agave_webview_eval` 的分支要**防重入**（它内部会泵控件队列，可能再进消息循环）。
- `agave_webview_destroy` 与引擎线程的 `macross_view_update` 竞争会崩：引用在飞时由引擎线程销毁。

## 6. 已知噪音 vs 真错误

| stderr 输出 | 是什么 |
|---|---|
| `[shell] window ready hwnd=...` / `[flag] set js=...` | 启动与引擎标志，正常（探针模式不会再打 window ready） |
| `[shell] atexit fired, bases exe=... agave=... ant=...` + 一串 `#00 exe+0x...` | 退出时的模块基址 + 调用栈，**是给符号化用的基址**；`tools/cdb_stack.ps1 -Symbolize <log>` 可直接解出函数/源码行 |
| `LEAK: N Node` | 文档结束时未释放的 DOM 包装（少量是预期的延迟一代释放） |
| `[snippet] ...` | 可能是正常噪音（core-js 探测），也可能是真错误，看 §3 的判据 |
| `Uncaught (in worker) ...` | worker 内未捕获异常（若页面没接 `onerror` 就是真的） |
| `[probe] headless window hidden ...` | 正常，探针状态行 |
| `[nav] ...` | 默认开启的导航动作日志，正常（`AGB_NAV_LOG=0` 可关） |
| `[con-log]` / `[con-info]` / `[con-warn]` / `[con-debug] ...` | 页面自己的 `console.*` 输出（polyfills 守卫收口，带时间前缀），不是引擎错误 |
| `[con-error] ...` | 同上但级别是 error：页面自己报的错（`console.error`、未捕获的 Promise），先看页面；`AGB_CON_LOG=0` 可静音 |
| `[con-error] Error: `（消息为空） | 页面 `console.error(new Error())` 一类调用，正常噪音 |
| `[evt] ...` / `[ui] ...` | 默认开启的生命周期/输入日志，正常（`AGB_EVT_LOG=0` / `AGB_UI_LOG=0` 可关） |

## 7. 验证闭环：回归清单 + A/B

> 全套件清单（15 个 `run_*.ps1` 的覆盖/依赖/判据、改动→该跑哪些、加回归页的写法、fixture 在哪）：
> [dev-tests.md](dev-tests.md)。下面只列几组最常用的判据。

- 一条命令级回归：`tools/run_worker_tests.ps1`（Worker 九项）；`tools/run_xhr_tests.ps1`
  （XHR 五项：投递重入 R94 + `responseText` UTF-8 解码 R96 + 响应头/`statusText`/
  `readyState`/`overrideMimeType` 与 Chrome 对拍 R97）；`tools/run_nav_tests.ps1`
  （`[nav]` 八项：链接/脚本跳转/历史后退/meta refresh/window.open/表单提交/302 重定向/关日志）；
  `tools/run_typing_tests.ps1`（键盘/IME 八项）；`tools/run_jit_loop_tests.ps1`（长循环 +
  引擎线程运行期间销毁视图）；`debug/csscheck.html`（CSS 能力矩阵）；`debug/life.html`（生命周期）；
  `tools/pages/cap_lifecycle.html`（`document.readyState` 三段 + `document.body` 为 `null`，
  与 Chrome 逐字节对拍，R93）；`tools/pages/cap_currentscript.html`（`document.currentScript`
  在内联/外链/`defer`/`async`/动态/嵌套脚本里的值，R95，`run_script_mode_tests.ps1` 第 5 组）；
  `debug/defercheck.html`；deepseek 登录页（`inp=2 all=129`）。
  回归清单与基线以本文为准；状态总结见 [arch-status.md](arch-status.md)。
- **页面清单**：`debug/` 下 `life.html`、`evalenv_test.html`、`expando_test.html`、`sched.html`、
  `font_test.html`、`csscheck.html`、`clipcheck.html`、`scrollcheck.html`、`beforeunload.html`、
  `defercheck.html`、`newtab.html`、`worker_support.html`；大图 `chat.deepseek.com`、
  `browserscore.dev`。
- **能力缺口基线**：`tools/pages/cap_{dom,css,history,storage_a}.html`（一条命令出当前值，
  见 [arch-capability-gaps.md](arch-capability-gaps.md) §0）；修了某项就更新那里的表格。
- **A/B 对比法**：改动前后结论不一致时，`git stash` → 编出旧 exe → 同参数同页面各跑一次 → 对比退出码
  与 `[exit]` 输出。这比"读代码猜"可靠得多（无头改造就是用这个方法证明零语义变化的）。
