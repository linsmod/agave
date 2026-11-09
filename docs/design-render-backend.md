# S3 设计：多 tab 并行与可插拔渲染后端（local / 进程 / RVVM）

> **设计记录，不是现状。** 现状见 [arch-overview.md](arch-overview.md) /
> [arch-status.md](arch-status.md)；待办与验收见 [plan-roadmap.md](plan-roadmap.md) 的
> "P2 多 tab 并行"。本文件冻结 2026-11-09 的决策与实测数据，实施进度回填 §8。

## 1. 问题与目标

事实（2026-11-09）：一个进程只有**一条引擎线程**，所有 tab 的 view 在它上面轮流跑
（`src/webview_control/AgaveWebView.c` 的引擎循环：`macross_event_dispatch()` → 逐 view
`macross_view_update()`/`pump()`）。后果：

1. 一个 tab 的长脚本/长同步工作会挡住其他 tab 的定时器、XHR 投递与输入命令（只有 1.5 s 看门狗中断兜底）。
2. 不卡也不并行：N 个 tab 的布局+绘制串行累加（R150 只省掉了隐藏 view 那一部分）。
3. 没有隔离：一个 tab 的引擎崩溃/AV 带走整个浏览器；内存也是一锅。
4. 中断/看门狗是进程级机制，按 tab 归因是后补的。

目标：**每个 tab/view 有自己的执行上下文**——卡住或很重的页面只影响它自己，tab 之间能真正并行，
UI 线程只做合成与输入归一化。验收：

| # | 判据 | 现状 |
|---|---|---|
| 1 | 两个 tab，一个 `while(true){}`；另一个点击/刷新/导航 **< 200 ms** 响应 | 等 1.5 s 中断 |
| 2 | 两个 CPU-bound tab 总墙钟明显小于串行（≥2 核 ~60%），单 tab 帧率不随 tab 数线性掉 | 线性掉 |
| 3 | 关掉卡死 tab 立即生效；看门狗按 tab 报/断，不误伤别的 tab | 进程级 |
| 4 | 现有套件全绿；探针与 GUI 行为不变 | — |
| 5 | 多 tab 压测（N tab × reload/navigate/close）不崩、无跨 tab 串数据 | 未做 |

## 2. 已具备的前提（为什么不从零开始）

- `RuntimeState`（view）/[`DocumentRuntime`](design-context-ownership.md)（文档）分离、每 frame 一个
  realm（[design-per-frame-realm.md](design-per-frame-realm.md)）、每 view 的模块表（P1-a）；
- 桥引擎表加锁、可中断 `Atomics.wait`（R146）；桥回调 activate/leave 成对（R149）；
- 隐藏 view 跳过布局/绘制（R150）——多进程的"冻结后台 tab"就是它的延伸；
- 控制面已经是"全入队 + mirror 出像素"（[include/agave_webview.h](../include/agave_webview.h)），
  `examples/view_host` 是不建控件的现成宿主，probe 是它的测试契约；
- **渲染后端 seam 已存在（B0，2026-11-09）**：`src/webview_control/AgwViewBackend.h/.c` 定义
  `AgwViewBackend`（3 个进程级 + 22 个 view 级 ops），`AgaveWebView` 的 per-view 调用全部走
  `backend->op(backendView)`；v1 只有 `local`，`AGB_VIEW_BACKEND` 里未实现的名字会警告并回退。

## 3. 决策：不是"加个线程"，是"渲染后端可插拔"

### 3.1 为什么放弃"每 view 一条引擎线程"作为默认路线

线程化要把全部进程级假设改成 per-thread 或加锁，其中三块特别贵：

| 假设 | 位置 | 线程化的代价 |
|---|---|---|
| 宿主镜像 `g_currentState`/`g_currentDocument` | [AntRuntime.cpp](../src/javascript/ant_backend/AntRuntime.cpp) | 改 thread_local + 所有回调路径复核 |
| 桥的 `g_methods[]`/wrapper 表/class 表 | `H:\ant-v16\bridge\AntBridge.c` | 全表加锁或迁进 per-engine 结构，热路径 |
| WebCore 单例：`Settings::globalSettings()`、`ImageDecodeService::shared()`、**全局 `AtomicString` 字符串表**（`src/webcore/platform/AtomicString.cpp` 的 `static HashSet<StringImpl*>* stringTable`，无锁） | WebCore | 字符串表是基础设施，跨线程共享意味着锁/分片；等于改一遍移植层 |

