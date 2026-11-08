# realm document 绑定：实施计划与断点续做（B0–B4）

> 状态：**实施计划，进行中**。设计见
> [design-realm-document-binding.md](design-realm-document-binding.md)；
> 背景缺陷见 [hist-lessons.md](hist-lessons.md) R130。
> 这份文档是**断点续做**用的：每批完成后勾掉对应项并写一行结果。
>
> 最后更新：2026-11-08（B0 尚未开始）

## 0. 当前事实（不要重新推导）

- R130 已修：子 frame 的 document 对象不再被父文档覆盖；二维码能画。
- 仍未修：子 frame 里 `location.href` / `document.location.href` 是**父页** URL。
  证据（真站 `chat.deepseek.com/sign_in`）：
  ```
  w.eval('document.URL')           -> open.weixin.qq.com/connect/qrconnect?...stylelite=1...
  w.eval('location.href')          -> https://chat.deepseek.com/sign_in
  w.eval('document.location.href') -> https://chat.deepseek.com/sign_in
  ```
  后果：wx 页 `t("stylelite")` 读不到 1，不显示 160px 新 UI，保留 280px 旧 UI，
  184px 视口出现滚动条（Chrome 是 160px 新 UI、无滚动条）。
- `AntRuntime.cpp` 现有 **105 处** `g_document`；分类见设计文档 §4。
- 已提交：`c50d022`（R130）、`932abc5`（设计文档）。
- 构建/运行前置：改 C++ 后
  `pwsh -File tools\build_project.ps1 *> build.log; $LASTEXITCODE`；
  重编前先 `Stop-Process -Name agave_shell`（铁律 3）。
  `prelude.js`/`polyfills.js` 是运行时加载，改注释不用重链。

## 1. 文件拆分（本次新增代码不再往 AntRuntime 里堆）

AntRuntime.cpp 约 1.3 万行。本次新增/重写的 **location + history + realm 检查**
放进独立模块；AntRuntime 只保留注册/胶水。

```
src/javascript/ant_backend/
  RealmTags.h            新增  ~25 行   native tag 常量（NODE/WINDOW/LOCATION/HISTORY）
  RealmDocument.h        新增  ~45 行   对外 API
  RealmDocument.cpp      新增  ~500 行  location 对象 + history 对象 + 每 frame 历史
                                        + documentOfReceiver + [realm-mismatch] 检查
  AntRuntime.cpp         改           删除 location*/sessionHistory* 实现，改调用点；
                                       listener 加 realm 字段与按 document 清理（保持原位）
```

构建：`src/src.cmake` 的 `if (OPT_JS_ANT)` 块里，`target_sources(${LIB_NAME} PRIVATE ...)`
在 `AntRuntime.cpp` 后面加 `RealmDocument.cpp`。KJS 的 GLOB 虽然能扫到，但显式列出更稳
（该文件注释已说明“新 .cpp 可能被静默漏掉”）。

后续（B4 之后，若 AntRuntime 仍过大）再拆：`RealmBinding.cpp`（slot/binding）、
`ListenerStore.cpp`、`DomBindings.cpp`。本轮不做，避免一次改太多。

## 2. RealmDocument API（先定契约）

```cpp
// RealmDocument.h
namespace AntRuntime { namespace RealmDocument {

// location: 每个 document 一个对象；per-frame 模式下 getter 用对象自带的 doc。
AntbValue* locationObject(AntbEngine*, WebCore::Document*);
int        setLocation(AntbValue* self, const AntbValue* value);   // window.location = ...

// history: 对象按 document 缓存，底层 FrameHistory 按 Frame* 共享。
AntbValue* historyObject(AntbEngine*, WebCore::Document*);
void       noteDocument(AntbEngine*, WebCore::Document*);          // 新主文档入历史
void       releaseEngine(AntbEngine*);                             // stopRealm/destroyContext
void       setPopstateDispatcher(void (*)(const char*, AntbValue*));

// 从 receiver 取文档：LOCATION/HISTORY tag -> 记录；NODE/WINDOW -> native doc。
WebCore::Document* documentOfReceiver(AntbValue* self);

// AGB_REALM_CHECK=1 时，写入类 API 的 self 文档 != 当前 binding 就打一行
// [realm-mismatch] where=... self=... current=...
void checkMismatch(const char* where, WebCore::Document* selfDoc);

}}
```

per-frame 模式开关：`perFrameRealmEnabled()` 在 AntRuntime 内部；初始化时调用
`RealmDocument::setPerFrameRealms(bool)`（或由 `locationObject`/`historyObject`
多传一个 bool）。共享模式（`AGB_PER_FRAME_REALM=0`）保留旧行为：location 仍跟随
当前绑定文档，避免破坏 A/B 基线。

## 3. 批次与步骤

### B0 护栏 + fixture（先做，能独立验收）

