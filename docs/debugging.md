# 调试与定位：探针、默认日志、环境变量、cdb、噪音对照

## 1. 无头探针（主力度量工具）

```powershell
agave_shell.exe --probe <url> [--eval <js>] [--wait ms] [--settle ms] [--timeout ms] [--reload n] [--offline]
```

- **eval 时机** = 页面 `window load` 事件 + `--wait`；`on_progress finished` 只是长延迟兜底
  （**加载失败也会报 finished**，别拿它当成功判据）。
- 页面 JS 调 `probeExit(result)` 带结果退出；页面 `console.*` 进 stderr（console guard 自动节流重复警告）。
- `--reload n` 语义：eval → reload → 轮询新文档就绪 → 再 eval，共 n+1 轮。
- `--offline`：模块只从磁盘缓存取，未命中立刻失败；先在线跑一次暖缓存，再确定性重放模块图。
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
`AGB_PROBE_SHOW=1` 才 ShowWindow（隐藏窗口收不到 WM_PAINT，像素/截图类检查必须开它）。
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

## 2. 默认打开的日志（页面生命周期 + 导航动作 + 用户输入）

这三类日志**不需要任何环境变量**：一次运行应该能自己解释发生了什么。

```
[ui] resize 1084x677
[ui] focus
[evt] realm start gen=2 engine=... (view context)
[nav] open url=https://chat.deepseek.com/
[nav] request main type=other method=GET url=https://chat.deepseek.com/
[evt] bindDocument ... (bound=0) url=about:blank
[evt] documentWillUnload doc=... listeners=0
[evt] window event pagehide listeners=0
[evt] window event unload listeners=0
[evt] realm stop gen=2 url=about:blank
[evt] realm start gen=3 engine=...
[evt] commit https://chat.deepseek.com/
[evt] bindDocument ... (bound=1) url=https://chat.deepseek.com/
[evt] window event DOMContentLoaded listeners=3
[evt] window event load listeners=5
[nav] request main type=link method=GET url=https://chat.deepseek.com/sign_in
[nav] popup create url=https://example.com/login
[ui] mouse down left at 320,180
[ui] mouse up left at 320,180
[ui] wheel up at 320,180
[ui] key down vk=0x41
[ui] char 'a'
[ui] blur
```

| 通道 | 内容 | 关掉 | 更详细 |
|---|---|---|---|
| `[evt]` 生命周期 | commit → realm start/stop → bindDocument → pagehide/unload → DOMContentLoaded → load | `AGB_EVT_LOG=0` | `AGB_EVT_DEBUG=1`（evaluate 深度、listener 记账、JS `dispatchEvent`） |
| `[nav]` 导航动作 | 宿主 `open`/`back`/`forward`/`reload`/`stop`；`request`（type=link/form/back-forward/reload/other + method + URL）；`submit`；`new-window`/`popup create`；`client-redirect`（meta refresh、JS 定时跳转）；`fail`/`blocked` | `AGB_NAV_LOG=0` | 无（每条只有一行） |
| `[ui]` 用户输入 | resize / focus / blur / mouse down/up / wheel / key down/up / char | `AGB_UI_LOG=0` | `AGB_UI_DEBUG=1` 连 `mouse move` 也打（默认不打，太吵） |

注意：
- `[ui]` 只覆盖 **GUI 模式**（`agave_shell.exe <url>`）：输入事件在控件窗口过程里产生；
  `--probe` 无窗口、`agave_view_host` 直接调 macross，都不会有 `[ui]` 行。
- `[evt]` / `[nav]` 在 `--probe` 里照样有（与窗口无关）。
- `[nav] request` 的 `type=` 来自 `NavigationAction`：`link`=链接默认动作、`form`=表单提交、
  `back-forward`=历史前进/后退、`reload`=重载、`other`=脚本 `location`/meta refresh/宿主打开。
  **重载时 `type=` 可能仍是上一次动作的类型**（WebCore 复用 `triggeringAction`），宿主发起的重载以
  `[nav] reload` 这行为准。
- **301/302 不会出现 `[nav]`**：native 网络后端让 WinHTTP 自己跟随重定向，WebCore 永远只看到最终
  200 和原始 URL（地址栏也停在原 URL）。要看到重定向链得先改成手动跟随，见
  [handover.md §4 P1-e](../handover.md) 与 §5 限制。
- `[nav] download` 只在会走 `cb_write_file_data` 的后端出现：native 后端目前没有下载链路
  （`FrameLoaderClientMC::startDownload` 仍是 `notImplemented()`），所以看不到这行。
- 这三组日志是**正常输出**，不是错误（见 §6 噪音对照）。

## 3. 环境变量全表

| 组 | 变量 | 作用 |
|---|---|---|
| 探针 | `AGB_PROBE_SHOW=1` | 显示探针窗口（要 WM_PAINT/截图时） |
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
| 崩溃 | `AGB_TRACE_CRASH=1` | 记录最近 256 条内部 trace（崩溃后能回看走到哪） |
| 性能 | `AGB_PROFILE=1` / `AGB_PERF_LOG=1` | RIP 采样直方图（退出时 dump `[profjs]`）/ 帧耗时（paint+js > 100ms 才打） |
| 页面 | `AGB_TITLE_DEBUG=1` / `AGB_SCRIPT_DEBUG=1` | `document.title` 轮询 / tokenizer 脚本加载 |
| CSS/布局 | `AGB_CSS_DEBUG=1` / `AGB_LAYOUT_DEBUG=1` / `AGB_BG_DEBUG=1` | 未实现 CSS 属性日志 / 布局 / 背景层 |
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
| profile | `AGB_USERDATA` | 整个 profile 根（cookies/localStorage/模块缓存/HTTP 缓存/shell state）。`--probe` 默认 `%LOCALAPPDATA%\Agave\probe`，`--userdata` 等价 |

