# CSS 多背景层与逐层渐变（最小标准子集）

> 设计记录（2026-11-12；已实现，现状见 [arch-rendering.md](arch-rendering.md) §6 与
> [arch-capability-gaps.md](arch-capability-gaps.md)）。本文只记录当时的问题、子集边界与取舍。

## 问题

百度首页的文心输入框（`#chat-input-main`）用标准 CSS 画“渐变描边”：

```css
#chat-input-main .chat-input-wrapper-border {
    border: 1.5px solid transparent;
    background: linear-gradient(white, white) padding-box,
                linear-gradient(160deg, #3377FE, #4C6FFF, #8370FF, #BA59FF) border-box;
}
```

两条背景层各自带 `<box>`（origin/clip）：上层的白色只铺 padding-box，下层的渐变铺
border-box，于是只有 1.5px 的透明边框处露出渐变，形成圆角渐变描边。

agave 的 `BackgroundLayer` 本来就是链表（多图/逐层 clip 都能画），但**渐变被存成
`RenderStyle` 上的单个 `m_backgroundGradient`**：`background` / `background-image`
里的每个 `linear-gradient()` 都覆盖同一个字段，绘制时只在最底层用这个渐变铺满
整个 border box。结果：

- 只留下最后一个渐变，白色内层消失；
- 渐变按最底层的 clip 绘制，上层的逐层 clip 无效；
- computed `background-image` / `background-clip` / `background-origin` 只报一层。

在百度上表现为：输入框本体（白底 + 1.5px 渐变描边）整块消失，只剩右侧“百度一下”
按钮浮在白底页面上。

## 目标（只做可用子集）

1. **渐变放进 `BackgroundLayer`**（与 `CachedImage` 并列），每个 `background` 层
   各自持有自己的 `CSSGradientValue`；拷贝/相等/`hasImage()`/\_cull\_ 都算上它。
2. **逐层绘制**：颜色仍只在最底层画一次；每层先画自己的渐变（按该层的
   `background-origin` 取背景定位盒），再画该层的图片；层序仍是先画列表尾部、
   再画头部（第一层在最上）。
3. **computed style 报列表**：`background-image` / `background-clip` /
   `background-origin` 按层输出逗号分隔；Chrome 对 `background-clip` 只列
   image 层用到的值，这里按层数输出即可。
4. **`box-shadow` 的 spread**：解析第 4 个长度、存入 `ShadowData`、绘制时把阴影
   形状外扩、computed 输出出来。
5. **textarea placeholder**：`RenderTextControl::paint()` 的占位符绘制从“仅 input”
   扩展到 `HTMLTextAreaElement`（Chrome 的 textarea 占位符同样画在值文本的基线）。
6. **NamedNodeMap 命名属性**：`element.attributes['onclick']` 必须返回 `Attr`
   （jQuery 1.x 的 `div.attributes[eventName].expando` 探测依赖它；缺了会让
   `$ is not defined`，百度脚本链整条断掉，连 placeholder 都不会设）。
7. **`window.devicePixelRatio` 兜底**：目前裸标识符 `devicePixelRatio` 会
   `ReferenceError`；探针/96 DPI 下先用 1，避免特性探测直接抛。

## 非目标（本次不做，记录在案）

- 多层 `background-position` / `-size` / `-repeat` / `-attachment` 的 computed
  列表（绘制已按层生效，只有 CSSOM 仍报第一层）。
- `box-shadow: inset ...`、`background-clip: text`、`::placeholder` 的 CSS 样式。
- 阴影 spread 对全部 overflow/repaint 边界的完整传播（先覆盖绘制与主路径）。
- `background-image` 的 `image-set()` / 多值 `-webkit-gradient` 等更花哨的语法。

## 数据流

```
CSSParser::parseBackgroundShorthand
  └─ 每个逗号分隔的 layer → 每个 longhand 一个 CSSValueList
CSSStyleSelector::HANDLE_BACKGROUND_VALUE
  └─ 按值逐个 BackgroundLayer::setBackgroundImage / setBackgroundClip / ...
       └─ 渐变：BackgroundLayer::setBackgroundGradient(gradient)
       └─ 一层只有一张图：setBackgroundImage 清 gradient，setBackgroundGradient 清 image
          （否则 `background: <color>` 补出的 `background-image: initial` 只清 url 不清渐变，
          R195 的 163 导航高亮就是这个）
RenderBox::paintBackgrounds（递归到 next，尾部先画）
  └─ paintBackgroundExtended(layer)
       ├─ 最底层：背景色
       ├─ 本层渐变（origin 盒）
       └─ 本层图片（clip 盒，已有逻辑）
CSSComputedStyleDeclaration
  └─ 遍历 backgroundLayers() 输出列表
```

## 验收

- 本地 fixture `tools/pages/layout/background_layers.html`：
  - 渐变描边：内白、边框处红/蓝（像素断言）；
  - 双层背景上白下蓝（图片层已在旧版通过，这里是回归保护）；
  - `box-shadow: 0 0 0 6px #00ff00` 的外扩阴影（像素断言）；
  - textarea placeholder 区域有深色像素（区域像素计数）。
- `getComputedStyle` 对上面的元素报出 `linear-gradient(...), linear-gradient(...)` 与
  `padding-box, border-box`。
- 百度首页对拍：`#chat-input-main` 截图与 Chrome 一致（白底 + 渐变描边 +
  蓝色“百度一下”按钮），且 jQuery 正常、textarea 的 `placeholder` 被页面脚本设置。
- 回归：`run_layout_tests.ps1`、`run_binding_semantics_tests.ps1`、
  `run_typing_tests.ps1`（textarea 输入不走 placeholder 通道）全绿。
