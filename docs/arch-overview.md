# 架构总图（当前实现）

> 状态：**当前态，最后核对 2026-11-05**。本文只回答"现在由什么组成、边界在哪、谁拥有什么"。
> 能力与限制见 [arch-status.md](arch-status.md)；待办见 [plan-roadmap.md](plan-roadmap.md)；
> 设计过程见 [design-per-view-js-context.md](design-per-view-js-context.md)、
> [design-frame-window.md](design-frame-window.md)（**设计记录，不是现状**）；
> 历史取舍见 [hist-lessons.md](hist-lessons.md)；任务路由见 [dev-onboarding.md](dev-onboarding.md)。

## 1. 运行拓扑

一个进程、三类线程、两个 DLL（`libagave.dll` → `antjsb.dll`，另加宿主 exe）。

```text
宿主 UI 线程                       引擎线程（每进程一条）                网络线程（每请求一条）
────────────                       ─────────────────────                 ─────────────────────
窗口/消息循环                       macross_initialize（一次）
  │ 输入、标签页、菜单               │
  ├─ agave_webview_*（入队） ──────► 命令队列（256 槽）
  │                                ├─ macross_event_dispatch()
  │                                ├─ macross_view_update()
  │                                │    ├─ paintView()
  │                                │    └─ pumpAsyncRequests() ◄────────── WinHttpSession::threadEntry
  ◄── PostMessage(AGW_MSG_*) ──────┘    └─ 页面 JS / 布局 / 绘制
      状态、重绘、下载回调
```

- **用控件的宿主永远不直接调 `macross_*`**：`agave_webview_navigate/eval/set_zoom` 等都是入队操作，任何线程可调；
  每个控件有自己的 `MaCrossView`、DIB、标题/进度和命令路由（`src/webview_control/AgaveWebView.c`）。
  （`agave_view_host` 是例外：它不建控件，直接驱动 MaCross，用于无窗口出图。）
- **JS 只在引擎线程执行**，且只在解析、事件派发、帧泵这些调用栈里执行。网络完成回调只把结果入队，
  绝不直接在网络线程跑页面 JS（见 [arch-module-loading.md](arch-module-loading.md)）。
- **回调线程**：标题/进度/对话框在引擎线程触发；下载状态是例外——控件深拷贝后 `PostMessage`，
  在 UI 线程派发。宿主不要在回调里做同步跨线程等待（契约见 `include/agave_webview.h`）。
- 一个进程的所有控件**共用一条引擎线程**，所以多个控件不会真正并行；但每个控件的 JS 上下文是独立的。
  具体后果（真实行为，不是"理论上"）：①一个 tab 的长脚本会挡住其他 tab 的命令（点击/刷新/导航）**和**
  它们的定时器/XHR 回调 —— 唯一出路是看门狗的三个判据（VM 里有没有 JS 在栈上 + 谁在等 + 命令是否 drain，见 [dev-debugging.md](dev-debugging.md) 的"真卡/慢"表 → `antb_interrupt_all_engines()`）；
  ②帧泵对**每个** view 依次 `macross_view_update()`（画 + 泵定时器），不分可见与否，所以 N 个 tab
  时每个 tab 每次都要画一遍（隐藏 tab 的绘制是纯浪费，见 [plan-roadmap.md](plan-roadmap.md)）；
  ③重 tab 会拖慢其他 tab 的帧率（同一条循环）。
  想真正并行只有两条路（都不是"加个线程"）：桥/运行时的**进程级单线程假设**要先拆掉
  （`g_currentState` 当前 realm、`g_wrappers`/`g_classes`/trampoline 表、ant 的 interned 字符串表都是无锁全局），
  或者改成**每 tab 一个子进程**（site-per-process 的简化版，`agave_view_host` 已经是现成的无控件宿主）。
  评估见 [plan-roadmap.md](plan-roadmap.md) 的 "P2 多 tab 并行"。

## 2. 代码层

