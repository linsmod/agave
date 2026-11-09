# virtpass_browser v1 —— RVVM 侧渲染后端接口规格（交付给 RVVM 仓库）

> **这是跨仓库接口规格，不是 agave 现状。** agave 侧的对应设计见
> [design-render-backend.md](design-render-backend.md)（S3，B3 批）；本文件定义
> `H:\github_repos\RVVM` 侧需要实现的 ABI 与两跳拓扑，供 RVVM 侧实现和评审。

## 1. 目标与非目标

**目标**：让 agave 的一个 view（tab）跑在 RVVM userland 实例里，Windows 侧只负责窗口、输入、
合成；网页引擎（DOM/布局/绘制/JS）在 guest 里。协议与 agave 已有的"渲染后端协议 v1"完全一致
（[design-render-backend.md](design-render-backend.md) §4），所以 agave 侧只换传输，不换语义。

**非目标**：不引入 GL 逐调用转发做 2D 光栅（实测结论：`bench_ecall`，见设计文档 §6）；
不改变 RVVM 现有的 NDK/cmdpost ABI（`test_render` 等继续用）；不做 one-VM-multi-view（B5 再议）。

## 2. 拓扑：两跳，各自职责

```
Win32 shell (agave_shell.exe / 宿主)
  │   A. 控制面：命名共享段 AgwIpcHeader（canvas + 命令环 + 事件环 + 文本 arena）
  │      + 两个命名事件 _evt（命令到达）/ _int（中断请求）；CreateProcess 拉起 winhost
  ▼
rvvm_winhost.exe（RVVM 侧，每 view 一个进程；A 的另一端由它扮演）
  │   B. 引擎面：virtpass_browser ecall（本文档定义）；guest 内存里的控制块
  ▼
guest: agave engine（riscv64 二进制，RVVM 的 `--app` 包）
```

- **A 跳**是 agave 已经实现并验证过的（`src/webview_control/AgwViewIpc.h` +
  `AgwViewBackendProcess.c`）：命名段布局、命令/事件环、帧背包（`frameSeq/ackSeq`）、
  `KILL_ON_JOB_CLOSE` 防孤儿。RVVM 侧只需在 winhost 里扮演"引擎端"这一半。
- **B 跳**是本文件新增的：guest 分配控制块（与 A 跳 **同一个** `AgwIpcHeader` 布局），
  通过 ecall 把 **guest 地址** 交给 winhost；winhost 用
  `rvvm_user_guest_ptr()`（`src/core/rvvm_user.h:655`）拿到 host 指针，直接读写这块内存。
- 两跳之间由 winhost 的 browser bridge 做**搬运**：
  - 命令/事件/文本：A 跳的命令环 → B 跳的 guest 块命令环（反之亦然），纯内存拷贝，量小；
  - 帧：guest canvas → A 跳段里的 canvas（**一次整幅拷贝**）。这不违反设计文档的"零拷贝"
    结论——零拷贝只在宿主与引擎同地址空间时成立；guest 内存属于 winhost 进程，
    shell 读不到，必须搬一次（M5 实测整幅拷贝 0.2–0.6ms，可接受）。
  - drop-in 可选优化（B3 之后）：winhost 只搬 damage 矩形。

## 3. 共享控制块（B 跳）

`AgwIpcHeader` 的**同一个布局**（字段见 `AgwViewIpc.h`，两边必须逐字节一致：固定宽整数、
`volatile LONG` 的 SPSC 索引、`char` arena）。约定：

- **guest 分配**整块（`sizeof(AgwIpcHeader)` + `AGW_IPC_FRAME_W_MAX * 4 * AGW_IPC_FRAME_H_MAX`
  字节，当前上限 2560×1600×4 = 16MB），并在 `BP_ATTACH` 时把 **guest 地址** 与长度交上来。
- 块里 `magic/version/width/height/frameOffset/framePitch` 由 guest 填好（`frameOffset` 相对块首）。
- 索引只允许"生产者写、消费者读"：命令环（shell→guest）由 winhost 写、guest 读；事件环
  （guest→shell）由 guest 写、winhost 读。与 A 跳完全一致，winhost 只是两跳的镜像。

