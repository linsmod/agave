# 调试与定位：探针、默认日志、环境变量、cdb、噪音对照

## 1. 无头探针（主力度量工具）

```powershell
agave_shell.exe --probe <url> [--eval <js>] [--wait ms] [--settle ms] [--timeout ms] [--reload n] [--offline] [--resize WxH]
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
- `--resize WxH`：页面 settle 后把控件（连同隐藏的父窗口）改成新尺寸，等 400ms 再跑 eval ——
  复测"窗口变化时页面怎么反应"（重排、百分比、`innerWidth`、`vw/vh`、`@media` 断点、`resize` 事件）。
  这正是拖窗口走的 `WM_SIZE → AGW_CMD_RESIZE → macross_view_resize` 路径，只是窗口从不显示。
  页面与复测命令：`tools/pages/resize/resize_probe.html` +
  `--eval-file tools/pages/resize/check_resize.js`（现状见
  [arch-capability-gaps.md](arch-capability-gaps.md) §2「窗口 resize」）。
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
`--shot <bmp>` 抓的是控件自己的 mirror 缓冲（引擎线程每帧写的那份），**不需要**显示窗口；
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
| `[nav]` 导航动作 | 宿主 `open`/`back`/`forward`/`reload`/`stop`；`request`（type=link/form/back-forward/reload/other + method + URL）；`submit`；`new-window`/`popup create`；`client-redirect`（meta refresh、JS 定时跳转）；`fail provisional`/`fail load`（主资源失败，带 `_mc_error` 码）/`fail resource`（子资源失败）/`cannot implement`（引擎拒绝显示）/`blocked` | `AGB_NAV_LOG=0` | 无（每条只有一行） |
| `[ui]` 界面事件 | resize / focus / blur / mouse down/up / wheel / key down/up / char | `AGB_UI_LOG=0` | `AGB_UI_DEBUG=1` 连 `mouse move` 也打（默认不打，太吵） |
| `[shell]` 宿主 | window ready / 进入与退出消息循环 / `CreateWindowEx` 失败 / `atexit` 模块基址 | — | — |
| `[probe]` 探针 | userdata、headless 状态、加载错误、对话框、退出码 | — | — |
| `[flag]` 引擎开关 | 桥接开关（`uncaught_nonfatal` 等），来自 antjsb.dll | — | — |
| `[style]` 渲染树 | 指定元素的 attach / detach / recalcStyle（是否拿到 renderer、computed display）。**opt-in** | 默认关 | `AGB_STYLE_LOG=1` 打开，`AGB_STYLE_FILTER=<子串>` 按 tag/id/class 过滤 |
| `[cache]` 资源缓存 | 内存缓存 hit/miss、HTTP 缓存 hit/miss、图片 `data`/`notify` 生命周期、绘制时用到的帧状态（`FrameComplete`/`rows=n/m`） | `AGB_CACHE_LOG=0` | — |
| `[dl]` 下载 | `start`/`state`（控制层派发给宿主）/`finish`/`fail`/`cancel`，带 id、字节数、落盘路径 | `AGB_DL_LOG=0` | — |

注意：
- `[ui]` 主要是 GUI 模式的输入（focus/blur/mouse/wheel/key/char）：输入事件在控件窗口过程里产生。
  `--probe` 也会打**控件创建时的 `[ui] resize`**（控件窗口真实存在，只是父窗口 `WS_POPUP` 隐藏）；
  `agave_view_host` 不走控件，所以完全没有 `[ui]`。
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
| 性能 | `AGB_PROFILE=1` / `AGB_PERF_LOG=1` | RIP 采样直方图（退出时 dump `[profjs]`）/ 帧耗时（paint+js > 100ms 才打） |
| 页面脚本 | `AGB_PAGE_EVAL=eval` | 页面脚本回到 direct-eval 语义（**默认 classic script**，顶层 `var`/`function` 进 `window`，R66）；A/B 与 `run_script_mode_tests.ps1` 用 |
| GC | `AGB_GC_OFF=1` | 关掉引擎 GC；"关掉就好"是"对象被提前回收"的决定性判据（R60/R63/R67） |
| Realm | `AGB_PER_FRAME_REALM=0` | 关掉"每 frame 一个 realm"（[design-per-frame-realm.md](design-per-frame-realm.md) S1/S2，**默认开启**）：回到一 view 一 realm + per-frame window 代理的旧模型，用于 A/B。`run_realm_tests.ps1` 覆盖两条路径 |
| 脚本编码 | `AGB_SCRIPT_CHARSET=1` | 每个脚本打印 `[script-charset] <编码> doc=<文档编码>`（排查乱码/重复日志） |
| 绘制诊断 | `AGB_PAINT_FULL=1` | 每帧整屏重绘（忽略脏矩形）；用来区分"失效/裁剪错"和"布局错"（见 §4） |
| 页面 | `AGB_TITLE_DEBUG=1` / `AGB_SCRIPT_DEBUG=1` | `document.title` 轮询 / tokenizer 脚本加载 |
| CSS/布局 | `AGB_CSS_DEBUG=1` / `AGB_LAYOUT_DEBUG=1` / `AGB_BG_DEBUG=1` | 未实现 CSS 属性日志 / 布局 / 背景层 |
| CSS/视口 | `AGB_VIEWPORT_DEBUG=1` | 打印 vw/vh 变量注册（`-agb-vwunit-…`）与每个变量在当前视口下的 px 值，以及含 vw/vh 的 `calc` 模板代入结果（R69） |
| 模块 | `AGB_MODULE_DEBUG=1` | 模块抓取/求值/预取（`[modfetch]`/`[modeval]`） |
| 模块 | `AGB_MODULE_CACHE=0` / `AGB_MODULE_CACHE_TTL` / `AGB_MODULE_CONCURRENCY=n` | 关磁盘缓存 / TTL / 并发槽（默认 12） |
| 模块 | `AGB_OFFLINE=1` | 只从缓存取（`--offline` 就是设它） |
| 网络 | `AGB_NET_LOG=1` | 导航与请求时间线（`net.stage`：crack/connected/openrequest/sending/sent） |
| 网络 | `AGB_NET_DEBUG=1` / `AGB_NET_TIMEOUT_MS` / `AGB_NO_PROXY=1` | 请求细节 / 超时 / 绕开代理解析 |
| 网络 | `AGB_HTTP_CACHE=0\|1` / `AGB_HTTP_CACHE_MB=n` | HTTP 缓存开关（默认开）/ 内存上限（默认 64MB） |
| 网络 | `AGB_SKIPRES=<file>` | 资源跳过表（默认读 `skipres.txt`） |
| 字体 | `AGB_FONT_LOG=1` | `[fontsel] src[i] ... supported=0/1` 与 `[font] beginLoad/data` |
| 控件 | `AGB_LOOP_DEBUG=1` / `AWG_CTX_DEBUG=1` | 控件帧循环 / 引擎上下文切换 |
| JS 文件 | `AGB_PRELUDE` / `AGB_POLYFILLS` | 指向别处的 `prelude.js` / `polyfills.js` 副本 |
| 注入脚本 | `AGB_INSPECTOR` | 覆盖外壳「注入 inspector.js」读取的路径（默认 exe 同目录的 `inspector.js`）。vConsole 固定读 exe 旁的 `vconsole.min.js` |
| profile | `AGB_USERDATA` | 整个 profile 根（cookies/localStorage/模块缓存/HTTP 缓存/shell state）。`--probe` 默认 `%LOCALAPPDATA%\Agave\probe`，`--userdata` 等价 |

## 4. 定位手段

- **想在页面里看 console / 网络 / 元素**：右键菜单或 `=` 菜单 → `注入 vConsole`
  （默认）或 `注入 inspector.js`（hover 高亮 + 半透明信息浮层，Esc 退出）。脚本从
  exe 同目录读，改完再点一次即可；`AGB_INSPECTOR` 换 inspector.js 路径。探针等价：
  `--probe <url> --inject vconsole|inspector`。细节见
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
- **卡死/假死**：设 `AGB_HANG_WATCH=1`（`AGB_HANG_SECONDS` 默认 5）——引擎线程连续 N 秒
  满核就打印 `[hang]` + JS 栈（`file:line`）；窗口无响应但 CPU 不高则是 UI 线程/等待问题，
  用 `tools/cdb_stack.ps1 -Attach <pid>` 看全线程。栈里 `intern_find/hash_key` 说明是
  属性/数组访问热点（脚本 VM），不是死锁。
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
- **像素断言**：`python tools/pixel_check.py <bmp> 250,150,ff0000 450,150,ffffff [--tol 8]`
  —— 读探针 `--shot` 的 BMP（32/24 位、bottom-up），`x,y,RRGGBB` 是截图坐标（左上原点）。
  绘制类回归（裁剪、变换、placeholder 消失）用它，几何用 `layout_diff.py`。
- **布局对拍 Chrome**：`python tools/layout_diff.py tools/pages/layout/<page>.html [--tol 1]`
  —— 同一份 `tools/pages/layout/lib_measure.js` 在 agave 探针和 headless Chrome 里各跑一遍，
  两边视口都固定成 1084×800（agave = 1100 窗口减 16px 经典滚动条；Chrome = `--hide-scrollbars`
  + `Emulation.setDeviceMetricsOverride`），按 `data-case="名字"` 输出
  `名字:left,top,width,height|prop=computed`，逐行比 rect/计算值。用例写在 HTML 里即可
  （`data-props` 指定要读的计算属性）。加页面：`tools/pages/layout/*.html`，一套跑多个：
  `python tools/layout_diff.py tools/pages/layout/*.html`。
- **一条命令截图**：`pwsh -File tools/screenshot.ps1 -Url <url> -Out <png> -Wait 12000`
  —— 内部就是 `agave_shell.exe --probe <url> --shot <bmp>`：探针窗口是隐藏的 `WS_POPUP`，
  从不 `ShowWindow`，抓的是控件的 mirror 缓冲（`agave_webview_paint_to`），所以屏幕上什么都不会出现。
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

- 一条命令级回归：`tools/run_worker_tests.ps1`（Worker 九项）；`tools/run_nav_tests.ps1`
  （`[nav]` 八项：链接/脚本跳转/历史后退/meta refresh/window.open/表单提交/302 重定向/关日志）；
  `tools/run_typing_tests.ps1`（键盘/IME 八项）；`tools/run_jit_loop_tests.ps1`（长循环 +
  引擎线程运行期间销毁视图）；`debug/csscheck.html`（CSS 能力矩阵）；`debug/life.html`（生命周期）；
  `debug/defercheck.html`；deepseek 登录页（`inp=2 all=129`）。回归清单与基线以本文为准；
  状态总结见 [arch-status.md](arch-status.md)。
- **页面清单**：`debug/` 下 `life.html`、`evalenv_test.html`、`expando_test.html`、`sched.html`、
  `font_test.html`、`csscheck.html`、`clipcheck.html`、`scrollcheck.html`、`beforeunload.html`、
  `defercheck.html`、`newtab.html`、`worker_support.html`；大图 `chat.deepseek.com`、
  `browserscore.dev`。
- **能力缺口基线**：`tools/pages/cap_{dom,css,history,storage_a}.html`（一条命令出当前值，
  见 [arch-capability-gaps.md](arch-capability-gaps.md) §0）；修了某项就更新那里的表格。
- **A/B 对比法**：改动前后结论不一致时，`git stash` → 编出旧 exe → 同参数同页面各跑一次 → 对比退出码
  与 `[exit]` 输出。这比"读代码猜"可靠得多（无头改造就是用这个方法证明零语义变化的）。