| 层 | 位置 | 职责 |
|---|---|---|
| 宿主壳 | `examples/browser_shell/main.c` | 标签页/地址栏/查找/书签；`--probe` 真无头探针。只做 UI，不碰引擎 |
| Win32 控件 | `src/webview_control/AgaveWebView.c` | 控件窗口、引擎线程、`AGW_CMD_*` 队列、剪贴板/IME、下载表；编进 `libagave.dll` |
| 视图适配 | `src/webview/` | MaCross `Page`/`Frame`/`WebView`、帧泵、`FrameLoaderClientMC`（willClose/commit/DCL/load）、`DownloadManager`、内部页、`AgbLog.h` |
| WebCore 移植 | `src/webcore/` | WebKit 2008：HTML/CSS 解析、DOM、布局、绘制、网络 |
| JS 门面 | `src/javascript/ant_backend/AntRuntime.cpp` | 每 view `RuntimeState`、realm 生命周期、DOM 桥外观、XHR/模块/定时器/history、加载 prelude/polyfills/internal.js |
| JS 桥 | `H:\ant-v16\bridge\AntBridge.c` → `antjsb.dll` | JS↔C 唯一漏斗 `agb_dispatch`；wrapper/class 表；桥源码属于 ant 仓库 |
| 运行时 JS | `src/javascript/ant_backend/prelude.js`、`polyfills.js`、`internal.js` | 从 exe 旁加载，可热改；`internal.js` 只给 `agave://` 页 |
| ant 引擎 | `H:\ant-v16`（独立仓库，`agave-bridge`） | silver VM/编译、ESM、Worker、wasm；见 [ref-ant-engine.md](ref-ant-engine.md) |
| 平台底座 | `src/wtf/`、`src/core/`、`include/` | 容器、字符串、断言、公共头 |

WebCore 内部的渲染管线（DOM → 样式 → 渲染树 → 布局 → 绘制）见 [arch-rendering.md](arch-rendering.md)。

### 2.1 三层：内核 / 引擎 / 运行时 JS

```text
宿主 exe
  ├─ libagave.dll
  │    ├─ 渲染内核（WebCore 移植）      HTML/CSS/DOM/布局/绘制/网络
  │    ├─ 脚本接缝（#if ENABLE(ANTJS)） Frame / tokenizer / event / loader 的少量挂点
  │    └─ AntRuntime（引擎适配层）      realm、桥外观、模块/XHR/定时器
  ├─ antjsb.dll（由 libagave 加载）     VM + JS↔C 桥（源码在 ant 仓库）
  └─ prelude.js / polyfills.js（由 AntRuntime 按 realm 读取）  把桥补成 Web API
```

- **依赖方向单一**：内核 → 接缝 → 适配层 → 引擎。内核不在普通代码里 include `AntRuntime`，
  只在 `#if ENABLE(ANTJS)` 守卫内引用；把 `OPT_JS_ANT` 关掉时，脚本入口落到 `OPT_USE_KJS`
  （默认 ON）的 2008 KJS 实现。
- **引擎是构建期选项**：`OPT_JS_ANT` 决定是否编 `AntRuntime.cpp`、是否链 `antjsb.dll`
  （[src.cmake](../src/src.cmake)）；不是运行期可插拔的 DLL 插件。
- **运行时 JS 是资产**：`prelude.js` / `polyfills.js` 从 exe 旁按 realm 加载，可独立替换/裁剪；
  `internal.js` 只注入 `agave://` 内部页。它们不参与链接，也不属于内核。
- **能力随层叠加**：内核层 = 文档能解析/排版/绘制；引擎层 = 脚本能执行、DOM 能操作；
  运行时 JS 层 = `fetch`/XHR/定时器/console/computed style 等 Web API 表面。
- **换引擎的改动面**收在接缝与适配层：桥的 `antb_*` 契约见 [ref-ant-engine.md](ref-ant-engine.md)，
  适配层职责见 [dev-onboarding.md](dev-onboarding.md) §3。

## 3. realm、所有权、进程级共享

- **每个 view 一个 `RuntimeState`**（`AntRuntime::createContext()`，在 `macross_view_create` 时建；
  从 C API 进入引擎线程时 `activateContext()` 切回自己的状态）。
- **每个 frame 文档一个 realm**（`docs/design-per-frame-realm.md`，默认开启）：主 frame 的文档用
  engine 创建时的主 realm，**每个 iframe 文档首次 bind 时建自己的 realm**（独立全局、独立 intrinsics、
  独立 wrapper），`AGB_PER_FRAME_REALM=0` 回退到"共享 realm + per-frame window 代理"的旧模型。
  顶层文档换文档时 `beginNewRealm()` 新建 `AntbEngine`（新 realm）；iframe 掉帧时
  `detachedFromParent3` → `forgetFrameRealm()` 释放该 realm 的 wrapper 并让 GC 回收。
