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
- **单行 flex 按 `flex-basis` 分配**（R81，P0-e M3 的收尾，2026-11-06）：横向单行不再走 2008 的
  "普通块宽度 + box-flex"，与 `flex-wrap` 共用 `layoutFlexLines()`（basis/floor/ceiling → 按 grow 或
  `shrink×base` 分配 → clamp 冻结重分）。验收：新增 `tools/pages/layout/flex_single.html` 23 个 case
  对拍 Chrome 全绿（修前 14 FAIL），`flex_wrap.html` 21 个不退化，7 个 layout 差分页全绿；
  css3test `#content` 从"左窄右宽 360/452"变成"左宽右窄 541/271"（Chrome 546.6/219.8）。
  未完成：`align-self`、`order` 语义、RTL 单行、`flex-basis` 百分比/calc。
- **`position: sticky` + 程序化滚动**（R82，2026-11-06）：sticky 的声明以前被整条丢弃（关键字不在
  gperf 表里），偏移现在由 `RenderBox::stickyPositionOffset()` 现算并作用在
  `RenderLayer::convertToLayerCoords`（绘制/命中）与 `absoluteBoundingBoxRect`（几何/脏矩形）；
  文档滚动（`documentElement.scrollTop`、`window.scrollTo/scrollBy`、`pageYOffset/scrollY`）以前
  全是 no-op，现在路由到 `FrameView`，`getBoundingClientRect()` 也改成视口坐标。
  验收：`tools/run_sticky_tests.ps1`（11 条几何 + 4 像素）绿；8 个 layout 差分页、`LAYOUT=PASS`、
  site smoke、nav/typing 全绿。未完成：`scrollIntoView()` 仍不滚动；sticky 的 layer 没有 z-index 细化。
- **CSS Color 4 色彩函数语法**（R83，2026-11-06）：`hsl()`/`rgb()` 的空格形式、`/ alpha`、百分比分量、
  `deg` hue 全部接受（以前只认逗号，整条声明静默被丢）；hsl→rgb 与百分比分量改成四舍五入（对齐 Chrome），
  alpha 序列化按两位有效数字。验收：新增 `tools/pages/layout/color_syntax.html` 13 个 case 对拍 Chrome 全绿。
  未完成：`turn` 单位（没有 `CSS_TURN`）；`getComputedStyle` 读自定义属性仍返回内部名 `-agb-…`。
- **`background` 简写的 `/ <size>`**（R84，2026-11-06）：`url() 0 0 / 16px 16px`、`center / cover`、
  `contain no-repeat` 以前整条声明被丢（简写里没有 `-webkit-background-size`，`/` 没人消费）；`cover`/`contain`
  的 computed 也返回空串。现在简写带 size、像素与 Chrome 对齐、computed 输出 `cover`/`contain`/单个 `auto`。
  验收：`background_shorthand.html` 12 case 对拍 Chrome 全绿、`background_paint.html` 7 像素（接进
  `run_layout_tests.ps1`）、`LAYOUT=PASS`。
- **`var()` 展开的图片资源 + `data:` URI 图片**（R85，2026-11-06）：`background: url(x) var(--color)` 的
  computed 有 `background-image` 却不画（展开用的临时 `CSSParser` 没有 styleElement，相对 URL 变空，
  于是第二个永不加载的 `CachedImage`）；`<img src="data:…">` 与 `background: url(data:…)` 一律空白
  （`data:` 落到 WinHTTP）。现在 `CSSParser::setValueDocument()` + `document()->baseURL()` 兜底，
  `ResourceHandleManager::add()` 本地解码 data: 并走 `CachedDelivery`。验收：
  `background_paint.html` 的 `#varrel`/`#data`/`#dataimg` 4 个像素，`LAYOUT=PASS`。

- **自定义属性的读写与序列化**（R87，2026-11-06）：`getComputedStyle().getPropertyValue('--x')` 以前返回内部名
  `-agb-…`（值里的 `var(-agb-y)` 同样），`el.style.setProperty('--x', v)` 被静默丢弃。现在读/序列化还原作者拼写，
  name 版 `setProperty/getPropertyValue/removeProperty` 走 `setCustomProperty/customPropertyValue/removeCustomProperty`，
  `valueText()` 也不再把逗号写成 `" , "`（变量套变量因此能算出色值）。验收：
  `custom_props.html` 3 case 对拍 Chrome 全绿 + `check_custom_props.js`（`run_layout_tests.ps1` 的 `custom_props: ok`）。
- **`calc()` 混合字体单位**（R88，2026-11-06）：`calc(100% - 130px - 1em)` 以前整条声明被丢
  （`max-width: none` → 浮动元素换行，css3test 的 Carbon 广告块就是它）。现在 `CalcEvaluator` 接受字体上下文
  （em/rem/ex/ch），样式表解析时把这类 calc 十六进制编码成 ident 留成 raw value，渲染时用元素 font-size 折成
  `CSS_CALC_LENGTH`；顺带修掉数字扫描把 `1em` 的 `e` 当指数的问题。验收：
  `calc_terms.html`（7 case 对拍 Chrome）与 `float_carbon.html`（广告真实结构）全绿。
- **加载中刷新崩溃（两处）**（R89，2026-11-06）：GUI 里 loading 时点刷新必崩两次 ——
  ①`DocumentLoader::updateLoading()` 的 `ASSERT(this == activeDocumentLoader())`（断言在本 build 是
  写 `0xbbadbeef` 的崩溃），由被替换掉的旧文档的子资源完成回调触发；②`RenderBox::destroy()` 原地释放
  仍在父层兄弟链里的 `RenderLayer`，刷新打断加载后 `FrameView::layout()` 的 `updateLayerPositions()`
  走到已释放层。现在前者对非活动 loader 只 `setLoading(false)`，后者先 `removeChild()` 摘链再销毁。
  验收：`run_hang_recovery_tests.ps1` 七条全绿、deepseek/baidu 连刷 0 crash（`tools/slow_reload_server.py`
  是手工复现页）。
- **css3test 0% → 100%：`<style>` 收集与脚本看门狗**（R90，2026-11-06）：分数一直显示 0% 不是布局问题，
  是两处静默失败叠在一起 —— ①`Document::recalcStyleSelector()` 的遍历遇到 `<body>` 就停（2008 的 HTML4 规则），
  脚本 appendChild 到 `documentElement` 的 `<style>` 因此不进样式表，`<style>.sheet` 是 null，css3test 的
  harness 在第一个 `@rules` 测试抛 `TypeError`，`runTests()` 整个被掀翻；②修好①之后 2.6 s 的脚本又被
  `AgaveWebView` 的"卡住队列"看门狗在 1.5 s 打断（不可 catch）。现在遍历收整棵树，看门狗的判据见 R92。
  验收：`#hgroup` = "100% / 525 out of 525 / 122 features"（`#timeTaken≈2.9s`）；spin 4000 ms 不被杀、
  `AGB_STUCK_EXEC_MS=1000` 仍能打断死循环；`check_cssom.js` 的 `append-body`/`append-root`（`LAYOUT=PASS`）。

- **看门狗：真卡 vs 假卡**（R92，2026-11-07）：GUI 里 css3test 仍然 0%（探针 100%）—— 页面 `onload` 跑在
  `macross_event_dispatch()` 里，`g_engineBusy` 是 0，1.5 s 的"队首等待"把 GUI 载入期排着的
  resize/focus/按键命令当成卡死，2.9 s 的 onload 被中断。现在桥加了 `antb_engines_js_depth()`
  （`js->vm_exec_depth`，ant 判 job 边界用的同一个字段），看门狗按三档判：JS 在跑 + 队首是用户动作
  ≥1.5 s → 中断；JS ≥12 s 无人等 → 中断并弹提示；没有 JS 且 ≥10 s 不 drain → 只记录（中断没用）。
  附带：队首类型 `-1` 表示空（`AGW_CMD_NAVIGATE` 是 0）、`eval` 不算用户动作、判定写 `agb_watchdog.txt`、
  新增 `on_unresponsive` 回调 + GUI 提示（`AGB_HANG_POPUP=0` 关闭）。验收：`run_hang_recovery_tests.ps1`
  8 条全绿（含 `slow_timer.html`），导航打断卡死页 4.6 s，`AGB_UNRESPONSIVE_MS=2000` 可复现，css3test 仍 525/525。
- **CSSOM 补齐：`CSS.supports('selector()')`、`@page`、`content`/`quotes`/`blink`**（R91，2026-11-06）：
  `selector(::before)` 被当声明按第一个冒号切开、`@page {…}` 在 2008 语法里只剩 error 分支、`content` 的关键字
  分支是空 FIXME、`quotes` 字符串对没有分支、`text-decoration: blink` 只接受不记录 —— 28 个 feature 因此全挂。
  现在 `selector()` 走真正的选择器解析器（不匹配的 `@media` 里解析，读完摘掉临时 `<style>`，不留空表）；
  `@page` 在 yacc 前改写成 `agb-page-<n>` 占位规则、解析完原地换成 `CSSPageRule`（顺序、`type=6`、`style`、
  `selectorText` 都对）；`parseQuotes()` 新增，`content` 接受 `normal`/`none` 与四个 quote 关键字，`blink` 保留。
  验收：css3test `?filter=css2.2` 525/525、122 features（与 Chrome 一致）；`check_cssom.js` 新增 14 条断言。
