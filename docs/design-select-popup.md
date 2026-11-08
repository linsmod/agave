# 设计记录：`<select>` 下拉菜单画在 view 自己的 surface 上（不是现状文档）

> **这是设计记录，冻结在 2026-11-08 决策时。** 当前行为以
> [arch-rendering.md](arch-rendering.md) / [arch-embedding.md](arch-embedding.md) 为准；
> 根因与验证见 [hist-lessons.md](hist-lessons.md) 的 R133。

## 问题

`<select>`（menu list，`size=1`）点击后什么都不发生。链路本身是移植过来的：
`HTMLSelectElement::menuListDefaultEventHandler()`（mousedown）→ `RenderMenuList::showPopup()`
→ `PopupMenu::show()` → **`if (cb_create_popup_menu)`** —— 而 agave 的宿主
（`src/webview_control/AgaveWebView.c` 的 `agwInstallCallbacks()`）**从来没有注册这个回调**
（只有老 MaCross 示例 `test/win32`、`examples/touchweb` 注册过），于是 `show()` 整个函数体被跳过。
更糟的是 `RenderMenuList::showPopup()` 在调用前就把 `m_popupIsVisible = true`，之后每次点击都走
`hidePopup()` 只把标志清掉 —— 表现为"点一下没反应，再点也没反应"。

同时 `<select>` 在 JS 侧也是空壳：`value` / `selectedIndex` / `options` / `length` 全是
`undefined`（`AntRuntime.cpp` 的 `nodeGetter` 只认 `input`/`textarea`），所以即使菜单能开，
页面（css3test 的 `filter.js`：`evt.target.value`）也拿不到结果。

## 两条候选路线

| | 原生 popup 窗口（MaCross 老路） | **引擎内 overlay（选定）** |
|---|---|---|
| 做法 | 宿主注册 `cb_create_popup_menu`，建 `WS_POPUP` 窗口 + DIB，把 `WM_PAINT`/鼠标/键盘转发给 `macross_menu_*` | 引擎自己持有 `_mc_popup_menu`（无窗口），`PopupMenu` 复用原有绘制/命中代码；`WebView` 把菜单画在自己的 surface 上并优先接收输入 |
| 线程 | popup 窗口在 UI 线程，绘制要在引擎线程 → 需要新的跨线程命令（`AGW_CMD_MENU_*`） | 全在引擎线程，零新增线程/窗口 |
| 可测性 | 无头探针抓不到（第二个窗口不在 mirror 缓冲里），只能靠事件日志 | `--probe --shot` 直接截到打开的菜单 → 可做像素回归 |
| 参考实现 | `test/win32/popmenu.cpp`、`examples/touchweb/src/popmenu.cpp` | 本次新增 |

选 overlay 的决定性理由：agave 的验收手段是**探针 + 像素**，overlay 让"下拉打开了没有"变成一个
可断言的像素问题；而原生窗口方案在无头模式下永远看不见。

## 实现（当前代码锚点）

1. **`PopupMenu` 两种模式共存**（`src/webcore/platform/PopupMenu.h`、
   `src/webview/PopupMenuMC.cpp`）：
   - 宿主注册了 `cb_create_popup_menu` → 老路径（外部窗口）不变；
   - 没注册 → `createOwnPopup(FrameView*)`：`new _mc_popup_menu`，`handle = this`，
     `show/hide/move/capture/dirty/update` 由引擎自己的静态函数实现。`dirty` 把 popup 局部矩形
     加上 `windowRect()` 的偏移后交给 `WebView::updateRect()`，`update` 调 `updateViewNow()`。
   - `PopupMenu::show()` 里如果两种模式都不可用，调用 `client()->hidePopup()` 复位
     `m_popupIsVisible`（否则 `<select>` 永久"打开"）。
   - 鼠标/键盘逻辑从 `WebEngine.cpp` 的 `macross_menu_*` **移进**
     `PopupMenu::handleMouseEvent()` / `handleKeyEvent()`；`macross_menu_*` 只剩转发，
     所以宿主窗口路径与 overlay 路径不会再漂移。
2. **`WebView` 负责合成与输入路由**（`src/webview/WebView.cpp`）：
   - `setActivePopup()/activePopup()`；`paintView()` 在页面画完后，把 `windowRect()` 区域填白、
     `translate` 到菜单原点、`popup->paint(clientRect(), &gc)`，再描一圈 1px 灰边；
   - `mouseDown/Move/Up` 在菜单打开时把视口坐标减去 `windowRect()` 后交给 `handleMouseEvent()`
     并吞掉事件（点菜单外 = 关闭且不落到页面，与 Chrome 一致）；
   - `keyDown/keyUp/textInput` 在菜单打开时全部交给菜单（`{up}{down}{home}{end}{pageup}{pagedown}`
     移动高亮，`{enter}` 选中并派发 `change`，`{esc}` 关闭）；滚轮关闭菜单后照常滚页面；
     `killFocus()` 关闭菜单（等价于老宿主窗口的 `WM_KILLFOCUS`）。
   - 菜单的坐标来自 `PopupMenu::calculatePositionAndSize()`：`contentsToWindow(select 绝对矩形)`
     就是 view 客户端坐标，与 `paintView()` 的坐标系一致。
3. **ant 后端的 DOM 属性**（`src/javascript/ant_backend/AntRuntime.cpp` 的 `nodeGetter`/`nodeSetter`）：
   - `select.value` / `selectedIndex` / `options`（真数组）/ `length` / `type`（`select-one`/`select-multiple`）；
   - `option.value` / `text` / `index` / `selected`；
   - 写 `selectedIndex` 走 `setSelectedIndex(i, true, true)`（派发 `change`），写 `value` 不派发
     （DOM 语义），写 `option.selected` 走 `setSelected()`。
4. **探针新增 `--click x,y`**（`examples/browser_shell/main.c`）：向控件窗口
   `PostMessage` `WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP`，和用户点击走同一条
   WndProc → 引擎命令路径；可重复给多次（打开菜单 + 点某一项）。

## 已知限制（还没做）

- 没有 type-ahead（菜单打开时键入字母不跳转），`textInput` 直接吞掉；
- 滚轮不滚动菜单列表（关闭菜单、滚页面，和 Chrome 的"菜单里滚动 = 滚列表"不同）；
- 菜单永远向下展开：选择框贴近视口底部时菜单会被视口裁掉（老 MaCross 也没有翻转逻辑）；
- `size>1` / `multiple` 的 `<select>` 走 `RenderListBox`（不是 popup），本次未回归；
- 菜单不跟随页面滚动：滚动时菜单留在原处（滚轮会先关闭它，所以只在程序化滚动时可见）；
- 阴影/圆角等外观细节与 Chrome 不同（白底 + 1px 灰边）。

## 验收（回归）

- `tools/pages/form_select.html` + `run_layout_tests.ps1` 两组：
  像素组断言菜单真的画出来了（首项高亮 `#9db7cf`、边框 `#a0a0a0`、菜单外仍白）；
  行为组用 `--click`/`{down}{enter}` 两条路各选一次，断言
  `value` / `selectedIndex` / `length` / `options[2].text` / `type` / `change` 日志。
- 真页：`https://css3test.com/?filter=css2.2` 点开 Filter → 选 Stable →
  `location.search=?filter=stable`、`localStorage.filter=stable`。
