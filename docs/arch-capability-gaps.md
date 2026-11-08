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
| 每个 frame 的 `window` 身份（`parent`/`top`/`self`/`frames`/`length`/`name`/`frameElement`） | **已支持**：每个 frame 文档有自己的 **realm**（默认开启，`AGB_PER_FRAME_REALM=0` 回退旧模型），`window`/`self` 就是该 realm 的全局对象，`parent !== self`、`parent.document` 是父 realm 的 document 对象、`top.Array !== Array`、`frames.length`/`name`/`frameElement` 按 frame tree 回答。旧模型（回退时）是 `Window` 代理 + 共享全局（R31） | 已修（R132）：`location`/`history` 按自身 document/Frame 回答，`parent.location` 读父文档；剩余 S4 的 WindowProxy 转发语义（目前 `parent` 是父 realm 的 global） | 已支持（`design-realm-document-binding.md`） |
| 子 frame 的顶层 `var` 污染父页 | **已隔离**：每个 frame 一个 realm，子 frame 的 `var x = 1` 父页读不到；跨帧调用 `parent.fn()`、读 `parent.someVar` 仍可用（同 VM 跨 realm 引用，不序列化） | ES module map 仍按引擎共享（两个 frame import 同一模块会拿到同一实例，S3 待做） | 已支持（`run_realm_tests.ps1` → `childVarLeak=undefined`） |
| `HTMLImageElement.complete` / `naturalWidth` / `naturalHeight` / `width` / `height` / `decode()` | **已支持（R34）**：`imgComplete=0 imgNatural=0 imgWidth=1 imgDecode=function`（`decode()` 立即 resolve） | —— | 已支持 |
| `document.querySelector(All)` 的选择器引擎 | **已支持（R35）**：选择器列表、组合器（后代/`>`/`+`/`~`）、属性选择器六个操作符、常用伪类（`nth-child`/`not`/`first-child`/`checked`/…）。实现是 `src/javascript/ant_backend/AgbSelector.h`（WebCore 2008 没有 `Node::querySelectorAll`） | —— | 已支持（`cap_selectors.html`） |
| `Element.insertAdjacentHTML` / `insertAdjacentElement` / `insertAdjacentText` / `replaceChildren` | **已支持（R34）**：四个插入位置都走 `insertBefore/appendChild`，HTML 走 `createFragmentFromMarkup` | —— | 已支持 |
| `HTMLElement.innerText` / `outerText` | **已支持（R34）**：走 `Element::innerText()`（会触发布局） | —— | 已支持 |
| 元素树遍历 `parentElement` / `firstElementChild` / `lastElementChild` / `nextElementSibling` / `previousElementSibling` / `isConnected` / `getRootNode()` | **已支持（R34）** | —— | 已支持 |
| 反射属性 `hidden` / `dir` / `lang` / `tabIndex` / `contentEditable` / `draggable` / `translate` / `slot` / `part` / `spellcheck` / `autofocus` | **已支持（R34）**：读写都落到属性（`el.hidden = true` → `hasAttribute('hidden')`） | —— | 已支持 |
| `getAttributeNames()` / `toggleAttribute()` / `nodeValue` / `offsetParent` / `scrollIntoView()` / `classList.replace()` | **已支持（R34）**；⚠ `scrollIntoView()` 只是存在（R82 实测滚动前后 `getBoundingClientRect().top` 不变，**不滚动**） | —— | 部分 |
| 文档滚动：`documentElement.scrollTop` / `body.scrollTop` / `window.scrollTo/scrollBy` / `pageYOffset` / `scrollX/Y` | **已支持（R82）**：`scrollTop/Left` 在 html/body 上路由到 `FrameView::setContentsPos`，`getBoundingClientRect()` 返回视口坐标（滚动后为负值），`pageYOffset/scrollY` 是实时 getter | 修前这四条全是 no-op（`scrollTop` 读回 0，滚动后 rect 不变） | 已支持 |
| `document.currentScript` | **已支持（R95）**：脚本执行期间是那个 `<script>` 元素（内联/外链/`defer`/`async`/动态插入/嵌套插入/`eval`/`new Function` 都算），模块与脚本返回之后是 `null` | 靠它取自己 `src` / `data-*` 的 JSONP、上报 SDK、懒加载器；微任务里读到的值与 Chrome 不同（Chrome 仍看得见元素，见 R95） | 已支持 |
| `XMLHttpRequest` 的 `responseText` / `response` | **已支持（R96）**：异步路径按 `responseType` 分派 —— `''`/`text`/`json` 时 `responseText` 是解码后的 UTF-8 文本（`json` 再 `JSON.parse`）；`arraybuffer`/`blob` 给二进制。`responseType` 为 `json`/`blob`/`arraybuffer` 时读 `responseText` 抛 `InvalidStateError`（`DOMException`） | 读接口返回、`xhr.responseType='json'` 的库 | 已支持 |
| `XHR.getResponseHeader` / `getAllResponseHeaders` / `overrideMimeType` / `statusText` / `readyState` | **已支持（R97）**，逐项与 Chrome 对拍：`getResponseHeader` 大小写不敏感、缺失返回 `null`、`readyState < 2` 时返回 `null`；`getAllResponseHeaders` 是 **小写名 + 按名排序 + `name: value\r\n` + 结尾 CRLF**，空时 `""`；`Set-Cookie` 两个访问器都过滤掉（Chrome 同样）；`statusText` 来自网络层（`ResourceHandleNet` 按状态码补标准 reason phrase）；`open()` 重置 status/头/`responseText`；`readyState` 走 `1,2,3,4`；`overrideMimeType` 的 charset 优先于 `Content-Type` 解码（但 `getResponseHeader('Content-Type')` 仍报原头） | 依赖响应头分页/判类型的库（多数 SDK） | 已支持 |
| `TextDecoder` 的编码标签 | **部分（R98）**：`utf-8`/`latin1`/`iso-8859-1`/`windows-1252`/`utf-16le` 由 ant 自带；`gbk`/`gb2312`/`gb18030`/`big5`/`euc-kr`/`euc-jp` 走宿主 provider（ant 只留挂载点，码表用 WebCore 现成的，进程里只有一份）。`shift_jis` 能构造但**解码错字**（`97 5C` 应是 `予` 却解成 `椿`）—— WebCore `shiftjis_conv_to_uchar` 的行列换算有误，是既有缺陷 | GBK 接口现在正常；**日文 Shift_JIS 页面仍乱码** | 部分支持；shift_jis 缺口，P1 |
| `document.readyState` / `document.body` / `document.head` | **已支持（R93）**：`readyState` 是真实状态 `loading` → `interactive` → `complete`（WebKit 2008 的中间态原名 `loaded`，对外按标准报 `interactive`）；`<body>` 尚未解析时 `document.body` 是 **`null`**（不是 `undefined`），`document.head` 同理 | —— | 已支持 |
| `XMLHttpRequest` 的 `timeout` / `abort()`、`fetch()` 的 `AbortSignal` | **已支持（R107）**：`xhr.timeout` 是 JS 侧计时器（到点 `readystatechange` → `timeout` → `loadend`，`status=0`），并通过 `__agbHttpAbort(id)` **真的关掉底层请求**（所以不受 WinHTTP ~4 s 的超时精度影响）；`xhr.abort()` 同样先断底层再发 `abort`/`loadend`；`fetch(url,{signal})` 接通 `AbortController`，abort 后以 `AbortError` 拒绝。未做：`AbortSignal.timeout()` 若 ant 不提供需要 polyfill | 轮询/上传/带超时的 SDK 不再挂死 | 已支持 |
| 页面生命周期事件（`pageshow` / `pagehide` / `visibilitychange` / `beforeunload` / `unload`） | **已支持（R106）**，顺序按 Chrome 132 实测对齐（`tools/chrome_lifecycle.py`）：导航/刷新时 `beforeunload` → `pagehide{persisted:false}` → `visibilitychange(hidden)` → `unload`，新文档 `load` → `pageshow{persisted:false}`；每个 frame 都有自己的 `pagehide`/`unload`（iframe 导航时旧 realm 先收尾再丢弃）；切 tab 只有 `visibilitychange`（`agave_webview_set_visible`）。**`navigator.sendBeacon` 没有**：在 `pagehide` 里发最后一份日志的页面会抛 `undefined is not a function`（`fetch(keepalive)` 也会被 realm 拆除取消） | 统计/埋点类库在卸载时静默丢数据 | 部分支持；sendBeacon 是缺口，见 [plan-roadmap.md](plan-roadmap.md) |
| `document.activeElement` / `hasFocus()` / `visibilityState` / `importNode` / `adoptNode` / `elementFromPoint` | **已支持（R34/R106）**：`visibilityState`/`hidden` 是真实状态——控件 `agave_webview_set_visible()` 切 tab 时更新并派发 `visibilitychange`（`macross_view_set_visible` → `AntRuntime::setViewVisible`），导航收尾时也会先转 `hidden` 再发 `unload`；`activeElement` 无焦点时回落到 `body` | 后台 tab 的页面现在能真正看到 `hidden` | 已支持 |
| `document.fonts` | **桩（R34）**：`{status:'loaded', ready:已 resolve, add/delete/check/forEach}`。没有 web font 加载，`ready` 立即兑现 | 用 `await document.fonts.ready` 的页面不再挂住；但 `fonts.load()` 不会真的加载字体 | 桩 |
| `navigator.clipboard.writeText` / `document.execCommand('copy')` | **writeText 已支持**（2026-11-07）：prelude polyfill → 宿主全局 `__agbClipboardSetText`（`AntRuntime.cpp`）→ 控件 `cb_clipboard_set_data`（`CF_UNICODETEXT`，与 Ctrl+C 同一条路，跑在引擎线程）。`write()` 恒 reject（没有 DataTransfer/Blob 路径）；`readText()` / `read()` 没有；`document.execCommand` 仍是 `undefined`（`AgbGapJsdom.h` 记录） | 站点「复制」按钮现在能真复制；读剪贴板（「粘贴」按钮）仍不可用 | 部分支持 |
| `window.open` 的引擎侧策略 | 可用，但**绕过** `FrameLoaderClientMC::dispatchCreatePage`，走控件 `agave_control_open_window` | 引擎级 new-window 策略/features 不生效；`[nav]` 里看到的是 `popup create`，不是 `popup open` | 管道差异，P2 |
| `el.onload = fn` 等事件处理属性 | **已修（R27）**：`hits=11`（属性 + addEventListener 都触发）；此前只有 `addEventListener` 生效，`onload` 是静默 expando | —— | 已支持 |
| HTML 属性事件 handler（`<body onresize="…">`、`onclick="…"`） | **会触发，但跑在遗留 KJS 世界**：handler 能读写 DOM（`document.documentElement.setAttribute(...)` 生效），但 `window.__x = 1` 页面脚本读不到（`undefined`）——两套全局。同一事件上 `addEventListener` 正常工作（R69 实测三种 resize 写法：前两条收到且能读页面全局，`<body onresize>` 也收到但看不到页面全局） | 依赖内联 handler 与页面 JS 交换状态的老站点 | **不推进**：现代站点用 `addEventListener`，`resize` 监听已支持。理由与将来的修法（`Document::createHTMLEventListener()` 改走 AntRuntime）见 [plan-roadmap.md](plan-roadmap.md)「P2 不推进」 |

