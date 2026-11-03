# handover：agave 刷新（reload）时序与模块图架构改造

> 本文自包含：只读本文即可接手，不需要翻其它文件。所有结论都带代码锚点。
> 状态：**改造已落地并编译通过，但未做浏览器验证**（见 §5 未验证项与风险）。

---

## 0. 结论前置（先看这里）

### 0.1 一句话结论
agave 的页面刷新路径存在**四类结构性缺陷**：旧文档没有拆卸事件、JS realm 不隔离、在途资源不按文档归属、桥接层全局表不随引擎释放。模块图（ESM）与页面时序的耦合是**第二个独立问题**：模块下载/排队是进程级全局，而页面时序是文档级，且"图就绪"与"解析结束"之间没有同步点。

### 0.2 优先级排序（按"是否阻断正确性"）

| 级别 | 事项 | 状态 | 依据 |
|---|---|---|---|
| **P0-a** | 模块 registry 的作用域必须与 realm 一致 | **已解决（真因是 polyfills，见 R10）** | 隔离模式下 154 模块大图的 import 变 undefined，4/4 复现 |
| **P0-b** | 模块图"发现/下载"与"执行"解耦，flush 不得阻塞 engine 线程 | 未做 | `moduleFetch` 在 engine 线程同步等网络，5s/模块 |
| **P0-c** | DOMContentLoaded 双发导致事件顺序错乱 | 未做 | WebCore 一次 + ant 合成一次，夹着模块执行 |
| **P1-a** | 模块调度状态（session/槽位/pending/deferred）收敛为 per-realm | 部分 | 现在是进程级全局，已用 generation 打补丁 |
| **P1-b** | 模块错误必须冒泡（嵌套模块异常现在完全静默） | 未做 | `evaluateModule` 只报最外层 |
| **P1-c** | `g_documentParsed` 死标志清理 | 未做 | 从不置 true，立即执行分支是死代码 |
| **P2** | prefetch 启发式（字符串扫 `from`）换成真 import 解析 | 未做 | `modulePrefetchImports` |
| **P2** | 桥接层加可观测计数（wrapper/class/root 余量） | 未做 | 表耗尽是静默的 |

### 0.3 本轮已落地的改造（编译通过，见 §4）
1. **teardown 事件**：旧文档在提交替换前收到 `pagehide` → `unload`（此前完全没有）。
2. **realm 隔离**：每文档一个 VM（旧 realm 的 globals / module registry / timers 不再泄漏到新文档）。
3. **generation 归档**：session/state 绑定递增 id；卸载前按 id 取消在途资源（XHR / 模块下载 / 待下载队列 / 待执行模块队列）。
4. **桥接层释放**：wrapper 表与 class 表改为可回收（空闲链表 / 空槽复用），engine 销毁时按 engine 清理；wrapper 支持 owner（文档）归属，文档卸载时按 owner 释放。
5. **入口模块管线合一**（方案 A）：`<script type="module" src>`（非 async）改走模块 fetcher——与 import 同一缓存、同一 generation 取消归属；tokenizer 单槽等待位（复用 CachedScript 的阻塞/恢复机制）保证模块在**解析流程内**执行、DCL 按规范落后于模块执行。
6. **file:// 模块直读**：file: URL 模块本地读盘、同步完成；交给引擎前伪装成 `http://file.ant/<path>`（引擎 esm_is_url 只认 http(s)，fetcher 端还原），import.meta.url 显示伪装形态。
7. **错误栈冒泡**（P1-b）+ **`--offline` 确定性重放**：见提交 `4efdef6`。

---

## 1. 系统地图（谁在什么时候做什么）

### 1.1 组件与入口
- **agave_shell.exe**（`examples/browser_shell/main.c`）：宿主壳；`--probe` 为无头调试模式。
- **libagave.dll**：引擎（WebCore 移植 + ant JS 后端）。
- **antbridge.dll**：JS 桥（**预编译产物**，`src/javascript/ant_backend/AntBridge.c` → `H:\ant-v16\bridge`）。
- **ant JS 后端**：`src/javascript/ant_backend/AntRuntime.cpp`（C++ 门面，per-view 一个 VM）+ `AntBridge.c`（C 桥）+ `polyfills.js`（引擎旁加载，可直接改）。

### 1.2 关键概念
- **realm**：一份 JS 世界（全局对象 + module registry + timers）。改造后 = **一个文档一个 realm**。
- **generation**：realm 的递增 id（进程级单调）。所有可能"活过文档"的资源都带它，卸载时按它取消。
- **owner**：桥接 wrapper 的归属（= 文档）。

### 1.3 页面 / realm / 模块图 三条时间轴（改造后的现状）

```
navigate → begin → clear() → provisional
   ├─ dispatchWillClose      → AntRuntime::documentWillUnload()
   │                            pagehide → unload → 丢监听器 → 释放该文档的 wrapper
   │                            → stopRealm(): cancelInFlightResources(gen)
   │                                          → 丢 XHR / 模块下载 / pending / deferred
   │                                          → 销毁旧 VM（globals/registry/timers 一并消失）
   ├─ makeDocumentView       新文档对象诞生
   └─ dispatchDidCommitLoad
committedLoad → AntRuntime::beginNewRealm()   新 generation + 新 VM
              → AntRuntime::bindDocument()      装 shims + prelude + polyfills
   └─ addData → 解析开始
        ├─ inline script    → 立即求值
        ├─ external script  → 同步加载后求值
        └─ module script    → prefetchModuleGraph() 下载 + queueModule() 排队
解析结束 → Document::finishedParsing → DOMContentLoaded(WebCore 侧，第 1 次)
        → dispatchDidFinishDocumentLoad
             → flushDeferredModules()   模块求值（只执行当前 generation 的）
             → dispatchWindowEvent("DOMContentLoaded")  第 2 次
        → dispatchDidFinishLoad → dispatchWindowEvent("load")
```

