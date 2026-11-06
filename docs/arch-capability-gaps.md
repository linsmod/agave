# 能力缺口：JS/DOM 与 CSS 支持清单（实测）

> 这篇只回答一件事：**哪些 Web 平台能力在 agave 里没有、或只实现了一半**，以及怎么用一条命令复测。
> 已修的历史缺陷在 [hist-lessons.md](hist-lessons.md)；当前待办与优先级在 [plan-roadmap.md](plan-roadmap.md)；
> 探针/日志用法在 [dev-debugging.md](dev-debugging.md)。本页不重复这些内容。
>
> 基线：2026-11-04，`build-zig-native`（native 网络后端）。下表「当前值」都是探针实测；
> 修掉一项后**同时更新本页和对应 `cap_*` 页面**。

## 0. 怎么复测

探测页都在 `tools/pages/`（被跟踪），页面把结果写进 `document.title`，所以命令统一是
`--eval "probeExit(document.title)"`。在仓库根执行：

```powershell
build-zig-native\agave_shell.exe --probe tools\pages\cap_dom.html     --wait 500 --eval "probeExit(document.title)"
build-zig-native\agave_shell.exe --probe tools\pages\cap_css.html     --wait 500 --eval "probeExit(document.title)"
build-zig-native\agave_shell.exe --probe tools\pages\cap_history.html --wait 500 --eval "probeExit(document.title)"
build-zig-native\agave_shell.exe --probe tools\pages\cap_storage_a.html --wait 900 --eval "probeExit(document.title)"
build-zig-native\agave_shell.exe --probe tools\pages\cap_script.html     --wait 1200 --eval "probeExit(document.title)"
build-zig-native\agave_shell.exe --probe tools\pages\cap_selectors.html  --wait 500 --eval "probeExit(document.title)"
build-zig-native\agave_shell.exe --probe tools\pages\cap_postmessage.html --wait 800 --eval "probeExit(document.title)"
# frame 的 window 身份：结果打在 [child] 控制台行上（页面不写 title）
build-zig-native\agave_shell.exe --probe tools\pages\cap_window_identity.html --wait 900 --eval "probeExit(0)"
# [gap-jsdom] 自检：六个缺口各打一行，值不变（innerText=undefined submit=undefined complete=undefined
# forms=0 attr=0 state=undefined）
build-zig-native\agave_shell.exe --probe tools\pages\cap_gap_log.html --wait 500 --eval "probeExit(document.title)"
# 窗口 resize：改控件尺寸再量（布局/百分比跟随；vw/vh、@media 断点、resize 事件不跟随）
build-zig-native\agave_shell.exe --probe tools\pages\resize\resize_probe.html --resize 500x400 --eval-file tools\pages\resize\check_resize.js
```

退出码 0 且 `[exit]` 行就是当前值。要加一项探测：在 `cap_dom.html` / `cap_css.html`
的 `document.title = [...]` 里加一行，再在本页加一行表格。

运行时也有信号：`[gap-jsdom]` 通道默认开启，页面第一次访问某个已知缺口时打一行
（`name=div#app.innerText area=dom effect="undefined; textContent works"`）。名单是
`src/javascript/ant_backend/AgbGapJsdom.h`，与本页 §1 一一对应；`AGB_GAP_LOG=0` 关掉。

**这些是存量缺口，不是回归**：页面行为不对时先查本页，别先怀疑页面或最近的提交。

## 1. JS / DOM

`cap_dom.html` 当前输出（2026-11-04 补齐一批 DOM 缺口后，见 R34）：

```
forms=1 links=1 images=2 anchors=1 scripts=1 all=13
formSubmit=function formAction=string imgComplete=0 imgNatural=0 imgWidth=1
insertAdjacentHTML=function innerText=string classListAdd=function classListReplace=function
matches=function closest=function textContent=string
parentElement=object firstElementChild=object nextElementSibling=undefined
isConnected=number getRootNode=function getAttributeNames=function toggleAttribute=function
replaceChildren=function scrollIntoView=function offsetParent=undefined nodeValue=string
tabIndex=number hidden=number formElements=2 imgDecode=function iframeCw=yes
activeElement=object hasFocus=function visibilityState=string fonts=object
importNode=function elementFromPoint=function
```

`cap_selectors.html`（R35 修复后）：
`attr=bar ds=bar qsAttr=1 qsClass=1 qsClassAttr=1 qsTagAttr=1 qsExact=1 qsExactCase=1 qsBegin=1 qsEnd=1 qsList=1
qsChild=3 qsDesc=3 qsAdj=1 qsSib=2 qsNth=1 qsOdd=2 qsNot=2 qsFirst=1 qsList2=4 qsNone=0`
`cap_script.html`（R27 修复后）：`hits=11 child=loaded`
`cap_postmessage.html`（R31 后）：`postMessage=function sent=yes got=1`

