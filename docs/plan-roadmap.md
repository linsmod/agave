# agave 计划：待办与验收标准

> 状态：**向前看，最后核对 2026-11-06**。完成项保留验收记录，未完成项按 P0/P1/P2 排。
> 当前能力见 [arch-status.md](arch-status.md)；历史根因见 [hist-lessons.md](hist-lessons.md)；
> 架构约束见 [arch-overview.md](arch-overview.md)。

## 已完成（保留验收记录）

- **P0-a per-frame `window`**（R31）：`tools/pages/cap_window_identity.html` 全绿；方案与限制见
  [design-frame-window.md](design-frame-window.md)。
- **P0-d `contentWindow` / `contentDocument`**（R34）：`cap_dom.html` → `iframeCw=yes`；
  子文档未建好时为 `null`（`about:blank` 例外，R49）。
- **下载 P0 + P1**：引擎/API/控制层/探针 + `agave://downloads` 面板；见 [arch-downloads.md](arch-downloads.md)。
- **长循环探针 `0xC0000005`**（R61）：不是 JIT —— 引擎线程还在跑 `AGW_CMD_EVAL` 时宿主销毁了视图
  （UAF）。修法在控件层（命令持引用、销毁推迟到引擎线程）与探针层（`--settle` 从 eval 返回计时）；
  判据 `tools/run_jit_loop_tests.ps1`。ant 侧交叉验证：`tools/ant_loop_repro.c` 在 `f68ba03c`/HEAD
  上同一脚本均 exit 0。
- **v.qq.com 载入期崩溃**（R62 + R63，2026-11-05）：两条独立根因——图像解码器在 libpng/GIF/JPEG
  回调里销毁自己（UAF → 堆破坏），以及 callback builtin 的回调没被 GC 扎根（`AGB_GC_OFF=1` 可判）。
  工具：`AGB_GUARD_MALLOC`（WTF 页守卫分配器）、`tools/cdb_heap_probe.ps1`；判据
  `ANT_GC_STRESS=1` + `tests/array_callback_gc.cjs`。
- **P0-i 每 frame 一个 realm**（S0–S2 + S5）：一侧 VM、每 frame 文档一个独立全局（S1 默认开启）、
  wrapper 按 realm（S2）、iframe 自己的 DCL 且 DCL 只发一次（顺带修 P0-c）、prelude/polyfills 每
  realm 一次且 eval 漏斗兜底装宿主 API。设计见 [design-per-frame-realm.md](design-per-frame-realm.md)，
  验收 `tools/run_realm_tests.ps1`；未完成项（模块 map 按 realm、WindowProxy 转发）仍列在 P0-i。
- **resize 收尾：vw/vh × zoom + surface 生命周期**（R70）：探针新增 `--zoom`，`Document::viewportCustomProperties()`
  用 `adjustForAbsoluteZoom()` 折算 CSS px（zoom 变化靠 `m_viewportZoom` 使缓存失效）；控件侧 surface 重建
  进 `agwLock`，`AGW_CMD_RESIZE` 只记尺寸、`agwApplyPendingResizes()` 每轮 drain 后应用一次（拖动按批合并）。
  验收：`--zoom 125` 下 `innerWidth == 100vw`（880/880）、`--zoom 125 --resize 500x400` → 400×320；
  `run_layout_tests.ps1`（含 resize 组）+ nav/typing/worker/realm/site smoke 全绿。

## P0：先修正确性

### P0-b 图就绪与执行解耦

- 现状：`moduleFetch()` 在 engine 线程上 `pump()+Sleep(0)` 同步等模块 body（5s/模块上限），
  `flushDeferredModules()` 在解析结束立刻执行，图没下完就补下载。
- 动作：改成"图就绪回调触发执行"，或把等待移出 engine 线程。
- 验收：154 模块图的 `load` 事件不再被推迟几十秒；解析不被网络阻塞。

### P0-c DOMContentLoaded 双发（**已完成**）

