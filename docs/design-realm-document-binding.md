# realm 级 document 绑定：从“环境文档”改成“按对象解析”（设计）

> 状态：**已实施（B0–B3 完成，B4 除 `AGB_PER_FRAME_REALM=0` 取舍外完成）**，见
> [plan-realm-document-binding.md](plan-realm-document-binding.md) 与
> [hist-lessons.md](hist-lessons.md) R132。它是 [design-per-frame-realm.md](design-per-frame-realm.md)
> S3 的收尾部分；当前实现见 [arch-overview.md](arch-overview.md) §3。

## 1. 为什么还要再做一层

R130 修好了“子 frame 的 document 对象被父文档覆盖”（写坏），二维码因此能画。但紧接着发现
子 frame 里 `location.href` / `document.location.href` 仍然是**父页**的 URL，只有
`document.URL` 是子页的：

```
w.eval('document.URL')        -> https://open.weixin.qq.com/connect/qrconnect?...stylelite=1...
w.eval('location.href')       -> https://chat.deepseek.com/sign_in
w.eval('document.location.href') -> https://chat.deepseek.com/sign_in
```

wx 页用 `window.location.href` 解析 `stylelite=1`；读不到就回退 `E=NaN`，不显示 2024 版
160px 新 UI，而是保留 280px 旧 UI → 184px 宽的 iframe 内容溢出 → 滚动条（Chrome 无）。

这不是 `location` 一个 getter 的问题。根因是同一类：**`g_document` 是 per-view 的“当前
文档”环境变量，而很多绑定把它当成 `self` 所属文档**。`AntRuntime.cpp` 里 `g_document`
有 **105 处**引用，其中约三分之一是 getter/setter/DOM 方法。只要回调进入时环境文档与
`self` 不属于同一 realm，就会静默读错；每发现一个就补一个 getter 只会继续冒同类 bug。

## 2. 不变量

1. **每个 realm 恰好拥有一个 WindowState**：该文档 + 该文档的 window 级对象（document /
   location / history / navigator / screen / performance / storage）。
2. **DOM getter/setter/method 的文档来自 `self`**：document 对象 / window 代理 / node
   wrapper 的 native tag，或 `node->document()`；不读 `g_document`。
3. **`g_document` 只允许出现在入口/宿主代码**：bindDocument、stopRealm、导航、脚本求值、
   事件派发入口。进入 realm 时由该 realm 的 binding 设置，回调期间只是镜像。
4. **跨 realm 访问读目标对象**：`parent.location` 读父 realm 的 location 对象，
   `contentWindow.document` 读子 realm 的 document 对象，与调用方在哪个 realm 无关。
5. **document 级状态按 realm 隔离**：history、listeners、deferred script/module、unload、
   beforeunload、timer 上下文都属于 realm；一个 frame 导航不碰另一个 frame。

## 3. 目标结构

```
Engine (one VM)
 ├─ RealmBinding[realm]            (main realm + every frame document)
 │   ├─ Document* document
 │   ├─ AntbValue* documentObject
 │   ├─ AntbValue* global          (window === self === realm global)
 │   ├─ AntbValue* location        (native tag LOC1 -> document)
 │   ├─ AntbValue* history         (native tag HIS1 -> realm)
 │   ├─ AntbValue* navigator / screen / performance / storage
 │   ├─ SessionHistory entries/index
 │   ├─ Listener records           (each carries realm)
 │   └─ Deferred scripts/modules
 └─ Bridge funnel: enter the callee's realm (or self's realm) and mirror its
    binding into g_document / g_docObj before a native callback runs.
```

关键点：

- **对象自带文档**。`antb_set_native(obj, document, AGB_TAG_LOCATION)` /
  `(obj, realm, AGB_TAG_HISTORY)`；getter 先看 tag，不再依赖 `g_document`。
- **`documentOf(self)` 严格化**：`self` 有 NODE/WINDOW/LOCATION/HISTORY tag 就返回其
  document；非文档 receiver 才回退到当前 binding，并打 `[realm-mismatch]`。
- **桥回调上下文**。现在 `contextActivate()` 用 `antb_realm_current()` 反推 realm；
  如果 ant 对 native getter 不保证“当前 realm = 函数所属 realm”，就在桥里显式带 realm
  （ant `func->realm` 或新增 hook），不要把推断当契约。
- **slot 只做 debug 入口**。R130 已把 `__agbFrameSlot` 改成 realm 编号；它不再承担
  “切 document”的语义，切 realm 就是切 binding。

## 4. 需要改的地方（按风险排序）

### A. getter / setter（必须 self 化）

| 函数 | 现在读的环境 | 应改为 |
|---|---|---|
| `locationGetter` / `locationSetter` / `locationAssign` / `locationReplace` / `locationReload` / `locationToString` | `g_document` | self 的 LOC1 tag |
| `navigateTo()` | `g_document` | 调用方传入 document |
| `titleSetter` | `g_document` | `documentOf(self)` |
| `docWrite` / `docWriteln` | `g_document` | `documentOf(self)` |
| `imageConstructor`（`new Image()`） | `g_document` | 当前 realm 的 document |
| `globalViewportSize` | `g_document` | 传入的 document（prelude 已传 `g.document`） |
| `domainMatches`（cookie） | `g_document` | 发起 cookie 读写的 document |
| `nodeGetter("defaultView")` | 当前 global | 该 node 的 document 的 realm global |
| `cssomWrap` / `wrapNode` / `mediaWrapObject` 的 owner | `g_document` | 节点的 document |