## 4. ecall 表（B 跳）

沿用 RVVM 现有 `SYS_ANDROID_BASE = 0x10000 + n` 的编号风格，新开一段
`SYS_BROWSER_BASE = 0x10100`：

| n | 名称 | 参数（a0..a3） | 返回 | 说明 |
|---|---|---|---|---|
| 1 | `BP_ATTACH` | `guest_block_addr, bytes, abi_version, flags` | 0 = ok，负 = 错误 | guest 启动后一次；winhost 校验 magic/version/尺寸并记录 host 指针 |
| 2 | `BP_WAIT` | `timeout_ms` | 1 = 有命令待处理，0 = 超时 | 阻塞等待命令（winhost 在内部等 `_evt`）。事件驱动，不要轮询 |
| 3 | `BP_FRAME_POST` | `seq` | 0 = ok | guest 已把一帧画进 canvas（`seq` 与块里 `frameSeq` 一致）；winhost 拷贝进 A 跳 canvas 并 ack |
| 4 | `BP_SERVICE_REQUEST` | `kind, guest_args_addr, len` | `request_id` | guest→shell 的服务请求（open_window/dialog/download/clipboard/profile_*）；winhost 转成 `AGW_VIEW_EVENT_SERVICE_REQUEST` |
| 5 | `BP_READY` | `caps` | 0 | guest 引擎起来、可以接命令 |
| 6 | `BP_EXIT` | `code` | — | guest 主动退出（页面 `window.close()` 等） |
| 7 | `BP_LOG` | `guest_str_addr, len` | 0 | 诊断输出转发到 winhost stderr（`AGB_RENDER_LOG` 风格），不进控制面 |

错误码（负值）：`-1` 未 attach、`-2` 版本不匹配、`-3` 块太小 / magic 错、`-4` 参数非法。

**中断**（对应 A 跳的 `_int` 事件）：winhost 在收到 `_int` 时把块里的
`interruptRequested` 置 1；guest 侧引擎起一个 **50ms 轮询的 watchdog 线程**（guest 线程，
调用 `macross_interrupt_view()`）。为什么不直接打断 guest：guest 代码卡在脚本里时，
host 无法安全地进 guest 上下文；50ms 轮询把"卡死 tab 关闭/中断"的延迟控制在 <200ms 验收内。
若 RVVM 后续提供"vCPU 可中断"原语，这里可升级成真正的事件唤醒（保留字段 `reserved`）。

## 5. 命令 / 事件

结构体与 agave 的 `AgwViewCommand` / `AgwViewEvent` 一一对应（`AgwViewProtocol.h`），
B 跳用 `AgwIpcCommand` / `AgwIpcEvent` 的 wire 形态（文本用 offset 而非指针）。
**语义不变**：可见性、焦点、resize、鼠标/键盘/文本/预编辑、导航/reload/stop/back/forward/eval/
search/zoom/context-menu；事件侧 ready/frame/title/progress/url/status/message/service-request/
unresponsive/exit。

新增字段：`interruptRequested`（`volatile LONG`，放在 `AgwIpcHeader` 保留区）。
服务 kind 枚举 v1：`1 open_window, 2 dialog, 3 download, 4 clipboard, 5 file_picker,
6 profile_get, 7 profile_set`（`profile_*` 对应 [design-render-backend.md](design-render-backend.md)
§9 的"共享 broker"决议）。

## 6. 时序

**启动**：shell 建段/事件 → `CreateProcess(rvvm_winhost.exe --app agave_engine --ipc <name>)` →
winhost 引导 guest → guest 分配块并 `BP_ATTACH` → `BP_READY` → shell 收 `VIEW_READY` →
shell 发 `NAVIGATE`（走命令环）→ guest `BP_WAIT` 返回 → 执行。

**输入**：shell `submit(INPUT_*)` 写 A 命令环 + `SetEvent(_evt)` → winhost 读到 → 写 guest 命令环 →
guest `BP_WAIT` 返回 → `macross_*` 事件。

