# 测试清单：有哪些套件、跑什么、怎么判

> 这份文档只回答"项目里有哪些测试、改动后该跑哪些"。单个用例的根因/历史看
> [hist-lessons.md](hist-lessons.md)；探针参数、日志、环境变量全表看
> [dev-debugging.md](dev-debugging.md)；加回归页的写法见 §4。

测试分三层，各自解决不同问题：

| 层 | 是什么 | 入口 |
|---|---|---|
| **一键套件** | `tools/run_*.ps1`：起服务/起探针/断言/收尾，退出码是判据 | §1 |
| **对拍工具** | 同一个页面 / 同一段 JS 在 agave 和 headless Chrome 各跑一遍，diff 结果 | `tools/layout_diff.py`（整页几何）、`tools/chrome_probe.py`（单点/规则/字体/截图） |
| **fixture 页面** | `tools/pages/**`（进套件、要稳定）、`debug/**`（临时复现、不进套件） | §4 |

## 1. 一键套件总表

### 全部跑一遍

```powershell
pwsh -File tools/run_all_tests.ps1                          # 24 个离线套件，默认全并行 ≈30 s，退出码 0 = 全绿
pwsh -File tools/run_all_tests.ps1 -Sequential              # 套件一个一个跑（≈2 min，调试输出用）
pwsh -File tools/run_all_tests.ps1 -Parallel 8              # 套件间并发度（1..32，缺省=逻辑核数）
pwsh -File tools/run_all_tests.ps1 -Only process            # 名字含 process 的
pwsh -File tools/run_all_tests.ps1 -Skip run_layout_tests,run_media_tests
pwsh -File tools/run_all_tests.ps1 -IncludeNetwork -IncludeReal   # 加上真站点 / ffmpeg 套件
pwsh -File tools/run_all_tests.ps1 -KeepLogs                # 保留每个套件的 stdout/stderr
```

默认**全并行**（`-Parallel` 缺省 = 本机逻辑核数，上限 16）：24 个套件同时起，每个套件是独立的

> ⚠ **16 路并发下的时序抖动**（2026-11-10 实测）：`run_window_tests`（process 命名 target）、
> `run_process_backend_tests`（early-eval-after-load-html）、`run_hidden_view_tests`、`run_image_tests` 偶尔各失败 1 条，
> **单跑全绿**；同机 4 次全量里 2 次红、且红的是不同套件，`-Parallel 4` 连跑 **34.7 s 0 failed**。
> 集中跑时若只看到这几条、单跑通过，按抖动处理；同一套件连续两次失败才算回归。
> 量化证据（2026-11-10）：`run_hang_recovery_tests` 在整批里 **52.4 s 且失败**、单独跑 **6.9 s 全绿**——它断言的是命令延迟预算，
> 被并发拖慢 7.6× 必然超预算。5 次默认全量里 3 次红，红的是**不同**套件（window/process/hidden/image/hang-recovery）。
> 根治方向（建议 runner 维护者定夺）：把断言时间预算的套件（hang-recovery/process/window/hidden/image）放回串行，
> 或把 `-Parallel` 默认降到 4~6（实测 4 路 **36.5 s 稳定全绿**），或把 `AGB_TEST_MAX_PROBES`（默认 32）降到 8~12。
`pwsh` 进程；套件内部再由 `tools/TestParallel.psm1` 把用例一起启动。每个探针进程拿到自己的：

