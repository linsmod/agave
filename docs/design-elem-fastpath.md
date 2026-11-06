# 元素访问快路径（P1-g 设计）

> **设计记录，不是现状**：本文冻结在 2026-11-06 的方案与实测；实现进度与当前能力见
> [plan-roadmap.md](plan-roadmap.md)（P1-g）和 [arch-status.md](arch-status.md)。

> 目标：把 `obj[key]` / `G[idx]` 这类**稀疏下标**与字符串键访问从 ~140–200 ns 降到 ~20–40 ns，
> 让 v.qq 一类站点里的字节码 VM 不再把浏览器拖成假死（[plan-roadmap.md](plan-roadmap.md) P1-g、
> [hist-lessons.md](hist-lessons.md) R60）。

## 1. 现状实测（2026-11-06，ant 引擎直接跑，不经过浏览器）

`tools/bench_vm.js`，N = 2,000,000：

| 场景 | ns/op | 说明 |
|---|---|---|
| 稠密数组读 `G[i & 7]` | **2.5** | R65/R66 的 IC 已起效 |
| 稠密数组写 `G[i & 7] = i` | **4.0** | 同上 |
| 字符串键读 `O[keys[i & 7]]` | **115** | 键是已 intern 的常量字符串 |
| 动态字符串键读 `O['k' + (i & 7)]` | **196** | 每次新拼串 |
| 稀疏数组读 `SPARSE[1000 + (i & 7)]` | **141.5** | 元素落到 shape 的索引键属性 |
| 稀疏数组写 `SPARSE[1000 + (i & 7)] = i` | **206.5** | 同上 |
| 超大下标读 `SPARSE[(i & 7) * 100000 + 3]` | **151.5** | 与 1000 只差 ~10 ns |
| `switch` 分发循环（5 个 case） | **104** | 每轮含一次 `T[++P]` 读取 |
| VM 自我递归 `j()`（30 ns/轮） | 30 | 每轮建两个数组 + 一次自调用 |

结论：

- **稠密已被照顾得很好**（2.5/4.0 ns），不必再动。
- **稀疏下标是断崖**：读慢 57×、写慢 52×。而真实站点的 VM 槽位来自字节码表
  （`G[T[++P]]`，槽位几百到几万），`G[100000] = v` 之后数组就不再走稠密缓冲 —— v.qq 卡死正落在这个档位。
- **`switch` 分发本身也可疑**：5 个 case 的空转循环 104 ns/轮，远高于其内部几次数组访问的合计。

复现：`H:\ant-v16\build-zig\ant.exe E:\mybrowsers\agave\tools\bench_vm.js`（脚本在仓库里，数字随引擎版本变）。

## 2. 为什么慢（代码锚点）

1. `arr_get`（`src/ant.c:2478`）先试 `fast_array` 与稠密缓冲；不在缓冲里就把 `idx` 转成**字符串**
   （`uint_to_str`，:2503）再沿原型链逐层 `shape_lookup`。
2. 索引键在 shape 里是**interned 字符串属性**（`ant_shape_prop_t.key.interned`，`include/shapes.h:44`），
   每次查都要对字符串做哈希（`ant_shape_lookup_interned`）。整数下标没有自己的哈希通道。
3. `js->intern.idx[]` 只缓存 `idx < INTERN_INDEX_CACHE_SIZE = 1024`（`include/isolate.h:65`）；
   实测把下标从 1000 提到 100003 只慢 ~10 ns → **瓶颈不是 interning，而是 shape 字符串哈希 + 回退链**。
4. 写路径 `setprop_index_impl`（:5039）稠密命中很快；未命中就 `goto keyed` 走同一条字符串键慢路径
   （:5092 起），还要判原型链上的索引键、`array_define_or_set_index` 等。
5. `switch`：`switch-dispatch-loop` 的 104 ns/轮说明 JIT 很可能把 `switch` 编译成链式比较，
   而不是跳表（待 4.3 用 `u`/反汇编确认）。

