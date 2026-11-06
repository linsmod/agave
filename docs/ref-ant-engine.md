# ant 引擎与桥接：哪份是真的、怎么改

两套 git 仓库互不相干。桥现在属于 ant 仓库，agave 只消费 `antjsb.dll`。

## 1. 真源矩阵（2026-11-05 核实）

| 对象 | 真源（改这里） | 其它拷贝 |
|---|---|---|
| ant 内核 `libant.a` / `src/silver/*` / `src/esm/*` / `include/*` | `H:\ant-v16`（分支 `agave-bridge`） | 本仓库**没有**引擎源码 |
| `AntBridge.c` / `AntBridge.h` | **ant 仓库** `H:\ant-v16\bridge\` | `3rdparty/ant/include/AntBridge.h`（发布头） |
| `antjsb.dll` | `H:\ant-v16\build-zig\`（Meson target `antjsb`） | `3rdparty/ant/bin`、`build-zig-native`；**运行时只加载 exe 同目录 `build-zig-native\antjsb.dll`** |
| `prelude.js` / `polyfills.js` | **本仓库** `src/javascript/ant_backend/` | `build-zig-native\`（运行时副本，可直接改来验证） |
| git 远端 | agave → GitHub `linsmod/agave`；ant 上游 → `theMackabu/ant`，本地镜像 `H:\ant-upstream`，推送 fork `fork` → `linsmod/ant` | 两套历史无关 |

**提交规则**：桥/内核的改动在 ant 仓库（`H:\ant-v16`，分支 `agave-bridge`）提交；
本仓库只跟踪发布产物（`3rdparty/ant/bin/antjsb.dll`、`lib/antjsb.dll.a`、`include/AntBridge.h`）。
`tools/build_antjsb.ps1` 发布前会检查 ant 的 `git status`，不干净时打印警告——
否则 dll 与源码分叉，下次同步基线时改动会静默丢失。

## 2. 构建链路

```powershell
# 只改桥或 ant 内核
pwsh -File tools\build_antjsb.ps1        # 编 libant.a + antjsb.dll，发布到 3rdparty/ant
cmake --build build-zig-native --parallel 16

# 只改 agave C++
cmake --build build-zig-native --parallel 16
```

- 桥的源码、构建、导出都在 ant 仓库；agave 里**不再有** `AntBridge.c` 副本，也没有同步脚本。
- `agave-bridge` 分支本身就是浏览器适配分支，**不需要 patch 清单**；版本契约是 commit。
- 改 ant 的**头文件**会让 `libant.a` 重编 200+ 个对象（~10 分钟）；只改 `.c` 是 1-2 个对象（~1 分钟）。
- 一致性自检：

```powershell
git -C H:\ant-v16 rev-parse HEAD
(Get-FileHash 3rdparty\ant\include\AntBridge.h -Algorithm MD5).Hash
(Get-FileHash H:\ant-v16\bridge\AntBridge.h -Algorithm MD5).Hash
```

## 3. 版本与分支

- `agave-bridge` 的基线 = tag `v16.5.c41757b4.0`（**冻结的基线**），落后上游 `origin/master` 54 个 commit；
  本地桥接/性能/errors/worker 等改动都在这个分支上。跟上游同步只能 rebase，别 merge。
- 分支已推到 fork：<https://github.com/linsmod/ant/tree/agave-bridge>（`git push fork agave-bridge`）。
  向上游提 PR 前必须先 rebase 到 `theMackabu/ant` 的 master。
- `H:\ant-v16` 历史上有 detached HEAD 检出 tag 的用法：**提交后立刻 `git switch -c <name>` 挂分支**，切走就丢。
- 当前 `antjsb` 也在 `agave-bridge` 上；没有单独的 patch 脚本需要维护。

## 4. ant 自带能力（先确认再怀疑）

- 跑 JS：`H:\ant-v16\build-zig\ant.exe script.js`（或 REPL）。怀疑"引擎语义 vs 浏览器/桥接"时先用它钉死。
- 自带测试：`H:\ant-v16\tests/test_wasm_*.cjs` 等（wasm 9/9 过：导入内存/表/全局、grow、SIMD）。
- worker 是**真实现**：独立线程 + 独立 isolate + `ant_wire` 结构化克隆（见 [hist-lessons.md](hist-lessons.md) R22）。
- wasm 是**解释执行**：导出函数跨界约 3.9µs/次，适合"一次调用跑大循环"（R19）。

## 5. ant 的反直觉清单（写 shim / 页面测试前必读）

- **字面量属性名赋值会被吞**：`g.Worker = fn` 可能写不进去；用动态名（`g['Wor'+'ker']`）或
  `Object.defineProperty`（这也是 `prelude.js`/`polyfills.js` 里很多写法的原因）。
- 命名函数表达式、原型上的静态名赋值同样有坑；绑定前先验证 `typeof`。
- `typeof X.y` / `delete` / `for...in` 的支持与标准 JS 不同；写探针脚本时避开（`docs/hist-lessons.md` 里有例子）。
- `js_set_filename` 只存**指针**：传入的字符串必须活到编译产物之后（桥里为此持有副本，见 R22）。
- `ant_runtime_init` 也只存 argv 指针：给它栈上数组会留悬垂指针（R22 的崩溃就是这么来的）。
- ant 的 ESM `esm_is_url` 只认 http(s)：`file:` 模块靠 agave 侧伪装成 `http://file.ant/...`，
  fetcher 再还原（见 [arch-module-loading.md](arch-module-loading.md)）。
