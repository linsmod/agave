# agave 文档索引

先读 [`../AGENTS.md`](../AGENTS.md)（铁律 + 文档地图），再按下面这张表跳。

| 文档 | 写什么 | 什么时候看 |
|---|---|---|
| [../AGENTS.md](../AGENTS.md) | 项目一句话、rules、六条铁律、最常用命令 | **每次会话开始** |
| [onboarding.md](onboarding.md) | 分层架构、目录地图、四个入口锚点、**我要改 X 去哪**、症状→先查哪 | 第一次改代码，或换个子系统 |
| [build-and-run.md](build-and-run.md) | 工具链、三条构建路径、改什么重编什么、产物与运行拓扑、环境坑 | 编不动 / 跑不起来 / 不知道 exe 怎么摆 |
| [debugging.md](debugging.md) | 无头探针、默认日志（`[evt]`/`[nav]`/`[ui]`/`[con-call]`）、环境变量全表、cdb 配方、定位手段、已知噪音 vs 真错误、A/B 验证 | 页面不对、崩了、卡住 |
| [capability-gaps.md](capability-gaps.md) | JS/DOM 与 CSS **不支持/半实现**清单：实测值、影响、复测命令、修复后收尾 | 页面用了某 API 行为不对 / 评估工作量 |
| [ant-engine.md](ant-engine.md) | ant/桥接**哪份是真的**、改引擎链路、ant.exe 单跑、ant 的反直觉清单 | 碰到 JS 引擎、桥接、worker、wasm |
| [conventions.md](conventions.md) | 提交信息、回归页放哪、文档怎么更新、代码风格 | 提交前 |
| [../handover.md](../handover.md) | 现状、待办（含验收标准）、已知限制、代码索引 | 想知道"还差什么" |
| [lessons.md](lessons.md) | R1..R46 经验库：现象 → 根因 → 修法 → 验证 | 遇到眼熟的 bug、或想推翻旧结论 |
| [architecture/module-pipeline.md](architecture/module-pipeline.md) | 入口模块管线（方案 A）三条泳道时序图 + 代码锚点 | 改模块加载/解析时序 |
| [architecture/per-view-js-context.md](architecture/per-view-js-context.md) | 每 view 独立 JS 上下文（设计记录，已实现） | 改 realm / 多 view / iframe |
| [architecture/frame-window.md](architecture/frame-window.md) | iframe 与 window 对象：规范差距、方案对比、per-frame window 代理的实施计划 | 改 frame / window 身份 / 跨 frame 通信 |
| [embedding.md](embedding.md) | 对外嵌入接口 | 集成 agave |
| [web-font.md](web-font.md) | `@font-face` 链路与坑 | 字体问题 |

## 维护规则

- 修掉一个缺陷 → 在 [lessons.md](lessons.md) 追加一条 `R<n>`（现象/根因/修法/验证四段，带代码锚点）。
- 发现/修掉一个 Web 能力缺口 → 更新 [capability-gaps.md](capability-gaps.md) 的表格与 `tools/pages/cap_*.html` 探测值。
- 状态或待办变了 → 改 [handover.md](../handover.md)（它的 §1 现状、§4 待办）。
- 新机制/新架构 → `docs/architecture/` 新建一篇，并在本索引加一行。
- 操作手法（命令、环境变量、踩坑）→ 只写在 build-and-run / debugging / ant-engine 里，AGENTS.md 不重复细节。