### 1.1 连 shim 都没有（feature detect 会走 fallback）

`ResizeObserver`、`IntersectionObserver`、`PerformanceObserver`、`indexedDB`、`caches`、`Notification`、
`geolocation`、`FileReader`、`customElements`、`ShadowRoot`/`attachShadow`、`OffscreenCanvas`、
`AudioContext`/`speechSynthesis`、`devicePixelRatio`、`print`、`Temporal`、`WebGL`。

**媒体元素（`<video>`/`<audio>`/`<source>`/`<track>`）已有实现**：元素类、常用 API、
容器头元数据（MP4/WebM/MKV）、`textTracks` + WebVTT cue，尺寸与 Chrome 对齐；
**真实播放**用 ffmpeg + waveOut（音频出声、视频逐帧绘制、时钟/暂停/seek/ended 都由媒体时钟驱动，
没装 ffmpeg 时退回内部定时器模拟）。
**仍未实现**：渐进式/流式播放（远程媒体先整段下载）、`object-fit` 裁剪、音画严格同步、
`MediaSource`(MSE)、EME、WebAudio、原生控件皮肤、字幕绘制、`requestVideoFrameCallback`。
详见 [arch-media.md](arch-media.md)，复测 `pwsh -File tools/run_media_tests.ps1`（API/几何）
与 `pwsh -File tools/run_media_real_tests.ps1`（真实播放）。

