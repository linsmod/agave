# 目标

根据用户提供的测试场景完善浏览器引擎对齐现代浏览器特性和表现

[script engine](tools/build_antbridge.ps1)

[build proj](tools/build_project.ps1)

[cdb](tools/cdb.exe)


# rules

DON'T CLEAN THE BUILD UNLESS YOU MUST.

DON'T SAY THE CONTEXT WILL GET FULL; IT IS AUTO-COMPACTED AND WILL NEVER BE FULL.

WHEN A BUG IS DEEP, DIG DEEP.

FOR SOME BUGS OR REQUIREMENTS THAT REQUIRE INFRASTRUCTURE ADJUSTMENTS, MAKE A DESIGN, RESPOND WITH A PLAN, AND IMPLEMENT IT.

MAKE HELPER SCRIPTS REUSABLE; WHEN A SCRIPT IS HIGH-FREQUENCY USED, PLACE IT IN THE PROJECT.

任务中经历的本该优化的阻塞点，可以临时用脚本方式绕过，但需要记录并提示延后处置。

WHEN USER REPLY A DOT[.] ONLY, MEANS OK/DONE/DO_AS_YOU_SAY/YES/GO_ON/ACCEPT/...

## 页面调试（当前可用途径）

**无头探针：`agave_shell.exe --probe <url> [--eval <js>] [--wait ms] [--settle ms] [--timeout ms]`**
- eval 时机 = 页面 window load 事件 + `--wait`；progress finished 只是长延迟兜底（失败加载也报 finished）
- 页面 JS 调 `probeExit(result)` 带结果退出；页面 console/warn 进 stderr（console guard 自动节流重复警告）
- 退出码：0=OK / 1=usage / 2=timeout未加载 / 3=timeout无exit / 4=无exit（重页面把 --settle 调大）/ 5=加载错误 / 6=空文档（body 无子节点=从未真正解析）。先看退出码再翻日志
- `--probe` 是**真无头**：命令行在创建任何窗口之前解析，命中 probe 就直接进 `probeRun()`——不建浏览器主窗口、不建工具栏、不读/写 session/bookmark 文件。
  唯一的窗口是 `WS_POPUP|WS_EX_TOOLWINDOW` 的**隐藏**父窗口（控件需要 parent HWND），不占任务栏、不抢前台，stderr 会打
  `[probe] headless window hidden, AGB_PROBE_SHOW=1 to show (pid <进程号>)`。
  `AGB_PROBE_SHOW=1` 才 ShowWindow（这时才有 WM_PAINT，像素/截图类检查要用它）；要看真实 GUI 用 `agave_shell.exe <url>`（不带 --probe）。
  验证某进程确实没有可见窗口：`powershell -File tools/check_window_visible.ps1 -ProcessId <pid>`（exit 0 = 无可见窗口，`-Json` 出明细）
- PowerShell 传 `--eval` 时 `&&` 会被搅成 `;`，脚本里用嵌套三元/逗号表达式替代
- 宿主窗口消息循环处理 WM_TIMER 必须按 `msg.hwnd` 过滤——webview 控件内部有自己的 id=1 帧泵定时器，wParam 相同会串；引擎线程回调里禁止 SetTimer（窗口属主线程，跨线程未定义行为），走 PostMessage 到主线程

**定位手段**
- **未捕获/被页面吞掉的错误定位**：`AGB_ERR_SNIPPET=1`（ant `src/errors.c`）。引擎创建内部错误（`js_mkerr_typed`）时，若消息含 `length` 或 `not a function`，直接往 stderr 打一行
  `[snippet] line=<行> col=<列> msg=<消息> | <出错点前 60 字符起的 130 字符源码>`
  —— 不用栈就能把错误对回 bundle 的某个偏移（React/lottie 这类把异常吞进 console.error 的代码里特别有用）。注意：
  ①行号恒为 2（页面脚本被 `try{\n…\n}catch` 包过），列号才是真偏移，可直接在对应 `.js` 里切字符串；
  ②两半标记之间是**出错前的上下文**，真正的 callee 在片段末尾附近；
  ③core-js/modernizr 的特性探测（`Object.defineProperty([],"length",{writable:!1}).length=1`、`openDatabase`、`fillText` 等）会故意抛错并被自己 catch，这些 `[snippet]` 是**正常噪音**，不代表缺陷