**帧**：guest 画进 canvas → `BP_FRAME_POST(seq)` → winhost 拷贝到 A canvas → shell 轮询看到
`FRAME` → 拷 mirror → `frameAck` → winhost 把 ack 写回 guest `ackSeq`。

**关闭/崩溃**：shell `_int` + EXIT 命令 → guest 退出或 winhost 300ms 后杀进程（A 跳已有）；
winhost 进程死 → shell 报 `AGW_VIEW_EVENT_EXIT(b=1)` → sad tab + respawn（A 跳已有，会重新拉起
winhost 并重复启动时序）。

## 7. RVVM 侧待实现清单

1. `src/virtpass/vp_browser.{c,h}`：共享层（块校验、环搬运、服务请求编解码），Android/win32 共用。
2. `win32_browser_bridge.c`：winhost 里的 host 端 —— 映射 A 跳的命名段与事件、实现 §4 的
   ecall 回调、`BP_WAIT` 的阻塞（等 `_evt`）、帧搬运与 ack 回写、`_int` → `interruptRequested`。
3. `src/core/rvvm_user.c`：`SYS_BROWSER_BASE` 分发接到 (2) 的实例表（沿用 `vp_cmdpost` 的
   "每 guest 一个实例"模式）。
4. guest sample：先做 `bench_browser`（不跑引擎）：guest 分配块 → attach → 循环
   `BP_WAIT` → 把 1 帧假数据写 canvas → `BP_FRAME_POST`，断言 A 跳能看到帧、ack 能回来。
   这一步就能量 M1（帧发布往返）与 M5（拷贝）。
5. 再把 agave engine 交叉编译成 riscv64 `--app` 包（阻塞项：平台层字体/线程/文件；
   见 [design-render-backend.md](design-render-backend.md) §9 与 §2 的 guest 前置）。

## 8. agave 侧对应（B3 批）

`src/webview_control/AgwViewBackendRvvm.c`：与 `AgwViewBackendProcess.c` 共享 90%——差别只有
拉起的命令（`rvvm_winhost.exe --app agave_engine --ipc <name>`）和"渲染进程"是 winhost。
协议、帧背包、sad tab、job object、中断路径全部复用，所以 B3 的 agave 工作量很小，
风险集中在 guest 端口（第 7 节第 5 条）。

## 9. 验收

| # | 判据 | 方法 |
|---|---|---|
| 1 | guest 帧到 shell：画面正确 | `--shot` + 像素断言（沿用 `run_process_backend_tests.ps1` 的像素用例） |
| 2 | 输入：`--keys` 打进 guest 的 DOM | 同 process 后端的输入用例 |
| 3 | 帧发布往返（M1） | `bench_browser`：发布→shell 收到 `FRAME` 的延迟，目标 ≤ 一次拷贝 + 100µs（"一次拷贝"取 process 传输实测的 0.5–0.7 ms/帧，见 design-render-backend §6.3；`BP_WAIT` 是事件驱动，不该复现 process 后端轮询那 15.8 ms 的平均延迟） |
| 4 | 输入延迟（M2） | shell 提交→guest keydown 回调：p99 ≤16ms（含引擎一帧） |
| 5 | 两个 tab：一个 `while(true)`，另一个 <200ms | 复用 `agave_multiview_probe`，`--backend rvvm` |
| 6 | 崩溃/关闭 | 杀 winhost → sad tab + respawn；关卡死 tab <300ms |

第 5/6 条复用已绿的 process 套件（`tools/run_process_backend_tests.ps1`，13 个用例），只把后端换成 `rvvm`。

## 10. 版本与兼容

- `ABI_VERSION = 1`，能力位与 agave 的 `AGW_VIEW_CAP_*` 对齐（damage / gl-composite / services）。
- 结构体只追加字段、不动既有偏移；`AgwIpcHeader` 的 `reserved` 区留 64 字节。
- 版本不匹配时 winhost 必须拒绝 `BP_ATTACH` 并让 shell 报错（不要静默降级）。