**`<canvas>` 部分可用**：`fillRect`/路径/颜色能画（`debug/canvas_fill.html` 像素验证），
但 **`drawImage(<img>)` 画不出东西**、`getImageData()` 也不存在（`undefined`）——
`CanvasRenderingContext2D::drawImage` 走到 `GraphicsContext::drawImage` 之后没有任何像素落地，
与 R138 的异步解码无关（关掉 `AGB_IMG_ASYNC` 同样不画；canvas 本来就走内联同步路径）。
`fillText`/`measureText` 仍缺（见 [plan-roadmap.md](plan-roadmap.md) 的 canvas 项）。
复测页：`debug/canvas_drawimage.html`（`big_image.jpg` 画进 canvas）。

`Audio` 全局构造器在 `prelude.js` 里：`new Audio(src)` 返回真正的 `<audio>` 元素
（此前是 `undefined`，Howler/video.js 这类库会在碰 DOM 前就抛错）。

`WebSocket`/`EventSource`/`BroadcastChannel` 存在（ant 原生，走 libuv），但浏览器里没有对应的 socket
事件泵；构造不抛错、连接行为未验证，属于"探测通过但连不上"的静默失败。

定位锚点（已修项只给锚点，不再列"差什么"）：

- 集合 / 表单 / 图片 / 文本 / 反射属性 / 元素树 / `activeElement`：R34，`AntRuntime.cpp` 的
  `titleGetter` / `nodeGetter` / `g_nodeClass` / `documentObjectFor`。