- DOM 节点属性读出 undefined：查 AntBridge 桥接链路（wrapNode / 类属性表 / catch-all），先于怀疑页面
- Vue "missing template or render function" ≈ `container.innerHTML` 拿到 undefined
- `<template>` 内容惰性（HTML5）：Node::createRendererIfNeeded 对 localName=="template" 返回，子树无 renderer 不渲染；children 留在 DOM（innerHTML/script 访问不变）。别删
- `on_progress finished=1` 不代表加载成功；host 侧用 `agave_webview_url` 比对请求 host 判定是否真提交（加载卡死时 FrameLoader 会以 about:blank 提交）
- 抓挂起线程栈：`tools/cdb.exe -p <pid> -c '~*k 30; qd'`（探针有超时会秒退，attach 要趁早）
  - 要断在**首次异常**上抓栈：写命令文件 `sxe av` / `g` / `~*kb 20` / `q`，再
    `tools\cdb.exe -y "<build-zig-native>;H:\ant-v16\bridge" -cf cmds.txt build-zig-native\agave_shell.exe --probe ...`
    —— **不要用 `--` 分隔**（这版 cdb 会报 0x80070057），也别用 Start-Process 传 `-c "长命令"`（引号被吞）。
    当前 `antbridge.dll` 旁边就有匹配的 `antbridge.pdb`（带源码行号），`-y` 指过去即可解出 `process.c @ 2355` 这种帧
