# agave 计划：待办与验收标准

> 状态：**向前看，最后核对 2026-11-05**。完成项保留验收记录，未完成项按 P0/P1/P2 排。
> 当前能力见 [arch-status.md](arch-status.md)；历史根因见 [hist-lessons.md](hist-lessons.md)；
> 架构约束见 [arch-overview.md](arch-overview.md)。

## 已完成（保留验收记录）

- **P0-a per-frame `window`**（R31）：`tools/pages/cap_window_identity.html` 全绿；方案与限制见
  [design-frame-window.md](design-frame-window.md)。
- **P0-d `contentWindow` / `contentDocument`**（R34）：`cap_dom.html` → `iframeCw=yes`；
  子文档未建好时为 `null`（`about:blank` 例外，R49）。
- **下载 P0 + P1**：引擎/API/控制层/探针 + `agave://downloads` 面板；见 [arch-downloads.md](arch-downloads.md)。

## P0：先修正确性

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

### P0-e 站点对齐：v.qq.com 布局与 Chrome 一致（M1–M4，2026-11-05 起）

> 基线（1100×800，`debug/vqq_chrome.png` vs `debug/vqq_after.png`）：agave 只渲染出 SSR 骨架。
> 现代 bundle 的入口模块被按 classic 求值（`SyntaxError: export used outside module`），
> Vite 的 modern 探测又用到 `import.meta` → 回退 legacy → `System is not defined`，客户端 JS 完全不执行：
> `.top-nav-wrap` innerHTML 7 B（Chrome 90 KB）、`document.scrollWidth` 2658（Chrome 1082）、
> `.left-nav-wrap` 高 1444（Chrome 704=100vh）、`.focus-img` 左边 542（Chrome 0）、
> hero 蒙层 `#8A4726F2` 被解析成 `rgb(0,0,0)`（整块不透明棕黑）。

- **M1 模块求值语义（P0）**：入口/内联 `<script type="module">` 改走 `KJSProxy::evaluateModule()`
  （现在 `HTMLTokenizer::scriptExecution()` → `FrameLoader::executeScript()` → classic `evaluate()`），
  使顶层 `export`、`import.meta.url`、动态 `import()` 在模块目标下工作。
  验收：`debug/modern_gate.html` → `gate=1`；v.qq.com `window.__vite_is_modern_browser===true`、
  `[antjs]` 不再出现 export/import.meta 报错与 `System is not defined`、`.top-nav-wrap` innerHTML > 0。
  **状态（2026-11-05）**：模块语义、`nomodule`、桥的 module record 已修（R51）——modern bundle 已求值、
  legacy 请求消失、hydration 标记到达；**顶栏仍为空**：header 片段
  `https://v.qq.com/page-slice/dynamic-channel-header.html` 从未被请求（业务加载动作未触发），
  作为 M1 的未完成项单独追。
  **追查记录（2026-11-05）**：从 bundle 读到触发条件 —— store 动作先
  `document.getElementById("channel-top-nav")`，不存在就创建并挂到 `.top-nav-wrap`，
  然后**只有它的 innerHTML 是空串或 `"[object Object]"` 时**才 `An.get(...)` 拉片段
  （`A1()`/`H9` 的判断）。实测 agave：`.top-nav-wrap` 是 `<!---->`、`#channel-top-nav` 根本
  不存在；`#channel-app` 有 SSR 内容但**没有** `__vue_app__`/`data-v-app`/`_vnode`，
  而 `document.querySelector("#channel-app")` 被调用两次且都命中（`AGB_QS_DEBUG` 日志），
  `hydration:start/end` 相差 26ms —— 说明 `mount("#channel-app")` 返回了，但 Vue 的挂载标记
  一个都没落下，挂载在 Vue 内部被跳过或提前返回。下一步：在 Vue runtime 的 `mount`/`render`
  入口打日志（vendor-vue chunk 内），确认是 `normalizeContainer` 之后哪一步返回。
  **结论（R58）**：module 脚本在解析流程内执行，`mount("#channel-app")` 跑在元素解析出来之前，
  Vue 静默返回；改走 `queueDeferredModule`（parse 后 flush）后 `__vue_app__` 出现、header 片段
  被请求。**新的阻塞**：hydration mismatch 之后，绘制阶段 `PNGImageDecoder::frameBufferAtIndex()`
  的 `m_frameBufferCache.resize(1)` 里访问违例（疑似 decoder 生命周期/堆破坏），v.qq 探针当前
  在 `RenderImage::paint` 崩溃；下一步查 `ImageSource`/`BitmapImage` 的 decoder 释放时机。
