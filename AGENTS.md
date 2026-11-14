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
| 构建/运行 / 调试、日志、回归、**Chrome 对拍** | [docs/dev-build-and-run.md](docs/dev-build-and-run.md)、[docs/dev-debugging.md](docs/dev-debugging.md)（`tools/chrome_probe.py`：同一段 JS 两边跑 / 查命中的 CSS 规则 / 查真实字体 / 参考截图） |
| **项目里有哪些测试、改动后跑哪些** | [docs/dev-tests.md](docs/dev-tests.md)（25 个 `run_*.ps1` 套件 + 对拍工具 + fixture 约定） |
| JS/DOM/CSS **能力缺口**（不支持清单 + 一条命令复测） | [docs/arch-capability-gaps.md](docs/arch-capability-gaps.md) |
| `video`/`audio` 媒体元素（尺寸、播放状态机、元数据、抽帧） | [docs/arch-media.md](docs/arch-media.md) |
| ant/桥接哪份是真的、函数/机制锚点、**签入的生成文件** | [docs/ref-ant-engine.md](docs/ref-ant-engine.md)、[docs/ref-code-index.md](docs/ref-code-index.md)（`src/webcore/buildKJS/` 下是 **CSS grammar / UA 样式表的签入生成文件**，构建直接编译，**不是 KJS/JS 引擎**；改 `.y`/`.css` 必须同步改生成物） |
| RVVM 渲染后端（B3）的跨仓库接口 | [docs/ref-virtpass-browser.md](docs/ref-virtpass-browser.md) |
| 提交/回归页/文档/代码风格约定 | [docs/dev-conventions.md](docs/dev-conventions.md) |
| 历史缺陷：现象 → 根因 → 修法 → 验证（R 系列） | [docs/hist-lessons.md](docs/hist-lessons.md) |
| 设计记录（**不是现状**） | [docs/design-per-view-js-context.md](docs/design-per-view-js-context.md)、[docs/design-frame-window.md](docs/design-frame-window.md)、[docs/design-per-frame-realm.md](docs/design-per-frame-realm.md)、[docs/design-realm-document-binding.md](docs/design-realm-document-binding.md)、[docs/design-render-backend.md](docs/design-render-backend.md)（S3 多 tab 并行 / 可插拔渲染后端）、[docs/design-hidpi.md](docs/design-hidpi.md)（页面按 DPI 渲染 / 缩放与 DPI 拆分） |

## 构建依赖（速查，细节见 ref-ant-engine）

- **两个 checkout**：agave 本仓库 + ant（默认 `H:\ant-v16`，分支 `agave-bridge`）；ant 源码不在本仓库。
- **默认构建不要求 ant 源码**：CMake 只消费 `3rdparty/ant` 的发布副本
  （`include/AntBridge.h`、`lib/antjsb.dll.a`、`bin/antjsb.dll`）。
- **CSS 生成文件是签入的**：`src/webcore/buildKJS/` 下的 `CSSGrammar.cpp`、
  `UserAgentStyleSheets.{h,cpp}` 由构建直接编译；CMake / `tools/build_project.ps1`
  不跑 bison，也不跑 `tools/gen-ua-stylesheets.py`。改 `.y`/`.css` 必须同步改生成物，
  流程见 [docs/ref-code-index.md](docs/ref-code-index.md) 的“生成文件与 `buildKJS/` 目录”。
- **改桥 / ant 内核**：`pwsh -File tools/build_antjsb.ps1`（Meson target `antjsb`），
  前置是 ant checkout + Meson/Ninja/MSYS2/Python3；LTO 保持 `b_lto=false`。
- **改了 ant 要在 ant 仓库提交**：ant 是独立仓库（`H:\ant-v16`，分支 `agave-bridge`），
  桥/内核的改动提交在**那边**；本仓库只提交发布产物 `3rdparty/ant/{bin,lib,include}`，
  **不保存 ant 的 patch**（历史上的 `tools/ant-patches/` 已随桥迁到 ant 仓库而删除）。
  否则下次同步基线/重建会把改动静默丢掉，dll 与源码不再对应。
  `tools/build_antjsb.ps1` 在 ant 工作区不干净时会打印警告。
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

## Rules

### Build and Execution Safety
- PowerShell does not support heredoc. Never use `<<` in PowerShell.
- Do not clean the build unless you must.
- Do not say the context will get full; it is auto-compacted and will never be full.
- When a bug is deep, dig deep.
- If a user process prevents the build, prefix a kill command in your build toolcall.