- 地址栏输入 → URL：`examples/browser_shell/main.c` 的 `normalizeUrl()` 是**唯一**入口（地址栏/Go/收藏夹/`--probe` 都走它）。
  本地路径（`X:\`、`X:/`、UNC、磁盘上存在的相对路径）→ `fileUrlFromLocalPath()`（`GetFullPathNameW` + `\`→`/` + 逐字节百分号转义）；
  `file:` URL 在引擎侧只能**单次转义**（`WebView::loadUrl` 会对整串调 `KURL::encode_string`，所以它必须幂等：`%XX` 原样保留）。
  调试入口：`agave_shell.exe --probe "<输入>" ...` 的 `[probe] main url=` 就是引擎最终 URL
- worker 相关：worker 是**独立线程 + 独立 isolate**，崩溃栈会出现在非主线程上（`~*kb` 里找 `worker_thread_main`）；
  `new Worker` 的入口在 ant 侧走 ESM/CJS 模块加载，栈帧名对 data: URL 会折成 `data:script/<hash>`
  （`esm_frame_filename`），所以看到 `data:script/xxxxxxxx` 就是 worker 里的代码。回归：`pwsh -File tools\run_worker_tests.ps1`
- 网络卡点：`AGB_NET_LOG=1` 看 `net.stage` 打点（crack/connected/openrequest/sending/sent）；WinHTTP SendRequest 偶发长滞是 WPAD/PAC 环境抖动，同刻 GUI 正常≠代码回归，先重试
- 字体（@font-face）：`AGB_FONT_LOG=1` 看 `[fontsel] src[i] ... supported=0/1` 与 `[font] beginLoad/data`；
  回归页 `tools/pages/iconfont_render.html`（真实多格式 src），`agave_view_host.exe <file-url> out.bmp` 出图核对字形。链路与坑见 `docs/web-font.md`

**构建规则（不守则静默无效）**
- 改 `AntBridge.c`：**改本仓库 `src/javascript/ant_backend/` 那份**，再 `tools/build_antbridge.ps1 -Force` 重编 dll、最后 relink libagave（antbridge.dll 是预编译产物，CMake 只拷贝）。
  不要改 `H:\ant-v16\bridge\AntBridge.c`——脚本每次都会用本仓库的版本覆盖它，改了白改（2026-11-04 踩过）
- 宿主窗口消息循环要自退：WM_DESTROY 里 PostQuitMessage（GetMessage 只认 WM_QUIT）；调 `agave_webview_eval` 的分支要防重入
- 代理会话沙箱拒绝执行 winget symlink 的 zig.exe；包装器（.zig-toolchain、zigspike）已直指 WinGet\Packages 真实 exe，zig 升级需同步路径

**ant 引擎 / 桥接：哪份是真的**（2026-11-04 核实）

两套 git 仓库互不相干，同名文件各有拷贝，改错地方会被静默覆盖：

| 对象 | 真源（改这里） | 其它拷贝 |
|---|---|---|
| ant 内核 `libant.a` / `src/silver/*` / `include/*` | `H:\ant-v16`（分支 `agave-bridge`） | 本仓库没有引擎源码 |
| `AntBridge.c` / `AntBridge.h` | **本仓库** `src/javascript/ant_backend/` | `H:\ant-v16\bridge\`（构建时被覆盖） |
| `antbridge.dll` | `H:\ant-v16\bridge\`（链接产物） | `3rdparty/ant/bin`、`3rdparty/ant/lib`、`build-zig-native`、ws host；**运行时只加载 exe 同目录 `build-zig-native\antbridge.dll`** |
| git 远端 | agave → GitHub `linsmod/agave`；ant 上游 → `theMackabu/ant`，本地镜像 `H:\ant-upstream`，推送 fork `fork` → `linsmod/ant` | 两套历史无关 |

- ant 的工作分支 `agave-bridge` 已推到 fork：`https://github.com/linsmod/ant/tree/agave-bridge`（`git push fork agave-bridge` 即可更新）。向上游 `theMackabu/ant` 提 PR 之前必须先 rebase 到它的 master（当前落后 54 个 commit）。

- ant 的 `agave-bridge` 基线 = tag `v16.5.c41757b4.0`（**冻结的基线**），落后 `origin/master` 54 个 commit；本地补丁叠在上面（`c1e00842` + 桥接/性能 2 个）。要跟上游同步只能 rebase，别直接 merge。
- `H:\ant-v16` 历史上是 detached HEAD 检出 tag 的用法：**提交后立刻 `git switch -c <name>` 挂分支**，否则切走就丢。
- 一致性自检（两边应完全相同）：
  ```powershell
  (Get-FileHash e:\mybrowsers\agave\src\javascript\ant_backend\AntBridge.c -Algorithm MD5).Hash
  (Get-FileHash H:\ant-v16\bridge\AntBridge.c -Algorithm MD5).Hash
  # dll 四处应同 hash：3rdparty\ant\bin = 3rdparty\ant\lib = build-zig-native = H:\ant-v16\bridge
  ```
- 改引擎内核（ant 侧）的完整链路：`ninja -C H:\ant-v16\build-zig libant.a` → `pwsh -File tools\build_antbridge.ps1 -Force`（重链并同步四处 dll）→ 只有改了本仓库 C++ 才需要再 `cmake --build build-zig-native`。

**常用命令**

```powershell
# 标准增量构建（不要 clean）。输出几千行，落盘再 grep，别直接进上下文
pwsh -File tools\build_project.ps1 *> build-native.log; $LASTEXITCODE

# 快速重链（改一两个文件时的最小路径，跳过脚本的 toolchain/configure 检查）
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
cmake --build e:\mybrowsers\agave\build-zig-native --parallel 16
# 注：脚本带 -NoTouchWeb 等参数会改目标集触发重新 configure，不是全量重编

# 改了 AntBridge.c 之后的完整链路（两步缺一不可）
pwsh -File tools\build_antbridge.ps1 -Force
cmake --build build-zig-native --parallel 16

# 无头探针：取页面求值结果 + 页面 console（stderr）
Start-Process -FilePath build-zig-native\agave_shell.exe `
  -ArgumentList '--probe','https://example.com/', `
                '--eval',"`"probeExit('title=' + document.title)`"", `
                '--wait','8000','--timeout','60000' `
  -RedirectStandardOutput out.txt -RedirectStandardError err.txt -PassThru -Wait
$LASTEXITCODE   # 退出码即失败类别

# 网络层时间线（卡在 crack/connected/sending 哪一步一目了然）
$env:AGB_NET_LOG = '1'      # 关闭置 $null；偶发挂起先重试
# JS 侧详细日志：$env:AGB_JS_DEBUG='1'

# 挂起进程抓全线程栈
tools\cdb.exe -p <pid> -c '~*k 30; qd'

# CPU 热点（连 ant 内部符号一起解）：antbridge 默认链接没有调试信息，cdb 只能显示
# antb_*+偏移。用 -g 重链出带 PDB 的调试版，再按 RVA 符号化：
#   zig-cxx.cmd 参数同 tools\build_antbridge.ps1，额外加 -g，vendor 用
#   build-zig\vendor 下递归 *.a（脚本里的列表是嵌套子目录，glob 到顶层会缺utf8proc）
#   -> 产物 ab_dbg.dll + ab_dbg.pdb(35MB)；PDB 名必须与 DLL 内嵌记录一致，别改名
#   替换 build-zig-native\antbridge.dll 后：AGB_PROFILE=1 跑探针拿 [profjs] 直方图，
#   再 `cdb -y <dll目录> -z antbridge.dll -c "ln antbridge+0x<rva>; qd"` 出函数/源码行；
#   在线抓栈用 `cdb -y <dll目录> -p <pid> -c "~*k 30; qd"`（帧上带 H:\ant-v16\src\... @ 行号）

# 本地页面测试：直接 file://（模块管线对 file: URL 本地直读，无需起服务）
# 如需 http 语义：Start-Process -WindowStyle Hidden python -ArgumentList '-m','http.server','8971','--directory','e:\mybrowsers\agave\debug'

# prelude.js / polyfills.js 直接改即可生效（引擎每次文档绑定时从 exe 旁加载，无需重链/重编）
#   prelude.js  = window 引导 shim（window/document 辅助、XHR/fetch/storage/matchMedia、事件派发）
#   polyfills.js= DOM/构造函数补丁（append/prepend、classList、构造器 stub、console guard）
#   Web Worker 的真实现在 prelude.js 末尾（原生线程 + http 源码预取 + preamble），不在 polyfills
#   改源文件后记得让构建把它拷到输出目录（add_custom_target prelude/polyfills），或直接改
#   build-zig-native\prelude.js 临时验证；$AGB_PRELUDE / $AGB_POLYFILLS 可指向别处的副本
Copy-Item src\javascript\ant_backend\polyfills.js build-zig-native\polyfills.js -Force
```
