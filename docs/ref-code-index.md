# 代码位置索引（函数 / 机制锚点）

> 用途：从"机制名"快速跳到函数。文件级"我要改 X 去哪"见 [dev-onboarding.md](dev-onboarding.md) §3；
> 能力缺口见 [arch-capability-gaps.md](arch-capability-gaps.md)。

| 主题 | 位置 |
|---|---|
| realm 生命周期 | `AntRuntime.cpp`：`stopRealm()` / `beginNewRealm()` / `documentWillUnload()`；`createContext()`（每 view 一个） |
| 渲染管线（阶段 → 文件） | [arch-rendering.md](arch-rendering.md)：DOM→样式→渲染树→布局→绘制，含布局内部与症状→文件 |
| 布局入口 / 脏标记 | `FrameView.cpp`：`layout()` / `scheduleRelayout()` / `layoutIfNeededRecursive()`；`Document.cpp`：`updateLayout()` / `recalcStyle()`；`RenderObject.cpp`：`setNeedsLayout()` / `markContainingBlocksForLayout()` |
| 窗口 resize / 视口语义 | 链路：`AgaveWebView.c` `WM_SIZE` → `AGW_CMD_RESIZE` → `macross_view_resize` → `WebView::resizeView()`（只 `FrameView::resize`）→ `FrameView::layout()` 开头的 `styleViewportSize` 钩子 → `Document::viewportSizeChanged()` + `updateStyleIfNeeded()`；vw/vh：`CSSParser::rewriteViewportUnits()`（写成 `var(--vwunit-…)`）+ `Document::viewportCustomProperties()`（按 FrameView 尺寸给 px，`calc` 走 `addViewportCalc()` 模板）；事件：`Frame::sendResizeEvent()` + `AntRuntime::dispatchResizeEvent()`；回归 `tools/pages/resize/` + `run_layout_tests.ps1`（R69） |
| 布局核心 | `RenderView.cpp`：`layout()`；`RenderBlock.cpp`：`layoutBlock()` / `layoutBlockChildren()` / `layoutPositionedObjects()`；`bidi.cpp`：`layoutInlineChildren()`；`RenderBox.cpp`：`calcWidth()` / `calcHeight()`；flex `RenderFlexibleBox.cpp`：`layoutHorizontalBox()` / `layoutVerticalBox()`；表格 `RenderTable.cpp`：`layout()` |
| 几何读取 | `Element.cpp`：`offsetWidth()` / `clientWidth()` / `scrollWidth()`（先 `updateLayoutIgnorePendingStylesheets()`）；`AntRuntime.cpp`：`nodeGetBoundingClientRect()` / `globalLayoutInfo()`（`__agbLayoutInfo`） |
| generation / 在途资源 | `AntRuntime.cpp`：`currentGeneration()` / `cancelInFlightResources()`；XHR `XhrJob::m_generation`、模块 `ModuleEntry::generation`、延迟模块 `DeferredModule::generation` |
| DOM pin / 文档 slot | `AntRuntime.cpp`：`releasePinnedNodes()` / `wrapNode()` / `slotForDocument()` / `activateSlot()` |
| 模块调度 | `AntRuntime.cpp`：`moduleStart()` / `moduleFetch()`（同步等待）/ `modulePrefetchImports()`（字符串扫描）/ `queueDeferredScript()` / `flushDeferredModules()` |
| 事件处理属性（`el.on*`） | `AntRuntime.cpp`：`nodeGetter`/`nodeSetter` 的 `on*` 分支 + `setNodeEventHandler()` / `findNodeEventHandler()`；回归 `tools/pages/cap_script.html`（R27） |
| 能力缺口（JS/DOM/CSS） | [arch-capability-gaps.md](arch-capability-gaps.md)（实测值 + 一条命令复测）；探测页 `tools/pages/cap_*.html` |
| 301/302 重定向 | `WinHttpSession.cpp`（`Request::followRedirects=false` → `WINHTTP_OPTION_REDIRECT_POLICY_NEVER`）；`ResourceHandleNet.cpp` `onResponse()`（3xx → `willSendRequest` → `ResourceHandleManager::redirect()`）；回归 `tools/nav_redirect_server.py` |
| 入口模块时序（三条泳道图） | [arch-module-loading.md](arch-module-loading.md) |
| 桥接释放 | `H:\ant-v16\bridge\AntBridge.c`：`antb_wrap_native_owner` / `antb_release_owner` / `agbReleaseEngineWrappers` / `agbReleaseEngineClasses` / `antb_engine_destroy` |
| 卡死页恢复（中断 + 重载） | `src/webview_control/AgaveWebView.c`：`agwPost()`（唯一排队口，导航/执行类命令先中断）、`agwEngineBusyFor()`（进程级 busy）/ `agwEngineStuckFor()`（引擎线程心跳 `g_enginePumpTick`，>500 ms 未泵消息即卡死）、`agave_webview_interrupt()` / `agave_webview_busy()`；桥 `antb_interrupt_all_engines()`（`bridge/AntBridge.c`）；引擎侧 `js_interrupt_pending` + `sv_jit_arm_interrupts()` + `js_interrupt_raise()`；壳 `--interrupt-after`、`shellAnnounceCrashFile()` |
| 浏览历史 | `examples/browser_shell/main.c`：`loadHistory()` / `recordHistory()` / `statePath()`（`history.txt` 是追加写，启动时读回并按连续重复去重） |
| 页面时序挂点 | `FrameLoaderClientMC.cpp`：`dispatchWillClose` / `committedLoad` / `dispatchDidFinishDocumentLoad` / `dispatchDidFinishLoad` |
| 默认日志收口 | `src/webview/AgbLog.h`：`agbLog(channel, fmt, ...)` + `agbLogElapsedMs()`；通道 `evt/nav/ui/shell/probe/flag`，时间原点跨 DLL 共享；格式 `[<ms>ms] [tag] 正文` |
| `[nav]` 导航日志 | 钩子在 `FrameLoaderClientMC.cpp`（request/new-window/submit/redirect/fail/blocked）、`WebEngine.cpp`（`macross_view_open_url/back/forward/reload/stop`）、`AgaveWebView.c`（popup/download）；开关 `AGB_NAV_LOG=0` |
| `[nav]` 回归 | `tools/run_nav_tests.ps1` + `tools/pages/navcheck_*.html` + `tools/nav_redirect_server.py` |
| WebCore 侧限制 | `src/webcore/loader/FrameLoader.cpp`（unload 门禁）、`src/webcore/dom/EventTargetNode.cpp`（事件转发边界）、`src/webcore/dom/Document.cpp`（`finishedParsing` 的 DCL） |
| 地址栏输入 → URL | `examples/browser_shell/main.c`：`parseShellArgv()` / `normalizeUrl()` / `fileUrlFromLocalPath()`；`file:` URL 只允许单次转义 → `KURL::encode_string()`（幂等） |
| 无头探针 | `examples/browser_shell/main.c`：`probeRun()`（`WS_POPUP` + `WS_EX_TOOLWINDOW` 隐藏父窗口）+ `probeWndProc()`；`AGB_PROBE_SHOW=1` 才显示 |
| Web Worker | `prelude.js` 末尾「Web Workers」段（`_agbWorkerStart` / `_agbWorkerPreamble` / `_agbWorkerPrefetch`）；ant 侧 `H:\ant-v16\src\modules\worker\{web,core}.c`；栈帧短名 `esm_frame_filename`（`H:\ant-v16\src\esm\loader.c`） |
| 回归页 / 脚本 | 页面在 `debug/`；`tools/run_worker_tests.ps1`、`tools/run_nav_tests.ps1`、`tools/run_typing_tests.ps1`、`tools/run_jit_loop_tests.ps1`、`tools/check_window_visible.ps1`、`tools/bench_compile.ps1`、`tools/measure_cpu.ps1` |
| 控件生命周期（R61） | `src/webview_control/AgaveWebView.c`：`agwDrainCommands()`（命令按 `viewId` 查目标并 `refs++`，跑完 `agwRelease()`）、`agave_webview_destroy()`（`refs>0` 时推迟到引擎线程）、`agwRelease()`；探针侧 `examples/browser_shell/main.c` 的 `__agb_evaldone__` → `AGW_PROBE_EVALDONE` → `--settle` |
| 裸引擎复现（无浏览器） | `tools/ant_loop_repro.c`：`antb_engine_create + antb_eval` 跑任意脚本；用来判定"是不是 ant/JIT 的锅"（R61 用它排除了 JIT） |
| 堆破坏 / UAF 定位 | `AGB_GUARD_MALLOC`（`src/wtf/FastMalloc.cpp`，页守卫 + free 即 unmap）+ `tools/cdb_heap_probe.ps1`（cdb 启动、dump 被写坏的块）；HEAP 消息是**发现点**不是写入点（R62） |
| GC 扎根（ant） | `H:\ant-v16`：callback builtin 用 `gc_temp_root_scope_begin/push`；判据 `ANT_GC_STRESS=1` + `H:\ant-v16\tests\array_callback_gc.cjs`（R63） |
| 键盘文本输入 | `src/webview/WebView.cpp`：`keyDown()`（只派发 keydown，记住 `preventDefault`）、`textInput()`（WM_CHAR：过滤控制字符 → `handleCharacterEvent`）；`src/webcore/page/EventHandler.cpp`：`keyEvent()` / `handleCharacterEvent()`（keypress + 默认插入）；`src/webview/KeyEventMC.cpp`：`PlatformKeyboardEvent`（字符不来自 vk）；回归 `tools/run_typing_tests.ps1`（R37） |
| 输入法 / Unicode 输入 | `src/webview_control/AgaveWebView.c`：`agwRegisterClass()`（`RegisterClassW`，Unicode 窗口）、`agwUtf16ToUtf8()`（代理对配对）、`agwProc` 的 `WM_CHAR`/`WM_IME_CHAR`；宿主循环 `GetMessageW`/`DispatchMessageW`；死循环与光标：`src/webcore/platform/unicode/TextBoundary.cpp`、`src/webcore/rendering/RenderText.cpp`；回归 `--ime` 三例（R38） |