## 3. 目标与判据

- 稀疏下标读 ≤ 40 ns、写 ≤ 60 ns（现 141/206）。
- 字符串键读 ≤ 40 ns（现 115）。
- `switch-dispatch-loop` ≤ 30 ns/op（现 104）。
- v.qq.com 在 `AGB_HANG_WATCH=1` 下不再出现 5 s `[hang]`（P1-g 既有验收）。
- 回归：`tools/run_script_mode_tests.ps1`、`tools/run_site_smoke.ps1`、worker/layout 回归全绿。

## 4. 方案（按风险递增，分三步）

### 4.1 shape 级 index → slot 表（主路）

shape 是不可变的：属性一旦加入，`(idx → slot)` 就固定；删除/新增都产生新 shape。
所以映射天然可缓存，不需要显式失效。

- `ant_shape_t` 增加**可选的**开放寻址表 `{uint32 idx; uint32 slot;}`：
  仅当 `ant_shape_may_have_index_keys()` 为真时分配；条目 ≤ 8 时退化为线性扫描（比哈希快）。
- 新增 `ant_shape_lookup_index(shape, idx)`；`arr_get` 的稀疏分支与 `setprop_index_impl` 的 `keyed:`
  分支改用它：命中后直接 `ant_object_prop_get_unchecked` / `ant_object_prop_set_unchecked`
  （写路径保留 `gc_write_barrier` 与 attrs 判断）。
- 语义不变：枚举 / `Object.keys` / `ownIndexKeys` / for-in 仍走原来的字符串键路径；
  `length`、只读数组、原型链索引键、typed array、字符串包装对象的判断顺序保持原样。
- 验收：bench 稀疏读 ≤ 40 ns、写 ≤ 60 ns；§3 的全部回归。
- 风险：shape 内存增大（每 shape 一条小表，索引键 shape 才分配）；表里只有整数，GC 不扫。

### 4.2 站点 IC（数字键）

`sv_ic_entry_t` 现在缓存 `(shape, atom) → slot`（R65）。数字键分支目前**完全绕开 IC**
（`jit_helper_get_elem` 对 `kTypeArray` + 数字键直接 `js_arr_get`）。
- 给 IC 增加 `(shape, uint32 idx) → slot` 变体；命中一次比较，不命中回落 4.1。
- 命中率计数（`AGB_IC_STATS=1`，新增或复用现有计数器），避免"以为有 IC 其实全 miss"。
- 验收：bench 稀疏读 ≤ 20 ns；IC 在 v.qq 上的命中率可观测（日志一行）。

### 4.3 `switch` 跳表

- 先用 cdb 反汇编一个 5–20 case 的整数 `switch`（`u <jit addr> L60`）确认是链式比较还是跳表。
- 是链式比较：对"密集连续 case"生成 `cmp/jae + jmp [table]`；对稀疏 case 保留链。
- 验收：bench `switch-dispatch-loop` ≤ 30 ns；v.qq 卡死时长显著下降（用 `[hang]` 出现次数/时长对比）。

## 5. 不推进 / 其他选项

- **不给对象加 dictionary-elements（对象级 int→value 哈希）**：改动面比 4.1 大（枚举顺序、length、
  defineProperty、delete 都要重排），而 4.1 已覆盖目标场景。
- **不把 `INTERN_INDEX_CACHE_SIZE` 抬到 8192 当主修**：实测只值 ~10 ns（141.5 → 151.5 的对比），
  可以作为 4.1 的补充项，但单独做不会让 v.qq 变好。

## 6. 验收怎么跑

