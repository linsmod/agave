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

## 3. realm、所有权、进程级共享

- **每个 view 一个 `RuntimeState`**（`AntRuntime::createContext()`，在 `macross_view_create` 时建；
  从 C API 进入引擎线程时 `activateContext()` 切回自己的状态）。
- **每个文档一个 realm**：文档提交时 `beginNewRealm()` 接手——首个文档复用 view 创建时建好的
  realm，之后每次换文档 `generation++` 并新建 `AntbEngine`；willClose → `documentWillUnload()` →
  `stopRealm()`（取消在途资源、释放 wrapper/pins/history、销毁 engine）。
  所以"一个 view 在同一时刻只有一个 engine"。
- **generation**：进程级递增的 realm id。所有可能活过文档的资源（XHR、模块下载、待执行模块、
  DOM pin、文档 slot）都带它；卸载时按它精确取消。
- **owner**：桥接 wrapper 的归属文档。文档卸载时按 owner 释放（`antb_release_owner`），避免下一个文档拿到旧节点。
- **slot**：同一 engine 内多个文档（iframe）的 DOM 上下文编号；从定时器/XHR/事件回调进入前
  `activateSlot()` 切回对应文档。
- **frame 与 window**：每个文档有自己的 `document` 对象，每个 frame 有自己的 Window 代理（R31）；
  顶层页的 `window` 仍是全局对象本身，代理的未知属性转发给共享全局。
  **engine 不是一个 frame 一个**，所以顶层 `var x` 仍写进共享全局（已知限制）。
- **进程级共享**：模块会话（`g_moduleSession`）、`DownloadManager` 单例、generation 计数器、
  桥的 wrapper/class 表（表是进程级的，条目随 engine 销毁释放）。

## 4. 一次导航

```text
navigate → commit → beginNewRealm（换文档即新 generation；通常也换新 engine）
  → bindDocument：装 shim → prelude.js → polyfills.js →（内部页再装 internal.js + __agbDownload*）
  → 解析 HTML
       ├─ 经典 script     → 解析调用栈内求值
       ├─ 入口 module     → 单槽等待位挂起解析 → body 到达后回到解析调用栈求值
       └─ defer/module 队列 → 解析结束 flushDeferredModules()
  → DOMContentLoaded（当前会发两次，P0-c）→ load
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
