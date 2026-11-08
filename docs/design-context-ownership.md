# 上下文所有权：view / document / realm（设计记录）

> 状态：**设计记录 + 迁移计划**。S0 与 S1 已实现：
> `DocumentRuntime`（每文档一个 realm）+ 按 realm 的 window 监听器表、
> 按 view 的 frame realm 表、`agb_busy_js_context()`、`AGB_CONTEXT_ASSERT`。
> 当前行为的权威描述是 [arch-overview.md](arch-overview.md) §3，本文只讲**为什么这样切**与**怎么迁**。

## 1. 问题：三条互相放大的欠债

1. **环境即上下文**。`AntRuntime.cpp` 里 1103 处 `g_*` 是 `g_currentState->...` 的宏镜像
   （[AntRuntime.cpp:343](../src/javascript/ant_backend/AntRuntime.cpp)）。正确性依赖一条口头约定：
   "进任何入口先 activate"。每个新入口（定时器、XHR、事件、网络回调、worker、看门狗）都要自己记得；
   漏了是**静默**串台（R130 的 `boundRealm` / `mainDocObj` / `applyRealmBinding` 就是补丁）。
2. **所有权粒度和浏览器树不对齐**。`RuntimeState` 是 per-view 的，却装着 document 级字段
   （`document`/`docObj`/`documentBound`/listeners/slots/cssom）；frame realm 表曾经是**文件级全局**
   （S0 已收进 view）；模块 map 仍是 engine 级；`g_currentState` 是普通全局指针而不是 TLS。
   "我是哪个文档 / 哪个 realm" 要靠多个指针拼，跨 tab 会互相踩。
3. **一条引擎线程跑所有 tab**。不能并行，而且"谁卡住了"没有身份：看门狗只能把通知广播给所有 view。
   tab 级弹窗（页面无响应）在 S0 之前无法正确归属。

## 2. 目标形态

```
ViewRuntime (per view, 只在引擎线程用)
 ├─ engine (ant VM)                      ← 生命周期暂时仍是"每顶层文档一个新 VM"（见 §6）
 ├─ DocumentRuntime* current             ← 显式句柄，不再靠"最后 activate 的是谁"
 │    ├─ document / docObj / documentBound
 │    ├─ realm（这个文档拥有的 realm；顶层=mainRealm，iframe=自己的 realm）
 │    ├─ listeners / cssomRefs / slot
 │    └─ generation（S2：文档级资源 id）
 ├─ frameRealms: 本 view 的表            ← S0 已做
 └─ busyContext (原子)                   ← "引擎线程最后进入的上下文"，跨线程可读
```

**四条不变量**

1. **一个 document = 一个 realm**（顶层文档要 mainRealm，iframe 文档要自己的 realm）。
   共享 realm 模式（`AGB_PER_FRAME_REALM=0`）是过渡开关，用 slot 在多个文档间切。
2. **`(engine, realm) → DocumentRuntime*` 是唯一的上下文查找**。ant 每次进 realm 是它自己的事，
   宿主侧靠这个映射恢复"当前文档"，而不是靠"上一次 activate 留下的是谁"。
3. **"谁在跑"是可读身份**：`agb_busy_js_context()`（S0）。看门狗/UI 线程读它做归属，
   不再广播。
4. **进程级表一律按 engine 归属**（桥的 wrapper/class/method 表已如此）。这是以后开线程的前置。

## 3. 字段归属表

| 字段（现状位置） | 归属 | S1 之后 |
|---|---|---|
| `engine` | view | `ViewRuntime::engine` |
| `nodeClass` / `windowClass` / `canvasCtxClass` / `styleSheet*` / `cssRule*` / `mediaList` / `cssDeclaration` | view（engine 级，realm 共享） | 不动 |
| `hidden` / `viewHidden` | view | 不动 |
| `pinsPendingRelease` / `polyfillsGeneration` | view | 不动（S2 视需要再拆） |
| `document` / `docObj` / `documentBound` | **document** | `DocumentRuntime` |
| `mainDocument` / `mainDocObj` / `boundRealm` | **document**（R130 补丁） | **已删除**：realm→document 由映射决定 |
| `listeners` | **realm**（一次 reload 会 bind 两个文档，监听器属于 window） | `RuntimeState::realmListeners`（`map<realm, vector>`） |
| `cssomRefs` / `cssomSeen` | **document**（realm 销毁时释放） | `DocumentRuntime` |
| `docSlots` / `docSlotCount` / `currentSlot` | view（`__agbBind` 编号空间） | 不动，slot → `DocumentRuntime*` |
| `generation` | view（现在） | S2 移到 document |

## 4. 入口清单（必须激活上下文的地方）

引擎线程每次**从宿主进入页面**都必须落到正确的 `DocumentRuntime`：

