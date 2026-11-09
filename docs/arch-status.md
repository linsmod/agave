# agave 现状：能力、已知限制与验证入口

> 状态：**当前态，最后核对 2026-11-07**。本文只回答"现在能做什么、哪些是已知限制"。
> 架构与所有权见 [arch-overview.md](arch-overview.md)；待办与验收标准见 [plan-roadmap.md](plan-roadmap.md)；
> 历史根因与设计取舍见 [hist-lessons.md](hist-lessons.md)；API 级缺口见
> [arch-capability-gaps.md](arch-capability-gaps.md)；函数级锚点见 [ref-code-index.md](ref-code-index.md)。

## 1. 已具备的能力

| 领域 | 现在具备 | 证据 / 细节 |
|---|---|---|
| 页面与导航 | HTTP(S)/`file:` 加载、HTML/CSS 解析、`defer`/`async`/ESM、301/302、`about:blank`、iframe、`agave://` 内部页、前进后退；文档编码按 **BOM > HTTP `Content-Type` charset > `<meta charset>` > 默认** 决定（R164） | R44–R49；[hist-lessons.md](hist-lessons.md) |
| JS / DOM | ant VM、DOM 桥、事件（鼠标/键盘/IME/输入）、computed style、选择器、`history` state、真实 DOM 集合、每 frame 一个 Window 代理；页面脚本按 **classic script** 语义执行（顶层 `var`/`function` 落到 `window`，R66）；**每 frame 一个 realm**（iframe 独立全局/intrinsics/wrapper，`var` 不串帧；`AGB_PER_FRAME_REALM=0` 回退旧模型）；`location`/`history` 是 document-scoped（R132：子 frame 读自己的 URL/历史，`document.location === window.location`，`parent.location` 读父文档）；文档生命周期 `document.readyState` 是真实状态（`loading`/`interactive`/`complete`，R93），`document.currentScript` 指向正在执行的 `<script>`（R95） | R27/R31/R34–R37/R45/R66/R67/R93/R95/R132 + [design-per-frame-realm.md](design-per-frame-realm.md) |
| 模块加载 | 入口 module 单槽等待（等待期间到达的数据不提前解析，R50）、解析调用栈内求值、延迟模块 flush；三条泳道时序 | [arch-module-loading.md](arch-module-loading.md) |
| CSS | 自定义属性/`var()`、`display:flex`/blockify、`gap`、`aspect-ratio`、`:is()`/`:where()`、媒体查询与深色模式、`box-shadow`、常量 `calc()`、圆角裁剪、长度单位含 `rem`/`em`（R165） | R39–R43；能力矩阵见 [arch-capability-gaps.md](arch-capability-gaps.md) §2 |
| 网络 | WinHTTP 后端、HTTP 缓存、代理解析、重定向、MIME/下载决策、`[netlog]` | [dev-debugging.md](dev-debugging.md) |
| Worker / wasm | 真 Worker（独立线程 + isolate）、wasm 解释执行（导入/内存/表/全局/SIMD） | R19/R22 |
| 下载 | 进程级 `DownloadManager`、`.part` 后 rename、三层状态 API、`agave://downloads` 面板 | [arch-downloads.md](arch-downloads.md) |
| 宿主 / UI | 标签页、地址栏、查找、书签、popup、下载面板、无头探针与无头截图、Unicode/IME | `examples/browser_shell`、[dev-debugging.md](dev-debugging.md) |

**已经被检查过、别再当缺陷重开**：ant 的 Worker 是真的（R22）、wasm 是真的（R19）、
渐变/`var()`/flex 大多可用（R11/R39）、滚动与 hover 重绘正确（R13/R14/R40）、
刷新后 realm/监听器会清理（R9/R10）。全局对象上的能力缺口**不用 Proxy 检测**是评估后的决定（R33）。
ant 的反直觉行为（字面量属性名、`typeof`、`delete` 等）见 [ref-ant-engine.md](ref-ant-engine.md)。

## 2. 已知限制（不是 bug，但接手前要知道）

- **UNC 路径**：`\\server\share` 会规范成 `file://server/share/...`，但 file loader 不认 URL 的 host
  （会变成 `file:///share/...` 而加载失败）。要支持得改 `ResourceHandle` 的 file 分支（R23）。
- **入口模块管线的 4 条限制**：动态插入/async 入口仍走 CachedScript；入口 `<script>` 的 `load`/`error` 不派发；
  `import.meta.url` 对 file: 模块显示伪装 URL（`http://file.ant/...`）；DCL 之后到达的 inline 模块不再执行。
  iframe 的模块入口（inline 与 `src=`）与 frame 内的 `import()` 已能执行（P1-e，2026-11-09）。
  详见 [arch-module-loading.md](arch-module-loading.md)。
- **一个 view 一个 engine，多个 frame 多个 realm**：同一进程的控件共用一条引擎线程；每个 frame 文档
  有自己的 realm（独立全局/intrinsics/wrapper，默认开启），**ES module map 也按 realm**
  （`js->modules` 进了 realm 镜像表：两个 frame import 同一 URL 各求值一次，P1-f 2026-11-09），
  **窗口命名/索引属性**（`frames[0]`、`frames['kid']`、`window[0]`）由装在每个 realm 全局对象上的
  同一个 catch-all getter 回答（R148），`parent`/`top` 是父/顶层 realm 的全局对象而不是 WindowProxy
  （S4 待做）。细节见 [design-per-frame-realm.md](design-per-frame-realm.md)；旧模型用
  `AGB_PER_FRAME_REALM=0` 回退。
