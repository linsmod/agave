# 能力缺口：JS/DOM 与 CSS 支持清单（实测）

> 这篇只回答一件事：**哪些 Web 平台能力在 agave 里没有、或只实现了一半**，以及怎么用一条命令复测。
> 已修的历史缺陷在 [lessons.md](lessons.md)；当前待办与优先级在 [handover.md §4](../handover.md)；
> 构建与探针用法在 [debugging.md](debugging.md)。本页不重复这些内容。
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
`cap_postmessage.html`：`postMessage=undefined sent=ERR TypeError: undefined is not a function got=0`

| 能力 | 当前值 | 影响 | 判定 / 建议 |
|---|---|---|---|
| `document.forms` / `links` / `images` / `anchors` / `scripts` / `embeds` / `plugins` / `applets` | **已支持（R34）**：真实集合（`forms=1 images=2 scripts=1 links=1`）。注意是**快照数组**，不是 live collection | —— | 已支持 |
| `HTMLFormElement.submit()` / `.action` / `.elements` / `.reset()` | **已支持（R34）**：`formSubmit=function formAction=string formElements=2`；`checkValidity()` 恒返回 true（没有约束校验） | —— | 已支持（`checkValidity` 是桩） |
| `history.pushState` / `replaceState` / `state` / `length` / `popstate` | **已支持（R36）**：state 对象真的存下来了（`pushState` 后 `history.state` 是同一个值），`length` 随 entry 增长，`back/forward/go` 恢复 URL 并派发带 `state` 的 `popstate`，`pushState` 会截断 forward。跨文档的前进后退交给 FrameLoader（`[nav] request main type=back-forward`） | —— | 已支持（`cap_history.html`；限制：`length` 只数当前文档的 entry） |
| `localStorage` 跨文档 | A 页写入 → 导航到 B → `null`；同一跑的 cookie 正常（`cookie=true`） | 用 localStorage 存 token/购物车的多页流程会丢状态；测试也不能拿它当跨页标记（用 cookie） | 缺口，P1 |
| `window.postMessage` / `message` 事件 | **已支持**：同窗口/跨 frame 都能投递（`{data, origin, source}`，`origin` 取发送方文档的 scheme://host[:port]）；`cap_postmessage.html` → `postMessage=function sent=yes got=1` | —— | 已支持 |
| iframe 的 `contentWindow` / `contentDocument` | **已支持（R34）**：`iframeCw=yes`，`contentWindow.document === contentDocument` 成立；子文档还没建好时是 `null` | —— | 已支持 |
| 每个 frame 的 `window` 身份（`parent`/`top`/`self`/`frames`/`length`/`name`/`frameElement`） | **已支持（R31）**：frame 有自己的 `Window` 代理，`parent !== self`、`parent.document` 是父文档、`frames.length`/`frames[i]`/`name`/`frameElement` 都按 frame tree 回答，未知属性委托给共享全局 | —— | 已支持（`cap_window_identity.html`） |
| 子 frame 的顶层 `var` 污染父页 | 仍是共享全局：子 frame 里 `var x = 1` 父页也能读到（`window.foo` 同理） | 依赖「全局隔离」的 polyfill 会互相看见；规范上这是每个 browsing context 独立的全局对象 | 已知限制，要修只能每个 frame 一个 engine（方案 A，不做；见 `docs/architecture/frame-window.md`） |
| `HTMLImageElement.complete` / `naturalWidth` / `naturalHeight` / `width` / `height` / `decode()` | **已支持（R34）**：`imgComplete=0 imgNatural=0 imgWidth=1 imgDecode=function`（`decode()` 立即 resolve） | —— | 已支持 |
| `document.querySelector(All)` 的选择器引擎 | **已支持（R35）**：选择器列表、组合器（后代/`>`/`+`/`~`）、属性选择器六个操作符、常用伪类（`nth-child`/`not`/`first-child`/`checked`/…）。实现是 `src/javascript/ant_backend/AgbSelector.h`（WebCore 2008 没有 `Node::querySelectorAll`） | —— | 已支持（`cap_selectors.html`） |
| `Element.insertAdjacentHTML` / `insertAdjacentElement` / `insertAdjacentText` / `replaceChildren` | **已支持（R34）**：四个插入位置都走 `insertBefore/appendChild`，HTML 走 `createFragmentFromMarkup` | —— | 已支持 |
| `HTMLElement.innerText` / `outerText` | **已支持（R34）**：走 `Element::innerText()`（会触发布局） | —— | 已支持 |
| 元素树遍历 `parentElement` / `firstElementChild` / `lastElementChild` / `nextElementSibling` / `previousElementSibling` / `isConnected` / `getRootNode()` | **已支持（R34）** | —— | 已支持 |
| 反射属性 `hidden` / `dir` / `lang` / `tabIndex` / `contentEditable` / `draggable` / `translate` / `slot` / `part` / `spellcheck` / `autofocus` | **已支持（R34）**：读写都落到属性（`el.hidden = true` → `hasAttribute('hidden')`） | —— | 已支持 |
| `getAttributeNames()` / `toggleAttribute()` / `nodeValue` / `offsetParent` / `scrollIntoView()` / `classList.replace()` | **已支持（R34）** | —— | 已支持 |
| `document.activeElement` / `hasFocus()` / `visibilityState` / `importNode` / `adoptNode` / `elementFromPoint` | **已支持（R34）**；`visibilityState` 恒 `"visible"`，`activeElement` 无焦点时回落到 `body` | —— | 已支持 |
| `document.fonts` | **桩（R34）**：`{status:'loaded', ready:已 resolve, add/delete/check/forEach}`。没有 web font 加载，`ready` 立即兑现 | 用 `await document.fonts.ready` 的页面不再挂住；但 `fonts.load()` 不会真的加载字体 | 桩 |
| `window.open` 的引擎侧策略 | 可用，但**绕过** `FrameLoaderClientMC::dispatchCreatePage`，走控件 `agave_control_open_window` | 引擎级 new-window 策略/features 不生效；`[nav]` 里看到的是 `popup create`，不是 `popup open` | 管道差异，P2 |
| `el.onload = fn` 等事件处理属性 | **已修（R27）**：`hits=11`（属性 + addEventListener 都触发）；此前只有 `addEventListener` 生效，`onload` 是静默 expando | —— | 已支持 |