而**多进程天然把这些静态量变成"每进程一份"**：不需要锁，边界即隔离。所以默认路线改为
**一个 tab/view 一个后端实例**（进程或 RVVM 实例），进程内线程只作为 local 后端的一种实现细节。

### 3.2 三条后端，一个协议

```
            ┌──────────────────────────── AgaveWebView（宿主 API 不变）────────────────────────────┐
Win32 shell │  tab 切换/缩放/输入归一化 → ViewBackend 接口（命令/事件/帧）                          │
            └───────────────┬──────────────────────────┬──────────────────────────┬───────────────┘
                            │ local                     │ child-process            │ rvvm
                    同线程引擎（现状）         Win32 子进程 + 命名共享内存 + 管道   RVVM 实例 + virtpass_browser
```

后端差异被协议吃掉：shell 与 `agave_webview_*` 不感知引擎在哪。切换开关
`AGB_VIEW_BACKEND=local|process|rvvm`（默认 `local`）。

## 4. 协议 v1（渲染后端协议）

传输：每个 view 三条通道，全部**共享内存环 + doorbell**（进程后端用命名 section + 事件/管道；
RVVM 后端用 guest 内存 + `virtpass_browser` 的 ecall；local 后端只是结构体队列）。
**批量友好**：一个环项可以带多个输入事件——每次跨界有固定成本（见 §6），不要"一事件一 ecall"。

### 4.1 host → guest（命令）


实现载体：`src/webview_control/AgwViewProtocol.h`（`AgwViewCommand`/`AgwViewEvent`/`AgwFrameBuffer`/`AgwFrameAck`，
固定布局 POD + 能力位；`text` 只在 submit 调用期间有效，远端后端自己拷贝）。

| 命令 | 载荷 | 说明 |
|---|---|---|
| `VIEW_RESIZE` | `w,h,scale,dpi` | 唯一触发重排的命令；必须幂等 |
| `VIEW_VISIBLE` | `0/1` | 冻结/解冻（后台 tab、最小化）；对接 R150 的 skip + 整幅重绘 |
| `VIEW_FOCUS` | `0/1` | 与 visibility 分开（活动窗口的焦点不等于可见性） |
| `VIEW_ZOOM` | `percent` | `macross_view_set_scale_factor`；控件缩放已接 |
| `INPUT_MOUSE` | `action,x,y,button,wheel,t` | 移动可合并（只留最后一个坐标），按下/抬起保序 |
| `INPUT_KEY` | `down/up,vk,scan,mods,t` | 严格保序 |
| `INPUT_TEXT` | `utf8 commit` | 直接喂 `macross_view_input_text()`；IME 提交从这条路进 |
| `INPUT_PREEDIT` | `utf8,caret` | 预留；agave 目前无预编辑渲染，先只做 commit |
| `CTRL` | `navigate/reload/stop/eval/shot` | 复用现有控制命令语义 |
| `SERVICE_REPLY` | `id,status,payload` | 对 guest 服务请求的应答 |

### 4.2 guest → host（事件 + 帧）

| 事件 | 载荷 | 说明 |
|---|---|---|
| `VIEW_READY` | `abi_version, capabilities` | 版本/能力协商（缩放、damage、GL 合成） |
| `FRAME` | `buffer_id, seq, damage[4n], format, stride` | **主路径**：BGRA 画布 + 脏矩形；每帧 ack 后才回收 buffer |
| `TITLE` / `PROGRESS` / `URL` | `utf8` / `pct` | 与现有回调一致 |
| `SERVICE_REQUEST` | `open_window/dialog/download/clipboard/file_picker/fetch?` | 一级请求/应答（跨进程必须，ndk 路径没有） |
| `UNRESPONSIVE` | `busy_ms` | 接现有看门狗，按 tab 报告（advisory） |
| `EXIT` / `CRASH` | `code` | 后端死亡 → sad tab + reload 新实例 |