| 能力 | 当前值 | 影响 | 判定 / 建议 |
|---|---|---|---|
| `document.forms` / `links` / `images` / `anchors` / `scripts` / `embeds` / `plugins` / `applets` | **已支持（R34）**：真实集合（`forms=1 images=2 scripts=1 links=1`）。注意是**快照数组**，不是 live collection | —— | 已支持 |
| `HTMLFormElement.submit()` / `.action` / `.elements` / `.reset()` | **已支持（R34）**：`formSubmit=function formAction=string formElements=2`；`checkValidity()` 恒返回 true（没有约束校验） | —— | 已支持（`checkValidity` 是桩） |
| `history.pushState` / `replaceState` / `state` / `length` / `popstate` | **已支持（R36）**：state 对象真的存下来了（`pushState` 后 `history.state` 是同一个值），`length` 随 entry 增长，`back/forward/go` 恢复 URL 并派发带 `state` 的 `popstate`，`pushState` 会截断 forward。跨文档的前进后退交给 FrameLoader（`[nav] request main type=back-forward`） | —— | 已支持（`cap_history.html`；限制：`length` 只数当前文档的 entry） |
| `localStorage` 跨文档 | A 页写入 → 导航到 B → `null`；同一跑的 cookie 正常（`cookie=true`） | 用 localStorage 存 token/购物车的多页流程会丢状态；测试也不能拿它当跨页标记（用 cookie） | 缺口，P1 |
| `window.postMessage` / `message` 事件 | **已支持**：同窗口/跨 frame 都能投递（`{data, origin, source}`，`origin` 取发送方文档的 scheme://host[:port]）；`cap_postmessage.html` → `postMessage=function sent=yes got=1` | —— | 已支持 |
| iframe 的 `contentWindow` / `contentDocument` | **已支持（R34）**：`iframeCw=yes`，`contentWindow.document === contentDocument` 成立；子文档还没建好时是 `null` | —— | 已支持 |
| 每个 frame 的 `window` 身份（`parent`/`top`/`self`/`frames`/`length`/`name`/`frameElement`） | **已支持**：每个 frame 文档有自己的 **realm**（默认开启，`AGB_PER_FRAME_REALM=0` 回退旧模型），`window`/`self` 就是该 realm 的全局对象，`parent !== self`、`parent.document` 是父 realm 的 document 对象、`top.Array !== Array`、`frames.length`/`name`/`frameElement` 按 frame tree 回答。旧模型（回退时）是 `Window` 代理 + 共享全局（R31） | `parent.location` 这类 getter 在子 realm 执行时仍读当前 `g_document`（S4 WindowProxy 未做） | 已支持（`design-per-frame-realm.md`） |
| 子 frame 的顶层 `var` 污染父页 | **已隔离**：每个 frame 一个 realm，子 frame 的 `var x = 1` 父页读不到；跨帧调用 `parent.fn()`、读 `parent.someVar` 仍可用（同 VM 跨 realm 引用，不序列化） | ES module map 仍按引擎共享（两个 frame import 同一模块会拿到同一实例，S3 待做） | 已支持（`run_realm_tests.ps1` → `childVarLeak=undefined`） |
| `HTMLImageElement.complete` / `naturalWidth` / `naturalHeight` / `width` / `height` / `decode()` | **已支持（R34）**：`imgComplete=0 imgNatural=0 imgWidth=1 imgDecode=function`（`decode()` 立即 resolve） | —— | 已支持 |
| `document.querySelector(All)` 的选择器引擎 | **已支持（R35）**：选择器列表、组合器（后代/`>`/`+`/`~`）、属性选择器六个操作符、常用伪类（`nth-child`/`not`/`first-child`/`checked`/…）。实现是 `src/javascript/ant_backend/AgbSelector.h`（WebCore 2008 没有 `Node::querySelectorAll`） | —— | 已支持（`cap_selectors.html`） |
| `Element.insertAdjacentHTML` / `insertAdjacentElement` / `insertAdjacentText` / `replaceChildren` | **已支持（R34）**：四个插入位置都走 `insertBefore/appendChild`，HTML 走 `createFragmentFromMarkup` | —— | 已支持 |
| `HTMLElement.innerText` / `outerText` | **已支持（R34）**：走 `Element::innerText()`（会触发布局） | —— | 已支持 |
| 元素树遍历 `parentElement` / `firstElementChild` / `lastElementChild` / `nextElementSibling` / `previousElementSibling` / `isConnected` / `getRootNode()` | **已支持（R34）** | —— | 已支持 |
| 反射属性 `hidden` / `dir` / `lang` / `tabIndex` / `contentEditable` / `draggable` / `translate` / `slot` / `part` / `spellcheck` / `autofocus` | **已支持（R34）**：读写都落到属性（`el.hidden = true` → `hasAttribute('hidden')`） | —— | 已支持 |
| `getAttributeNames()` / `toggleAttribute()` / `nodeValue` / `offsetParent` / `scrollIntoView()` / `classList.replace()` | **已支持（R34）**；⚠ `scrollIntoView()` 只是存在（R82 实测滚动前后 `getBoundingClientRect().top` 不变，**不滚动**） | —— | 部分 |
| 文档滚动：`documentElement.scrollTop` / `body.scrollTop` / `window.scrollTo/scrollBy` / `pageYOffset` / `scrollX/Y` | **已支持（R82）**：`scrollTop/Left` 在 html/body 上路由到 `FrameView::setContentsPos`，`getBoundingClientRect()` 返回视口坐标（滚动后为负值），`pageYOffset/scrollY` 是实时 getter | 修前这四条全是 no-op（`scrollTop` 读回 0，滚动后 rect 不变） | 已支持 |
| `document.activeElement` / `hasFocus()` / `visibilityState` / `importNode` / `adoptNode` / `elementFromPoint` | **已支持（R34）**；`visibilityState` 恒 `"visible"`，`activeElement` 无焦点时回落到 `body` | —— | 已支持 |
| `document.fonts` | **桩（R34）**：`{status:'loaded', ready:已 resolve, add/delete/check/forEach}`。没有 web font 加载，`ready` 立即兑现 | 用 `await document.fonts.ready` 的页面不再挂住；但 `fonts.load()` 不会真的加载字体 | 桩 |
| `window.open` 的引擎侧策略 | 可用，但**绕过** `FrameLoaderClientMC::dispatchCreatePage`，走控件 `agave_control_open_window` | 引擎级 new-window 策略/features 不生效；`[nav]` 里看到的是 `popup create`，不是 `popup open` | 管道差异，P2 |
| `el.onload = fn` 等事件处理属性 | **已修（R27）**：`hits=11`（属性 + addEventListener 都触发）；此前只有 `addEventListener` 生效，`onload` 是静默 expando | —— | 已支持 |
| HTML 属性事件 handler（`<body onresize="…">`、`onclick="…"`） | **会触发，但跑在遗留 KJS 世界**：handler 能读写 DOM（`document.documentElement.setAttribute(...)` 生效），但 `window.__x = 1` 页面脚本读不到（`undefined`）——两套全局。同一事件上 `addEventListener` 正常工作（R69 实测三种 resize 写法：前两条收到且能读页面全局，`<body onresize>` 也收到但看不到页面全局） | 依赖内联 handler 与页面 JS 交换状态的老站点 | **不推进**：现代站点用 `addEventListener`，`resize` 监听已支持。理由与将来的修法（`Document::createHTMLEventListener()` 改走 AntRuntime）见 [plan-roadmap.md](plan-roadmap.md)「P2 不推进」 |