- **媒体真实播放**（R99 + R100，2026-11-07）：`<video>`/`<audio>` 不再只是模拟时钟——
  ffmpeg 解 PCM 走 waveOut 出声、解 BGRA 帧按时间戳节流后由 `RenderMedia` 绘制；
  `currentTime` 由设备位置/墙钟驱动，`pause` 冻结、seek 重开解码器、`ended` 由媒体时钟触发。
  远程 http(s) 媒体由引擎自己下载成本地副本再交给工具（子进程在本进程树里做不了 TLS）。
  验收：`tools/run_media_real_tests.ps1`（本地视频 / 远程视频 / 本地 MP3 / 直接打开媒体 URL，
  含画面与 ffmpeg 参考帧对拍），实现边界见 [arch-media.md](arch-media.md)。

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

### P0-l 子 frame realm 没有定时器（2026-11-06，**已完成**）

- 现象：deepseek 登录页的微信二维码 iframe 里 `jquery.min.js` 抛
  `ReferenceError: 'setTimeout' is not defined`，随即 `jQuery is not defined`，二维码交互链整条断掉
  （`debug/ds_hangstack.log`、`debug/ds_profile_first.log`）。最小复现 `debug/iframe_timer_check.html` +
  `--eval` 读子 frame 全局：`parent=function childTimer=undefined childInterval=undefined
  childConsole=object childFetch=function childPromise=function`（`debug/iframe_timer_check2.log`）。
- 根因：`antb_engine_create()` 只调一次 `ant_bootstrap_modules()`，`init_timer_module()` 把
  `setTimeout/setInterval/clearTimeout/...` 装在**主 realm 的 global** 上；`ant_realm_create()` 走
  `js_realm_init_intrinsics()`（core + realm intrinsics + console），**不装 timer 模块**。
  `bindFrameRealm()` 只装 host API + prelude/polyfills，而 `prelude.js` 的定时器包装又是
  `if (typeof g.setTimeout === 'function')` 保护着的，子 realm 里静默跳过 → 子 frame 永远没有定时器。
  这是 P0-i S3「事件/定时器/模块按 realm」未做完的那一半。
- **修法（ant 仓库）**：`src/modules/timer.c` 把 `timers_define_common()` 抽成
  `ant_realm_install_timers(js)`（只往**当前** realm 的 global 装函数，不重置 isolate 级的
  `js->timers` / Timeout proto / template）；`bridge/AntBridge.c` 的 `antb_realm_create()` 先
  `ant_realm_enter()` 进新 realm 再调它，使 cfunc meta 绑定到该 realm（VM 在回调时按
  `func->realm` 自动切回）。**刻意不改 `ant_realm_create()`**：Node 的
  `vm.runInNewContext()` 必须保持 `typeof setTimeout === 'undefined'`
  （`tests/test_vm.cjs:97`；已用 `node:vm` 直接复测）。新增 `tests/test_realm_timers.c`
  （meson `realm-timers`）：新 realm 默认无定时器、装完有回调、主 realm 不受影响。
- **验收（2026-11-06）**：`debug/iframe_timer_after.log` →
  `childTimer=function childInterval=function`；`debug/ds_after_timers.log` 里两条
  `'setTimeout' is not defined` / `'jQuery' is not defined` 均消失、
  `[exit] rs=complete inp=2`；`tools/run_realm_tests.ps1` 扩到 14 项全绿
  （含 child timer 真的回调）；`tools/build_antjsb.ps1` 发布新 `antjsb.dll` 后重链
  `libagave`。

### P1-h deepseek 首屏 4.3 s 的构成（2026-11-06 测量，**待优化**）

- 测量：`AGB_NET_LOG=1 AGB_PERF_LOG=1 --probe https://chat.deepseek.com/`
  （`debug/ds_perf_probe.log`），`AGB_PROFILE=1`（`debug/ds_profile_first.log`），
  `AGB_HANG_WATCH=1 AGB_HANG_SECONDS=1`（`debug/ds_hangstack.log`）。多轮日志
  `debug/ds_*.log` 的时间戳高度一致，**不是网络抖动**。
- 时间线（`commit` → `load`）：
  | 段 | 墙钟 | 性质 |
  |---|---|---|
  | 主文档 `chat.deepseek.com/` | 0→330 ms | 网络：`ttfb=185ms dur=215ms`，之后 `net.deliver lag=28ms` |
  | `default-vendors.js`(1.0 MB)+`main.js`(1.6 MB)+`main.css`(0.3 MB) 命中 HTTP 缓存 | 341→350 ms | 缓存投递（timer 每 10 ms 一泵） |
  | **静默 350→930 ms** | ~580 ms | **CPU：编译/求值 vendor bundle**。`[perf] net.pump 1123ms` + `[perf] update paint=2ms js=1225ms` 落在同一帧；`[hang]` 抓到 JS 栈是 React 提交阶段 `default-vendors…js:2:778162 / 778025 / 773134`（`sF=2`、`switch(a.tag)` 的 fiber 遍历），不是网络等待 |
  | `katex.js` + `smcp.min.js` + `fp.min.js` + 4 个 `settings?scope=` | 930→1380 ms | 网络：`smcp` 299 ms、`fp.min.js` 391 ms、settings 163–253 ms；**但它们的 `net.deliver lag` 高达 120–200 ms** —— 完成事件排队等引擎线程空出来 |
  | **静默 1380→3047 ms** | ~1.6 s | **CPU/等待**：`[con-debug] Error:` 后到 `deviceprofile/v4` 之间没有网络日志；指纹 SDK 在等自己的定时器/回调，而引擎线程被上一段 JS 占满 |
  | `wxLogin.js`/`appleid.auth.js`/`qrconnect` iframe | 3047→3660 ms | 网络 + iframe 文档提交；`ERROR: called Frame::paint with nil renderer`（提交瞬间还没有 renderer） |
  | iframe 内 `weui.css`/`qrcode`/`jquery` + 二维码解码 | 3660→4290 ms | `img data` 从 3745 ms 拖到 4290 ms（~540 ms），`qrcode` 的 `net.deliver lag=323ms` |
  | `load` 事件（`listeners=310`，两条：iframe + 主文档） | 4290 ms | 首屏可交互 |
- 关键结论：**首屏慢的主因不是网速**，而是「大 bundle 的同步编译/求值」与「网络完成事件必须等引擎线程」两段
  互相放大：`ResourceHandleManager::downloadTimerCallback()` 在引擎线程上泵，`net.deliver lag` 就是
  「网络已好、引擎没空」的直接证据；`WebEngine::macross_view_update()` 把 paint + `pumpAsyncRequests()`
  + `antb_pump()` 串在同一帧，`js=1225ms` 里既跑页面脚本又投递全部 XHR/定时器回调。
- **2026-11-06 精确归因（新增 `AGB_PUMP_BREAKDOWN=1`，`debug/ds_pumpbd.log`、
  `debug/ds_budget_on.log`、`debug/ds_budget_off.log`）**：`pumpAsyncRequests()` 四段计时
  显示 `xhr=0ms module=0ms arrived=0ms vm=1203ms` —— 慢的 1.2 s **全部在 `antb_pump()`
  里的 React 调度器/提交代码**，XHR 回调、模块投递、网络完成事件本身都是 0–20 ms。
  `AGB_HANG_WATCH=1` 的 JS 栈（`sF=2`、`switch(a.tag)` fiber 遍历）与此一致。
- **①「网络完成事件与 JS 求值错帧」已实测否决，不推进**：两种实现都试过——
  (a) 每 tick 只投一个完成事件（含 `m_cached` 与 `WinHttpSession::pump()` 两条路径）、
  (b) 自适应时间预算（`AGB_NET_SLICE_MS`，默认 8 ms，只在某个投递真的超过预算时把尾部留到下帧）。
  单帧最长 `net.pump` 确实从 1099/1178 ms 降到 178/518/304 ms，但 `net.deliver lag`
  从 ~200 ms 恶化到 800–2400 ms（人为排队），首屏 `load` 4431–5001 ms vs 基线 3953–4681 ms
  **没有改善甚至变慢**。原因：瓶颈是单次 1.1 s 的同步 React commit，切片既切不开它，
  又让后续资源白等。代码已回退，保留 `AGB_PUMP_BREAKDOWN` 诊断。
- 优化方向（按性价比，需先有判据再动）：①`default-vendors.js` 的**执行期**优化
  （ant 的 JIT/对象属性 IC，见 P1-g；或让站点的 `requestIdleCallback`/调度器分片，属能力缺口）；
  ②**编译缓存已实测否决，不推进**：`tools/bench_compile_exec.cjs`（用法：`ant.exe tools/bench_compile_exec.cjs <bundle.js>`）对 1 MB vendor bundle
  测得 `compile≈110ms execute≈105ms`（裸 sandbox，3 轮稳定）；浏览器里 1.2 s 是 React 对
  真实 DOM 的逐节点桥接，不是编译。为省 ≤110 ms 新增桥 API + `sv_func_t` 生命周期管理
  不划算，结论写进 R76；
  ③`AGB_SKIPRES` 跳过首屏用不到的 `katex`/`web-tts`/`opus-decoder` chunk（4290 ms 之后
  6061/6087 ms 仍在下 TTS、6084 ms 还发了一条 `url=` 空请求）；④`load` 双发（iframe + 主文档
  各一条 `listeners=310`）要确认不是 P0-c 的残留。
