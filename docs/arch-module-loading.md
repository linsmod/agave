# 模块加载与求值（当前实现）

> 状态：**当前态，最后核对 2026-11-05**。三条泳道（宿主主线程 / 引擎线程 / WinHTTP worker）、
> 入口模块单槽等待位、`file:` 模块伪装层。设计与取舍见 [hist-lessons.md](hist-lessons.md) R7/R9/R17；
> 总图与所有权模型见 [arch-overview.md](arch-overview.md)；待办见 [plan-roadmap.md](plan-roadmap.md)。

## 怎么读

- **模块求值只允许发生在引擎线程的解析调用栈里**：网络泵（`pumpAsyncRequests`）只负责把 body 放进
  队列，绝不执行页面 JS —— 泵回调里 detach 节点会和正在进行的绘制/布局打架。
- 入口 `<script type="module" src>`（非 async）用**单槽等待位**把解析挂起，等 body 到了再在
  解析流程内 `scriptExecution()` 求值；这就是"模块执行先于 DOMContentLoaded"的来源。
- `flushDeferredModules()` 在解析结束时执行延迟模块（`<script defer>` 与 DCL 前排队者），
  随后才是 DCL / load（⚠ DCL 目前会派发两次，见 [plan-roadmap.md](plan-roadmap.md) P0-c）。

## 时序图

```text
入口模块加载（当前实现）：模块求值只在解析流程内发生
════════════════════════════════════════════════════════════════

主线程（宿主）                引擎线程（webview 控件内部）                WinHTTP worker
─────────────                ───────────────────────────                ─────────────
探针: agave_webview_eval ──PostMessage──► ┌ eval 队列泵（经典 script，
                                          │  agave_webview_eval 可重入）
                                          │
[帧泵 WM_TIMER id=1 每帧]                 │
  macross_view_update:                    │
    paintView()                           │
    pumpAsyncRequests:                    │
      collectXhrJobs()   ← XHR 回调交付   │
      g_moduleSession->pump() ────────────┼──── 完成回调在这里被拉取 ◄── 网络I/O完成
      │    ModuleFetchClient::onComplete: │
      │      store 磁盘缓存 + 扇出 import  │
      │      moduleEntryScriptArrived     │
      │        → 投递 ArrivedEntryBody    │
      │        （泵回调内禁止执行 JS ★）    │
      ├─ drain arrived                    │
      │    → notifyModuleEntry 钩子 ──────┼──► HTMLTokenizer::notifyModuleEntry
      │      （只传 body，不执行 JS）       │      （见下方导航调用栈）
      └─ antb_pump(engine)（timer/microtask）

引擎线程（导航调用栈）——模块求值的唯一合法位置：
────────────────────────────────────────────
  dispatchWillClose → 旧 realm teardown
    （cancelInFlightResources：已登记入口发 failed 通知，等待位不会永久阻塞）
  beginNewRealm（gen++，新 VM）
  解析 HTML（HTMLTokenizer）
    ├─ <script type="module" src>（非 async）
    │    登记等待位（单槽）+ m_requestingScript 防重入
    │    fetchAndQueueModuleScript ──► moduleStart ──► WinHTTP（异步）
    │    缓存命中 → 同步通知（老 CachedScript ref() 同款）
    ├─ stash 剩余 src + loadingExtScript(true)   ← 解析阻塞，等同经典外链脚本
    │
    │  ……帧循环：body 到达 → drain → 钩子
    │
    │  ◄── notifyModuleEntry（回到解析调用栈）：
    │    scriptExecution(source, url)
    │      └─ prefetchModuleGraph + queueModule
    │    loadingExtScript(false) + write(rest)
    │    （解析恢复，继续剩余 HTML）
    ├─ 继续解析 ……
    └─ 解析结束 → flushDeferredModules → 求值模块   ★ 解析流程内 = 已验证可靠上下文
         → DOMContentLoaded（落后于模块执行，对齐 spec；双发是 P0-c 待办）
         → load

file:// 模块的伪装层（不破坏 ant 限制模块https设定）：
────────────────────────────────────
  agave → 引擎：file:///e:/x/main.js  ──►  http://file.ant/e:/x/main.js
                （esm_is_url 只认 http(s)；esm_resolve_url 是通用 :// join）
  引擎 → agave：模块 fetcher 收到伪装 URL ──► 还原 file:/// ──► 本地读盘（同步完成）
  代价：import.meta.url 显示伪装 URL；percent-escape 在还原时解码

已知限制（详见 [hist-lessons.md](hist-lessons.md) 的 R7）：
  ① 动态插入/async 入口仍走 CachedScript
  ② 入口 script 元素的 load/error 事件暂不派发
  ③ import.meta.url 伪装形态
  ④ DCL 后到达的 inline 模块排队不执行
  ⑤ iframe 等待位单槽冲突时回退 CachedScript

```

## 代码锚点

| 环节 | 位置 |
|---|---|
| 帧泵入口（paint 后泵异步） | `src/webview/WebEngine.cpp`：`macross_view_update()` → `paintView()` → `AntRuntime::pumpAsyncRequests()` |
| 网络泵 | `AntRuntime.cpp`：`pumpAsyncRequests()`（`collectXhrJobs` → `g_moduleSession->pump()` → drain arrived → `antb_pump`） |
| 模块下载完成 | `AntRuntime.cpp`：`ModuleFetchClient::onComplete()`（写磁盘缓存、扇出 `import`、投递 `ArrivedEntryBody`） |
| 交给 tokenizer | `AntRuntime.cpp`：`notifyModuleEntry()` → `HTMLTokenizer::notifyModuleEntry()`（只传 body） |
| 等待位 / 防重入 | `src/webcore/html/HTMLTokenizer.cpp`：`m_awaitingModuleEntry` / `m_pendingModuleUrl` / `m_pendingModuleNode` / `m_requestingScript` / `loadingExtScript()` |
| 入口模块求值 | `HTMLTokenizer.cpp`：`scriptExecution()` → `AntRuntime::prefetchModuleGraph()` + `queueModule()` |
| 延迟模块求值 | `AntRuntime.cpp`：`flushDeferredModules()`（`dispatchDidFinishDocumentLoad` 里调用） |
| file: 伪装层 | `AntRuntime.cpp`：`kEngineFileUrlPrefix` = `http://file.ant/`、`engineUrlForFile()` / `fileUrlForEngine()` |