- 动态建脚本/link 的 IDL 反射（R108）：`s.type/.rel/.media/.charset/.as` 字符串落属性、
  `s.async/.defer` 真布尔（falsy 移除属性、`=== true` 可判）；`AntRuntime.cpp` 的
  `nodeGetter` / `nodeSetter`，桥侧布尔判定/构造 `antb_is_boolean` / `antb_make_boolean`。
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
| `<select>`（menu list）：点开下拉、`value`/`selectedIndex`/`options`/`length`/`type`、`option.value`/`text`/`index`/`selected`、`change` | **已支持（R133）** | 菜单由引擎画在 view 自己的 surface 上（无宿主窗口），点击/方向键/回车/ESC 都可用，`--probe --click` 可回归（`tools/pages/form_select.html`）。**仍缺**：type-ahead（键入字母跳转）、滚轮滚菜单列表、贴近视口底部时向上翻转、`size>1`/`multiple` 的 list box 未回归 |
| `change` | 支持（失焦时） | 未在测试页覆盖（需要 blur 用例） |
| `paste`/`cut`/`copy` | 合成事件可派发；**写出**有路：`navigator.clipboard.writeText`（见 §1）、Ctrl+C 走 `Editor::copy()` → Pasteboard → `cb_clipboard_set_data` | `ClipboardEvent` 对象没有（事件里拿不到 `clipboardData`），`execCommand('copy')` / `readText()` 仍缺 | 部分 |
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
| `align-self` | 被忽略 | **不支持**：`RenderStyle` 没有 align-self，子项一律按 `align-items`；`align-self:flex-end` 的元素会被 stretch 成整列高（css3test `aside` 因此比 Chrome 高）。`getComputedStyle(el).alignSelf` 有访问器，但返回 `""`（Chrome 是 `auto`） |
| flex 的 computed 值 | `getComputedStyle(el).flex` / `.flexGrow` / `.flexShrink` / `.flexBasis` 都有值；`display` 仍是 `-webkit-box` | **部分**：R115 补齐了四个 flex 计算值（见下面那行），但 `display:flex` 的 computed 仍回 `-webkit-box`（`modernKeywordID()` 是单向映射）。影响：用 `getComputedStyle(el).display === 'flex'` 做能力检测的库会误判。修法方向：computed 序列化里把 `BOX`/`INLINE_BOX` 吐成 `flex`/`inline-flex` |
| `align-content: space-between/center/flex-end` | 行间分布生效 | **已支持（R54）**：默认 `stretch` 把单行撑满容器（`align-items:center` 因此在整个盒子里居中，与 Chrome 一致）；`space-around` 近似成 `space-between` |
| `scrollbar-width: none` / `-ms-overflow-style: none` | 滚动条不占宽 | **已支持（R54）**：`RenderStyle::scrollbarNone()`；文档滚动条在 `FrameView::applyOverflowToViewport()`、元素滚动条在 `RenderLayer::updateScrollInfoAfterLayout()` 里不占空间 |
| `::-webkit-scrollbar{display:none}` / `{width:0}` | 不占滚动条宽度 | **已支持（R56）**：解析期把这类规则改写成 `scrollbar-width:none`（`rewriteWebkitScrollbarRules()`，@media 内也处理）；`-thumb/-track/-button/-corner/-resizer` 规则整条丢弃（原生滚动条本来就画不了这些样式），不影响滚动本身 |
| `position: sticky` | `sticky`（computed 也返回 `sticky`） | **已支持（R82）**：`top`/`right`/`bottom`/`left`（含 `%`）单轴或双轴；最近滚动容器可以是 overflow 盒或文档视图；绘制/命中/几何三处都随滚动平移，滚动时整视口重绘（不 blit）。复测：`tools/run_sticky_tests.ps1`（11 条几何 + 4 像素）。**限制**：`scrollIntoView()` 仍是 no-op；sticky 建 layer 参与 stacking 但没有 z-index 细化 |
| `aspect-ratio: 16/9` + `width:160px` | `height=90px` | **已支持**（R41）：宽或高为 auto 时按比例推另一轴（按 content box），`16 / 9` 与 `16/9` 都认 |
| flex `gap: 8px` | `8px` | **已支持**（R41）：`gap`/`row-gap`/`column-gap`，简写 1 值=两轴、2 值=row col；flex 仍是 2008 的 `-webkit-box` 体系，但间距按现代语义插在 in-flow 子项之间。简写的 `inherit`/`initial` 也生效（R114；此前会穿透到 SVG 断言） |
| flex 计算值：`getComputedStyle(el).flex` / `.flexGrow` / `.flexShrink` / `.flexBasis` | 与 Chrome 一致：`flex:2 3 40px` → `"2 3 40px"` / `2` / `3` / `"40px"`；`flex:1` → `"1 1 0%"`；`flex:auto` → `"1 1 auto"`；`flex:none` → `"0 0 auto"`；`flex-basis:0` → `"0px"`；`calc(50% - 4px)` → `"calc(50% - 4px)"` | **已支持（R115）**：`flex`/`flex-shrink`/`flex-basis` 进 computed switch（`flex` 按 grow shrink basis 序列化），`flex-grow` 走 `-webkit-box-flex` 别名；prelude 的 `__computedCamel` 补了四个 camelCase 属性；`flex-basis: 0`（无单位零）以前被当作非长度丢掉，现在按 `0px`；`flex-basis: calc(百分比 ± px)` 进 `propertyAcceptsCalcLength()` 白名单。⚠ 限制：百分比按 `Length` 的定点存（R118 起 1/10000，`33.333%` 读回 `33.333%`；R115 时是 1/128 → `33.3281%`）；`flex: 1 1 calc(...)` 简写也支持（R116，`parseFlexShorthand()` 解码编码 token）；`getComputedStyle()` 对象没有 `cssText`（与本条无关的既有缺口） |
| `:is(p)` / `:where(p)` / `:not(:is(...))` / `:-webkit-any(...)` | 与 Chrome 同 | **已支持**（R41 + R121）：解析期把 `:is()`/`:where()` 文本展开成选择器列表（`p:is(.a,.b) span` → `p.a span, p.b span`）；R121 起还会把 `:not(:is(a, b))` / `:not(:where(a, b))` / `:not(:-webkit-any(a, b))` 按德摩根展开成 `:not(a):not(b)`，并把 `:-webkit-any(` 归一成 `:is(`（生成式 CSS 用 `:not(:is(:lang(ae), …))` 表达逻辑属性，以前整条规则被 2008 语法拒绝、声明静默丢失）。**限制**：参数仍必须是简单选择器（带组合器不展开）；只处理 `:not()` 里**恰好只有一个** functional pseudo 的形式 |
| 表单控件外观（`-webkit-appearance: menulist` / `textfield` / `button` / `checkbox` / `slider`） | 浅青填充 `#E6FFFF` + 青绿描边 `#40BFBF`、5px 圆角 | **与 Chrome 不同**：`src/webcore/platform/picasso/RenderThemePS.cpp` 的 `paintMenuList` / `paintMenuListButton` / `paintTextField` / `paintButton` / checkbox / slider 全部沿用 MaCross 示例配色（`ps_color s = {0.25, 0.75, 0.75}`、`f = {0.90, 1.00, 1.00}`，第 325/350/385/497/517/540 行），而 Chrome 是浅灰 `#f8f8f8` 填充 + `#767676` 边框 + 深色箭头、2px 圆角。没有被页面 CSS 覆盖的 `<select>` / `<input>` / 按钮因此发青（css3test 的 Filter 下拉就是）。修法方向：只改这几处配色/圆角，不动布局 |
| `getComputedStyle()` 的**简写**：`margin` / `padding` / `border` / `border-width|style|color` / `border-top…` / `border-radius` / `background` / `font` / `list-style` / `overflow` / `outline` | 与 Chrome 逐字一致（空值/四值折叠/椭圆半径都对）：`margin:"1px 2px 3px 4px"`、`border:"2px solid rgb(10, 20, 30)"`（四边不同时给 `""`）、`border-radius:"4px 6px 8px 10px / 1px 2px 3px 4px"`、`background:"rgb(255, 0, 0) none no-repeat scroll 1px 2px / auto padding-box border-box"`、`font:"italic 700 13px / 19.5px Arial"` | **已支持（R124）**：`CSSComputedStyleDeclaration::shorthandValue()` 从 longhand 重算并按 CSSOM 折叠；此前这些 id 要么没有 case（返回 `""`），要么被错误地和 `content` 分到一组（`getComputedStyle(el).border` 返回 `"normal"`）。prelude 的访问器改为由 `__agbStylePropertyNames()`（`StylePropertyNames.h`）生成，不再靠手写白名单。复测：`tools/pages/cap_computed.html` + `check_computed.js`（40 条断言，接在 `run_layout_tests.ps1` 的 computed 组） |
| computed 值仍与 Chrome 不同的项 | `alignSelf:""`、`order:""`、`boxShadow:""`、`inset:""`、`gap:"0px"`、`transition:""`、`font-family` 只有第一个家族 | **缺口（R124 之后）**：①`inset`/`transition` 没有属性 id（`inset` 连解析都没有，`transition` 引擎不实现）；②`align-self` / `box-shadow` / `order` 有 id 但 computed switch 没有分支（`box-shadow` 只在绘制里实现）；③`gap` 的 initial 存成 `0` 分不出 `normal`；④`font-family` 只存第一个家族（`RenderStyle` 的 `FontFamily` 是单值，见 R123 的字体替换条）；⑤`display:flex` 仍报 `-webkit-box` |
| 百分比求值取整（`width:33.333%` vs `width:calc(33.333%)`） | 两条路径同值：800px 容器都是 `267px`（`round(266.664)`）；Chrome 是 266.656（分数像素，最后绘制才栅格化） | **已统一（R117）**：`Length::calcValue()` 的 `Percent` 与 `Calculated` 都走 `double` + 四舍五入（以前 Percent 整数截断，同一作者值会差 1px）。⚠ 百分比按 1/10000 定点存储（R118；`33.333%`/`33.336%`/`0.1%` 读回原值，`33.333%` 在 R115 时是 `33.3281%`）；`calcMinValue()` 与 `AutoTableLayout`/`RenderTableSection` 的表格百分比数学仍是整数截断（已改 64 位防溢出，取整方向未统一） |
| `var(--missing, rgb(7,8,9))` | `rgb(7, 8, 9)` | 支持（fallback 正常）；**级联/继承已按规范工作**（R39：元素级覆盖、`!important`、继承都对，`getComputedStyle(el).getPropertyValue('--x')` 可读）；`calc()` 内的 `var(--常量token)` 在解析期代入（R43）；仍未做：`@property`、混合单位的 `calc()` 求值 |
| `box-sizing: border-box` | `border-box` | **已支持**（R39）：无前缀名此前整条被丢（只有 `-webkit-box-sizing` 有词条），现在走别名表；布局按 border-box 计算，`getComputedStyle(el).boxSizing` 可读 |
| `@media (prefers-color-scheme: dark)` | **默认 `light`**（不跟随系统）；`AGB_COLOR_SCHEME=dark` 时匹配 | **已支持**（R39，默认值见 R86）：`matchMedia` 与 CSS 用同一答案（`prefersDarkColorScheme()`）。**默认不跟随系统深浅色** —— 宿主不再按 `AppsUseLightTheme` 注入（那会让所有带 dark 块的站点在深色系统上整页变黑，css3test 就是例子）；要看系统值用 `AGB_COLOR_SCHEME=auto`（读注册表），或显式 `dark`/`light` 钉死 |
| `matchMedia('(min-width: …)')` | `matches` 正确 | **已支持**（R39）：width/height/device-*/orientation/resolution/prefers-* 都按真实视口求值，`not`/`only`/媒体类型/逗号列表也处理；未做：`addListener` 的变更回调（返回的 list 是静态快照） |
| 窗口 resize（`--resize WxH` / 拖窗口；1100x800 → 500x400） | `inner=500x400`、`pct=250`（50%）、`vwBox=500x200`、`calcBox=400x150`、`mediaCSS=rgb(200, 30, 40)`（移动端断点）、`mediaCSSwide` 回到基准色、`matchMedia=true`、三种 `resize` 监听都收到 `500x400` | **全部跟随（R69）**：`WM_SIZE → AGW_CMD_RESIZE → macross_view_resize` 走 `FrameView::resize`；`FrameView::layout()` 发现 `styleViewportSize` 变了就先 `Document::viewportSizeChanged()` + `updateStyleIfNeeded()`（selector 重建 → `@media` 按新视口重滤；vw/vh 变量换成新值），再正常布局并按尺寸变化全量重绘（放大到 1400x900 新区域是白的）。`resize` 事件在布局后由 `Frame::sendResizeEvent()` 派发，Ant 侧由 `AntRuntime::dispatchResizeEvent()` 送到页面的 `addEventListener("resize")`/`window.onresize`。代价：每次尺寸变化一次全文档样式重算 + 重排（拖动时按帧合并）。残留：`<body onresize>` 这类 HTML 属性 handler 跑在旧的 KJS 世界，读不到页面 JS 全局 |
| `@media (min-width: 1px)`（无媒体类型） | 生效 | **已支持**（R39）：`rewriteBareMediaQueries()` 补 `all and `；2008 的语法要求媒体类型在前，否则整块丢弃 |
| `align-items: safe center` | `center` | **已支持**（R42）：解析期删掉 `safe`/`unsafe` 溢出关键字，保留对齐关键字；否则两 token 会让整条声明被丢、`align-items` 退回 `stretch` |
| `width: unset`（CSS-wide keyword） | `width: auto`；图片按固有比例，150×51 的图在 `height:17px` 下是 **50×17**（百度 guide-bub 正是 `img{width:16px}` + 更高优先级 `img{width:unset}`） | **已支持（R78）**：`inherit`/`initial` 一直有；`unset` 没有进生成的关键字表，解析期按文本识别（`CSSParser::parseValue()`），继承属性→`inherit`、其余→`initial`（`isInheritedProperty()` 覆盖本引擎实现的属性，未知属性按非继承处理）。**未做**：`revert`/`revert-layer` —— 需要级联来源信息才能回滚到 UA/user 源，按丢弃声明处理 |
| `document.styleSheets` / `sheet.cssRules` / `<style>.sheet` | baidu 首页 `sheets=32 rules=3216`；deepseek 登录页 `sheets=4 rules=2006`；同一对象重复读取 `===` 成立；`insertRule`/`deleteRule` 后 `cssRules.length` 与级联立即变化 | **已支持（R79）**：live wrapper（与 node/canvas 同一套 `antb_wrap_native_as_owner` + class 模型，方法挂在 class 上，document 卸载时释放）；`document.styleSheets` 在文档对象的统一 getter 里分发，`style/link.sheet` 在 `nodeGetter` 里按 `ownerNode` 查找。rule 的 `type`/`selectorText`/`cssText`/`style`/`media`/内层 `cssRules`、`@font-face`、`CSSStyleDeclaration.getPropertyValue/getPropertyPriority/setProperty/removeProperty` 都可读/可改。**限制**：①不区分同源/跨域，跨域样式表的 `cssRules` 也能读（Chrome 会抛 `SecurityError`）；②`sheet.cssRules` 每次访问返回新的 `CSSRuleList`（WebCore 2008 的实现），缓存该列表后看不到后续 `insertRule`，重新读取即可；③`<style>` 无论在 `<head>`、body 里（解析器会搬到 head）还是脚本 appendChild 到 body/`<html>`（R90 起都进样式表，以前遍历在 `<body>` 停） |
| `CSS.supports('selector(…)' )` | `selector(::first-letter)` → `true`，`selector(!!!)` → `false` | **已支持（R91）**：`selector()` 参数交给真正的选择器解析器（临时 `<style>` 里包一层不匹配的 `@media` 解析，读完摘掉元素，`document.styleSheets` 不留空表）。以前按"名字:值"解析，被第一个冒号切碎 → 恒 `false` |
| `@page` / `@page :left { margin: 2cm }` | CSSOM 里是 `PAGE_RULE`（`type=6`），`selectorText=":left"`，`style` 可读（`margin-left: 2cm`） | **已支持（R91）**：2008 语法只有 error 分支，解析前把块改写成 `agb-page-<n>` 占位规则（位置不变），解析完原地换成 `CSSPageRule`。**限制**：嵌在别的 at-rule 里的 `@page` 仍是占位符（规则丢失）；页边盒子（`@top-left {}` 等，CSS3 Paged Media）未实现；引擎没有分页布局，规则只存在于 CSSOM |
| `content: open-quote` / `quotes: "«" "»"` / `text-decoration: blink` | 都能解析（`el.style.getPropertyValue()` 有值），**渲染不实现** | **部分支持（R91）**：`content` 接受 `normal`/`none` 与四个 quote 关键字（`ContentData` 没有 OPEN_QUOTE 类型，渲染忽略）、`quotes` 新增 `parseQuotes()`（`CSSStyleSelector::applyProperty` 里 `CSS_PROP_QUOTES` 是显式 no-op，R114 加了 `return;`；此前 SVG 打开后会掉进 `applySVGProperty()` 的未实现断言，css3test 因此崩）、`blink` 保留但不闪 |
| 旧版 SVG 表现属性 `color-rendering` / `color-profile` / `enable-background` / `marker` | 不渲染；`color-rendering`/`color-profile` 的 ident 值仍会解析并留在 CSSOM | **不支持（按浏览器忽略）**：`RenderStyle`/`SVGRenderStyle` 没有对应字段，现代浏览器也已移除这些属性。R114 起不再触发 `applySVGProperty()` 的崩溃断言；`AGB_CSS_DEBUG=1` 时打 `unimplemented SVG propertyID: N` 便于发现其他未映射属性 |
| column flex 里的 `position:absolute` 子项 | 按 `align-items` 定 staticX | **已支持**（R42）：`RenderFlexibleBox::layoutVerticalBox()` 的 positioned 分支按 `boxAlign()` 修正 static position（`BCENTER` 居中、`BEND` 右对齐）；此前无条件贴左 |
| `iframe.frameBorder = "0"`（JS property） | `border-top-width: 0px` | **已支持**（R42）：`kReflectedAttributes` 加 `frameBorder→frameborder`，property 写入落到 `setAttribute()` → `parseMappedAttribute()` 的 `border-width:0` 呈现提示；静态 `frameborder="0"` 一直可用 |
| 8 位 hex `#RRGGBBAA` / `#RGBA` | `rgba(138, 71, 38, 0.945)` | **已支持（R51）**：`Color::parseHexColor()` 认 4/8 位；声明值与样式表里的 8 位 hex 还会在解析前改写成 `rgba()`（旧 tokenizer 只认 3/6 位）。alpha 是 8 位量化，序列化按 Chrome 的习惯给两位有效数字（`0.5`/`0.25`） |
| CSS Color 4 色彩函数：`hsl(200 60% 20%)`、`rgb(1 2 3)`、`hsl(... / .5)`、`rgb(8% 24% 32%)`、`hsl(200deg ...)` | 与 Chrome 逐字节一致（`hsl(200 60% 20%)` → `rgb(20, 61, 82)`） | **已支持（R83）**：2008 只认逗号形式，`parseColorParameters`/`parseHSLParameters` 改成"逗号或空格、alpha 用 `,` 或 `/`"的单循环；hue 支持 `deg`/`rad`/`grad`；hsl→rgb 改四舍五入（旧实现 `0.32*256` 截断成 81，Chrome 82）。**限制**：`turn` 单位没有 `CSS_TURN`（tokenizer 不接受，声明被丢）；alpha 8 位量化，`/ 50%` 序列化成 `0.5`、`/ 0.25` 序列化成 `0.25`（与 Chrome 一致），但 `0.125` 会变 `0.13` |
| `background: url() 0 0 / 16px 16px`、`center / cover`、`background-size: contain` | `background-size` = `16px 16px` / `cover` / `contain`；`background-position` 与 Chrome 一致 | **已支持（R84）**：简写的 `properties[]` 补出 `-webkit-background-size`，循环跳过 `/` 分隔符（以前整条声明被丢）；`cover`/`contain` 的 computed 以前返回空串，现在原样输出文本（新增 `CSS_IDENT_TEXT` 单位）；两个分量都是 auto 时只输出 `auto`。复测：`tools/pages/layout/background_shorthand.html`（12 case 对拍 Chrome）、`background_paint.html`（7 像素，接进 `run_layout_tests.ps1`） | 已支持 |
| `background: url(...) var(--color)`（色从变量来） | 与字面量色一致：图 + 底色都画 | **已修（R85）**：`var()` 展开的声明由 `applyExpandedProperty()` 用全新 `CSSParser` 重解析，以前没有 `styleElement`，`parseBackgroundImage()` 里 base 为空 → 相对 URL 变**空**（`[cssimg] url=[]`）→ 第二个永不加载的 `CachedImage`。现在 `CSSParser::setValueDocument()` + `document()->baseURL()` 兜底，展开后的 URL 与源声明完全相同（绝对 URL 本来就不受影响）。复测：`background_paint.html` 的 `#varrel`（相对 URL + `var(--tint)`）2 像素 |
| `data:` URI 图片 | 正常绘制（`<img src="data:…">`、`background: url(data:…)`） | **已修（R85）**：`ResourceHandleManager::add()` 在 file:// 之前加 data: 分支，本地解析 `data:[<mediatype>][;base64],<payload>`（base64 + 百分号解码，默认 `text/plain;charset=US-ASCII`），走与 cache hit/file 相同的异步 `CachedDelivery`。复测：`background_paint.html` 的 `#data`/`#dataimg` 2 像素 |
| 自定义属性：computed 序列化 + 程序化读写 | `getComputedStyle(root).getPropertyValue('--x')` 返回作者拼写；`el.style.setProperty('--x', v)` / `getPropertyValue` / `removeProperty` / `cssText` 都可用 | **已修（R87）**：以前读回来的是内部名 `-agb-…`（值里的 `var(-agb-y)` 也是），而且 name 版 `setProperty('--x')` 因 `propertyID('--x') == 0` 被静默丢弃。现在 `restoreCustomPropertyNames()` 在读/序列化时还原，`CSSMutableStyleDeclaration::setCustomProperty/customPropertyValue/removeCustomProperty` 支撑 name 版 API，`valueText()` 不再把逗号写成 `" , "`（那会让 `--tint: rgb(1,2,3)` 当中转值失效）。复测：`tools/pages/layout/custom_props.html`（对拍 Chrome，`data-props` 直接写 `--tint,--pad`）+ `check_custom_props.js`（接在 `run_layout_tests.ps1`）。**仍与 Chrome 不同**：`getComputedStyle('--alias')` 返回原文 `var(--tint)`，Chrome 会代换一层 |
| `min-height: calc(18px + 4px)` | `22px` | **已支持**（R43）：解析期把纯常量的 `calc()` 折叠成单值（`+ - * /`、括号、绝对长度单位）；自定义属性值里的 `calc(var(--token) + 4px)` 先做 sheet 级常量代入再折叠。**限制**：含 `%`/`vw`/`vh`/`em`/`rem`/`env()` 的混合单位 calc 仍不支持（保留原样、声明被丢），需要布局期求值才能覆盖 `calc(100% - 24px)` 这类 |
| `calc(100% - 130px - 1em)` / `calc(100% - 12em - 130px)`（混合百分比 + 字体单位） | 与 Chrome 一致：`max-width` = `calc(100% - 142px)` / `calc(100% - 274px)` | **已支持（R88）**：样式表解析时没有元素字体，这类 calc 会十六进制编码成 ident（`agbfontcalcx…`）留成 raw value，渲染该元素时再用 `style->fontSize()` 折算成既有的 `CSS_CALC_LENGTH`（percent+px）。`em` 按元素字体、`rem` 按根元素、`ex`/`ch` 近似 0.5em。⚠ 修前整条声明被丢（`max-width: none`，浮动元素因此换行）。复测：`tools/pages/layout/calc_terms.html`（7 case 对拍 Chrome）、`float_carbon.html` |

