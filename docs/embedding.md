# AgaveWebView —— 可嵌入的 WebView 控件

给第三方程序用的最小内核接口：**一个子窗口 + 一套 C API**。宿主只要照常用自己的消息循环，剩下的（引擎线程、渲染缓冲、输入转发）都在控件内部。

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

宿主链接 `agave_webview` + `libagave.dll.a`，并把 `libagave.dll`、`antbridge.dll` 放到 exe 旁边。

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
* 回调在引擎线程触发，**不要**在回调里做同步跨线程等待；
* `navigate` / `eval` / `set_zoom` 是入队操作，任何线程调用都安全；
* 每个控件独立：独立 `MaCrossView`、独立 DIB、独立进度与标题。

## 多控件

`macross_*` 的回调是进程级的，控件层注册一次并按 `MaCrossView*` 路由到对应实例，
所以一个进程里可以同时存在任意多个控件（上限 32）。演示见 `examples/embed_demo`：
一个窗口里两个控件，左边本地页、右边 163.com，互不影响。

## 已知限制

* 只支持 Windows（子窗口 + DIB）；
* 每个控件一个引擎 view，但 `macross_initialize` 是进程级，只初始化一次；
* `on_new_window` 目前不会被触发（引擎没有暴露 window.open 回调）；
* 控件不处理右键菜单，宿主可以自己弹（或等我们补）。
