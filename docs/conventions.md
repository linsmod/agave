# 约定：提交、回归页、文档、代码风格

## 1. 提交信息

- 标题：**英文祈使句**，一句话说清做了什么（不是"update xxx"）。
  `ant backend: real Web Workers (http(s) scripts, events, importScripts)`
- 正文用 `-m` 分段，按「现象 → 根因 → 修法 → 验证」写，包含：
  - 为什么必须改（不改会怎样，最好带数字：`100% → 3.4% CPU`、`inp=2 all=129`）；
  - 关键代码锚点（文件/函数）；
  - 跑了哪些回归（探针页、套件）。
- 一个提交一件事；`docs:` / `shell:` / `ant backend:` / `esm:` 这类前缀已经在用，保持一致。
- 不提交构建产物，除了 `3rdparty/ant/lib/antbridge.dll`（分发副本，仓库一直在跟）。

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
| 修掉一个缺陷 | `docs/lessons.md` 追加 `R<n>`：现象 / 根因 / 修法（代码锚点）/ 验证（可重跑判据） |
| 现状、待办、限制变了 | `handover.md` 的 §1 现状 / §4 待办 / §5 限制 |
| 新机制或架构决策 | `docs/architecture/` 新建一篇，并在 `docs/README.md` 索引里加一行 |
| 新的调试开关/环境变量 | `docs/debugging.md` 的环境变量表 |
| 新的构建/运行坑 | `docs/build-and-run.md` 的环境坑 |
| 新的改法（改 X 去哪） | `docs/onboarding.md` 的任务索引表 |

原则：**同一件事只在一个地方详述**，其它地方给链接。AGENTS.md 只放铁律与地图。

## 4. 代码风格

- 沿用 **WebKit 2008 风格**：4 空格缩进、大括号换行、`m_` 成员前缀、`RefPtr`/`PassRefPtr`。
  仓库里有 `tools/code_format.sh` 和 `tools/code_style.ini` 可参考。
- 桥接层（`AntBridge.c`）是 C：K&R 大括号、`snake_case` 函数名、`antb_` 前缀；注释解释**为什么**。
- `prelude.js` / `polyfills.js`：2 空格缩进、`var`（要兼容老引擎）、IIFE 包裹；
  改完 `node --check` 一下（`tools/check_prelude_lines.py` 专门查"字符串内换行"）。
- 不许 `clang-format` 全量刷文件（会把 diff 变成几千行）；只格式化自己改的片段。

## 5. 提交前 checklist

1. `cmake --build build-zig-native` 通过（改了桥/引擎则连 dll 一起重链，见 [ant-engine.md](ant-engine.md)）。
2. 探针跑过受影响的页面；**退出码**是 0 或与基线一致。
3. `--probe` 相关改动：再跑 `tools/run_worker_tests.ps1` + deepseek 登录页（`inp=2 all=129`）。
4. 文档按 §3 更新（lessons / handover / 本目录）。
5. `git status` 干净（`debug/`、`build-zig-native/`、`*.log` 都不该出现）。
