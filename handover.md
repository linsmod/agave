# handover：agave 浏览器引擎（现状 / 待办 / 代码索引）

> 这一篇只写「现在是什么样、还差什么」。
> 历史经验 —— 每个缺陷的 现象 → 根因 → 修法 → 验证（R1…R26，含设计约束）——在
> [`docs/lessons.md`](docs/lessons.md)；构建、探针、cdb、环境变量的**权威**手册是
> [`AGENTS.md`](AGENTS.md)，这里不重复。
>
> 最近更新：2026-11-04（**每个 frame 一个 window 对象（R31）**、无头截图改走探针 mirror 缓冲（R32）、
> R30 iframe 视口按帧求值、R29 缩放图片绘制、R28 `:empty` 失效、R27 `el.on*` 事件属性、
> R26 301/302 对 shell 可见、R22 Web Worker、R19 wasm 二进制通道）。

## 1. 现状

**能跑通**：页面加载/解析（含 `defer`/`async`/ESM 模块）、DOM 桥接、computed style、布局与绘制、
滚动、鼠标/键盘事件、定时器、XHR/fetch（二进制 + POST）、localStorage、Web Worker、`file:` 本地页面、
标签页/地址栏/查找/书签、`beforeunload`、301/302 重定向（地址栏更新 + `[nav] redirect`）、
元素事件处理属性 `el.onload/onerror = fn`（R27）、`window.postMessage`（同/跨 frame）、
`:empty` 等结构选择器随子节点变化失效（R28）、CSS 缩放的图片正确绘制（R29）、
iframe 的视口度量按帧求值（R30）、**每个 frame 自己的 `window` 对象**（R31：`parent !== self`、
`parent.document` 是父文档、`frames`/`length`/`name`/`frameElement` 按 frame tree 回答）、
**无头截图**（R32：`--probe --shot`，屏幕上不出现任何窗口）、
**一批 jsdom/DOM 缺口**（R34：元素树遍历、反射属性、`insertAdjacent*`、`form.submit/action/elements`、
`img.complete/naturalWidth/decode`、`iframe.contentWindow/contentDocument`、`document.forms` 等真实集合、
`activeElement`/`hasFocus`/`visibilityState`/`importNode`/`elementFromPoint`、`document.fonts` 桩）。
**选择器引擎**（R35：`querySelector(All)`/`matches`/`closest` 支持选择器列表、组合器、属性选择器六个操作符、
`nth-child`/`not` 等伪类；实现是 `src/javascript/ant_backend/AgbSelector.h`）。
**History 状态机**（R36：`pushState`/`replaceState` 保存 state、`history.state`/`length` 是真的、
`back/forward/go` 恢复 URL 并派发带 `state` 的 `popstate`；跨文档的前进后退仍交给 FrameLoader）。
**键盘文本输入**（R37：字符只来自 `WM_CHAR`、不再用虚拟键码猜；每个字符插一次；keydown 的
`preventDefault` 能拦住字符；`keypress` 的 `keyCode`/`charCode` 是真字符；Backspace 删一个；
`event.shiftKey/ctrlKey` 有值；`--keys`/`--pre-eval` 无头回归）。
**输入法输入**（R38：控件是 Unicode 窗口、宿主消息循环用 `GetMessageW`/`DispatchMessageW`、
`WM_IME_CHAR` 与 `WM_CHAR` 同一处理器、代理对配对成 4 字节 UTF-8；顺带修掉 grapheme 迭代器对
emoji 的死循环和「emoji 之后光标退到它前面」；`--ime` 无头回归）。

