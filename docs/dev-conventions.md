# 约定：提交、回归页、文档、代码风格

## 1. 提交信息

- 标题：**英文祈使句**，一句话说清做了什么（不是"update xxx"）。
  `ant backend: real Web Workers (http(s) scripts, events, importScripts)`
- 正文用 `-m` 分段，按「现象 → 根因 → 修法 → 验证」写，包含：
  - 为什么必须改（不改会怎样，最好带数字：`100% → 3.4% CPU`、`inp=2 all=129`）；
  - 关键代码锚点（文件/函数）；
  - 跑了哪些回归（探针页、套件）。
- 一个提交一件事；`docs:` / `shell:` / `ant backend:` / `esm:` 这类前缀已经在用，保持一致。
- 不提交构建产物，除了 `3rdparty/ant/lib/antjsb.dll.a` 和 `3rdparty/ant/bin/antjsb.dll`
  （plain clone 的预编译回落；PDB 不跟）。

## 2. 回归页与脚本

- 临时调试页放 `debug/`——但它是 **`.gitignore` 忽略**的，全新 clone 里没有。
  所以要留下来的回归资产有两个去处：
  - 页面 → `tools/pages/`（被跟踪，如 `iconfont_render.html`）；
  - 脚本 → `tools/`（被跟踪，如 `run_worker_tests.ps1`、`check_window_visible.ps1`）。
- 页命名习惯：子系统 + 用途（`csscheck.html`、`scrollcheck.html`、`defercheck.html`、`worker_support.html`）；
  能自测的页面自己算 `window.__t` / `window.__fail`，脚本只读结论。
- 高频用的手工命令 → 写进 `tools/*.ps1`（AGENTS.md 的规则：**helper scripts 要可复用**）。

## 3. 文档怎么更新

| 变化 | 写到哪 |
|---|---|
| 修掉一个缺陷 | `docs/hist-lessons.md` 追加 `R<n>`：现象 / 根因 / 修法（代码锚点）/ 验证（可重跑判据） |
| 能力、限制、已证伪的旧结论变了 | `docs/arch-status.md`（能力/限制）；API 级细节改 `docs/arch-capability-gaps.md` |
| 线程 / 所有权 / realm / 模块 / 下载边界变了 | 对应的 `docs/arch-*.md`（总图、模块、下载） |
| 新的设计决策 | `docs/design-*.md` 新建**设计记录**，顶部写"不是现状"，并在 `docs/README.md` 加一行 |
| 待办或验收标准变了 | `docs/plan-roadmap.md` |
| 新的调试开关/环境变量 | `docs/dev-debugging.md` 的环境变量表 |
| 新的构建/运行坑 | `docs/dev-build-and-run.md` 的环境坑 |
| 新的改法（改 X 去哪） | `docs/dev-onboarding.md` §3 的任务索引表 |
| 发现/修掉一个 Web 能力缺口（JS/DOM/CSS） | `docs/arch-capability-gaps.md` 的表格 + 对应 `tools/pages/cap_*.html` 探测值 |
| 跨仓库真源 / 函数锚点 | `docs/ref-ant-engine.md`、`docs/ref-code-index.md` |

原则：**当前态只写 `arch-*`，设计记录只写 `design-*`，历史只写 `hist-*`，操作只写 `dev-*`，查表只写 `ref-*`**。
同一件事只在一个地方详述，其它地方给链接；`AGENTS.md` 只放铁律与地图。

## 4. 代码风格

- 沿用 **WebKit 2008 风格**：4 空格缩进、大括号换行、`m_` 成员前缀、`RefPtr`/`PassRefPtr`。
  仓库里有 `tools/code_format.sh` 和 `tools/code_style.ini` 可参考。
- 桥接层（`H:\ant-v16\bridge\AntBridge.c`）是 C：K&R 大括号、`snake_case` 函数名、`antb_` 前缀；注释解释**为什么**。
- `prelude.js` / `polyfills.js`：2 空格缩进、`var`（要兼容老引擎）、IIFE 包裹；
  改完 `node --check` 一下（`tools/check_prelude_lines.py` 专门查"字符串内换行"）。
- 不许 `clang-format` 全量刷文件（会把 diff 变成几千行）；只格式化自己改的片段。
- 默认日志（`[evt]/[nav]/[ui]/[shell]/[probe]/[flag]/[con-*]`）必须走 `src/webview/AgbLog.h` 的
  `agbLog(AGB_LOG_*, ...)`，不要在调用点自己 `fprintf(stderr, "[tag] ...")`：时间戳、通道开关、
  跨 DLL 共用时钟都在收口处统一。页面 console 由 polyfills 守卫转 `__agbConsoleLineEx(level, text)`
  （`agbLogConsole`，级别进 tag：`con-log`/`con-info`/`con-warn`/`con-error`/`con-debug`；一个通道
  `AGB_LOG_CONSOLE`，开关 `AGB_CON_LOG=0`）。
  可选调试通道（`[netlog]` 等）另说。

## 5. 提交前 checklist

1. `cmake --build build-zig-native` 通过（改了桥/引擎则连 dll 一起重链，见 [ref-ant-engine.md](ref-ant-engine.md)）。
2. 探针跑过受影响的页面；**退出码**是 0 或与基线一致。
3. `--probe` 相关改动：再跑 `tools/run_worker_tests.ps1` + deepseek 登录页（`inp=2 all=129`）。
4. 文档按 §3 更新（arch-status / arch-capability-gaps / hist-lessons / 本目录）。
5. `git status` 干净（`debug/`、`build-zig-native/`、`*.log` 都不该出现）。
