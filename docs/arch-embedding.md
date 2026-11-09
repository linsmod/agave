# AgaveWebView —— 可嵌入的 WebView 控件

给第三方程序用的最小内核接口：**一个子窗口 + 一套 C API**。宿主只要照常用自己的消息循环，剩下的（引擎线程、渲染缓冲、输入转发）都在控件内部。

⚠ 宿主的消息循环必须用 **Unicode 版**：`GetMessageW` + `TranslateMessage` + `DispatchMessageW`。
控件窗口是 Unicode 窗口（`RegisterClassW`），字符经 `WM_CHAR` / `WM_IME_CHAR` 以 UTF-16 码元到达；
`GetMessageA`/`DispatchMessageA` 会把它们转成 ANSI 代码页字节，中文输入法打出来就是乱码
（`ÖÐÎÄ`）。宿主自己的 ANSI 窗口不受影响 —— `DispatchMessageW` 会按目标窗口自动转换。

```c
#include "agave_webview.h"

AgaveWebViewCallback cb = {0};
cb.on_title    = on_title;
cb.on_url      = on_url;
cb.on_progress = on_progress;
cb.on_error    = on_error;

AgaveWebView* view = agave_webview_create(hwnd, 0, 0, 800, 600, &cb, NULL);
agave_webview_navigate(view, "https://example.com");
...
agave_webview_destroy(view);
```

## 构建

```
cmake -S . -B build-zig-native -DOPT_SHELL=ON
cmake --build build-zig-native --target agave_webview   # 静态库
cmake --build build-zig-native --target agave_embed     # 双控件演示
```

产物：

| 文件 | 用途 |
|---|---|
| `libagave.dll` | 引擎 + Win32 控件（`agave_webview_*` 和 `macross_*` 都在这里） |
| `libagave.dll.a` | MinGW 导入库；宿主只需链接这个 |
| `antjsb.dll` | 底部 JS 引擎适配层，由 `libagave.dll` 自动依赖 |
| `agave_embed.exe` | 演示宿主 |

宿主编译时定义 `AGW_USE_DLL` 并链接 `libagave.dll.a`；运行时把
`libagave.dll`、`antjsb.dll` 放到 exe 旁边。不再有单独的 `libagave_webview.dll`。

三个物理件对应三层：`libagave.dll`（渲染内核 + 控件）、`antjsb.dll`（JS 引擎层）、
exe 旁的 `prelude.js` / `polyfills.js`（运行时 JS 资产，可热替换）。分层与依赖方向见
[arch-overview.md](arch-overview.md) §2.1。

## API 一览

| 分类 | 函数 |
|---|---|
| 生命周期 | `agave_webview_create` / `agave_webview_destroy` / `agave_webview_set_rect` / `agave_webview_hwnd` |
| 导航 | `agave_webview_navigate` / `load_html` / `back` / `forward` / `reload` / `stop` |
| 脚本 | `agave_webview_eval` |
| 状态 | `agave_webview_url` / `agave_webview_title` |
| 显示 | `agave_webview_set_zoom` / `agave_webview_zoom` |
| 其它 | `agave_webview_set_focus` / `set_user_agent` / `set_download_dir` / `set_log` |

回调：`on_title`、`on_url`、`on_progress`、`on_status`、`on_error`、`on_crashed`、`on_message`、
`on_dialog`、`on_context_menu`、`on_close_window`、`on_unresponsive`、`on_command_dropped`、
`on_download`、`on_new_window`（保留，引擎当前不触发）。

**窗口生命周期**（R158）：`<a target>` / `<form target>` / `window.open(url, name)` 的判定在引擎
（`HTMLAnchorElement`：中键、ctrl、cmd 等同 `_blank`；`_self`/`_top`/`_parent` 由 frame 树解析），
**具名窗口由控件登记**（`windowName` + `on_new_window` / `on_activate_view`）：名字命中就导航那个
view 并让宿主切过去，没命中才建新控件交给宿主开 tab。两个后端行为一致——process 后端里渲染进程
把请求变成 `SERVICE_REQUEST(open_window/close_window)` 事件，控件在引擎线程上完成同样的动作。
`window.close()` → `on_close_window`，宿主决定怎么关。