**CSS 主题化**（R39：自定义属性按元素级联并继承、`!important`、`getComputedStyle(el).getPropertyValue('--x')`；
`display:flex` 的子元素按现代规范 blockify（inline 子项不再是 0×0）；`prefers-color-scheme` 由宿主按
Windows `AppsUseLightTheme` 注入 `AGB_COLOR_SCHEME`，CSS 与 `matchMedia` 同一答案；`matchMedia` 另支持
width/height/orientation/resolution；无媒体类型的 `@media (min-width: …)` 能解析；无前缀 `box-sizing` 生效）。
**圆角裁剪不再吞掉脏矩形**（R40：`clip(path)` 与设备矩形求交，聚焦/局部重绘时不再把文字擦掉）。
**flex gap / aspect-ratio / `:is()` `:where()`**（R41：`gap`/`row-gap`/`column-gap` 按现代语义插在 in-flow 子项之间，
简写 1 值=两轴；`aspect-ratio` 在另一轴确定时推另一轴；`:is()`/`:where()` 解析期展开成选择器列表；
placeholder 改为 `paint()` 里独立绘制、不再写进输入框的值文本，聚焦空框时折叠全选以免被 `::selection`
高亮，可见性实时看 `value` 是否为空）。
**footer 居中 / iframe 边框**（R42：`safe`/`unsafe` 对齐前缀被解析、column flex 里绝对定位子项按
`align-items` 定 static position、`iframe.frameBorder` JS property 反射回 `frameborder` 属性；
deepseek 备案号居中、二维码 iframe 无 2px 外框）。
**`calc()` 常量折叠**（R43：纯常量 calc 在解析期折成单值、自定义属性里 `calc(var(--token) + …)` 先做
sheet 级常量代入；表单 feedback 的 `min-height` 由 4px 恢复 22px；混合单位 calc 仍不支持）。

**一个 frame 只有一份文档 / 探针加载门控**（R44：`FrameLoader::init()` 不再提交初始空文档，
第一次 `load()` 解析出来的就是唯一那份；探针的"加载完成"改用引擎 `on_progress(finished)`，
`--wait` 从真文档 load 起算；本地路径带 `?query`/`#fragment` 不再被当成网络地址）。
**输入事件补齐**（R45：`event.key`/`code`、`isTrusted`、`{once:true}`、`beforeinput`（含 data/inputType
与 preventDefault 取消插入）、`input` 的 inputType/data；回归页 `tools/pages/cap_input_events.html`）。
**加载失败一定通知宿主**（R46：`FrameLoaderClientMC` 的错误工厂不再返回 null —— null 在 FrameLoader 里
是"没有错误"的哨兵，阻塞端口/不支持的 URL/不支持的 MIME 因此全都静默；`MC_ERROR_CODE` 新增
`ERR_NET_POLICY_*` 一族区分"浏览器主动拒绝"与网络故障；`cancelledError` 故意保持 null）。
**下载管理器**（R47：二进制/附件不再报错而是落盘 —— 决策在 `dispatchDecidePolicyForMIMEType`，
`ResourceLoader::detachHandle()` 把活着的连接交给进程级 `DownloadManager`，`.part` 写完再 rename，
文件名支持 `filename*`/中文/去重；`cb_download` + `on_download` 三层状态，探针下载 → exit 0。
设计见 [docs/download-manager.md](docs/download-manager.md)，回归服务器 `tools/download_server.py`）。
**内部页 runtime**（R48：下载面板是 `agave://downloads` 的 HTML 页 + 标签页（`Ctrl+J`）；
`agave.*` 只在内部 realm 注册，普通页 `typeof agave === 'undefined'` —— 能力按 realm 隔离；
`agave://` 由引擎用 `SubstituteData` 供给，`document.URL` 就是 agave 地址）。
**about:blank 同步建文档**（R49：`about` 进内置 scheme 表；空加载没有数据块，`begin()` 从不被触发，
现在在 `DocumentLoader::finishedLoading()` 提交之后显式 `createEmptyDocument()` ——
`appendChild(iframe)` 返回时 `contentDocument` 就有文档，`window.open('about:blank')` 同样）。

**已经被检查过的"看起来不能用"**（别再当缺陷重开）：ant 的 Worker 是真的（R22）、ant 的 wasm 引擎是真的
（R19）、渐变/`var()`/flex 大多可用（R11/R39）、滚动与 hover 重绘正确（R13/R14/R40）、刷新后 realm/监听器会清理（R9/R10）。

**还没做**（§4 有验收标准）：模块图 flush 阻塞 engine 线程（P0-b）、DOMContentLoaded 双发（P0-c）、
模块调度状态不是 per-realm（P1-a）、`g_documentParsed` 死标志（P1-c）、worker 里没有网络（P1-d）、
能力缺口剩 8 项（`attachShadow`/`createRange`/`createTreeWalker`/`execCommand` 等，P2）、
prefetch 仍是字符串扫描（P2）、桥接无计数（P2）。能力缺口的完整实测见
[docs/capability-gaps.md](docs/capability-gaps.md)。

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
   看 stdout 的 `[exit] ...` 与退出码；stderr 里有**默认打开**的生命周期日志
   （`[evt]`：commit/realm/bindDocument/DCL/load/teardown）、导航动作日志
   （`[nav]`：open/request/submit/client-redirect/redirect/popup）与 GUI 输入日志（`[ui]`），
   关掉用 `AGB_EVT_LOG=0` / `AGB_NAV_LOG=0` / `AGB_UI_LOG=0`（详见 [docs/debugging.md](docs/debugging.md)）。
   这些日志（以及 `[shell]`/`[probe]`/`[flag]`/页面 console 的 `[con-*]`）统一经 `src/webview/AgbLog.h`
   输出，行首是 `[<进程内相对ms>]` 时间戳，跨 DLL 共享同一个原点，所以 stderr 直接就是一条时间线。