---

## 2. 分析结论（缺陷清单 + 证据）

### 2.1 刷新路径的四类缺陷（已修）

| # | 缺陷 | 根因（代码锚点） | 修法 |
|---|---|---|---|
| D1 | 旧文档收不到 `pagehide`/`unload`/`beforeunload` | ant 侧 `dispatchWindowEvent()` 全程只被喂过 `"DOMContentLoaded"`/`"load"`（`FrameLoaderClientMC.cpp`）；WebCore 自己的 `unloadEvent` 只在 `window.close()` 时派发（`src/webcore/loader/FrameLoader.cpp:625-639`，`m_didCallImplicitClose` 门禁），且走 `EventTargetNode::dispatchWindowEvent()`，**不会**进 ant 监听表（只有 `dispatchEvent()` 才转发，`src/webcore/dom/EventTargetNode.cpp:325`） | `dispatchWillClose` → `AntRuntime::documentWillUnload()`（`AntRuntime.cpp:6364`） |
| D2 | 刷新后新文档仍看得到旧文档的 globals | VM 是**每视图**一个，`window` 就是 QuickJS 全局对象；`bindDocument()` 只把 `g_docObj`/global 的 native 指针重指向新文档，属性原样保留 | 每文档一个 VM（`beginNewRealm()`，`AntRuntime.cpp:3456`） |
| D3 | 旧文档的监听器永不清理 | `dropListenersForOldDocument()`（`AntRuntime.cpp`）长期**没有调用点** | 在 `documentWillUnload()` 里调用 |
| D4 | 每次刷新泄漏一整棵文档的 DOM | `wrapNode()` 把每个包装过的节点 `ref()` 进一个**全局** `pinned` 集合，永不释放 | 按 generation 分组，延迟一代 `deref()`（旧文档仍在用节点，立刻释放会提前 free） |

**实测（改造前，browserscore/本地 life 页）**：刷新后 `marker=string`（globals 泄漏）、无 pagehide/unload、`DOMContentLoaded` 双发、`readystatechange` 从不触发、Node 泄漏 1 文档 7 → 3 文档 58。

### 2.2 在途资源不按文档归属（已修）

| 资源 | 归属字段 | 取消动作 | 锚点 |
|---|---|---|---|
| 异步 XHR | `XhrJob::m_generation` | `ResourceHandle::cancel()` + 丢 JS 回调 + 摘 job | `AntRuntime.cpp:345` / `:497` |
| 模块下载 | `ModuleEntry::generation` | `Net::Session::cancel(id)` + 标记 failed + 回收 in-flight 计数 | `:2232` / `:1829` |
| 待下载 import 队列 | `g_modulePending` 改为 `(url, generation)` | 丢弃旧代条目 | `:1829` |
| 已排队待执行的延迟模块 | `DeferredModule::generation` | 丢弃；flush 时也只执行当前代的 | `:3215` / `:3240` / `:3254` |
| DOM 节点 pin | `pinnedNodes()[generation]` | 延迟一代释放 | `:141` / `:6217` |
| 文档 slot 表 | `docSlotGeneration[]` | realm 停止时清空（消除悬垂指针）+ 旧代槽位复用（消除 16 槽退化） | `:246` |

统一入口 `cancelInFlightResources(generation)`（`:3386`），在 `stopRealm()` 开头调用（`:3399`）——**先取消，再拆 realm**。

### 2.3 桥接层：进程级全局表不随引擎释放（已修）

| 表 | 容量 | 隔离模式下的后果 | 修法 |
|---|---|---|---|
| `g_wrappers`（wrapper 缓存 + GC root） | 65536 | 每导航一个 engine → 条目与永久 root 无限累积；`e->js` 指向已销毁 runtime；用满后 `antb_wrap_native` 返回 NULL | 空闲链表 + 按 engine / 按 owner 释放 |
| `g_classes`（类表 + proto 永久 pin） | 128 | **每次导航新建一个 "Node" 类 → 128 次导航后 DOM 绑定彻底失效** | 空槽复用 + 按 engine 释放 |
| `g_class_props[128][32]` | — | 类释放后属性表残留 | 随类一起清 |

新增 API：`antb_wrap_native_owner` / `antb_wrap_native_as_owner` / `antb_release_owner`；`antb_engine_destroy()` 现在先清表再 `js_destroy`。

### 2.4 页面时序 × 模块图的六个不合理点（**主问题，尚未修**）

**M1（最核心）模块 registry 的作用域必须等于 realm。**
模块**实例化**在 engine 内（随 `js_destroy` 消失），模块**源码**在进程级缓存里。若 ant 侧对"同一 URL"存在任何进程级状态（实例 / wrapper / 按 URL 的 key），新 realm 里拿到的就可能是未初始化实例 → 表现正是"某个 import 是 undefined"。
→ 这是隔离模型成立的前提，**必须先确认**。

