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
  它们的定时器/XHR 回调 —— 出路是看门狗的三个判据（VM 里有没有 JS 在栈上 + 谁在等 + 命令是否 drain，
  见 [dev-debugging.md](dev-debugging.md) 的"真卡/慢"表）：**有人等**（导航/停止/查找）→
  `antb_interrupt_all_engines()` 中断；**没人等**（页面无响应）→ 只报告（tab 级提示 + 每事件一份
  `agb_busy_*.txt` 带 JS 栈），不杀页面；
  ②帧泵对每个 view 依次更新：可见的走 `macross_view_update()`（布局 + 绘制 + 泵定时器），不可见的
  （后台 tab / 最小化窗口，Page Visibility 由宿主给出）只走 `macross_view_pump()`（标题 + 异步工作），
  不布局不绘制，回到可见时整幅重绘一次（R150）；
  ③重 tab 会拖慢其他 tab 的帧率（同一条循环）。
  想真正并行：**渲染后端可插拔**（local / Win32 子进程 / RVVM 实例，共用一个"渲染后端协议"），
  `AGB_VIEW_BACKEND=process` 时每个 view 是一个独立的 `agave_render_host.exe`：控件只发命令/收事件，
  像素走命名共享段（canvas + damage），渲染进程崩溃由控件检测并报 load error。命令文本有两条路：
  ≤16 KB 走 64 KB 的 SPSC 文本 arena（带读写指针 + 背压），更大的载荷（注入脚本、大 HTML）走
  **一次性命名共享段**（大小按载荷精确创建），两边都不静默截断。见
  [design-render-backend.md](design-render-backend.md) §4.1.1；每 view 一条引擎线程因为要拆
  桥/运行时的进程级无锁全局（含 WebCore 的全局 `AtomicString` 表），已降级为备选。
  待办与验收见 [plan-roadmap.md](plan-roadmap.md) 的 "P2 多 tab 并行"。
- **宿主壳是单实例（Chrome 模型，2026-11-11）**：一个 profile 根一把命名互斥体
  （`Local\agave_shell_profile_<hash>`）+ 一个隐藏窗口（类 `AgaveShellHandoff`，标题带同一个
  hash）。第二次启动不建窗口，把 `WM_COPYDATA` 请求交给已有壳（`--new-tab`/裸 URL → 最近活跃
  窗口加 tab；`--new-window` → **同一进程**再开一个窗口；无 URL → 只激活），拿到 ack 后退出。
  壳进程内最多 `MAX_SHELL_WINDOWS` 个窗口，每个窗口有自己的 tab 列表/工具栏/缩放/查找状态
  （`ShellWindow` + UI 线程上下文 `g_cur`），关掉最后一个窗口才退出；session 扁平保存打开的
  窗口的 tab。`--probe` 不建窗、不参与单例。设计、协议与边缘情况见
  [design-shell-singleton.md](design-shell-singleton.md)；回归 `tools/run_singleton_tests.ps1`
  （`--probe` + 文件 mock，只测身份/决策/ack 逻辑，约 2 s；真实窗口路径手动验证）。

## 2. 代码层

| 层 | 位置 | 职责 |
|---|---|---|
| 宿主壳 | `examples/browser_shell/`：`main.c`（标签/会话/单实例/探针）、`ui_chrome.c`（工具栏/查找/状态栏组合）、`ui_widgets.c`（按钮/标签条）、`ui_theme.c`（GDI+ 主题/图标/DPI）、`shell_ops.c`（OS 缝合口） | 标签页/地址栏/查找/书签；单实例接力（第二次启动只投递 URL）与同进程多窗口；`--probe` 真无头探针、`--ui-shot` 离屏 chrome 回归。只做 UI，不碰引擎；设计见 [design-shell-ui.md](design-shell-ui.md) |
| Win32 控件 | `src/webview_control/AgaveWebView.c` | 控件窗口、引擎线程、`AGW_CMD_*` 队列、剪贴板/IME、下载表；编进 `libagave.dll` |
| 渲染后端 seam | `src/webview_control/AgwViewBackend.h/.c`、`AgwViewBackendProcess.c` | 控件与"谁在跑页面"之间的接口（`local` = 进程内 macross；`process` = 每 view 一个 `agave_render_host.exe`，协议 `AgwViewIpc.h`）；`AGB_VIEW_BACKEND` 选后端，设计见 [design-render-backend.md](design-render-backend.md) |
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
- **每个文档一个 `DocumentRuntime`**（S1，[design-context-ownership.md](design-context-ownership.md)）：
  文档对象、`docObj`、CSSOM refs、它自己的 realm 都在这里；`RuntimeState::documents` 是该 view
  的文档表，`activeDocument` 是"切回这个 view 时要恢复的文档"。`g_*` 宏按归属分流：view 级读
  `g_currentState`，文档级读 `g_currentDocument`；`activateDocument()` 是唯一写入口。
  这取代了 R130 的 `mainDocument`/`mainDocObj`/`boundRealm` 补丁：realm→document 是查表。