- 现状：`Document::finishedParsing()` 派发一次（进 ant 监听表），`FrameLoaderClientMC::dispatchDidFinishDocumentLoad()`
  又 `AntRuntime::dispatchWindowEvent("DOMContentLoaded")` 一次，且 `flushDeferredModules()` 夹在两者之间
  → 页面看到 **DCL → 模块执行 → DCL**，标准是 **模块执行 → DCL**。
- **修法（per-frame realm 期间顺手修掉）**：`Document::finishedParsing()` 里先
  `AntRuntime::bindDocument(this)` + `flushDeferredModules(this)`，再派发那一个 WebCore DCL 事件；
  删掉 `FrameLoaderClientMC` 那次手写派发。iframe 因此天然获得自己的 DCL（以前直接 return）。
- 验收：`run_realm_tests.ps1` → `childDcl=1`、`parentDcl=1`；nav/typing/worker/layout/jit/script 全绿。

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
  被请求。**曾经的阻塞已清（R62 + R63）**：v.qq 载入约 10 s 的 `0xC0000005` 是两个独立缺陷——
  ①图像解码器在自己的 libpng/libjpeg/GIF 回调里 `delete m_reader` 并清空输入缓冲（use-after-free
  → 堆破坏，早先 `PNGImageDecoder::frameBufferAtIndex` / `WinHttpSession::run` 的栈都只是发现点）；
  ②ant 的 callback builtin（`Array.map` 等）没把回调 GC 扎根，JIT 传参走 alloca 缓冲、不在根集合里，
  回调里一次 GC 就把函数收了。修法与判据见 [hist-lessons.md](hist-lessons.md) R62/R63。
  v.qq 现在 `--wait 12000` 得到 `rs=complete len=173733 nav=57`，连续 30 s 不再崩（此前 8–10 s 必崩）。
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

### P1-g `obj[key]` / `G[idx]` 访问快路径（R60 的性能前置）

- 现状：QIMEI 的字节码 VM 每步 `G[T[++P]]` 都走通用属性路径，实测约 6M 次/秒；Chrome 里百毫秒级的
  计算在 ant 里要分钟级 → GUI 表现为假死（R60）。
- 2026-11-06 复测（`AGB_HANG_WATCH=1` + `--probe https://v.qq.com/`，90 s 超时，日志 `debug/hang_probe_vqq.log`）：
  仍在挂，15 次 `[hang]` 快照。持续的那段是 `vendor-common-D7eCwrIl.js:1:49505` 的 `j`
  （源码就是 `for(;;) switch(T[++P]) { case …: G[T[++P]] = G[T[++P]] … }` 的字节码分发循环，
  每步都走 `G[idx]`）；另外两段在 Vue 挂载（`vendor-vue…js:21` 的 `Wo/Oc/i`，栈里能看到
  `mount → setup → index-*.js`）和 superplayer。→ 本项验收（v.qq 不再报 5 s `[hang]`）**仍未达成**。
- 2026-11-06 定位到断崖（`tools/bench_vm.js`，引擎直跑）：稠密数组读/写 **2.5/4.0 ns**，
  稀疏下标（元素落到 shape 的 interned 字符串键属性）**141.5/206.5 ns**，字符串键 115–196 ns，
  `switch` 分发 104 ns/轮。→ 主修方向是**整数下标 → slot 的 shape 级表 + 站点 IC**，
  方案、判据、风险见 [design-elem-fastpath.md](design-elem-fastpath.md)。