- **M2 布局级 CSS（P1）**：①8 位 hex `#RRGGBBAA`（颜色解析）**已做（R51）**；②`transform` 的
  translate/scale/rotate（绘制 + 命中）**绘制与失效已完成（R52 绘制、R55 变换感知脏矩形），hit-test 与 stacking context 未做**；
  ③视口单位 `vw`/`vh`（本站 209×100vw、8×100vh）**已做（R53）**；④`calc()` 混合单位
  （`%`/`vw`/`vh`/`var`，本站 491 处）在布局期求值 —— **全部已做（R53 vw/vh/var/数字链，R54 百分比混合）**；
  ⑤横向溢出与 `overflow:hidden` 对 scrollWidth 的影响 —— **已修（R53：scrollWidth 2658→1100）**。
  验收（2026-11-05 实测）：`scrollWidth == innerWidth`（1100）、`.left-nav-wrap` 高 = 800（100vh）、
  卡宽 265（Chrome 259 @1082 视口）、卡片按行换行；hero 的变换感知重绘已修（R55，局部重绘不再裁掉
  translate 到布局盒外的部分）。
- **M3 现代 flexbox（P1）**：`flex-wrap:wrap`、`flex-basis`、`align-content`、`flex` 简写与
  legacy `-webkit-box` 的差异。**已完成（R53 基础断行，R54 有 flex 子项的三趟布局 + flex 简写 +
  flex-basis/align-content + scrollbar-width）**：`tools/layout_diff.py` 的 19 个 flex 用例对拍
  Chrome 全绿（固定宽换行、`flex:1 1 0` 三等分、`flex:0 0 200px`、align/justify/align-content）。
  验收：`.video-banner__container` 每行张数/换行位置一致；v.qq.com 卡片 265×246、12 张 4 行。
- **M4 视觉细节（P2）**：`object-fit`、`-webkit-line-clamp`、`filter`/`backdrop-filter`/`mask-image`、
  `@container`、字体回退。验收：首屏 ROI（hero/卡片行）像素差在阈值内。

测量方式：同一段 JS 在 Chrome（CDP）与 agave（`--eval-file`）各跑一次、固定 1100×800 且隐藏滚动条；
脚本 `tools/cdp_eval.py`、对比用 `debug/vqq_chrome_eval.js` / `debug/vqq_probe3.js`。

## P1：机制收敛

### P1-a 模块调度状态收敛 per-realm

- 现状：`g_moduleCache`（保留全局，内容寻址没问题）/ `g_moduleSession` / `g_modulePending` / `g_deferredModules`
  是文件级全局，两个标签页共用同一批下载槽与 pending 队列（现在靠 generation 打补丁）。
- 动作：session / pending / deferred 移到 `RuntimeState`。
- 验收：两个标签页同时加载模块图互不抢占；A 页排队的模块不会被 B 页执行。

### P1-c 清 `g_documentParsed` 死标志

- 现状：`AntRuntime.cpp` 声明并读取，从不置 true → "立即执行模块"分支是死代码。
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

> 注：其中多数已在 R34–R36 落地（真实集合、`form.submit/action`、`history.state`、`postMessage`、
> 属性选择器）。剩余项以 [arch-capability-gaps.md](arch-capability-gaps.md) 的实测值为准。

- 动作：①`documentGetter` 集合分支改为包装真实 `HTMLCollection`/NodeList；②类表补
  `HTMLFormElement` 的方法/属性、`HTMLImageElement` 的 `complete/naturalWidth/width`；
  ③History 建真实条目并派发 `popstate`；④storage 改成每次读取都走磁盘/共享表。
- 验收：`tools/pages/cap_dom.html` / `cap_history.html` / `cap_storage_a.html` / `cap_selectors.html` /
  `cap_postmessage.html` 的 `[exit]` 值与 [arch-capability-gaps.md](arch-capability-gaps.md) 的"已支持"一致。

## P2：增强

### P2 prefetch 换真解析

- 现状：`modulePrefetchImports()` 用 `body.find("from")` 字符串扫描，字符串字面量会误判，预下载的图不完整。
- 动作：用引擎自己的 import 解析（或至少跳过字符串/注释再扫）。
- 验收：预下载的图完整，求值阶段零同步补下载。

### P2 桥接可观测性

- 动作：给桥接加计数输出（wrapper 数 / class 槽位 / 每 engine 的 root 数），`AGB_EVT_DEBUG` 同一开关。
- 验收：表耗尽不再是静默失败（能提前在日志里看到余量）。

### P2 下载

- 异步写、并发上限、断点续传、另存为、`<a download>`、`agave://history`。
- 验收：见 [arch-downloads.md](arch-downloads.md) §11；P2 每项需要独立探针。

### P2 静默坑优先级（原 `todo.txt` 折叠）

1. `requestIdleCallback` 不回调 → 改成 `setTimeout(cb({didTimeout:false,timeRemaining:()=>50}), 1)`。
2. `CSS.escape` 不转义 → 实现 CSSOM 转义规则。
3. `MutationObserver` 永久静默 → 需要引擎 DOM mutation 钩子，成本高，先标记。
4. `window.open` 假窗口 + `postMessage` no-op → OAuth 弹窗类流程会挂。
5. `document.scripts` 空 → 脚本扫描型库会误判。
6. canvas `fillText`/`measureText` → 图表/文字库画不出字或排版错位。
7. `WebSocket`/`EventSource` 存在但不可用 → 要么接通，要么显式 `undefined`。
