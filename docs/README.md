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
| [arch-web-font.md](arch-web-font.md) | `@font-face` 链路与坑 | 字体问题 |
| [design-per-view-js-context.md](design-per-view-js-context.md) | 每 view 独立 JS 上下文（设计记录，已实现） | 改 realm / 多 view / iframe |
| [design-frame-window.md](design-frame-window.md) | iframe 与 window 对象（设计记录）：规范差距、方案对比、实施 | 改 frame / window 身份 / 跨 frame 通信 |
| [dev-onboarding.md](dev-onboarding.md) | 代码在哪、**我要改 X 去哪**、症状→先查哪 | 第一次改代码，或换子系统 |
| [dev-build-and-run.md](dev-build-and-run.md) | 工具链、构建路径、改什么重编什么、产物拓扑、环境坑 | 编不动 / 跑不起来 |
| [dev-debugging.md](dev-debugging.md) | 无头探针、日志、环境变量全表、cdb、噪音对照、回归清单、A/B | 页面不对、崩了、卡住 |
| [dev-conventions.md](dev-conventions.md) | 提交信息、回归页、文档更新、代码风格 | 提交前 |
| [hist-lessons.md](hist-lessons.md) | R 系列经验库：现象 → 根因 → 修法 → 验证 | 遇到眼熟的 bug、想推翻旧结论 |
| [plan-roadmap.md](plan-roadmap.md) | 待办与验收标准（P0/P1/P2） | 想知道"接下来做什么" |
| [ref-ant-engine.md](ref-ant-engine.md) | ant/桥接**哪份是真的**、改引擎链路、ant 的反直觉 | 碰到 JS 引擎、桥接、worker、wasm |
| [ref-code-index.md](ref-code-index.md) | 函数/机制锚点索引 | 知道机制名、要找函数 |

## 常见路径

- **新人**：`AGENTS.md` → [arch-overview.md](arch-overview.md) → [dev-onboarding.md](dev-onboarding.md) → [arch-status.md](arch-status.md)。
- **改代码**：[dev-onboarding.md](dev-onboarding.md) §3 → [arch-rendering.md](arch-rendering.md)（布局/绘制）→ [ref-code-index.md](ref-code-index.md) → [dev-build-and-run.md](dev-build-and-run.md)。
- **查 bug**：[dev-debugging.md](dev-debugging.md) → [arch-capability-gaps.md](arch-capability-gaps.md) → [hist-lessons.md](hist-lessons.md)（布局问题先看 [arch-rendering.md](arch-rendering.md) §7）。
- **做设计**：[design-frame-window.md](design-frame-window.md) / [design-per-view-js-context.md](design-per-view-js-context.md) → [hist-lessons.md](hist-lessons.md)。

## 维护规则

- 修掉一个缺陷 → [hist-lessons.md](hist-lessons.md) 追加 `R<n>`（现象/根因/修法/验证，带代码锚点）。
- 能力变了 → 更新 [arch-status.md](arch-status.md) + [arch-capability-gaps.md](arch-capability-gaps.md) 的实测值。
- 线程 / 所有权 / realm / 模块 / 下载边界变了 → 改对应的 `arch-*.md`，不要只改设计记录。
- 新的设计决策 → 新建 `design-*.md`，顶部写"设计记录，不是现状"。
- 新的待办 → [plan-roadmap.md](plan-roadmap.md)；完成项保留验收记录。
- 操作手法（命令、环境变量、坑）→ 只写在 `dev-*.md`；`AGENTS.md` 不重复细节。
- 命名：全小写 kebab-case；只有 `README.md` / `AGENTS.md` 大写；不要 `handover`、日期、`final`。