- 验收：`--probe https://chat.deepseek.com/ --wait 5000` 的 `[evt] window event load` ≤ 2.5 s，
  且 `AGB_PERF_LOG=1` 下单帧 `js=` 不超过 300 ms、`net.deliver lag` 中位数 < 50 ms；
  `debug/ds_smoke.js` 的 `inp=2 all=…` 与 `rs=complete` 不退化。

## P1：机制收敛

### P1-dom-attr-accessors popup 探针用例（2026-11-10 已修 R182——**根因不是 eval 路由**）

- 现象：`run_nav_tests` 的 `window.open`、`run_window_tests` 的 local `target=_blank`/命名 target、
  `run_tab_lifecycle_tests` 的 popup 用例失败，probe sanity eval 拿不到 `probeExit`（exit 4）。
- 真实根因：接口 class 镜像有一半是**进程级 static**（`g_documentIface`/`g_elementIface`/`g_tagClasses`/
  selection/range/textTrack），第二个 engine（popup）建类时覆盖了它们，主 engine 之后包的每个节点都是
  跨 VM 原型的空壳（`document.documentElement.tagName === undefined`）。当时"eval 落到 popup 文档"的
  判断是错的：`document.URL`/`childNodes` 一直是主文档的，只有走 `classForNode()` 的节点坏了。
- 修法：镜像全部搬进 `RuntimeState`（per engine）、`applyRealmInterfacesFor(state, realm)` 显式写 state、
  `ensureInterfaceClasses` 用 `stateForEngine(engine)` 查表；顺带修掉 `AGB_PER_REALM_IFACES=0` 每次重建
  类表的问题。详细记录：`docs/hist-lessons.md` R182、`issues/wip/ant-dom-bindings.md` §22。
- 验收（已满足）：三个套件恢复全绿；全量 `run_all_tests.ps1` 20 套件 250.9 s 0 failed。

### G1 DOM 绑定收尾（**已完成 2026-11-11**；#15 的后续，四项全部落地）

> #15《ant runtime 是手写 DOM 门面》于 2026-11-11 **完结**：语义层（原型链/构造器/`instanceof`/归属按 Chrome/
> 属性值 173 全有值/事件层级+冒泡+`isTrusted`+`PointerEvent`/集合 live+索引 own key/`el.style` 真
> `CSSStyleDeclaration`）已对齐 Chrome；架构层完成了生成管线 + 派发表 + 归属规则。剩余工作拆成 G1：
> 计划与验收见 `issues/wip/dom-bindings-next.md`，条目见 `issues/INDEX.md` #17。

- G1-1 接口覆盖 18 → 41（**2026-11-11 完成**：CharacterData/Text/Comment/DocumentFragment、表格族、Style/Meta/Body/
  Heading/Quote/Pre/OList/UList/BR/HR、SVGElement/SVGSVGElement、Window；`AGB_BINDING_CHECK=1` = 41 interfaces /
  457 attrs / 134 methods / 0 missing owner。`HTMLDocument` 的表被移除——它的成员在 Chrome 里属于 Document）。
  统一走 `tools/gen_bindings.ps1 -Generator ANT` + Chrome 归属对拍。
- G1-2 `el.attributes` 变真 `NamedNodeMap`（**2026-11-11 完成**：`length`/`item`/`getNamedItem`/`setNamedItem`/`removeNamedItem`/
  `*NS`/索引 own key/live，元素是**真 `Attr`**；顺带修掉 ant 的 exotic-index 崩溃与 `PassRefPtr` 悬垂指针）。
- G1-3 `ParentNode`/`ChildNode` 七个方法从 `polyfills.js` 落成 C++（**2026-11-11 完成**：`polyfills.js` 869 → 815 行，
  归属按 Chrome——`Element`/`Document`/`DocumentFragment` 有 `append`/`prepend`/`replaceChildren`，
  `Element`/`CharacterData` 有 `before`/`after`/`remove`/`replaceWith`，`Node.prototype` 上一个都没有；
  `matches`/`closest` 暂留 JS）。
- G1-4 文档收口（**2026-11-11 完成**：`arch-capability-gaps` 增 `element.attributes`/`Attr` 行、`dev-tests` 断言数 194、
  本文件与 `issues/INDEX.md` #17、`wip/dom-bindings-next.md` 同步）。
- 验收（**全部达成 2026-11-11**）：`run_binding_semantics_tests` **194/194** 全绿；全量 `-Parallel 4`
  20 套件 **34.5 s 0 failed**；`AGB_BINDING_CHECK=1` = **43 interfaces / 0 missing owner(s)**。
- **不在 G1**：生成器产行为（A1 主体，2~3 周，另立项）；wrapper 身份 bug（用户指示跳过，见 `P1-wrap`）；
  261 个 IDL 全量迁移；live collection 的 WebCore 化。

### P1-bind 脚本动态插入 iframe 后脚本"停住"（2026-11-10 已修 R183）

- 现象（2026-11-10 晚修正）：脚本里 `appendChild(iframe)` 之后，**调用方脚本的剩余部分在子 frame 的 realm 里执行**
  （`window.__after` 丢失、`document.title` 落到子文档）；探针 eval 里表现为 `probeExit` 找不到 → exit 4。
  内联解析期、timer 回调、探针 eval 都触发；`AGB_PER_FRAME_REALM=0` 也触发；静态 `<iframe>` 不受影响。
  `AGB_BIND_TRACE=1` 打印 `bindDocument()` 的 target/prev/JS depth（本次为此加的诊断开关）。
- 影响：页面脚本动态插入 iframe 之后的逻辑可能整体不执行（页面可见）。
- 修法（已完成）：`bindDocument()` 显式 `antb_realm_enter(engine, runtime->realm)`；`Document::finishedParsing()`
  在子 frame 上保存并归还调用方文档（主框架不归还，否则撤销导航）。`AGB_BIND_TRACE=1` 是这次加的诊断开关。
  验收：`cap_binding_semantics.html` 的 "script-created iframe keeps the caller document"（169/169）。
- 详细记录：`docs/hist-lessons.md` R183、`issues/wip/ant-dom-bindings.md` §28.6–§28.7。

### P1-wrap `textContent` 写入破坏其它节点的 wrapper 身份（2026-11-10 发现，**待查**）

- 现象：`var a=document.createElement('a'); a.textContent='hello';` 之后，之前拿到的另一个 wrapper 与其原生节点不再相等：
  `inp.form === form` 从 true 变 false（`inp.form.tagName==='FORM'`、`inp.form===inp.parentNode` 仍成立 → 同一原生节点、
  不同 JS wrapper）。二分确认 `a` 还是游离节点时也会发生，与 `appendChild`/属性写入无关。
- 影响：`===` 身份比较（库的 memo/缓存、`el === parent.firstChild`）会偶发不成立；wrapper cache 的 identity 被破坏。
- 起点：`Node::setTextContent()` 路径上是否触发了 `antb_release_owner`/`Document::updateDocumentsRendering` 时的 wrapper 失效，
  以及 `wrapNode()` 的 `(ptr, tag, realm)` 缓存键是否被 realm/owner 变化打散。
- 已查明边界（2026-11-10 晚，`AGB_WRAP_TRACE=1`）：最小复现需要"页面含静态 iframe"+完整写入序列；无 iframe 的页面不复现。已排除 wrapper 释放、realm/owner 不同、form 归属解析错误、帧解析归还（试过无效已回退）。下一步：让 `wrapNode` 的缓存命中/未命中可见（桥侧），分清"命中失败"还是"返回另一对象"。
- 记录：`issues/wip/ant-dom-bindings.md` §28.8.1–§28.8.2。

### P1-z 图片解码离开 paint（L0–L3，2026-11-08 起；L0–L3 已完成，剩余见下）

- 现状：`RenderImage::paint → BitmapImage::draw → ImageSource::createFrameAtIndex →
  ImageDecoder::frameBufferAtIndex` 全在引擎线程上，且 JPEG `outputScanlines()` 一次解完整张图。
  实测 `debug/img_decode_block.html`（4000×3000 JPEG 显示成 400×300）：`[perf] paint=467ms
  rect=400x300`、页面 rAF `maxgap=100ms` —— 脏矩形很小，时间全在解码里；这段时间 JS/输入/滚动/
  下一帧全停（UI 线程只是继续 BitBlt 旧 mirror）。
- **已完成 L0+L1（R136）**：`[perf]` 带 `decode=`；JPEG 行循环/PNG 字节块按每帧预算切片
  （`AGB_IMG_DECODE_MS`，默认 12ms），部分帧照常绘制、靠重绘续解；缩放绘制只采已解出的行并按
  预算限制采样（`AGB_IMG_SAMPLE_MAX`）。实测单帧 paint 467ms → **20ms**、rAF gap 100ms →
  **38ms**、切片与一次性解码逐字节同像素；判据 `tools/run_image_tests.ps1`（IMAGE=PASS）。
- **已完成 L3（R137）**：按显示尺寸解码。`ImageSource::setScaleHint()` +
  libjpeg `scale_denom`（1/2、1/4、1/8）；自带 libjpeg 原本 `#undef IDCT_SCALING_SUPPORTED`
  且缺 `jidctred.c`，已补（`dist/include/jmorecfg.h`、`dist/src/jpeg/jidctred.c`）。12MP JPEG
  显示 400×300：decode **424ms → 35ms**，切片帧 35 → 5，`paint` 最大 15ms；判据
  `run_image_tests.ps1` 的 L3 组。重复背景（`drawPattern`）仍按 natural 解码，PNG/GIF 暂不缩放。