## 4. 定位手段

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
- **抓挂起线程栈**：`tools\cdb.exe -p <pid> -c '~*k 30; qd'`（探针有超时会秒退，attach 要趁早）。
- **断在首次异常**：写命令文件 `sxe av` / `g` / `~*kb 20` / `q`，再
  `tools\cdb.exe -y "<build-zig-native>;H:\ant-v16\bridge" -cf cmds.txt build-zig-native\agave_shell.exe --probe ...`
  —— **不要用 `--` 分隔**（这版 cdb 报 0x80070057），也别用 Start-Process 传 `-c "长命令"`（引号被吞）。
  `antbridge.dll` 旁边有匹配的 `antbridge.pdb`，`-y` 指过去就能解出 `process.c @ 2355` 这种帧。
- **CPU 热点**（连 ant 内部符号一起解）：`AGB_PROFILE=1` 跑探针拿 `[profjs]` 直方图；antbridge 默认
  没调试信息时用 `-g` 重链出 `ab_dbg.dll` + `ab_dbg.pdb`，`cdb -y <dll目录> -z antbridge.dll -c "ln antbridge+0x<rva>; qd"`
  出函数/源码行；在线抓栈用 `cdb -y <dll目录> -p <pid> -c "~*k 30; qd"`。
- **地址栏输入 → URL**：`examples/browser_shell/main.c` 的 `normalizeUrl()` 是唯一入口（地址栏/Go/收藏夹/
  `--probe` 都走它）。本地路径 → `fileUrlFromLocalPath()`（`GetFullPathNameW` + `\`→`/` + 逐字节百分号转义）；
  `file:` URL 在引擎侧只能**单次转义**（`WebView::loadUrl` 会对整串调 `KURL::encode_string`，它必须幂等）。
  调试入口：`[probe] main url=` 就是引擎最终 URL；运行时的导航决策看 `[nav]`（见下一条）。
- **导航去了别处 / 没发生**：按 `[nav] open`（宿主发起）→ `[nav] request type=... method=...`（引擎分类）
  → `[evt] commit`（真正提交）的顺序找断点。`popup create`/`blocked` 是弹窗，`client-redirect` 是
  meta refresh / JS 定时跳转；`[nav] back/forward/reload/stop` 是宿主的历史与重载。301/302 不会出现在
  `[nav]` 里（§2 的原因），要查重定向链先用 `AGB_NET_LOG=1` 或抓包。
- **worker**：独立线程 + 独立 isolate，崩溃栈在**非主线程**（`~*kb` 里找 `worker_thread_main`）；
  worker 入口在 ant 侧走 ESM/CJS，栈帧名对 data: URL 折成 `data:script/<hash>`（看到它=worker 内的代码）。
  回归：`pwsh -File tools\run_worker_tests.ps1`。
- **网络卡点**：`AGB_NET_LOG=1` 看卡在 crack/connected/sending 哪一步；WinHTTP `SendRequest` 偶发长滞是
  WPAD/PAC 环境抖动，同刻 GUI 正常≠代码回归，先重试。
- **字体**：`AGB_FONT_LOG=1`；回归页 `tools/pages/iconfont_render.html`，
  `agave_view_host.exe <file-url> out.bmp` 出图核对字形，链路见 [web-font.md](web-font.md)。

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
| `[shell] atexit fired, bases exe=... agave=... ant=...` + 一串 `#00 exe+0x...` | 退出时的模块基址 + 调用栈，**是给符号化用的基址**，不是崩溃 |
| `LEAK: N Node` | 文档结束时未释放的 DOM 包装（少量是预期的延迟一代释放） |
| `[snippet] ...` | 可能是正常噪音（core-js 探测），也可能是真错误，看 §3 的判据 |
| `Uncaught (in worker) ...` | worker 内未捕获异常（若页面没接 `onerror` 就是真的） |
| `[probe] headless window hidden ...` | 正常，探针状态行 |
| `[nav] ...` | 默认开启的导航动作日志，正常（`AGB_NAV_LOG=0` 可关） |
| `[evt] ...` / `[ui] ...` | 默认开启的生命周期/输入日志，正常（`AGB_EVT_LOG=0` / `AGB_UI_LOG=0` 可关） |

## 7. 验证闭环：回归清单 + A/B

- 一条命令级回归：`tools/run_worker_tests.ps1`（Worker 九项）；`tools/run_nav_tests.ps1`
  （`[nav]` 七项：链接/脚本跳转/历史后退/meta refresh/window.open/表单提交/关日志）；
  `debug/csscheck.html`（CSS 能力矩阵）；`debug/life.html`（生命周期）；`debug/defercheck.html`；
  deepseek 登录页（`inp=2 all=129`）。完整清单与基线见 [handover.md §3](../handover.md)。
- **A/B 对比法**：改动前后结论不一致时，`git stash` → 编出旧 exe → 同参数同页面各跑一次 → 对比退出码
  与 `[exit]` 输出。这比"读代码猜"可靠得多（无头改造就是用这个方法证明零语义变化的）。