- [ ] `RealmTags.h`：把 `AGB_TAG_NODE` / `AGB_TAG_WINDOW` 从 AntRuntime 挪进来；
      新增 `AGB_TAG_LOCATION ('LOC1')`、`AGB_TAG_HISTORY ('HIS1')`。
- [ ] `RealmDocument.cpp` 先落 `checkMismatch` + `documentOfReceiver` 骨架。
- [ ] `AGB_REALM_CHECK=1`：在 `titleSetter`、`docWrite`/`docWriteln`、
      `historyPushState`、location 写入路径调用 `checkMismatch`（先只覆盖写入，
      避免跨 realm 读取误报）。
- [ ] 新增 `tools/pages/cap_realm_cross.html` + `cap_realm_cross_child.html`：
      子页记录 `location.href/search`、`document.location.href`、
      `(new Image()).ownerDocument === document`、`history.length`；
      父页读 `f.contentWindow.location.href`、`f.contentDocument.URL`、
      `parent.location.href`（在子页侧读）、`document.title` 隔离、
      子 `history.pushState` 后父子 `history.length` 各自不变。
- [ ] 接进 `tools/run_realm_tests.ps1`：per-frame 必须全过；共享模式至少不退化
      （location 身份类断言只在 per-frame 下要求，先注释说明）。
- 验收：`pwsh -File tools/run_realm_tests.ps1` → `ALL REALM CHECKS PASSED`；
  `AGB_REALM_CHECK=1` 下无 `[realm-mismatch]`（真站 + fixture）。

### B1 location / history 对象化（修滚动条，预期 R131）

- [ ] location：
  - `locationObjectFor`（每 document 缓存）在 `RealmDocument.cpp` 实现：
    href getter/setter、assign/replace/reload/toString/valueOf；
    `antb_set_native(obj, doc, AGB_TAG_LOCATION)`（per-frame 模式）。
  - 删除 AntRuntime 的 `g_locationObj`、`locationGetter/Setter/Assign/Replace/
    Reload/ToString` 与 `navigateTo`（迁到 RealmDocument）。
  - `installGlobalApi` 的 location 块改 `RealmDocument::locationObject(g_engine, g_document)`。
  - `titleGetter` 的 `"location"` 分支改 `RealmDocument::locationObject(engine, d)`。
  - `windowSetter` 的 `location/href` 改 `RealmDocument::setLocation(self, value)`。
  - `document.location === window.location` 必须成立（同一缓存对象）。
- [ ] history：
  - `FrameHistory { Frame*; vector<Entry>; int index; }`，按 Frame* 缓存；
    `Entry { url, state(retained), document }`。
  - `historyObjectFor` 每 document 一个 JS 对象（native = FrameHistory*）；
    `history.length/state`、`pushState/replaceState`、`back/forward/go`
    全部从 self 的 HISTORY tag 取 FrameHistory。
  - 迁移 `sessionHistoryNoteDocument` / `Clear` / `Reset` / `ApplyURL` / `Go`：
    主文档入历史逻辑保持原样，只是存到 main frame 的 FrameHistory。
  - popstate 用 `setPopstateDispatcher(dispatchWindowEventState)` 回调。
  - `installGlobalApi` 的 history 块改 `RealmDocument::historyObject(...)`。
  - `stopRealm` 调 `RealmDocument::releaseEngine(engine)`。
- 验收：
  - fixture：子 `location.search` = `?x=1`；`parent.location.href` = 父 URL；
    `document.location === window.location`；子 `history.pushState` 后子 length+1、
    父不变。
  - 真站：wx 页 `#tpl_iframe` display:block、`#tpl_old_iframe` none，
    可见 QR 160×160，iframe `clientWidth == innerWidth`（无滚动条）；
    截图与 Chrome 对照（Chrome 命令见 §5）。
  - `run_realm_tests` / `run_nav_tests` / `run_script_mode_tests` / `run_xhr_tests` 全绿。

### B2 getter / method self 化

- [ ] `titleSetter`：`title` 用 `documentOf(self)`；`cookie` 把 doc 传给
      `setCookieForCurrentDocument(doc, text)`（改签名）。
- [ ] `docWrite` / `docWriteln`：`documentOf(self)`。
- [ ] `imageConstructor`：用当前 realm 的 document（`g_document` 在入口已同步，
      加 `checkMismatch`）。
- [ ] `globalViewportSize`：优先用 `args[0]` 的 document（prelude 已传 `g.document`）。
- [ ] `domainMatches`：cookie 安全判断用调用方传入的 document。
- [ ] `nodeGetter("defaultView")`：返回 `n->document()` 所属 realm 的 global，
      不是当前 global。
- [ ] `wrapNode` / `cssomWrap` / `mediaWrapObject` 的 owner：用节点的 document。
- [ ] `docCreateHTMLDocument`：保留 `documentOf(NULL)` 作为 implementation 对象的
      显式 fallback，并在注释里写明原因。
