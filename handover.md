# handover：agave 浏览器引擎（现状 / 待办 / 代码索引）

> 这一篇只写「现在是什么样、还差什么」。
> 历史经验 —— 每个缺陷的 现象 → 根因 → 修法 → 验证（R1…R24，含设计约束）——在
> [`docs/lessons.md`](docs/lessons.md)；构建、探针、cdb、环境变量的**权威**手册是
> [`AGENTS.md`](AGENTS.md)，这里不重复。
>
> 最近更新：2026-11-04（R24 探针真无头、R23 地址栏走 file:、R22 Web Worker、R21 `font` 简写、
> R20 deepseek 登录页、R19 wasm 二进制通道）。

## 1. 现状

**能跑通**：页面加载/解析（含 `defer`/`async`/ESM 模块）、DOM 桥接、computed style、布局与绘制、
滚动、鼠标/键盘事件、定时器、XHR/fetch（二进制 + POST）、localStorage、Web Worker、`file:` 本地页面、
标签页/地址栏/查找/书签、`beforeunload`。

**已经被检查过的"看起来不能用"**（别再当缺陷重开）：ant 的 Worker 是真的（R22）、ant 的 wasm 引擎是真的
（R19）、渐变/`var()`/flex 大多可用（R11）、滚动与 hover 重绘正确（R13/R14）、刷新后 realm/监听器会清理（R9/R10）。

**还没做**（§4 有验收标准）：模块图 flush 阻塞 engine 线程（P0-b）、DOMContentLoaded 双发（P0-c）、
模块调度状态不是 per-realm（P1-a）、`g_documentParsed` 死标志（P1-c）、worker 里没有网络（P1-d）、
prefetch 仍是字符串扫描（P2）、桥接无计数（P2）。

## 2. 系统地图

### 2.1 组件

| 组件 | 位置 | 说明 |
|---|---|---|
| 宿主壳 | `examples/browser_shell/main.c` | 标签页/地址栏/菜单；`--probe` 为**真无头**调试模式（不建浏览器窗口） |
| 引擎 | `libagave.dll` | WebCore 移植（解析/布局/绘制/网络）+ ant JS 后端 |
| JS 桥 | `antbridge.dll` ← `src/javascript/ant_backend/AntBridge.c` | 预编译产物，改完要 `tools/build_antbridge.ps1 -Force` 再 relink |
| JS 后端门面 | `src/javascript/ant_backend/AntRuntime.cpp` | 每个 view 一个 `RuntimeState`；每个文档一个新 engine |
| 运行时可热改的 JS | `prelude.js`（window 引导 shim）/ `polyfills.js`（DOM 构造器补丁） | 引擎每次绑定文档时从 exe 旁加载，改完不用重链 |
| ant 引擎本体 | `H:\ant-v16`（分支 `agave-bridge`） | `libant.a` + `src/silver/*`（VM/编译）+ `src/esm/*` + `src/modules/worker/*` |

### 2.2 关键概念

- **realm**：一份 JS 世界。现在是**一个文档一个 `AntbEngine`**（`beginNewRealm()` 建、`stopRealm()` 销毁），
  旧文档的 globals / module registry / timers 随 engine 一起消失。
- **generation**：递增的 realm id。所有可能"活过文档"的资源（XHR、模块下载、待执行模块、DOM pin、文档 slot）
  都带它，卸载时按它取消。
- **owner**：桥接 wrapper 的归属（= 文档），文档卸载时按 owner 释放。
- **slot**：同一 engine 内多个文档（iframe 场景）的 DOM 上下文编号；回调进入前 `activateSlot()` 切回去。
- **无头探针**：`--probe` 的退出码是唯一权威判据（`0` OK / `1` usage / `2` 超时未加载 / `3` 超时无 exit /
  `4` 无 exit / `5` 加载错误 / `6` 空文档），细节见 `AGENTS.md`。

### 2.3 三条时间轴（导航 → realm → 模块与事件）