### 1.1 连 shim 都没有（feature detect 会走 fallback）

`ResizeObserver`、`IntersectionObserver`、`PerformanceObserver`、`indexedDB`、`caches`、`Notification`、
`geolocation`、`FileReader`、`customElements`、`ShadowRoot`/`attachShadow`、`OffscreenCanvas`、
`Audio`/`AudioContext`/`speechSynthesis`、`devicePixelRatio`、`print`、`Temporal`、`WebGL`。

`WebSocket`/`EventSource`/`BroadcastChannel` 存在（ant 原生，走 libuv），但浏览器里没有对应的 socket
事件泵；构造不抛错、连接行为未验证，属于"探测通过但连不上"的静默失败。

定位锚点（已修项只给锚点，不再列"差什么"）：

- 集合 / 表单 / 图片 / 文本 / 反射属性 / 元素树 / `activeElement`：R34，`AntRuntime.cpp` 的
  `titleGetter` / `nodeGetter` / `g_nodeClass` / `documentObjectFor`。
- History state：R36，`AntRuntime.cpp` 的 `sessionHistory` / `historyPushState` / `historyBack/Forward`。
- 选择器：R35，`src/javascript/ant_backend/AgbSelector.h`。
- frame window / `contentWindow`：R31/R34，`windowObjectFor` / `documentObjectFor` /
  `HTMLFrameOwnerElement::contentDocument()`。
- storage：写入在文档 teardown 落盘、读取只在 engine 重建时发生（跨进程能读到旧值，同进程导航读不到）。
- `window.postMessage`：`prelude.js` 的 `g.postMessage` → `__agbFramePostMessage`；
  宿主桥 `agavePostMessage` 是页面→壳，两个方向不要混。
- 缩放图片：R29，`src/webcore/platform/picasso/ImagePS.cpp` 的 `draw_scaled_image()`。
- 事件处理属性：R27，`AntRuntime.cpp` 的 `nodeSetter` / `nodeGetter` `on*` 分支。

### 输入事件（`tools/pages/cap_input_events.html`）

一条命令复测（真实按键，不是合成事件）：

```powershell
agave_shell.exe --probe ../tools/pages/cap_input_events.html --keys "abc"     --eval "probeExit(window.reportInputEvents())" --wait 300
agave_shell.exe --probe ../tools/pages/cap_input_events.html     --pre-eval "document.getElementById('c').focus()" --keys "y"     --eval "probeExit(window.reportInputEvents())" --wait 300
```

