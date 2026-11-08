# tab / view 拆除生命周期：设计记录

> 状态：**设计记录 + 实施中**（本文件描述要落地的契约；落地后的现状见
> [arch-embedding.md](arch-embedding.md) 的「线程模型」与「已知限制」）。
> 历史缺陷见 [hist-lessons.md](hist-lessons.md) R141。

## 1. 问题（现状审计，2026-11-08）

关闭一个 tab（`closeTab()` → `agave_webview_destroy()`）**没有走页面生命周期**，只是
「销毁这个 view + 丢掉 VM」，而且销毁发生在宿主线程。具体：

| # | 缺口 | 后果 |
|---|---|---|
| A | `agave_webview_destroy()` 在 `refs <= 0` 时于宿主线程直接 `macross_view_destroy()` → `delete WebView`（DOM/JS/unload/ant 引擎全部在宿主线程拆除）。`refs` 只覆盖命令分发与每帧 `macross_view_update()`，**不覆盖 `macross_event_dispatch()`** | 宿主线程拆文档的同时引擎线程在跑该文档的 WebCore 定时器（网络完成、`setTimeout`）；R141 的 `CSSFontSelector::fontLoaded` 空指针就是这样踩出来的 |
| B | 导航路径的 `dispatchWillClose()` → `documentWillUnload()`（pagehide → visibilitychange → unload → `stopRealm()`）只在 `closeOldDataSources()` 调用；关 tab 只走 `destroyContext()` | 关 tab 不发 pagehide/visibilitychange；`cancelInFlightResources()` 不跑；`forgetRealmScripts()` / `g_apiRealms` 不清 → 新 realm 复用地址会跳过 prelude（注释 [AntRuntime.cpp:670](../src/javascript/ant_backend/AntRuntime.cpp) 明说要清） |
| C | `wrapNode()` 给每个被 JS 碰过的 node `ref()`，按 generation 记在 `pinnedNodes()`，只在 `stopRealm()` 的「延迟一代」释放；`g_pinsPendingRelease` 是进程全局 | 关 tab 泄漏该 tab 所有被 JS 访问过的 node；`Node::m_document` 是 `DocPtr`，于是整个 Document + CSSStyleSelector + DocLoader 都活着 |
| D | `Document::detach()` 只停媒体/拆渲染树，不摘 `CachedFont` 的 client；`CSSFontSelector::fontLoaded()` 不判 `renderer()` | 多个文档共享同一字体 URL 时，字体在 detach 之后完成 → 回调进已 detach 的文档 → `renderer()->setNeedsLayout...` 读 `this+0x32`，AV |
| E | `agave_webview_destroy()` 的窗口销毁在 `agwFreeItem()`（引擎线程）里 `DestroyWindow()`；popup 的窗口又是在引擎线程创建的 | Win32 线程亲和性错（宿主创建的窗口不能在引擎线程 DestroyWindow） |

## 2. 契约

### 2.1 线程

- **引擎线程独占** WebCore、`AntRuntime`、`macross_*`、surface 缓冲。宿主线程只碰 HWND 与
  公开 API 的入队。
- `agave_webview_destroy()` 可以在任意线程调用，但它只做「标记 + 摘除 + 排队」：
  1. `destroyed = 1`；从 `g_views` 摘除；
  2. 清 `GWLP_USERDATA`，把窗口销毁交给**创建它的线程**（同线程直接 `DestroyWindow`，
     否则 `PostMessage(AGW_MSG_RETIRE)`）；
  3. 把 item 放进 reap 队列。
- 引擎线程在每轮 `macross_event_dispatch()` **之后** `agwReapViews()`：`refs <= 0` 的 item
  才真正拆除（`macross_view_destroy` + surface + `free`）；被 pin 住的留给最后一次
  `agwRelease()` 拆（今天的行为，保留）。
- 道理：`refs` 的语义从「正在用这个 view」升级为「引擎线程正在用这个 view」，而 reap
  发生在整轮结束时，`eventDispatch` 自然被覆盖。

### 2.2 realm 拆除（一个入口）

```
macross_view_destroy(view)
  ├─ registerPageContext(page, 0)
  ├─ AntRuntime::activateContext(view->jscontext)      // 切到该 view 的 realm
  ├─ AntRuntime::documentWillUnload()                  // 导航同款 teardown
  │     pagehide → visibilitychange(hidden) → unload
  │     → dropListenersForDocument(doc) → teardownRealm(state)
  ├─ delete WebView                                    // DOM/Frame/Page 拆除（在引擎线程）
  └─ AntRuntime::destroyContext(context)
        └─ 若还有 realm（文档从未 bind：engine 还在）→ teardownRealm(state) 兜底
```

`teardownRealm(RuntimeState*)` 是 `stopRealm()` 的**状态显式**版本：临时把 `g_currentState`
指向目标 state（`g_*` 宏因此作用正确），跑完恢复；`stopRealm()` = `teardownRealm(g_currentState)`。
`destroyContext()` 先 teardown 再删 state，保证「关 tab」与「导航」清的是同一套东西：

- `cancelInFlightResources(generation)`（XHR / module 下载 / entry script）
- `releasePinnedNodes()`：该 state 的 pin 一代延迟；销毁时**当前 generation 立即释放**
- `docObj` / `mainDocObj` / window & document 对象缓存 / doc slots
- listeners、`antb_release_owner(engine, document)`、CSSOM refs
- `forgetRealmScripts()`、`g_apiRealms->clear()`
- `antb_engine_destroy()`

### 2.3 页面资源（铁律 8）

- 文档拆除时，字体必须摘链：`Document::detach()` → `CSSStyleSelector::fontSelector()`
  → `CSSFontSelector::documentDetached()`：清 `m_fonts`（释放 `CSSFontFace` → `CSSFontFaceSource`
  → `CachedFont::deref(this)`），并置 `m_detached` 让 `getFontData()` 返回 0、
  `fontLoaded()` 直接返回。
- `CSSFontSelector::fontLoaded()` 另加 `renderer()` 判空止血（与 `Document.cpp` 其它调用点一致）。
- 其它资源维持现状：图片 → `Image::~Image` 的 `ImageDecodeService::cancel()`；
  媒体 → `Document::detach()` 的 `stopMediaElements()`；网络 → `stopLoading(true)` 的
  `cancelRequests(docLoader)`。

## 3. 验证

`tools/run_tab_lifecycle_tests.ps1`（probe 驱动，全部无头）：

1. **teardown 有始有终**：`AGB_EVT_DEBUG=1` 下，关闭一个 view 后 `realm start` 与
   `realm stop` 成对；关 tab 顺序包含 `pagehide` → `visibilitychange` → `unload` → `realm stop`。
2. **pin 不泄漏**：view 销毁后进程退出时 `LEAK: N Node` 不出现（或不再增长）。
3. **字体在途关 tab**：本地慢字体 + 关 view，等待字体本该完成的时间，断言退出码 0 且没有
   `agb_crash.txt`。
4. **多 view**：主页面 + `window.open` 的 popup，退出时两个 view 各有一对 start/stop。

## 4. 暂缓（记录，不在本批）

- GUI 宿主窗口关闭（`WM_DESTROY`）目前直接退出、不逐个 closeTab，unload 不跑；
  与「关 tab」语义不同，需要单独决定是否在退出时同步 teardown（会受 unload 卡住影响）。
- popup 窗口仍在引擎线程创建（`agwOnOpenWindow`）；本批只保证销毁走对线程。
- `WebView::stop()` / `~WebView()` 里的 `garbageRelease()` 仍从调用线程碰 `TimerBase`；
  等 GC 完全切到 ant 后一起处理。