### Development Pipeline
- Large tasks: make a design, give a plan, wait for confirmation, respond to user concerns and challenges, and give advice/suggestions. After user approval, write a self-contained full document for recovery from interruption. Implement step by step, write tests and tools as needed, run tests, clean up, and commit. Track and update progress in the document. At completion, based on what actually happened at closure, organize and register new or updated workflows—including reusable environment information and scripts—in `workflows`, so later contributors can directly start in the same scenario.
- Medium tasks: follow appropriate subsets of the full pipeline.
- Small tasks, such as minor function/feature improvements or in-place bug fixes: write simple notes, pass tests, clean up, and commit when done.
- If a bug or requirement needs infrastructure adjustments, or the change would cross many components, levels, or projects, ask the user whether they want the full development pipeline.

### User Communication and Intent
- User concerns are high priority: stop and reply immediately. Also stop and make suggestions when you find that the current design direction requires major changes.
- Not every user message asks you to start a full development pipeline. Some are just questions; explore, answer, or discuss them.
- When the user replies with only a dot [.] or something not meaningful, it means OK / YES / DO AS YOU SAY / GO ON / ACCEPT ADVICE. A dot may also mean resuming an interrupted chat. Just go on and do not mention this.
- When the user replies with question mark(s) [?], stop and explain the facts, progress, what happened, and the problem, then give suggestions for how to proceed.
- When a user question is ambiguous, list the possible interpretations and ask first to resolve the ambiguity. Do not overthink or repeatedly speculate. Once fully confirmed, restate it immediately in English. Proceed according to that restatement; do not continue reasoning from the original ambiguous content.

### Cleanup, Documentation, and Indexing
- After all work is done, write or update workflows docs and index newly created tools, docs, and scripts in the proper document or section to help others.
- Clean up one-off tools, scripts, unused or outdated code comments, and log/printf points after run confirmation, not before run checks. Apply the same rule to documentation changes.
- Make common helper tools/scripts reusable; integrate and index them in the project.
- When building a project or continuing existing work, check common locations for Quick Tools and Quick Scripts first. Do not always write your own. If existing tools/scripts are not efficient enough, upgrade them and update their usage docs if present. If no docs are found, write and index them.

### Issue Tracking and Problem Handling
- For problems that cannot be solved even after deep local code investigation, web search, and search via the user's logged-in GitHub account, skip them, continue with the remaining work, and record them during wrap-up.
- Whenever you find capability gaps, half-finished paths, or unregistered behavior inconsistent with modern browsers (for browser projects only): handle and report small issues appropriately; register large issues as new items in `issues/INDEX.md`. If the content is too long, create `issues/<details-file-name>.md` and reference it in INDEX. Optionally provide fix suggestions. If a fix is made, update `docs/arch-*.md` / `docs/hist-lessons.md` as appropriate.
- For cross-repo references, when you update another repo, write and pass tests, and commit the changes you made.

### Naming
- When designing infrastructure/architecture or planning new things, prefer common words that clearly reflect what the thing is when naming structures, files, classes, variables, and functions, instead of overly complex technical terms. Provide choices when necessary.

### Tests

- Add a command-line argument or environment variable to test programs so test runs can be distinguished from user runs.
- Tests must use separate data directories when resource contention may occur, and separate log file names when sharing the same output directory.
- Do not kill user runs during tests unless they prevent the build.
- Main tests should normally be included in the common `test-all` target. Use a parallel test suite where possible, or create one if none exists.
- Design new tests to be time-efficient and non-time-sensitive when run in parallel with other tests.
- 对于新加的测试，如无外部依赖，应该将运行时间保持在较低合理范围。
- 在文档注明预估时间或实际平均执行时间。测试必须在probe模式，对操作系统或桌面UI功能依赖应该以MOCK方式解决。
- 集中运行测试时，除了检查是否PASS，还需要度量各个测试实际执行时间。如果超出预期太多，根据情况选择仅提醒用户或修复它。
- 可以并发的测试必须并发，userdata dir冲突的独立出各自的dir去执行。有资源冲突必须串行要文档或代码注释中明确。
- 新增测试沿用已有并行模式套件或并行辅助工具或改进它，必须串行的放在具体测试脚本内部。
- 对于已有的时间敏感或并发敏感的测试，单独执行通过也算通过，不需要特别提示用户。
- 运行那些必须占用桌面切换窗口或占用键盘鼠标的测试时，如果测试结果混乱，先不要自我怀疑，重新测试一次或两次，用户注意到你在测试或许会等你。


# Write one if missing.
docs/INDEX.md is for index md under docs/, should place docs like arch-*.md, design-*.md, plan-*.md,task-*.md and so on
issus/INDEX.md is for index md or files under issues/, should place common issues found by user or you. with tag and status mark.
tools/INDEX.md is for register or mention tools/scripts, should place scripts for dev/debug/build/test/mock/deploy/clean/repo-mgnt under the dir
workflows/INDEX.md is for registered workflows, should place real dev or project-mgnt pipelines with conditions, requirements, env info, expirences, common commands for that pipe, and tool refs under the dir.


