# 单实例 shell：profile 锁、命令接力与同进程多窗口（设计记录）

> **设计记录，不是现状。** 现状见 [arch-overview.md](arch-overview.md) §1（宿主壳）。
> 决策日期 2026-11-11；实现与验收入口见文末。

## 1. 要解决的问题

现在同一个 profile 可以开任意多个 `agave_shell.exe`，它们共享同一批磁盘状态：

- `%APPDATA%\Agave\{cookies,localstorage}.txt` 启动读一次、变更整文件重写（无锁、无 rename）；
- `%APPDATA%\Agave\module-cache\<hash>.mjs` 直接 `fopen("wb")`；
- `%LOCALAPPDATA%\Agave\http-cache` 的 `.tmp`、`cache\media` 的 URL 哈希文件、`downloads` 的 `<name>.part`；
- `%APPDATA%\AgaveShell\{bookmarks,history,session}.txt`。

两个进程同时写就是丢更新、半截模块（还会按 TTL 缓存 1 小时）、下载互踩。Chrome 的做法是
**一个 user-data-dir 一个浏览器进程**：后来者不自己开窗口，而是把「打开这个 URL」的请求
接力给已有进程，然后自己退出。窗口本身也属于同一个浏览器进程（`--new-window` 只是多一个
窗口，不是多一个进程）。

## 2. 目标 / 非目标

目标：

1. 同一个 profile 根只有一个 shell 进程；第二个启动的 `agave_shell.exe` 投递请求后退出（退出码 0）。
2. `URL` / `--new-tab [URL]` → 最近活跃窗口里新增标签页；`--new-window [URL]` → **同一进程**里新开窗口；无 URL/无参数 → 激活已有窗口。
3. `--userdata` 不同 = 不同 profile = 各自实例（和 Chrome 的 `--user-data-dir` 一样）。
4. `--probe` 永不参与单例（测试/并行探针保持现状）。
5. 失败要说话：投递超时、版本不符、旧实例不响应 → stderr 一行 + 非 0 退出码，不静默吞。

非目标（本轮不做）：

- 跨登录会话互斥：命名对象用 `Local\`（会话内），快速用户切换下两个会话各有一份。
- session 恢复保持「多窗口结构」：先扁平恢复成一个窗口。
- `window.open` / `target=_blank` 改成新窗口：现状仍是新标签页。
- takeover（旧实例崩溃后接管它的窗口）。

## 3. 身份：一个 profile 根 = 一把锁 + 一个投递窗口

- **profile 根** = `g_dataDir`：`--userdata` / `AGB_USERDATA` 给出时就是它，否则是
  `%APPDATA%\AgaveShell`（或 `%LOCALAPPDATA%\AgaveShellData` / exe 旁兜底）。
- 规范化：`GetFullPathNameA` + 全小写 + 去尾部 `\` / `/`，FNV-1a 64 得 `h`。
- 锁：`CreateMutexA(NULL, FALSE, "Local\\agave_shell_profile_<16hex>")`，`GetLastError() ==
  ERROR_ALREADY_EXISTS` 即「已有实例」。互斥体随进程退出自动释放，不会留下 stale 锁。
- 投递目标：主实例创建一个**不显示的窗口**，类名 `AgaveShellHandoff`、标题
  `agave-shell-<16hex>`（`FindWindowA` 用它定位）。不同 profile 标题不同，不会串台。

为什么两样都要：互斥体是「谁在跑」的权威判据（进程死即释放）；窗口是「往哪投递」的地址
（互斥体不可发送消息）。只用窗口会在启动竞态里找不到人；只用互斥体会拿到锁却没有投递地址。

## 4. 启动时序

```text
parseShellArgv
  ├─ --probe      → probeRun（不建浏览器窗口，不碰单例）
  └─ GUI
       shellStateInit            // 定 g_dataDir + 读 bookmarks/history
       CreateMutex(profile)
         ├─ 已存在 → handoffClient()  // FindWindow 重试 → SendMessageTimeout →
         │                             // 读 ack → stdout 一行 → ExitProcess(0/3)
         └─ 新建
              createShellWindow()   // 窗口 + 工具栏（还没有标签页）
              createHandoffWindow() // 隐藏窗口，FindWindow 能找到
              g_handoffReady = 1    // 之后到的请求立即执行
              createTab(初始 URL / session)
              ShowWindow（AGB_SHELL_HIDDEN=1 时跳过）
              消息循环