- `contextActivate(engine)`：桥的每个 native 回调之前（`antb_set_context_activate`），
  S1 起用 `(engine, antb_realm_current(engine))` 查表。
- `bindDocument()` / `bindFrameRealm()`：文档挂载 / 换 realm。
- `globalActivateSlot()`（`__agbBind`）：定时器 / XHR / 事件回调回到自己的 realm。
- `activateSlot()`：共享 realm 模式下按 slot 切文档。
- `beginNewRealm()` / `teardownRealm()` / `stopRealm()` / `destroyContext()`：生命周期。
- `pumpAsyncRequests()` / `antbEvalPage()` / 宿主 eval：求值前。

新增入口必须同时更新本清单（这是 S0 的执行契约）。

## 5. 迁移步骤与验收

### S0（已完成，低风险）

1. **frame realm 表按 view**：`g_frameRealms`（文件级）→ `RuntimeState::frameRealms`。
   修掉"另一个 tab 导航会清空本 tab 的 iframe realm 映射"。
   验收：A 导航后 B 的 iframe realm 仍在（`AGB_EVT_DEBUG` 的 `frame realm dropped` 只针对 A 的文档）。
2. **`agb_busy_js_context()`**：引擎线程 `contextActivate()` 写入的当前上下文（原子指针），
   任何线程可读；配套 `agb_dump_js_stack_for(ctx, path)`。
   验收：`AGB_WD_DEBUG=1` 下，卡住时打印 `busy ctx=`，且只有该 view 收到通知。
3. **`AGB_CONTEXT_ASSERT=1`**：AntRuntime 入口校验"只允许引擎线程进入"，跨线程时打印
   `[ctx] ... thread=... engine=...`。验收：`agave_embed` 双 view 回归无告警。

### S1（已实现）

1. 新增 `DocumentRuntime`（document/docObj/documentBound/cssom/realm/realmSlot）；每个
   ViewRuntime 持有 `documents` 表 + `activeDocument`。
2. `g_*` 宏分两路：view 级字段读 `g_currentState`，文档级字段读 `g_currentDocument`；
   `activateDocument()` 是唯一写入口，`activateState()` 会恢复 view 的 `activeDocument`。
3. 删除 `applyRealmBinding` 的 R130 补丁逻辑（`mainDocument`/`mainDocObj`/`boundRealm`）：
   realm→document 由 `documentForRealm()` 查表给出。
4. **window 监听器按 realm 存，不按 document 存**：一次 reload 会先 bind 临时文档、再 bind
   提交文档（同一个 realm），页面第一段 inline script 注册的 load 监听器必须对两个文档都可见；
   iframe 各自的 realm 天然隔离（R132）。
5. 验收：
   - `run_realm_tests.ps1`、`run_hang_recovery_tests.ps1`、`run_tab_lifecycle_tests.ps1`、
     `run_worker_tests.ps1`、`run_xhr_tests.ps1`、`run_nav_tests.ps1` 等套件全绿；
   - `AGB_PER_FRAME_REALM=1` 与 `=0` 两种模式都过同一组用例；
   - 10 次 hammer reload 后 `window event load ... listeners=0` 为 0（reload chain 用例）。

### S2（之后）

- 模块 map / 定时器归属 / cancellation 全部按 `DocumentRuntime`（generation 变文档级）。
- 评估"顶层导航不换 engine"：arch-status 写明 per-document engine 是 GC root 的替代（R4/R67），
  要换得先让 ant 支持按 realm 释放 root；否则保持现状。

### S3（可放弃）

- 帧泵隐藏 view 不画；`AGB_PER_VIEW_THREAD=1` 做 per-view 引擎线程。
- 前置：S1 完成、桥表按 engine 分片、`g_engines[]` 加锁（roadmap P2 中断收口）。

## 6. 迁移注意

- **不手工改 1103 处 `g_*`**：宏改指向 `g_currentDocument`，只在热点入口（dispatch/事件/定时器）
  逐步改成显式参数。
- **`g_currentDocument` 永远非 NULL**：指向 `g_emptyDocument`（其 `view` 为 `g_emptyState`），
  这样未绑定文档的早期调用读到的是空值而不是崩溃。
- **engine 生命周期不动**：S1 只把"文档"从 view 里拆出来，不改变"每顶层文档一个 VM"。
- **R130 的补丁要删干净**：留着两套绑定逻辑会互相打架。

## 7. 不做

- 不重写 WebCore / 不换 JS 引擎。
- 不为并行而并行：进程-per-tab 只在需要崩溃隔离时考虑（roadmap P2 第 2 条）。
- 不把看门狗中断当架构解：它只是 S3 之前的止血，导航打断那条必须保留。
