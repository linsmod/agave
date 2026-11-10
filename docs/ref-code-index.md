# 代码位置索引（函数 / 机制锚点）

> 用途：从"机制名"快速跳到函数。文件级"我要改 X 去哪"见 [dev-onboarding.md](dev-onboarding.md) §3；
> 能力缺口见 [arch-capability-gaps.md](arch-capability-gaps.md)。

| 主题 | 位置 |
|---|---|
| realm 生命周期 | `AntRuntime.cpp`：`stopRealm()` / `beginNewRealm()` / `documentWillUnload()`；`createContext()`（每 view 一个） |
| CSSOM（styleSheets/cssRules） | `AntRuntime.cpp`：`cssomEnsureClasses()`（6 个 class + 共享方法）、`cssomWrap()`/`cssomGetter()`/`cssomMethod()`（live wrapper）、`cssomReleaseRefs()`（`stopRealm`/`destroyContext`/`stop` 里 deref）；`document.styleSheets` 在 `titleGetter`，`style/link.sheet` 在 `nodeGetter`；回归 `tools/pages/cap_cssom.html` + `tools/pages/check_cssom.js` |
| CSS transform（绘制/脏矩形/文字） | `RenderStyle.cpp`：`localTransformMatrix()` / `absoluteTransformMatrix()` / `transformedRect()`；绘制 `RenderBlock.cpp::paint()`（起手 `setMatrix`）；脏矩形 `RenderBox.cpp::computeAbsoluteRepaintRect()`；**文字跟随矩阵**：`src/webcore/platform/picasso/FontPS.cpp::Font::drawGlyphs()`（把线性部分送进 picasso 的 `ps_set_text_matrix`，world matrix 置单位阵）＋ `3rdparty/picasso/src/gfx/gfx_font_adapter_win32.cpp::prepare_glyph()`（advance 过 text matrix）；回归 `tools/pages/layout/transform_repaint.html` / `transform_text.html`（R52/R55/R80） |
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
| 命令投递状态 / 丢弃原因（R163） | `include/agave_webview.h`：`AgwCommandStatus`（`OK`=已入队；`ERR_BAD_ARGS/ERR_NO_VIEW/ERR_QUEUE_FULL`）+ `AGW_REASON_*` 文本常量（控件、渲染进程、宿主共用一份词表）；控件 `AgaveWebView.c`：`agwPost()` 返回状态、`agwReportCommandDrop()` 只收宏、`agave_webview_eval()` 返回状态；渲染进程 `examples/render_host/main.c` 的丢弃事件也发宏；引擎侧 P1-ac：`KJSProxy::evaluate()` 去掉遗留 fallthrough（`#else`）、`DOMWindow::document()` 不 ASSERT、属性 handler 无文档不建 listener；回归 `run_process_backend_tests.ps1` 的 "early eval after load_html" 用例 + `AGB_PROBE_CRASH_EVAL` 探针钩子 |
| 内部页 / 自定义 scheme（R162） | `src/webcore/loader/PageScheme.{h,cpp}`：`registerPageScheme(scheme, factory)`、`pageSchemeSubstituteData(url, out)`（固定表 8 项，大小写不敏感）；接入点 `MainResourceLoader::load()`（在创建 `ResourceHandle` 之前命中就造 `SubstituteData`，所以 navigate/reload/iframe/window.open 一致）；provider `src/webview/InternalPages.{h,cpp}`（`agave://<name>` → exe 旁 `<name>.html`，白名单 `[a-z0-9_-]`；名字从 `url.url()` 里取——`agave://x` 的 `url.path()` 是空的），`macross_initialize()` 注册；`WebView::loadUrl()` 不再有 `agave://` 特判。⚠ `WebCore::Shared` 引用计数从 0 起：用 `RefPtr<T> = new T`，不要 `adoptRef`。回归：`run_nav_tests.ps1` 的 internal page 两例 + `run_process_backend_tests.ps1` 的 internal page reload 例 |
| 右键菜单（R161） | 引擎 `EventHandler::sendContextMenuEvent` → `ChromeClientMC` → `cb_context_menu`；控件 `AgaveWebView.c`：`agwQueueContextMenu()`（拷 payload + `AGW_MSG_CONTEXT` 到 UI 线程）+ `agwOnContextMenu`（本地路径）+ `AGW_VIEW_EVENT_CONTEXT_MENU` 分支（process 路径）；渲染进程 `examples/render_host/main.c` 的 `onContextMenu()`（`cb_context_menu` → 事件，`link+image+text` 按长度拼包）；探针 `--right-click x,y` + `probeOnContextMenu` 的 `[ctx] menu` 行；回归 `run_process_backend_tests.ps1` 的 context menu 用例 |
| 崩溃报告（R160） | `src/webview/AgbCrash.{h,cpp}`：`agbCrashInstall(tag, sink, user)`（VEH + 未处理异常过滤器；**dbghelp 在 install 阶段加载**，模块 PDB 用 `SymLoadModuleEx` 登记，`SymFromAddrW`/`SymGetLineFromAddrW64` 解符号；`full=1` 用 `StackWalk64` 走所有线程）、`agbCrashSetInfo()`（额外上下文行）；共享段通道：`AgwViewIpc.h` 的 `crashLen`/`crashText`，渲染进程 sink 在 `examples/render_host/main.c`；控件：`AgwViewBackendProcess.c` `processCrashReport()` + `AgaveWebView.c` EXIT 分支（先读报告再销毁后端视图）→ `on_crashed(view, code, report, user)`；shell：`onCrashed` 把报告 HTML 转义进 sad tab，探针模式打印 + `AGB_PROBE_CRASH_PAGE` 落盘；测试钩子 `AGB_RENDER_CRASH_AV`（一次性，`--crash-av`）；回归 `run_process_backend_tests.ps1` 的 crash report 用例 |
| 窗口生命周期（R158） | 引擎：`HTMLAnchorElement::defaultEventHandler`（target + 中键/ctrl/cmd→`_blank`）、`FrameLoader::urlSelected`→`load`→`checkNewWindowPolicy`→`createWindow`、`FrameLoaderClientMC::dispatchCreatePage(request, frameName)`、`WebView.cpp` 的 `setGroupName("PageGroup-<n>")`（每 view 一组，别让引擎跨 tab 解析具名 target）；回调 `cb_open_window(url,name,flags,rect)` / `MC_OPEN_WINDOW_HOOK(url,name)` / `cb_close_window`；控件：`AgaveWebView.c` 的 `windowName` 注册表 + `agwOpenWindowFor()`（复用→`on_activate_view`，新建→`on_new_window`）、`agwOnOpenWindow`、`agave_control_open_window`、`SERVICE_REQUEST` 派发；渲染进程 `examples/render_host/main.c` 的 `onOpenWindow`/`onCloseWindow`/`onOpenWindowHook`；shell：`onNewWindow`（建 tab）、`onActivateView`（切 tab）；回归 `tools/run_window_tests.ps1` |
| 宿主命令的送达契约（R157） | `src/webcore/page/Page.h` `documentEpoch()` + `src/webcore/page/Frame.cpp` `setDocument()`（主 frame 自增）→ `src/webview/WebView.cpp` `documentEpoch()` → `macross_view_document_epoch()`（`src/webview/WebEngine.cpp`）；控件侧 `AgaveWebView.c`：`agwPost()`（盖 `command.docEpoch`）、`agwRefreshDocumentEpochs()`（每轮引擎迭代，**不能只在 drain 时刷**）、`agwCommandIsDocumentScoped()`、`agwReportCommandDrop()` → `on_command_dropped`；本地后端 `AgwViewBackend.c`（`AGW_VIEW_CMD_EVAL` 返回 `macross_view_eval_script` 的结果、`localDocumentEpoch()`），进程后端 `AgwViewBackendProcess.c` + `examples/render_host/main.c`（`AGW_VIEW_EVENT_DOCUMENT` / `AGW_VIEW_EVENT_COMMAND_DROPPED`、`AGB_IPC_STALE_EPOCH` 测试钩子）；引擎侧判据 `KJSProxy::lastEvaluateRan()`；回归 `tools/run_process_backend_tests.ps1` 的三个 dropped 用例 |
| 卡死页恢复（中断 + 重载） | `src/webview_control/AgaveWebView.c`：`agwPost()`（唯一排队口）、`agwWatchdogThread()`（三种判据：`antb_engines_js_depth()` 有 JS + 队首是用户动作 ≥1.5 s → 中断；JS ≥12 s 无人等 → 中断；无 JS 且 ≥10 s 不 drain → 只记录）、`agwCommandIsUserAction()`、`agwQueueWaitType()`、`agwWatchdogRecord()`（`agb_watchdog.txt`）/ `agwEngineBusyFor()`（命令执行中）；刷新链路：`WebView::reload()`（先 `stopLoading(true)` 再 reload）、`FrameLoader::receivedData(ResourceLoader*,…)`（按投递者路由）、`FrameLoaderClientMC::committedLoad`（拒绝陈旧 loader）、`Loader::didFail`（取消走 `error.isNull()` 分支，不污染缓存）、`agave_webview_interrupt()` / `agave_webview_busy()`；桥 `antb_interrupt_all_engines()`（`bridge/AntBridge.c`）；引擎侧 `js_interrupt_pending` + `sv_jit_arm_interrupts()` + `js_interrupt_raise()`；壳 `--interrupt-after`、`shellAnnounceCrashFile()` |
| 浏览历史 | `examples/browser_shell/main.c`：`loadHistory()` / `recordHistory()` / `statePath()`（`history.txt` 是追加写，启动时读回并按连续重复去重） |
| 页面时序挂点 | `FrameLoaderClientMC.cpp`：`dispatchWillClose` / `committedLoad` / `dispatchDidFinishDocumentLoad` / `dispatchDidFinishLoad` |
| 默认日志收口 | `src/webview/AgbLog.h`：`agbLog(channel, fmt, ...)` + `agbLogElapsedMs()`；通道 `evt/nav/ui/shell/probe/flag`，时间原点跨 DLL 共享；格式 `[<ms>ms] [tag] 正文` |
| `[nav]` 导航日志 | 钩子在 `FrameLoaderClientMC.cpp`（request/new-window/submit/redirect/fail/blocked）、`WebEngine.cpp`（`macross_view_open_url/back/forward/reload/stop`）、`AgaveWebView.c`（popup/download）；开关 `AGB_NAV_LOG=0` |
| `[nav]` 回归 | `tools/run_nav_tests.ps1` + `tools/pages/navcheck_*.html` + `tools/nav_redirect_server.py` |
| WebCore 侧限制 | `src/webcore/loader/FrameLoader.cpp`（unload 门禁）、`src/webcore/dom/EventTargetNode.cpp`（事件转发边界）、`src/webcore/dom/Document.cpp`（`finishedParsing` 的 DCL） |
| 地址栏输入 → URL | `examples/browser_shell/main.c`：`parseShellArgv()` / `normalizeUrl()` / `fileUrlFromLocalPath()`；`file:` URL 只允许单次转义 → `KURL::encode_string()`（幂等） |
| 无头探针 | `examples/browser_shell/main.c`：`probeRun()`（`WS_POPUP` + `WS_EX_TOOLWINDOW` 隐藏父窗口）+ `probeWndProc()`；`AGB_PROBE_SHOW=1` 才显示 |
| Web Worker | `prelude.js` 末尾「Web Workers」段（`_agbWorkerStart` / `_agbWorkerPreamble` / `_agbWorkerPrefetch`）；ant 侧 `H:\ant-v16\src\modules\worker\{web,core}.c`；栈帧短名 `esm_frame_filename`（`H:\ant-v16\src\esm\loader.c`） |
| 回归页 / 脚本 | 页面在 `debug/`；`tools/run_worker_tests.ps1`、`tools/run_nav_tests.ps1`、`tools/run_typing_tests.ps1`、`tools/run_xhr_tests.ps1`（XHR 投递重入）、`tools/run_jit_loop_tests.ps1`、`tools/check_window_visible.ps1`、`tools/bench_compile.ps1`、`tools/measure_cpu.ps1` |
| `document.currentScript` / 脚本执行期状态（R95） | `Document.h`：`currentScript()` / `setCurrentScript()` + `CurrentScriptScope`（RAII，嵌套还原）；设置点：`HTMLTokenizer::scriptExecution()`（解析器路径，元素由 `scriptHandler()` 的内联分支与 `notifyFinished()` 的 `scriptNode.release()` 提供）、`HTMLScriptElement::evaluateScript()`（元素路径）、`AntRuntime::flushDeferredModules()`（`defer` 队列，`DeferredModule::scriptElement`）；模块一律不设 |
| XHR / `fetch` | `src/javascript/ant_backend/prelude.js`（`XMLHttpRequest` 包 `__agbHttpRequestEx`，`fetch` 用同一入口）→ `AntRuntime.cpp`：`XhrJob`（`start`/`deliver`/`cancel`）、`xhrJobs()`、`collectXhrJobs()`（先摘后投 + 重入守卫，R94）、`cancelXhrJobsForGeneration()`；回归 `tools/run_xhr_tests.ps1` |
| 控件生命周期（R61） | `src/webview_control/AgaveWebView.c`：`agwDrainCommands()`（命令按 `viewId` 查目标并 `refs++`，跑完 `agwRelease()`）、`agave_webview_destroy()`（`refs>0` 时推迟到引擎线程）、`agwRelease()`；探针侧 `examples/browser_shell/main.c` 的 `__agb_evaldone__` → `AGW_PROBE_EVALDONE` → `--settle` |
| 裸引擎复现（无浏览器） | `tools/ant_loop_repro.c`：`antb_engine_create + antb_eval` 跑任意脚本；用来判定"是不是 ant/JIT 的锅"（R61 用它排除了 JIT） |
| 堆破坏 / UAF 定位 | `AGB_GUARD_MALLOC`（`src/wtf/FastMalloc.cpp`，页守卫 + free 即 unmap）+ `tools/cdb_heap_probe.ps1`（cdb 启动、dump 被写坏的块）；HEAP 消息是**发现点**不是写入点（R62） |
| GC 扎根（ant） | `H:\ant-v16`：callback builtin 用 `gc_temp_root_scope_begin/push`；判据 `ANT_GC_STRESS=1` + `H:\ant-v16\tests\array_callback_gc.cjs`（R63） |
| 键盘文本输入 | `src/webview/WebView.cpp`：`keyDown()`（只派发 keydown，记住 `preventDefault`）、`textInput()`（WM_CHAR：过滤控制字符 → `handleCharacterEvent`）；`src/webcore/page/EventHandler.cpp`：`keyEvent()` / `handleCharacterEvent()`（keypress + 默认插入）；`src/webview/KeyEventMC.cpp`：`PlatformKeyboardEvent`（字符不来自 vk）；回归 `tools/run_typing_tests.ps1`（R37） |
| 文本选区 / Selection API（R175） | 控件：`AgaveWebView.c` 的 `WM_MOUSEMOVE`（`MK_*` → button，modifier 打包进 button 高 8 位）、`WM_SETFOCUS`/`WM_KILLFOCUS`、`agwOnClipboardGetData()`（按 `lstrlenW` 换 UTF-8）；引擎：`WebView::mouseDown/mouseUp`（`m_clickCount` 连击 + `PlatformMouseEvent::setClickCount`）→ `EventHandler::handleMousePressEvent` / `handleMouseDraggedEvent` / `updateSelectionForMouseDrag`；快捷键 `EventHandler::handleDefaultEditingShortcut()`（Ctrl+A/C/X/V/Insert、Shift/Ctrl+Shift+方向键）→ `Editor::execCommand`（命令表小写键 + `commandIsSupported/Enabled` 给 `queryCommand*`）；桥 `AntRuntime.cpp`：`ensureSelectionClasses()` / `selectionGetter` / `selectionMethod` / `rangeGetter` / `rangeMethod` / `wrapRange()` / `selectionReleaseRefs()`，`documentObjectFor` 装 `getSelection/createRange/execCommand/queryCommand*`，`installGlobalApi` 装 `window.getSelection`；`selectionchange`：`Frame::respondToChangedSelection` → `AntRuntime::dispatchDocumentEvent()`；词边界：`src/webcore/platform/unicode/TextBoundariesUnicode.cpp`（`findWordBoundary` / `findNextWordFromIndex`，用 `TextBoundary.cpp` 的 UAX#29 表；因为 `IN` 枚举与 windows.h 冲突，必须放在不含 windows.h 的 TU）；`user-select` 生效点 `Node::canStartSelection()`；宿主入口 `agave_webview_editor_command()` / `macross_view_exec_editor_command()`；回归 `tools/run_selection_tests.ps1` + `tools/pages/cap_selection.html`，调试 `AGB_SEL_DEBUG=1` |
| 输入法 / Unicode 输入 | `src/webview_control/AgaveWebView.c`：`agwRegisterClass()`（`RegisterClassW`，Unicode 窗口）、`agwUtf16ToUtf8()`（代理对配对）、`agwProc` 的 `WM_CHAR`/`WM_IME_CHAR`；宿主循环 `GetMessageW`/`DispatchMessageW`；死循环与光标：`src/webcore/platform/unicode/TextBoundary.cpp`、`src/webcore/rendering/RenderText.cpp`；回归 `--ime` 三例（R38） |
| 注入调试脚本 / 剪贴板（R112） | `src/javascript/ant_backend/inspector.js`：hover 高亮 + 页内右键菜单（`onContextMenu` / `onMouseDown` / `openMenu` / `elementInfo`），`window.__agaveInspector` 的 `inspectAt` / `menuAt` / `menuSelect` / `elementInfo`；复制链 `prelude.js`（`navigator.clipboard.writeText`）→ `AntRuntime.cpp`：`globalClipboardSetText`（`__agbClipboardSetText`）→ `src/webview/WebCallback.h` 的 `cb_clipboard_set_data` → `AgaveWebView.c`：`agwOnClipboardSetData`（`CF_UNICODETEXT`）；回归 `tools/run_inspector_tests.ps1` + `tools/send_ui_events.ps1`（`-RightClick` / `-LeftClick`） |
| flex 布局（R54/R81/R110/R113） | `RenderFlexibleBox.cpp`：`layoutFlexLines()`（行 flex：宽度分配、`align-items:stretch`、`align-content` + `m_preFlexHeight`、gap）、`layoutVerticalBox()`（列 flex 的旧 `-webkit-box` 路径）、`calcHorizontalPrefWidths()`（容器 preferred width，含 gap）；`RenderBox.cpp::calcHeight()` 的 `isStretchingChildren()` 分支；调试 `AGB_FLEX_DEBUG=1`；回归 `tools/pages/layout/flex_*.html` + `tools/run_layout_tests.ps1` |