- **generation**：进程级递增的资源 id。所有可能活过文档的资源（XHR、模块下载、待执行模块、
  DOM pin、文档 slot）都带它；卸载时按它精确取消。realm 与 generation 目前不是一一对应：
  一个 view 的 generation 对应顶层文档，iframe 的 realm 共用 view 的 generation（S3 待细化）。
- **owner**：桥接 wrapper 的归属文档。文档卸载时按 owner 释放（`antb_release_owner`），避免下一个文档拿到旧节点。
- **slot**：同一 engine 内多个文档（iframe）的 DOM 上下文编号；从定时器/XHR/事件回调进入前
  `activateSlot()` 切回对应文档。
- **frame 与 window**：每个 realm 的 `window`/`self` 就是该 realm 的全局对象；`parent`/`top` 是
  父/顶层 realm 的全局对象（同 VM，属性访问和调用照常，`parent !== self`、`top.Array !== Array`）。
  ES module map 仍是引擎级（两个 frame import 同一模块共享实例，S3 待做）。
- **进程级共享**：模块会话（`g_moduleSession`）、`DownloadManager` 单例、generation 计数器、
  桥的 wrapper/class/method-trampoline 表（wrapper 按 `(js, realm, ptr, tag)` 存，class 和
  method 表按 engine 归属）。**契约**：任何进程级桥表在新增条目时，`antb_engine_destroy()`
  必须释放该 engine 的条目（wrapper/class 已这样做；method 表此前漏了，见 R77），否则
  “每文档一个新 engine”的隔离会被表容量上限击穿，而不是被 JS 状态泄漏击穿。

## 4. 一次导航

```text
navigate → commit → beginNewRealm（换文档即新 generation；通常也换新 engine）
  → bindDocument：装 shim → prelude.js → polyfills.js →（内部页再装 internal.js + __agbDownload*）
  → 解析 HTML
       ├─ 经典 script     → 解析调用栈内求值
       ├─ 入口 module     → 单槽等待位挂起解析 → body 到达后回到解析调用栈求值
       └─ defer/module 队列 → 解析结束 flushDeferredModules()
  → DOMContentLoaded（只发一次；deferred module 先跑）→ load
  → willClose → documentWillUnload → stopRealm
```

模块管线的完整调用栈（三条泳道 + 代码锚点）见 [arch-module-loading.md](arch-module-loading.md)；
DCL 双发与模块执行顺序的待办见 [plan-roadmap.md](plan-roadmap.md) P0-c。

## 5. 下载与内部页

- **下载决策点唯一**：`FrameLoaderClientMC::dispatchDecidePolicyForMIMEType()`；
  `attachment` 或不可显示 MIME → `PolicyDownload`（MIME 为空则放行给内容嗅探）。
  `ResourceLoader::detachHandle()` 把连接所有权交给进程级 `DownloadManager`，先写 `.part` 再 rename。
- **线程模型**：`cb_download` 在引擎线程 → 控件深拷贝进 32 槽定长表 → `PostMessage` → UI 线程派发；
  取消/改目录走命令队列回到引擎线程。详见 [arch-downloads.md](arch-downloads.md)。
- **内部页**：`agave://downloads` 由 `WebView.cpp` 用 `SubstituteData` 供给，仍是普通文档 + 独立 realm；
  `internal.js` 和 `__agbDownload*` 只在 `agave://` realm 注册，普通页面 `typeof agave === 'undefined'`。

## 6. 不能违反的边界

1. 页面 JS 只在引擎线程执行；网络线程/UI 线程不得直接进 JS。
2. `prelude.js` / `polyfills.js` 从 exe 旁加载；改完拷到 `build-zig-native\` 即生效，不用重链。
3. 桥源码在 `H:\ant-v16\bridge\AntBridge.c`；改完跑 `tools/build_antjsb.ps1` 重建 `antjsb.dll`（[ref-ant-engine.md](ref-ant-engine.md)）。
4. 全局上的 `window`/`document` 等必须是**数据属性**；ant 的裸标识符查找不走 exotic getter（R31/R33）。
5. 一个 frame 只有一份文档（R44）；iframe 的 DCL 仍不单独派发（见 `FrameLoaderClientMC.cpp` 注释）。
6. 控件回调里不要同步跨线程等待；销毁与引擎线程的 `macross_view_update` 有竞态，按控件文档的规则做。