**M2 图"发现/下载"与"执行"没有同步点，flush 时用同步等待补洞。**
下载是**逐层发现**（`ModuleFetchClient::onComplete` → `modulePrefetchImports` → 下一层），每层一个 RTT；而 `flushDeferredModules()` 在解析结束**立刻**执行 → 图未就绪 → `moduleFetch` 在 **engine 线程**上 `pump()+Sleep(0)` 同步等（5s/模块上限）。
→ 架构上是把"图就绪"与"解析结束"强行对齐；浏览器是后台并行下完再执行。副作用：154 模块图会把 load 事件整体推迟几十秒。
→ 也解释了此前观测：**开 `AGB_MODULE_DEBUG`（多打日志=变慢）反而成功**，纯粹是时序被挪动。

**M3 DOMContentLoaded 双发 → 事件顺序错乱。**
WebCore 的 `Document::finishedParsing` → `dispatchEvent`（进 ant 监听表）+ ant 的 `dispatchWindowEvent("DOMContentLoaded")` 各一次，而 `flushDeferredModules()` 夹在中间 → 页面看到 **DCL(WebCore) → 模块执行 → DCL(ant)**；标准是 **模块执行 → DCL**。

**M4 模块调度状态是进程级，与页面时序错配。**
`g_moduleCache` / `g_moduleSession` / `g_modulePending` / `g_deferredModules` 全是文件级全局。两个标签页共用 12 个下载槽和同一 pending 队列；A 页排队的模块会被 B 页的 flush 消费（现用 generation 打补丁，属结构外的补丁）。

**M5 模块错误没有冒泡机制。**
`evaluateModule` 失败只打一行 `[antjs] ...: module evaluation failed` 就继续下一个；**嵌套模块内部异常完全不报告**（`antb_eval_module` 只返回最外层结果）→ "某个 import 是 undefined"这类症状永远查不到根因。

**M6 prefetch 是字符串扫描的启发式。**
`modulePrefetchImports()` 用 `body.find("from")` 找 import → 模块内字符串字面量会误判 → 预下载的图天然不完整 → 求值时仍要同步补下载（是 M2 的根因之一）。

**附带**：`g_documentParsed` 是死标志（声明 + 读取，从不置 true）→ `evaluate()` 里"立即执行模块"分支是死代码，模块永远走延迟队列。功能符合 spec，但掩盖了"模块何时可执行"的真实判定。

---

## 3. 改动清单（工作区未提交）

| 文件 | 改动 |
|---|---|
| `src/javascript/ant_backend/AntRuntime.cpp` | generation 体系；realm 隔离（`stopRealm`/`beginNewRealm`）；teardown 事件；按 generation 取消在途资源；DOM pin 按代分组；文档 slot 按代回收 |
| `src/javascript/ant_backend/AntRuntime.h` | 新增 `documentWillUnload()` / `beginNewRealm()` |
| `src/webview/FrameLoaderClientMC.cpp` | `dispatchWillClose` → `documentWillUnload()`；`committedLoad` → `beginNewRealm()` + `bindDocument()` |
| `src/javascript/ant_backend/AntBridge.c` / `.h` | wrapper/class 表可回收；owner 归属；engine 销毁清理 |
| `examples/browser_shell/main.c` | 探针 `--reload <n>`：多轮 reload，每轮等新文档就绪再 eval |

`git status`：4 个文件 modified（AntBridge.c/.h 另计）。

---

## 4. 构建与验证手册

### 4.1 构建
```powershell
# 常规增量（不要 clean）
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
cmake --build e:\mybrowsers\agave\build-zig-native --parallel 16
# 完整脚本：pwsh -File tools\build_project.ps1

# ⚠️ 改过 AntBridge.c 之后必须两步，缺一则改动静默失效：
pwsh -File tools\build_antbridge.ps1 -Force
cmake --build e:\mybrowsers\agave\build-zig-native --parallel 16
```
ant 源码树在 `H:\ant-v16`（`include/` + `build-zig/libant.a`），桥接产物落到 `3rdparty/ant/bin|lib` 与 `build-zig-native`。

### 4.2 无头探针
```powershell
# 本地页直接用 file://（模块管线对 file: URL 本地直读，无需起服务）
Start-Process -WindowStyle Hidden python -ArgumentList '-m','http.server','8971','--directory','e:\mybrowsers\agave\debug'

$ev = "probeExit('kids='+document.body.children.length)"   # ⚠️ 不能含空格，见 §4.4
$p = Start-Process -FilePath 'e:\mybrowsers\agave\build-zig-native\agave_shell.exe' `
  -ArgumentList '--probe','http://127.0.0.1:8971/life.html','--eval',$ev,'--wait','800','--settle','1000','--timeout','60000' `
  -RedirectStandardOutput out.txt -RedirectStandardError err.txt -PassThru -Wait
```
退出码：`0` OK / `1` usage / `2` 超时未加载 / `3` 超时无 exit / `4` 无 exit / `5` 加载错误 / `6` 空文档。

参数：`--probe <url> [--eval js] [--wait ms] [--settle ms] [--timeout ms] [--reload n] [--offline]`。
`--reload n` 的语义：eval → reload → 轮询新文档就绪 → 再 eval，共 n+1 轮。
`--offline`：模块只从磁盘缓存取（AGB_OFFLINE=1，与 `AGB_MODULE_DEBUG` 同一环境变量约定），未命中立刻失败、不发网络；重放时跳过 TTL 过期检查。用途：暖缓存（先在线跑一次）后确定性重放模块图，把网络时序从复现变量中剔除。