### 4.3 帧与背压

- 三缓冲：guest 写 `buffer_id` → `FRAME` 发布 → host 合成后 `FRAME_ACK{buffer_id, seq}` → guest 才复用。
- host 合成只处理 damage；`damage` 为空表示整幅无效（后端重排/尺寸变化）。
- 尺寸变化、隐藏转可见后的首帧一律按整幅 damage 处理（与 R150 的 `forceFullPaint` 同义）。
- 崩溃代际：`VIEW_READY` 带实例代际，帧/事件带代际；旧代际的帧丢弃（防 sad-tab 后迟到帧）。

### 4.4 服务通道

`window.open`、dialog、download、clipboard、文件选择今天是裸回调 + 活指针（
`agwOnOpenWindow` 同步返回 `MaCrossView*`）。跨后端必须 request/reply + 异步句柄；`window.open`
的返回对象同时是 S4 的 WindowProxy 工作，两件事一起收。

## 5. 各后端实现要点

| | local（现状） | child-process | rvvm |
|---|---|---|---|
| 传输 | 结构体队列（`AGW_CMD_*`） | 命名共享内存 + 管道/事件 | guest 内存 + `virtpass_browser` ecall/doorbell |
| 帧 | mirror DIB | 共享 section 里的 BGRA + damage | guest 画布指针直接读（同地址空间） |
| 死法 | 同进程崩 | 进程退出码/WaitForSingleObject | guest `EXIT`/fault → host 回调 |
| 隔离 | 无 | 内存/崩溃 | 内存/崩溃/ISA（+ 未来沙箱） |
| profile | 进程内文件 | 每实例目录或 broker | guest rootfs（per-instance 或共享挂载） |

## 6. 实测：逐调用转发与 GL 合成的成本（2026-11-09）

> 出处：`H:\github_repos\RVVM\src\virtpass\guest-samples\bench_ecall.c`；
> 复跑 `rvvm_winhost_x86_64.exe --app bench_ecall`（`RVVM_BENCH_SIZE` / `RVVM_NOJIT` 可选）。
> 环境：GTX 1080 Ti + ANGLE，`BGRA_EXT` 可用。以下数字是 RVVM 侧对我的论断
> **"`vp_gl_dispatch` 是逐调用转发（每帧几千个 ecall 会比软件光栅还慢）"** 的实测回复。

| 指标 | JIT（现行） | NOJIT（解释器下界） |
|---|---|---|
| A0 纯 guest 计算本底 | 7.1 ns | 114 ns |
| **A1 裸 ecall 地板**（getpid） | **71.7 ns** | **242 ns** |
| **A2 全 GL 通路**（glGetError） | **1248 ns** | **5173 ns** |
| → GL 转发税（A2−A1） | 1177 ns | 4931 ns |
| **b 每调用边际成本**（帧拟合） | **1.29 µs / 1.32 µs**（两尺寸一致） | **5.05 µs / 5.17 µs** |
| a 全帧上传+finish（1280×800） | 0.51 ms | 0.53 ms |
| a（1920×1080） | 0.85 ms | 0.88 ms |
| C 软件填充地板（memset） | 0.036 ms / 0.073 ms | 1.00 ms / 2.13 ms |
| 3000 调用/帧的 ecall 税 | **3.9–4.0 ms** | **15.2–15.5 ms** |

### 6.1 RVVM 侧判读（原样摘录 1、3、4）

> 1. **ecall 本身不慢**：裸 ecall 只有 72ns（JIT）。1.3µs 的大头在**封送+分发**（A2−A1 占 94%）
>    ——`gl_call` 12 槽 memset（guest -O0 模拟执行）+ `cmdpost_dispatch` 查表。所以"ecall 慢"这个
>    说法不准，准确说是"**逐调用转发**这条链慢"。
> 3. **但"撑不住"不成立**：JIT 下 3000 调用 = 3.9ms，仍远在 16.7ms 预算内。是"慢 20 倍"，不是"不可行"。
> 4. **NOJIT 下结论反转**：解释器路径里 guest 自身软件填充被拖到 1–2ms/帧，**反而比 GL 上传通路
>    （0.53ms）慢**——未来开页表的话，"把像素算在 guest、传出去"未必输给"在 guest 软画"。

