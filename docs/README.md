# agave 文档索引

先读 [`../AGENTS.md`](../AGENTS.md)（铁律 + 最常用命令），再按本文跳。
**一级目录即全部文档**：前缀表示文档类型，避免把"现状"和"设计记录/历史"混读。

## 前缀约定

| 前缀 | 类型 | 内容规则 |
|---|---|---|
| `arch-` | 当前架构 / 现状 | 现在时；只写现在是什么；不写 R 历史；文件顶部标"最后核对" |
| `design-` | 设计记录 | 冻结在决策时；顶部必须写"不是现状"，当前态链到 `arch-` |
| `dev-` | 开发指南 | 命令、路由、规范；不重复架构叙事 |
| `hist-` | 历史 | 只追加 R 条目（现象/根因/修法/验证）；不描述现状 |
| `plan-` | 计划 | 待办 + 验收标准；向前看 |
| `ref-` | 参考 | 查表：跨仓库真源、函数/机制锚点 |

## 全部文档

| 文档 | 写什么 | 什么时候看 |
|---|---|---|
| [arch-overview.md](arch-overview.md) | **当前架构总图**：进程/线程/模块、代码分层、realm 与所有权、导航、下载/内部页、边界 | 第一次需要完整图，或怀疑某篇文档过时 |
| [arch-status.md](arch-status.md) | **现状**：已具备能力、已知限制、验证入口 | 想知道"现在能做什么、哪些不是 bug" |
| [arch-rendering.md](arch-rendering.md) | **内核渲染管线**：DOM → 样式 → 渲染树 → 布局 → 绘制；布局内部、几何读取、症状→文件 | 布局/间距/0×0/命中不对，或第一次改 WebCore 渲染 |
| [arch-module-loading.md](arch-module-loading.md) | 模块加载与求值：三条泳道时序图 + 代码锚点 | 改模块加载/解析时序 |
| [arch-downloads.md](arch-downloads.md) | 下载子系统：决策点、所有权、状态机、三层 API、线程模型、面板 | 改下载 / 加下载 UI |
| [arch-embedding.md](arch-embedding.md) | 宿主接入契约：控件 API、线程模型、多控件 | 集成 agave / 写宿主 |
| [arch-capability-gaps.md](arch-capability-gaps.md) | JS/DOM/CSS 不支持/半实现清单：实测值、影响、复测命令 | 页面用了某 API 行为不对 / 评估工作量 |
| [arch-media.md](arch-media.md) | **媒体元素**：`<video>`/`<audio>`/`<track>` 的类、UA 版式与尺寸、播放状态机、元数据解析、可选 ffmpeg 抽帧 | 改媒体元素 / 页面视频音频行为不对 |
| [arch-web-font.md](arch-web-font.md) | `@font-face` 链路与坑 | 字体问题 |
| [design-per-view-js-context.md](design-per-view-js-context.md) | 每 view 独立 JS 上下文（设计记录，已实现） | 改 realm / 多 view / iframe |
| [design-frame-window.md](design-frame-window.md) | iframe 与 window 对象（设计记录）：规范差距、方案对比、实施 | 改 frame / window 身份 / 跨 frame 通信 |
| [design-per-frame-realm.md](design-per-frame-realm.md) | **每 frame 一个 realm**（设计记录）：ant realm 机制、影响面、S0–S5 迁移与验收 | 改 realm / iframe 隔离 / window 身份 |
| [design-realm-document-binding.md](design-realm-document-binding.md) | **realm 级 document 绑定**（设计记录，未实施）：`g_document` 环境读错、按对象解析、location/history 对象化、B0–B4 迁移 | 改 iframe 里的 document/location/history，或同类“跨 realm 读错文档” |
| [design-elem-fastpath.md](design-elem-fastpath.md) | **元素访问快路径**（设计记录）：稀疏下标/字符串键 140–200 ns 实测、shape 索引表与站点 IC、验收阈值 | 改 `obj[key]` 性能 / VM 卡死 / P1-g |
| [design-select-popup.md](design-select-popup.md) | **`<select>` 下拉菜单**（设计记录）：为什么画在 view 自己的 surface 上、PopupMenu 双模式、输入路由、已知限制 | 改下拉菜单 / 表单控件输入 / 加原生 popup |
| [design-tab-teardown.md](design-tab-teardown.md) | **关 tab / view 拆除生命周期**（设计记录）：引擎线程独占、reap 队列、realm teardown 统一入口、文档级资源清理（字体） | 改关闭/销毁路径、页面资源清理、`agave_webview_destroy` |
| [design-render-backend.md](design-render-backend.md) | **S3：多 tab 并行与可插拔渲染后端**（设计记录）：local/进程/RVVM 三后端、渲染后端协议 v1（命令/事件/帧/服务）、逐调用 GL 转发实测（`bench_ecall`）、B0–B5 分批与验收 | 做多 tab 并行/隔离、跨进程或 RVVM 渲染后端、评估 GL 合成 |
| [design-shell-singleton.md](design-shell-singleton.md) | **单实例 shell**（设计记录）：一个 profile 一个浏览器进程、第二次启动的命令接力协议（`--new-tab`/`--new-window`）、同进程多窗口模型、失败模式 | 改启动流程/多窗口/`--userdata` 语义、加第二个实例才发现的行为 |
| [design-shell-ui.md](design-shell-ui.md) | **壳 UI 组件化 + 平台 ops**（设计记录）：`UiChrome/UiWidgets/UiTheme` 分层、`shell_ops_t` OS 缝合口、亮暗/DPI 主题、无头 `--ui-shot` 验证 | 改工具栏/标签栏/主题/DPI、加壳 UI 控件、把 OS 调用从 shell 里挪走 |
| [dev-onboarding.md](dev-onboarding.md) | 代码在哪、**我要改 X 去哪**、症状→先查哪 | 第一次改代码，或换子系统 |
| [dev-build-and-run.md](dev-build-and-run.md) | 工具链、构建路径、改什么重编什么、产物拓扑、环境坑 | 编不动 / 跑不起来 |
| [dev-debugging.md](dev-debugging.md) | 无头探针、日志、环境变量全表、cdb、噪音对照、A/B、Chrome 对拍 | 页面不对、崩了、卡住、想查 Chrome 怎么算 |
| [dev-tests.md](dev-tests.md) | **测试清单**：24 个 `run_*.ps1` 套件的覆盖/依赖/判据、改动→该跑哪些、加回归页的写法、fixture 在哪 | 改完代码要验证 / 想知道有没有现成测试 |
| [dev-conventions.md](dev-conventions.md) | 提交信息、回归页、文档更新、代码风格 | 提交前 |
| [hist-lessons.md](hist-lessons.md) | R 系列经验库：现象 → 根因 → 修法 → 验证 | 遇到眼熟的 bug、想推翻旧结论 |
| [plan-roadmap.md](plan-roadmap.md) | 待办与验收标准（P0/P1/P2） | 想知道"接下来做什么" |
| [plan-realm-document-binding.md](plan-realm-document-binding.md) | **realm document 绑定实施计划 B0–B4**（自包含、断点续做）：文件拆分、步骤、命令、进度表 | 接手/继续 R130 之后的 realm 修复 |
| [ref-virtpass-browser.md](ref-virtpass-browser.md) | **virtpass_browser v1**（跨仓库接口规格，交付 RVVM 侧）：两跳拓扑、共享控制块、ecall 表、中断/帧搬运、RVVM 侧待实现清单、验收 | 做 S3 B3（RVVM 渲染后端）/ 与 RVVM 侧对接 |
| [ref-ant-engine.md](ref-ant-engine.md) | ant/桥接**哪份是真的**、改引擎链路、ant 的反直觉 | 碰到 JS 引擎、桥接、worker、wasm |
| [ref-code-index.md](ref-code-index.md) | 函数/机制锚点索引 | 知道机制名、要找函数 |

