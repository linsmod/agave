# agave 浏览器引擎 —— Agent 入口

agave 是**从零移植的浏览器**：WebKit 2008 的 WebCore（解析/布局/绘制/网络）+ ant JS 引擎
（`H:\ant-v16`：VM/编译、ESM、Worker、wasm）+ Win32 控件与宿主壳。目标是让真实站点
（deepseek、百度、Vue 应用……）表现对齐现代浏览器。

[script engine](tools/build_antbridge.ps1) · [build proj](tools/build_project.ps1) · [cdb](tools/cdb.exe)

## 文档地图（详情都在 docs/，这里只放铁律）

| 想知道 | 看 |
|---|---|
| 全部文档索引 | [docs/README.md](docs/README.md) |
| 分层架构 / 目录地图 / **我要改 X 去哪** / 症状→先查哪 | [docs/onboarding.md](docs/onboarding.md) |
| 工具链、构建、产物与运行拓扑、环境坑 | [docs/build-and-run.md](docs/build-and-run.md) |
| 探针、默认日志（`[evt]`/`[nav]`/`[ui]`/`[con-call]`）、环境变量全表、cdb、噪音对照、A/B 验证 | [docs/debugging.md](docs/debugging.md) |
| JS/DOM/CSS **能力缺口**（不支持清单 + 一条命令复测） | [docs/capability-gaps.md](docs/capability-gaps.md) |
| ant/桥接哪份是真的、改引擎链路、ant 的反直觉 | [docs/ant-engine.md](docs/ant-engine.md) |
| 提交/回归页/文档/代码风格约定 | [docs/conventions.md](docs/conventions.md) |
| 现状、待办（含验收标准）、已知限制、代码索引 | [handover.md](handover.md) |
| 历史缺陷：现象 → 根因 → 修法 → 验证（R1..R26） | [docs/lessons.md](docs/lessons.md) |
| 入口模块管线三条泳道时序图 | [docs/architecture/module-pipeline.md](docs/architecture/module-pipeline.md) |

## rules

DON'T CLEAN THE BUILD UNLESS YOU MUST.

DON'T SAY THE CONTEXT WILL GET FULL; IT IS AUTO-COMPACTED AND WILL NEVER BE FULL.

WHEN A BUG IS DEEP, DIG DEEP.

FOR SOME BUGS OR REQUIREMENTS THAT REQUIRE INFRASTRUCTURE ADJUSTMENTS, MAKE A DESIGN, RESPOND WITH A PLAN, AND IMPLEMENT IT.

MAKE HELPER SCRIPTS REUSABLE; WHEN A SCRIPT IS HIGH-FREQUENCY USED, PLACE IT IN THE PROJECT.

任务中经历的本该优化的阻塞点，可以临时用脚本方式绕过，但需要记录并提示延后处置。

WHEN USER REPLY A DOT[.] ONLY, MEANS OK/DONE/DO_AS_YOU_SAY/YES/GO_ON/ACCEPT/...

## 六条铁律（违反会**静默**失败）

1. **改 `AntBridge.c` 只改本仓库** `src/javascript/ant_backend/` 那份，然后
   `pwsh -File tools/build_antbridge.ps1 -Force` + relink。`H:\ant-v16\bridge\AntBridge.c` 会被脚本覆盖，改了白改。
2. **改 ant 内核先重编 libant**：`ninja -C H:\ant-v16\build-zig libant.a` → 再重链 dll；
   只改本仓库 C++ 时才需要 `cmake --build build-zig-native`。
3. **重编前先停掉正在跑的 `agave_shell`**：旧进程占着 `antbridge.dll` 时链接静默失败，跑的还是旧 dll。
4. `prelude.js` / `polyfills.js` 是**运行时**从 exe 旁加载的：拷到 `build-zig-native\` 即生效，**不用重链**。
5. 探针默认**真无头**（不建浏览器窗口）；要像素/截图才 `AGB_PROBE_SHOW=1`。
6. **不要 rebase** ant 的 `agave-bridge`：基线是冻结 tag，落后上游 54 个 commit，同步另外安排。

## 三个最常用命令

```powershell
# 增量构建（输出落盘再 grep，别直接灌上下文）
pwsh -File tools\build_project.ps1 *> build-native.log; $LASTEXITCODE

# 无头探针：退出码是权威判据（0 OK / 1 usage / 2..4 超时或无 exit / 5 加载错误 / 6 空文档）
agave_shell.exe --probe <url> --eval "probeExit(...)" --wait 3000 --settle 1000 --timeout 60000

# Worker 回归（起本地 http server → 跑九项 → 收服务）
pwsh -File tools\run_worker_tests.ps1
```
