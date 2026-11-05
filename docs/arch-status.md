# agave 现状：能力、已知限制与验证入口

> 状态：**当前态，最后核对 2026-11-06**。本文只回答"现在能做什么、哪些是已知限制"。
> 架构与所有权见 [arch-overview.md](arch-overview.md)；待办与验收标准见 [plan-roadmap.md](plan-roadmap.md)；
> 历史根因与设计取舍见 [hist-lessons.md](hist-lessons.md)；API 级缺口见
> [arch-capability-gaps.md](arch-capability-gaps.md)；函数级锚点见 [ref-code-index.md](ref-code-index.md)。

## 1. 已具备的能力

| 领域 | 现在具备 | 证据 / 细节 |
|---|---|---|
| 页面与导航 | HTTP(S)/`file:` 加载、HTML/CSS 解析、`defer`/`async`/ESM、301/302、`about:blank`、iframe、`agave://` 内部页、前进后退 | R44–R49；[hist-lessons.md](hist-lessons.md) |
| JS / DOM | ant VM、DOM 桥、事件（鼠标/键盘/IME/输入）、computed style、选择器、`history` state、真实 DOM 集合、每 frame 一个 Window 代理；页面脚本按 **classic script** 语义执行（顶层 `var`/`function` 落到 `window`，R66）；**每 frame 一个 realm**（iframe 独立全局/intrinsics/wrapper，`var` 不串帧；`AGB_PER_FRAME_REALM=0` 回退旧模型） | R27/R31/R34–R37/R45/R66/R67 + [design-per-frame-realm.md](design-per-frame-realm.md) |
| 模块加载 | 入口 module 单槽等待（等待期间到达的数据不提前解析，R50）、解析调用栈内求值、延迟模块 flush；三条泳道时序 | [arch-module-loading.md](arch-module-loading.md) |
| CSS | 自定义属性/`var()`、`display:flex`/blockify、`gap`、`aspect-ratio`、`:is()`/`:where()`、媒体查询与深色模式、`box-shadow`、常量 `calc()`、圆角裁剪 | R39–R43；能力矩阵见 [arch-capability-gaps.md](arch-capability-gaps.md) §2 |
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
- **入口模块管线的 5 条限制**：动态插入/async 入口仍走 CachedScript；入口 `<script>` 的 `load`/`error` 不派发；
  `import.meta.url` 对 file: 模块显示伪装 URL（`http://file.ant/...`）；DCL 之后到达的 inline 模块不再执行；
  iframe 的模块入口回退 CachedScript。详见 [arch-module-loading.md](arch-module-loading.md)。
- **一个 view 一个 engine，多个 frame 多个 realm**：同一进程的控件共用一条引擎线程；每个 frame 文档
  有自己的 realm（独立全局/intrinsics/wrapper，默认开启），模块 map 仍按引擎共享（S3 待做），
  `parent`/`top` 是父/顶层 realm 的全局对象而不是 WindowProxy（S4 待做）。细节见
  [design-per-frame-realm.md](design-per-frame-realm.md)；旧模型用 `AGB_PER_FRAME_REALM=0` 回退。
- **CSS 边界**：grid 布局、`position:sticky` 未实现（按 static 降级）；transform 做了
  translate/scale/rotate 的绘制与变换感知的脏矩形（hit-test、stacking context 未做，R52/R55）；`vw/vh` 在解析期换算成 px
  （窗口 resize 不重算）；含 `%` 的混合 `calc()` 已支持（R54，`Length::Calculated`），
  flex 简写/`flex-basis`/`align-content`/`scrollbar-width` 已支持（R54），
  `::-webkit-scrollbar{display:none}` 会在解析期改写成 `scrollbar-width:none`（R56）。
  完整实测值与复测命令见 [arch-capability-gaps.md](arch-capability-gaps.md) §2。
- **wasm 是解释执行**：导出函数跨界约 3.9µs/次，适合"一次调用跑大循环"（R19）。
- **worker 无网络 / 无 `SharedWorker`/`ServiceWorker`**：worker 里 `fetch`/`XHR` 不可用，
  动态 `importScripts` 只能父侧预取 + 查表（R22）。
- **GC root 只能到 engine 边界**（ant 没有 unpin）：per-document engine 让这一点等价于"每文档释放"（R4）。
  代价是 `antb_retain()` 住的事件监听器/回调即使 `removeEventListener` 也要留到 realm 销毁（R67）。
- **localStorage 不跨文档**：同一进程内 A 文档写入后导航到 B，B 读到 `null`（cookie 正常；跨进程能读到旧值）。
  写盘发生在文档 teardown，可读要等下一次 engine 重建；导航类测试页需要跨文档状态时用 cookie。
- **多控件共用一条引擎线程**：每个控件有独立 JS 上下文，但同一进程的控件不会真正并行；
  回调线程契约见 [arch-embedding.md](arch-embedding.md)。
- **下载面板仍是普通 realm**（没有独立生命周期），面板每次打开重新读快照；后续见 [plan-roadmap.md](plan-roadmap.md)。

## 3. 怎么验证

```powershell
agave_shell.exe --probe <url> --eval "probeExit(...)" --wait 3000 --settle 1000 --timeout 60000
```

退出码是权威判据（`0` OK / `1` usage / `2..4` 超时或无 exit / `5` 加载错误 / `6` 空文档）。
stderr 默认有 `[evt]`/`[nav]`/`[ui]` 生命周期与导航日志，开关和环境变量全表见
[dev-debugging.md](dev-debugging.md)。回归脚本、`[gap-jsdom]`、A/B 对比法也在那一篇。