- **2026-11-06 第二轮拆解（`tools/bench_vm.js`，订正上一轮的误判）**：`switch` 本身没有 104 ns ——
  10/40 连续 case 是 13.5/25 ns、40 个稀疏 case 33.5 ns、同义手写 if 链 12.5 ns；之前那个 112 ns/轮
  主要花在数组读 + `++` + 循环上。字符串键是更大的杠杆：**单键（IC 不抖）就 92.5 ns**，poly8 133 ns，
  动态拼接键 197 ns —— 说明瓶颈是 `jit_helper_get_elem` 里的 root scope + 通用 walk，不是 IC 抖动。
  **字符串键已完成**（§9.1，2026-11-06）：`jit_helper_get_elem` 在 root scope 之前解析扁平字符串键 ——`string-key-read` 115 → **22 ns**、单键 92.5 → **16.5**、poly8 133 → 24.5。规划（[design-elem-fastpath.md](design-elem-fastpath.md) §9）：字符串键把“不可分配”的快路径提到
  root scope 之前（目标 ≤25 ns）+ 站点 IC 扩到 4 项（poly8 ≤40 ns）；switch 先做字面量 case 的
  判别式复用 + 二分（O(log n)），再对整数稠密 case 生成 `OP_SWITCH` 跳表（MIR 已支持
  `MIR_SWITCH`，`src/jit/compile.c:203`），目标 sparse40 ≤12 ns。⚠ 第一次尝试（中途 `add_hidden_local` 存判别式）**已回退**：该 VM 的局部槽与求值栈共用帧布局，函数中途新增局部会踩栈，语义立刻坏（见 design §9.2.1）；switch 优化必须走函数级预留槽或 `OP_SWITCH`。验收：两项都做完再按 P1-g 的
  `[hang]` 判据复测 v.qq/163，改动前先留基线。
- 2026-11-06 代价记录：中断轮询在建引擎时即 arm，JIT 每个循环回边/调用多一次 load+branch；`switch-dispatch-loop` 从 104 → 116 ns/轮、稀疏读 41 → 37.5（噪声内）。P1-g 的验收阈值仍按原样，但评测时要记得这部分固定开销。
- 2026-11-06 现场抓取（`tools/js_stack.ps1 -Pid <pid>`，不 attach）：卡死时 JS 栈是
  **9 层嵌套的 `j`**（`vendor-common…js:1` 的 50257/50432/52943/53032/53844/55082 循环出现），
  即字节码 VM 在 JS 层自我递归（`m(...)` 每调一次就压一层解释器帧）——除了 `G[idx]` 的属性路径，
  每次 VM 内调用还带一整套帧建立/参数搬运，量级问题被放大。
- **2026-11-06 第一轮已落地（ant 仓库）**：数组元素读/写的稀疏快路径 + 索引键 memo（`js_index_get_fast`、
  `arr_get` / `setprop_index_impl` 的快路径、`idx_hash`）。`tools/bench_vm.js`：稀疏读 141.5→**41 ns**、
  写 206.5→**28 ns**（稠密 2.5/4.0 ns 无回归）。测试 `tests/test_index_elem.c`（meson `index-elem`）+
  `run_script_mode_tests.ps1` 全绿。**站点级验收仍未过**：v.qq 在 `AGB_HANG_WATCH=1` 下仍报 14 次 `[hang]`
  （QIMEI `getWebInstance → new j`）。剩余：字符串键 115–196 ns、`switch` 分发 104 ns/轮、
  数字键站点 IC（见 [design-elem-fastpath.md](design-elem-fastpath.md) §4.2/§4.3、§7）。
  顺带发现：数组越界读不查原型链索引键（`Array.prototype[7]=777; [][7]` → `undefined`），已记录在 §8。
- **已完成**：`obj[key]` 动态字符串键的属性 IC（R65，script 模式下 66→28ns / 63→25ns / 75→52ns）；
  长循环/销毁竞态（R61）不再干扰基准。
- **已完成（R66 + R67，默认开启）**：script 模式的顶层 `var`/`function` 发布到 global
  （编译器登记 + `sv_global_declare`），探针基准 20–60×
  （`empty 210→3ns`、`static-key 618→5.5`、`dyn-key 515→27`），v.qq `nav=57`（eval 模式 22）。
  翻默认前暴露的 chat.deepseek.com AV 已定位并修掉：`antb_retain()` 对函数值调用的
  `gc_pin_permanent()` 只接受对象类型，函数被静默丢弃 pin → 监听器被回收 → 下次派发读到已释放的
  closure（R67）。现在 `AGB_PAGE_EVAL` 默认 `script`，`=eval` 保留旧语义做 A/B。
