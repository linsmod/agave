# 每个 view 独立的 JS 上下文（设计记录：per-view Ant engine）

> 状态：**设计记录**。2026-11-03 起已实现（每个 view 一个 `RuntimeState` + 一个 `AntbEngine`，
> `AntRuntime::createContext()`；每个文档再换一个新 engine，`beginNewRealm()` / `stopRealm()`，
> 见 [hist-lessons.md](hist-lessons.md) R10/R17）。
> 本文保留当时的实施方案与权衡；下面「现状」「需要改的点」「工作量」都是**设计时**的表述。
> 当前实现在 [arch-overview.md](arch-overview.md) §3。

## 现状（设计时，2026-11-02）

* `AntRuntime` 是进程级单例：`g_engine`（一个 `AntbEngine`）、`g_document`、
  `g_docObj`、`g_documentBound`、`g_nodeClass` 等全是文件级全局。
* 文档隔离是用"槽位"模拟的：`g_docSlots[]` + `activateSlot()`，
  定时器/XHR 回调进入前切回自己的文档（现状见 `AntRuntime.cpp` 的 `slotForDocument()` / `activateSlot()`）。
* 所以 **DOM 层面**已经隔离，但 **JS 全局对象**没有：
  控件 A 执行 `window.foo = 1`，控件 B 能读到 `window.foo === 1`。

## 目标

两个控件各自有独立的 `window`，`window.foo` 互不可见；
`document`、`location`、事件监听、定时器、XHR 天然跟随各自 view。

## 方案 A（推荐）：per-view Ant engine + 入口处上下文切换

### 结构

```cpp
struct RuntimeState {
    AntbEngine*  engine;
    Document*    document;
    AntbValue*   docObj;
    bool         documentBound;
    AntbClass*   nodeClass;
    /* listener map / xhr jobs / pending wrappers ... */
};
static std::vector<RuntimeState*> g_states;   // 每个 view 一份
static RuntimeState* g_current;               // 引擎线程当前的
```

保留现有全局变量名，但让它们变成"当前状态"的镜像：
进入某个引擎的回调时，把 `g_*` 从对应 `RuntimeState` 灌进去。

### 为什么这样切最省

`AntBridge` 的 `agb_dispatch(slot, js, ...)` 是**所有** JS 调 C 的唯一入口，
`js`（`ant_t*`）能唯一定位引擎。在 dispatch 开头调用一个钩子：

```c
agb_context_activate(js);   /* 由 AntRuntime 实现：g_current = stateFor(js)，
                               并把 g_engine/g_document/... 指向它 */
```

一次性覆盖全部 150+ 个 native 回调，不用逐个改签名。

### 重要：antbridge.dll 必须手工重建

`src/javascript/ant_backend/AntBridge.c` 是**源码副本**，仓库里的
`3rdparty/ant/bin/antbridge.dll` 是预先构建好的二进制，
agave 的 CMake 只负责把它拷到输出目录。

所以对 `AntBridge.c` 的任何修改（包括本文提到的 trampoline 扩容）
**不会**自动生效，必须：

1. 把 `src/javascript/ant_backend/AntBridge.c` 同步到
   `H:\ant-v16\bridge\AntBridge.c`（`tools/build_antbridge.ps1` 会做这一步）；
2. 重新编译并链接 —— 就是 `pwsh -File tools\build_antbridge.ps1 -Force`
   （内部 `-shared -o antbridge.dll` + `-DAGB_BUILD_DLL`，见 `AGENTS.md`）；
3. 把新 `antbridge.dll` / `antbridge.dll.a` 拷回
   `3rdparty/ant/{bin,lib}` 和 `build-zig-native\`、`host\nativerun\`；
4. `cmake --build build-zig-native` 重新链接 libagave。

### 需要改的点

1. **桥**：`AGB_TRAMPOLINE_COUNT` 从 256 提到 **1024**（已完成）。
   每个引擎注册 ~49 个方法，1024 够 ~20 个引擎。
2. **桥**：新增 `agb_context_activate` 钩子（一个函数指针，dispatch 开头调用）。
3. **AntRuntime**：
   * `start()` 改为 `createEngine()`，返回 state 句柄；
   * `bindDocument(doc)` / `evaluate()` 增加 state 参数；
   * `g_*` 全局保留，但只在 `activate(state)` 里写；
   * listener 表、XHR job 列表、`g_nodeClass` 移入 state。
4. **KJSProxy**：`m_frame` → `Page` → `MaCrossView` → state；
   在 `evaluate()` 前 `AntRuntime::activate(state)`。
   （Frame 到 view 的映射：`WebEngine` 里已有 `_mc_view*`，可在
   `macross_view_create` 时把 state 指针存进 `WebView`。）
5. **WebEngine**：`macross_view_create` 创建自己的引擎；
   `macross_view_destroy` 销毁。
6. **销毁顺序**：先销毁 JS 引擎再销毁 Frame，否则回调会摸到野指针。

### 风险

* **重入**：一个引擎的回调里触发另一个引擎（例如 `window.open`）。
  需要在 `activate` 里保存/恢复 `g_current`（栈式）。
* **内存**：每个引擎有独立 VM（Ant 的堆按引擎分配），
  10 个 view 会明显吃内存；需要实测单引擎基线。
* **Ant 的全局状态**：若 `antbridge` 内部还有跨引擎的静态状态
  （如 `g_last_call`、错误缓冲），需要确认它们是否线程安全/可重入。
* **销毁**：现有代码在 `stop()` 里只销毁一个引擎，改成析构每个 state。

## 方案 B（不推荐）：每个 view 一个进程

隔离最彻底，但控件要跨进程传位图和输入，代价远高于方案 A。

## 验收

```
控件 A: window.foo = 1; document.title = 'A';
控件 B: typeof window.foo === 'undefined' && document.title !== 'A'
```

两个 1.5s 定时器各自看到自己的 document（现有 ctxA/ctxB 用例可复用），
再加一条 `window.foo` 断言。

## 工作量

| 步骤 | 预计 |
|---|---|
| 桥钩子 + trampoline 扩容 | 0.5 天（扩容已完成） |
| AntRuntime 状态化（250+ 处引用） | 2–3 天 |
| KJSProxy / WebEngine 接线 | 0.5 天 |
| 销毁顺序、重入、内存实测 | 1 天 |
| 回归（163/百度/本地 20+ 页面） | 0.5 天 |

## 现有可用的替代做法

在方案 A 落地前，嵌入方可以：
* 一个进程只跑一个"活跃"控件（其余控件延迟加载）；
* 或不依赖 `window` 全局传值，改用 `agavePostMessage` / `localStorage`。
