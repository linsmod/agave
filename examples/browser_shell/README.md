# Agave BrowserShell

桌面浏览器外壳，**基于 `agave_webview` 控件构建**：每个标签页就是一个
`AgaveWebView` 子窗口，shell 本身只负责 UI（标签栏、工具栏、地址栏、
查找栏、状态栏、书签/历史/会话持久化）。

引擎线程、渲染缓冲、输入转发、缩放、popup 都在控件内部；
shell 不直接调用任何 `macross_*` 接口 —— 它和第三方宿主用的是同一套 API。

```
+-------------------------------------------------------------+
| <  >  R  X | [地址栏.......................................] Go  + |
+-------------------------------------------------------------+
| [1  标题/URL              x] [2  标题/URL              x] ... |
+-------------------------------------------------------------+
|                                                             |
|                    网页（引擎直接画进 DIB）                   |
|                                                             |
+-------------------------------------------------------------+
| 状态栏：加载进度 / 悬停链接 / 错误 / 下载路径                 |
+-------------------------------------------------------------+
```

## 构建

```
cmake -S . -B build-zig-native -DOPT_JS_ANT=ON -DOPT_NET_BACKEND=native -DOPT_SHELL=ON
cmake --build build-zig-native --target agave_shell
```

产物 `build-zig-native/agave_shell.exe`，需要同目录有 `libagave.dll` 和 `antjsb.dll`。

## 线程模型

UI 线程只负责窗口/消息循环；引擎线程、命令队列和回调线程契约由控件拥有，
完整说明见 [arch-embedding.md](../../docs/arch-embedding.md)。

* 输入、导航、缩放等都进一个加锁的命令队列（`ShellCommand`），UI 线程**从不**直接调用引擎 API；
* 引擎回调只把标题/URL/进度写进对应标签的字段，然后 `PostMessage(WM_SHELL_STATE)`；
* **DIB 归引擎线程所有**：UI 只记录"想要多大"，由引擎线程创建/重建缓冲区并调用
  `macross_view_resize`。否则 UI 释放缓冲区时引擎可能还在往里画（会黑屏/崩溃）。

实测：163.com 加载期间 UI 线程的 `WM_NULL` 响应在 0.1~14 ms，不再卡死。

## 功能

| 操作 | 说明 |
|---|---|
| 地址栏 + Enter | 导航；不含点号/协议时按搜索词处理 |
| `+` / Ctrl+T | 新标签页 |
| 点标签 | 切换；点标签右侧 `x` 关闭 |
| `<` `>` / Alt+←→ | 后退 / 前进 |
| `R` / F5 | 刷新 |
| `X` / Esc | 停止 |
| Ctrl+L / F6 | 聚焦地址栏 |
| Ctrl+F | 查找栏（回车执行 `macross_view_search_text`） |
| 右键菜单 | 后退/前进/刷新/停止/新标签/查找 |
| 滚轮、点击、键盘 | 全部转发给页面（中文经 `WM_CHAR` 转 UTF-8） |
| 下载 | 引擎 DownloadManager 写盘，状态栏显示进度/结果；保存到 `%USERPROFILE%\Downloads` |
| Ctrl+D | 加书签（存 `<数据目录>\bookmarks.txt`） |
| `=` 按钮 / 菜单 | 书签、历史、缩放、注入调试脚本 |
| 右键菜单 → 注入 vConsole | 从 exe 同目录读 `vconsole.min.js` 注入当前页（默认注入目标） |
| 右键菜单 → 注入 inspector.js | 从 exe 同目录读 `inspector.js` 注入当前页：hover 高亮元素 + 半透明信息浮层 |
| 探针 `--inject <name\|path>` | `--probe` 时在 `--eval` 之前注入（`inspector` / `vconsole` / 文件路径） |
| Ctrl+= / Ctrl+- / Ctrl+0 | 放大 / 缩小 / 重置（25% 步进，状态栏显示百分比） |
| 会话恢复 | 退出时把打开的标签写 `session.txt`，下次无参启动自动恢复 |

数据目录优先 `%APPDATA%\AgaveShell`，不可写时退回 exe 同目录的
`AgaveShellData`（便携模式）。

## 注入调试脚本

右键菜单和 `=` 菜单各有两个入口：`注入 vConsole`（默认）和 `注入 inspector.js`。
两者都是**把 exe 同目录下的文件读出来，再 `agave_webview_eval` 到当前页**，所以：

- 改脚本不用重编：改 `build-zig-native\inspector.js`，再点一次菜单即可（等价于 prelude 的热改）；
- 文件缺失时状态栏会提示 `Cannot read ...`，不是静默失败；
- `AGB_INSPECTOR=<path>` 可以换 `inspector.js` 的读取路径（探针/多份副本用）；
- 脚本长度不再受 8192 字节限制：控件队列对超长 payload 走堆分配（`AgwCommand.heapText`）。
- 探针复现：`agave_shell.exe --probe <url> --inject inspector --eval "..."`；
  `--inject inspector|vconsole` 走的就是菜单那两个函数，所以连实例化步骤一起覆盖。
- vConsole 是 UMD：本页 realm 有 prelude 的 AMD `define`，bundle 会注册成模块
  `VConsole` 而不是 `window.VConsole`。外壳把 `require('VConsole')` 的实例化语句
  **拼在同一个 eval 的末尾**（`injectScriptAsset` 的 `suffix`），注册与实例化之间
  不依赖命令队列的时序。

`inspector.js` 的行为：hover 时在 `<html>` 下放一个绝对定位的框 + 半透明信息浮层
（`z-index` 最大），**不参与文档流**；Esc 退出；再次注入等于关闭。它靠
`elementFromPoint` 命中元素，命中前把自己的浮层 `display:none`（本引擎的 HTML
不认 `pointer-events`，否则浮层会挡住 hover）。

## 踩过的坑

1. **地址栏 EN_CHANGE 死循环**：`SetWindowText` 会立刻回调，若在 `WM_COMMAND`
   里导航就无限递归爆栈。地址栏改为子类化，只处理 `VK_RETURN`。
2. **重复创建 view 崩溃**：`WM_SIZE` 与 `WinMain` 各建一次。现在尺寸未变直接返回。
3. **缓冲区所有权**：早期 UI 线程重建 DIB，引擎还在画旧指针 → 内容全黑。
   现在 DIB 只在引擎线程创建/销毁。
4. **新建标签必须走 UI 线程**：标签数组是 UI 状态，命令只负责让引擎建 view。

## 与 examples/touchweb 的关系

`examples/touchweb` 是完整手机浏览器（标签页/历史/收藏/设置/自动填表），
已在 `-DOPT_EXAMPLES=ON` 下能编译通过；它的 UI 是触摸式、按 480x503 设计。
本目录是同一引擎上的桌面外壳。