- **已完成 L2（R138）**：`ImageDecodeService` 工作线程（2 条 + 队列 + 每帧 `poll()`），
  引擎线程只排队/收帧；`ImageSource` 增加 adopt 模式，`Image` 不是 refcounted 所以用
  `cancel()` 而不是持引用。**paint 里 `decode=0`**，off-thread 帧与内联帧逐字节一致；
  canvas/打印走内联同步（无后续重绘），动画格式仍同步。踩坑：`Image::drawTiled` 提前调
  `nativeImageForCurrentFrame()`，内联解码会缓存半帧、把 adopt 帧挡住（背景图消失），
  已让 `nativeImageForCurrentFrame()` 也过异步门。判据 `run_image_tests.ps1` 的 L2 组 +
  8 图×3 reload 压测 + layout/sticky/typing/nav/fuzz。
- 剩余：GIF/APNG 逐帧同步解码；PNG 无缩放解码（只挪到 worker）；**canvas `drawImage(<img>)`
  画不出东西**（既有缺口，见 [arch-capability-gaps.md](arch-capability-gaps.md)）；
  `img.decode()` 仍是立即 resolve 的 stub（`CachedImage::imageFrameReady` 是可用的挂点）。
  拆文档/关 tab 时进行中的 job 已经处理（cancel + 结果丢弃 + 队列排空 + 记账，见 R139）：
  `macross_event_dispatch()` 也 poll，`live` 收尾归零；`dropped` 分支要 cache 淘汰才会走到。
- 验收：L2 后同页 `decode=0ms` 且反复导航/关窗 50 次无线程泄漏；L3 后大图 `decode` 降到 1/4 以下；
  `run_image_tests`、`run_layout_tests` 像素组、`fuzz_layer`、deepseek/v.qq smoke 不退化。

### P1-y SVG 渲染：复活 ksvg2（2026-11-07 起）

- 路线：启用仓库自带的 2008 版 ksvg2（`ENABLE_SVG`），不移植 Blink/miniblink 的 SVG，也不先用第三方
  光栅器顶 SVG-as-image。设计、缺口盘点与踩坑见 [design-ksvg2-revival.md](design-ksvg2-revival.md)。
- 已完成 A1/A2：`OPT_SVG`（默认 ON）→ `ENABLE_SVG`；`tools/gen_svg_names.ps1` 重生成真实的
  `SVGNames`/`XLinkNames`/`SVGElementFactory`（原为禁用分支的空桩）与带 SVG 属性的 CSS 表；
  补齐 `SVGResource`/`SVGPaintServer`(+solid/gradient/pattern)/`SVGResourceClipper|Masker|Marker`/
  `PathTraversalState`/`ImageBuffer` 等缺失实现。**SVG 文档**能渲染 rect/circle/path 的 fill/stroke：
  `debug/svg_shapes.svg` 探针像素验收 红 4800（80×60）/绿 2804（≈π·30²）/蓝 1200（40×30/2）/黑 420；
  HTML 回归 `imgbox_160` 深色占比 37.8%（R29 基线 37.7%）。
- 已完成 B1（2026-11-08）：**JS 创建的 inline SVG 能渲染**。DeepSeek 登录页 logo（React 用
  `createElementNS` 建的 `<svg>`，182×29 / 11 个 `<path>`）在 830、1100 宽下均能画出（探针蓝色像素
  13692）。根因：SVG 根在 flex/零高度图层里被父块的绘制遍历整段跳过，`RenderSVGRoot::requiresLayer()`
  给 SVG 根自己的图层后由 layer 树独立绘制；同时把 overflow 裁剪改成按**渲染盒**（布局 px，含页面缩放）
  而不是按 SVG 用户单位 `viewport()`。回归：`debug/svg_shapes.svg` 红 4800/绿 2804/蓝 1200/黑 420 不变，
  `debug/svg_inline_js.html` 红 3200，`imgbox_160` 深色 37.8%。
- 已完成 B1 补丁（2026-11-08）：`RenderSVGRoot::calcPrefWidths()`（SVG 根原先沿用 `RenderContainer`
  的空实现，min/max 首选宽恒 0，flex/shrink-to-fit 容器跟着塌成 0 宽 —— logo 比 Chrome 右移 91px）；
  `RenderSVGRoot::nodeAtPoint()` 在空白处也返回 `<svg>` 自身（inspector 能选中根）；`RenderPath::absoluteRects()`
  改用紧包围盒（原先借用重绘矩形，没 stroke 也膨胀 3–4px）。回归页
  `tools/pages/layout/svg_inline_box.html`（几何组：agave/Chrome 都是 `icon 209,0,182,28`）。
- 待办（按顺序）：
  1. 阶段 B2：HTMLParser 外来内容（HTML **静态标记**里的 `<svg>`，React 动态建树已可用）。
     验收：`debug/svg_inline.html` 渲染出红矩形（当前 0 像素，解析器把它当普通未知元素）。
  2. 阶段 C：`SVGImage`（`<img src=*.svg>`、CSS `url(*.svg)` 背景）。验收：`debug/svg_img.html`
     （当前 0 像素）。可复用新增的 picasso `ImageBuffer`。
  3. 页面缩放（≠100%）下 inline SVG 的偏移：125% 时 SVG 内容相对盒子整体偏移/被裁（盒子 rect 227×35，
     内容仍按 182×29 的用户单位变换）。需要把 zoom 统一到 SVG 的 viewport/CTM 计算里。
  4. ~~DeepSeek 登录页最后两个字形的 path 画在其 bbox 左侧约 16px~~ —— 修好 `RenderSVGRoot` 的盒位置后
     才看清是**所有**字形整体左移 `origin·(1−viewBoxScale)`：`applyContentTransforms()` 把 viewBox
     变换接在 origin 平移之后（picasso 在当前用户空间拼接），已改为 viewBox 先、origin 后；
     上色逐项比对 `p0..p9` 全部 ±1px。见 R119。
  3. 渐变真渲染：`SVGPaintServerGradient::draw` 接 `ps_gradient_create_linear/radial`（含
     objectBoundingBox 变换），替换现在的“首停靠色”降级。
  4. marker / clipPath / mask / pattern / filter：marker 不画、mask 不应用、clipPath 只逐子路径
     `clip()`、pattern 的 `setup()` 返回 false。
  5. SVG 文本渲染验收（`RenderSVGText` 路径，本次未覆盖）。
  6. CSS SVG 属性（`fill`/`stroke`/`stop-color`…）在 selector 的应用/继承验证（解析侧的属性表已重生成）。
  7. JS SVG DOM：`SVGAnimatedPoints::ref/deref` 目前是 no-op（JSSVGAnimatedPoints 用 `RefPtr` 但对象归元素所有），
     需在阶段 D 换成正确的绑定语义。
- 复测：`agave_shell.exe --probe file:///E:/mybrowsers/agave/debug/svg_shapes.svg --shot build-zig-native\svg.bmp`
  （`--shot` 路径要落在 build 目录；本代理环境给子进程的写权限只覆盖那里）。

### P1-a 模块调度状态收敛 per-view（**已完成 2026-11-09，S2**）

- 现状（修前）：`g_moduleCache`（保留全局，内容寻址没问题）/ `g_moduleSession` / `g_modulePending` /
  `g_deferredModules` 是文件级全局，两个标签页共用同一批下载槽与 pending 队列（现在靠 generation 打补丁）。
- 已做：session / pending / routed-entry 表 / arrived bodies / deferred 队列 / lastFlushDocument /
  in-flight 计数全部移进 `RuntimeState`；module body 缓存也按 view（共享 in-flight 表项会被先关的
  tab 取消掉另一个 tab 的等待）。文档级取消：`DocumentRuntime::generation` +
  `cancelDocumentResources()`（frame 离场只取消它自己的 XHR/module/deferred）。
- 验收：`run_xhr_tests.ps1` 新增 `xhr_frame_cancel.html`（frame 导航走后 XHR 不回调，修前 fired=1、
  修后 fired=0）；realm/hang/tab-lifecycle/worker/nav/script-mode/jit/typing/layout 全绿。
- **仍未做**：ES module registry（ant 的 `js->modules.state`）仍是 engine 级、不是 realm 级；
  iframe 里的 ESM 入口脚本/动态 import 目前跑不起来（见下面 P1-e），所以等 P1-e 一起做。
  WebCore 侧 `HTMLTokenizer.cpp` 的 `s_moduleEntryWaiter` 仍是文件级单 waiter（两个 tab 同时解析
  入口 module 会互相覆盖），也要在 P1-e 里按 document 分表；模块 body 缓存按 view 意味着
  父/子文档 import 同一 URL 时共用一个表项，子文档离场会取消它（等 P1-f 的 per-realm registry 消掉）。

### P1-e frame 里的 ESM：入口模块与动态 import（**已完成 2026-11-09**）

- 修前（2026-11-09 实测）：`<iframe>` 里的 `<script type="module" src=...>` / inline 模块不执行（只有
  "queued for parse end"，没有后续）；frame 里的 `import()` 报
  `ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING`；经典脚本里的 `import()` 在 timer 回调里报
  `Cannot resolve module`。