| 能力 | 现状 | 备注 |
|---|---|---|
| `addEventListener` / `on*` 属性 / capture / bubble / `stopPropagation` / `removeEventListener` | **支持** | 16 种类型全通（`#a`–`#j` 用例） |
| `{once:true}` | **支持**（R45） | 引擎派发与 JS `dispatchEvent` 两条路径都会摘除 |
| `event.key` / `event.code` | **支持**（R45） | 由 `keyIdentifier()`+`charCode()`+shift 推出（`a`/`Enter`/`ArrowLeft`/`KeyA`/`Digit1`） |
| `event.isTrusted` | **支持**（R45） | 引擎派发 `true`，JS `dispatchEvent` 为 `false` |
| `beforeinput` + `data`/`inputType` + `preventDefault` 取消插入 | **支持**（R45） | 由 `textInput` 映射；`#c` 用例 `c[val="",prevented=1]` |
| `input` 事件的 `inputType`/`data` | **支持**（R45） | `inputType="insertText"`，`data` 取自同一次 `textInput` |
| `keydown` 的 `preventDefault` 拦字符 | **支持** | `#b` 用例 `b[val="",prevented=1]` |
| `select` / `selectionchange` | **不支持** | 键入不产生这两个事件 |
| `change` | 支持（失焦时） | 未在测试页覆盖（需要 blur 用例） |
| `paste`/`cut`/`copy` | 合成事件可派发 | 真实剪贴板路径未接（无 `ClipboardEvent`） |
| 事件顺序 | **`keydown > keyup > keypress > beforeinput > input`** | Chrome 是 `keydown > keypress > beforeinput > input > keyup`；宿主按键投递顺序所致 |
| `document.createEvent` / `new MouseEvent()` | **不支持** | `typeof document.createEvent === 'undefined'`（2026-11-06 实测），页面无法合成事件；`dispatchEvent` 本身可用（见 R45 用例） |

## 2. CSS

`cap_css.html` 当前输出：

```
grid=block transform=[] boxShadow=rgb(255, 0, 0) 0px 0px 4px sticky=sticky aspectHeight=90px
gap=8px isColor=rgb(1, 2, 3) whereColor=rgb(4, 5, 6) varColor=rgb(7, 8, 9)
varCascade=rgb(9, 8, 7) varInherit=rgb(1, 2, 3) boxSizing=border-box boxWidth=100 flexItemW=20
safeAlign=center safeItemY=427 absChildX=88 calcMinH=22px calcVarMinH=22px calcMixed=calc(100% - 24px)
bareMedia=2px schemeBg=rgb(0, 0, 200) mediaDark=1 mediaMin=1
```

（`schemeBg`/`mediaDark` 默认是**浅色**：`rgb(200, 0, 0)` / `0`。要跟系统走设 `AGB_COLOR_SCHEME=auto`，
要钉死设 `dark` / `light` —— 见 R86。）

