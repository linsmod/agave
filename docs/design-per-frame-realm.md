# 每 frame 一个 realm：设计记录（规范对齐）

> 状态：**设计记录**，S0 起实施中。当前实现在 [arch-overview.md](arch-overview.md) §3、
> [design-frame-window.md](design-frame-window.md)（方案 B 的记录，**不是现状**）。
> 能力/限制见 [arch-status.md](arch-status.md)；任务路由见 [dev-onboarding.md](dev-onboarding.md)。

## 1. 问题

现在**一个 view 只有一个 realm**：iframe 与主 frame 共享同一个全局对象、同一套 intrinsics，
只是每 frame 有一个 `WindowProxy`（R31）。与 HTML 规范的差距：

| 规范 | 现状 | 后果 |
|---|---|---|
| 每个 navigable 一个 realm（独立全局 + 独立 intrinsics） | 一 view 一 realm | `iframe.contentWindow.Array === Array`；跨 frame `instanceof` 恒真 |
| 子 frame 的顶层 `var`/`function` 属于自己 | 写进共享全局 | 父子互相污染；同名库单例互相覆盖（v.qq "duplicated SuperPlayer"） |
| 跨 realm 传对象不共享原型 | 共享 | 组件库的 `instanceof`/`constructor` 判定走错分支 |
| 每个 document 独立派发 DCL/load | iframe 的 DCL 不单独派发 | 子 frame 里的框架初始化时序错 |
| `window` 是 realm 的全局 | 全局本身（对顶层正确） | 子 frame 的 `window` 就是父的全局 |

## 2. 为什么可行：ant 原生支持多 realm

ant 的 realm 是**同一个 isolate/VM 里的独立全局**，不是新 VM：

- `ant_realm_create(js)` 建一个 realm：独立的 `global`/`global_this`、`builtins`（`Array`/`Object`/
  `Promise`/... 全套 intrinsics）、`global_lexicals`、microtask 队列、builtin records（`include/realm.h`）；
- `ant_realm_enter(js, realm)` 切换，realm 状态保存在 `ant_realm_t`，进出开销是字段搬运；
- 函数/闭包带 `func->realm`，调用时自动切回它自己的 realm（`src/silver/engine.c`、`glue.c`）；
- 跨 realm 的对象引用是**同 VM 原生指针**，不需要序列化（`parent.foo`、`parent.fn()` 都能直接用）；
- GC 管 realm 生命周期：`ant_realm_collect`、`ant_realm_token_retain/release`；
- Node 的 `vm.createContext` 就是这套（`src/modules/vm.c`），回归 `tests/test_vm_realm_gc.cjs`。

所以 agave 要做的不是"再造一个引擎"，而是**把 realm 的粒度从 view 改成 document**。

## 3. 目标架构

```
view（一个 AgaveWebView）
 ├─ AntbEngine（一个 isolate / VM，一个引擎线程）
 │    ├─ realm[0] 主 frame 文档       ← window = realm.global
 │    ├─ realm[1] iframe 文档         ← 自己的 global/intrinsics/lexicals
 │    └─ ...
 ├─ 每 document：RuntimeState 的 document 段（listeners / history / 定时器 / 模块表）
 └─ WindowProxy（每 frame 一个）：parent/top/frames[i] → 进入目标 realm 读它的 global
```

- **window = 当前 realm 的 global**（顶层成立；子 frame 也成立）。- **`parent` / `top` / `frames[i]`** 是转发对象：get/set/has 进目标 realm 再操作它的 global。
  同 VM 所以 `parent.document`、`parent.fn()` 仍然可用，但 `parent.Array !== Array`、跨 realm
  `instanceof` 为 false —— 这正是规范行为。
- **DOM 包装按 realm 分**：同一个 DOM node 在父/子 realm 里有不同的 wrapper 对象
  （`parent.document.body !== document.body`，规范如此）。