- 验收：`tools/run_script_mode_tests.ps1`（函数 pin 存活、`window.pageVar=number`、eval 回退、
  `bench_hot` ns 上限）；`run_jit_loop_tests.ps1`、nav/typing/worker/layout 全绿；
  v.qq.com 在 `AGB_HANG_WATCH=1` 下不再报 5 s `[hang]`。

### P0-h v.qq.com 载入 AV：superplayer 混淆循环（R68，**未修**）

- 现象：`--probe https://v.qq.com/` 约 8–9 s `exit -1073741819`（3/5–5/5，`--eval` 无关）；
  `AGB_GC_OFF=1` 3/3 不崩（对象被提前回收的判据）；`AGB_PAGE_EVAL=eval` 同样崩
  → **与本轮 script 语义/R67 无关**，把 R67 的 pin 修回退后仍 2/3 崩（原有缺陷，页面当天版本把它暴露出来）。
- 取证（见 dev-debugging「只在非调试器下复现的 AV」）：VEH + `agb_crash.txt` + ant JS 栈 → 崩在
  `superplayer.js:10:606395`，即那段字符串数组轮转/反混淆 IIFE
  （`!function(e,t){for(var n=TE,i=CE();;)try{ if(766013==…) break; t.push(t.shift()) }catch(e){} }()`）；
  该循环里 `UL()` 会做 `e[r] = decode(...)`（`r = 数字 + 字符串` 拼接键）。两条原生栈：
  ①`hash_key ← intern_find ← js_setprop_keyed ← jit_put_elem ← jit_helper_put_elem_strict`
  （键 8 字符，`extra_*` 无异常）；②直接崩在 JIT 代码里，读的地址在字符串数据附近。
- 已排除：非字符串键（`AGB_PUT_TRACE` 实测 0 次）、类型混淆、`AGB_GUARD_MALLOC` WTF 侧（guard 下同样崩）。
- 下一步：在 `str_materialize`/`coerce_to_str`（拼接产生 rope，`vstr` 现场 materialize，**分配可触发 GC**）
  与 `js_setprop_keyed` 之间加"键内容在 setprop 前后一致"的断言，定位是 rope 物化后内容被回收、
  还是 key 指针在 `intern_string` 前就被前一次 GC 失效；同时确认 `e[r]=o` 里 `e` 是 Array 还是
  原始值包装（两条分支一个走 `js_setprop_array_fast`，一个走 primitive 原型 setter）。
- 验收：`--probe https://v.qq.com/` 连续 6 次 exit 0 且 `rs=complete`；`AGB_GUARD_MALLOC=512-4096` 下 2/2 干净。

### P0-i 每 frame 一个 realm（规范对齐，设计见 [design-per-frame-realm.md](design-per-frame-realm.md)）

- 现状：**已完成**。默认是"一个 engine + 每 frame 文档一个 realm"：iframe 有独立全局与
  intrinsics（`top.Array !== Array`）、独立 `var` 作用域、独立 DOM wrapper，iframe 有自己的
  DCL；旧模型（共享 realm + per-frame Window 代理）保留为 `AGB_PER_FRAME_REALM=0` 回退。
- 关键前提（已验证）：ant 的 realm 是**同一 VM 内的独立全局**，`ant_realm_create/enter` 可用，
  Node 的 `vm.createContext` 就是这套；跨 realm 对象引用不需要序列化。
- 阶段：S0 桥接 realm API（**已完成**）→ S1 每 document 一个 realm（**已完成并默认开启**，
  `AGB_PER_FRAME_REALM=0` 回退旧模型）→ S2 wrapper 按 realm（**已完成**，ant `4494171c`）→
  S3 事件/定时器/模块按 realm（**部分完成**：iframe 的 DCL 与单次 DCL 已修，见 P0-c；
  ES module map 仍按引擎）→ S4 WindowProxy 转发（待做）→ S5 回归与文档（**已完成**：
  `tools/run_realm_tests.ps1` 10 项并入常规矩阵）。
