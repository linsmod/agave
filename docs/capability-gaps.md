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
```

退出码 0 且 `[exit]` 行就是当前值。要加一项探测：在 `cap_dom.html` / `cap_css.html`
的 `document.title = [...]` 里加一行，再在本页加一行表格。

**这些是存量缺口，不是回归**：页面行为不对时先查本页，别先怀疑页面或最近的提交。

## 1. JS / DOM

`cap_dom.html` 当前输出：

```
forms=0 links=0 images=0 anchors=0 scripts=0 all=13
formSubmit=undefined formAction=undefined
imgComplete=undefined imgNatural=undefined imgWidth=undefined
insertAdjacentHTML=undefined innerText=undefined
classListAdd=function matches=function closest=function textContent=string
```

`cap_selectors.html`：`attr=bar ds=bar qsAttr=0 qsClass=1 qsClassAttr=0 qsTagAttr=0`
`cap_script.html`（R27 修复后）：`hits=11 child=loaded`
`cap_postmessage.html`：`postMessage=undefined sent=ERR TypeError: undefined is not a function got=0`

| 能力 | 当前值 | 影响 | 判定 / 建议 |
|---|---|---|---|
| `document.forms` / `links` / `images` / `anchors` / `scripts` / `embeds` / `plugins` / `applets` | 全是长度 0 的数组（`getElementsByTagName('*')=13` 正常） | `document.forms[0].submit()`、用 `document.scripts` 找入口的打包器、用 `document.images` 预加载的代码会**静默走空分支** | 缺口，P1 |
| `HTMLFormElement.submit()` / `.action` | `undefined`；点 submit 按钮能提交（WebCore 默认动作） | 程序化提交、读/改 action、表单库的 `.submit()` 路径失效 | 缺口，P1 |
| `history.pushState` / `replaceState` | 是函数，但**只改 document URL**：`state=undefined`、`len=undefined`、`pops=0`、`href` 被改写 | history 模式的 SPA（Vue/React Router）无法保存/恢复 state，收不到 `popstate`，前进后退会跳过视图 | 半实现，P1 |
| `localStorage` 跨文档 | A 页写入 → 导航到 B → `null`；同一跑的 cookie 正常（`cookie=true`） | 用 localStorage 存 token/购物车的多页流程会丢状态；测试也不能拿它当跨页标记（用 cookie） | 缺口，P1 |
| `window.postMessage` / `message` 事件 | `undefined`，调用直接抛 `TypeError`；同窗口/跨窗口都收不到 | 跨 frame 组件无法通信：DeepSeek 的微信扫码 iframe 用 `postMessage` 报 `wxReady`/扫码状态，父页因此永远等不到 ready（二维码区域空） | 缺口，P1 |
| `document.querySelector(All)` 的属性选择器 | `[data-bar]` / `[class*=probe]` / `div[data-bar]` 全部 `0`；`.probe-cls` 为 `1` | 用 `querySelector('script[data-x]')` 做幂等、`[data-state]` 做 UI 分支的代码全部走空分支；不报错 | 缺口，P1 |
| `HTMLImageElement.complete` / `naturalWidth` / `width` | 全 `undefined`（图片实际能加载/绘制） | 图片预加载、`img.onload` 之外的完成判定、二维码/图标尺寸计算失效 | 缺口，P2 |
| `Element.insertAdjacentHTML` | `undefined` | jQuery、统计/广告脚本常用；有特性检测的会降级，没有的直接抛错 | 缺口，P2 |
| `HTMLElement.innerText` | `undefined`（`textContent` 正常） | 老代码/测试断言常用 | 缺口，P2 |
| `window.open` 的引擎侧策略 | 可用，但**绕过** `FrameLoaderClientMC::dispatchCreatePage`，走控件 `agave_control_open_window` | 引擎级 new-window 策略/features 不生效；`[nav]` 里看到的是 `popup create`，不是 `popup open` | 管道差异，P2 |
| `el.onload = fn` 等事件处理属性 | **已修（R27）**：`hits=11`（属性 + addEventListener 都触发）；此前只有 `addEventListener` 生效，`onload` 是静默 expando | —— | 已支持 |

定位锚点：

- 集合空壳：`AntRuntime.cpp` 的 `documentGetter` 里 `"forms"`/`"links"`/… 分支
  （注释原文 “Collections are exposed as empty arrays … the real NodeLists are not wrapped yet”）。
- History：`AntRuntime.cpp` 的 `historyPushState`（注释 “only rewrites the document URL … without a
  real history entry”）；`historyBack/Forward` 直接调 `page()->goBack()/goForward()`；
  `popstate` 只在 `prelude.js` 的 `onpopstate` 名字表里出现，没有任何派发点。
- 表单方法/属性：桥接类表（`AntBridge.c` 的 wrapper 方法表 + `AntRuntime.cpp` 的类属性表）没有
  `HTMLFormElement` 的 `submit`/`action` 等条目。
- storage：`AntRuntime.cpp` 的 storage 绑定；写入在文档 teardown 落盘、读取只在 engine 重建时发生
  （跨进程能读到旧值，同进程导航读不到）。
- `window.postMessage`：`prelude.js` 只有 Worker 的 `postMessage` 和宿主桥 `agavePostMessage`
  （页面→壳），没有标准 `window.postMessage`/`message` 事件；`onmessage` 只出现在事件名表里。
- 属性选择器：`AntRuntime.cpp` 的 `nodeQuerySelector` / `nodeQuerySelectorAll` 只把简单选择器交给
  WebCore；`[attr]` / `[attr=v]` / `[class*=]` / `div[attr]` 都返回空（`.class`、`#id`、tag 正常）。