- 已修（三处）：
  1. **模块语义交给解析器**：删掉 `AntRuntime::looksLikeModule()` 的源码嗅探（它把以 `import(` 开头的
     经典脚本误判成模块）。`HTMLTokenizer::scriptExecution(isModule)` / `HTMLScriptElement::isModuleScript()`
     是唯一判定；`prefetchModuleGraph(url, source, isModule)` 也由解析器传标志，不再扫经典脚本。
  2. **frame 的模块队列会 flush**：`flushDeferredModules()` 过去要求 `document->body()`，而"整页只有
     head 级内容（只有 `<script type=module>`）"的真实 frame 文档在 finishParsing 时还没有 body，模块
     永远留在队列里。现在正文只在"没有 body **且** 队列为空"（= frame 初始空文档）时跳过。
  3. **动态 import 的基 URL**：ant 侧给每个 eval/经典脚本的 code unit 记下 `import_base_path`
     （`js_eval_bytecode_mode_result`），`sv_op_import` 在无 callback 时用包含调用的函数
     （`func->debug->filename`）作基 URL，经 `js_dynamic_import_from()` 走普通 ESM loader；file: 页面
     的基 URL 由 agave 的 `setFilename()` 转成 `http://file.ant/` 伪装空间（与模块入口一致）。
- 验收：两个 iframe 各自 `import('./shared_mod.js')` 各执行一次（`window.__sharedLoads` 各自 1）；
  frame 的 inline/`src=` 模块入口执行；以 `import(` 开头的经典脚本按经典脚本执行且动态 import 成功；
  `run_script_mode/realm/nav/worker/xhr/tab-lifecycle/typing/hang/layout` 全绿；ant 侧
  `tests/test_dynamic_import_script_base.js` + `test_vm_dynamic_import_missing.cjs` 通过。
- **仍未做**：WebCore 的 `s_moduleEntryWaiter`（HTMLTokenizer.cpp）仍是文件级单 waiter —— 两个 tab 同时
  解析入口 module 会互相覆盖；ES module registry 仍按 engine（P1-f）。

### P1-f ES module registry 按 realm（**已完成 2026-11-09**）

- 修前：ant 的 `js->modules` 在 `ant_t` 上、不在 `realm/fields.h` 的镜像表里，所以同一 engine 里的
  两个 frame realm 共享 module map（规范要求每个 realm 一份；两个 frame import 同一 URL 只有第一个
  真的求值，第二个拿到它的 namespace）。还有一个更隐蔽的初始化缺口：新 realm 的
  `builtins.vm_main_context_loader` 是 0，而经典脚本 unit 的 `import_handler` 也是 0，
  `0 == 0` 让 `import()` 走了 vm-context 分支（global `import`，主 realm 的 loader）。
- 已做：
  1. `realm/fields.h` 加 `ANT_REALM_FIELD(modules)`（ESM registry、require 缓存、evaluation stack 全部
     跟 realm 走）；`realm_mark_one()` 标记 `modules.cjs.*`、`hooks`、`import_meta`（非当前 realm 的
     值不在 isolate 根里）。
  2. `js_realm_init_intrinsics()` 给新 realm 的 module 字段设真正的空值（零在 NaN box 里是**数字**，
     不是 undefined）。
  3. `esm_state_free()` 拆出来：realm 销毁时释放自己的 `ant_esm_state`；
     `esm_loader_cache_cleanup()` 同步清当前 realm 的镜像（避免 js_destroy 与 realm_free 双释放）；
     新 realm 的 state 继承 engine 的 VFS bundle（`--bundle` 场景）。
  4. `js_vm_dynamic_import()` 的 sentinel 判断加 `handler &&`，未初始化 realm 不再误走 vm-context 分支。
  5. agave 侧 `s_moduleEntryWaiter`（HTMLTokenizer 的文件级单 waiter）改成 **per-tokenizer**：
     `fetchAndQueueModuleScript(url, owner)` 把 tokenizer 当 owner 传下去，body 到达时回到请求它的
     那个 parser（两个 tab 同时解析入口模块不再互相覆盖）。
- 验收：ant `tests/test_realm_modules.c`（meson `test-realm-modules`）：main 两次 import=1、新 realm
  0→1、切回 main 仍 1、realm 再入仍 1（缓存）、第二个 realm=1、50 个短命 realm 各求值一次；
  `tests/test_realm_*.cjs` 全过；`run_script_mode_tests.ps1` 的 `frame module entry + per-realm import()` 通过；
  agave 全部常规套件绿。


### P1-c 清 `g_documentParsed` 死标志（已完成）

- 现状：R108 的修法已经删掉这个从不置位的标志，"立即执行模块"走 `queueModule` 的两个分支判定。
- 验收：`grep g_documentParsed src/` 为空，模块执行时机只有队列/封口判定一个来源。✅ 2026-11-09

### P1-ab 全局对象的 exotic getter 与 inspector safe-eval（新发现，2026-11-09）

- 现状：R148 把窗口 getter 装到了每个 realm 的全局对象上；ant 的
  `src/inspector/safe_eval.c` 沿原型链找属性时"遇到带 exotic getter 的对象就放弃"，
  于是 inspector 求值 `window.hasOwnProperty` 这类**沿原型链**的读会失败（自己的属性不受影响）。
- 影响：只影响调试器/inspector 的表达式求值，页面 JS 不受影响（页面走完整 getter，未命中继续原型链）。
- 动作：safe_eval 对"窗口类型"的 exotic getter 放宽（读不到就走原型链），或让 inspector 直接走
  `js_getprop_fallback_len()`；验收：inspector 里 `window.hasOwnProperty`（原型链上的）可读，
  `window.length`（全局自己的属性）本来就正常。


### P1-aa 跨 realm 桥回调的 document 镜像（已完成 2026-11-09）

- 现状：`contextActivate()` 在每个桥方法回调前用**被调函数所在 realm**去 `applyRealmBinding()`，
  但 ant 的 C builtin（node class 的方法，engine 级创建）带的是**创建它的 realm**（通常主 realm）：
  子 frame 里执行 `parent.document.getElementById('fr')` 会把宿主 `g_currentDocument` 切到父文档，
  回调返回后 ant 把 realm 还原，宿主镜像却留在父文档，直到下一次回调才被纠正。
- 症状：`AGB_REALM_CHECK=1` 下探针报
  `realm-mismatch document.title self=<child> current=<parent>`；这段窗口内**无 receiver 的
  document 级宿主函数**（`setTimeout`/`fetch`/`new Image()`）会落到父文档（R128 那一类）。
- 修法：桥的方法 trampoline 成对调用 `contextActivate`/`contextLeave`（`antb_set_context_leave`，
  native 返回 0 的路径也调）；宿主在 activate 时把当时的 `g_currentDocument` 压进
  `RuntimeState::bindStack`，leave 时弹回并 `activateDocument()`（记录已被拆掉的文档不还原）。
  ant 侧判据 `tests/test_bridge_context_hooks.c`（meson `test-bridge-context-hooks`，enter/leave 必须
  成对、not-handled 也成对）；agave 侧 `run_realm_tests.ps1` 的 `childFramesByOwner` 重新启用，
  两种 realm 模式下 `AGB_REALM_CHECK=1` 均 0 条 mismatch。✅ 2026-11-09

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

### P2 媒体：渐进式流 + `object-fit` + 音画严格同步

现在的真实播放是"整段吃进本地副本再解"，三条明确的下一步（细节见
[arch-media.md](arch-media.md) §8）：

1. **渐进式**：http(s) 媒体改 Range 请求 + 边下边解，别等整段下载完；同步探测
   （`ResourceHandle::loadResourceSynchronously`）也要从引擎线程挪走，否则大文件会卡 UI。
2. **`object-fit` / `object-position`**：绘制时按 `cover`/`contain` 裁剪或留边，而不是拉伸。
3. **音画对齐**：视频帧按解码 PTS 而不是"帧序号 / 25"，并给音频漂移做补偿；`playbackRate`
   变速时同时改音频解码速率（`atempo`）或至少让视频跟随。

验收：`run_media_real_tests.ps1` 增加一条"边下边播"用例（首帧在下载完成前出现）、
一条 `object-fit: cover` 的 16:9→4:3 裁剪像素对拍、一条 60 s 播放后 |audio-video| < 50 ms。

### P2 prefetch 换真解析

- 现状：`modulePrefetchImports()` 用 `body.find("from")` 字符串扫描，字符串字面量会误判，预下载的图不完整。
- 动作：用引擎自己的 import 解析（或至少跳过字符串/注释再扫）。
- 验收：预下载的图完整，求值阶段零同步补下载。

### P2 桥接可观测性

- 动作：给桥接加计数输出（wrapper 数 / class 槽位 / 每 engine 的 root 数），`AGB_EVT_DEBUG` 同一开关。
- 验收：表耗尽不再是静默失败（能提前在日志里看到余量）。

### P2 shell 的 GUI-only 操作没有自动化判据

- 现状：tab 新建/关闭/切换、查找、缩放、书签、右键菜单只发生在 GUI 壳里，探针跑不到，
  回归脚本对它们零断言（日志已经补齐，见 [hist-lessons R75](hist-lessons.md)）。
- 2026-11-11 部分覆盖：chrome 的布局/命中/绘制/按钮回调由 `--ui-shot` +
  `tools/run_shell_ui_tests.ps1` 覆盖（34–61 项，无窗口/引擎/profile；`--ui-menu`
  还把自绘菜单的模型/布局/键盘/像素拉进离屏断言）；**窗口框架的 NC 点击**
  由 `tools/check_shell_frame.ps1`（`AGB_SHELL_HIDDEN=1`，不显示窗口；Chromium
  `CUSTOM_DRAWN` 同路径）验证，**菜单宿主**（分层弹窗/捕获/模态循环）由
  `tools/check_shell_menu.ps1` 验证。真实 GUI 的 tab/find 命令流仍未自动化。