- **桥回调是 activate/leave 成对的**：每次 JS→C 桥方法进来先 `contextActivate()`（按当前 realm 切
  view/document 镜像），返回前 `contextLeave()` 把调用方脚本的文档还原（R149，P1-aa）；回调里发生
  导航时只还原仍在的文档记录，teardown 清空保存栈。
- **隐藏 view 不布局不绘制**：后台 tab / 最小化窗口的 view 仍然跑定时器、XHR、网络和标题更新
  （`macross_view_pump()`），但引擎帧泵跳过它的 `paintView()` 与 mirror 拷贝（`AgaveWebView` 的
  `visible` 标志，R150）；回到可见时整幅重绘一次（跳过期间的帧没画过）。布局仍是按需的：hidden 页的
  几何读（`offsetWidth`/`getBoundingClientRect`…）会内部 `updateLayout()`。这是 S3（每 view 一条
  引擎线程）的前置：K 条线程不能都去画看不见的 view。
- **CSS 边界**：grid 布局未实现（降级成 block）；`position:sticky` 已支持（R82，绘制/命中/几何随滚动平移，
  滚动时整视口重绘）；transform 做了
  translate/scale/rotate 的绘制与变换感知的脏矩形（hit-test、stacking context 未做，R52/R55），
  文字字形也跟随同一矩阵（R80，`Font::drawGlyphs()` 走 picasso 的 text matrix）；`vw/vh`、`@media` 断点
  和 `window` 的 `resize` 事件都跟随窗口 resize（R69：vw/vh 是文档级变量，`FrameView::layout` 在尺寸变化时先重建
  selector 再重排）；含 `%` 的混合 `calc()` 已支持（R54，`Length::Calculated`），
  flex 简写/`flex-basis`/`align-content`/`scrollbar-width` 已支持（R54），
  CSS Color 4 色彩函数（`hsl(200 60% 20%)`、`rgb(1 2 3 / 50%)`、百分比分量、`deg` hue）已支持（R83），
  `::-webkit-scrollbar{display:none}` 会在解析期改写成 `scrollbar-width:none`（R56）。
  完整实测值与复测命令见 [arch-capability-gaps.md](arch-capability-gaps.md) §2。
- **SVG**：只有 **JS 建树**的 inline `<svg>`（`createElementNS`）会走 ksvg2 渲染；HTML 静态标记里的
  `<svg>` 仍被当成普通未知元素（HTMLParser 没有外来内容分支，B2）。SVG 根现在参与布局首选宽（R119，
  flex/shrink-to-fit 容器不再塌成 0 宽），但页面缩放 ≠100% 时内容相对盒子仍有偏移；
  `<img src=*.svg>` / CSS `url(*.svg)` 背景仍不渲染（阶段 C）。细节见
  [design-ksvg2-revival.md](design-ksvg2-revival.md)。
- **wasm 是解释执行**：导出函数跨界约 3.9µs/次，适合"一次调用跑大循环"（R19）。
- **worker 无网络 / 无 `SharedWorker`/`ServiceWorker`**：worker 里 `fetch`/`XHR` 不可用，
  动态 `importScripts` 只能父侧预取 + 查表（R22）。
- **GC root 只能到 engine 边界**（ant 没有 unpin）：per-document engine 让这一点等价于"每文档释放"（R4）。
  代价是 `antb_retain()` 住的事件监听器/回调即使 `removeEventListener` 也要留到 realm 销毁（R67）。
- **localStorage 不跨文档**：同一进程内 A 文档写入后导航到 B，B 读到 `null`（cookie 正常；跨进程能读到旧值）。
  写盘发生在文档 teardown，可读要等下一次 engine 重建；导航类测试页需要跨文档状态时用 cookie。
- **多控件共用一条引擎线程**：每个控件有独立 JS 上下文，但同一进程的控件不会真正并行；
  回调线程契约见 [arch-embedding.md](arch-embedding.md)。
- **关 view/tab 是引擎线程上的异步拆除**：`agave_webview_destroy()` 只标记 + 入队，引擎线程在
  `event_dispatch()` 之后跑 `documentWillUnload`（pagehide/visibilitychange/unload）→ realm
  teardown → DOM/VM 销毁；宿主线程调用时等一次回收（2 s 上限）。文档级资源的清理挂在
  `Document::detach()` 上（媒体、字体），见 R141 与 [design-tab-teardown.md](design-tab-teardown.md)。
- **默认根字号 14px（Chrome 是 16px）**：`Settings::m_defaultFontSize` 的初始值就是 14。页面自己写了
  `html{font-size:…}` 时没有影响；没写的站点，`rem`/`medium` 相关的尺寸整体比 Chrome 小约 12.5%
  （R165 顺带发现）。`@media` 前导里的 `rem` 不跟这个值走：它按 CSS 初始 16px 折叠（R166，与 Chrome 的
  断点一致）。改默认值会动到所有字体度量与既有 layout 回归，单独排期。
- **下载面板仍是普通 realm**（没有独立生命周期），面板每次打开重新读快照；后续见 [plan-roadmap.md](plan-roadmap.md)。

## 3. 怎么验证

```powershell
agave_shell.exe --probe <url> --eval "probeExit(...)" --wait 3000 --settle 1000 --timeout 60000
```

退出码是权威判据（`0` OK / `1` usage / `2..4` 超时或无 exit / `5` 加载错误 / `6` 空文档）。
stderr 默认有 `[evt]`/`[nav]`/`[ui]` 生命周期与导航日志，开关和环境变量全表见
[dev-debugging.md](dev-debugging.md)。回归脚本、`[gap-jsdom]`、A/B 对比法也在那一篇。
