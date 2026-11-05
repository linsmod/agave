# ant 引擎与桥接：哪份是真的、怎么改

两套 git 仓库互不相干，同名文件各有拷贝，**改错地方会被静默覆盖**。

## 1. 真源矩阵（2026-11-04 核实）

| 对象 | 真源（改这里） | 其它拷贝 |
|---|---|---|
| ant 内核 `libant.a` / `src/silver/*` / `src/esm/*` / `include/*` | `H:\ant-v16`（分支 `agave-bridge`） | 本仓库**没有**引擎源码 |
| `AntBridge.c` / `AntBridge.h` | **本仓库** `src/javascript/ant_backend/` | `H:\ant-v16\bridge\`（构建时被覆盖） |
| `antbridge.dll` | `H:\ant-v16\bridge\`（链接产物） | `3rdparty/ant/bin`、`3rdparty/ant/lib`、`build-zig-native`；**运行时只加载 exe 同目录 `build-zig-native\antbridge.dll`** |
| `prelude.js` / `polyfills.js` | **本仓库** `src/javascript/ant_backend/` | `build-zig-native\`（运行时副本，可直接改来验证） |
| git 远端 | agave → GitHub `linsmod/agave`；ant 上游 → `theMackabu/ant`，本地镜像 `H:\ant-upstream`，推送 fork `fork` → `linsmod/ant` | 两套历史无关 |

## 2. 构建链路

```powershell
# 只改 AntBridge.c（本仓库那份）
pwsh -File tools\build_antbridge.ps1 -Force   # 编 dll + 同步四处副本
cmake --build build-zig-native --parallel 16  # relink libagave

# 改了 ant 内核（H:\ant-v16）
ninja -C H:\ant-v16\build-zig libant.a
pwsh -File tools\build_antbridge.ps1 -Force
```

- 改 `H:\ant-v16\bridge\AntBridge.c` **等于白改**：脚本每次都用本仓库版本覆盖它（2026-11-04 踩过）。
- 改 ant 的**头文件**会让 `libant.a` 重编 200+ 个对象（~10 分钟）；只改 `.c` 是 1-2 个对象（~1 分钟）。
- 一致性自检（两边应完全相同）：

```powershell
(Get-FileHash e:\mybrowsers\agave\src\javascript\ant_backend\AntBridge.c -Algorithm MD5).Hash
(Get-FileHash H:\ant-v16\bridge\AntBridge.c -Algorithm MD5).Hash
# dll 四处应同 hash：3rdparty\ant\bin = 3rdparty\ant\lib = build-zig-native = H:\ant-v16\bridge
```

## 3. 版本与分支

- `agave-bridge` 的基线 = tag `v16.5.c41757b4.0`（**冻结的基线**），落后上游 `origin/master` 54 个 commit；
  本地补丁叠在上面（`c1e00842` + 桥接/性能 + errors + worker 栈名几笔）。跟上游同步只能 rebase，别 merge。
- 分支已推到 fork：<https://github.com/linsmod/ant/tree/agave-bridge>（`git push fork agave-bridge`）。
  向上游提 PR 前必须先 rebase 到 `theMackabu/ant` 的 master。
- `H:\ant-v16` 历史上有 detached HEAD 检出 tag 的用法：**提交后立刻 `git switch -c <name>` 挂分支**，切走就丢。

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

## 6. 桥接的边界

- `AntBridge.c` 是**所有 JS↔C 的唯一漏斗**（`agb_dispatch(slot, js, ...)`），每个 native 方法注册一个
  trampoline 槽（`AGB_TRAMPOLINE_COUNT`）。
- wrapper 表（native 指针 ↔ JS 对象）是**进程级**的，随 engine 创建/销毁：`antb_wrap_native_owner` /
  `antb_release_owner` / `antb_engine_destroy`。
- 每个 view 一个 `RuntimeState`，每个文档一个新 engine（`beginNewRealm`/`stopRealm`），
  回调进来前靠 `antb_set_context_activate` 切状态（细节见
  [design-per-view-js-context.md](design-per-view-js-context.md)）。