| 特性 | 当前 computed | 判定 / 建议 |
|---|---|---|
| `display: grid` | `block` | **不支持**：`CSSParser::modernKeywordID()` 明确把 grid 降级成 block（`CSS.supports('display','grid')` 也返回 false）；P1 |
| `transform: translateX(10px)` | 绘制生效（computed 仍返回空） | **部分支持（R52/R55/R80）**：别名表补无前缀 `transform`，修掉 parseTransform 的三个上游 bug；translate/scale/rotate 在 `RenderBlock::paint()` 里按 transform-origin 合成矩阵绘制，**文字（字形）也走同一矩阵（R80：`Font::drawGlyphs()` 把线性部分塞进 picasso 的 text matrix）**。**限制**：hit-test 未映射、未建 stacking context、`getComputedStyle(el).transform` 仍为空；变换后的字形仍是 1-bit（全站文字都是 mono，无抗锯齿） |
| `box-shadow: 0 0 4px red` | `rgb(255, 0, 0) 0px 0px 4px` | **已支持**（R11 把 `box-shadow` 别名到 `-webkit-box-shadow`）；旧文档里“非 `-webkit-` 未实现”已过时；绘制未逐像素验证 |
| `width/height: 100vw/100vh` | `100vw=1100px`、`50vh=400px`（resize 到 500x400 后是 `500x200`） | **已支持（R53/R69）**：vw/vh 是文档级 CSS 变量（`rewriteViewportUnits()` 写 `var(--vwunit-…)`，`Document::viewportCustomProperties()` 按当前 FrameView 尺寸给值），窗口 resize 时随样式重算；`vmin/vmax` 同。`calc(100vw - 100px)` 这类含 vw/vh 的 calc 由 `Document::addViewportCalc()` 存模板按视口代入（2008 语法没有 `+`/`-` 产出式，var-in-calc 进不了语法）。zoom 变化按 `effectiveZoom` 折算成 CSS px（R70）。 |
| 嵌套自定义属性的 `calc()`（`--card: calc((var(--w) - var(--count) * var(--gap)) / var(--n))`） | `265.333px` | **已支持（R53）**：`substituteCalcVariables`+`foldCalcConstants` 迭代 3 轮；纯数字 `calc()` 也可折叠（`calc(3 - 1)`→2）；**含 `%` 的混合单位已支持（R54）**：`calc(100% - 24px)`、`calc(20px + 10%)`、`calc((100% - 20px)/2)`、`calc(100%/3)` 在布局期按包含块求值（`Length::Calculated`） |
| `flex-wrap: wrap`（含 flex 子项） | 按 contentWidth 断行、行内按 grow 分配 | **已支持（R54/R81）**：单行与换行共用一套三趟布局（basis/floor/ceiling → 断行 → 每行按 grow 或 `shrink×base` 分配并用 override size 重排），再按 `justify-content`/`align-items`/`align-content` 摆放；`align-items:stretch` 在行算法里把 auto 高子项撑到行高 |
| `flex: 1 1 0` / `flex: 0 0 200px` / `flex-basis` | grow/shrink/basis 生效 | **已支持（R54，R81 补齐单行）**：`flex` 简写展开成 `-webkit-box-flex` + `flex-shrink` + `flex-basis`（`flex:1`→1 1 0%，`auto`→1 1 auto，`none`→0 0 auto）；新增属性走 `CSSPropertyNames` 末尾追加 + `extraProps` 旁路。R81 之前**只有 `flex-wrap:wrap` 那条路读 basis**，单行仍按“普通块宽度 + box-flex”分配（`flex:2` 会输给内容更多的 `flex:1`）；现在单行与换行共用 `layoutFlexLines()`：basis 起算、`flex-shrink × base` 收缩、`min|max-width` 冻结重分、`min-width:auto` 取 min-content（`min-width:0` 不 clamp） |
| `align-self` | 被忽略 | **不支持**：`RenderStyle` 没有 align-self，子项一律按 `align-items`；`align-self:flex-end` 的元素会被 stretch 成整列高（css3test `aside` 因此比 Chrome 高） |
| flex 的 computed 值 | `getComputedStyle(el).flexGrow/.flexShrink/.flexBasis` 与 `getPropertyValue('flex-grow'/'flex-shrink'/'flex-basis')` 全是空串；`display` 是 `-webkit-box` | **缺口（2026-11-06 实测）**：布局是对的，但 `CSSComputedStyleDeclaration` 没序列化这几个属性（`flex` 简写展开后只存在 RenderStyle 的 box 数据里），且 `display:flex` 保留了 `-webkit-box` 的 computed。影响：用 `getComputedStyle().display === 'flex'` 或读 `flexBasis` 做断言的库/测试会误判。修法方向：在 `CSSComputedStyleDeclaration` 补 `flex-grow`/`flex-shrink`/`flex-basis` 分支，并在 `getPropertyValue('display')` 里把 `-webkit-box` 反序列化成 `flex`（`CSSParser::modernKeywordID()` 的逆映射） |
| `align-content: space-between/center/flex-end` | 行间分布生效 | **已支持（R54）**：默认 `stretch` 把单行撑满容器（`align-items:center` 因此在整个盒子里居中，与 Chrome 一致）；`space-around` 近似成 `space-between` |
| `scrollbar-width: none` / `-ms-overflow-style: none` | 滚动条不占宽 | **已支持（R54）**：`RenderStyle::scrollbarNone()`；文档滚动条在 `FrameView::applyOverflowToViewport()`、元素滚动条在 `RenderLayer::updateScrollInfoAfterLayout()` 里不占空间 |
| `::-webkit-scrollbar{display:none}` / `{width:0}` | 不占滚动条宽度 | **已支持（R56）**：解析期把这类规则改写成 `scrollbar-width:none`（`rewriteWebkitScrollbarRules()`，@media 内也处理）；`-thumb/-track/-button/-corner/-resizer` 规则整条丢弃（原生滚动条本来就画不了这些样式），不影响滚动本身 |
| `position: sticky` | `sticky`（computed 也返回 `sticky`） | **已支持（R82）**：`top`/`right`/`bottom`/`left`（含 `%`）单轴或双轴；最近滚动容器可以是 overflow 盒或文档视图；绘制/命中/几何三处都随滚动平移，滚动时整视口重绘（不 blit）。复测：`tools/run_sticky_tests.ps1`（11 条几何 + 4 像素）。**限制**：`scrollIntoView()` 仍是 no-op；sticky 建 layer 参与 stacking 但没有 z-index 细化 |
| `aspect-ratio: 16/9` + `width:160px` | `height=90px` | **已支持**（R41）：宽或高为 auto 时按比例推另一轴（按 content box），`16 / 9` 与 `16/9` 都认 |
| flex `gap: 8px` | `8px` | **已支持**（R41）：`gap`/`row-gap`/`column-gap`，简写 1 值=两轴、2 值=row col；flex 仍是 2008 的 `-webkit-box` 体系，但间距按现代语义插在 in-flow 子项之间 |
| `:is(p)` / `:where(p)` | `rgb(1, 2, 3)` / `rgb(4, 5, 6)` | **已支持**（R41）：解析期把 `:is()`/`:where()` 文本展开成选择器列表（`p:is(.a,.b) span` → `p.a span, p.b span`）。限制：参数必须是简单选择器（不能带组合器），且不能嵌套在别的伪类括号里 |
| `var(--missing, rgb(7,8,9))` | `rgb(7, 8, 9)` | 支持（fallback 正常）；**级联/继承已按规范工作**（R39：元素级覆盖、`!important`、继承都对，`getComputedStyle(el).getPropertyValue('--x')` 可读）；`calc()` 内的 `var(--常量token)` 在解析期代入（R43）；仍未做：`@property`、混合单位的 `calc()` 求值 |
| `box-sizing: border-box` | `border-box` | **已支持**（R39）：无前缀名此前整条被丢（只有 `-webkit-box-sizing` 有词条），现在走别名表；布局按 border-box 计算，`getComputedStyle(el).boxSizing` 可读 |
| `@media (prefers-color-scheme: dark)` | **默认 `light`**（不跟随系统）；`AGB_COLOR_SCHEME=dark` 时匹配 | **已支持**（R39，默认值见 R86）：`matchMedia` 与 CSS 用同一答案（`prefersDarkColorScheme()`）。**默认不跟随系统深浅色** —— 宿主不再按 `AppsUseLightTheme` 注入（那会让所有带 dark 块的站点在深色系统上整页变黑，css3test 就是例子）；要看系统值用 `AGB_COLOR_SCHEME=auto`（读注册表），或显式 `dark`/`light` 钉死 |
| `matchMedia('(min-width: …)')` | `matches` 正确 | **已支持**（R39）：width/height/device-*/orientation/resolution/prefers-* 都按真实视口求值，`not`/`only`/媒体类型/逗号列表也处理；未做：`addListener` 的变更回调（返回的 list 是静态快照） |
| 窗口 resize（`--resize WxH` / 拖窗口；1100x800 → 500x400） | `inner=500x400`、`pct=250`（50%）、`vwBox=500x200`、`calcBox=400x150`、`mediaCSS=rgb(200, 30, 40)`（移动端断点）、`mediaCSSwide` 回到基准色、`matchMedia=true`、三种 `resize` 监听都收到 `500x400` | **全部跟随（R69）**：`WM_SIZE → AGW_CMD_RESIZE → macross_view_resize` 走 `FrameView::resize`；`FrameView::layout()` 发现 `styleViewportSize` 变了就先 `Document::viewportSizeChanged()` + `updateStyleIfNeeded()`（selector 重建 → `@media` 按新视口重滤；vw/vh 变量换成新值），再正常布局并按尺寸变化全量重绘（放大到 1400x900 新区域是白的）。`resize` 事件在布局后由 `Frame::sendResizeEvent()` 派发，Ant 侧由 `AntRuntime::dispatchResizeEvent()` 送到页面的 `addEventListener("resize")`/`window.onresize`。代价：每次尺寸变化一次全文档样式重算 + 重排（拖动时按帧合并）。残留：`<body onresize>` 这类 HTML 属性 handler 跑在旧的 KJS 世界，读不到页面 JS 全局 |
| `@media (min-width: 1px)`（无媒体类型） | 生效 | **已支持**（R39）：`rewriteBareMediaQueries()` 补 `all and `；2008 的语法要求媒体类型在前，否则整块丢弃 |
| `align-items: safe center` | `center` | **已支持**（R42）：解析期删掉 `safe`/`unsafe` 溢出关键字，保留对齐关键字；否则两 token 会让整条声明被丢、`align-items` 退回 `stretch` |
| `width: unset`（CSS-wide keyword） | `width: auto`；图片按固有比例，150×51 的图在 `height:17px` 下是 **50×17**（百度 guide-bub 正是 `img{width:16px}` + 更高优先级 `img{width:unset}`） | **已支持（R78）**：`inherit`/`initial` 一直有；`unset` 没有进生成的关键字表，解析期按文本识别（`CSSParser::parseValue()`），继承属性→`inherit`、其余→`initial`（`isInheritedProperty()` 覆盖本引擎实现的属性，未知属性按非继承处理）。**未做**：`revert`/`revert-layer` —— 需要级联来源信息才能回滚到 UA/user 源，按丢弃声明处理 |
| `document.styleSheets` / `sheet.cssRules` / `<style>.sheet` | baidu 首页 `sheets=32 rules=3216`；deepseek 登录页 `sheets=4 rules=2006`；同一对象重复读取 `===` 成立；`insertRule`/`deleteRule` 后 `cssRules.length` 与级联立即变化 | **已支持（R79）**：live wrapper（与 node/canvas 同一套 `antb_wrap_native_as_owner` + class 模型，方法挂在 class 上，document 卸载时释放）；`document.styleSheets` 在文档对象的统一 getter 里分发，`style/link.sheet` 在 `nodeGetter` 里按 `ownerNode` 查找。rule 的 `type`/`selectorText`/`cssText`/`style`/`media`/内层 `cssRules`、`@font-face`、`CSSStyleDeclaration.getPropertyValue/getPropertyPriority/setProperty/removeProperty` 都可读/可改。**限制**：①不区分同源/跨域，跨域样式表的 `cssRules` 也能读（Chrome 会抛 `SecurityError`）；②`sheet.cssRules` 每次访问返回新的 `CSSRuleList`（WebCore 2008 的实现），缓存该列表后看不到后续 `insertRule`，重新读取即可 |
| column flex 里的 `position:absolute` 子项 | 按 `align-items` 定 staticX | **已支持**（R42）：`RenderFlexibleBox::layoutVerticalBox()` 的 positioned 分支按 `boxAlign()` 修正 static position（`BCENTER` 居中、`BEND` 右对齐）；此前无条件贴左 |
| `iframe.frameBorder = "0"`（JS property） | `border-top-width: 0px` | **已支持**（R42）：`kReflectedAttributes` 加 `frameBorder→frameborder`，property 写入落到 `setAttribute()` → `parseMappedAttribute()` 的 `border-width:0` 呈现提示；静态 `frameborder="0"` 一直可用 |
| 8 位 hex `#RRGGBBAA` / `#RGBA` | `rgba(138, 71, 38, 0.945)` | **已支持（R51）**：`Color::parseHexColor()` 认 4/8 位；声明值与样式表里的 8 位 hex 还会在解析前改写成 `rgba()`（旧 tokenizer 只认 3/6 位）。alpha 是 8 位量化，序列化按 Chrome 的习惯给两位有效数字（`0.5`/`0.25`） |
| CSS Color 4 色彩函数：`hsl(200 60% 20%)`、`rgb(1 2 3)`、`hsl(... / .5)`、`rgb(8% 24% 32%)`、`hsl(200deg ...)` | 与 Chrome 逐字节一致（`hsl(200 60% 20%)` → `rgb(20, 61, 82)`） | **已支持（R83）**：2008 只认逗号形式，`parseColorParameters`/`parseHSLParameters` 改成"逗号或空格、alpha 用 `,` 或 `/`"的单循环；hue 支持 `deg`/`rad`/`grad`；hsl→rgb 改四舍五入（旧实现 `0.32*256` 截断成 81，Chrome 82）。**限制**：`turn` 单位没有 `CSS_TURN`（tokenizer 不接受，声明被丢）；alpha 8 位量化，`/ 50%` 序列化成 `0.5`、`/ 0.25` 序列化成 `0.25`（与 Chrome 一致），但 `0.125` 会变 `0.13` |
| `background: url() 0 0 / 16px 16px`、`center / cover`、`background-size: contain` | `background-size` = `16px 16px` / `cover` / `contain`；`background-position` 与 Chrome 一致 | **已支持（R84）**：简写的 `properties[]` 补出 `-webkit-background-size`，循环跳过 `/` 分隔符（以前整条声明被丢）；`cover`/`contain` 的 computed 以前返回空串，现在原样输出文本（新增 `CSS_IDENT_TEXT` 单位）；两个分量都是 auto 时只输出 `auto`。复测：`tools/pages/layout/background_shorthand.html`（12 case 对拍 Chrome）、`background_paint.html`（7 像素，接进 `run_layout_tests.ps1`） | 已支持 |
| `background: url(...) var(--color)`（色从变量来） | 与字面量色一致：图 + 底色都画 | **已修（R85）**：`var()` 展开的声明由 `applyExpandedProperty()` 用全新 `CSSParser` 重解析，以前没有 `styleElement`，`parseBackgroundImage()` 里 base 为空 → 相对 URL 变**空**（`[cssimg] url=[]`）→ 第二个永不加载的 `CachedImage`。现在 `CSSParser::setValueDocument()` + `document()->baseURL()` 兜底，展开后的 URL 与源声明完全相同（绝对 URL 本来就不受影响）。复测：`background_paint.html` 的 `#varrel`（相对 URL + `var(--tint)`）2 像素 |
| `data:` URI 图片 | 正常绘制（`<img src="data:…">`、`background: url(data:…)`） | **已修（R85）**：`ResourceHandleManager::add()` 在 file:// 之前加 data: 分支，本地解析 `data:[<mediatype>][;base64],<payload>`（base64 + 百分号解码，默认 `text/plain;charset=US-ASCII`），走与 cache hit/file 相同的异步 `CachedDelivery`。复测：`background_paint.html` 的 `#data`/`#dataimg` 2 像素 |
| 自定义属性的 computed 序列化 | `getComputedStyle(root).getPropertyValue('--x')` 返回 **`-agb-x` 开头的内部名**（值里的 `var()` 也被改写） | **缺口（R83 实测）**：解析期把 `--x` 改写成 `-agb-x` 防冲突，`getComputedStyle` 读回时**没有还原**；渲染不受影响，但站点自己读字面量（或快照对比）会拿到错字符串。修法：读取时把 `-agb-` 前缀还原成 `--`（值与声明都一样） |
| `min-height: calc(18px + 4px)` | `22px` | **已支持**（R43）：解析期把纯常量的 `calc()` 折叠成单值（`+ - * /`、括号、绝对长度单位）；自定义属性值里的 `calc(var(--token) + 4px)` 先做 sheet 级常量代入再折叠。**限制**：含 `%`/`vw`/`vh`/`em`/`rem`/`env()` 的混合单位 calc 仍不支持（保留原样、声明被丢），需要布局期求值才能覆盖 `calc(100% - 24px)` 这类 |