```
navigate → begin → clear() → provisional
   ├─ dispatchWillClose      → AntRuntime::documentWillUnload()
   │                            pagehide → unload → 丢监听器 → 按 owner 释放 wrapper
   │                            → stopRealm(): cancelInFlightResources(gen)
   │                                          → 丢 XHR / 模块下载 / pending / deferred
   │                                          → 销毁旧 engine（globals/registry/timers 一并消失）
   ├─ makeDocumentView       新文档对象诞生
   └─ dispatchDidCommitLoad
committedLoad → AntRuntime::beginNewRealm()   新 generation + 新 engine
              → AntRuntime::bindDocument()      装 shim + prelude + polyfills
   └─ addData → 解析
        ├─ inline script    → 立即求值
        ├─ external script  → 同步加载后求值
        ├─ defer script     → queueDeferredScript()（解析后按文档顺序执行）
        └─ module script    → prefetchModuleGraph() 下载 + queueModule() 排队
解析结束 → Document::finishedParsing
             → dispatchEvent(DOMContentLoaded)          ← 第 1 次（⚠ 见 P0-c）
             → FrameLoaderClientMC::dispatchDidFinishDocumentLoad
                  → flushDeferredModules()            模块求值（只执行当前 generation）
                  → AntRuntime::dispatchWindowEvent("DOMContentLoaded")   ← 第 2 次
        → dispatchDidFinishLoad → dispatchWindowEvent("load")
```

Web Worker 走另一条独立时间轴：`new Worker(url)` → prelude 包装类解析 URL（`data:`/`blob:`/`file:` 直接交给
ant 原生构造器；http(s) 走异步桥取源码 + preamble 后包成 `data:` URL）→ ant 起独立线程 + 独立 isolate。

**入口模块管线（方案 A）的完整调用栈**——三条泳道（宿主主线程 / 引擎线程 / WinHTTP worker）、
入口模块的单槽等待位、"模块求值只在解析调用栈里发生"的约束、`file:` 模块的伪装层——
见 [`docs/architecture/module-pipeline.md`](docs/architecture/module-pipeline.md)。

## 3. 怎么验证一个改动

1. **单页**：`agave_shell.exe --probe <url> --eval "probeExit(<表达式>)" --wait <ms> --settle <ms> --timeout <ms>`，
   看 stdout 的 `[exit] ...` 与退出码；stderr 是页面 console / 引擎日志。
2. **多轮/生命周期**：加 `--reload n`（每轮 eval 一次）；`AGB_EVT_DEBUG=1` + `AGB_NET_LOG=1` 拼时间线。
3. **回归清单**（`debug/` 下的页面，全部可 `--probe` 直接跑）：
   `life.html`（生命周期）、`evalenv_test.html`（eval 语义）、`expando_test.html`（元素 expando）、
   `sched.html`（调度通道）、`font_test.html`（computed style）、`csscheck.html`（CSS 能力矩阵）、
   `clipcheck.html`（嵌套裁剪）、`scrollcheck.html`（滚动重绘）、`beforeunload.html`、`defercheck.html`、
   `newtab.html`、`worker_support.html`。
4. **两个"大图"**：`https://chat.deepseek.com/`（登录页 `inp=2 all=129`、无 `antjs` 错误）、
   `https://browserscore.dev/`（Vue 模块图）。
5. **现成脚本**：`tools/run_worker_tests.ps1`（Worker 九项）、`tools/bench_compile.ps1`（启动墙钟/CPU）、
   `tools/measure_cpu.ps1`（CPU 占用）、`tools/check_window_visible.ps1`（无头探针确实不出窗口）。

探针参数、退出码、环境变量全表、cdb 配方、噪音对照见
[`docs/debugging.md`](docs/debugging.md)。

## 4. 待办（按优先级，含验收标准）

### P0-b 图就绪与执行解耦
- 现状：`moduleFetch()` 在 engine 线程上 `pump()+Sleep(0)` 同步等模块 body（5s/模块上限），
  `flushDeferredModules()` 在解析结束立刻执行，图没下完就补下载。