### 4.3 测试页
| 文件 | 用途 |
|---|---|
| `debug/life.html` | 生命周期：readystatechange/DOMContentLoaded/beforeunload/pagehide/pageshow/unload/load + 定时器 + 每文档随机 id |
| `debug/mod/{index,main,helper,deep,def}.js` | 模块图（命名导出 / 默认导出 / 嵌套导入） |
| `debug/browserscore.html` + `bs-app.js` | Vue 页快照（远程版 `https://browserscore.dev/`） |

### 4.4 已知坑
- `Start-Process -ArgumentList` **不会自动给含空格的参数加引号** → `--eval` 里不能有空格（含空格会被拆成多个 argv → JS 语法错误）。
- PowerShell 传 `--eval` 时 `&&` 会被搅成 `;`。
- ant 解析器对 `typeof X.y` / `delete` / `for...in` 的支持与标准 JS 不同，写探针脚本时避开。
- `AGB_NET_LOG=1` 看网络阶段；WinHTTP `SendRequest` 偶发长滞是 WPAD/PAC 环境抖动，重试即可。

### 4.5 环境变量
`AGB_EVT_DEBUG=1`（bindDocument/realm 事件）、`AGB_MODULE_DEBUG=1`（模块抓取/求值）、`AGB_MODULE_CACHE=0`（关磁盘缓存）、`AGB_MODULE_CONCURRENCY=n`（并发槽，默认 12）、`AGB_JS_DEBUG=1`、`AGB_NET_LOG=1`。

---

## 5. 未验证项与风险（接手者必读）

