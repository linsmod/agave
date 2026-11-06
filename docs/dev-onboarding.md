# 上手：代码在哪、改哪里

> 先读 [arch-overview.md](arch-overview.md) 建立整体图；本文只做任务路由。
> 构建/运行命令见 [dev-build-and-run.md](dev-build-and-run.md)，调试见 [dev-debugging.md](dev-debugging.md)。

## 1. 一句话

agave 是**从零移植的浏览器**：**WebKit 2008 的 WebCore**（HTML/CSS 解析、DOM、布局、绘制、网络）+
**ant JS 引擎**（`H:\ant-v16`：silver VM/编译器、ESM、Worker、wasm）+ **Win32 控件与宿主壳**。
目标是在真实站点（deepseek、百度、Vue 应用……）上对齐现代浏览器行为。

## 2. 代码在哪

| 层 | 位置 | 职责 |
|---|---|---|
| 宿主壳 | `examples/browser_shell/main.c` | 标签页/地址栏/查找/书签/快捷键；`--probe` 真无头探针 |
| Win32 控件 | `src/webview_control/AgaveWebView.c` | 控件窗口、引擎线程、`AGW_CMD_*` 队列、剪贴板/IME。**命令执行期间持有视图引用**，`agave_webview_destroy()` 因此会推迟到引擎线程释放（见 R61） |
| 视图 + WebCore | `src/webview/`、`src/webcore/` | MaCross 适配、帧泵、页面时序；WebKit 2008 解析/DOM/CSS/布局/绘制/网络 |
| JS 桥 + 运行时 | `src/javascript/ant_backend/` + ant 的 `bridge/` | `AntRuntime.cpp`（realm/桥外观）、`prelude.js`/`polyfills.js`/`internal.js`；`AntBridge.c` 在 ant 仓库 |
| ant 引擎 | `H:\ant-v16`（独立仓库） | silver VM/编译、ESM loader、Worker、wasm、antjsb；见 [ref-ant-engine.md](ref-ant-engine.md) |

仓库根目录：

| 目录 | 内容 |
|---|---|
| `src/` | 引擎源码（见上表） |
| `examples/` | `browser_shell`（桌面壳）、`view_host`（无窗口渲染出 BMP）、`embed_demo`、`touchweb` |
| `include/` | 公共头：`agave_webview.h`、http/wtf；`AntBridge.h` 在 `3rdparty/ant/include` |
| `tools/` | 脚本、`cdb.exe`、`pages/`（被跟踪的测试页） |
| `docs/` | 全部文档，入口 [README.md](README.md) |
| `debug/` | 本地调试页（`.gitignore` 忽略，见 [dev-conventions.md](dev-conventions.md)） |
| `3rdparty/` | ant 预编译产物与第三方库（`antjsb.dll` 分发副本） |
| `build-zig-native/` | **唯一可运行目录**：exe + dll + `prelude.js`/`polyfills.js` |
| `dist/`、`packages/`、`vcproj/`、`test/`、`unit_tests/` | 打包模板 / 第三方源码包 / VS 工程 / 演示宿主与单元测试 |

一次导航的时序和模块管线见 [arch-overview.md](arch-overview.md) §4 与
[arch-module-loading.md](arch-module-loading.md)。

## 3. 我要改 X，去改哪里