2. **多轮/生命周期**：加 `--reload n`（每轮 eval 一次）；`AGB_EVT_DEBUG=1` + `AGB_NET_LOG=1` 拼时间线。
3. **回归清单**（`debug/` 下的页面，全部可 `--probe` 直接跑）：
   `life.html`（生命周期）、`evalenv_test.html`（eval 语义）、`expando_test.html`（元素 expando）、
   `sched.html`（调度通道）、`font_test.html`（computed style）、`csscheck.html`（CSS 能力矩阵）、
   `clipcheck.html`（嵌套裁剪）、`scrollcheck.html`（滚动重绘）、`beforeunload.html`、`defercheck.html`、
   `newtab.html`、`worker_support.html`。
4. **两个"大图"**：`https://chat.deepseek.com/`（登录页 `inp=2 all=129`、无 `antjs` 错误）、
   `https://browserscore.dev/`（Vue 模块图）。
5. **现成脚本**：`tools/run_worker_tests.ps1`（Worker 九项）、`tools/run_nav_tests.ps1`（`[nav]` 八项，含 302 重定向）、
   `tools/run_typing_tests.ps1`（键盘输入五项：打字/退格/Tab 聚焦/keydown 过滤/textarea 回车）、
   `tools/bench_compile.ps1`（启动墙钟/CPU）、`tools/measure_cpu.ps1`（CPU 占用）、
   `tools/check_window_visible.ps1`（无头探针确实不出窗口）、`tools/probe.ps1` / `tools/screenshot.ps1`
   （无头探针 / 无头截图，屏幕上什么都不出现）。
6. **看页面踩了哪些缺口**：`[gap-jsdom]` 默认开启，每个已知缺口第一次被访问时打一行，
   名单在 `src/javascript/ant_backend/AgbGapJsdom.h`（= `docs/capability-gaps.md` §1 的机器可读版）。

探针参数、退出码、环境变量全表、cdb 配方、噪音对照见
[`docs/debugging.md`](docs/debugging.md)。

## 4. 待办（按优先级，含验收标准）

### P0-a per-frame `window` 对象（✅ 2026-11-04 完成，R31）
- 现状：每个 frame 一个 `Window` 代理（tag `WND1`，native = 该 frame 的 Document）。`window`/`self`/`top`/
  `parent`/`frames`/`length`/`name`/`frameElement`/`document` 按 frame 回答，其余属性委托给共享全局
  （`parent.jQuery`、跨 frame 调用照旧）。顶层文档仍然直接用全局对象（`window === self === top`），零风险。
- 验收已过：`tools/pages/cap_window_identity.html` —— 子 frame `parent!==window`、`parent.doc!==doc`、
  `parentTitle=cap_window_identity`、`top===parent`、`parent.len=1`、`frames[0]===me`、`name=kid`、`frameEl=kid`、
  `inner=200x120`；DeepSeek 登录页二维码完整落在 200×200 iframe 内，`cv2.QRCodeDetector` 解出
  `https://open.weixin.qq.com/connect/confirm?uuid=...`。
- **已知限制**：子 frame 的顶层 `var x` 仍写进共享全局（要修只能每 frame 一个 engine，方案 A，不做）。
  见 `docs/architecture/frame-window.md` §5。

### P0-d iframe 的 `contentWindow` / `contentDocument`（✅ 2026-11-04 完成，R34）
- `contentWindow` 返回该 frame 的 window 对象（顶层是全局、frame 是 `Window` 代理），
  `contentDocument` 返回它的 document 对象；子文档还没建好时是 `null`（`about:blank` 例外：
  R49 之后 `appendChild(iframe)` 返回时已经同步有文档）。