- 一个 error 没有栈时（页面关掉 `Error.stackTraceLimit`）用 `AGB_ERR_SNIPPET=1` 定位。
- **interned 字符串（`intern_string`）是 isolate 级永生的**：条目从 `intern_chunk_t` 顺序切出（`src/ant.c:206`），
  扩容只换 slot 表、不移位（`:188`），整块只在 `js_destroy` 里 `intern_table_free()`（`:222` 唯一调用点）。
  所以 `const char* interned` 可以安全缓存（`js->intern.idx[]` / `idx_hash` 就是靠这个）——
  但 **JS 字符串值** 不保证：`sv_key_to_propstr` 之类转换出来的字节可能是临时/rope buffer，
  不能只靠“pin 住值”当永久键用（R71 的崩溃就是这么来的：错误值被当键 + 转换在 root scope 之前）。
- **C 侧持有 JS 值一律走 `antb_retain()`**（监听器、定时器、XHR 回调……）；它内部是
  `gc_pin_permanent()`。对象走 `permanent_roots`（`gc_permanent` 免扫），**函数/闭包走
  `permanent_values`**（每次 GC 重新 mark）——两者都不要绕过桥自己 pin，且必须检查返回值：
  pin 失败是静默的（R67：函数曾整类 pin 失败，监听器被回收后 deepseek 载入 AV）。

## 6. 桥接的边界

- `AntBridge.c` 是**所有 JS↔C 的唯一漏斗**（`agb_dispatch(slot, js, ...)`），每个 native 方法注册一个
  trampoline 槽（`AGB_TRAMPOLINE_COUNT`）。
- wrapper 表（native 指针 ↔ JS 对象）是**进程级**的，随 engine 创建/销毁：`antb_wrap_native_owner` /
  `antb_release_owner` / `antb_engine_destroy`。
- 每个 view 一个 `RuntimeState`，每个文档一个新 engine（`beginNewRealm`/`stopRealm`），
  回调进来前靠 `antb_set_context_activate` 切状态（细节见
  [design-per-view-js-context.md](design-per-view-js-context.md)）。
- **engine 是多实例的**：`antb_engine_create()` 每次造一个独立 `ant_t`，注册进 `AntBridge.c`
  的 `g_engines[32]`；活跃数量 = 打开的 view 数（tab/弹窗），导航时旧 engine 销毁、新 engine 建。
  **frame 不是独立 engine**，是同一 VM 里的独立 realm（P0-i）。**worker 不在这张表里**：
  `worker_thread_main()` 自己 `ant_create()` 一个 VM 跑在 `uv_thread` 上（`src/modules/worker/core.c`），
  由 worker 自己的 `terminate()` 打断。
- **中断（`antb_interrupt_all_engines()`）**：给注册表里**每个** engine 置 `interrupt_pending`。
  "全部"是对的 —— 所有 view engine 都由**同一个引擎线程**串行执行，任一时刻最多一个在跑，
  调用方（看门狗线程 / UI 线程）也无从知道是哪一个；没在跑的 engine 只是带着标志，下次顶层
  eval 由 `antb_begin_top_level()` 把 `terminating` + `interrupt_pending` 一起清掉。
  ⚠ 已知粗糙处（见 [plan-roadmap.md](plan-roadmap.md) 的 "P2 ant 中断路径收口"）：
  循环里的 `sv_jit_arm_interrupts()` 实际是空转（创建时已 arm，`interrupts_armed` 只置不清），
  真跑起来会在**引擎线程之外** `jit_drop_all_code()`（释放正在执行的代码页）；`g_engines` 无锁，
  与 `antb_engine_destroy` 有极窄的 UAF 窗口；`Atomics.wait()` 无 timeout 走 `pthread_cond_wait`，
  只置标志唤不醒它。
- 日志：ant 不直接依赖 agave 的 `AgbLog.h`；桥通过 `antb_set_logger` 回调，
  `AntRuntime.cpp` 把 `[flag]` 行接回 agave 的统一日志。