# rules of issues
> 含用户登记的问题，修复过程中发现的额外问题，在issues/INDEX.md索引，不入GIT。
> 开始解决前先标记为WIP来避免其他AGENT争抢。
> 列表中的同类问题，修复覆盖到的，可一起处理。
> 互相依赖关系的可能性较大的，先调查确认，顺序处理。
> 处理过程可以更改issues文档以完善调查结论，说明解决方案等。
> 收尾时有遗留的，将当前结案，遗留部分写为新问题。

### 状态标签

- `NEW`：JUST ADD；
- `WIP`：标记为认领，解决中；注意探索完才认领，不要认领别人的。
- `ARH`：结案，存档；已明确不做的忽略即可。因故遗留部分添加为NEW并ref一下旧的。

### issues域或组件

举例：`ui` / `shell` / `render` / `css` / `shell-pages` / `inspector` / ...（按项目实际所需补充）

### track the fixing
 - 需要超大上下文来完成目标，多个复杂步骤，或者架构级别调整时必须写issues/task/task-<task-name.md>。
 - task文档必须自包含，可预防会话中断或AGENT丢上下文时提供足够的信息。
 - issues/下的都不受GIT管理，不要和项目其他文档相互引用。
 - 小问题直接搞定不用写task文档，有歧义或难以决策时立即停下给出可能的建议并询问用户意见。

### 条目格式

- 1. `NEW` `[domain]` `[domain]` — `issue-detail.md` <问题简述> 
- 2. `WIP` `[domain]` — `文件名.md` <问题简述, 进度，可选设计或架构文档ref，相关文件ref>
- 3. `ARH` `[domain]` — `文件名.md` <问题简述，可选存档原因>
- 10. `NEW` `[domain]` `[domain]` — <问题说明> 


## rules of docs/ 

| 前缀 | 类型 | 内容规则 |
|---|---|---|
| `arch-` | 当前架构 / 现状 | 现在时；只写现在是什么；不写 R 历史；文件顶部标"最后核对" |
| `design-` | 设计记录 | 冻结在决策时；顶部必须写"不是现状"，当前态链到 `arch-` |
| `hist-` | 历史 | 只追加 R 条目（现象/根因/修法/验证）；不描述现状 |
| `plan-` | 计划 | 待办 + 验收标准；向前看 |
| `ref-` | 参考 | 查表：跨仓库真源、函数/机制锚点 |

# Workflows
Workflow docs should place under workflows/
workflows/INDEX.md is for registered workflows
workflow doc should contains real dev or project-mgnt, scene related pipelines with conditions, requirements, env info, relevant files, task expirences, common commands, tool or workflow go to branchs refs and so on.

## Common Rules for workflows
> 按实际情况选择流程执行，禁止随便选择流程。
> 如果还没有所需的对应流程，按通常流程执行目标，执行完成后根据实际过程初始化一个适合该场景的工作流。
> 具体工作流文件的命名应该字面上明确它所针对的场景
> 如果将要抽出的流程与现有流程有交叉但又不完全适用，或对原流程具有破坏性，应该分叉出新的流程。
> 流程可以中途根据情况切换或切回。
> 工作流中动态确定的部分可以类似condition-action结构,并在具体流程文档内部根据情况预设转到其他流程。
> 执行过程中留意不适用的情况，在完成流程之后填写更新意见或分叉建议。
> 如果通用流程不适用项目的具体组件，part，domain，section，scene等，就创建专门的流程。
> 具体工作流的独立私有规矩写在工作流内部，不要污染全局AGENTS规则。
> 执行完工作流之后，在工作流内部记录`执行经验`和`改进意见`。
> 以上都是隐含操作，直接执行，不暴露给用户，除非有前置规则说需要先说明要干嘛再调用工具的情况。

### workfow应该包含
> 适用的场景，必要的前置条件
> 如果有相关资产或工具，明确存放位置或命名规则方便查找或新增。
> 如需流程涉及执行命令，应该说明或引用所需的环境设置。
> 涉及具体模块时，给出存放位置，主要模块的文件路径，直接或间接但很大关联的组件名称或函数名称。
> 退出或转换的条件