- 验证：`cap_dom.html` → `iframeCw=yes`；`cap_window_identity.html` 里 `contentWindow.document === contentDocument`。

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

### P1-f DOM 集合 / 表单 / History 补全
- 现状：`document.forms/links/images/anchors/scripts` 恒为空数组；`form.submit()`/`form.action`
  未桥接；`history.pushState` 只改 document URL（无 state/length/popstate）；`localStorage` 不跨文档。
  实测值与复测命令见 [`docs/capability-gaps.md`](docs/capability-gaps.md) §1。
- 动作：①`AntRuntime.cpp` 的 `documentGetter` 集合分支改为包装真实 `HTMLCollection`/NodeList；
  ②类表补 `HTMLFormElement` 的方法/属性、`HTMLImageElement` 的 `complete/naturalWidth/width`；
  ③History 建真实条目并派发 `popstate`（或明确 shim 语义并在文档写死）；
  ④实现 `window.postMessage` + `message` 事件（跨 frame）；⑤`querySelector(All)` 支持属性选择器；
  ⑥storage 改成每次读取都走磁盘/共享表，而不是只在 engine 重建时读。
- 验收：`tools/pages/cap_dom.html` / `cap_history.html` / `cap_storage_a.html` / `cap_selectors.html` /
  `cap_postmessage.html` 的 `[exit]` 值与 `docs/capability-gaps.md` 的"已支持"一致；
  DeepSeek 登录页的微信二维码能显示（`capability-gaps.md` §6 的案例）。

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
- **CSS 能力边界**：grid 布局、transform 绘制、`position:sticky` 未实现（按 static 降级）；
  `aspect-ratio`、flex `gap`/`row-gap`/`column-gap`、`:is()`/`:where()`、`prefers-color-scheme`、
  元素级级联的 `var()`（含 fallback / `!important` / 继承）、`box-sizing`（无前缀）、`box-shadow`、
  `safe`/`unsafe` 对齐、常量 `calc()` 折叠（混合单位 calc 仍不支持）
  均已支持（R39/R41/R42/R43）。**完整实测值、影响与复测命令见 [`docs/capability-gaps.md`](docs/capability-gaps.md) §2**
  （R11/R39/R41/R42/R43 是当时的过程记录）。
- **wasm 是解释执行**：导出函数跨界约 3.9µs/次，适合"一次调用跑大循环"（R19）。
- **worker 无网络 / 无 `SharedWorker`/`ServiceWorker`**：见 P1-d 与 R22。
- **GC root 只能到 engine 边界**（ant 没有 unpin）：现在 per-document engine 让这一点等价于"每文档释放"（R4）。
- **localStorage 不跨文档**：同一进程内 A 文档写入后导航到 B，B 读到 `null`（cookie 正常；跨进程能读到
  旧值）。写盘发生在文档 teardown，可读要等下一次 engine 重建；导航类测试页需要跨文档状态时用 cookie，
  见 `tools/pages/navcheck_history.html` 的注释。

## 6. 关键代码位置索引