- 已具备的零件：`tools/send_ui_events.ps1`（`-RightClick` 打右键；WM_COMMAND 可直接点按钮）、
  探针的 `on_context_menu`（只记录、不开菜单）。
- 动作：写 `tools/run_shell_tests.ps1`（**尚未实现**）—— 起 GUI（临时 `--userdata`）→ 用 `WM_COMMAND` 点
  `ID_NEWTAB`/`ID_FIND_*`/`ID_BACK`、用 `-RightClick` 打右键 → 断言 `[shell] tab create/activate/…`、
  `[shell] find …`、`[shell] context menu … choice=N` 出现，然后 `WM_CLOSE`。
- 约束：会短暂显示窗口，不能进默认回归；键盘快捷键**不可测**（`GetKeyState` 读真实键盘状态，
  PostMessage 的虚拟按键骗不过它），只能测按钮/菜单路径。
- 验收：脚本退出码 0/1，覆盖 tab 三连 + 查找 + 右键选择。

### P2 单实例壳的剩余项（2026-11-11 起步）

- 现状：一个 profile 一个浏览器进程、第二次启动的接力（`--new-tab`/`--new-window`/激活）与
  同进程多窗口已实现，见 [design-shell-singleton.md](design-shell-singleton.md)。
  回归 `tools/run_singleton_tests.ps1` 只测**逻辑**（probe + `AGB_HANDOFF_MOCK_DIR` 文件 mock，
  约 2 s）；真实接力/多窗口的 UI 路径仍是手动（`AGB_SHELL_HIDDEN=1`）。
  想把 UI 路径也自动化：把 `ShellWindow` 的窗口操作抽成可替换的接口（mock 窗口），
  再按同样的 probe 方式驱动。
- 未做：
  1. **session 只有扁平列表**：重启后所有 tab 合成一个窗口；先关掉的窗口的 tab 不保留。
     动作：`session.txt` 加窗口分隔行（`window` 头 + tab 行），恢复时按窗口重建，`saveSession` 保留
     关闭窗口的策略可选（Chrome 有 restore-on-startup 开关）。
  2. **`window.open` / `target=_blank` 仍是新 tab**，不是新窗口。动作：`onNewWindow` 调
     `createShellWindow()`（现成的），并更新 `run_window_tests` 的期望。
  3. **锁是 `Local\`（会话内）**：快速用户切换的两个会话可以同时跑同一 profile，仍会互相写
     cookies/localStorage。动作：profile 内 lockfile（Chrome 的做法）+ 启动时检查，或把写盘改成
     temp+rename 原子替换（对多开也安全）。
  4. **第二实例的开关不透传**：`--backend`/`--zoom`/`--offline` 被忽略（stderr 有提示）。动作：
     在请求里带 flags 位，接收方按需应用（或明确拒绝并说明）。
  5. **没有 takeover**：已有实例崩溃后新实例从零启动，不恢复它的窗口（session 会由崩溃实例的
     WM_DESTROY 之前的保存决定，可能落后）。
- 验收：①两个窗口重启后仍是两个窗口；②`window.open` 出窗口且 `run_window_tests` 绿；
  ③跨会话测试（两个登录会话、同一 profile）不产生半截文件。

### P2 view-source

- 现状：右键菜单这一项是灰的 —— 引擎没有"把当前文档序列化成字符串交给宿主"的 API，
  旧实现 `eval("document.documentElement.outerHTML…")` 的结果没有任何出口（`agave_webview_eval` 丢弃返回值）。
- 动作：`WebView` 增加 `documentHTML()`（`Document::toString()` 或 serializer），控制层新增
  `agave_webview_get_html()` → shell 用 `agave_webview_load_html()` 在新标签页打开（`<pre>` 转义）。
- 验收：右键 "View page source" → 新标签页显示源码，`document.URL` 仍是 `file:`/`http:` 原地址的派生标记。

### P2 ant 中断路径收口（**已完成 2026-11-09**）

- ~~`antb_interrupt_all_engines()` 里那句 `sv_jit_arm_interrupts(engine->js)` 是空转~~：已删除。arm
  只在 `antb_engine_create()`（引擎线程）做；`AntBridge.h` 注明"arm 只能在引擎线程，这里只翻标志"。
  这句如果哪天真的生效，会在**引擎线程之外** `jit_drop_all_code()` —— 释放对方正在执行的代码页。
- ~~`g_engines[]`/`g_engineCount` 无锁~~：已加一把 C11 `atomic_flag` 自旋锁；扫描（depth/interrupt）
  的**整个**字段访问都在锁内，`antb_engine_destroy()` 先在锁内摘表再拆（拆的时候读侧已经拿不到它）。
- ~~`Atomics.wait(sab, i, v)` 无 timeout 真·不可中断~~：无 timeout 分支改成 100 ms 切片
  `pthread_cond_timedwait` 轮询 `interrupt_pending`，中断延迟 ≤100 ms（`src/modules/atomics.c`）。
- worker VM 仍不在注册表里（worker 走自家 `terminate()`）—— **保留为已知限制**，写进了
  `AntBridge.h` 与 `antb_interrupt_all_engines()` 的注释；worker 不在引擎线程上跑，不挡页面命令。
- 验收：ant `tests/test_bridge_interrupt.c`（meson target `test-bridge-interrupt`）—— ①`Atomics.wait`
  无 timeout 从另一线程 `antb_interrupt_all_engines()` 后 ≤5 s 内以 termination 错误返回；②等待期间
  `antb_engines_js_depth()` ≥1；③一个线程反复 create/destroy、另一个线程同时扫描，不崩、结束后 depth=0。
  负向验证：去掉 atomics 修复后同一测试会挂到超时。agave 侧 hang 回归 9 条全绿。

### P2 下载

- 异步写、并发上限、断点续传、另存为、`<a download>`、`agave://history`。
- 验收：见 [arch-downloads.md](arch-downloads.md) §11；P2 每项需要独立探针。

### P1-ac eval 掉进遗留 KJS 世界（2026-11-09 已修，R163）

- 现象：加载失败的 view / `load_html()` 之后立刻 eval（新文档还没提交）会崩在
  `DOMWindow::document()` 的断言，随后写 `0xbbadbeef`；栈是 `KJS::IfNode::execute` →
  `JSDOMWindow::getValueProperty` → `DOMWindow::document`。
- 根因：`KJSProxy::evaluate()` 的 ant 分支后面那段遗留解释器**不在 `#else` 里**，守卫一假就换引擎；
  遗留世界用 `Window::retrieve()` 造出没有文档的 JSDOMWindow。
- 修法：`ENABLE(ANTJS)` 下守卫失败 = 什么都不跑（`m_lastEvaluateRan=false` → `on_command_dropped`），
  遗留解释器只留给 `#else` 的构建；`DOMWindow::document()` 返回 0 不再 ASSERT；属性 handler 在没有文档时
  不建延迟 listener。判据：`run_process_backend_tests.ps1` 的 "early eval after load_html" 用例——
  `dropped=1 kind=eval reason=no live document` 且无 ASSERT/崩溃。
- 遗留（不在本项范围）：属性 handler（`<body onload=…>`）仍跑在遗留 KJS 世界，见 `arch-capability-gaps.md`
  的"HTML 属性事件 handler"行，立场仍是"不推进"。

### P2 静默坑优先级（原 `todo.txt` 折叠）

1. `requestIdleCallback` 不回调 → 改成 `setTimeout(cb({didTimeout:false,timeRemaining:()=>50}), 1)`。
2. `CSS.escape` 不转义 → 实现 CSSOM 转义规则。
3. `MutationObserver` 永久静默 → 需要引擎 DOM mutation 钩子，成本高，先标记。
4. `window.open` 假窗口 + `postMessage` no-op → OAuth 弹窗类流程会挂。
5. `document.scripts` 空 → 脚本扫描型库会误判。
6. canvas `fillText`/`measureText` → 图表/文字库画不出字或排版错位。
7. `WebSocket`/`EventSource` 存在但不可用 → 要么接通，要么显式 `undefined`。
8. `el.scrollIntoView()` / `scrollIntoViewIfNeeded()` 不滚动 → `Element::scrollIntoView()` →
   `RenderLayer::scrollRectToVisible()` 那条链没落到 `FrameView::setContentsPos`；文档滚动本身已在
   R82 接通，这里只是入口不对（实测：调用前后 `getBoundingClientRect().top` 不变）。
9. 探针在大文档站点上偶发退出崩溃（css3test，改动前就有，**未修**）：payload 已打印、`[probe] main url=`
   之后 `[crash] code=0xc0000005`，栈是同一组帧的深递归（`libagave+0x2da47e`/`0x2ea62e`/`0x2dbac1`），
   同一次运行还可能打印 `LEAK: 4628 RenderObject` —— 像是 render 树 teardown 递归。判据：同一条命令重跑
   几次，成功与崩溃都可能出现；`#hgroup` 之类的 payload 不受影响。要修先拿 `tools/js_stack.ps1`/`cdb_stack.ps1`
   抓符号，别从 CSS 侧猜。
10. `navigator.sendBeacon` 不存在（R106 把页面生命周期事件补齐后暴露的下一个缺口）：`pagehide` 里发最后一份
   统计/埋点的页面会抛 `TypeError`，退而用 `fetch(keepalive)` 也保不住——`stopRealm()` 的
   `cancelInFlightResources()` 会按 realm 取消在飞请求。做法：桥加 `antb_beacon_post(url, body, type)`
   走 network manager 的**保活表**（不挂在 realm generation 上、不随 realm 拆除取消），prelude 里补
   `navigator.sendBeacon`（入队成功返回 true）。验收：`debug/lifecycle_probe.html` 的 `pagehide` 里
   `navigator.sendBeacon('/log', ...)` 在导航后仍到达本地 server；`run_nav_tests.ps1` 不退化。