| 项 | 状态 | 影响 |
|---|---|---|
| **R1 隔离模式下 154 模块大图求值失败** | **已二分定位（--offline 实验证实，见下）** | **P0**。修复方向见下 |
| R2 桥接层释放改造 | 已编译通过，**未做运行时验证** | 需确认：反复导航后 DOM 仍可用；wrapper/class 表不再耗尽 |
| R3 DOM pin 延迟一代释放 | 设计如此（立即释放会提前 free 旧文档节点） | 同一时刻最多多留一代 DOM 内存 |
| R4 wrapper 的 GC root 只能到 engine 边界 | ant 的 GC 无 unpin（`H:\ant-v16\include\gc\roots.h` 只有 `gc_pin_permanent` / 栈式 `gc_push_root`） | 同 engine 内换文档只能回收 cache，root 仍在；隔离模式下等价于每文档释放 ✓ |
| R5 旧页面长定时器 | 实测刷新后**不再触发**（引擎按文档丢弃定时器） | 符合浏览器语义，无需处理 |
| R6 `g_moduleCache`（内存 body + 磁盘缓存） | 仍按 URL 全局共享，只有"在途"部分按代取消 | 磁盘缓存是内容寻址，与 realm 无关，可保持全局 ✓ |
| R7 入口模块管线合一（方案 A） | 已落地并验证：file:// 直读 + http 大图 offline 重放 + reload×2 三轮（realm 隔离下逐轮 modeval ok）均通过 | 已知限制：①动态插入的入口和 async 入口仍走 CachedScript（需要等待位之外的语义，风险优先）；②入口 script 元素的 load/error 事件暂不派发（避免在帧循环上下文跑页面 JS）；③import.meta.url 对 file: 模块显示伪装 URL（http://file.ant/...）；④DCL 后才到达的 inline 模块排队后不再执行（回到改造前行为）；⑤frame 嵌套时等待位单槽，iframe 的模块入口回退 CachedScript |
| R8 生命周期日志已贯通（f028880） | `AGB_NET_LOG=1`（导航时序）+ `AGB_EVT_DEBUG=1`（realm/teardown）拼出完整时间线：start → willclose → teardown → realm 换代 → commit → domready → load；引擎 reload 路径经 netlog 验证 teardown 完整（3 组导航全带 willclose） | 待查：`--reload 2 + --eval` 组合下 AGB_EVT_DEBUG 只见 1 组 teardown（netlog 无 eval 时 3 组齐全）——疑似探针 mark-eval 的队列泵重入与 teardown 竞争（agave_webview_eval 可重入是已知坑），探针侧嫌疑大，未定位 |
| R9 刷新后 `Identifier 'makeMap' has already been declared`（2026-11-03 已修） | 根因：模块 body 被写两遍。磁盘缓存命中时 `moduleFetch` 把整份 body `assign` 进 `entry->body` 并置 `done`，但同一 URL 的**已在途 prefetch 请求**继续 `onData::append` 同一份文件 → body 变成 2 倍（实测 vue.esm-browser.js 568612 → 1137224 字节）。首个文档不受影响（模块注册表按 URL 只求值一次），下一个文档（刷新）拿到翻倍源码 → 顶层声明重复 → 语法错误 | 修法（`AntRuntime.cpp`）：①`moduleStart` 先查磁盘缓存（prefetch 也走缓存，不再产生"body 已完整却仍在下载"的状态）；②`ModuleFetchClient::onData` 在 `entry->done` 之后丢弃字节；③`ModuleEntry::serial` 身份校验（retry 会 erase+重建 entry，旧 client 不得写入新 entry）；④缓存命中时 `moduleCancelRequest()` 取消在途请求并归还槽位；⑤入口脚本遇到"上一代 realm 取消导致的 failed"时重试而非永久失败。验证：`--reload 2` 三轮 app.js 均 6767 字节（此前第 2 轮起 1137224 → 声明冲突），冷缓存 + file:// 模块页回归通过。剩余 `TypeError: undefined is not a function` 属 R1（引擎 Link），与刷新无关 |
| R13 hover 时内容随机消失/被纯色遮挡（2026-11-03 已修） | 现象：鼠标移到链接/结果标题上（`:hover` 变色）时该行内容"变没"或出现纯色块。两个根因都在 rect 计算：**A. inline flow 的重绘矩形是 0×0** —— `RenderFlow::absoluteClippedOverflowRect()` 在 `isInlineFlow()` 分支里用 `width()/height()` 作矩形尺寸，但移植版本里**没有任何地方对 RenderFlow 调 `setWidth()/setHeight()`**（`RenderFlow.cpp`/`RenderInline.cpp` 全文无此调用），m_width/m_height 恒为 0 → 重绘脏区退化成一个点 → hover 变色不触发重绘，旧文字像素留着；一旦背景重绘擦掉该区域，文字就再也不画回来（"被纯色遮挡"）。**B. `clip()` 是替换语义而非求交** —— `GraphicsContext::clip(const IntRect&)` 用 `ps_scissor_rect`，picasso 的实现是 `ctx->state->clip.type = clip_device; clip.rect = tr;`（整块替换，`_clip_path` 的交集被丢弃），而 WebKit 的 `clip(rect)` 语义是与当前裁剪求交：父级 overflow 裁剪会被子元素自己的 clip 覆盖 → 子元素画到自己盒子之外（背景横向"溢出到同一行"）。修法：A 改成取所有 line box 的并集（`left/top/right/bottom` 由 `curr->xPos()/yPos()/width()/height()` 累出）；B 改用 `ps_clip_rect`（走 `_clip_path` 的 `clip_intersect`，且 `ps_save/ps_restore` 本来就会恢复）—— **⚠ B 项的修法本身是错的，已由 R14 推翻**（`ps_clip_rect` 收的是世界坐标，这里传的是设备坐标 → rect 被变换两次 → 滚动时裁剪区整体平移，屏幕下半段不重绘）。验证：`debug/csscheck.html` 全绿（除已知未实现的 grid/transform/sticky/aspect/gap），用户实测 hover 不再丢内容；辅助页 `debug/hovercheck.html`（inline `:hover` 变色）、`debug/clipcheck.html`（三层嵌套 `overflow:hidden`）、`debug/hoverprobe.ps1`/`mousescan.ps1`（真实鼠标 + PrintWindow 像素对比） |
| R14 滚动时整屏"隔行遮掩"/下半屏不重绘（2026-11-04 已修） | 现象：文档滚动后，屏幕上有一段区域保留旧像素（像拉了一半的窗帘），拖滚动条来回可以开合。根因在 R13 的 B 项：`GraphicsContext::clip()` 先用 `ps_world_to_viewport` 把 rect 转成**设备坐标**，再交给 `ps_clip_rect()` —— 但 `ps_clip_rect()` 的参数是**世界坐标**（picasso 内部建 path 后再乘 `world_matrix`），于是 rect 被变换了**两次**。滚动时 world_matrix 里带 `-contentsY` 的平移，裁剪区整体被平移同样的量 → 视口下缘（≈滚动距离）那一段落进裁剪区外，完全不重绘，旧像素留在屏幕上（`ps_scissor_rect` 才是设备坐标语义，所以 R13 之前"滚动是对的、只是 clip 不求交"）。附带问题：`ps_clip_rect` 的求交发生在**世界坐标**里，两次 clip 之间 world_matrix 一变（滚动路径正是如此），求交的坐标系就错位了 | 修法（`GraphicsContextPS.cpp`）：clip 状态改为自维护**设备坐标**矩形（`GraphicsContextPlatformPrivate::clip` + `clipStack`，随 `savePlatformState/restorePlatformState` 压栈）——`clip()` 里把用户坐标 rect 转设备坐标，与当前 clip **求交**后用 `ps_scissor_rect` 下发。既保住"clip 与已有裁剪求交"的语义（子元素不再画到父级 overflow 盒子外，R13 的 B 项意图），又不会二次变换、也不受 world_matrix 变化影响。验证：新增 `debug/scrollcheck.html`（120 行交替底色）+ `examples/view_host --wheel/--wheelup`（真实滚轮事件驱动文档滚动，与拖滚动条同一条路径）；滚动 200px 后逐行像素比对：修复前下半屏仍是旧内容（ROW 20..29 重复出现），修复后整屏连续（ROW 10..39）；`debug/clipcheck.html` 三层嵌套 overflow 裁剪仍正确；`debug/csscheck.html` 无回归。**附带发现（未修）**：滚轮方向是反的 —— `WebView::mouseWheel()` 把 `MOUSE_WHEEL_UP` 映射成 delta=-10，`ScrollView::wheelEvent()` 又按 `-deltaY*LINE_STEP` 滚动，滚轮下滚反而向上滚；测试宿主里 `--wheel`/`--wheelup` 已注明该反转 |
| R15 新标签页打开后是空白 + 一批基础交互缺口（2026-11-04 已修一批） | 现象：点 `target=_blank` 的链接，新标签页建出来了但永远是 about:blank。**根因**：`cb_open_window` 必须同步返回一个活的 `_mc_view*` —— 引擎的 new-window 路径（`FrameLoaderClientMC::dispatchCreatePage` → 先要 "about:blank"，再把真实 URL 载进它拿到的 frame）需要这个 view 才能继续加载；而控件的引擎 view 是排队命令（AGW_CMD_RESIZE）才建的，`agwOnOpenWindow` 返回时还是 NULL → 引擎放弃加载 → 空白标签页。**同批修掉的**：①新标签页继承了来源标签页的 `user` 指针（title/url/progress 全写进旧标签页）→ 新增 `agave_webview_set_user()`；②`window.open('rel.html')` 不按文档基址解析（变成 `http://rel.html/`）→ ant 桥接里用 `KURL(base, url)` 解析；③DOM 桥接缺 `HTMLElement.click()`（页面无法用脚本触发链接）→ 补上；④ctrl/cmd/中键点链接不开新标签页 → `HTMLAnchorElement::defaultEventHandler` 里把 target 改成 `_blank`；⑤shell 缺 Ctrl+W（关标签页）/Ctrl+R（刷新）；⑥**复制粘贴整条链是断的**：引擎有 `Editor → Pasteboard` 路径，但没有任何宿主实现 `cb_clipboard_set_data/data_len/get_data` → 控件里用 Win32 剪贴板（UTF-8↔UTF-16）补齐；⑦**关掉正在加载的标签页会崩**：`agave_webview_destroy()` 在宿主线程直接 `macross_view_destroy()`，而引擎线程可能正在 `macross_view_update()` 里用它 → 改成"引用还在飞行时由引擎线程在 agwRelease 里销毁"；⑧**GET 表单提交丢字符**：`encodeCString()` 用 `DeprecatedCString(const char*, unsigned)`，这个构造函数的第二个参数是**含 NUL 的缓冲区大小**不是长度 → 名字的最后一位和值的最后一位都被吃掉（`?q=hello` 变成 `?=hell`）→ 传 `length()+1` | 验证（探针，新增 `debug/newtab.html`/`newtab-target.html`，探针新增 `on_new_window` 回调并打印新窗口最终 URL）：target=_blank → 新窗口 `file:///.../newtab-target.html title=NEW TAB TARGET`；`window.open('newtab-target.html')` → 同上（相对路径已解析）；同标签页链接 → 主窗口 URL 切换；表单 GET → `?q=hello`；`debug/csscheck.html` 仍全绿。**仍缺（未做）**：`window.close()` 在 prelude 里是空实现（页面关不掉自己的标签页）；`beforeunload` 确认框、打印（`ChromeClientMC` 里是 notImplemented，导航不会被拦）；拖放（引擎有 `updateDragAndDrop`，控件没转发拖放事件）；中键粘贴；Ctrl+Tab 切标签页、Alt+←/→ 前进后退快捷键；滚轮方向是反的（见 R14） |
| R12 百度结果页 100% CPU + 断言退出（2026-11-03 已修） | 现象：`https://www.baidu.com/s?wd=baidu` 前 28 秒占满一核，内存稳定；修好 CPU 后立刻暴露 `ASSERTION FAILED: !view() \|\| !view()->layoutState() \|\| isCompact() \|\| isRunIn()`（`RenderFlow::absoluteClippedOverflowRect`）→ 进程 abort。定位手段：内置采样器 `AGB_PROFILE=1`（采样 JS/引擎线程 RIP，退出时 dump `[profjs]`）+ `Get-Process ... Threads \| TotalProcessorTime` 找热点线程 + `cdb -p <pid> -c "~~[0xTID]s; kb 90; qd"` 抓该线程栈。**根因 1**：`rewriteCustomPropertyNames`（变量支持里新加）用 `out += String(c)` 逐字符拼 `String` → O(n²)，百度几 MB 的 CSS 变成几十秒 memcpy（82% 样本落在 `StringImpl::append → memcpyFast`）。改为 `std::vector<UChar>` 预分配 + 一次构造。**根因 2**：`HTMLElement::isContentEditable()/isContentRichlyEditable()/contentEditable()` 里调 `document()->updateRendering()`，而 `isContentEditable()` 被 `RenderBlock::hasLineIfEmpty()` **在布局中**调用 → 布局期 `recalcStyle` → detach renderer → `removeChildNode` → `repaint` → 求 overflow rect 触发断言。三处 `updateRendering()` 全部移除（读状态不得启动样式重算） | 验证：CPU 28s/28s → **2.4s/34s**；页面正常（title=`baidu_百度搜索`、`#content_left` 11 个子节点、readyState=complete）；`debug/csscheck.html` 全项与 browserscore `--reload 1` 两轮 `app.js ok` 无断言 |
| R11 CSS 特性缺口补第一批（2026-11-03） | 按 `debug/csscheck.html`（computed style + 真实几何判定）实测：块/inline-block/float/绝对定位/margin auto/padding/border-radius/opacity/字号均 ok；缺口为 **`var()`（整条声明被丢）、flex（`display:flex` 降级成 block）、grid、transform（含 -webkit-）、gradient（三种写法都 none）、box-shadow（仅 -webkit- 可用）、`getComputedStyle(el,'::before')`**。底子是 WebKit 2008 的 `-webkit-box` 体系（`RenderFlexibleBox` 1056 行、`display:-webkit-box` + `-webkit-box-pack:justify` 实测布局正确），默认值 orient=HORIZONTAL/pack=BSTART/align=BSTRETCH 与现代 flex 默认一致 | 本轮补：①`CSSGrammar`(y 与 buildKJS 生成的 .cpp 同步) 别名表追加 `box-shadow/flex/flex-grow/flex-direction/flex-wrap/justify-content/align-items/order` → `-webkit-*`，且别名查找不再限首字母 `b`；②`CSSParser::modernKeywordID()` 把 flex/grid 关键字按属性上下文翻译（`display:flex→-webkit-box`、`row→horizontal`、`flex-start→start`、`space-between→justify`、`wrap→multiple`、`display:grid→block` 降级），不动关键字表以免破坏 `white-space:nowrap`；③`CSS.supports` 从 prelude 移到 polyfills.js 并严格化（读回值必须等于请求值，flex 走 -webkit-box 白名单，grid 显式黑名单），否则 grid 虚报为支持。实测：`flex-space-between`/`box-shadow`/`supports-flex` 转 ok，`supports-grid` 正确变 false。**渐变其实早就能画**：`parseGradientFunction` → `setBackgroundGradient` → `RenderBox` 的 `fillRect(FloatRect, CanvasGradient*)` 全通，像素采样（PrintWindow + 逐点取色）实测 `(20,300)=251,0,3`→`(540,300)=128,0,126`→`(1040,300)=11,0,243` 线性渐变正确；看起来"没画"是两个假象：①探针隐藏窗口不 paint（`AGB_PROBE_SHOW=1` 也需普通 GUI 模式才有 WM_PAINT，隐藏窗口 PrintWindow 得到白屏）；②`getComputedStyle().background-image` 只读 `backgroundImage()` 不读 `backgroundGradient()` → 恒返回 `none`。本轮补：computed style 报告渐变 + stop 分布按 CSS Images 3 修正（首尾未知 stop 贴 0%/100%，此前是 1/(n+1)，即 `linear-gradient(red,blue)` 渲染成 33%/67% 的偏色）。`getComputedStyle(el,'::before')` 也已通（ant 后端的 `getComputedStyle` 是 prelude 里的 JS shim，只吃一个参数、pseudo 被丢掉；现在 shim 透传第三参给 `__agbComputedStyle`，桥里用 `renderer()->getPseudoStyle(BEFORE)` 取伪元素 RenderStyle，并补了 `content` 的 computed 序列化）。**`var()` 也已支持**（同批）：词法上 `--x` 不是合法 ident（`ident = -?{nmstart}{nmchar}*`），所以在 `parseSheet`/`parseDeclaration`/`parseValue` 入口把 `--x` 重写成 `-agb-x`（跳过注释与字符串），property 规则用该前缀返回伪 id `CSS_PROP_CUSTOM_PROPERTY`；`--x: value` 存进进程级文本表，含 `var(` 的声明序列化回文本 → `expandVariables()` 展开（支持 fallback）→ `parseExpandedValue()` 重新解析。**已知局限**：①展开发生在**解析期**，变量表按 sheet 清空、跨 sheet 失效；②**同名取首次定义**（后定义的局部覆盖被忽略）；③无元素级作用域/继承覆盖；④无 fallback 且未定义的引用会丢弃整条声明（符合规范）。实测 `var-root/var-scoped-first-wins/var-fallback/var-width/var-border` 全 ok、`CSS.supports(color,var(--x))` 为 true（polyfills 里 var() 直接判真，语义上是对的）。**剩余未做：grid 布局、transform 绘制** |
| R10 **R1 真因：polyfills.js 是进程级 once，不是 per-realm**（2026-11-03 已修） | `loadPolyfills()` 用 `static int loaded` 只跑一次，而 realm 隔离后每个文档一个新 VM。首轮 VM 拿到 polyfills（DOM `append/prepend/replaceWith/remove/matches/closest/dataset`、`Symbol.hasInstance` 的 Node 判定、`style.sheet`、`matchMedia`、`CSS.supports`、**`URLSearchParams` 包装类**），刷新后的新 VM 是"裸"环境 → ①页面 `class URLParams extends URLSearchParams` 丢失子类方法 → `urlParams.toJSON is not a function`（用户复现：`TypeError: undefined is not a function at data (app.js:50:59)`，app.js:50 正是 `urlParams.toJSON({...})`）；②首轮"通过"、刷新必挂，正是"每进程第一次正常"的形状。**原 R1 归因（ant ESM Link/Evaluate）被推翻**：引擎 registry 与 Link 均无问题 | 修法：`RuntimeState.polyfillsGeneration` 记录已注入的 realm，`loadPolyfills()` 每个 realm 跑一次（同一 realm 内仍只一次）。验证：`--reload 2` 三轮 `app.js 6767 bytes ok`（修复前每轮 FAILED）；Vue 三轮完整渲染；`agave://page.js network error` 的未捕获 promise 一并消失（同源于缺失的 shim）；`debug/life.html` 与 `file://` 模块图刷新回归通过 |
| R1 定位详情（--offline 实验，2026-11-03） | 暖缓存 + `--offline` 确定性重放：214/214 模块全部磁盘缓存零网络供给，Vue 自身求值成功（打出 dev build 横幅），`app.js` 求值 100% 失败 "undefined is not a function"（1047ms，4/4 稳定）。**M2 网络时序交错被排除**；基线"通过"疑似旧架构共享 VM 的假象（上次运行的模块实例残留使 import 非 undefined）。症状与 ant 自家 tech-debt 逐字吻合（`H:\ant-v16\docs\exec-plans\tech-debt.md` §ESM loader linking：re-export 复制值不转发 live binding、`export default` 非 binding、循环图 → "undefined is not a function"，backlog 2026-10-22）。引擎侧 registry 确认 per-engine（`js->modules.state`，`loader.c:1657`），M1 假设不成立 | 修复：ant 侧按 spec 修 Link/Evaluate（上游 backlog 有完整验收标准），或 agave 侧"图就绪硬前置再统一求值"绕过（与 P0-b 合并做）。下一步：先做 P1-b 错误冒泡拿到失败 import 链 |