## 常见路径

- **新人**：`AGENTS.md` → [arch-overview.md](arch-overview.md) → [dev-onboarding.md](dev-onboarding.md) → [arch-status.md](arch-status.md)。
- **改代码**：[dev-onboarding.md](dev-onboarding.md) §3 → [arch-rendering.md](arch-rendering.md)（布局/绘制）→ [ref-code-index.md](ref-code-index.md) → [dev-build-and-run.md](dev-build-and-run.md)。
- **查 bug**：[dev-debugging.md](dev-debugging.md) → [arch-capability-gaps.md](arch-capability-gaps.md) → [hist-lessons.md](hist-lessons.md)（布局问题先看 [arch-rendering.md](arch-rendering.md) §7）。
- **验证改动**：[dev-tests.md](dev-tests.md)（该跑哪个套件 + 怎么加回归页）→ [dev-debugging.md](dev-debugging.md) §4（Chrome 对拍）。
- **做设计**：[design-frame-window.md](design-frame-window.md) / [design-per-view-js-context.md](design-per-view-js-context.md) → [hist-lessons.md](hist-lessons.md)。

## 维护规则

- 修掉一个缺陷 → [hist-lessons.md](hist-lessons.md) 追加 `R<n>`（现象/根因/修法/验证，带代码锚点）。
- 能力变了 → 更新 [arch-status.md](arch-status.md) + [arch-capability-gaps.md](arch-capability-gaps.md) 的实测值。
- 线程 / 所有权 / realm / 模块 / 下载边界变了 → 改对应的 `arch-*.md`，不要只改设计记录。
- 新的设计决策 → 新建 `design-*.md`，顶部写"设计记录，不是现状"。
- 新的待办 → [plan-roadmap.md](plan-roadmap.md)；完成项保留验收记录。
- 操作手法（命令、环境变量、坑）→ 只写在 `dev-*.md`；`AGENTS.md` 不重复细节。
- 命名：全小写 kebab-case；只有 `README.md` / `AGENTS.md` 大写；不要 `handover`、日期、`final`。