| `indexedDB.open(...)` | 请求异步 `onerror` | **桩实现（R57 诊断）**：`window.indexedDB` 不存在时直接引用会抛 `ReferenceError`，所以提供 open/deleteDatabase/databases 都异步失败的桩 + `IDBKeyRange`；站点走“失败回退内存存储”的分支，而不是在 Promise 链里炸掉。真正的 IDB 存储未做 |

已确认可用的（别再当缺口重开）：块/inline-block/float/绝对定位/margin auto/padding/border-radius/
opacity/渐变（R11）、`var()` 含 fallback 与元素级级联（R39）、`box-sizing`（R39）、
`prefers-color-scheme` 与 `matchMedia` 的尺寸/方向查询（R39）、flex 子元素 blockify（R39）、
`gap`/`aspect-ratio`/`:is()`/`:where()`（R41）、`safe`/`unsafe` 对齐前缀与 flex 绝对定位子项对齐、
`frameBorder` 属性反射（R42）、常量 `calc()` 折叠（R43）、
百分比混合 `calc()`、`flex` 简写/`flex-basis`/`align-content`/`scrollbar-width`（R54）、
`classList.add/toggle`、`matches`、`closest`、`dataset`、
`append`、`remove`、`textContent`、`querySelectorAll`、`getElementsByClassName`、`document.cookie`。