```powershell
# 引擎级数字（改完先看这个，快且无浏览器噪声）
H:\ant-v16\build-zig\ant.exe E:\mybrowsers\agave\tools\bench_vm.js

# 站点级：v.qq 是否还"假死"
$env:AGB_HANG_WATCH=1
.\build-zig-native\agave_shell.exe --probe https://v.qq.com/ --wait 20000 --settle 1000 --timeout 90000 *> debug\hang_probe.log
Select-String -Path debug\hang_probe.log -Pattern '\[hang\]'

# 卡住时抓 JS 栈（不 attach）
pwsh -File tools\js_stack.ps1 -Pid <pid>

# 回归
pwsh -File tools\run_script_mode_tests.ps1
pwsh -File tools/run_site_smoke.ps1
```
## 7. 已落地（2026-11-06，第一轮）

改动都在 ant 仓库（`src/ant.c`、`src/silver/glue.c`、`include/ant.h`、`include/isolate.h`）：

- `js_index_get_fast()`（新，`ant.h`）：不分配、不跑 JS 的元素读——稠密缓冲或"shape 里的自有数据属性"命中才答；
  accessor / 稀疏缺失 / 越界一律返回 false 让调用方走通用路径。
- `jit_helper_get_elem()` 在建立 GC temp-root scope **之前**先试上面这条（数组 + 数字键）；
  这是本轮最大的一笔（省掉每次元素读的 root scope + 回退分派）。
- `arr_get()` 增加"自有索引键属性"快路径：`index_key_find` + `ant_shape_lookup_interned` 直接取 slot，
  不再走 `uint_to_str` + 通用 `lkp()`。
- `setprop_index_impl()` 增加"稀疏元素已存在"的直写快路径（带 `gc_write_barrier` 与 attrs/frozen 判断），
  数组不再因为 `plain = vtype(obj)==kTypeObject` 的判断而被踢回字符串键慢路径。
- 索引键 memo：`INTERN_INDEX_CACHE_SIZE` 保持 1024，但新增 `idx_hash`（1024 项直接映射）覆盖更大的下标，
  避免 VM 槽位（几千到几万）每次重新 intern 十进制键。
- `js_get_index_fast()` 对 `kTypeArray` 直接 `arr_get()`（原先只认 `kTypeObject`，数组必 miss 再二次分派）。

实测（同机、同 build，`tools/bench_vm.js`）：

| 场景 | 之前 | 之后 |
|---|---|---|
| 稀疏数组读 | 141.5 ns | **41 ns** |
| 稀疏数组写 | 206.5 ns | **28 ns** |
| 稠密读 / 写 | 2.5 / 4.0 ns | 2.5–5 / 3–4.5 ns（无回归） |
| 字符串键读 | 115 ns | 118 ns（未动） |
| `switch` 分发 | 104 ns/轮 | 104 ns/轮（未动） |

测试：ant 仓库新增 `tests/test_index_elem.c`（meson test `index-elem`），覆盖稠密/稀疏读写、空洞、length 截断与再扩展、
delete、索引 accessor、超大下标（走 `idx_hash`）、5000 个槽位的求和；并断言 `js_index_get_fast()`
在 accessor / 越界 / 已删除情况下**必须拒绝**回答。agave 侧 `tools/run_script_mode_tests.ps1` 全绿
（bench_hot：dense 5 ns、static-key 8 ns、vm-dispatch 9 ns）。

**仍未达成**：v.qq.com 在 `AGB_HANG_WATCH=1` 下仍报 `[hang]`（14 次，栈是 QIMEI 的 `getWebInstance → new j`，
`vendor-common…js:1:52943`），P1-g 的站点级验收还没过。下一轮按 §4.2（数字键站点 IC）、§4.3（switch 跳表）
以及字符串键 115–196 ns 那条继续；顺带发现一个语义偏差（已记入 §5）。

## 8. 本轮发现的语义偏差

数组越界读**不查原型链上的索引键**：`Array.prototype[7] = 777; [][7]` 在 agave 里是 `undefined`
（标准是 `777`）。根因是 `arr_get()` 在 `idx >= semantic_len` 时直接返回 `undefined`，没有走原型链；
`test_index_elem.c` 把当前行为钉住（断言 `undefined`），将来修的时候要同时改这条断言。
修它的代价是要在"越界读"这条热路径上加原型链索引键的失效判断（现代引擎用 Array.prototype 的
"元素写入计数器"做 guard），所以没有塞进本轮。