定位锚点：

- 集合：`AntRuntime.cpp` 的 `titleGetter` 里 `"forms"`/`"links"`/… 分支，走 `wrapNodeList()`（快照，不是 live collection）。
- 元素属性：`nodeGetter` / `nodeSetter`（反射属性表 `kReflectedAttributes`）、`g_nodeClass` 上的方法（`getRootNode`/`insertAdjacent*`/`submit`…）。
- History：`AntRuntime.cpp` 的 `historyPushState`（注释 “only rewrites the document URL … without a
  real history entry”）；`historyBack/Forward` 直接调 `page()->goBack()/goForward()`；
  `popstate` 只在 `prelude.js` 的 `onpopstate` 名字表里出现，没有任何派发点。
- 表单方法/属性：桥接类表（`AntBridge.c` 的 wrapper 方法表 + `AntRuntime.cpp` 的类属性表）没有
  `HTMLFormElement` 的 `submit`/`action` 等条目。
- storage：`AntRuntime.cpp` 的 storage 绑定；写入在文档 teardown 落盘、读取只在 engine 重建时发生
  （跨进程能读到旧值，同进程导航读不到）。
- `window.postMessage`（已支持）：`prelude.js` 的 `g.postMessage` → 桥 `__agbFramePostMessage`
  （`AntRuntime.cpp` 的 `globalFramePostMessage`）派发到 `message` 监听器。宿主桥 `agavePostMessage`
  是另一个方向（页面→壳），不要混。
- iframe 文档：桥接层有 `g_docSlots` 槽位，`__agbActivateSlot(i)` 可切换绑定；
  `contentWindow`/`contentDocument` 没有暴露成 DOM 属性。
- 缩放图片（已修，R29）：`BitmapImage::draw` 的 `dst!=src` 分支原本走 picasso 的图像填充采样器，
  画出来是鬼影；现在由 `src/webcore/platform/picasso/ImagePS.cpp` 的 `draw_scaled_image()`
  自己做 box filter 再 bitblt。
