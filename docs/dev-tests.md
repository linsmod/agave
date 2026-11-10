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
pwsh -File tools/run_all_tests.ps1                          # 19 个离线套件，默认并发 4，退出码 0 = 全绿
pwsh -File tools/run_all_tests.ps1 -Sequential              # 旧的一件一件跑（≈390 s）
pwsh -File tools/run_all_tests.ps1 -Parallel 8              # 并发度（1..16）
pwsh -File tools/run_all_tests.ps1 -Only process            # 名字含 process 的
pwsh -File tools/run_all_tests.ps1 -Skip run_layout_tests,run_media_tests
pwsh -File tools/run_all_tests.ps1 -IncludeNetwork -IncludeReal   # 加上真站点 / ffmpeg 套件
pwsh -File tools/run_all_tests.ps1 -KeepLogs                # 保留每个套件的 stdout/stderr
```

默认**并发执行**（`-Parallel 4`）：每个套件是独立的 `pwsh` 进程，`AGB_USERDATA` 指向
`%TEMP%\agb-suites-<id>\profile-<suite>`，互不共享 cookie/cache；结果逐套件打印 `PASS/FAIL + 墙钟`。
四个套件有硬冲突或怕负载，放在并发阶段之后**逐个串行**：`run_hang_recovery_tests`（断言没有
`agb_crash.txt`/`agb_watchdog.txt` 出现）、`run_process_backend_tests`（故意崩 renderer 做崩溃报告、
还量命令延迟）、`run_image_tests`（paint/rAF 时间上限）、`run_selection_tests`（**系统剪贴板**：
和 process 套件的复制用例不能同时跑）；`-IncludeReal` 时 `run_media_real_tests`
（ffmpeg/音频设备）也进这一队。失败时默认保留该套件的日志（路径会打印），`-KeepLogs` 连成功的也留。

默认排除"要外部条件"的三个：`run_site_smoke`（真站点，网络）、`run_media_real_tests`（ffmpeg +
远程样片）、`run_c3`（外部工作区 + 8899 端口，本来就不是标准回归）。每个套件打印
`PASS/FAIL + 秒数`，最后给最慢的 5 个和总数；单个套件超过 180 s 会打 `WARN`（AGENTS rule 9）。

**软基线**（2026-11-09 实测，16 核本机；**不是断言**；下表是**串行**墙钟，默认并发整批 ≈ 245 s）：

| 套件 | 基线 | 套件 | 基线 |
|---|---:|---|---:|
| `run_process_backend_tests` | 37 s | `run_xhr_tests` | 20 s |
| `run_image_tests` | 53 s | `run_window_tests` | 17 s |
| `run_layout_tests` | 50 s | `run_nav_tests` | 13 s |
| `run_hang_recovery_tests` | 38 s | `run_typing_tests` | 17 s |
| `run_jit_loop_tests` | 22 s | `run_script_mode_tests` | 11 s |
| `run_hidden_view_tests` | 9 s | `run_realm_tests` | 9 s |
| `run_inspector_tests` | 9 s | `run_tab_lifecycle_tests` | 7 s |
| `run_worker_tests` | 6 s | `run_media_tests` | 6 s |
| `run_sticky_tests` | 4 s | `run_encoding_tests` | 9 s |
| `run_selection_tests` | 28 s | | |
| **合计 19 套件（串行）** | **~390 s** | **默认并发整批** | **~250 s** |

规则（软基线，靠人/Agent 判断，不做程序化对比）：上表是**串行**墙钟；并发跑时每个套件会被别的
套件拖慢（`run_layout_tests` 55 s、`run_typing_tests` 17 s 是并行下的常见值），要拿它和基线比就加
`-Sequential`。集中跑的时候除了看 PASS，还看每套件秒数——**比基线慢 30 s 以上就在对话里提示**
（AGENTS rule 9），顺带查一下是机器忙、测试真的退化还是新用例加多了。新增套件请控制在几十秒内
并在这里补一行；会和别的套件抢固定文件/端口的，加进 `run_all_tests.ps1` 的 `$serialOrder`。

`run_site_smoke`（真站点，网络）与 `run_media_real_tests`（ffmpeg + 远程样片）本机没测出稳定值，
需要时用 `-IncludeNetwork` / `-IncludeReal` 单独跑。

### 单个套件

全部用 `pwsh -File tools/<脚本>` 跑，**退出码 0 = 通过**（`run_c3.ps1` 例外，见下）。
`run_media_*` / `run_sticky_tests` 等脚本里的 Chrome 数值是写死的基准，不需要本机 Chrome；
只有 `run_layout_tests.ps1` 会真的启动 Chrome。

| 脚本 | 覆盖 | 依赖 | 判据 / 输出 |
|---|---|---|---|
| `run_layout_tests.ps1` | **主回归**：`tools/pages/layout/*.html` 全部与 headless Chrome 比几何（`data-case`）、几组像素断言（变换重绘、变换文字、背景、圆角、滚动条隐藏、vw/vh/resize、CSSOM、computed 简写序列化、flex computed、DOM 生命周期）、**文字灰度抗锯齿**（`tools/font_aa_check.py` + `tools/pages/font_antialias.html`，R167） | Chrome、python `websockets` | 每组打印 `N/N checks ok`，最后 `LAYOUT=PASS`；失败打印 `LAYOUT=FAIL (n)` |
| `run_inspector_tests.ps1` | 注入 `inspector.js`：菜单项、行命中、元素信息 JSON、剪贴板；真实 `WM_RBUTTONDOWN/UP`（页面 contextmenu 必须被取消、壳的原生菜单不弹）与真实左键点击 | 无 | `inspector: 26/26 checks passed` |
| `run_nav_tests.ps1` | `[nav]` 通道八项：链接点击 / 脚本跳转 / 历史后退 / meta refresh / `window.open` / 表单提交 / 302 / `AGB_NAV_LOG=0` 静音 | 自起 python http.server（8973） | `ALL NAV CHECKS PASSED (n cases)` |
| `run_xhr_tests.ps1` | XHR 投递重入（R94）、`responseText` UTF-8 解码（R96）、响应头/`statusText`/`readyState`/`overrideMimeType` 与 Chrome 对拍（R97） | 自起静态服务（8991） | `ALL XHR CHECKS PASSED` |
| `run_encoding_tests.ps1` | 文档编码优先级（R164）：BOM > HTTP `Content-Type` charset > `<meta charset>`/`@charset` > 默认；`document.characterSet` 报真实规范名 | 自起 `tools/encoding_server.py`（8993，`-Port` 可换） | `ALL ENCODING CHECKS PASSED (5 cases)`，约 9 s |
| `run_worker_tests.ps1` | Worker 九项：http worker、`importScripts`、blob、terminate、subclass、error 事件、structured clone | 自起服务（8971，可 `-Port`/`-KeepServer`） | `ALL WORKER CHECKS PASSED`；setup 失败 exit 2 |
| `run_typing_tests.ps1` | 键盘/IME：走真实 `WM_KEYDOWN/WM_CHAR/WM_KEYUP` 与 `WM_IME_CHAR`（`--keys`/`--ime`）；末尾两条量**光标几何**（`AGB_CARET_DEBUG=1` 红色光标 + `tools/caret_check.py`，输入框光标=文字高度而不是盒子高度） | 无 | `ALL TYPING CHECKS PASSED (n cases)` |
| `run_selection_tests.ps1` | 文本选区与快捷键（R175）：真实拖选/反向拖选/跨段落、双击选词 + `dblclick`、三击选段、`--shift-click`、`user-select:none`/嵌套 `text`、Ctrl+A/C/V/X、Shift+方向键、Ctrl+Shift+方向键、`--editor` Copy/SelectAll、`window.getSelection`/`Range`/`execCommand` 与 Chrome 对拍字段、选区高亮像素；`-Process` 再加 6 个 process 后端用例 | 无（**会覆盖系统剪贴板**） | `ALL SELECTION CHECKS PASSED (n cases)`；local 约 28 s，`-Process` 再加约 15 s |
| `run_jit_loop_tests.ps1` | 长 JIT 循环 + "引擎线程仍在跑 EVAL 时销毁视图"（R61 的 0xC0000005） | 无 | `ALL JIT LOOP CHECKS PASSED` |
| `run_hang_recovery_tests.ps1` | 卡死页恢复：`--interrupt-after` 中断、排队导航中断卡死脚本、空闲页不误中断、慢页连续 reload 不被杀 | 自起服务（8982/8984） | `ALL HANG RECOVERY CHECKS PASSED` |
| `run_script_mode_tests.ps1` | classic script 语义（顶层 `var`/`function` 进 `window`，R66）+ 热点循环性能 + `document.readyState`/`currentScript` | 无（Chrome 数值为注释里的基准） | `ALL SCRIPT MODE CHECKS PASSED` |
| `run_realm_tests.ps1` | 每 frame 一个 realm（`design-per-frame-realm.md`）：先编译 `tools/ant_realm_probe.c` 直接测桥（一 isolate 三 realm），再走 iframe 页面 | 本机编译器（脚本自己编 `ant_realm_probe.exe`） | `ALL REALM CHECKS PASSED` |
| `run_tab_lifecycle_tests.ps1` | 关 view/tab 的生命周期（R141，`design-tab-teardown.md`）：teardown 顺序 `pagehide → visibilitychange → unload → realm stop`、每个 `realm start` 都有 `realm stop`、**JS 创建的节点不 pin 住 Document**（无 `LEAK:` 行）、`window.open` popup 是独立 view context 且同样拆除、字体下载在途时销毁 view 不崩且 `documentDetached` 命中 | 自起 `tools/tab_lifecycle_server.py`（8979，`/slow.ttf` 延迟 1.5s） | `ALL TAB LIFECYCLE CHECKS PASSED (n cases)` |
| `run_process_backend_tests.ps1` | `process` 渲染后端（S3 B2a/B2b）：一个 view 一个 `agave_render_host.exe`，协议走命名共享段（canvas + 命令环 + 事件环 + 文本 arena + 大载荷共享段）。**用例默认并发 4**（`-Parallel N` / `-Sequential`；每个用例独立 `AGB_USERDATA` + `AGB_RENDER_TAG`，产物落在构建目录 `process-tests-*`，成功后删除）；冲突用例显式串行：crash/respawn/crash-report（写 `agb_crash.txt`、设 `AGB_PROBE_CRASH_PAGE`）、protocol stats / multiview / hung-tab（量延迟）、launcher。断言页面 JS/title/定时器、`--shot` 像素（共享 canvas → mirror）、`--keys` 输入跨边界、**32 KB 注入资产整段到达**（`--inject inspector`，`window.__agaveInspector` 存在且 `inspectAt` 命中：R169 的大载荷通道）、**300 KiB `--eval-file` 首尾标记都在**（不再有 4 KB/8 KB/32 KB 的静默截断）、**286 KB vConsole 注入、挂载并点开面板**（同时覆盖 R170 的 focus-after-DOM-mutation 和 R172 的 `document.createEvent`）、**页面复制到 OS 剪贴板**（R171）、**back/forward 计数跨进程**、渲染进程崩溃报 load error（probe 立即退出 5，不等到 timeout）、**双 view：A 卡死时 B 的命令 31 ms 回答、`agave_webview_interrupt(A)` 31 ms 生效**、local 对照 `blocked=1`、崩溃后视图可 respawn、**关掉卡死 tab 立即生效（destroy_ms=0）**、`--backend process` 参数与 launcher（`tools/agave.ps1`，默认 process）走同一条路、未知后端被拒绝、默认 local 后端不退化、**命令不会静默消失**（无页面时的 eval 报 `dropped`；navigate 刚返回就 eval 要么执行要么报错；`AGB_IPC_STALE_EPOCH=1` 下渲染进程拒收过期代数命令，宿主收到 `on_command_dropped`）；**崩溃页报告**（`AGB_RENDER_CRASH_AV` 制造真 AV：环境/异常码/符号栈进共享段，`on_crashed` 把它交给宿主，sad tab 的 HTML 落盘再加载读回）、**右键菜单跨进程**（`--right-click`：渲染进程 `cb_context_menu` → `CONTEXT_MENU` 事件 → 宿主 `on_context_menu`，断言 `[ctx] menu … link=…`）、**内部页跨进程**（`agave://downloads` 导航 + `--reload 2` 仍是 `title=下载`：scheme provider 让 reload 走 loader 而不是网络）、**P1-ac**（`AGB_PROBE_CRASH_EVAL=1`：`load_html` 后立刻 eval 只报 `dropped`，不得 ASSERT/崩溃）、每例后不得残留 renderer 进程（25 例，并发 4 约 37 s） | 无（`tools/pages/cap_hidden_view.html` / `cap_typing.html` / `inspector_demo.html` / `window_links.html`） | `ALL PROCESS BACKEND CHECKS PASSED` |
| `run_window_tests.ps1` | 窗口生命周期（R158），**local 与 process 两个后端各跑一遍**：普通链接同 tab 跳转；`target="_blank"` 开新 tab 并加载出标题、主 tab 不变；`window.open(url)` 同样开 tab、页面拿到窗口对象；具名 target 点两次只开**一个**窗口，第二次导航它并回调 `on_activate_view`（宿主切 tab）；popup 里 `window.close()` 到达宿主。每例后不得残留**自己 tag 的** renderer（`AGB_RENDER_TAG`，别人的壳不受影响，R159） | 无（`tools/pages/window_*.html`） | `ALL WINDOW CHECKS PASSED` |
| `run_hidden_view_tests.ps1` | 隐藏 view（后台 tab / 最小化窗口）跳过布局与绘制（R150）：`--hide-after` 后定时器继续跑、`document.hidden`/`visibilitychange` 正确、`AGB_LOOP_DEBUG` 的 `[loop] paints=/skips=` 证明 paint 冻结而 skip 增长；无隐藏的对照跑 paint 持续增长；`--hide-after`+`--show-after`+`--shot` 证明回到可见后首帧整幅重绘（隐藏期间改色的方块在截图里必须是新色） | 无（`tools/pages/cap_hidden_view.html`） | `ALL HIDDEN VIEW CHECKS PASSED` |
| `run_sticky_tests.ps1` | `position:sticky`：视口/内层滚动容器/containing block 边界、命中跟随（R82），几何 + 像素 | 无（期望值来自 Chrome） | `STICKY=PASS` |
| `run_image_tests.ps1` | 图片解码（R136–R140）：切片解码与一次性解码**逐字节同像素**（JPEG + PNG）、**工作线程解出的帧与内联帧逐字节一致**且 paint 里不再解码、大图不再单帧停摆（paint 上限 / rAF gap）、L3 缩放解码的加速比、**拆文档时进行中的解码不崩且队列排空**、**零字节图片资源（204 / `Content-Length: 0` / 截断头）只算坏图不崩、且不会每帧重解**、`AGB_IMG_DECODE_MS=0`/`AGB_IMG_NO_SCALE`/`AGB_IMG_SAMPLE_MAX` 开关有效 | 无（脚本自己用 System.Drawing 生成 4000×3000 JPEG / 3000×2000 PNG 到 `build-zig-native/img_test`；零字节那组自起 `tools/empty_image_server.py`，随机空闲端口） | `IMAGE=PASS` |
| `run_media_tests.ps1` | 媒体元素 API/几何/传输（play → timeupdate → ended）；有 ffmpeg 走真解码，没有则内部 ticker | 无（ffmpeg 可选） | `MEDIA=PASS` |
| `run_media_real_tests.ps1` | 真解码回放：`currentTime` 走墙钟、暂停/续播、seek、`ended`、画面与 ffmpeg 同位置解码一致 | ffmpeg（缺失则跳过像素项）、网络（远程样片；`-SkipRemote` 可关） | `MEDIA-REAL=PASS (n skipped)` |
| `run_site_smoke.ps1` | 真实站点冒烟：deepseek / baidu / vqq 的加载、JS 执行、DOM 规模（vqq 带 `AGB_GC_OFF=1`，见 R68） | **网络** | `ALL SITE SMOKE PASSED`；每站打印 `all=元素数` |
| `run_c3.ps1` | css3test 本地镜像的**临时**跑法（硬编码了 harness 工作区与 8899 服务，不属于标准回归） | 外部工作区 + 本地服务 | 打印探针输出，无 PASS/FAIL |

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

## 3. 依赖与坑

- **Chrome**：只有 `run_layout_tests.ps1` 需要（`layout_diff.py` 启动 `--headless=new` + 临时 profile
  + 1084×800 视口）。找不到 Chrome 会直接报错；换成别的浏览器对拍要先改 `layout_diff.py`。
- **python**：所有对拍/像素工具都是 python；`layout_diff.py`/`chrome_probe.py` 还依赖 `websockets`。
- **本地服务端口**：worker 8971、nav 8973、tab lifecycle 8979、hang recovery 8982/8984、xhr 8991、encoding 8993。脚本自己起停；
  并行跑多个套件时用 `-Port` 错开（worker 支持）。
- **并行批跑的 profile**：`run_all_tests.ps1` 给每个套件设一个独立的
  `AGB_USERDATA`（`%TEMP%\agb-suites-<id>\profile-<suite>`），跑完删掉；单独跑套件时仍用
  `%LOCALAPPDATA%\Agave\probe`，测试若依赖“上一次的 cookie/cache”，用单跑而不是批跑。
- **网络**：`run_site_smoke.ps1` 必须要；`run_media_real_tests.ps1` 的远程样片需要（`-SkipRemote` 关掉）。
  站点打不开时 exit 5/6 是网络问题，不是引擎问题（见脚本头注释），重跑再判断。
- **ffmpeg**：`run_media_tests.ps1` 没有也能跑（内部 ticker 模拟时钟）；`run_media_real_tests.ps1`
  缺 ffmpeg 会跳过依赖画面的断言并报 `skipped`。
- **BMP 输出必须落在 `build-zig-native\`**：探针沙箱只写这里，写 `debug\` 会静默 `shot failed`（exit 4）。
  截图类套件（`run_sticky_tests` 等）用 `[IO.Path]::GetTempPath()` 建临时目录，在受限沙箱下要把
  `TEMP`/`TMP` 指到工作区内再跑（`run_image_tests.ps1` 自己就这么做），否则报
  `pixel_check.py ... FileNotFoundError: ...\Temp\agb-sticky-...\sticky.bmp`，看起来像像素回归其实是写盘被拒。
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