### B. 每 realm 状态（不是 getter，但同样按 view 共享）

- `sessionHistory*` / `historyPushState` / `historyGo`：现在在 `RuntimeState`（per view），
  iframe 里的 `history.length`/`pushState` 会动主 frame 的历史。
- `dispatchWindowEventState` / `dispatchResizeEvent` / `beforeUnloadGuard` /
  `documentWillUnload`：按 `g_document` 派发；iframe 的事件应进自己的 realm。
- listener 表还是 per view；listener 记录应带 realm，派发前 enter 它。
- deferred module / deferred script 队列：归 realm，frame 导航时按 realm 清。

### C. 允许保留 `g_document` 的入口

`bindDocument` / `stopRealm` / `activateState` / `contextActivate` / `noteMainBinding` /
`currentRealmSlotNumber`、`evaluate` / `queueDeferredScript` / `queueModule`、
`globalOpenWindow` / `globalCloseWindow` / `globalDownloadOpen`、`loadCookies` /
`isInternalDocument`。这些是“宿主在操作当前 view”，不是“JS 对象在问自己的文档”。

## 5. 迁移批次（每批独立可验收）

### B0 护栏 + 用例（先做，1 天内）

- 新增 `AGB_REALM_CHECK=1`：在 `documentOf` 和每个 binding 入口检查
  `self` 的 document 是否等于当前 binding；不一致打一行
  `[realm-mismatch] fn=... self=... current=...`（每站点一次）。
- 新增 `tools/pages/cap_realm_cross.html`（父 + 子，同源）：
  - 子读 `location.href/search`、`document.location`、`parent.location.href`；
  - 父读 `f.contentWindow.location.href`、`f.contentDocument.URL`；
  - `document.location === window.location`；
  - `new Image().ownerDocument === document`；
  - 子 `history.pushState` 后 `history.length` 只增子 frame 的；
  - 子改 `document.title` 父标题不变。
- 接进 `tools/run_realm_tests.ps1`：per-frame 与 `AGB_PER_FRAME_REALM=0` 两种模式都跑。

### B1 location / history 对象化（修当前滚动条）

- `AGB_TAG_LOCATION` + 每 document 一个 location 对象（`window.location` 与
  `document.location` 同一对象），getter 从 tag 取 doc。
- `AGB_TAG_HISTORY` + session history 挂到 realm，`history.length/pushState/back` 只动本 realm。
- 验收：wx 页 `stylelite=1` 被读到 → `#tpl_iframe` 显示、QR 160×160、iframe
  `clientWidth == innerWidth`（无滚动条）；与 Chrome 截图/几何对拍。
- 预期编号：R132。

### B2 getter / method self 化

- 按 §4-A 逐项改；`documentOf(NULL)` 只保留 `createHTMLDocument` 的显式传参。
- 每改一项打开 `AGB_REALM_CHECK=1` 跑 cap_realm_cross + deepseek/baidu。

### B3 事件 / 监听 / 队列按 realm

- listener 记录带 realm；dispatch 前 enter；`beforeunload`/`unload` 按 realm 清。
- iframe 的 `history`、deferred modules/scripts、timer 上下文完全独立。

### B4 强制与收尾

- CI/常规矩阵里固定跑 `AGB_REALM_CHECK=1`；零 `[realm-mismatch]` 才算过。
- 删掉 per-view 的 `g_locationObj`、window cache 旧回退和 `documentOf` 的 ambient 回退。
- 评估是否还需要 `AGB_PER_FRAME_REALM=0`（一个发布周期后再决定）。

## 6. 验收基线

- `location.href/search` 在子 frame = 子 URL（同步、定时器、父通过 contentWindow 读三种路径）。
- `document.location === window.location`；`parent.location.href` 从子读 = 父 URL。
- `new Image().ownerDocument === document`；`document.title` 只改本 frame。
- 子 frame `history.length` 与 pushState 不影响主 frame。
- 真站：wx 页选 2024 版 UI（QR 160×160，iframe 无滚动条），Chrome 对照一致。
- `AGB_REALM_CHECK=1` 下 `run_realm_tests`、`run_nav_tests`、`run_script_mode_tests`、
  `run_xhr_tests` 全绿，且 deepseek / baidu smoke 无 mismatch。

## 7. 备选与不做

- **每 frame 一个 engine**：跨 frame 引用要序列化，`parent.fn()` 全废，代价高一个量级（见
  design-per-frame-realm §7）。不做。
- **继续逐 getter 打补丁**：能救单个站点，但 105 处 `g_document` 会持续冒同类问题。只作为
  B1 的临时手段，不作为长期方案。
- **把 `g_document` 直接删掉**：先完成 B1–B3 再评估；入口/宿主代码仍需要一个“当前 view 的
  文档”概念，可以保留为 funnel 设置的镜像。