- 动作：改成"图就绪回调触发执行"，或把等待移出 engine 线程。
- 验收：154 模块图的 `load` 事件不再被推迟几十秒；解析不被网络阻塞。

### P0-c DOMContentLoaded 双发
- 现状：`Document::finishedParsing()` 派发一次（进 ant 监听表），`FrameLoaderClientMC::dispatchDidFinishDocumentLoad()`
  又 `AntRuntime::dispatchWindowEvent("DOMContentLoaded")` 一次，且 `flushDeferredModules()` 夹在两者之间
  → 页面看到 **DCL → 模块执行 → DCL**，标准是 **模块执行 → DCL**。
- 动作：二选一——WebCore 侧那次不再进 ant 监听表，或去掉 ant 侧那次。
- 验收：`life.html` 日志里 DCL 只出现一次，且出现在模块求值之后。

### P1-a 模块调度状态收敛 per-realm
- 现状：`g_moduleCache`（保留全局，内容寻址没问题）/ `g_moduleSession` / `g_modulePending` / `g_deferredModules`
  是文件级全局，两个标签页共用同一批下载槽与 pending 队列（现在靠 generation 打补丁）。
- 动作：session / pending / deferred 移到 `RuntimeState`。
- 验收：两个标签页同时加载模块图互不抢占；A 页排队的模块不会被 B 页执行。

### P1-c 清 `g_documentParsed` 死标志
- 现状：`AntRuntime.cpp:3164` 声明、`:3198` 读取，从不置 true → "立即执行模块"分支是死代码。
- 动作：要么真正维护它（解析完成时置位），要么删掉该分支。
- 验收：模块执行时机只有单一判定来源。

### P1-d worker 里的网络
- 现状：worker 线程里 `fetch` 不可用（ant 的 fetch 需要它自己的运行时网络栈，worker isolate 里没有），
  `XMLHttpRequest` 是 undefined；`importScripts` 只能加载父侧预取过的**字面量** URL；worker 内相对
  `import()` 解析不了（入口是 `data:` URL）。
- 影响：pdf.js / wasm / 需要二次取资源的 worker 库会失败；纯计算类不受影响。
- 动作（只改 `prelude.js`）：worker 侧 preamble 提供 RPC 版 `fetch`/`XMLHttpRequest` —— 请求包
  `{__agbNet:{id,url,method,body}}` 走 `postMessage` 到父侧，父侧 wrapper 在原生 worker 的 message 里拦截，
  用现成的 `__agbHttpRequestEx` 取回（base64）再 `postMessage` 回复；控制包在两侧都要从页面可见的
  `onmessage` 里过滤掉。`importScripts` 的动态 URL 仍然做不到同步（规范要求同步），只能父侧预取 + 查表。
- 验收：`debug/ws_workerfetch.html` 从 `fetch-err` 变成 `fetch=<字节数>`；动态 `importScripts` 给出清晰错误。

### P2 prefetch 换真解析
- 现状：`modulePrefetchImports()` 用 `body.find("from")` 字符串扫描，字符串字面量会误判，预下载的图不完整。
- 动作：用引擎自己的 import 解析（或至少跳过字符串/注释再扫）。
- 验收：预下载的图完整，求值阶段零同步补下载。

### P2 桥接可观测性
- 动作：给桥接加计数输出（wrapper 数 / class 槽位 / 每 engine 的 root 数），`AGB_EVT_DEBUG` 同一开关。
- 验收：表耗尽不再是静默失败（能提前在日志里看到余量）。

## 5. 已知限制（不是 bug，但接手前要知道）

- **UNC 路径**：`\\server\share` 会规范成 `file://server/share/...`，但引擎的 file loader 不认 URL 的 host
  （会变成 `file:///share/...` 而加载失败）。要支持得改 `ResourceHandle` 的 file 分支（R23）。