| 主题 | 位置 |
|---|---|
| realm 生命周期 | `AntRuntime.cpp`：`stopRealm()` / `beginNewRealm()` / `documentWillUnload()`；`createContext()`（每 view 一个） |
| generation / 在途资源 | `AntRuntime.cpp`：`currentGeneration()` / `cancelInFlightResources()`；XHR `XhrJob::m_generation`、模块 `ModuleEntry::generation`、延迟模块 `DeferredModule::generation` |
| DOM pin / 文档 slot | `AntRuntime.cpp`：`releasePinnedNodes()` / `wrapNode()` / `slotForDocument()` / `activateSlot()` |
| 模块调度 | `AntRuntime.cpp`：`moduleStart()` / `moduleFetch()`（同步等待）/ `modulePrefetchImports()`（字符串扫描）/ `queueDeferredScript()` / `flushDeferredModules()` |
| 事件处理属性（`el.on*`） | `AntRuntime.cpp`：`nodeGetter`/`nodeSetter` 的 `on*` 分支 + `setNodeEventHandler()` / `findNodeEventHandler()`；回归 `tools/pages/cap_script.html`（R27） |
| 能力缺口（JS/DOM/CSS） | [`docs/capability-gaps.md`](docs/capability-gaps.md)（实测值 + 一条命令复测）；探测页 `tools/pages/cap_*.html` |
| 301/302 重定向 | `WinHttpSession.cpp`（`Request::followRedirects=false` → `WINHTTP_OPTION_REDIRECT_POLICY_NEVER`）；`ResourceHandleNet.cpp` `onResponse()`（3xx → `willSendRequest` → `ResourceHandleManager::redirect()`）；回归 `tools/nav_redirect_server.py` |
| 入口模块时序（三条泳道图） | [`docs/architecture/module-pipeline.md`](docs/architecture/module-pipeline.md) |
| 桥接释放 | `AntBridge.c`：`antb_wrap_native_owner` / `antb_release_owner` / `agbReleaseEngineWrappers` / `agbReleaseEngineClasses` / `antb_engine_destroy` |
| 页面时序挂点 | `FrameLoaderClientMC.cpp`：`dispatchWillClose` / `committedLoad` / `dispatchDidFinishDocumentLoad` / `dispatchDidFinishLoad` |
| 默认日志收口 | `src/webview/AgbLog.h`：`agbLog(channel, fmt, ...)` + `agbLogElapsedMs()`；通道 `evt/nav/ui/shell/probe/flag`，时间原点跨 DLL 共享；格式 `[<ms>ms] [tag] 正文` |
| `[nav]` 导航日志 | 钩子在 `FrameLoaderClientMC.cpp`（request/new-window/submit/redirect/fail/blocked）、`WebEngine.cpp`（`macross_view_open_url/back/forward/reload/stop`）、`AgaveWebView.c`（popup/download）；开关 `AGB_NAV_LOG=0` |
| `[nav]` 回归 | `tools/run_nav_tests.ps1` + `tools/pages/navcheck_*.html` + `tools/nav_redirect_server.py` |
| WebCore 侧限制 | `src/webcore/loader/FrameLoader.cpp`（unload 门禁）、`src/webcore/dom/EventTargetNode.cpp`（事件转发边界）、`src/webcore/dom/Document.cpp`（`finishedParsing` 的 DCL） |
| 地址栏输入 → URL | `examples/browser_shell/main.c`：`parseShellArgv()` / `normalizeUrl()` / `fileUrlFromLocalPath()`；`file:` URL 只允许单次转义 → `KURL::encode_string()`（幂等） |
| 无头探针 | `examples/browser_shell/main.c`：`probeRun()`（`WS_POPUP` + `WS_EX_TOOLWINDOW` 隐藏父窗口）+ `probeWndProc()`；`AGB_PROBE_SHOW=1` 才显示 |
| Web Worker | `prelude.js` 末尾「Web Workers」段（`_agbWorkerStart` / `_agbWorkerPreamble` / `_agbWorkerPrefetch`）；ant 侧 `H:\ant-v16\src\modules\worker\{web,core}.c`；栈帧短名 `esm_frame_filename`（`H:\ant-v16\src\esm\loader.c`） |
| 回归页 / 脚本 | 页面在 `debug/`（见 §3）；`tools/run_worker_tests.ps1`、`tools/run_nav_tests.ps1`、`tools/run_typing_tests.ps1`、`tools/check_window_visible.ps1`、`tools/bench_compile.ps1`、`tools/measure_cpu.ps1` |
| 键盘文本输入 | `src/webview/WebView.cpp`：`keyDown()`（只派发 keydown，记住 `preventDefault`）、`textInput()`（WM_CHAR：过滤控制字符 → `handleCharacterEvent`）；`src/webcore/page/EventHandler.cpp`：`keyEvent()` / `handleCharacterEvent()`（keypress + 默认插入）；`src/webview/KeyEventMC.cpp`：`PlatformKeyboardEvent`（字符不来自 vk）；回归 `tools/run_typing_tests.ps1`（R37） |
| 输入法 / Unicode 输入 | `src/webview_control/AgaveWebView.c`：`agwRegisterClass()`（`RegisterClassW`，Unicode 窗口）、`agwUtf16ToUtf8()`（代理对配对）、`agwProc` 的 `WM_CHAR`/`WM_IME_CHAR`；宿主循环 `GetMessageW`/`DispatchMessageW`（`examples/browser_shell`、`examples/embed_demo`）；死循环与光标：`src/webcore/platform/unicode/TextBoundary.cpp`（`PrecedingCharsBreak`/`FollowingCharsBreak` 的 `c_ERROR_TYPE`）、`src/webcore/rendering/RenderText.cpp`（`previousOffset`/`nextOffset` 跨代理对）；回归 `--ime` 三例（R38） |