## 生成文件与 `buildKJS/` 目录（不是 JS 引擎）

> 看到目录名里的 "KJS" 时先读这一节：**本项目只用 ant 跑 JS，没有 KJS / QuickJS。**

- **JS 引擎是 ant**（`antjsb.dll`，源码在 `H:\ant-v16`）。页面 JS 的语法/编译/VM 都在 ant 仓库，
  与 `buildKJS/` 无关。`buildQJS/` 是 QuickJS 路线的同一批生成文件，当前构建**不使用**。
- `buildKJS/` 是 WebKit 移植遗留的**生成文件目录名**，里面是 **CSS 侧**的生成产物；
  当前构建仍然直接编译/链接它们（不是死代码）。名字带 KJS 只是历史包袱，不表示 JS 引擎归属：

| 真源（要改的） | 生成物（构建实际消费的） | 怎么生成 |
|---|---|---|
| `src/webcore/css/CSSGrammar.y`（bison 语法） | `src/webcore/buildKJS/CSSGrammar.cpp` | `bison` 不在 PATH；没有就**同步手改生成文件**（R173 的 custom property 规则就是手改的）。只改 `.y` 不会生效。生成文件里的 `#line` 指向旧 WebKit 路径，行号不可信 |
| `src/webcore/css/{html4,quirks,svg,view-source}.css` | `src/webcore/buildKJS/UserAgentStyleSheets.{h,cpp}` | `python tools/gen-ua-stylesheets.py`（复刻 `src/webcore/css/make-css-file-arrays.pl`，后者要 perl+CPP）；改完必须重链 `libagave`（字节数组是编译进去的） |
| `src/webcore/**/*.idl` + `bindings/scripts/CodeGenerator{JS,QJS}.pm` | `buildKJS/JS<Interface>.{h,cpp}`、`buildQJS/QJS<Interface>.{h,cpp}`（**ant 构建两份都不编译**：`src/src.cmake` 只在 `OPT_USE_KJS`/`OPT_USE_QJS` 时 glob 它们） | `pwsh -File tools/gen_bindings.ps1 -Generator QJS -Compare`（MSYS2 perl，16 并发约 95 s）。见下 |

