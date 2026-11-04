# 上手：agave 是什么、代码在哪、改哪里

## 1. 一句话

agave 是**从零移植的浏览器**：**WebKit 2008 的 WebCore**（HTML/CSS 解析、DOM、布局、绘制、网络）+
**ant JS 引擎**（`H:\ant-v16`：silver VM/编译器、ESM、Worker、wasm）+ **Win32 控件与宿主壳**。
目标是在真实站点（deepseek、百度、Vue 应用……）上对齐现代浏览器行为。

## 2. 分层

| 层 | 位置 | 规模 | 职责 | 入口锚点 |
|---|---|---|---|---|
| 宿主壳 | `examples/browser_shell/main.c` | ~2.3k 行 | 标签页/地址栏/查找/书签/快捷键；`--probe` 真无头探针 | `WinMain` → `parseShellArgv` → `probeRun` |
| Win32 控件 | `src/webview_control/AgaveWebView.c` | 1.6k | 控件窗口、**引擎线程**、命令队列（`AGW_CMD_*`）、剪贴板/IME | `agave_webview_create` |
| 视图层 | `src/webview/` | 7.4k | MaCross：`Page`/`Frame`/`WebView`/ResourceHandle 适配、帧泵 | `macross_view_update`（paint + 泵异步）、`WebView::loadUrl` |
| WebCore 移植 | `src/webcore/` | 76k | WebKit 2008：解析器、DOM、CSS、布局、绘制、网络、历史 | `HTMLTokenizer::parse`、`Document::finishedParsing` |
| JS 桥 + 运行时 | `src/javascript/ant_backend/` | `AntRuntime.cpp` 7.8k、`AntBridge.c` 2.5k、`prelude.js` 935、`polyfills.js` 512 | 文档↔engine 绑定、DOM 桥、XHR、模块管线、JS 语义 shim | `AntRuntime::bindDocument`、`antb_*` |
| JS 引擎 | `H:\ant-v16`（独立 git 仓库） | — | silver VM/编译、ESM loader、`modules/worker`、wasm | `ant_create`、`js_*`、`antb_eval_module` |
| 平台底座 | `src/wtf/`、`src/core/`、`include/` | ~6k | WTF 容器、字符串、断言、公共头 | — |
| 工具 | `tools/` | — | 构建/重链/基准/回归/cdb/测试页 | 见 [build-and-run.md](build-and-run.md) |

## 3. 目录地图（仓库根）

| 目录 | 内容 |
|---|---|
| `src/` | 引擎源码（上表） |
| `examples/` | 宿主与示例：`browser_shell`（桌面壳）、`view_host`（无窗口渲染出 BMP）、`embed_demo`（嵌入）、`touchweb`（手机风格 UI，可选） |
| `include/` | 公共头：`agave_webview.h`、`AntBridge.h`、http/wtf 等 |
| `tools/` | 全部脚本、`cdb.exe`、`pages/`（**被跟踪的**测试页） |
| `docs/` | 文档，入口见 [README.md](README.md) |
| `debug/` | 本地调试页（**`.gitignore` 忽略**，见 [conventions.md](conventions.md)） |
| `3rdparty/` | ant 预编译产物与第三方库（`antbridge.dll` 分发副本在这里） |
| `build-zig-native/` | **唯一可运行目录**：exe + dll + `prelude.js`/`polyfills.js` |
| `dist/`、`packages/`、`vcproj/`、`test/`、`unit_tests/` | 打包模板 / 第三方源码包 / VS 工程 / 演示宿主与单元测试 |

## 4. 一次导航/一个脚本怎么走（速览）

```
WinMain → createTab → agave_webview_create（控件）
   → 引擎线程 macross_view_create → WebView::loadUrl（file: URL 在这里做转义）
   → FrameLoader → 网络/本地读 → HTMLTokenizer 解析
        ├ inline/external script → AntRuntime::evaluate
        └ module script → prefetchModuleGraph/queueModule → 解析结束 flushDeferredModules
   → 每个文档 AntRuntime::beginNewRealm()：新 AntbEngine + bindDocument()
        → 加载 prelude.js（window shim）→ polyfills.js（DOM 构造器补丁）
   → dispatchDidFinishDocumentLoad（DCL）→ dispatchDidFinishLoad（load）
```

