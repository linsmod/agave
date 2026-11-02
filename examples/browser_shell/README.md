# Agave BrowserShell

桌面浏览器外壳，跑在 `macross_*` C API 上。双线程：UI 与引擎互不阻塞。

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

产物 `build-zig-native/agave_shell.exe`，需要同目录有 `libagave.dll` 和 `antbridge.dll`。

## 线程模型

| 线程 | 负责 |
|---|---|
| UI 线程 | 窗口、控件、消息循环、把当前标签的位图 `BitBlt` 到窗口 |
| 引擎线程 | 持有全部 `MaCrossView`，消费命令队列，跑 `macross_event_dispatch()` + `macross_view_update()` |

UI 线程**从不**直接调用引擎 API：

* 输入、导航、缩放等都进一个加锁的命令队列（`ShellCommand`）；
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
| 下载 | `cb_write_file_data` 直接写入 `%USERPROFILE%\Downloads` |
| Ctrl+D | 加书签（存 `<数据目录>\bookmarks.txt`） |
| `=` 按钮 / 菜单 | 书签、历史、缩放 |
| Ctrl+= / Ctrl+- / Ctrl+0 | 放大 / 缩小 / 重置（25% 步进，状态栏显示百分比） |
| 会话恢复 | 退出时把打开的标签写 `session.txt`，下次无参启动自动恢复 |

数据目录优先 `%APPDATA%\AgaveShell`，不可写时退回 exe 同目录的
`AgaveShellData`（便携模式）。

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