- **IDL 绑定生成**（`tools/gen_bindings.ps1`，2026-11-10）：261 个 `.idl` 一次性生成，`-Compare` 与签入目录逐字节对拍。
  **QJS：483/490 逐字节一致**；7 个不一致是**签入副本的历史配置混合产物**，不是生成器缺陷——
  `QJSDOMWindow.{h,cpp}`、`QJSHTMLEmbedElement.{h,cpp}`、`QJSHTMLObjectElement.{h,cpp}` 是当年**不带
  `ENABLE_SVG`（和 `ENABLE_SVG_EXPERIMENTAL_FEATURES`）** 生成的（单独用去掉这两个 define 的命令重跑即逐字节一致），
  `QJSDOMParser.h` 只差一行的行尾空格（签入时被删）。`-Skip` 列上这 7 个再加 `-Strict` 就是严格门禁
  （实测 exit 0）。另有 32 个文件只出现在新生成里（`SVGTests`/`SVGLangSpace`/`EventTarget` 等 16 个
  mixin/protocol 接口，签入树从来没为它们生成过文件）。
- **JS 后端只算半个**：484 个一致，`JSHTMLEmbedElement`/`JSHTMLObjectElement` 同样是当年的 SVG-off 产物，
  但 `page/DOMWindow.idl` **已经无法用它生成**——`CodeGeneratorJS.pm` 的 `GetLegacyHeaderIncludes` 只认
  `LegacyParent=KJS::Window`，而现在的 IDL 写的是 `LegacyParent=Window`（KJS 侧早已停用），于是 die
  "Don't know what headers to include for module window"。JS 后端仅作历史对照，新后端照 `CodeGeneratorQJS.pm` 写。