| 想改 | 改这里 | 备注 |
|---|---|---|
| `window`/`document` 辅助、XHR/fetch、storage、`matchMedia`、`Worker` | `src/javascript/ant_backend/prelude.js` | **热改**：拷到 `build-zig-native\` 即生效，不重链 |
| DOM 构造器补丁（`append`/`classList`/`instanceof Node`/构造器 stub） | `src/javascript/ant_backend/polyfills.js` | 热改；每 realm 执行一次 |
| DOM 桥（节点属性/方法/事件/构造函数） | `H:\ant-v16\bridge\AntBridge.c`（wrapper 表 + 分发）+ `AntRuntime.cpp`（`nodeGetter`/`wrapNode`/类表） | 改完 `build_antjsb.ps1` + relink |
| HTML 解析、`script`/module 时序 | `src/webcore/html/HTMLTokenizer.cpp` | 等待位/`scriptExecution` 在这里 |
| CSS 解析、选择器 | `src/webcore/css/CSSParser.cpp`、`CSSStyleSelector.cpp` | 词法/语法老，改前先看 `AGENTS.md` 里 `var()` 的写法 |
| computed style | `src/webcore/css/CSSComputedStyleDeclaration.cpp` | 未实现属性默认静默（`AGB_CSS_DEBUG` 才打） |
| 布局 / 间距 / 元素 0×0 | 先看 [arch-rendering.md](arch-rendering.md) §3/§7；代码在 `src/webcore/rendering/` | flex 横向只有一套算法 `RenderFlexibleBox::layoutFlexLines()`（单行与换行共用，`AGB_FLEX_DEBUG=1` 可看分配） |
| 绘制 / 裁剪 / 渐变 | `src/webcore/platform/picasso/GraphicsContextPS.cpp` | picasso 后端；`clip` 是设备坐标 |
| 网络（HTTP/缓存/代理） | `src/webcore/platform/network/native/` | WinHTTP 后端 + `NetLog.cpp` |
| 页面生命周期钩子（willClose/commit/DCL/load） | `src/webview/FrameLoaderClientMC.cpp` | 时序问题的第一现场；`[evt]` 钩子也在这里 |
| 默认日志收口（`[evt]`/`[nav]`/`[ui]`/`[shell]`/`[probe]`/`[flag]`） | `src/webview/AgbLog.h` + 各调用点 | 只加日志，不动热路径；回归 `tools/run_nav_tests.ps1` |
| 快捷键 / 标签页 / 地址栏 / 探针 | `examples/browser_shell/main.c` | `normalizeUrl()` 是 URL 唯一入口 |
| 控件窗口 / 剪贴板 / IME / 帧泵定时器 | `src/webview_control/AgaveWebView.c` | 跨线程规则见 [dev-debugging.md](dev-debugging.md) |
| JS 语义 / VM / 编译 / ESM / worker / wasm | `H:\ant-v16\src\{silver,esm,modules}` | 见 [ref-ant-engine.md](ref-ant-engine.md) |

## 4. 症状 → 先查哪（别一上来就怀疑页面）

| 症状 | 先查 |
|---|---|
| 白屏 / 元素缺失 | 探针**退出码** → `[antjs]` 错误 → `AGB_ERR_SNIPPET=1` 拿源码位置 |
| `undefined is not a function` / `Cannot read properties of undefined` | 桥接缺方法或 wrapper 的 catch-all getter（`nodeGetter`），先于怀疑页面；已知缺口查 [arch-capability-gaps.md](arch-capability-gaps.md) |
| 某个 Web API / 选择器不生效，但页面在别的浏览器正常 | 先查 [arch-capability-gaps.md](arch-capability-gaps.md)（`cap_*` 页可一条命令复测） |
| 样式不对 | `getComputedStyle` 是否走到、属性是否"未实现"；跑 `debug/csscheck.html` |
| 布局不对（位置/间距/元素 0×0/滚动区） | [arch-rendering.md](arch-rendering.md) §7；先用 `__agbLayoutInfo()` 分清"没 renderer"和"有 renderer 没布局" |
| 画错 / 残影 / 不重绘 | 隐藏窗口没有 WM_PAINT（用 `--probe AGB_PROBE_SHOW=1` 或 `agave_view_host` 出 BMP）；再看 clip/repaint |
| 卡住 / CPU 100% | `AGB_PROFILE=1` 直方图 + `cdb` 抓忙线程栈（[dev-debugging.md](dev-debugging.md)） |
| 崩溃 / `0xC0000005`，尤其**只与时长相关**（长循环、长 eval、大脚本） | 先查**生命周期**不是优化器：宿主是否在引擎线程还在跑 `AGW_CMD_EVAL` 时销毁了视图（`tools/run_jit_loop_tests.ps1`，[hist-lessons.md](hist-lessons.md) R61）；再 `cdb` 断首次异常。**`AGB_GC_OFF=1` 一关就不崩 → GC 扎根**（R63，ant callback builtin）；调试器里出现 `HEAP: Free Heap block ... modified` → 开 `AGB_GUARD_MALLOC=512-4096` 抓写入者（R62） |
| 网络没提交 / 一直重试 | `AGB_NET_LOG=1` 看 stage；`progress finished=1` **不代表**加载成功 |
| 崩溃 | `cdb` 断在首次异常抓全线程栈；worker 崩在**非主线程**（`worker_thread_main`） |
| 刷新后行为变了 | realm/监听器泄漏（[hist-lessons.md](hist-lessons.md) R10/R17） |