- 属性选择器：`AntRuntime.cpp` 的 `nodeQuerySelector` / `nodeQuerySelectorAll` 只把简单选择器交给
  WebCore；`[attr]` / `[attr=v]` / `[class*=]` / `div[attr]` 都返回空（`.class`、`#id`、tag 正常）。
- 图片属性：`nodeGetter` 没有 `HTMLImageElement` 的 `complete`/`naturalWidth`/`width`/`height` 分支。
- 事件处理属性（已修）：`AntRuntime.cpp` 的 `nodeSetter`/`nodeGetter` `on*` 分支 +
  `setNodeEventHandler()`/`findNodeEventHandler()`（R27）。

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

## 2. CSS

`cap_css.html` 当前输出：

```
grid=block transform=[] boxShadow=rgb(255, 0, 0) 0px 0px 4px sticky=static aspectHeight=90px
gap=8px isColor=rgb(1, 2, 3) whereColor=rgb(4, 5, 6) varColor=rgb(7, 8, 9)
varCascade=rgb(9, 8, 7) varInherit=rgb(1, 2, 3) boxSizing=border-box boxWidth=100 flexItemW=20
safeAlign=center safeItemY=427 absChildX=88 calcMinH=22px calcVarMinH=22px calcMixed=0px
bareMedia=2px schemeBg=rgb(0, 0, 200) mediaDark=1 mediaMin=1
```

（`schemeBg`/`mediaDark` 随系统深浅色变化：浅色时是 `rgb(200, 0, 0)` / `0`。要固定看某一种，
探针前设 `AGB_COLOR_SCHEME=dark` 或 `light`，宿主不会覆盖显式设置的值。）

| 特性 | 当前 computed | 判定 / 建议 |
|---|---|---|
| `display: grid` | `block` | **不支持**：`CSSParser::modernKeywordID()` 明确把 grid 降级成 block（`CSS.supports('display','grid')` 也返回 false）；P1 |
| `transform: translateX(10px)` | `""` | **不支持**（含 `-webkit-transform` 的读取与绘制）；动画/位移类站点影响大；P1 |
| `box-shadow: 0 0 4px red` | `rgb(255, 0, 0) 0px 0px 4px` | **已支持**（R11 把 `box-shadow` 别名到 `-webkit-box-shadow`）；旧文档里“非 `-webkit-` 未实现”已过时；绘制未逐像素验证 |
| `position: sticky` | `static` | **不支持**，按 static 布局；P2 |
| `aspect-ratio: 16/9` + `width:160px` | `height=90px` | **已支持**（R41）：宽或高为 auto 时按比例推另一轴（按 content box），`16 / 9` 与 `16/9` 都认 |
| flex `gap: 8px` | `8px` | **已支持**（R41）：`gap`/`row-gap`/`column-gap`，简写 1 值=两轴、2 值=row col；flex 仍是 2008 的 `-webkit-box` 体系，但间距按现代语义插在 in-flow 子项之间 |
| `:is(p)` / `:where(p)` | `rgb(1, 2, 3)` / `rgb(4, 5, 6)` | **已支持**（R41）：解析期把 `:is()`/`:where()` 文本展开成选择器列表（`p:is(.a,.b) span` → `p.a span, p.b span`）。限制：参数必须是简单选择器（不能带组合器），且不能嵌套在别的伪类括号里 |
| `var(--missing, rgb(7,8,9))` | `rgb(7, 8, 9)` | 支持（fallback 正常）；**级联/继承已按规范工作**（R39：元素级覆盖、`!important`、继承都对，`getComputedStyle(el).getPropertyValue('--x')` 可读）；`calc()` 内的 `var(--常量token)` 在解析期代入（R43）；仍未做：`@property`、混合单位的 `calc()` 求值 |
| `box-sizing: border-box` | `border-box` | **已支持**（R39）：无前缀名此前整条被丢（只有 `-webkit-box-sizing` 有词条），现在走别名表；布局按 border-box 计算，`getComputedStyle(el).boxSizing` 可读 |
| `@media (prefers-color-scheme: dark)` | 深色时匹配 | **已支持**（R39）：`AGB_COLOR_SCHEME` 由宿主按注册表 `AppsUseLightTheme` 设置；`matchMedia` 与 CSS 用同一答案 |
| `matchMedia('(min-width: …)')` | `matches` 正确 | **已支持**（R39）：width/height/device-*/orientation/resolution/prefers-* 都按真实视口求值，`not`/`only`/媒体类型/逗号列表也处理；未做：`addListener` 的变更回调（返回的 list 是静态快照） |
| `@media (min-width: 1px)`（无媒体类型） | 生效 | **已支持**（R39）：`rewriteBareMediaQueries()` 补 `all and `；2008 的语法要求媒体类型在前，否则整块丢弃 |
| `align-items: safe center` | `center` | **已支持**（R42）：解析期删掉 `safe`/`unsafe` 溢出关键字，保留对齐关键字；否则两 token 会让整条声明被丢、`align-items` 退回 `stretch` |
| column flex 里的 `position:absolute` 子项 | 按 `align-items` 定 staticX | **已支持**（R42）：`RenderFlexibleBox::layoutVerticalBox()` 的 positioned 分支按 `boxAlign()` 修正 static position（`BCENTER` 居中、`BEND` 右对齐）；此前无条件贴左 |
| `iframe.frameBorder = "0"`（JS property） | `border-top-width: 0px` | **已支持**（R42）：`kReflectedAttributes` 加 `frameBorder→frameborder`，property 写入落到 `setAttribute()` → `parseMappedAttribute()` 的 `border-width:0` 呈现提示；静态 `frameborder="0"` 一直可用 |
| `min-height: calc(18px + 4px)` | `22px` | **已支持**（R43）：解析期把纯常量的 `calc()` 折叠成单值（`+ - * /`、括号、绝对长度单位）；自定义属性值里的 `calc(var(--token) + 4px)` 先做 sheet 级常量代入再折叠。**限制**：含 `%`/`vw`/`vh`/`em`/`rem`/`env()` 的混合单位 calc 仍不支持（保留原样、声明被丢），需要布局期求值才能覆盖 `calc(100% - 24px)` 这类 |