```

投递窗口在**建标签页之前**创建，所以第二个实例不必等页面加载；如果主实例还没
`g_handoffReady`，它先回 `state=starting`，客户端每 100 ms 重试（上限 15 s）。

## 5. 投递协议 v1

请求（`WM_COPYDATA`，`dwData = 'ASH1'`，数据在发送方栈上，接收方必须当场复制）：

```c
typedef struct {
    DWORD size;                 /* sizeof(ShellHandoff) */
    DWORD version;              /* 1 */
    DWORD op;                   /* 0 = focus, 1 = new tab, 2 = new window */
    DWORD pid;                  /* sender (diagnostics) */
    char  ackName[64];          /* "Local\\agave_shell_ack_<pid>_<n>" */
    char  url[MAX_PATH_CHARS];  /* may be empty (focus / blank tab) */
} ShellHandoff;
```

应答（**发送方**建的一次性命名共享段，接收方 `OpenFileMapping(FILE_MAP_WRITE)` 后写一段
UTF-8 文本，发送方 `SendMessage` 返回后读）：

```text
state=ok pid=1234 op=new-tab windows=2 target=0x000A1234 tabs=2
win0 hwnd=0x000A1234 tabs=1 active=about:blank
win1 hwnd=0x000B5678 tabs=2 active=file:///.../page.html
```

- `state=starting`：主实例还在启动，客户端重试。
- `state=error` + `err=...`：版本不符 / 请求被拒。
- 客户端用 `SendMessageTimeoutW(..., SMTO_ABORTIFHUNG, 10s)`：主实例卡死时不会把第二个
  进程无限挂住；超时按失败处理（stderr + exit 3）。

## 6. 命令行语义

| 第二个实例的命令行 | 已有实例时 | 没有实例时 |
|---|---|---|
| `URL` | 最近活跃窗口新增 tab 并激活 | 新进程、单窗口、URL tab |
| `--new-tab [URL]` | 同上；无 URL 则 `about:blank` tab | 新进程、单窗口 |
| `--new-window [URL]` | **同进程**新窗口（+URL tab），并激活 | 新进程、单窗口 |
| 无 URL / 无参数 | 激活（restore + 前台）已有窗口，不新增 tab | 新进程、恢复 session |
| 其它开关（`--backend` 等） | 忽略（已有实例的后端不变），不报错 | 正常生效 |
| `--probe` | 不投递、不建窗，照常跑 | 同左 |

最近活跃窗口 = 最后一次 `WM_ACTIVATE` 的窗口，或最后一次被投递目标/新建的窗口；
隐藏模式下（`AGB_SHELL_HIDDEN=1`）不调用 `SetForegroundWindow`。

## 7. 同进程多窗口模型

一个进程里最多 `MAX_SHELL_WINDOWS` 个窗口，每个窗口一份：

```c
typedef struct ShellWindow {
    int    used;
    HWND   hwnd, back, forward, reload, stop, address, go, newtab, menu;
    HWND   findEdit, findNext, findPrev, findClose;
    Tab    tabs[MAX_TABS];       /* Tab 带 ShellWindow* window 回指 */
    int    activeTab, zoom, findVisible, minimized;
    char   status[1024];
    ClosedTab closed[MAX_CLOSED_TABS];
    int    closedCount;
} ShellWindow;
```

- 窗口表 + UI 线程的当前上下文 `g_cur`。原来 `g_hwnd`/`g_tabs`/`g_activeTab`/`g_zoom`/…
  这些逐窗口全局改为 `g_cur->...` 的宏，函数体基本不动（宏展开成成员访问，
  `sizeof(g_status)` 之类仍然成立）。
- **每个入口先定位上下文**：`WM_NCCREATE` 从 `CREATESTRUCT.lpCreateParams` 取窗口对象写
  `GWLP_USERDATA`；`WndProc` 进入时按 hwnd 定位；控件回调（`user` 是 `Tab*`）通过
  `tab->window` 定位；消息循环里按 `msg.hwnd` 的根祖先定位。模态对话框/菜单的嵌套消息循环
  保存并恢复 `g_cur`。
- `view → (window, tab)` 是全窗口表查询，`AGW_SHELL_*` 消息（带 view 指针）据此路由。
- 关窗：移出窗口表 + 释放窗口对象；**最后一个**窗口关闭才 `PostQuitMessage`，此时
  `saveSession()` 把所有**仍打开**窗口的标签页扁平写盘（重启后一个窗口恢复；先关掉的窗口的
  tab 就和 Chrome 一样不进入 session）。
- 多窗口的 Page Visibility：每个窗口只有自己的 active tab 可见；窗口最小化只影响自己。

## 8. 失败模式与测试钩子

| 情况 | 行为 |
|---|---|
| 主实例启动慢 | 客户端 `FindWindow` 重试 15 s，收到 `state=starting` 每 100 ms 重投 |
| 主实例 UI 线程卡死 | `SendMessageTimeout` 10 s 超时 → 客户端 stderr + exit 3 |
| 主实例已退出（无锁） | 第二个进程成为主实例 |
| profile 路径写法不同（`X` / `X\` / `X\.`） | 规范化后同一把锁 |
| `AGB_SHELL_HIDDEN=1` | 不 `ShowWindow`、不抢前台；供自动化（实例仍然真实存在） |

## 9. 验收

自动化（`tools/run_singleton_tests.ps1`，全部 `--probe` + 文件 mock，约 2 s）只验**逻辑**，
不操控 UI、不创建窗口/控制台/壳进程：

1. profile 身份：`X`、`X\`、`X\.` 是同一个 key；不同目录是不同 key。
2. 决策：无 URL → `op=focus`；裸 URL / `--new-tab` → `op=new-tab`；`--new-window` →
   `op=new-window`（URL 一并带上）。
3. 请求编组：`key` / `op` / `opname` / `url` 落到 mock 的 `request.txt`。
4. ack：`state=ok` → 打印并 exit 0；`state=starting` → 重试到 ok；`state=error` → exit 3；
   没有 ack → 在 `AGB_HANDOFF_WAIT_MS` 内失败（不挂住）。
5. 钩子是 opt-in：普通 `--probe` 的输出里不出现 `[handoff]`。

实现（测试钩子，不改变产品路径）：`AGB_PROBE_HANDOFF=1` 让 probe 走
`handoffProbeMain()`（不建消息窗口、不建互斥体），`AGB_HANDOFF_MOCK_DIR` 用文件 mock
运行中的壳（和 `AGB_CLIPBOARD_FILE` mock 剪贴板同一种手法）。

**不自动化**（UI/进程行为；手动用 `AGB_SHELL_HIDDEN=1` 跑，窗口建而不显示）：

- 真实 `WM_COPYDATA` 接力、加 tab、同进程新窗口、最近活跃窗口选择；
- 关一个窗口不退进程、关最后一个才退出并保存 session；
- `process` 后端下接力（每个 tab 一个 renderer）。

套件用 `AGB_SHELL_HIDDEN=1` 跑，窗口不显示、不抢焦点；每个用例独立 `AGB_USERDATA`。
预计 10–15 s（GUI 进程启动 + 投递，串行——同一桌面抢前台会让判定不稳定）。

## 10. 风险与已接受的取舍

- **默认 profile 的行为变了**：以前第二份是独立实例，现在会投递并退出。想并行多开就显式给
  不同 `--userdata`（测试套件本来就这么做）。
- **多窗口共用一个 UI 线程**：某个窗口的同步 UI 操作会挡住其他窗口（引擎线程本来就共用）；
  与 Chrome 的「浏览器进程单线程 UI」同构，不是新问题。
- **`Local\` 不是 `Global\`**：不同登录会话可以各跑一个实例；同一 profile 跨会话仍可能互相写，
  与现状相同（文档记录为限制，不扩大范围）。
- **session 结构丢失**：多窗口重启后合成一个窗口（进 [plan-roadmap.md](plan-roadmap.md)）。
- 第二实例的 `--backend` / `--zoom` 等开关不透传：它只是请求方，已有实例的后端/状态说了算
  （不报错；第二实例的 argv 仍会打进它自己的 stderr 日志）。

## 11. 代码锚点（实现后补）

- 单例/投递：`examples/browser_shell/main.c` 的 `profileKey()`、`handoffClient()`、
  `createHandoffWindow()`、`handoffProc()`。
- 窗口模型：`ShellWindow`、`g_cur`、`createShellWindow()`、`windowForHwnd()`、`windowForView()`。
- 测试：`tools/run_singleton_tests.ps1`；现状文档：[arch-overview.md](arch-overview.md) §1。