## 9. 第二轮规划：字符串键 92.5 ns、switch 跳表（2026-11-06 实测后修订）

先修正上一轮的一个误判：之前把 `switch-dispatch-loop` 的 112 ns/轮归给 switch 分发，**不对**。
拆开测（`tools/bench_vm.js`，N=2M）：

| 场景 | ns/op | 说明 |
|---|---|---|
| `strkey-mono`（单键，IC 不抖动） | **92.5** | 说明 115 ns 不是 IC 抖动造成的，是**路径本身** |
| `strkey-poly2` / `poly8` | 125.5 / 133 | 多态再叠 ~35 ns，IC 只存一个入口 |
| `dynamic-string-key-read`（`'k'+(i&7)`） | 197 | 拼接 + interning + 分配，另算 |
| `switch-dense10` | 13.5 | 10 个连续 case |
| `switch-dense40` | 25 | 40 个连续 case |
| `switch-sparse40` | **33.5** | 40 个任意 case 值（真实 VM 形状） |
| `if-chain40`（同义手写） | 12.5 | 编译器的 if 链反而更快 |
| `switch-dispatch-loop`（含数组读 + ++ + 循环） | 112 | 真正的成本在数组读和循环，不在 switch |

结论：**字符串键是更大的杠杆（92.5 → 目标 ≤25 ns），switch 跳表是 ~20 ns/dispatch 的吞吐改进**，
两者都不足以单独解释 v.qq/163 的 20 s 卡顿（按 33 ns/次算约 6 亿次分派），所以两条都要做完再复测站点。

### 9.1 字符串键读：先不加根、不走通用 walk 的快路径

现状（`src/jit/glue.c` `jit_helper_get_elem`）：非数字键先建 GC temp-root scope，再
`intern_find_flat`（或 `intern_string`）→ `sv_try_prop_get_ic_lookup` → 命中返回；不命中落到
`sv_getprop_by_key` 全量 walk。92.5 ns 里很大一块是 **root scope 的建立/拆解**（与稀疏数组那轮
同一个教训：数组快路径提前到 root scope 之前后，141 → 41 ns）。

实施：
1. 在 root scope **之前**加“不可能分配”的字符串键快路径：`vtype(key)==kTypeString` 且
   `vtype(obj)==kTypeObject`、非 exotic、shape 非空时，用**已有 intern 指针**（`vstr` 字节 + 
   `intern_find_flat` 的 64 项 memo，memo 命中则零分配）→ `ant_shape_lookup_interned` → 
   slot 取 `has_getter/has_setter == 0` 的值直接返回；memo 未命中就返回 false 交给原路径。
2. 站点 IC 从 1 项扩到 4 项（`(shape, atom) → slot`，MRU 淘汰），解决 poly2/poly8 的 35 ns 抖动；
   命中统计用 `AGB_IC_STATS` 输出每站点命中率（新增，避免“以为有 IC 其实全 miss”）。
3. 不做：给字符串拼接结果做 interning 缓存（`dynamic` 197 ns 主要花在拼接本身，属另一个问题；
   若要做，先测 `${}` 与 `+` 两条路径的占比）。

验收：`strkey-mono ≤ 25 ns`、`poly8 ≤ 40 ns`、`dynamic` 不退化；`run_script_mode_tests.ps1` 全绿；
v.qq/163 的 `[hang]` 时长/次数有可测量的下降（记录 before/after）。

### 9.2 switch：字节码级跳表，解释器与 JIT 共用

现状（`src/silver/compiler.c:7101 compile_switch`）：每个 case 编成
`DUP; <case 表达式>; SEQ; JMP_TRUE`，即**线性链**；case 是字面量时也在每次派发时重复
load+比较。40 个稀疏 case 就是 33.5 ns。