## 3. 已知但另处详述（本页不重复）

- **worker 无网络**（worker 里没有 `fetch`/XHR，`importScripts` 只能预取字面量 URL，
  无 `SharedWorker`/`ServiceWorker`）：[plan-roadmap.md](plan-roadmap.md) P1-d / R22。
- **入口模块限制**（动态/async 入口、`import.meta.url` 伪装、DCL 之后的 inline 模块）：
  R7 / [arch-module-loading.md](arch-module-loading.md)。
- **wasm 是解释执行**：R19。

## 4. 修复后的收尾动作

1. 改代码 → 跑 §0 的对应命令，确认 `[exit]` 里的值变了。
2. 更新本页表格（把“当前值”改成新值，判定改为“已支持”，保留一行历史/或直接移到 §2 末尾的可用清单）。
3. 如果是一个缺陷（不是纯新增能力），在 [hist-lessons.md](hist-lessons.md) 追加 `R<n>`（现象/根因/修法/验证）。
4. 如果改变了站点兼容性判断（比如 grid 可用了），同步 [arch-status.md](arch-status.md) §2 的 CSS 边界。

## 5. 把 shim 拿掉，看真实报错（诊断手法）

JS 侧 shim 在 `prelude.js`（window/document/XHR/storage）和 `polyfills.js`（DOM 构造器、`append`、
console guard 等）。它们让页面"看起来能跑"，缺的 API 被静默补上，控制台往往没有错误。
诊断时**只关 polyfills**（prelude 必须保留，否则连 `window`/`document` 都没有）：