| `indexedDB.open(...)` | 请求异步 `onerror` | **桩实现（R57 诊断）**：`window.indexedDB` 不存在时直接引用会抛 `ReferenceError`，所以提供 open/deleteDatabase/databases 都异步失败的桩 + `IDBKeyRange`；站点走“失败回退内存存储”的分支，而不是在 Promise 链里炸掉。真正的 IDB 存储未做 |

| `border-radius: 50%`（百分比半径） | 期望圆/椭圆；当前画成**方角** | **不支持**：`RenderStyle` 的四个角半径存的是 px `IntSize`，百分比在解析期就丢了（px 半径正常；半径和超过边长时按 CSS 缩小，见 R120）。本地 `debug/radius_test.html` 的 64×64 `#g` 复现。要支持得把半径改成 `Length`、布局后按盒子尺寸解算，再让 `scaledBorderRadii()` 接百分比结果。⚠ R125 起 **px 的四值/椭圆简写**（`8px 8px 0 0`、`4px / 2px`）能正确解析成四个角，以前被当成单个角的 h/v |

| 字体选择与字体盒（`font-family` 回退、`line-height: normal`） | 行盒 = hhea 的 asc/desc/lineGap 各自四舍五入后相加（与 Chrome/Skia 一致） | **部分支持（R123）**：字体盒已按字体文件度量（YaHei 12/14/16/20 → 16/19/21/26，与 Chrome 逐项一致），`line-height: var(--x)` 也会在字体确定后展开。⚠ 两个已知缺口：①`FontCachePS::createFontPlatformData()` 把 arial/segoe ui/times/… **整个家族**替换成 Microsoft YaHei（为 CJK 缺字），所以西文也用雅黑渲染（“字显得大”的根因）；`AGB_FONT_NO_SUBST=1` 是临时对照开关，关掉后十组字体与 Chrome 全对，但混排行盒只按**主字体**度量（12px 中文 16 vs Chrome 20，需要按 run 的 fallback 字体取 max），且默认字体是 Times 而不是本机 Chrome 的 **Noto Sans SC**（calc_terms 因此仍差 3px）。②没有 web font 加载（`document.fonts` 只是桩） |
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
5. 滚动条（R132）：根因不是布局，而是子 frame 的 `location.href` 读到了父页，微信页拿不到
   `stylelite=1`，回退到 280px 旧 UI；修好后选 160px 新 UI，`clientWidth == scrollWidth == 200`。
   仍未完成：子 frame 的跨文档 `history.back/forward` 仍是 no-op（只保证 `length`/`state`/
   `pushState` 独立）；`parent` 仍转发到父 realm 的 global，不是完整 WindowProxy（S4）。



