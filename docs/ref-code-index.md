# 代码位置索引（函数 / 机制锚点）

> 用途：从"机制名"快速跳到函数。文件级"我要改 X 去哪"见 [dev-onboarding.md](dev-onboarding.md) §5；
> 能力缺口见 [arch-capability-gaps.md](arch-capability-gaps.md)。

| 主题 | 位置 |
|---|---|
| realm 生命周期 | `AntRuntime.cpp`：`stopRealm()` / `beginNewRealm()` / `documentWillUnload()`；`createContext()`（每 view 一个） |
| generation / 在途资源 | `AntRuntime.cpp`：`currentGeneration()` / `cancelInFlightResources()`；XHR `XhrJob::m_generation`、模块 `ModuleEntry::generation`、延迟模块 `DeferredModule::generation` |
| DOM pin / 文档 slot | `AntRuntime.cpp`：`releasePinnedNodes()` / `wrapNode()` / `slotForDocument()` / `activateSlot()` |
| 模块调度 | `AntRuntime.cpp`：`moduleStart()` / `moduleFetch()`（同步等待）/ `modulePrefetchImports()`（字符串扫描）/ `queueDeferredScript()` / `flushDeferredModules()` |
| 事件处理属性（`el.on*`） | `AntRuntime.cpp`：`nodeGetter`/`nodeSetter` 的 `on*` 分支 + `setNodeEventHandler()` / `findNodeEventHandler()`；回归 `tools/pages/cap_script.html`（R27） |
| 能力缺口（JS/DOM/CSS） | [arch-capability-gaps.md](arch-capability-gaps.md)（实测值 + 一条命令复测）；探测页 `tools/pages/cap_*.html` |
| 301/302 重定向 | `WinHttpSession.cpp`（`Request::followRedirects=false` → `WINHTTP_OPTION_REDIRECT_POLICY_NEVER`）；`ResourceHandleNet.cpp` `onResponse()`（3xx → `willSendRequest` → `ResourceHandleManager::redirect()`）；回归 `tools/nav_redirect_server.py` |
| 入口模块时序（三条泳道图） | [arch-module-loading.md](arch-module-loading.md) |
| 桥接释放 | `H:\ant-v16\bridge\AntBridge.c`：`antb_wrap_native_owner` / `antb_release_owner` / `agbReleaseEngineWrappers` / `agbReleaseEngineClasses` / `antb_engine_destroy` |
| 页面时序挂点 | `FrameLoaderClientMC.cpp`：`dispatchWillClose` / `committedLoad` / `dispatchDidFinishDocumentLoad` / `dispatchDidFinishLoad` |
| 默认日志收口 | `src/webview/AgbLog.h`：`agbLog(channel, fmt, ...)` + `agbLogElapsedMs()`；通道 `evt/nav/ui/shell/probe/flag`，时间原点跨 DLL 共享；格式 `[<ms>ms] [tag] 正文` |
| `[nav]` 导航日志 | 钩子在 `FrameLoaderClientMC.cpp`（request/new-window/submit/redirect/fail/blocked）、`WebEngine.cpp`（`macross_view_open_url/back/forward/reload/stop`）、`AgaveWebView.c`（popup/download）；开关 `AGB_NAV_LOG=0` |
| `[nav]` 回归 | `tools/run_nav_tests.ps1` + `tools/pages/navcheck_*.html` + `tools/nav_redirect_server.py` |
| WebCore 侧限制 | `src/webcore/loader/FrameLoader.cpp`（unload 门禁）、`src/webcore/dom/EventTargetNode.cpp`（事件转发边界）、`src/webcore/dom/Document.cpp`（`finishedParsing` 的 DCL） |
| 地址栏输入 → URL | `examples/browser_shell/main.c`：`parseShellArgv()` / `normalizeUrl()` / `fileUrlFromLocalPath()`；`file:` URL 只允许单次转义 → `KURL::encode_string()`（幂等） |
| 无头探针 | `examples/browser_shell/main.c`：`probeRun()`（`WS_POPUP` + `WS_EX_TOOLWINDOW` 隐藏父窗口）+ `probeWndProc()`；`AGB_PROBE_SHOW=1` 才显示 |
| Web Worker | `prelude.js` 末尾「Web Workers」段（`_agbWorkerStart` / `_agbWorkerPreamble` / `_agbWorkerPrefetch`）；ant 侧 `H:\ant-v16\src\modules\worker\{web,core}.c`；栈帧短名 `esm_frame_filename`（`H:\ant-v16\src\esm\loader.c`） |
| 回归页 / 脚本 | 页面在 `debug/`；`tools/run_worker_tests.ps1`、`tools/run_nav_tests.ps1`、`tools/run_typing_tests.ps1`、`tools/check_window_visible.ps1`、`tools/bench_compile.ps1`、`tools/measure_cpu.ps1` |
| 键盘文本输入 | `src/webview/WebView.cpp`：`keyDown()`（只派发 keydown，记住 `preventDefault`）、`textInput()`（WM_CHAR：过滤控制字符 → `handleCharacterEvent`）；`src/webcore/page/EventHandler.cpp`：`keyEvent()` / `handleCharacterEvent()`（keypress + 默认插入）；`src/webview/KeyEventMC.cpp`：`PlatformKeyboardEvent`（字符不来自 vk）；回归 `tools/run_typing_tests.ps1`（R37） |
| 输入法 / Unicode 输入 | `src/webview_control/AgaveWebView.c`：`agwRegisterClass()`（`RegisterClassW`，Unicode 窗口）、`agwUtf16ToUtf8()`（代理对配对）、`agwProc` 的 `WM_CHAR`/`WM_IME_CHAR`；宿主循环 `GetMessageW`/`DispatchMessageW`；死循环与光标：`src/webcore/platform/unicode/TextBoundary.cpp`、`src/webcore/rendering/RenderText.cpp`；回归 `--ime` 三例（R38） |