**入队命令的契约**（R157）：`agave_webview_eval` 这类入队操作没有返回值，所以"没送到"必须能被听见——
每条命令要么执行、要么回调一次 `on_command_dropped(view, kind, reason, user)`。原因只有三种：
视图还没有页面（`no page yet`）、后端里没有可用文档（`no live document`）、命令等到导航提交之后才
轮到自己（`document replaced by a navigation`，按文档代数丢弃，不会拿去脚本新页面）。导航/缩放/尺寸/
可见性不属于任何文档，永远不丢。

## 线程模型

| 线程 | 做什么 |
|---|---|
| 宿主线程 | 拥有 HWND；输入通过 `PostMessage` 进控件，控件再转成命令入队 |
| 引擎线程（进程内共享一条） | `macross_initialize`、所有 `MaCrossView`、命令队列、`event_dispatch`、`view_update` |

* 宿主线程**不会**被页面布局阻塞（实测 163 加载期间 UI 消息仍是毫秒级）；
* 回调在引擎线程触发，**不要**在回调里做同步跨线程等待；下载状态是例外——控件深拷贝后
  `PostMessage`，`on_download` 在 UI 线程派发（见 [arch-downloads.md](arch-downloads.md) §7）；
* `navigate` / `eval` / `set_zoom` 是入队操作，任何线程调用都安全；
* `agave_webview_destroy` 可以从任何线程调用，但引擎侧（`MaCrossView`、WebCore、Ant
  VM、surface 缓冲）**只在引擎线程拆**：调用方只标记 + 从活动表摘除 + 把控件挂进 reap
  队列，引擎线程在每轮 `event_dispatch()` 之后 `agwReapViews()`；窗口按**创建它的线程**
  销毁（tab 是宿主线程，popup 是引擎线程，跨线程时用 `AGW_MSG_RETIRE`）。控件销毁期间
  宿主会等一次回收完成（2 s 上限，卡在页面脚本里时靠看门狗 abort）。设计记录见
  [design-tab-teardown.md](design-tab-teardown.md)。
* 每个控件独立：独立 `MaCrossView`、独立 DIB、独立进度与标题。

## 多控件

`macross_*` 的回调是进程级的，控件层注册一次并按 `MaCrossView*` 路由到对应实例，
所以一个进程里可以同时存在任意多个控件（上限 32）。演示见 `examples/embed_demo`：
一个窗口里两个控件，左边本地页、右边 163.com，互不影响。

## 已知限制

* 只支持 Windows（子窗口 + DIB）；
* 每个控件一个引擎 view，但 `macross_initialize` 是进程级，只初始化一次；
* `on_new_window` 由 shim 的 `window.open` 驱动：控件创建好 popup 控件并
  交给宿主摆放（宿主可改位置/尺寸），宿主返回 0 表示阻止（控件自动销毁）；
  popup 窗口属于引擎线程，其消息由控件的引擎循环泵出；
* 每个控件有独立的 `RuntimeState` 和 ant engine（per-view JS 上下文，2026-11-03 起；
  文档切换时换新 realm），`window.foo` 不会跨控件泄漏；但所有控件仍共用一条引擎线程，重活会互相排队；
* `macross_initialize` 是进程级、只初始化一次；模块会话、`DownloadManager`、generation 计数器
  等少量资源是进程级共享的；
* 控件不处理右键菜单，宿主可以自己弹（或等我们补）。
* `<select>` 的下拉菜单**不需要宿主窗口**：引擎把菜单画进 view 自己的 surface，输入由
  `WebView` 优先路由给它（`macross_menu_*` 只留给注册了 `cb_create_popup_menu` 的老宿主）。
  细节与限制见 [design-select-popup.md](design-select-popup.md)。