---

## 6. 待办清单（含验收标准）

### P0-a 模块图求值失败（对应 M2 / R1，诊断已由 --offline 实验收敛）
- 动作：~~查 ant 侧 registry 是否进程级~~（已排除：引擎侧 per-engine，见 R1 详情）。改为按 spec 修 ant 的 Link/Evaluate，或做"图就绪硬前置"绕过；先做 P1-b 拿失败 import 链。
- 验收：隔离模式下 `https://browserscore.dev/` 的模块求值不再失败；`debug/mod/` 与远程大图均通过。

### P0-b 图就绪与执行解耦（对应 M2）
- 动作：`flushDeferredModules()` 遇到未就绪的图时**不阻塞 engine 线程**——改为"图就绪回调触发执行"；或把等待移出 engine 线程。
- 验收：154 模块图的 load 事件不再被推迟数十秒；解析不被网络阻塞。

### P0-c 修 DOMContentLoaded 双发（对应 M3）
- 动作：二选一——WebCore 侧那次不再进 ant 监听表，或 ant 侧那次去掉；目标是页面只看到一次 DCL，且顺序为 **模块执行 → DCL**。
- 验收：life 页日志里 DCL 只出现一次，且出现在模块求值之后。

### P1-a 模块调度收敛 per-realm（对应 M4）
- 动作：`g_moduleSession` / `g_modulePending` / `g_deferredModules` 从进程全局改为 per-RuntimeState；磁盘缓存保持全局。
- 验收：两个标签页同时加载模块图时互不抢占槽位；A 页的模块不会被 B 页执行。