- **每个 frame 文档一个 realm**（`docs/design-per-frame-realm.md`，默认开启）：主 frame 的文档用
  engine 创建时的主 realm，**每个 iframe 文档首次 bind 时建自己的 realm**（独立全局、独立 intrinsics、
  独立 wrapper），`AGB_PER_FRAME_REALM=0` 回退到"共享 realm + per-frame window 代理"的旧模型。
  顶层文档换文档时 `beginNewRealm()` 新建 `AntbEngine`（新 realm）；iframe 掉帧时
  `detachedFromParent3` → `forgetFrameRealm()` 释放该 realm 的 wrapper 并让 GC 回收。
  frame realm 表属于 view（`RuntimeState::documents` 里 `realm != NULL` 的记录），不再进程级共享。
- **document-scoped window 对象**（`RealmDocument.cpp`，R132）：`location`/`history` 各自带
  document/Frame，getter 从 receiver 解析文档，不再读 per-view 的 `g_document`；
  `document.location === window.location`，子 frame 读自己的 URL/历史，`parent.location` 读父文档。
- **generation 有两条**（S2）：`RuntimeState::generation` 是 realm 生命周期 id（pin、polyfills、
  整 realm 取消），`DocumentRuntime::generation` 是文档 id。异步工作（XHR、module fetch、排队 import、
  deferred script）两个都记：整个 realm 离场按 realm id 取消，一个 frame 文档离场按文档 id
  精确取消它自己启动的工作（`cancelDocumentResources()`）。
- **owner**：桥接 wrapper 的归属文档。文档卸载时按 owner 释放（`antb_release_owner`），避免下一个文档拿到旧节点。
- **监听器按 realm**：`window.addEventListener` / `onxxx` 存在 `RuntimeState::realmListeners`
  （`map<realm, vector>`）。一次 reload 会先 bind 临时文档、再 bind 提交文档（同一 realm），页面第一段
  inline script 注册的 load 监听器必须对两者都可见；iframe 的 realm 各自一份，天然隔离（R132）。
- **"谁在跑"可读**：`agb_busy_js_context()` 返回引擎线程最后进入的 `RuntimeState*`（跨线程原子读），
  看门狗据此把"页面无响应"归属到具体 view（`macross_view_js_context()` 对拍），不再广播给所有 view。
- **slot**：同一 engine 内多个文档（iframe）的 DOM 上下文编号；从定时器/XHR/事件回调进入前
  `activateSlot()` 切回对应文档。
- **frame 与 window**：每个 realm 的 `window`/`self` 就是该 realm 的全局对象；`parent`/`top` 是
  父/顶层 realm 的全局对象（同 VM，属性访问和调用照常，`parent !== self`、`top.Array !== Array`）。
  **ES module map 也按 realm**（P1-f）：`js->modules` 在 realm 镜像表里，两个 frame import 同一 URL
  各求值一次、各得自己的 namespace（HTML 规范：module map 属于 environment settings object）。
- **进程级共享**：generation 计数器、桥的 wrapper/class/method-trampoline 表（wrapper 按
  `(js, realm, ptr, tag)` 存，class 和 method 表按 engine 归属）。模块调度（fetch session、
  pending、routed entry、deferred 队列、body 缓存）**按 view**（S2，roadmap P1-a）；
  ES module registry 仍按 engine（frame 的 ESM 入口/动态 import 尚不可用，见设计文档 S2 未做）。
  **契约**：任何进程级桥表在新增条目时，`antb_engine_destroy()` 必须释放该 engine 的条目
  （wrapper/class 已这样做；method 表此前漏了，见 R77），否则“每文档一个新 engine”的隔离会被
  表容量上限击穿，而不是被 JS 状态泄漏击穿。

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