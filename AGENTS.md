# agave 浏览器引擎 —— Agent 入口

agave 是**从零移植的浏览器**：WebKit 2008 的 WebCore（解析/布局/绘制/网络）+ ant JS 引擎
（`H:\ant-v16`：VM/编译、ESM、Worker、wasm）+ Win32 控件与宿主壳。目标是让真实站点
（deepseek、百度、Vue 应用……）表现对齐现代浏览器。

[script engine](tools/build_antjsb.ps1) · [build proj](tools/build_project.ps1) · [cdb](tools/cdb.exe)

## 文档地图（详情都在 docs/，这里只放铁律）

| 想知道 | 看 |
|---|---|
| 全部文档索引 + 前缀约定 | [docs/README.md](docs/README.md) |
| 当前架构总图 / 现状 / 已知限制 | [docs/arch-overview.md](docs/arch-overview.md)、[docs/arch-status.md](docs/arch-status.md) |
| 内核渲染管线：DOM→样式→布局→绘制；**布局问题先看哪** | [docs/arch-rendering.md](docs/arch-rendering.md) |
| 待办与验收标准 | [docs/plan-roadmap.md](docs/plan-roadmap.md) |
| 代码在哪、**我要改 X 去哪**、症状→先查哪 | [docs/dev-onboarding.md](docs/dev-onboarding.md) |
| 构建/运行 / 调试、日志、回归 | [docs/dev-build-and-run.md](docs/dev-build-and-run.md)、[docs/dev-debugging.md](docs/dev-debugging.md) |
| JS/DOM/CSS **能力缺口**（不支持清单 + 一条命令复测） | [docs/arch-capability-gaps.md](docs/arch-capability-gaps.md) |
| ant/桥接哪份是真的、函数/机制锚点 | [docs/ref-ant-engine.md](docs/ref-ant-engine.md)、[docs/ref-code-index.md](docs/ref-code-index.md) |
| 提交/回归页/文档/代码风格约定 | [docs/dev-conventions.md](docs/dev-conventions.md) |
| 历史缺陷：现象 → 根因 → 修法 → 验证（R 系列） | [docs/hist-lessons.md](docs/hist-lessons.md) |
| 设计记录（**不是现状**） | [docs/design-per-view-js-context.md](docs/design-per-view-js-context.md)、[docs/design-frame-window.md](docs/design-frame-window.md) |

## 构建依赖（速查，细节见 ref-ant-engine）

- **两个 checkout**：agave 本仓库 + ant（默认 `H:\ant-v16`，分支 `agave-bridge`）；ant 源码不在本仓库。
- **默认构建不要求 ant 源码**：CMake 只消费 `3rdparty/ant` 的发布副本
  （`include/AntBridge.h`、`lib/antjsb.dll.a`、`bin/antjsb.dll`）。
- **改桥 / ant 内核**：`pwsh -File tools/build_antjsb.ps1`（Meson target `antjsb`），
  前置是 ant checkout + Meson/Ninja/MSYS2/Python3；LTO 保持 `b_lto=false`。
- **改了 ant 要在 ant 仓库提交**：ant 是独立仓库（`H:\ant-v16`，分支 `agave-bridge`），
  桥/内核的改动提交在**那边**（或至少导出 patch 到 `tools/ant-patches/`）；本仓库只提交
  发布产物 `3rdparty/ant/{bin,lib,include}`。否则下次同步基线/重建会把改动静默丢掉，
  dll 与源码不再对应。`tools/build_antjsb.ps1` 在 ant 工作区不干净时会打印警告。
- **依赖链**：`agave_shell/embed -> libagave.dll -> antjsb.dll`；控件已编进 `libagave`，
  没有 `libagave_webview.dll`。
- **开关**：`OPT_JS_ANT=ON` 才链接 `antjsb`；`OPT_NET_BACKEND=native` 走 WinHTTP。

## 文档更新（够用就行）

- **什么时候写**：代码的当前行为、能力边界、构建/调试方式变了才写；纯重构、改名、格式化不必为了凑 diff 改文档。
- **写到哪**：现状 → `docs/arch-*.md`；待办/验收 → `docs/plan-roadmap.md`；可复用的根因/验证 →
  `docs/hist-lessons.md` 追加 `R<n>`；设计决策 → `docs/design-*.md`；命令/环境变量/坑 →
  `docs/dev-*.md`；函数锚点/跨仓库真源 → `docs/ref-*.md`。