完整时间轴见 [handover.md §2.3](../handover.md)，模块管线三条泳道见
[architecture/module-pipeline.md](architecture/module-pipeline.md)。

## 5. 我要改 X，去改哪里

| 想改 | 改这里 | 备注 |
|---|---|---|
| `window`/`document` 辅助、XHR/fetch、storage、`matchMedia`、`Worker` | `src/javascript/ant_backend/prelude.js` | **热改**：拷到 `build-zig-native\` 即生效，不重链 |
| DOM 构造器补丁（`append`/`classList`/`instanceof Node`/构造器 stub） | `src/javascript/ant_backend/polyfills.js` | 热改；每 realm 执行一次 |
| DOM 桥（节点属性/方法/事件/构造函数） | `AntBridge.c`（wrapper 表 + 分发）+ `AntRuntime.cpp`（`nodeGetter`/`wrapNode`/类表） | 改完 `build_antbridge.ps1 -Force` + relink |
| HTML 解析、`script`/module 时序 | `src/webcore/html/HTMLTokenizer.cpp` | 等待位/`scriptExecution` 在这里 |
| CSS 解析、选择器 | `src/webcore/css/CSSParser.cpp`、`CSSStyleSelector.cpp` | 词法/语法老，改前先看 `AGENTS.md` 里 `var()` 的写法 |
| computed style | `src/webcore/css/CSSComputedStyleDeclaration.cpp` | 未实现属性默认静默（`AGB_CSS_DEBUG` 才打） |
| 布局 | `src/webcore/rendering/` | flex 实际走 `-webkit-box`（`RenderFlexibleBox`） |
| 绘制 / 裁剪 / 渐变 | `src/webcore/platform/graphics/GraphicsContextPS.cpp` | picasso 后端；`clip` 是设备坐标 |
| 网络（HTTP/缓存/代理） | `src/webcore/platform/network/native/` | WinHTTP 后端 + `NetLog.cpp` |
| 页面生命周期钩子（willClose/commit/DCL/load） | `src/webview/FrameLoaderClientMC.cpp` | 时序问题的第一现场 |
| 快捷键 / 标签页 / 地址栏 / 探针 | `examples/browser_shell/main.c` | `normalizeUrl()` 是 URL 唯一入口 |
| 控件窗口 / 剪贴板 / IME / 帧泵定时器 | `src/webview_control/AgaveWebView.c` | 跨线程规则见 [debugging.md](debugging.md) |
| JS 语义 / VM / 编译 / ESM / worker / wasm | `H:\ant-v16\src\{silver,esm,modules}` | 见 [ant-engine.md](ant-engine.md) |

## 6. 症状 → 先查哪（别一上来就怀疑页面）

| 症状 | 先查 |
|---|---|
| 白屏 / 元素缺失 | 探针**退出码** → `[antjs]` 错误 → `AGB_ERR_SNIPPET=1` 拿源码位置 |
| `undefined is not a function` / `Cannot read properties of undefined` | 桥接缺方法或 wrapper 的 catch-all getter（`nodeGetter`），先于怀疑页面 |
| 样式不对 | `getComputedStyle` 是否走到、属性是否"未实现"；跑 `debug/csscheck.html` |
| 画错 / 残影 / 不重绘 | 隐藏窗口没有 WM_PAINT（用 `--probe AGB_PROBE_SHOW=1` 或 `agave_view_host` 出 BMP）；再看 clip/repaint |
| 卡住 / CPU 100% | `AGB_PROFILE=1` 直方图 + `cdb` 抓忙线程栈（[debugging.md](debugging.md)） |
| 网络没提交 / 一直重试 | `AGB_NET_LOG=1` 看 stage；`progress finished=1` **不代表**加载成功 |
| 崩溃 | `cdb` 断在首次异常抓全线程栈；worker 崩在**非主线程**（`worker_thread_main`） |
| 刷新后行为变了 | realm/监听器泄漏（[lessons.md](lessons.md) R10/R17） |