实施（分两步，第一步就能拿到大部分收益）：
1. **编译期线性扫描已经被 if-chain 反超**（12.5 vs 33.5），说明 JIT 对 `if` 的形态更好。
   先做最小改动：`compile_switch` 对“全部 case 都是字面量常量”的情况，把判别式存进临时槽，
   case 体改成复用该槽（不再每次 `DUP`），并把比较按**数值升序二分**组织（O(log n)，MIR 只需要
   `BEQ/BNE`，无新指令依赖），不满足条件时退回现状。
2. 真正的跳表：新增字节码 `OP_SWITCH`（表：`min, span, default_off, target_off[span]`），
   仅当 case 全是整数常量、`span ≤ 4096`、密度 ≥ 50% 时生成。解释器 O(1) 查表；
   JIT 侧 MIR **已经支持 `MIR_SWITCH`**（`src/jit/compile.c:203` 已把它当终止指令处理），
   直接映射成一条 `MIR_SWITCH`。
3. 语义要点：严格相等（`===`）→ 只对 `kTypeNumber` 且整数值查表，`-0 === 0` 视为命中，
   `NaN` 不命中；case 表达式若有副作用（非字面量）不得跳表；`default` 位置任意。

验收：`switch-sparse40 ≤ 12 ns`、`switch-dense40 ≤ 10 ns`；`run_script_mode_tests.ps1`、
`put-elem-gc`、`index-elem` 全绿；补一个 `tests/test_switch_table.c`（语义：-0/NaN/字符串 case/
副作用 case 回退）。

### 9.3 为什么两条都要做完再看站点

v.qq 的 VM 每个字节码步做：`T[++P]`（数组读 ~3 ns）→ `switch`（33.5 ns）→ `G[x]` 若干次
（数字键 37.5 ns / 字符串键 92.5 ns）→ 可能一次自调用（30 ns）。按 20 s 反推约 6 亿步，
把 switch 降到 12 ns、字符串键降到 25 ns 后应该能量到明显缩短，但不保证“不 hang”；
真正的验收仍是 P1-g 的 `[hang]` 判据，改动前先记录基线（`debug/hang_probe_vqq.log`）。

### 9.2.1 第一次尝试（已回退）：隐藏局部变量存判别式不可行

按 §9.2 第一步试过：在 `compile_switch` 里 `add_hidden_local()` 存判别式、case 比较改成
`GET_LOCAL8/LOADK/SEQ/JMP_TRUE`、去掉每 case 的 `DUP` 与 stub 的 `POP`。编译通过、机器码也照常生成，
但**语义立刻坏了**：

```
switch (2) { case 1: x = 1; break; case 2: x = 2; break; }   → x = 2      (对)
switch ("k1") { case "k0": … case "k1": y = 2; }             → y = 0      (错)
switch (5) { case 1: z = 1; break; default: z = 9; }         → z = console.log 的函数 (错)
```

原因：这个 VM 的局部变量槽（`local - param_locals`）和求值栈共用同一个帧布局，
**函数中途新增一个局部**会让后续 locals 的槽位整体后移，而判别式此刻还压在求值栈上 ——
两边互相踩，表现为“字符串 case 不命中 / 变量拿到别的值”。所以隐藏局部变量必须在
**函数入口**一次性预留（`reserve_hidden_locals` 那类），不能在语句中途插入。

结论：判别式复用/二分/跳表都要走**函数级预留槽**或**真正的 `OP_SWITCH` 指令**（§9.2 第二步），
不能靠中途 `add_hidden_local`。这一轮的 switch 改动已回退，语义回归脚本
[tools/bench_switch_semantics.js](../tools/bench_switch_semantics.js) 保留下来（fallthrough、middle default、
字符串 case、`-0`/`NaN`、带副作用的 case 表达式、循环里的 break/continue），下一轮改 switch 先用它兜底。