- 验收：`AGB_REALM_CHECK=1` 下 fixture + deepseek/baidu 无 mismatch；
  `cap_realm_cross` 的 `new Image().ownerDocument`、`document.title` 全过。

### B3 事件 / 监听 / 队列按 realm

- [ ] `Listener` 加 `AntbRealm* realm`；`nodeAddEventListener` /
      `setNodeEventHandler` 记录 `currentRealm()`。
- [ ] `runListenersFor`：调用前 `antb_realm_enter(l.realm)` + `applyRealmBinding(l.realm)`，
      调用后恢复 previous realm + binding（同一 listener 列表可跨 realm）。
- [ ] `dropListenersForDocument(doc)`：只摘掉该 document 的 listener；
      `documentWillUnload` 用它（替换 blanket clear），`frameDocumentWillUnload`
      在 `forgetFrameRealm` 前调用。
- [ ] `frameDocumentWillUnload` 的 pagehide/unload 已在子 realm，确认 unload 后
      listener/定时器不再引用该 realm。
- 验收：fixture 里父/子各注册 click/自定义事件，互不串；子 frame 卸载后父的
  listener 仍在；`run_typing_tests` / `run_worker_tests` 不退化。

### B4 强制与收尾

- [ ] `AGB_REALM_CHECK=1` 加进 `run_realm_tests.ps1` 的两条路径。
- [ ] 删除 per-view `g_locationObj`、`sessionHistory` 字段（`RuntimeState` 只剩
      engine/mainRealm/currentSlot 之外的通用字段）。
- [ ] 更新 `docs/arch-status.md`（iframe location/history）、
      `docs/arch-capability-gaps.md` §6.5（滚动条已修）、`docs/dev-debugging.md`
      （AGB_REALM_CHECK）、`docs/hist-lessons.md` 追加 R131。
- [ ] 评估 `AGB_PER_FRAME_REALM=0`：保留一个周期，文档标“仅 A/B”。
- 验收：全套 `run_realm_tests`、`run_nav_tests`、`run_script_mode_tests`、
  `run_xhr_tests`、`run_jit_loop_tests`、真站 10 次二维码 + 无滚动条。

## 4. 断点续做协议

每次开工：

```powershell
Get-Process agave_shell | Stop-Process -Force
pwsh -File tools\build_project.ps1 *> build.log; $LASTEXITCODE
pwsh -File tools\run_realm_tests.ps1 *> build-zig-native\realm.log; $LASTEXITCODE
```

真站验证（无头，不弹窗）：

```powershell
# 二维码 + 模板/滚动条/位置（build-zig-native\qr_tpl.js 已存在）
agave_shell.exe --probe https://chat.deepseek.com/sign_in --wait 9000 --settle 300 `
  --timeout 60000 --eval-file build-zig-native\qr_tpl.js
# 期望：oldTpl display=none; newTpl display=block; visibleImgs 160x160;
#       deClient[0]==200（clientWidth == innerWidth，无滚动条）
```

Chrome 对照（不依赖 websockets）：

```powershell
& 'C:\Program Files\Google\Chrome\Application\chrome.exe' --headless=new --disable-gpu `
  --user-data-dir=build-zig-native\chrome-profile --window-size=1100,800 `
  --virtual-time-budget=12000 --screenshot=build-zig-native\chrome_ds.png `
  https://chat.deepseek.com/sign_in
```

判断滚动条：读子 frame `documentElement.clientWidth == innerWidth` 且
`scrollWidth == clientWidth`（fixture 直接断言）。

## 5. 进度记录（每批更新）

| 日期 | 批次 | 结果 | 提交 |
|---|---|---|---|
| 2026-11-08 | 设计 / R130 | 已完成 | `c50d022` / `932abc5` |
|  | B0 |  |  |
|  | B1 |  |  |
|  | B2 |  |  |
|  | B3 |  |  |
|  | B4 |  |  |

## 6. 风险 / 待定

- **共享模式**（`AGB_PER_FRAME_REALM=0`）：location 继续跟当前绑定文档；跨 realm
  身份类断言只在 per-frame 下要求，避免为了 fallback 把新模型复杂化。
- **history 跨文档**：主 frame 继续用现有 FrameLoader back/forward；子 frame
  只保证 `length/state/pushState/replaceState` 独立，跨文档 back 仍是 no-op
  （记录到 capability gaps，不假装完整）。
- **popstate 的 realm**：`dispatchWindowEventState` 需要保证在目标 frame 的 realm
  里派发；B1 先在 main frame 验证，B3 再补子 frame。
- **性能**：`contextActivate` 每个桥回调多一次 `antb_realm_current` + 比较；目前
  只在不同 realm 时做 binding 切换，先接受，若 profile 显示热点再优化为缓存。
- **CMake**：新增 .cpp 后如果 `build_project.ps1` 没重新 configure，先跑一次
  `cmake -S . -B build-zig-native`（脚本通常会自动 configure；否则链接期会报
  未定义符号）。