- **入口模块管线的 5 条限制**：动态插入/async 入口仍走 CachedScript；入口 `<script>` 的 `load`/`error` 不派发；
  `import.meta.url` 对 file: 模块显示伪装 URL（`http://file.ant/...`）；DCL 之后到达的 inline 模块不再执行；
  iframe 的模块入口回退 CachedScript（详见 `docs/lessons.md` R7；调用栈见
  [`docs/architecture/module-pipeline.md`](docs/architecture/module-pipeline.md)）。
- **一个 view 一个 realm**：`window` 对象每 engine 一个，iframe 与主 frame 共享它
  （`dispatchDidFinishDocumentLoad` 因此跳过非主 frame，见 `FrameLoaderClientMC.cpp`）。
- **CSS 能力边界**：grid 布局、transform 绘制、非 `-webkit-` 的 box-shadow 未实现；
  `var()` 在解析期展开（跨 sheet 失效、同名取首次定义）——`debug/csscheck.html` 是能力矩阵（R11）。
- **wasm 是解释执行**：导出函数跨界约 3.9µs/次，适合"一次调用跑大循环"（R19）。
- **worker 无网络 / 无 `SharedWorker`/`ServiceWorker`**：见 P1-d 与 R22。
- **GC root 只能到 engine 边界**（ant 没有 unpin）：现在 per-document engine 让这一点等价于"每文档释放"（R4）。

## 6. 关键代码位置索引

| 主题 | 位置 |
|---|---|
| realm 生命周期 | `AntRuntime.cpp`：`stopRealm()` / `beginNewRealm()` / `documentWillUnload()`；`createContext()`（每 view 一个） |
| generation / 在途资源 | `AntRuntime.cpp`：`currentGeneration()` / `cancelInFlightResources()`；XHR `XhrJob::m_generation`、模块 `ModuleEntry::generation`、延迟模块 `DeferredModule::generation` |
| DOM pin / 文档 slot | `AntRuntime.cpp`：`releasePinnedNodes()` / `wrapNode()` / `slotForDocument()` / `activateSlot()` |
| 模块调度 | `AntRuntime.cpp`：`moduleStart()` / `moduleFetch()`（同步等待）/ `modulePrefetchImports()`（字符串扫描）/ `queueDeferredScript()` / `flushDeferredModules()` |
| 入口模块时序（三条泳道图） | [`docs/architecture/module-pipeline.md`](docs/architecture/module-pipeline.md) |
| 桥接释放 | `AntBridge.c`：`antb_wrap_native_owner` / `antb_release_owner` / `agbReleaseEngineWrappers` / `agbReleaseEngineClasses` / `antb_engine_destroy` |
| 页面时序挂点 | `FrameLoaderClientMC.cpp`：`dispatchWillClose` / `committedLoad` / `dispatchDidFinishDocumentLoad` / `dispatchDidFinishLoad` |
| WebCore 侧限制 | `src/webcore/loader/FrameLoader.cpp`（unload 门禁）、`src/webcore/dom/EventTargetNode.cpp`（事件转发边界）、`src/webcore/dom/Document.cpp`（`finishedParsing` 的 DCL） |
| 地址栏输入 → URL | `examples/browser_shell/main.c`：`parseShellArgv()` / `normalizeUrl()` / `fileUrlFromLocalPath()`；`file:` URL 只允许单次转义 → `KURL::encode_string()`（幂等） |
| 无头探针 | `examples/browser_shell/main.c`：`probeRun()`（`WS_POPUP` + `WS_EX_TOOLWINDOW` 隐藏父窗口）+ `probeWndProc()`；`AGB_PROBE_SHOW=1` 才显示 |
| Web Worker | `prelude.js` 末尾「Web Workers」段（`_agbWorkerStart` / `_agbWorkerPreamble` / `_agbWorkerPrefetch`）；ant 侧 `H:\ant-v16\src\modules\worker\{web,core}.c`；栈帧短名 `esm_frame_filename`（`H:\ant-v16\src\esm\loader.c`） |
| 回归页 / 脚本 | 页面在 `debug/`（见 §3）；`tools/run_worker_tests.ps1`、`tools/check_window_visible.ps1`、`tools/bench_compile.ps1`、`tools/measure_cpu.ps1` |