### workfow 常见通用场景，
> workflows/wf-bugfix-just-do-because-simple.md
> workflows/wf-bugfix-need-more-explorer.md
> workflows/wf-bugfix-need-full-dev-pipe.md
> workflows/wf-bugfix-from-indexed-issue.md
> workflows/wf-bugfix-from-crash-info.md
> workflows/wf-fix-build-errors.md
> workflows/wf-full-dev-pipe.md
> workflows/wf-user-have-new-ideas.md
> workflows/wf-user-joined-ui-tests.md
> workflows/wf-user-have-infra-question.md
> workflows/wf-user-have-simple-question.md
> workflows/wf-user-challenge-for-plan-or-reply.md
> workflows/wf-build-and-run.md
> workflows/wf-update-test-cases-or-suites.md
> workflows/wf-update-workflows-itself.md
> workflows/wf-add-new-tests.md
> workflows/wf-design-and-impl-test-suite.md


### workflow的自动进化
> 如果按要求执行工作流之后，发现其过于简陋或过于繁复，导致实际运行的流程成本增加或者效能低下，你应该主动对更新完善相关条目或章节提出改进建议，如果建议是针对具体工作流，应该明确是改进还是分叉。如果用户同意，走wf-update-workflows-itself.md
> 如果还没有适用的工作流就按需创建或补充。
 


## 七条铁律（违反会**静默**失败）

1. **桥源码在 ant 仓库**：真源是 `H:\ant-v16\bridge\AntBridge.c`，改完跑
   `pwsh -File tools/build_antjsb.ps1` 重建 `antjsb.dll` / `antjsb.dll.a` / `AntBridge.h`，
   再 relink `libagave`。agave 里不再保留桥的副本。
2. **改 ant 内核也走同一个脚本**：`pwsh -File tools/build_antjsb.ps1` 会先编 `libant.a`
   再链 `antjsb.dll`；只改本仓库 C++ 时才需要 `cmake --build build-zig-native`。
   - 改Ant核心需要在ant repo中添加测试，改完后任务收尾时要提交。
3. **重编前先停掉正在跑的 `agave_shell`**：旧进程占着 `antjsb.dll` 时链接静默失败，跑的还是旧 dll。
4. `prelude.js` / `polyfills.js` 是**运行时**从 exe 旁加载的：拷到 `build-zig-native\` 即生效，**不用重链**。
5. 探针默认**真无头**（不建浏览器窗口）；要像素用 `--shot <png|bmp>`（抓控件自己的 mirror 缓冲；`.png` 由壳自己编码、不用再转，`.bmp` 留给 pixel_check），`--shot-rect x,y,w,h` 直接裁到某块；`AGB_PROBE_SHOW=1` 只在调试「窗口可见时行为不同」的路径时才用。
6. **不要 rebase** ant 的 `agave-bridge`：基线是冻结 tag，落后上游 54 个 commit，同步另外安排。
7. **截图用无头模式**：用 `tools/screenshot.ps1`（= `--probe ... --shot [-rect x,y,w,h]`，窗口从不 `ShowWindow`，不抢焦点、不上任务栏）；不要启动 GUI 壳截图、不要 `SetForegroundWindow`、不要截屏抓桌面。能用探针数字判断的，就不要截图。
8. **页面资源需要对接页面生命周期**：新增或重构各种页面资源无论图片，字体，样式，JS，其他与页面有关的资源，按情况在拆页（doc）时挂上恰当的资源销毁或清理函数。

10. **UI相关测试如果偶尔失败需要二次确认**
用户可能正在操作桌面会无意影响到测试结果。

## 三个最常用命令

```powershell
# 增量构建（输出落盘再 grep，别直接灌上下文；脚本自身不写日志文件）
pwsh -File tools\build_project.ps1 *> build.log; $LASTEXITCODE

# 无头探针：退出码是权威判据（0 OK / 1 usage / 2..4 超时或无 exit / 5 加载错误 / 6 空文档）
build-zig-native\agave_shell.exe --probe <url> --eval "probeExit(...)" --wait 3000 --settle 1000 --timeout 60000

# 全部离线回归（20 个套件，默认全并行约 25 秒；-Sequential 约 2 分钟。见 docs/dev-tests.md §1）
pwsh -File tools\run_all_tests.ps1          # 单个：-Only process；跳过：-Skip run_layout_tests 并发量：见脚本内注释

# 个别测试的时间敏感或负载敏感是已知问题，单独跑能过也算通过，不用记也不用专门提。
```


## SKILLS
[Quick Search Files on Local Machine ](c:/Users/linswin/.agents/skills/everything-search/SKILL.md)
[Sandbox Bypass](c:/Users/linswin/.agents/skills/traycmd/SKILL.md) Section:一次性/临时命令


## Known Agent Applicaiton Bug
- 文件读写异常，表现为权限不足。遇到时请要求用户重启一次。

## Enable New Vendor libs
May need edit `build/mconfig.h.in`

## The main program or shell-ui or agave browser program.
[borwser program](examples/browser_shell/main.c)