已确认可用的（别再当缺口重开）：块/inline-block/float/绝对定位/margin auto/padding/border-radius/
opacity/渐变（R11）、`var()` 含 fallback 与元素级级联（R39）、`box-sizing`（R39）、
`prefers-color-scheme` 与 `matchMedia` 的尺寸/方向查询（R39）、flex 子元素 blockify（R39）、
`gap`/`aspect-ratio`/`:is()`/`:where()`（R41）、`safe`/`unsafe` 对齐前缀与 flex 绝对定位子项对齐、
`frameBorder` 属性反射（R42）、常量 `calc()` 折叠（R43）、
`classList.add/toggle`、`matches`、`closest`、`dataset`、
`append`、`remove`、`textContent`、`querySelectorAll`、`getElementsByClassName`、`document.cookie`。

## 3. 已知但另处详述（本页不重复）

- **worker 无网络**（worker 里没有 `fetch`/XHR，`importScripts` 只能预取字面量 URL，
  无 `SharedWorker`/`ServiceWorker`）：[handover.md P1-d](../handover.md) / R22。
- **入口模块限制**（动态/async 入口、`import.meta.url` 伪装、DCL 之后的 inline 模块）：
  R7 / [module-pipeline.md](architecture/module-pipeline.md)。
- **wasm 是解释执行**：R19。

## 4. 修复后的收尾动作

1. 改代码 → 跑 §0 的对应命令，确认 `[exit]` 里的值变了。
2. 更新本页表格（把“当前值”改成新值，判定改为“已支持”，保留一行历史/或直接移到 §2 末尾的可用清单）。
3. 如果是一个缺陷（不是纯新增能力），在 [lessons.md](lessons.md) 追加 `R<n>`（现象/根因/修法/验证）。
4. 如果改变了站点兼容性判断（比如 grid 可用了），同步 [handover.md](../handover.md) §5 的 CSS 边界。

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