- 验收：新增 `cap_realm_identity.html`（iframe `var` 不可见、`top.Array !== Array`、
  `parent !== self`、`parent.fn()` 可用）；`cap_dom`/`cap_window_identity` 不退化；
  nav/typing/worker/layout/jit/script-mode 全绿。

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

### P2 不推进（已评估，保留理由）

- **HTML 属性事件 handler（`<body onresize="…">`、`onclick="…"`）**：会触发、能读写 DOM，但跑在遗留
  KJS 世界（`Document::createHTMLEventListener()` → `KJSProxy::createHTMLEventHandler()` →
  `JSLazyEventListener`），看不到页面 JS 的全局（handler 里 `window.__x = 1`，页面读不到）。
  现代站点用 `addEventListener`，`resize` 监听已可用（R69），**不再推进**。
  **若将来要修**：`createHTMLEventListener()` 改走 AntRuntime —— 把属性代码编译成
  `function(event){…}`（`antb_eval`），`this` 对 window handler 是全局、对元素 handler 是元素，
  `return false` → `preventDefault()` + `stopPropagation()`，并用 `bindDocument()` 保证在正确的 realm 里跑。
- **onresize 的验收方式**：不再单独为属性写法做回归；`tools/pages/resize/` 里 `<body onresize>` 只当
  "属性路径仍然存在"的观察项（用 DOM 属性记录，因为它的全局与页面不通）。

### P0-j `obj[dynamicKey] = v` 崩在 `hash_key`（163 adtracker，**已修 2026-11-06**）

- 现象：163 `adtracker2022.be06f843.js`（`requireTdidSass2_2_7`）在 `obj[key] = v` 时 first-chance AV；
  `agb_crash.txt` + `tools/cdb_stack.ps1 -Symbolize` 得到：
  `hash_key(hash.h:126) ← intern_find ← setprop_impl/lkp ← js_setprop_keyed ← jit_put_elem ← jit_helper_put_elem_strict`，
  读地址是垃圾（如 `0x1a933d30000`），即**键的字节指针已失效**。
- 已修（ant `e86c54b2`）：`jit_put_elem` 原来在 root scope **之前**做 `sv_key_to_propstr()`（原始 key 未根），
  且**非索引数字键**分支（小数/负数/>2^32）把转换结果直接传进去、完全没 pin。现在先根 obj/key/val，
  两个分支都 pin 转换后的 key。效果：163 从 2/2 崩（`-1073741819`）降到 0/2，同模块的 `TypeError: oom` 也消失。
- **残留根因（已修，ant 后续提交）**：`sv_key_to_propstr()` 转换失败时返回的是**错误值**（大键导致 `intern_string`/`js_mkstr` oom），`jit_put_elem` 仍把它当键字符串交给 `js_setprop_keyed` → `vstr()` 对错误对象取长度/字节 → `hash_key` 读到垃圾（AV 地址落在页边界）。修法：转换后 `is_err(key_jv)` 直接返回该错误；另外把转换结果复制成扁平字符串再存。验证：163 `--probe` 3/3 无崩溃、`TypeError: oom` 归零，`cdb_stack.ps1 -Url` 无 first-chance AV；ant 新增 `tests/test_put_elem_gc.c`（meson `put-elem-gc`，`ANT_GC_STRESS=1`）。\n- **排查记录**：已确认
  `gc_push_root` 存 slot 地址、`gc_visit_roots` 会访问所有值，所以不是“字符串没进根表”。
  当前最可能的假设：`sv_key_to_propstr()` 产出的字节来自 **rope/临时扁平化 buffer**，不是稳定字符串，
  所以 pin 住“值”也保不住“字节”。