11. 窗口最小化（`WM_SIZE` 的 `SIZE_MINIMIZED`）目前不触发 `visibilitychange`（切 tab 已接
   `agave_webview_set_visible()`）；Chrome 会发。做法：shell 的 `WM_SIZE` 里按最小化状态调
   `agave_webview_set_visible(g_tabs[g_activeTab].view, !minimized)`。

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
- **2026-11-06 第二轮（“连刷到白屏”，R73）**：又发现三处，已修：
  ①子资源被取消时被记成“该 URL 失败”并留在**进程级缓存**里（`Loader::didFail` 的 1 参版），
  下一个文档直接复用错误条目 → 站点自己的资源兜底页（deepseek 的 `_sriCk`）= 白屏；
  ②`FrameLoader::receivedData` 按 `activeDocumentLoader()` 而不是投递者路由，停掉的旧响应的在途字节
  会灌进新文档；③`reload` 不做文档级拆除（`stopLoading(false)`，不 unload、不摘监听器）；
  ④`committedLoad` 会接受陈旧 loader 的提交。
  验收：`tools/run_hang_recovery_tests.ps1` **六条全绿**（新增“HTTP 渐进响应连刷 8 次文档完好”），
  deepseek sign_in 1.2 s 间隔连刷 6 次 → exit 0、正文非空、无“页面资源加载异常”兜底页。
- **2026-11-06 第三轮（连刷白屏的真因，R77）**：R73 的三处不是全部。桥的 native-method 表
  （`g_methods[1024]` / `g_method_count`）是**进程级且只增不减**，`antb_engine_destroy()` 没回收；
  每个 realm 注册约 170 个桥方法，约 6 次刷新就耗尽，之后 `antb_define_method()` **静默 no-op**
  → 新 `document` 只剩 `implementation`、load 监听器注册不上、`listeners=0`。已在 ant 侧修
  （按 engine 释放 + 复用空槽）；验收改成 `tools/run_hang_recovery_tests.ps1` **七条全绿**
  （第七条：`/chain.html` 连刷 10 次后最终文档仍有 `document.addEventListener`、5 个脚本全跑完）。
- **待办（不阻塞白屏修复）**：reload 仍没有“导航事务”收口 —— 连按刷新会为每次按键各发一个主资源
  请求（前一个由 `stopAllLoaders` 取消），且 `WebView::reload()` 的 `stopLoading(true)` 会在旧文档
  仍解析时提前派发它的 DCL/load。它们不丢 realm/监听器（realm 在 commit 时整体重建），但浪费一次
  往返、事件时序不现代。若要做：在 `FrameLoader` 加导航代次令牌，把 in-flight 的 reload 合并/排队，
  并让 `committedLoad`/`dispatchWillClose` 校验令牌。先记在这，等有真实站点依赖事件时序再动。
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

### P0-m 页面数据能碰到的 ASSERT = 进程退出（发布壳是 Debug 构建）

- 现状：`tools/build_project.ps1` 写死 `-DCMAKE_BUILD_TYPE=Debug`，所以 `src/wtf/Assertions.h` 的
  `ASSERT` 全部生效，失败即 `CRASH()`（写 `0xbbadbeef`，随后 OS 杀进程）。GUI 壳就是用户实际在用的
  那个二进制，于是**任何**一条页面样式/脚本能碰到的断言都会让浏览器直接消失 —— R131 的两条
  （圆角缩放四舍五入、legend 比 fieldset 高）都只要几行 CSS。
- 已做（2026-11-08，R131）：两条已知路径按 bug 修掉（`RenderStyle::scaledBorderRadii()` 改成精确
  有理数因子 + 向下取整；`RenderFieldset::paintBoxDecorations()` 把负的边框盒高度夹成 0、
  `paintBorderMinusLegend()` 把 startY 夹进 `[ty, ty+h]`），各留一个回归页。
- 要决定的事（二选一或都做）：
  1. **发布壳用 NDEBUG**：`build_project.ps1` 拆 debug（带断言，开发/测试用）与 release
     （`-DCMAKE_BUILD_TYPE=Release`，`ASSERT_DISABLED` 随 `NDEBUG` 打开）两个构建目录，断言只在
     debug 里致命。风险：断言现在也当着"半成品路径的哨兵"，关掉后那些路径会静默画错/少画，
     要先跑一遍测试矩阵确认没有只靠断言暴露的路径。
  2. **按输入来源分级**：给完全由 CSS/DOM 数据决定的几何断言加一个 `PAGE_ASSERT`（记录 + 继续，
     不进 `CRASH()`），只在真的"引擎自己不变量"上用 `ASSERT`。
- 判据：一批"故意恶心"的页面（0 高度盒、超大圆角、负 margin、legend 溢出、table 塌陷、百分比圆角）
  连续跑 10 分钟不产生新的 `agb_crash.txt`；`run_layout_tests.ps1` 仍全绿。

### P1 长页面帧率：整视口重绘仍是 ~70ms（R135 之后）

- 现状（2026-11-08，R135 修完脏矩形放大与"样式变化=整页布局"之后）：css3test 每帧
  写一次内联样式从 5.3 → **11.7 FPS**，剩下的成本是**整视口重绘 ~70ms**（1084x677，
  文字为主）；`AGB_PERF_LOG` 的 `[perf]` 行给出 `layout=0 paint=70 rect=1068x677`。
  `AGB_PROFILE` 的采样热点在 Picasso 的 pattern 填充（`wrap_mode_repeat`）与字形绘制。
- 可选动作（按性价比）：
  1. **字形光栅缓存**：同一 (font, glyph, 缩放) 只栅格化一次，按字形 bitmap 复用
     （R167 起文字默认走 AA outline，每次绘制都要重新光栅化；整视口文字重绘因此比
     1-bit mono 慢约 19%：`debug/text_paint.html` + `debug/fps_paint.js` 10.6→8.6 FPS）；
  2. **平铺背景缓存**：把 pattern 填充的整块结果缓存成一张 tile 图（尺寸/phase 变了才重做），
     `RenderBox::paintBackgroundExtended()` 只 blit；
  3. **分块失效**：现在 html/body 样式变化会 `view()->repaint()`（WebKit 的 canvas 规则），
     若背景本身没变可以只重绘变化区域（要区分 background-position 与 background-color）。
- 判据：css3test 每帧写内联样式的 rAF 计时 ≥ 25 FPS；`run_layout_tests.ps1` 与像素组不退化；
  截图与现在逐像素一致（缓存不能改变外观）。

### P2 多 tab 并行

- 现状：一条引擎线程串行跑所有 view（[arch-overview.md](arch-overview.md) §1），所以 tab 之间既不并行，
  卡死的 tab 还会挡住宿主命令与别的 tab 的定时器/XHR（只有 1.5 s 看门狗中断能救）。
- 第一步（便宜、可先做）：**隐藏 view 不画** —— 已完成（R150，2026-11-09）。帧泵原来对每个 view 都
  `macross_view_update()`（`paintView()` + `pumpAsyncRequests()`），隐藏 tab 的绘制纯浪费。现在
  `AgaveWebView` 带一个 Page Visibility 标志（shell 的后台 tab / 最小化窗口驱动），不可见时只跑
  `macross_view_pump()`（标题 + 异步工作）、不布局不绘制不拷 mirror，回到可见时整幅重绘一次。
  ⚠ 判据不是 `IsWindowVisible`：探针窗口永远是隐藏的，但它必须照常出像素（`--shot`），所以开关必须是
  宿主明确给出的 Page Visibility，而不是 OS 可见性。判据：`tools/run_hidden_view_tests.ps1`（隐藏期
  `[loop] paints=` 冻结、`skips=` 增长，定时器照跑，显示后首帧重绘）。
- 已知缺口（下一步，便宜的第二个点）：**`requestAnimationFrame` 是 `setTimeout(16)` 垫片**
  （`prelude.js`），所以隐藏 tab 的 rAF 循环仍在跑 JS（浏览器会把隐藏 tab 的 rAF 停掉/限速）。
  绘制已经跳过，但 JS 还在烧引擎线程：后台动画页（css3test 那类 rAF 写样式的）依然会拖慢当前 tab。
  动作：把 `requestAnimationFrame` 换成宿主的真 rAF（帧泵里按可见 view 出帧），或在垫片里按
  `document.hidden` 把回调排队、`visibilitychange` 恢复时补发；判据：隐藏 tab 的 rAF 计数停止增长，
  显示后恢复，`run_hidden_view_tests.ps1` 加一条。
- 已修（2026-11-09，R157）：**导航期间提交的宿主命令不再静默消失**。命令按"发出时的文档代数"盖章，
  跨代就丢弃并回调 `on_command_dropped(view, kind, reason, user)`；本地后端用 `submit` 返回 0 表达同一件事，
  远端后端回 `COMMAND_DROPPED` 事件（`AGB_VIEW_EVENT_DOCUMENT` 保证静态页面提交后宿主也学得到新代数）。
  判据（`run_process_backend_tests.ps1` 16 项）：无页面时的 eval、navigate 刚返回就 eval、`AGB_IPC_STALE_EPOCH=1`
  三种情形都必须"执行或报错"。顺带修掉 `agwPost()` 里 `queued` 判空早于入队的死条件——导航现在真的会打断卡住脚本。
