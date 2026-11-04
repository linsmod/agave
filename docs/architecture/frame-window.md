# iframe 与 window 对象：现状、规范差距与实施计划

> 状态：**已落地（R31）**：方案 B 的 1–3 步 + 第 5 步的限制说明完成；第 4 步（`postMessage` 的
> `event.source` 走代理、`targetOrigin` 校验）还没做。验收页 `tools/pages/cap_window_identity.html`。
> 相关：`handover.md` §4 P0-a、`docs/lessons.md` R30、`docs/capability-gaps.md` §1。

## 1. 规范要求（我们差在哪）

HTML 规范里**每个 browsing context 有自己的 Window 对象**：

| 规范 | 我们现在的行为 |
|---|---|
| 每个 frame 一个 `window`，`self === window` | 所有 frame 共享同一个全局对象 |
| 子 frame 里 `var x = 1` 是**子 frame 自己的**全局 | 写进共享全局，父页也能看到（互相污染） |
| `window.parent` / `window.top` 是**别的** Window（`parent !== self`，顶层除外） | 全部指向自己（`prelude.js` 里 `g.parent = g; g.top = g`） |
| `window.frames` 是子 frame 的 Window 集合，`window.length` 是数量 | `g.frames = g`，`length` 不存在 |
| `window.name` / `window.frameElement` | 不存在 |
| `window.document` / `location` 是**本 frame** 的 | 已经是按绑定 document 求值（`g_document` / `__agbActivateSlot`），这块是对的 |
| `innerWidth/innerHeight/outerWidth/outerHeight` 是本 frame 视口 | **已修（R30）**：改成 getter，按绑定 document 求值 |

实际影响（不是理论洁癖）：

- 跨 frame 组件普遍写 `if (window.parent !== window) { ... }` 来区分"我在 iframe 里"；
  我们这里恒为 false，组件走错分支。
- 子 frame 的 polyfill / 全局缓存会污染父页（Vue/React 的 devtools 钩子、`window.__xxx`）。
- DeepSeek 登录页：微信 iframe 里的页面按"自己是一个 200×200 视口"排版，但仍被裁切，
  怀疑还有依赖 `parent`/`frames` 的分支没走到（见 §4 验收）。

## 2. 为什么不能简单地"再造一个全局"

当前架构（见 `docs/onboarding.md`、`handover.md` §2）：

- **一个 view 一个 realm**（一个 `AntbEngine`），**一个 document 一个 realm 生命周期**；
- frame 文档在桥接层有**槽位**：`g_docSlots[]` + `bindDocument()` + `__agbActivateSlot(i)`；
- 所有桥接 getter 都用 `g_document`，所以**文档级**的东西天然按帧切换；
- **全局对象只有一个**（`antb_global(engine)`），`window IS the global object`（`bindDocument` 注释）；
- wrapper 表、监听器表、模块注册表都是**进程/引擎级**的，按 generation 打补丁。

所以"每个 frame 一个 engine"意味着：`parent.foo` 跨 realm 失效、`postMessage` 需要序列化、
每个 frame 的 wrapper/监听器表要独立——这是一次架构级改造，不能顺手做。

## 3. 方案对比

### 方案 A：每个 frame 一个 engine（真隔离）
- 优点：完全符合规范，全局天然隔离。
- 代价：跨 frame 直接属性访问（`parent.foo`）失效；`postMessage` 要跨引擎序列化；
  wrapper/监听器/模块表全部要 per-engine 化；现有 `g_currentState` 单例要改成栈。
- 结论：**不做**。收益（规范洁癖）与代价（跨 frame 互操作全废）不成比例。

### 方案 B：per-frame Window 代理对象（推荐）
在**同一个 realm** 里为每个 frame 造一个 `WindowProxy`（C++ 原生对象，tag 如 `AGB_TAG_WINDOW`）：

- 固定属性（`document`/`location`/`innerWidth`/`innerHeight`/`outerWidth`/`outerHeight`/
  `parent`/`top`/`self`/`frames`/`length`/`name`/`frameElement`/`postMessage`/`addEventListener`…）
  由代理自己回答，值按该 frame 的 document 计算；
- 其它属性 **get/set 委托给共享全局**——这样 `parent.foo`、`window.jQuery`、polyfill 的全局钩子
  仍然互通（与现在的行为兼容），只是"当前 frame 的 window 对象"是独立身份；
- `window`/`self`/`top`/`parent` 在脚本里解析成**当前 frame 的代理**，`parent` 解析成父 frame 的代理；
- `var x` 仍然是共享全局的属性（**这一条 B 修不了**，属于已知限制，写进文档）。

桥接层已有同类机制可以照抄：node wrapper 的 catch-all getter/setter
（`AntRuntime.cpp` 的 `nodeGetter`/`nodeSetter` 按属性名分发），以及 `g_docSlots` 的槽位模型。

### 方案 C：只把 frame 相关属性做成 getter（最小改动）
- `parent`/`top`/`self`/`frames`/`length`/`name` 改成按帧求值。
- 代价低，但 `parent === self` 仍然成立（因为都是同一个全局），跨 frame 判定依旧错。
- 结论：**只作为 B 的中间态**，不单独采用。

## 4. 实施计划（方案 B）

| 步 | 内容 | 验收 |
|---|---|---|
| 1 | ✅ 视口度量按帧求值（R30） | `tools/pages/cap_frame_viewport.html`：父 1100×800、子 200×200 |
| 2 | ✅ 桥接层：不需要新增槽位字段 —— `Document::parentDocument()` 与 `FrameTree`（`childCount/child/name/setName`）已经能回答父子关系，`installFrameGlobals()` 在每次 bind 时刷新全局属性 | `cap_window_identity.html`：切到子 frame 后 `document`/`parent`/`length`/`name` 都是子 frame 的值 |
| 3 | ✅ C++ `WindowProxy` 对象（tag + catch-all get/set + 固定属性表），`prelude.js` 的 `window/self/top/parent/frames` 改为读代理 | 新增 `tools/pages/cap_window_identity.html`：子 frame 里 `parent !== self`、`parent.document === 父文档`、`frames.length === 1`、`name` 可读 |
| 4 | `postMessage` 走代理（`event.source` 是发送方代理，`targetOrigin` 校验） | `cap_postmessage.html` 仍 `got=1`，且 `event.source !== window` |
| 5 | ✅ 文档化已知限制：`var x` 仍是共享全局；`frames[i]` 返回代理 | `docs/capability-gaps.md` §1 更新 |

## 5. 已知限制（方案 B 之后仍然存在）

- **顶层 `var` / 函数声明仍然写进共享全局**：`parent` 里能读到子 frame 的 `var x`。
  要修只能走方案 A（每 frame 一个 engine），不做。
- `window.length` 只能统计**直接子 frame**（我们只有 document 槽位，没有 frame tree）。
- 跨 frame 直接函数调用（`parent.fn()`）继续可用（共享全局的好处），但 `this` 不是对方的 window。
