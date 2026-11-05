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
| `libagave_webview.dll` | 控件 DLL（第三方只需这个 + 头文件） |
| `libagave_webview.dll.a` | MinGW 导入库 |
| `agave_embed.exe` | 演示宿主 |

宿主编译时定义 `AGW_USE_DLL` 并链接导入库；运行时把
`libagave_webview.dll`、`libagave.dll`、`antbridge.dll` 放到 exe 旁边。

## API 一览

| 分类 | 函数 |
|---|---|
| 生命周期 | `agave_webview_create` / `agave_webview_destroy` / `agave_webview_set_rect` / `agave_webview_hwnd` |
| 导航 | `agave_webview_navigate` / `load_html` / `back` / `forward` / `reload` / `stop` |
| 脚本 | `agave_webview_eval` |
| 状态 | `agave_webview_url` / `agave_webview_title` |
| 显示 | `agave_webview_set_zoom` / `agave_webview_zoom` |
| 其它 | `agave_webview_set_focus` / `set_user_agent` / `set_download_dir` / `set_log` |

回调：`on_title`、`on_url`、`on_progress`、`on_status`、`on_error`、`on_download`、`on_new_window`（保留，引擎当前不触发）。

## 线程模型

| 线程 | 做什么 |
|---|---|
| 宿主线程 | 拥有 HWND；输入通过 `PostMessage` 进控件，控件再转成命令入队 |
| 引擎线程（进程内共享一条） | `macross_initialize`、所有 `MaCrossView`、命令队列、`event_dispatch`、`view_update` |

* 宿主线程**不会**被页面布局阻塞（实测 163 加载期间 UI 消息仍是毫秒级）；
* 回调在引擎线程触发，**不要**在回调里做同步跨线程等待；下载状态是例外——控件深拷贝后
  `PostMessage`，`on_download` 在 UI 线程派发（见 [arch-downloads.md](arch-downloads.md) §7）；
* `navigate` / `eval` / `set_zoom` 是入队操作，任何线程调用都安全；
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