- 顺带修了 `CodeGenerator.pm` 的 `ScanDirectory`：`$thisDir` 少一个 `my`，递归会把父层循环的目录覆盖掉，
  于是**只搜索 `--include` 根的第一个子目录链**——真实构建给每个 IDL 目录单独 `--include`，所以一直没暴露；
  用单一根目录生成时 `SVGCircleElement → SVGTests` 这类 mixin 父接口就会 "Could NOT find specified parent"
  （47 个 SVG IDL 全挂）。

- 常见错误：①只改 `.y` / `.css` 就以为生效（实际被编译的是生成文件）；②把 `buildKJS/` 当"KJS 引擎"删掉或绕开；
  ③把 `buildQJS/` 当现状去改。三种都会得到"改完没反应"或改错副本。
- **生成不是构建的一部分**：`src/src.cmake` 直接 `GLOB` `src/webcore/buildKJS/*.cpp`（buildQJS 另有一份），
  CMake、`tools/build_project.ps1` 都不会调用 bison / `gen-ua-stylesheets.py`。因此改语法/UA 表都是**手动**：
  改完生成文件后再增量构建、让编译器打包进 `libagave`。
- 改这两处的验证：`tools/gen-ua-stylesheets.py` 后重跑 `run_layout_tests.ps1` + 受影响的探针；
  语法改动用最小页面探针断言（R173：`--VC-BG-0` 的 `var()` 解析；R174：UA `form` 边距）。