### 6.2 由此修正的结论与设计后果

- **撤回"ecall 慢"**：裸 ecall 72 ns；成本在封送+分发（`gl_call` memset + `cmdpost_dispatch` 查表），
  1.3 µs/调用。协议设计要**批量**，这一条数字直接支持"一个环项带多个事件"。
- **"逐调用 GL 转发做 2D 光栅不划算"成立，但理由换成量级对比**：5000 调用 = 6.5 ms，而整帧软件填充
  地板 0.036 ms——差约两个数量级；不是"撑不住"（3000 调用 3.9 ms 在 16.7 ms 预算内），而是**不划算**。
- **"GL 只做合成"设计保持不变**，主路径仍应选**零拷贝 damage**：同地址空间 memcpy 全帧
  0.034–0.07 ms，比 GL 上传（0.51–0.88 ms）便宜 ~15 倍；GL 合成留给缩放/缩略图/圆角。
- **新增一条比"填充"更强的结论**：NOJIT 下 A0 也要 114 ns（JIT 7.1 ns，16 倍）。所以
  **引擎（layout/paint/DOM）放进 VM 必须以 JIT 为前提**；一旦走解释器/页表路径，"引擎在 guest 里"
  整体不可行，不是"像素算在哪"的问题。这条要写进路线风险，别只拿 C 行做判断。
- 自定义 `virtpass_browser` 的必要性论证**不依赖 ecall 速度**，四条理由：零拷贝 damage、有序输入流、
  服务通道、隔离。

### 6.3 待补测（在开工前做，成本低）

| # | 测什么 | 验收数字 | 用途 |
|---|---|---|---|
| M1 | no-op 帧发布往返（无 GL、无上传、只有 FRAME+ACK） | ≤10 µs/帧 | 协议地板 |
| M2 | 输入延迟：doorbell 唤醒 vs 轮询（60Hz/1kHz） | p99 ≤16 ms（含引擎一帧） | 决定输入通道形态 |
| M3 | 批量 vs 逐条命令（N 事件一 ecall） | 省 ~1.3 µs/事件 | 环项打包规则 |
| M4 | 真实软件光栅（`ps_*` 填充，不是 memset）在 guest 的成本 | 整帧 ≤5 ms | guest 内软画是否可行 |
| M5 | damage blit 在桥上下文里的实测（同地址空间 memcpy + 合成） | ≤0.1 ms/帧 | 主路径确认 |

## 7. 迁移与回退

- 开关：`AGB_VIEW_BACKEND=local|process|rvvm`（默认 `local`），单 tab 可覆盖；
- 现有 API（[include/agave_webview.h](../include/agave_webview.h)）与 shell 不变，后端在
  `AgaveWebView` 内部选；
- 每批独立可构建、可回归、可回退；local 后端始终保留为 golden path。

## 8. 分批计划与验收