### P1-b 模块错误冒泡（对应 M5）
- 动作：嵌套模块的异常要冒泡到最外层，并带"哪个 URL 的哪一层"。
- 验收：故意让嵌套模块抛错，日志能直接指出失败的模块 URL。

### P1-c 清 `g_documentParsed` 死标志（对应附带项）
- 动作：要么真正维护它（文档解析完成时置位），要么删掉该分支。
- 验收：模块执行时机规则单一来源。

### P2 prefetch 换真解析（对应 M6）
- 动作：下载时用引擎自己的 import 解析取代字符串扫描。
- 验收：预下载的图完整，求值阶段零同步补下载。

### P2 桥接可观测性
- 动作：给桥接加计数输出（wrapper 数 / class 数 / root 余量），便于回归时盯表耗尽。

---

## 7. 快速上手路径（建议顺序）

1. 读 §2.4（M1~M6）建立问题模型。
2. 复现 R1：`--probe https://browserscore.dev/ --eval "probeExit('kids='+document.body.children.length)"`，看 stderr 的 `module evaluation failed`。
3. 先做 P0-c（最低成本、立刻能验证时序口径），再做 P0-a（决定隔离模型成不成立）。
4. P0-a 有结论后再做 P0-b / P1-a。

---

## 8. 关键代码位置索引

