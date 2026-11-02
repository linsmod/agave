# Agave BrowserShell

一个最小的 Win32 浏览器外壳，直接跑在 `macross_*` C API 上。

```
+---------------------------------------------------+
| <  >  R  X | [地址栏................................] Go |
+---------------------------------------------------+
|                                                   |
|                  网页渲染区域                      |
|            （引擎直接画进 DIB section）             |
|                                                   |
+---------------------------------------------------+
| 状态栏：标题 / 进度 / 悬停链接                      |
+---------------------------------------------------+
```

## 构建

```
cmake -S . -B build-zig-native -DOPT_JS_ANT=ON -DOPT_NET_BACKEND=native -DOPT_SHELL=ON
cmake --build build-zig-native --target agave_shell
```

产物：`build-zig-native/agave_shell.exe`（需要同目录有 `libagave.dll` 和 `antbridge.dll`）。

## 用法

```
agave_shell.exe [url]
```

不带参数默认打开 `http://www.163.com`。

## 功能

| 操作 | 说明 |
|---|---|
| 地址栏 + Enter | 导航；输入不含点/协议栈时按搜索词处理（百度） |
| `<` `>` | 后退 / 前进 |
| `R` | 刷新 |
| `X` | 停止加载 |
| `Go` | 等同于地址栏回车 |
| F5 / Esc | 刷新 / 停止 |
| Alt+← / Alt+→ | 后退 / 前进 |
| Ctrl+L 或 F6 | 聚焦地址栏并全选 |
| 鼠标 | 点击、拖动、滚轮都转发给页面 |
| 键盘 | 全部转发给页面（中文输入走 WM_CHAR → UTF-8） |

## 实现要点

* 内容区是一个 32 位顶朝下 DIB section，引擎直接渲染进去，`WM_PAINT` 用 `BitBlt` 贴到窗口——没有额外的内存拷贝。
* 8ms 定时器里跑 `macross_event_dispatch()` + `macross_view_update()`；引擎内部按脏矩形决定要不要真的重画。
* 地址栏只响应回车（子类化拿 `VK_RETURN`）。**不要**在 `WM_COMMAND` 里处理 `EN_CHANGE`：`SetWindowText` 会立刻回调，导致无限递归。
* 窗口尺寸变化时重建 DIB 并 `macross_view_resize`；尺寸没变则跳过（重复创建 view 会导致崩溃）。

## 与 examples/touchweb 的关系

`examples/touchweb` 是完整的手机浏览器（标签页、历史、收藏、设置、自动填表），
已经在 `examples/examples.cmake` 里接好，但它依赖原 IDE 工程的若干 typedef/宏
（`uchar_t`、`ustring`、`DATA_PATH`…），目前编译不过，需要单独适配。
在它可用之前，本目录提供一个能立刻用的桌面外壳。