- **模块表按 realm**：ES module map 属于 realm，不再进程级共享（规范如此；也修掉两个 frame
  重复求值/单例覆盖的问题）。**尚未实现**（`js->modules.state` 仍是引擎级）。

### 3.1 prelude / polyfills 注入几次

`prelude.js` 和 `polyfills.js` 都是**每个 realm 注入一次**（`g_preludedRealms` /
`g_polyfilledRealms` 按 realm 记账），也就是：**每个 frame 文档一次，换文档（新 engine）再来一次**。
一个主文档 + N 个 iframe 就是 N+1 次；这与"页面每个 frame 都有自己的 Web API 表面"一致，
不是重复注入。`internal.js` 仍只给 `agave://` 页。

宿主 API（`__agb*` + window 方法）由 `bindDocument()` 在创建 realm 时安装；**eval 漏斗
（`antbEvalPage` → `ensureGlobalApi()`）兜底**：如果某个 realm 在被求值时还没装过 API
（比如 ant 因为调用另一个 realm 的函数而切了进去），先补装再执行。之前 v.qq 报的
`__agbHttpRequestEx is not defined`（prelude 的 `fetch` 里）就是"realm 有 prelude 但没 API"
这一类，现在不可能再出现。

## 4. 影响面清单（改哪、为什么）

| 子系统 | 现状 | 目标 | 风险 |
|---|---|---|---|
| `RuntimeState`（per view） | 持有 engine + 单 realm | engine（VM）+ document→realm 映射 | 现有 `activateContext()` 的语义要拆成"切 view"和"切 realm" |
| `beginNewRealm()` | 换文档 = 销毁/新建 engine | 换文档 = 新建/销毁 **realm**，engine 保留 | engine 级缓存（intern/shape）跨文档复用，要确认没有按文档的生命周期假设 |
| `bindDocument()` | 只设 `g_document`/slot | 还要 `ant_realm_enter(docRealm)` + 发布该 realm 的 `window` | 每个入口（事件/定时器/网络回调）都要先进 realm |
| wrapper 表（`AntBridge.c`） | 进程级 `(js, ptr, tag)` | 加 realm 维度，按 realm 释放 | 命中率/内存；`owner` 释放逻辑要按 realm 扩 |
| 监听器表（`g_listeners`） | per view | per document（realm） | 现在 `listeners()` 读 `g_currentState->listeners`，要改成 realm 维度 |
| 定时器 / `history` / 模块会话 | 引擎/进程级 | per realm | 定时器回调要切回创建它的 realm |
| `prelude.js` / `polyfills.js` | 每 realm 注入（已有 generation 机制） | 保持不变，但要验证真的每 realm 一份 | 共享全局上的跨 frame 助手（`__agb*`）要在每个 realm 注册 |
| `postMessage` | 同 realm 直接传 | 规范是结构化克隆 + `event.source` 代理 | 现有克隆逻辑保留，只修 source/origin |
| 内部页 / 下载面板 | 普通文档 | 自己的 realm | 面板的 `internal.js` 只在 `agave://` realm 注册 |

## 5. 迁移计划（每步都可独立验收）

- **S0 桥接**：`antb_realm_create/destroy/enter/handle`，以及"调用前确保在 realm"的入口；
  自测：一个 engine 建两个 realm，`Array !== Array`、A realm 的 `var` 在 B realm 看不到、
  `parent.fn()`（跨 realm 调用）仍能跑。
- **S1 每 document 一个 realm**：engine 保留，主 frame 与 iframe 各一个 realm；
  `bindDocument` 进 realm；`window` = realm global。
  验收：`cap_realm_identity.html` —— iframe 里 `var x` 父页读不到、`top.Array !== Array`、
  `window.top !== window`。**状态：已实现并默认开启（`AGB_PER_FRAME_REALM=0` 回退旧模型）**，
  验收 `tools/run_realm_tests.ps1`：`childVarLeak=undefined`、`topArraySame=false`、
  `parentFn=PF-ok`、`childReachable=string`、`sameBody=false`；nav/typing/worker/layout/jit/
  script-mode 全绿；deepseek / baidu / v.qq 正常。