- 图片属性：`nodeGetter` 没有 `HTMLImageElement` 的 `complete`/`naturalWidth`/`width`/`height` 分支。
- 事件处理属性（已修）：`AntRuntime.cpp` 的 `nodeSetter`/`nodeGetter` `on*` 分支 +
  `setNodeEventHandler()`/`findNodeEventHandler()`（R27）。

## 2. CSS

`cap_css.html` 当前输出：

```
grid=block transform=[] boxShadow=rgb(255, 0, 0) 0px 0px 4px sticky=static
aspectHeight=20px gap=undefined isColor=rgb(0, 0, 0) whereColor=rgb(0, 0, 0) varColor=rgb(7, 8, 9)
```

| 特性 | 当前 computed | 判定 / 建议 |
|---|---|---|
| `display: grid` | `block` | **不支持**：`CSSParser::modernKeywordID()` 明确把 grid 降级成 block（`CSS.supports('display','grid')` 也返回 false）；P1 |
| `transform: translateX(10px)` | `""` | **不支持**（含 `-webkit-transform` 的读取与绘制）；动画/位移类站点影响大；P1 |
| `box-shadow: 0 0 4px red` | `rgb(255, 0, 0) 0px 0px 4px` | **已支持**（R11 把 `box-shadow` 别名到 `-webkit-box-shadow`）；旧文档里“非 `-webkit-` 未实现”已过时；绘制未逐像素验证 |
| `position: sticky` | `static` | **不支持**，按 static 布局；P2 |
| `aspect-ratio: 16/9` + `width:160px` | `height=20px`（期望 90px） | **忽略**该属性；P2 |
| flex `gap: 8px` | `undefined`（computed 里没有该属性） | **不支持**；flex 实为 2008 的 `-webkit-box` 体系（R11）；P2 |
| `:is(p)` / `:where(p)` | `rgb(0, 0, 0)` | **选择器不解析**；P2 |
| `var(--missing, rgb(7,8,9))` | `rgb(7, 8, 9)` | 支持（fallback 正常）；解析期展开的局限见 R11：跨 sheet 失效、同名取首次定义、无元素级作用域 |

已确认可用的（别再当缺口重开）：块/inline-block/float/绝对定位/margin auto/padding/border-radius/
opacity/渐变（R11）、`var()` 含 fallback、`classList.add/toggle`、`matches`、`closest`、`dataset`、
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
4. DeepSeek 页内这块仍然空白（iframe 元素存在、尺寸样式 200×200，但页面里看不到）。
   待验证的下一层：微信组件靠 `window.postMessage` 给父页发 `wxReady`/扫码状态，而
   `window.postMessage` 目前不存在（§1）；父页可能因此不把二维码容器显示出来。
   补 `window.postMessage` + `message` 事件后要回来复测这一节。
