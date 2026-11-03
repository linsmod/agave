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
- 探针窗口默认**隐藏**（引擎照常渲染进缓冲区，但没人看得见）；`AGB_PROBE_SHOW=1` 才 ShowWindow，能肉眼看绘制。要看真实 GUI 用 `agave_shell.exe <url>`（不带 --probe）
- PowerShell 传 `--eval` 时 `&&` 会被搅成 `;`，脚本里用嵌套三元/逗号表达式替代
- 宿主窗口消息循环处理 WM_TIMER 必须按 `msg.hwnd` 过滤——webview 控件内部有自己的 id=1 帧泵定时器，wParam 相同会串；引擎线程回调里禁止 SetTimer（窗口属主线程，跨线程未定义行为），走 PostMessage 到主线程

**定位手段**
- DOM 节点属性读出 undefined：查 AntBridge 桥接链路（wrapNode / 类属性表 / catch-all），先于怀疑页面
- Vue "missing template or render function" ≈ `container.innerHTML` 拿到 undefined
- `<template>` 内容惰性（HTML5）：Node::createRendererIfNeeded 对 localName=="template" 返回，子树无 renderer 不渲染；children 留在 DOM（innerHTML/script 访问不变）。别删
- `on_progress finished=1` 不代表加载成功；host 侧用 `agave_webview_url` 比对请求 host 判定是否真提交（加载卡死时 FrameLoader 会以 about:blank 提交）
- 抓挂起线程栈：`tools/cdb.exe -p <pid> -c '~*k 30; qd'`（探针有超时会秒退，attach 要趁早）
- 网络卡点：`AGB_NET_LOG=1` 看 `net.stage` 打点（crack/connected/openrequest/sending/sent）；WinHTTP SendRequest 偶发长滞是 WPAD/PAC 环境抖动，同刻 GUI 正常≠代码回归，先重试
- 字体（@font-face）：`AGB_FONT_LOG=1` 看 `[fontsel] src[i] ... supported=0/1` 与 `[font] beginLoad/data`；
  回归页 `tools/pages/iconfont_render.html`（真实多格式 src），`agave_view_host.exe <file-url> out.bmp` 出图核对字形。链路与坑见 `docs/web-font.md`

**构建规则（不守则静默无效）**
- 改 `AntBridge.c`：必须先 `tools/build_antbridge.ps1 -Force` 再 relink libagave（antbridge.dll 是预编译产物，CMake 只拷贝）
- 宿主窗口消息循环要自退：WM_DESTROY 里 PostQuitMessage（GetMessage 只认 WM_QUIT）；调 `agave_webview_eval` 的分支要防重入
- 代理会话沙箱拒绝执行 winget symlink 的 zig.exe；包装器（.zig-toolchain、zigspike）已直指 WinGet\Packages 真实 exe，zig 升级需同步路径

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

# 本地页面测试：直接 file://（模块管线对 file: URL 本地直读，无需起服务）
# 如需 http 语义：Start-Process -WindowStyle Hidden python -ArgumentList '-m','http.server','8971','--directory','e:\mybrowsers\agave\debug'

# polyfills.js 直接改即可生效（引擎每次文档绑定时从 exe 旁加载，无需重链）
Copy-Item src\javascript\ant_backend\polyfills.js build-zig-native\polyfills.js -Force
```