- **S2 wrapper 按 realm**：DOM 包装加 realm 维度；`parent.document.body !== document.body`。
  验收：跨 frame 节点比较/事件目标正确；`cap_dom.html`、`cap_window_identity.html` 全绿。
  **状态：已完成**（ant `4494171c`：wrapper 表键 `(js, realm, ptr, tag)`、`gc_unpin_permanent`、
  `antb_release_realm`；agave 掉帧时释放该 realm 的 wrapper）。已知偏差（S3/S4 修）：
  模块表仍按引擎、iframe 的 DCL 仍不单独派发、`parent` 是父 realm 的 global 而不是 WindowProxy。
- **S3 事件/定时器/模块按 realm**：派发、回调、模块求值都先切到目标 realm。
  验收：iframe 的 DCL 单独派发；两个 frame 各跑一份同一 module 不互相覆盖；worker 回归不变。
  **状态：事件 + 定时器/realm binding 已完成**（`Document::finishedParsing()` 绑定本 document 后
  flush 模块再派发唯一的 DCL，iframe 由此获得自己的 DCL，同时修掉 P0-c 双发；frame 的 `load` 走
  `dispatchFrameDocumentEvent`；R130 把 `__agbBind` 的 per-view document slot 改成 realm 编号，
  每个 realm 各有一份 document/docObj binding，子 frame 的定时器与回调不再把父文档写进自己的
  document 对象）。**未完成**：ES module map 仍是引擎级（`js->modules.state`），
  两个 frame import 同一模块仍共享实例；要按 realm 得把 loader 状态搬进 realm 字段；
  getter 仍读 ambient `g_document`（`location`/`history`/`document.write` 等）的问题与
  收尾计划见 [design-realm-document-binding.md](design-realm-document-binding.md)。
- **S4 WindowProxy 转发**：`parent`/`top`/`frames`/`length`/`name`/`frameElement`、`postMessage.source`。
  验收：`cap_window_identity.html` + `cap_postmessage.html`（`event.source !== window`）。
- **S5 回归与文档**：nav/typing/worker/layout/jit/script-mode + v.qq/deepseek + 新验收页；
  更新 `arch-overview`/`arch-status`/`arch-capability-gaps`，把 `design-frame-window.md` 标为历史。
  **状态：已完成**：`tools/run_realm_tests.ps1`（10 项：裸引擎隔离、共享 realm 基线、
  `var` 不串帧、独立 intrinsics、per-realm wrapper、frame DCL、父 DCL 单次）进常规矩阵；
  `arch-overview` §3、`arch-status`、`plan-roadmap` P0-c 已同步。

## 6. 验收基线（每步都要保持）

- `tools/run_nav_tests.ps1` 8/8、`run_typing_tests.ps1` 8/8、`run_worker_tests.ps1` 9/9、
  `run_layout_tests.ps1` `LAYOUT=PASS`、`run_jit_loop_tests.ps1` 全绿、
  `run_script_mode_tests.ps1` 全绿。
- 探针：`cap_dom.html`、`cap_css.html`、`cap_window_identity.html` 的 `[exit]` 值不退化。

## 7. 备选与不做的理由

- **每 frame 一个 engine**（多个 VM）：跨 frame 引用要序列化，`parent.foo`/`parent.fn()` 全废，
  wrapper/监听器/定时器都要跨引擎代理 —— 收益与 ant realm 相同，代价高一个量级。不做。
- **继续用 WindowProxy 单 realm（方案 B，现状）**：修不了 `var` 隔离、intrinsics 身份、
  模块单例互相覆盖。作为过渡保留，S1 起被本方案替代。