| 批 | 内容 | 验收 |
|---|---|---|
| B0 | `ViewBackend` 接口 + local 后端（行为零变化，纯重构） | ✅ 2026-11-09：`src/webview_control/AgwViewBackend.h/.c`（25 个 ops：3 个进程级 + 22 个 view 级），`AgaveWebView` 的 per-view 调用全部改走 `backend->op(backendView)`；`AGB_VIEW_BACKEND=local` 默认，未实现的名字**打一行警告并回退 local**；其余全套件绿 |
| B1 | 协议 v1 结构体化（命令/事件/帧/ack），local 走协议 | ✅ 2026-11-09：`src/webview_control/AgwViewProtocol.h`（`AgwViewCommand`/`AgwViewEvent`/`AgwFrameBuffer`/`AgwFrameAck` + 命令/事件 kind + 能力位）；后端接口收成 `submit`/`submitBatch`/`poll`/`frameAck` + create/resize/update/pump；控件的命令 drain 只构造协议命令；local 的 `submit` 把它们映回 macross，`poll` 返回 0（事件仍走 macross 回调）、`frameAck` 空（本地直接画宿主 buffer）。全套件绿、`bench_hot` 不退化 |
| B2 | `process` 后端（共享内存 + 管道 + 崩溃检测 + sad tab） | ✅ B2a 2026-11-09：`examples/render_host`（每 view 一个 `agave_render_host.exe`，无窗口、只写 BGRA）+ `AgwViewIpc.h`（命名共享段：canvas + 命令环 + 事件环 + 文本 arena，单生产者/消费者无锁；固定 2560×1600 canvas，pitch 固定，resize 只改 `width/height` 不重建映射）+ `AgwViewBackendProcess.c`（CreateProcess + 背压：`frameSeq==ackSeq` 才画下一帧；崩溃 = 进程句柄 signaled → `EXIT` 事件带退出码）。`observe`/`update`/`pump` 对远端是空操作（渲染进程自己有循环）；控件把共享 canvas **一次拷贝**进 mirror（跳过宿主 DIB）。`AGB_VIEW_BACKEND=process` 可跑通：页面 JS/title/定时器、`--shot` 像素、`--keys` 输入、渲染进程崩溃报 load error（probe 立即退出 5，不再等 `--timeout`）。套件 `tools/run_process_backend_tests.ps1`（5 项）。**B2b 待做**：双 tab 响应性验收（一个 `while(true)`）、sad tab UI、per-view interrupt（`processInterrupt` 仍是空操作）、damage 矩形（现在整幅）、canvas 尺寸超过 2560×1600 的降级 |
| B3 | `rvvm` 后端（`virtpass_browser` + guest 渲染器，先零拷贝 damage） | 单 tab 出帧/输入；M1/M2/M5 达标 |
| B4 | 服务通道（open/dialog/download/clipboard/file）+ window.open 异步（S4 WindowProxy） | 弹窗/下载/对话框跨后端可用 |
| B5 | 冻结与压测（R150 对接）、多实例 profile 策略、`one-VM-multi-surface` 评估 | 验收表 1–5 |

## 9. 决策与未决问题

已定（2026-11-09，用户拍板）：

1. **网络 = guest 自己的 socket**（已定，2026-11-09）：隔离好，代价是多一跳 emulation；不引入
   host `FETCH` 服务。若实测成为瓶颈，再对 `fetch`/`XHR` 单独开 host 服务（协议里
   `SERVICE_REQUEST` 已留位）。
2. **profile/cookie = 共享 broker**（已定，2026-11-09）：宿主进程持有 `cookies.txt` /
   `localstorage.txt` / HTTP 缓存目录并串行化访问，渲染实例通过 `SERVICE_REQUEST(profile_*)`
   读写，自己不再直接写这些文件。B3 先用 per-instance 临时 profile 打通链路，broker 随 B4 的
   服务通道落地；验收：两个 tab 共享登录态，broker 重启不丢已提交的写。
3. **one-VM-multi-surface**：一个 VM 托管多个 view 省启动成本，需要 virtpass 支持多图形 session
   （今天的 `vp_core` 多 session 是终端 pty，不是图形面）。
4. **IME 预编辑**：host 候选窗 + commit 先通；guest 侧预编辑需要引擎支持 composition。
5. **R150 与后端冻结的关系**：local 用 `visible` 标志；process/rvvm 用 `VIEW_VISIBLE` 命令，
   首帧整幅重绘语义必须一致（同一条验收用例）。

## 10. 附：与现有 RVVM 桥的对照

| 维度 | 现有 NDK/cmdpost 桥 | `virtpass_browser` v1 |
|---|---|---|
| 帧 | guest pixbuf → host RGBA→BGRA + 整帧 DIB 翻转 | 共享 BGRA + damage 列表，host 直接读（零拷贝） |
| 输入 | console tty（`WM_CHAR`）+ GameActivity 队列（AKEYCODE），无 composition | 一条有序输入流（mouse/key/text/preedit/focus） |
| 生命周期 | `APP_CMD_*` 与窗口激活耦合（README 明说不能映射 `WM_ACTIVATE`） | visibility/focus/resize 独立命令 |
| 服务 | 无 | `SERVICE_REQUEST/REPLY` |
| 版本 | 跟 NDK stub | 自带 ABI 版本 + 能力位 |