- **路线已定（2026-11-09）**：不是"每 view 一条引擎线程"，而是**渲染后端可插拔**
  （local / Win32 子进程 / RVVM 实例，共用一个"渲染后端协议"），设计、协议 v1、三后端映射、
  分批计划与实测数据都在 [design-render-backend.md](design-render-backend.md)。
  - 线程化的代价集中在三块进程级假设：宿主镜像（`g_currentState`）、桥的 trampoline/wrapper 表、
    WebCore 单例（`Settings::globalSettings()`、`ImageDecodeService::shared()`、全局 `AtomicString`
    字符串表无锁）；多进程让这些天然每进程一份 → 线程化降级为备选（parked），避免两套方案各做一半。
  - 协议要点：批量命令环（一个环项可带多个输入事件）、**零拷贝 BGRA + damage**（主路径）、
    GL 合成只用于缩放/缩略图、输入保序与合并规则、`SERVICE_REQUEST/REPLY`（popup/dialog/download/
    clipboard/文件选择）、visibility/focus/resize 独立于生命周期、帧 ack/背压/崩溃代际。
  - 实测（RVVM `bench_ecall`，GTX 1080 Ti + ANGLE；详见设计文档 §6）：裸 ecall 72 ns、逐调用 GL 通路
    1.29 µs/调用（大头是封送+分发，不是 ecall）、3000 调用 3.9 ms（在 16.7 ms 预算内，所以"撑不住"不成立，
    是"不划算"：整帧软件填充地板 0.036 ms）；全帧 GL 上传 0.51–0.88 ms，零拷贝 damage 0.034–0.07 ms。
    NOJIT 下结论反转：guest 软填充 1–2 ms > GL 上传 0.53 ms，且纯 guest 计算本底 114 ns vs 7.1 ns
    → **引擎放进 VM 必须以 JIT 为前提**。
  - 分批：B0 接口 + local（行为零变化）→ B1 协议 v1 → B2 process 后端 → B3 rvvm 后端 →
    B4 服务通道 + `window.open` 异步（与 S4 WindowProxy 一起）→ B5 冻结/压测/profile 策略。
  **B4 的最小一片已完成**（2026-11-09，R158）：`SERVICE_REQUEST(open_window/close_window)`，
  `target=_blank` / 具名 target / `window.open` / `window.close` 在 local 与 process 两个后端一致，
  具名窗口由控件注册表复用并回调 `on_activate_view`；判据 `tools/run_window_tests.ps1`（两后端各 5 项）。
  剩下的服务（dialog/download/clipboard/file_picker）与 `window.opener` 仍挂在 B4/S4。
  **B3 的跨仓库接口已出**（2026-11-09）：`virtpass_browser v1` 规格见
  [ref-virtpass-browser.md](ref-virtpass-browser.md)（两跳拓扑、guest 分配共享控制块 + `BP_*` ecall、
  中断 50ms 轮询、帧搬运一次拷贝、RVVM 侧待实现清单与验收）；agave 侧 `AgwViewBackendRvvm` 与
  process 后端共享 90%，风险在 guest 端口。
- 验收：两个 tab，一个跑 `while(true){}`，另一个点击/刷新在 200 ms 内响应（当前要等 1.5 s 中断）；
  其余验收数字见设计文档 §1/§8。**验收 1 已可复现**（`AGB_VIEW_BACKEND=process` +
  `agave_multiview_probe.exe`：B 的延迟 31 ms，local 对照 `blocked=1`；收在
  `run_process_backend_tests.ps1` 的 13 项里）。process 传输上的两项实测已进设计文档 §6.3：
  一次镜像拷贝 ~0.45 ms/帧（1100×800，首帧冷启动 ~3.1 ms），帧发布→控件 poll 平均 15.8 ms
  （轮询节奏的下限，RVVM 那一路用事件驱动的 `BP_WAIT` 不该复现这个数）。

### P2 工具栏文本闪烁（`<` / `>` 在刷新时闪）

- 现象：刷新时前进/后退按钮上的箭头文本闪一下；history 状态并没有变化（`cb_history_status` 在刷新期间一次都不触发，
  已用 `AGB_HISTORY_DEBUG=1` 验证）。
- 根因：外壳每次 `WM_SIZE` 都 `layoutAll()`，对整排按钮无条件 `MoveWindow(..., TRUE)`（强制重绘），
  而加载过程中滚动条出现/消失会让客户区尺寸变化 → 一连串 `WM_SIZE` → 每个按钮被反复重绘；父窗口又没有
  `WS_CLIPCHILDREN`，`WM_ERASEBKGND` 之外的背景绘制会盖住子控件文本。
- 已修（2026-11-06）：新增 `placeChild()`，几何未变就不 `MoveWindow`；主窗口加 `WS_CLIPCHILDREN`；
  `WM_ERASEBKGND` 保持返回 1（原实现已有，避免重复擦除）。
- 验收：GUI 里刷新时箭头不再闪；回归 `run_hang_recovery_tests.ps1` 六条全绿（布局代码不影响探针）。

### P2 文字尺寸对齐的三个小缺口（2026-11-09；2/3 已修）

修 `rem`（R165）和 163 编码（R164）时顺带量的，1 仍未修，2/3 已在同一批补掉（R166）：

- **默认根字号 14px vs Chrome 16px**：`Settings::init()` 里 `m_defaultFontSize = 14`，
  而 Chrome/WebKit 默认是 16。页面不写 `html{font-size}` 时，`rem` 与 `medium` 派生的
  字号/行高整体小 12.5%（`debug/rem_probe.html` 实测 `root=14px`）。**字体本身**已在 R168 换成
  Chrome 的 Noto Sans SC（随构建打包），所以现在只剩字号这一项。影响面是所有站点，不只是 rem。
  下一步：先把默认值改成 16，跑 `run_layout_tests`（字体相关页要用固定根字号）、`run_site_smoke`
  与几张贴图对比；`config.ini`/设置页里用户的显式字号要继续优先。
- **`@media (min-width: 48rem)` 的 rem 不求值 → 已修（R166）**：`CSSParser::rewriteMediaQueryRem()`
  在语法前把 `@media` 前导里的 rem 按 CSS 初始 16px 折成 px（声明里的 rem 仍按根字号），
  `parseMediaQuery()` 的字符串路径与 `prelude.js` 的 `matchMedia` 同步；验收：
  `rem_units.html` 的 `rem-media-query`（60×10）、`resize_probe` 的 20rem/40rem 与
  `matchMedia(min-width:20rem/40rem)` 断言（`run_layout_tests` resize 组）。
- **块盒 computed `margin-right` 返回 used 填充值 → 已修（R166）**：`CSSComputedStyleDeclaration`
  改走 `valueForMargin()`：`auto` 用 used 值，百分比/calc 按包含块宽度解 px，固定长度用指定值；
  验收：`rem_units.html` 的 `rem-margin` 重新断言 `margin-right=20px`（14 case），
  `debug/margin_computed.html` 与 Chrome 的 8 个 case 对拍。

### P1-x deepseek 内联监控脚本抛 `undefined is not a function`（站点兼容，**未修**）

- 现象：`https://chat.deepseek.com/` 与 `/sign_in` 的**第一段内联脚本**（`_sriCk` / volcengine 监控，
  正文里的 `"use strict";(()=>{…}）`）在 agave 里抛
  `TypeError: undefined is not a function`（列号 ~1497，指向 `a("scriptLoadError", …)` 之前的初始化段），
  于是这段监控/兜底逻辑不生效；同时更早出现的
  `NotSupportedError: Only raw key import is supported`（`H:\ant-v16\src\modules\crypto.c:1217`）说明
  `crypto.subtle.importKey` 只支持 `raw`，站点用 SPKI/PKCS8 导入会失败。
- 影响：站点自身功能不受致命影响（主 SPA 仍能启动），但 Any 依赖该监控/兜底的分支会走错；
  排查白屏时容易被这条错误带偏（R73 的真因是缓存污染，不是它）。
- 下一步：①补 `crypto.subtle.importKey` 的 `spki`/`pkcs8`（至少 RSA/EC 公钥解析，或返回可用的空实现并打日志）；
  ②用 `debug/ds_index.html` 里的那段脚本单独跑，定位 `undefined is not a function` 具体缺的 API；
  ③把结论写回 `arch-capability-gaps.md`。

### G2 真实站点视觉正确：GitHub 首页（进行中）

- [x] 后到样式表触发重算（`FrameView::layout` → `updateStyleIfNeeded`）
- [x] 重算后整体重绘（`Document::updateStyleIfNeeded` → `renderer()->repaint()`）
- [x] 片段解析的 `<style>` 建表（`HTMLStyleElement::insertedIntoDocument`）
- [x] `--shot` 抓最终帧而不是第一帧（探针把页面标可见）
- [ ] GitHub 首页截图复测（R187 修复后）
- [x] shadow DOM：`::part()`/`::slotted()`/shadow 内样式作用域（R188；`run_shadow_tests`）
- [x] `slot` 分发 + `slotchange`（R188；`assignedNodes/assignedSlot`、微任务合并）
- [x] `createTreeWalker`、`requestFullscreen`（R188；全屏只做 DOM 语义，窗口级另立）
- [ ] 像素级回归：把"后到样式表"场景加进 `run_layout_tests`