| 资源 | 每用例的值 | 谁写 |
|---|---|---|
| `AGB_USERDATA` | `build-zig-native\test-runs\<suite>-<id>\<case>\profile` | 探针（cookie/cache/localStorage 不共享） |
| cwd | `<case>\` | 探针（`agb_crash.txt` 落在自己的目录） |
| `AGB_DIAG_DIR` | `<case>\diag\` | 探针（`agb_watchdog.txt` / `agb_busy_*.txt`） |
| `AGB_CLIPBOARD_FILE` | `<case>\clipboard.txt` | 探针（**模拟剪贴板**，不读也不写系统剪贴板） |
| `AGB_RENDER_TAG` | `<runTag>-<case>` | 探针（“没有残留 renderer”逐用例成立） |
| stdout/stderr/eval 文件 | `%TEMP%\agb-test-logs\<suite>-<id>\` | 宿主 pwsh |

所以**没有串行阶段了**：`run_hang_recovery_tests` 只看自己用例目录里的 crash/watchdog 文件；
`run_process_backend_tests` 的崩溃用例有独立 cwd/diag，最后做一次聚合的 renderer 残留检查；
`run_image_tests` 的计时改成“同波次一次解码”的相对比较 + 两波（像素组先跑完，再跑 4 个计时探针）；
`run_selection_tests` 的复制用例各写各的剪贴板文件。跨进程的探针总数由
`AGB_TEST_MAX_PROBES`（默认 24）用 `test-runs\.gate\` 里的 slot 文件限制；slot 与探针 PID 绑定，
套件被 kill 后下一个等待者会自动清掉，不会死锁。失败时默认保留该套件的日志（路径会打印），
`-KeepLogs` 连成功的也留。

> 写盘经验：把每个探针的 stdout/stderr 重定向到工作区（`build-zig-native\test-runs\`）会让
> 16 个简单探针从 ~0.6 s 变成 ~2.8 s（工作区有实时扫描/文件过滤）；日志因此放 `%TEMP%`，
> 探针自己写的 profile/截图/剪贴板文件才留在工作区（探针沙箱只保证能写这里）。

默认排除"要外部条件"的三个：`run_site_smoke`（真站点，网络）、`run_media_real_tests`（ffmpeg +
远程样片）、`run_c3`（外部工作区 + 8899 端口，本来就不是标准回归）。每个套件打印
`PASS/FAIL + 秒数`，最后给最慢的 5 个和总数；单个套件超过 180 s 会打 `WARN`（AGENTS rule 9）。
套件内的 HTTP/py 服务默认绑随机空闲端口（`-Port` 仍可指定），并发的批跑不再抢 8971/8973/8991…。

> ✅ **2026-11-10**：旧的四个“串行套件”冲突已经全部用隔离消掉，默认整批从 ~250 s 降到
> **~23–28 s**（20 套件，16 核本机，3 次连跑 23.2/25.9/27.8 s）。`-Sequential` 仍保留（套件一个
> 一个跑，便于看单套件输出），`-Parallel 1` 只影响套件间并发。

**软基线**（2026-11-10 实测，16 核本机；**不是断言**；下表是**单个套件单独跑**的墙钟，
同一套件的用例已并行）：

| 套件 | 单独跑 | 套件 | 单独跑 |
|---|---:|---|---:|
| `run_process_backend_tests` | 14 s | `run_xhr_tests` | 6 s |
| `run_image_tests` | 11 s | `run_window_tests` | 5 s |
| `run_layout_tests` | 7 s | `run_nav_tests` | 5 s |
| `run_hang_recovery_tests` | 8 s | `run_typing_tests` | 5.5 s |
| `run_jit_loop_tests` | 5 s | `run_script_mode_tests` | 5.5 s |
| `run_hidden_view_tests` | 5 s | `run_realm_tests` | 4 s |
| `run_inspector_tests` | 7 s | `run_tab_lifecycle_tests` | 5 s |
| `run_worker_tests` | 6 s | `run_media_tests` | 5 s |
| `run_sticky_tests` | 3.5 s | `run_encoding_tests` | 5 s |
| `run_selection_tests` | 8 s | `run_binding_semantics_tests` | 3 s |
| `run_shadow_tests` | 2 s | `run_webp_tests` | 2 s |
| `run_singleton_tests` | 2 s | | |
| **23 套件整批（默认并行）** | **~25 s** | **`-Sequential`（套件逐个）** | **~2 min** |

规则（软基线，靠人/Agent 判断，不做程序化对比）：集中跑时除了看 PASS，还看每套件秒数——
**单套件比基线慢 30 s 以上、或整批慢于 ~45 s 就在对话里提示**（AGENTS rule 9），顺带查一下是
机器忙、测试真的退化还是新用例加多了。新增套件请控制在十几秒内并在这里补一行；如果它需要独占
某个资源（固定端口、系统剪贴板、真实设备、单实例程序），先优先改成隔离/模拟；确实不能隔离的
用例在脚本里显式串行，并在注释和本表里写明原因。

`run_site_smoke`（真站点，网络）与 `run_media_real_tests`（ffmpeg + 远程样片）本机没测出稳定值，
需要时用 `-IncludeNetwork` / `-IncludeReal` 单独跑。

### 单个套件

全部用 `pwsh -File tools/<脚本>` 跑，**退出码 0 = 通过**（`run_c3.ps1` 例外，见下）。
`run_media_*` / `run_sticky_tests` 等脚本里的 Chrome 数值是写死的基准，不需要本机 Chrome；
只有 `run_layout_tests.ps1` 会真的启动 Chrome。

| 脚本 | 覆盖 | 依赖 | 判据 / 输出 |
|---|---|---|---|
| `run_layout_tests.ps1` | **主回归**：`tools/pages/layout/*.html` 全部与 headless Chrome 比几何（`data-case`）、几组像素断言（变换重绘、变换文字、背景、**多层背景/渐变描边/阴影 spread/textarea 占位符**（R189）、**box-shadow 连续重绘不叠加变黑**（R191）、圆角、滚动条隐藏、vw/vh/resize、CSSOM、computed 简写序列化、flex computed、DOM 生命周期）、**文字灰度抗锯齿**（`tools/font_aa_check.py` + `tools/pages/font_antialias.html`，R167） | Chrome、python `websockets` | 每组打印 `N/N checks ok`，最后 `LAYOUT=PASS`；失败打印 `LAYOUT=FAIL (n)`；约 8 s |
| `run_inspector_tests.ps1` | 注入 `inspector.js`：菜单项、行命中、元素信息 JSON、剪贴板；真实 `WM_RBUTTONDOWN/UP`（页面 contextmenu 必须被取消、壳的原生菜单不弹）与真实左键点击；剪贴板走 per-case `AGB_CLIPBOARD_FILE`，鼠标事件发送后要看到 `[ui] mouse down …` 才继续，失败重发 | 无 | `inspector: n/n checks passed`；四个探针并行、鼠标用例顺序驱动，约 7 s |
| `run_nav_tests.ps1` | `[nav]` 通道八项：链接点击 / 脚本跳转 / 历史后退 / meta refresh / `window.open` / 表单提交 / 302 / `AGB_NAV_LOG=0` 静音 | 自起 python http.server（8973） | `ALL NAV CHECKS PASSED (n cases)` |
| `run_xhr_tests.ps1` | XHR 投递重入（R94）、`responseText` UTF-8 解码（R96）、响应头/`statusText`/`readyState`/`overrideMimeType` 与 Chrome 对拍（R97）、**同一响应多条 `Set-Cookie` 全部可见**（R190，`tools/multi_cookie_server.py`） | 自起静态服务（8991）+ 多 cookie 服务 | `ALL XHR CHECKS PASSED` |
| `run_encoding_tests.ps1` | 文档编码优先级（R164）：BOM > HTTP `Content-Type` charset > `<meta charset>`/`@charset` > 默认；`document.characterSet` 报真实规范名 | 自起 `tools/encoding_server.py`（8993，`-Port` 可换） | `ALL ENCODING CHECKS PASSED (5 cases)`，约 9 s |
| `run_worker_tests.ps1` | Worker 九项：http worker、`importScripts`、blob、terminate、subclass、error 事件、structured clone | 自起服务（8971，可 `-Port`/`-KeepServer`） | `ALL WORKER CHECKS PASSED`；setup 失败 exit 2 |
| `run_typing_tests.ps1` | 键盘/IME：走真实 `WM_KEYDOWN/WM_CHAR/WM_KEYUP` 与 `WM_IME_CHAR`（`--keys`/`--ime`）；**Enter 的 keypress(13:13) + textarea 单换行/新行光标**、内联 `onkeydown`/`onclick` 属性 handler（`cap_inline_events.html`）；末尾两条量**光标几何**（`AGB_CARET_DEBUG=1` 红色光标 + `tools/caret_check.py`，输入框光标=文字高度而不是盒子高度） | 无 | `ALL TYPING CHECKS PASSED (11 cases)` |
| `run_selection_tests.ps1` | 文本选区与快捷键（R175）：真实拖选/反向拖选/跨段落、双击选词 + `dblclick`、三击选段、`--shift-click`、`user-select:none`/嵌套 `text`、Ctrl+A/C/V/X、Shift+方向键、Ctrl+Shift+方向键、`--editor` Copy/SelectAll、`window.getSelection`/`Range`/`execCommand` 与 Chrome 对拍字段、选区高亮像素；`-Process` 再加 6 个 process 后端用例 | 无（**会覆盖系统剪贴板**） | `ALL SELECTION CHECKS PASSED (n cases)`；local 约 28 s，`-Process` 再加约 15 s |
| `run_jit_loop_tests.ps1` | 长 JIT 循环 + "引擎线程仍在跑 EVAL 时销毁视图"（R61 的 0xC0000005） | 无 | `ALL JIT LOOP CHECKS PASSED` |
| `run_hang_recovery_tests.ps1` | 卡死页恢复：`--interrupt-after` 中断、排队导航中断卡死脚本、空闲页不误中断、慢页连续 reload 不被杀 | 自起服务（8982/8984） | `ALL HANG RECOVERY CHECKS PASSED` |
| `run_script_mode_tests.ps1` | classic script 语义（顶层 `var`/`function` 进 `window`，R66）+ 热点循环性能 + `document.readyState`/`currentScript`；**模块侧**：frame 模块入口 / 每 realm `import()`、`import(` 开头的 classic 脚本、**HTML import map**（精确 + 前缀 + 相对 + 未映射 reject）、**`document.baseURI`/`Node.baseURI`**（含 `<base href>`）、**URLSearchParams 包装类迭代 + frame realm 的 web 全局**（R176）、**探针生命周期 `--wait-dcl`**（prelude 的 DCL 标记只在显式开关时被消费，eval 由 DCL 而非 load 触发）、**`window.__agbInfo()`**（pid/view/engine/realm/document/generations/slot/mainFrame + args/AGB_* env + backend；子 frame 报自己的身份；process 后端通过 `CONTEXT` 事件报出与页面相同的 doc 地址，`tools/pages/dcl_probe.html`、`agbinfo_frame.html`） | 无（Chrome 数值为注释里的基准） | `ALL SCRIPT MODE CHECKS PASSED`（17 项检查，约 4 s） |
| `run_webp_tests.ps1` | WebP 解码（issues #19）：`tools/pages/fixtures/webp_{lossy,lossless,alpha}.webp`（各 40×30，Pillow `method=0` 生成）经 `new Image()` 走真实资源加载路径，断言 `naturalWidth/Height`。~2 s。⚠ 只解**首帧**，动画 WebP 与 AVIF 未做 | fixture 在 `tools/pages/fixtures/` |
| `run_binding_semantics_tests.ps1` | DOM 绑定语义（B1 + B2）：每接口一个 class + 真实原型链、构造器、`@@toStringTag`、`instanceof` 正反例、`constructor`/`Object.prototype.toString`、方法按接口归属、接口对象描述符（`prototype` 不可写、`constructor` 非枚举）、非法构造器抛 `TypeError`、门面（id/class/value/onclick）不回归、polyfill 落在 `Element`/`CharacterData` 原型上、CSSOM/Selection/Range/Canvas 的既有类可 `instanceof`；**B2-1**：`window` 是 `Window` 实例（原型/构造器/`[object Window]`/`instanceof EventTarget`）、事件层级（`MouseEvent → UIEvent → Event`、`KeyboardEvent`/`FocusEvent`/`InputEvent`/`WheelEvent`/`CustomEvent`）、引擎派发 click 是 `MouseEvent` 且 `target`/`currentTarget`/`eventPhase` 在 handler 内正确、冒泡到父节点、派发后复位、页面 `dispatchEvent` 的 `isTrusted=false` 与可取消返回值；**B2-2**：`querySelectorAll` 是静态 `NodeList`（非数组、`item`/`forEach`/迭代器）、`children`/`childNodes`/`getElementsByTagName` 是 live 集合且 identity 稳定、`getElementsByClassName` 按 token 匹配、`document.forms` 是 live `HTMLCollection`、`namedItem`、`new NodeList()` 抛 `Illegal constructor`；**B2-3**：常见属性在接口原型上是 WebIDL accessor（descriptor 形状、`in`、`for-in`、读写仍落到 DOM、只读属性不产生 own prop、expando 仍可写）；已知缺口（`element.style` 的值仍是 shim）单独断言。**A1/A2 生成表**：`AGB_BINDING_CHECK=1` 再跑一次探针，断言 6 个接口（Node/Element/HTMLElement/Document/HTMLCollection/NodeList）的成员数与 `0 missing owner(s)`；页面另断言成员**归属**（`id` 在 Element、`style` 在 HTMLElement、`attributes` 在 Element 而非 Node，Chrome 对拍值） | 无（`tools/pages/cap_binding_semantics.html`） | `ALL BINDING SEMANTICS CHECKS PASSED`（205 条断言 + generated tables，约 3 s） |
| `run_shadow_tests.ps1` | Shadow DOM / DOM2 Traversal / Fullscreen（R188）：shadow 内样式不泄漏、页面样式不进组件树、`:host`/`:host(...)`/`::slotted()`/`::part()`、`assignedNodes`/`assignedSlot`、`host.innerHTML` 只换 light DOM、`shadowRoot.innerHTML` 只换组件树、closed root；`slotchange` 与 `fullscreenchange` 的异步语义（第二个探针读 `window.__asyncResult`）；`createTreeWalker` 的 SHOW/FILTER/REJECT/SKIP/root/currentNode | 无（`tools/pages/cap_shadow_dom.html` + `cap_shadow_async.html`） | `ALL SHADOW CHECKS PASSED`（30 + 10 条断言，约 2 s） |
| `run_realm_tests.ps1` | 每 frame 一个 realm（`design-per-frame-realm.md`）：先编译 `tools/ant_realm_probe.c` 直接测桥（一 isolate 三 realm），再走 iframe 页面；**B2-4** 断言 `child.HTMLElement !== parent.HTMLElement`、`child.HTMLElement.prototype !== parent.HTMLElement.prototype`（每 realm 一套接口类） | 本机编译器（脚本自己编 `ant_realm_probe.exe`） | `ALL REALM CHECKS PASSED` |
| `run_tab_lifecycle_tests.ps1` | 关 view/tab 的生命周期（R141，`design-tab-teardown.md`）：teardown 顺序 `pagehide → visibilitychange → unload → realm stop`、每个 `realm start` 都有 `realm stop`、**JS 创建的节点不 pin 住 Document**（无 `LEAK:` 行）、`window.open` popup 是独立 view context 且同样拆除、字体下载在途时销毁 view 不崩且 `documentDetached` 命中 | 自起 `tools/tab_lifecycle_server.py`（8979，`/slow.ttf` 延迟 1.5s） | `ALL TAB LIFECYCLE CHECKS PASSED (n cases)` |
| `run_process_backend_tests.ps1` | `process` 渲染后端（S3 B2a/B2b），26 个用例；含 `callback-thread`（R185：状态回调必须在拥有窗口的线程上到达，否则宿主会跨线程碰 Win32 窗口）：'一个 view 一个 `agave_render_host.exe`，协议走命名共享段（canvas + 命令环 + 事件环 + 文本 arena + 大载荷共享段）。**全部用例并行**（缺省 `-Parallel 16`，并受全局 `AGB_TEST_MAX_PROBES` 约束；每个用例独立 `AGB_USERDATA`、cwd、`AGB_DIAG_DIR`、`AGB_CLIPBOARD_FILE`、`AGB_RENDER_TAG`，产物落在 `process-tests-*`，成功后删除）。crash/respawn/crash-report 不再串行（`agb_crash.txt` 在各自 cwd）；protocol stats / multiview 的延迟阈值放宽为“只抓通道坏了或极慢”，最后做一次聚合的 renderer 残留检查。断言页面 JS/title/定时器、`--shot` 像素（共享 canvas → mirror）、`--keys` 输入跨边界、**32 KB 注入资产整段到达**（`--inject inspector`，`window.__agaveInspector` 存在且 `inspectAt` 命中：R169 的大载荷通道）、**300 KiB `--eval-file` 首尾标记都在**（不再有 4 KB/8 KB/32 KB 的静默截断）、**286 KB vConsole 注入、挂载并点开面板**（同时覆盖 R170 的 focus-after-DOM-mutation 和 R172 的 `document.createEvent`）、**页面复制到 OS 剪贴板**（R171）、**back/forward 计数跨进程**、渲染进程崩溃报 load error（probe 立即退出 5，不等到 timeout）、**双 view：A 卡死时 B 的命令 31 ms 回答、`agave_webview_interrupt(A)` 31 ms 生效**、local 对照 `blocked=1`、崩溃后视图可 respawn、**关掉卡死 tab 立即生效（destroy_ms=0）**、`--backend process` 参数与 launcher（`tools/agave.ps1`，默认 process）走同一条路、未知后端被拒绝、默认 local 后端不退化、**命令不会静默消失**（无页面时的 eval 报 `dropped`；navigate 刚返回就 eval 要么执行要么报错；`AGB_IPC_STALE_EPOCH=1` 下渲染进程拒收过期代数命令，宿主收到 `on_command_dropped`）；**崩溃页报告**（`AGB_RENDER_CRASH_AV` 制造真 AV：环境/异常码/符号栈进共享段，`on_crashed` 把它交给宿主，sad tab 的 HTML 落盘再加载读回）、**右键菜单跨进程**（`--right-click`：渲染进程 `cb_context_menu` → `CONTEXT_MENU` 事件 → 宿主 `on_context_menu`，断言 `[ctx] menu … link=…`）、**内部页跨进程**（`agave://downloads` 导航 + `--reload 2` 仍是 `title=下载`：scheme provider 让 reload 走 loader 而不是网络）、**P1-ac**（`AGB_PROBE_CRASH_EVAL=1`：`load_html` 后立刻 eval 只报 `dropped`，不得 ASSERT/崩溃）、每例后不得残留 renderer 进程（25 例，全并行约 14 s） | 无（`tools/pages/cap_hidden_view.html` / `cap_typing.html` / `inspector_demo.html` / `window_links.html`） | `ALL PROCESS BACKEND CHECKS PASSED` |
| `run_window_tests.ps1` | 窗口生命周期（R158），**local 与 process 两个后端各跑一遍**：普通链接同 tab 跳转；`target="_blank"` 开新 tab 并加载出标题、主 tab 不变；`window.open(url)` 同样开 tab、页面拿到窗口对象；具名 target 点两次只开**一个**窗口，第二次导航它并回调 `on_activate_view`（宿主切 tab）；popup 里 `window.close()` 到达宿主。每例后不得残留**自己 tag 的** renderer（`AGB_RENDER_TAG`，别人的壳不受影响，R159） | 无（`tools/pages/window_*.html`） | `ALL WINDOW CHECKS PASSED` |
| `run_singleton_tests.ps1` | 单实例壳的**逻辑**（不是 UI，`design-shell-singleton.md`；全部 `--probe`，经 `TestParallel.psm1` 并行）：profile 身份（`X`/`X\`/`X\.` 同一 key、不同目录不同 key）、接力决策（无 URL → focus、裸 URL/`--new-tab` → new-tab、`--new-window` → new-window）、ack 处理（ok / `starting` 重试 / error → exit 3 / 无 ack 快速失败）；运行中的壳用 `AGB_HANDOFF_MOCK_DIR` 文件 mock（与 `AGB_CLIPBOARD_FILE` 同一手法），不创建任何窗口/控制台/壳进程。真 WM_COPYDATA/多窗口 UI 路径不在此套件（见设计记录 §9） | 无 | `SINGLETON=PASS`，约 2 s |
| `run_shell_ui_tests.ps1` | shell chrome 的无头离屏渲染（`design-shell-ui.md`）：亮色 / 暗色 / 150% DPI / 暗色+自绘菜单 / 暗色+焦点胶囊五份 PNG；`--ui-shot` 自报 34–61 项断言——标签/关闭/`+`/后退/菜单/地址胶囊/查找按钮/**窗口 min-max-close** 命中、DOWN+UP 回调（`ID_BACK`/切换/关闭/新标签/最大化/关闭窗口）、hover、按钮/标签/内容区几何不重叠、亮暗与 DPI 的像素/度量（tabstrip 40/60、toolbar 44/66、活动标签 surface、状态栏 surface_alt）、**暗色层次**（strip < toolbar、胶囊 < toolbar、hover > 活动标签、焦点环 `#A8C7FA`）、**自绘菜单**（`--ui-menu`：锚定/宽度、子菜单翻转、行/分隔行命中、方向键跳过禁用/分隔行、Right/Left 进出子菜单、点选/点外取消、hover/背景像素、圆形工具栏 hover）、**焦点环**（`--ui-focus`：胶囊填充 + 环像素）；**窗口框架的 NC 点击**不在离屏测试里（需真实 HWND，用 `AGB_SHELL_HIDDEN=1` 手工发 `WM_NCHITTEST`/`WM_NCLBUTTONDOWN`/`WM_LBUTTONUP` 验证；菜单宿主的真实 HWND 路径同理用 `tools/check_shell_menu.ps1`） | 无（不建窗口/引擎/profile；子进程 `AGB_SHELL_OPS=mock`） | `ALL SHELL UI CHECKS PASSED`；每份约 1.2–1.5 s，五份并行 ≈1.5 s |
| `run_hidden_view_tests.ps1` | 隐藏 view（后台 tab / 最小化窗口）跳过布局与绘制（R150）：`--hide-after` 后定时器继续跑、`document.hidden`/`visibilitychange` 正确、`AGB_LOOP_DEBUG` 的 `[loop] paints=/skips=` 证明 paint 冻结而 skip 增长；无隐藏的对照跑 paint 持续增长；`--hide-after`+`--show-after`+`--shot` 证明回到可见后首帧整幅重绘（隐藏期间改色的方块在截图里必须是新色） | 无（`tools/pages/cap_hidden_view.html`） | `ALL HIDDEN VIEW CHECKS PASSED` |
| `run_sticky_tests.ps1` | `position:sticky`：视口/内层滚动容器/containing block 边界、命中跟随（R82），几何 + 像素 | 无（期望值来自 Chrome） | `STICKY=PASS` |
| `run_image_tests.ps1` | 图片解码（R136–R140）：切片解码与一次性解码**逐字节同像素**（JPEG + PNG）、**工作线程解出的帧与内联帧逐字节一致**且 paint 里不再解码、大图不再单帧停摆（paint 上限 / rAF gap）、L3 缩放解码的加速比、**拆文档时进行中的解码不崩且队列排空**、**零字节图片资源（204 / `Content-Length: 0` / 截断头）只算坏图不崩、且不会每帧重解**、`AGB_IMG_DECODE_MS=0`/`AGB_IMG_NO_SCALE`/`AGB_IMG_SAMPLE_MAX` 开关有效 | 无（脚本自己用 System.Drawing 生成 4000×3000 JPEG / 3000×2000 PNG 到 `build-zig-native/img_test`；零字节那组自起 `tools/empty_image_server.py`，随机空闲端口） | `IMAGE=PASS` |
| `run_media_tests.ps1` | 媒体元素 API/几何/传输（play → timeupdate → ended）；有 ffmpeg 走真解码，没有则内部 ticker | 无（ffmpeg 可选） | `MEDIA=PASS` |
| `run_media_real_tests.ps1` | 真解码回放：`currentTime` 走墙钟、暂停/续播、seek、`ended`、画面与 ffmpeg 同位置解码一致 | ffmpeg（缺失则跳过像素项）、网络（远程样片；`-SkipRemote` 可关） | `MEDIA-REAL=PASS (n skipped)` |
| `run_site_smoke.ps1` | 真实站点冒烟：deepseek / baidu / vqq 的加载、JS 执行、DOM 规模（vqq 带 `AGB_GC_OFF=1`，见 R68） | **网络** | `ALL SITE SMOKE PASSED`；每站打印 `all=元素数` |
| `run_c3.ps1` | css3test 本地镜像的**临时**跑法（硬编码了 harness 工作区与 8899 服务，不属于标准回归） | 外部工作区 + 本地服务 | 打印探针输出，无 PASS/FAIL |

**手动检查（不进默认套件）**：`pwsh -File tools/check_shell_frame.ps1` 用
`AGB_SHELL_HIDDEN=1` 起真实 GUI（窗口不显示），验证自定义窗框：client origin=0、
`WM_NCHITTEST` 的 min/max/close/tab/caption 码、`WM_NCLBUTTONDOWN + WM_LBUTTONUP`
的 max/restore/min/close，以及 `[shell] window frame extended=1`
（`DwmExtendFrameIntoClientArea` 成功；客户区覆盖整个窗口，白边不靠改颜色）。
需要真实 HWND，所以不放进 probe-only 的默认回归。
同一个启动方式下，`pwsh -File tools/check_shell_menu.ps1` 验证自绘菜单的分层弹窗
路径：`☰` 打开应用菜单 → 首行点击生效；页面右键打开上下文菜单 → Esc 关闭；
`WM_CLOSE` 干净退出。`pwsh -File tools/check_shell_tabsearch.ps1` 验证标签条的
v 入口打开"搜索标签"弹层、Esc 关闭；`pwsh -File tools/check_shell_findbar.ps1`
验证查找栏是浮层（页面不被压高）。三者都约 2–3 s。

⚠ `check_shell_frame.ps1` 的 `click-min` 是最小化路径的唯一覆盖：它曾经因为
"最小化后客户端高 1px → 标签条可用宽度为负 → 整数除零"而稳定 FAIL，别再把它
当成脚本竞态跳过。同一脚本的 `min-no-tiny-view-resize` 断言最小化期间**没有**
`[ui] resize` 落到 100px 以下（否则每次最小化/还原都会把视图缩到 1x1 再还原：
DIB 重建 + 页面在 1x1 上重排各一次）。

## 2. 改动 → 该跑什么

| 改动 | 至少跑 |
|---|---|
| 渲染 / 布局 / CSS 解析 / 字体度量 | `run_layout_tests`（几何 + 像素全组）、`run_site_smoke` |
| 命中测试 / layer / 绘制裁剪 / z-order | `run_layout_tests` 的像素组、`run_sticky_tests`、`run_inspector_tests` |
| 图片解码 / 缩放绘制 / 图片在页面里的渐进出现 | `run_image_tests`、`run_layout_tests`（含图片的像素组）、`run_sticky_tests` |
| DOM / JS 绑定 / 事件 / 生命周期 | `run_script_mode_tests`、`run_realm_tests`、`run_xhr_tests`、`run_jit_loop_tests`、`run_tab_lifecycle_tests` |
| 关 tab / 视图销毁 / 页面资源清理 | `run_tab_lifecycle_tests`、`run_nav_tests`、`run_jit_loop_tests`、`run_image_tests`（拆文档组） |
| 编码 / charset 解码 / `<meta>` 处理 | `run_encoding_tests`、`run_site_smoke` |
| 导航 / 加载器 / 重定向 | `run_nav_tests`、`run_site_smoke` |
| 输入（键盘 / IME / 焦点） | `run_typing_tests` |
| 媒体元素 / 解码 | `run_media_tests`，改解码时加 `run_media_real_tests` |
| 卡死 / 中断 / 销毁时序 | `run_hang_recovery_tests` |
| Worker / wasm | `run_worker_tests` |
| 改 ant 内核或桥 | 先 `pwsh -File tools/build_antjsb.ps1`，再上面相关组 + **在 ant 仓库 `H:\ant-v16` 补测试并提交**（铁律 2） |
| 宿主壳 / 启动流程 / 单实例 / 多窗口 / `--userdata` 语义 | `run_singleton_tests`、`run_window_tests`、`run_process_backend_tests`（launcher 用例） |
| shell chrome / 工具栏 / 标签条 / 主题 / DPI / 自绘菜单 / `--ui-shot` | `run_shell_ui_tests`（无头离屏；菜单宿主与真实窗口的 EDIT 输入另见 `check_shell_menu.ps1` / 手工 GUI） |

## 3. 依赖与坑

- **Chrome**：只有 `run_layout_tests.ps1` 需要（`layout_diff.py` 启动 `--headless=new` + 临时 profile
  + 1084×800 视口）。找不到 Chrome 会直接报错；换成别的浏览器对拍要先改 `layout_diff.py`。
- **python**：所有对拍/像素工具都是 python；`layout_diff.py`/`chrome_probe.py` 还依赖 `websockets`。
- **本地服务端口**：worker、nav、tab lifecycle、hang recovery、xhr、encoding 默认都绑**随机空闲端口**
  （脚本自己起停；`-Port` 仍可指定）。批跑时不再需要手工错开。
- **每用例 profile / 日志**：`tools/TestParallel.psm1` 给每个探针一个独立的
  `build-zig-native\test-runs\<suite>-<id>\<case>\profile`（跑完删），stdout/stderr/eval 文件在
  `%TEMP%\agb-test-logs\...`。单独跑套件也一样，所以“依赖上一次 cookie/cache”的检查要自己
  显式用共享 profile，不要赌默认值。
- **剪贴板**：并行套件用 `AGB_CLIPBOARD_FILE` 模拟剪贴板（读写一个 UTF-8 文件），不碰 Win32 剪贴板；
  只有专门验证 OS 剪贴板互操作的用例才允许读系统剪贴板，并在脚本头注释说明。
- **网络**：`run_site_smoke.ps1` 必须要；`run_media_real_tests.ps1` 的远程样片需要（`-SkipRemote` 关掉）。
  站点打不开时 exit 5/6 是网络问题，不是引擎问题（见脚本头注释），重跑再判断。
- **ffmpeg**：`run_media_tests.ps1` 没有也能跑（内部 ticker 模拟时钟）；`run_media_real_tests.ps1`
  缺 ffmpeg 会跳过依赖画面的断言并报 `skipped`。
- **探针自己写的文件必须在 `build-zig-native\` 下**：探针沙箱只保证能写这里，写 `%TEMP%`/`debug\`
  会静默 `shot failed`（exit 4）。截图、profile、`AGB_CLIPBOARD_FILE`、crash 文件都放在
  `test-runs\<case>\`；只有**宿主 pwsh** 写的日志/重定向才放 `%TEMP%`（见上文写盘经验）。
- **跑套件时不要同时重编**：套件会起 `agave_shell.exe` 探针，占着 `libagave.dll` 会让链接失败
  （铁律 3）。
- **临时复现页放 `debug/`，会进套件的放 `tools/pages/`**：`run_layout_tests` 会把
  `tools/pages/layout/*.html` 全部自动收进去，任何不稳定/依赖本机字体的页面都会把主回归弄红。

## 4. 加一个回归用例

**几何（最常见）**：在 `tools/pages/layout/` 新建页面，给要比的元素加
`data-case="名字"`（可选 `data-props="width,margin-left"` 读计算值），不用改脚本：
`run_layout_tests.ps1` 会自动和 Chrome 逐项比（容差 1px）。要点：

- 页面必须**与字体无关**（用固定尺寸色块，不要靠文字宽度）——我们和 Chrome 的字体/回退不同（R123）；
- 需要 JS 建 DOM 的（例如 SVG）在页面里用 `createElementNS`，静态 `<svg>` 标记还没进 SVG 命名空间（R121 之前的 B2 缺口）；
- 只放**当前行为**的断言：修 bug 时先写页面看它红，再改代码看它绿。

**像素**：在 `run_layout_tests.ps1` 里加一段仿照现成 `== paint: …` 的组：
`--probe <页面> --shot build-zig-native\xxx.png`，再用 `python tools/pixel_check.py <png|bmp> x,y,RRGGBB [...]`
或区域计数规格 `x0,y0,x1,y1,RRGGBB,MIN`。截图坐标是左上原点；PNG/BMP 都直接读。
只想看某个元素时加 `--shot-rect x,y,w,h`（CSS 像素，也就是 `getBoundingClientRect()` 的值），
坐标就相对那块矩形。

**单点/规则/字体**：不写新脚本，用
`python tools/chrome_probe.py --url <url> --js-file debug/x.js --agave`（同一段 JS 两边跑）、
`--matched-styles`（哪条规则命中）、`--platform-fonts`（真实字体）、`--screenshot`（参考图），
详见 [dev-debugging.md §4](dev-debugging.md#4-定位手段)。

**套件内加一个探针用例**：不要手写 `Start-Process` 再一个个等。用
`Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking`，然后：

```powershell
$run = New-TestRun -Suite 'mysuite'
$jobs = @(
    Start-TestProbe -Run $run -Name 'case a' -Url $url -Eval 'probeExit(1)' -Wait 300 -Settle 300
    Start-TestProbe -Run $run -Name 'case b' -Url $url2 -EvalFile $checkJs -Env @{ AGB_X = '1' }
)
$results = Complete-TestProbes -Jobs $jobs   # 顺序与 $jobs 一致
Remove-TestRun $run $failures                # 失败时保留目录并打印路径
```

helper 负责每个用例的 profile/cwd/diag/模拟剪贴板/日志隔离和全局 slot 门禁；对时间敏感的
用例（点击后开菜单、双击、nav 后 eval）失败就重跑一次——用例之间已经互不影响。给 `Start-TestProbe`
传 `-ClipboardValue` 可预置该用例的模拟剪贴板内容，传 `-RenderTag` 可逐用例检查 renderer 残留。

## 5. 页面 fixture 在哪

| 目录 | 用途 |
|---|---|
| `tools/pages/layout/*.html` | 几何/像素回归页（自动进 `run_layout_tests`），配套 `lib_measure.js` |
| `tools/pages/encoding/*.html` | 编码优先级页（UTF-8 源文件；GBK/BOM 由 `encoding_server.py` 在发送时变换），配套 `check_encoding.js` |
| `tools/pages/*.html` + `check_*.js` | 各专项套件的页面与断言脚本（typing/xhr/worker/nav/realm/媒体…） |
| `tools/pages/**`（served） | nav/xhr/worker 等需要 http 语义的页面，由套件自己起服务 |
| `tools/pages/fuzz_layer.html` | 确定性 DOM/样式变异 fuzz（position/overflow/opacity/float/iframe/table/select，每步强制 layout），专打层树拆除；探针跑 10 轮应 0 崩溃 0 `[lay] DESTROY-WHILE-LINKED`（R134） |
| `debug/**` | 临时复现页与诊断页（不进套件；`debug/` 在 `.gitignore` 里） |
| `tools/ant_*.c` | 直接打桥的 C 探针（realm/pin/loop 复现） |