- 下一步（做法已定）：
  1. `jit_put_elem` 里先 `intern_string(js, bytes, len)` 把键拷进**永久 intern chunk**（只有 `js_destroy` 释放，
     生命周期已核实：`intern_table_free` 唯一调用点在 `js_destroy`）；
  2. 用永久指针走 `setprop_cstr(js, obj, interned, len, val)`，不再把临时 JS 字符串交给 `js_setprop_keyed`；
  3. 回归：`ANT_GC_STRESS=1` 下循环 `obj[动态键]=v`（含 `obj[i/2]`、负数键）新增 meson 用例；
     163 用 `tools/cdb_stack.ps1 -Url https://www.163.com/` 跑 3 次确认 `hash_key` 不再出现；
  4. 顺带：`agwPost` 收口（`02db00b`）缺自动化用例；GUI “卡死页重新导航”手感验证未做。

### P0-k 卡死的页面可恢复（中断 + 重载/导航，**已完成 2026-11-06**）

- 问题：页面脚本不返回时引擎线程被占，刷新/导航/关标签页都只是排队，窗口“活着但没反应”。
- 已做：①ant 侧 `antb_interrupt_all_engines()`（`sv_jit_arm_interrupts` + 原子置位，任意线程可调），
  顶层 eval 入口清 `interrupt_pending`/`terminating`，避免误杀下一段脚本；②控件侧 `agwPost()` 收口：
  NAVIGATE/LOAD_HTML/BACK/FORWARD/RELOAD/STOP/EVAL 在引擎忙 >300 ms 时先中断再入队；
  busy 是**进程级**（一条引擎线程服务所有 view）；`agave_webview_interrupt/busy` 暴露给宿主；
  ③shell 刷新（按钮/F5/Ctrl+R/右键菜单）与地址栏导航都走这条；④探针 `--interrupt-after <ms>` / `--nav-after <ms>` 可直接回归。
- 验收（`tools/run_hang_recovery_tests.ps1`，**五条全绿**）：`debug/hang_loop.html`（20 s 死循环）
  + `--interrupt-after 2000` → **5.6 s** 结束（此前要等 20 s 循环自己跑完，`[hang]` 之外毫无办法）；
  + `--nav-after 2000` → 导航在中途生效、**3.1 s** 结束（此前 21 s）；空闲页 `--interrupt-after 300`
  不影响文档、不写崩溃文件；④慢页面（1.5 s 解析）连刷 3 次：**0 次 abort**、内容 `done`（修前会白页）；
  ⑤卡死页刷到同一 URL：脚本被中断、新文档 `title === ready`（修前刷新后仍白，因为中断请求没被消费）。
  GUI 刷新/导航的手感验证由人工完成。
- 已知限制：中断后页面状态不可信，必须配重载/导航；`<script>` 里的死循环能中断，
  但引擎线程若卡在 **native 代码**（非 JS 循环）则没有 safepoint，仍需杀进程。
- **2026-11-06 修正（白页回归）**：第一版用“引擎线程心跳”判卡死，但心跳在**加载中**和**空闲**时都会过期，
  于是每次刷新都被当成卡死 → 中断正在解析的脚本 → 文档半途而废 → **连刷几次就白页**。
  现在判据改成**队列是否排空**（命令在队头停留 >1.5 s 才算卡死，执行中的命令已出队所以长 eval 不受影响），
  由控件里的看门狗线程（200 ms）驱动；另外 `js_interrupt_raise()` 改为**消费一次**（清 `interrupt_pending`），
  否则中断标志会一直挂着，把**刷新后的新文档**脚本也一起杀掉 —— 这正是“卡死页怎么刷都白”的根因。
- 两个实现要点（踩过才知道）：①**中断轮询必须在建引擎时就 arm**（`sv_jit_arm_interrupts`），
  因为 `interrupts_armed` 是在 **JIT 编译期**决定要不要插 safepoint 的；中断时才 arm，已经编译好的
  热循环没有轮询点，请求只能等脚本自己返回。②忙判定不能只看“命令在跑”：导航命令几毫秒就返回，
  页面是卡在**解析/定时器回调**里，所以用**引擎线程心跳**（`g_enginePumpTick`，控件消息泵每 ~10 ms 更新）
  —— 超过 500 ms 没泵消息即视为卡死，与命令无关。