| 主题 | 位置 |
|---|---|
| generation 体系 | `AntRuntime.cpp:91`（字段）、`:102`（`nextGeneration`）、`:117`（`currentGeneration`） |
| realm 生命周期 | `AntRuntime.cpp:3399`（`stopRealm`）、`:3456`（`beginNewRealm`）、`:6364`（`documentWillUnload`） |
| 在途资源取消 | `AntRuntime.cpp:3386`（`cancelInFlightResources`）、`:497`（XHR）、`:1829`（模块下载）、`:3254`（延迟模块） |
| DOM pin | `AntRuntime.cpp:141`（`releasePinnedNodes`）、`:6217`（`wrapNode`） |
| 文档 slot | `AntRuntime.cpp:246`（`slotForDocument`） |
| 模块调度 | `AntRuntime.cpp:1725-1728`（全局状态）、`:2232`（`moduleStart`）、`moduleFetch` wait loop、`:3215`/`:3240`（deferred） |
| 桥接释放 | `AntBridge.c`：`antb_wrap_native_owner` / `antb_release_owner` / `agbReleaseEngineWrappers` / `agbReleaseEngineClasses` / `antb_engine_destroy` |
| 页面时序挂点 | `FrameLoaderClientMC.cpp`：`dispatchWillClose` / `committedLoad` / `dispatchDidFinishDocumentLoad` / `dispatchDidFinishLoad` |
| WebCore 侧限制 | `src/webcore/loader/FrameLoader.cpp:625-639`（unload 门禁）、`src/webcore/dom/EventTargetNode.cpp:325`（事件转发边界） |