- **写多少**：一个改动只改最相关的一处；现有文档能容纳就就地改，新子系统/新设计才新建文件。
  同一事实只在一处详述，其它地方给链接。
- **不要混**：`arch-*` 用现在时，不写“R27 修了什么”；历史只进 `hist-lessons`，设计记录只进 `design-*`。
- **保持一致**：改文件名/标题/章节时顺手改引用链接，至少确认相对链接可达；文档与代码冲突时
  以代码为准，在当次改动里修正，来不及就记进 `plan-roadmap.md`。
- **不要求**：不要求每个提交写 changelog，不要求为纯重构补文档，也不把细节搬进 `AGENTS.md`。

## rules

DON'T CLEAN THE BUILD UNLESS YOU MUST.

DON'T SAY THE CONTEXT WILL GET FULL; IT IS AUTO-COMPACTED AND WILL NEVER BE FULL.

WHEN A BUG IS DEEP, DIG DEEP.

FOR SOME BUGS OR REQUIREMENTS THAT REQUIRE INFRASTRUCTURE ADJUSTMENTS, MAKE A DESIGN, RESPOND WITH A PLAN, AND IMPLEMENT IT.

MAKE HELPER SCRIPTS REUSABLE; WHEN A SCRIPT IS HIGH-FREQUENCY USED, PLACE IT IN THE PROJECT.

任务中经历的本该优化的阻塞点，可以临时用脚本方式绕过，但需要记录并提示延后处置。

**发现缺口必须记录并推进**：凡是发现能力缺口、半成品路径、与现代浏览器不一致的行为（不只是这次修的那个 bug），
都要当场写进 [docs/plan-roadmap.md](docs/plan-roadmap.md)（优先级 + 验收判据），修完把结论写回
`docs/arch-*.md` / `docs/hist-lessons.md`；不允许只在对话里说一句"已知残留"就算完。
评审后决定不做的项，写成**「不推进 + 理由/替代路径」**（放在 plan-roadmap 的「P2 不推进」），
避免以后被当成待办重开。

WHEN USER REPLY A DOT[.] ONLY, MEANS OK/DONE/DO_AS_YOU_SAY/YES/GO_ON/ACCEPT/...

## 七条铁律（违反会**静默**失败）

1. **桥源码在 ant 仓库**：真源是 `H:\ant-v16\bridge\AntBridge.c`，改完跑
   `pwsh -File tools/build_antjsb.ps1` 重建 `antjsb.dll` / `antjsb.dll.a` / `AntBridge.h`，
   再 relink `libagave`。agave 里不再保留桥的副本。
2. **改 ant 内核也走同一个脚本**：`pwsh -File tools/build_antjsb.ps1` 会先编 `libant.a`
   再链 `antjsb.dll`；只改本仓库 C++ 时才需要 `cmake --build build-zig-native`。
3. **重编前先停掉正在跑的 `agave_shell`**：旧进程占着 `antjsb.dll` 时链接静默失败，跑的还是旧 dll。
4. `prelude.js` / `polyfills.js` 是**运行时**从 exe 旁加载的：拷到 `build-zig-native\` 即生效，**不用重链**。
5. 探针默认**真无头**（不建浏览器窗口）；要像素用 `--shot <bmp>`（抓控件自己的 mirror 缓冲），`AGB_PROBE_SHOW=1` 只在调试「窗口可见时行为不同」的路径时才用。
6. **不要 rebase** ant 的 `agave-bridge`：基线是冻结 tag，落后上游 54 个 commit，同步另外安排。
7. **截图不许打扰用户**：用 `tools/screenshot.ps1`（= `--probe ... --shot`，窗口从不 `ShowWindow`，不抢焦点、不上任务栏）；不要启动 GUI 壳截图、不要 `SetForegroundWindow`、不要截屏抓桌面。能用探针数字判断的，就不要截图。

## 三个最常用命令

```powershell
# 增量构建（输出落盘再 grep，别直接灌上下文）
pwsh -File tools\build_project.ps1 *> build-native.log; $LASTEXITCODE

# 无头探针：退出码是权威判据（0 OK / 1 usage / 2..4 超时或无 exit / 5 加载错误 / 6 空文档）
agave_shell.exe --probe <url> --eval "probeExit(...)" --wait 3000 --settle 1000 --timeout 60000

# Worker 回归（起本地 http server → 跑九项 → 收服务）
pwsh -File tools\run_worker_tests.ps1
```