```powershell
'/* shims disabled */' | Set-Content build-zig-native\polyfills_off.js
$env:AGB_POLYFILLS = 'E:\mybrowsers\agave\build-zig-native\polyfills_off.js'
agave_shell.exe --probe https://chat.deepseek.com/sign_in --wait 12000 --eval "probeExit(document.title)"
```

DeepSeek 登录页的对照组：开 polyfills `inputs=2 all=124`；关掉后 `inputs=0 all=68`，并出现
`ResizeObserver: parameter 1 is not of type 'Element'` —— 这是被 shim 遮住的第一层失败。

**注意**：不是所有 gap 都会变成异常。被页面 `try/catch` 吞掉的、缺失的事件（`onload`）、
不支持的属性选择器都可能完全静默。这时要靠 `cap_*` 探测页做"有/无"对照，而不是只看控制台。

## 6. 案例：DeepSeek 登录页的微信二维码

现象：登录页主体正常，但"微信扫码登录"右侧的框一直空白；控制台只有一条空消息
`[con-call] Error: `，看不出原因。

排查链：

1. 二维码由微信 `WxLogin` 组件创建 iframe 并塞进 `#wxLogin`；DeepSeek 的 wechat-login 模块在
   `script.onload` 里 resolve 加载 Promise，然后才 `new WxLogin(...)`。
2. 引擎此前把 `el.onload = fn` 当普通 expando，**从不触发**（同一元素的 `addEventListener('load')`
   正常），Promise 永不 resolve → iframe 从不创建 → 控制台无错误。见 `tools/pages/cap_script.html`
   （修前 `hits=1`，修后 `hits=11`）。
3. R27 修复后：`iframes=1 wxKids=1`；`open.weixin.qq.com/connect/qrconnect` 返回 200 并 commit；
   该 URL 单独打开能画出二维码，放进跨域 iframe（`debug/frame_cross.html`）也能画出。
4. 现在二维码**能画出来**了，剩下两层都已定位并修掉前两层：
   - **`:empty` 不失效**（R28）：`#wxLogin` 在 React 重渲染时是空的，解析成 `display:none`、
     没有 renderer；二维码 iframe 插进去后 `:empty` 不再匹配却没人重算 → 一直 0×0。
   - **CSS 缩放的图片画成鬼影**（R29）：二维码图片按 160×160 缩放显示时，黑色模块被画成透明，
     白底上只剩轮廓、深色底上"看着像二维码"，扫码器读不出来。
   - **iframe 视口取了父页的**（R30）：`outerWidth` 是 prelude 执行时复制的 1100，微信页按它排版，
     塞进 200×200 的 iframe 就溢出。
5. 仍未完成：二维码在 200×200 的 iframe 里**右侧/底部被裁掉**（内容宽于视口，出现滚动条）。
   下一步是 per-frame `window` 代理（`window.parent`/`top`/`frames` 现在都指向自身），
   然后再看子文档布局；`__agbActivateSlot(1)` 可以直接读子 frame 文档来量。



