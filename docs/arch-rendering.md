# 渲染管线：DOM → 样式 → 布局 → 绘制（当前实现）

> 状态：**当前态，最后核对 2026-11-06**。本文回答"一个盒子从 DOM 到像素经过谁、布局问题先看哪"。
> 进程/线程/所有权见 [arch-overview.md](arch-overview.md)；能力边界见 [arch-capability-gaps.md](arch-capability-gaps.md)；
> 调试开关与"布局错还是绘制错"的判据见 [dev-debugging.md](dev-debugging.md) §3/§4。

## 0. 先记住三条

- **渲染树是 DOM 的投影**，不是第二份文档模型。`display:none` 的元素没有 `RenderObject`
  （探针里 `renderer=0`），所以"元素在 DOM 里但量不到"通常不是布局问题。
- **样式重算、布局、绘制都在引擎线程同步发生**；JS 读几何会就地触发一次布局，因此布局可能出现在
  任意脚本调用栈里，不只在帧泵。
- 布局是渲染树上的一趟纵向递归（flex 等个别算法内部会多趟），没有现代浏览器的分片/合成。
  `RenderLayer` 主要负责 z-order、裁剪、滚动条与滚动偏移，不参与尺寸计算。

## 1. 管线

```text
HTML ──HTMLTokenizer──► DOM
CSS  ──CSSParser──────► CSSStyleSelector ──styleForElement──► RenderStyle（计算值）
                                    │
DOM attach（Element::attach / Text::attach）
   └─ createRendererIfNeeded ──► RenderObject::createObject(display)
        → 渲染树：RenderBlock / RenderInline / RenderText / RenderTable / RenderReplaced …
          （块里混排 inline 与 block 时生成匿名块，见 RenderBlock::makeChildrenNonInline）

布局（引擎线程，任意 JS 调用栈内都可能发生）
  Document::updateLayout
    或 FrameView::layoutIfNeededRecursive（帧泵每帧）
      → FrameView::layout ──► RenderView::layout
          → RenderBlock::layoutBlock
              ├ childrenInline() → RenderBlock::layoutInlineChildren（bidi.cpp）
              └ else             → RenderBlock::layoutBlockChildren（块流/margin 折叠/float）
              → calcHeight → layoutPositionedObjects → overflow/repaint 记账
      → RenderLayer::updateLayerPositions

绘制（帧泵，脏区非空时）
  WebView::paintView → ScrollView::paint → Frame::paint
    → RenderView 的 layer->paint（RenderLayer::paintLayer：负 z → 正常流 → 正 z）
      → RenderBox / RenderBlock / RenderInline / RenderText::paint → GraphicsContext

命中测试（事件入口，和布局共用几何）
  EventHandler::hitTestResultAtPoint → RenderLayer::hitTest → RenderBox/RenderBlock::nodeAtPoint
```

## 2. 阶段 → 文件

| 阶段 | 入口函数 | 主要文件 | 改这里的典型任务 |
|---|---|---|---|
| HTML → DOM | `HTMLTokenizer::write()` / `scriptExecution()` | `src/webcore/html/HTMLTokenizer.cpp` | 解析时序、`script`/module 挂起（见 [arch-module-loading.md](arch-module-loading.md)） |
| CSS 解析 | `CSSParser::parseSheet()` / `parseDeclaration()` / `parseValue()` | `src/webcore/css/CSSParser.cpp`、`CSSGrammar.y` | 词法/语法、属性别名、未实现属性 |
| 选择器 + 层叠 | `CSSStyleSelector::styleForElement()` | `src/webcore/css/CSSStyleSelector.cpp` | 选择器匹配、`display` 修正（flex blockify）、继承 |
| computed style | `CSSComputedStyleDeclaration::getPropertyValue()` | `src/webcore/css/CSSComputedStyleDeclaration.cpp` | `getComputedStyle` 的序列化与"未实现"默认值 |
| 渲染树 attach/detach | `Element::attach()` / `Element::recalcStyle()` / `RenderObject::createObject()` | `src/webcore/dom/Element.cpp`、`src/webcore/rendering/RenderObject.cpp`、`src/webcore/dom/Text.cpp` | 元素没有 renderer、样式变更后不重建 |
| 布局调度 | `Document::updateLayout()`、`FrameView::layout()` / `scheduleRelayout()`、`RenderObject::setNeedsLayout()` | `src/webcore/dom/Document.cpp`、`src/webcore/page/FrameView.cpp`、`src/webcore/rendering/RenderObject.cpp` | "改了但没重排"、"重排了但没画" |
| 布局核心（块） | `RenderView::layout()`、`RenderBlock::layoutBlock()` / `layoutBlockChildren()` / `layoutPositionedObjects()` | `src/webcore/rendering/RenderView.cpp`、`RenderBlock.cpp` | 块宽高、margin 折叠、float、绝对定位 |
| 行内布局 | `RenderBlock::layoutInlineChildren()`、`RenderText::calcPrefWidths()` / `width()` | `src/webcore/rendering/bidi.cpp`、`InlineFlowBox.cpp`、`RootInlineBox.cpp`、`break_lines.cpp`、`RenderText.cpp` | 换行、行高、baseline、文本宽度 |
| flex | `RenderFlexibleBox::layoutBlock()` / `layoutHorizontalBox()` / `layoutVerticalBox()` | `src/webcore/rendering/RenderFlexibleBox.cpp` | `display:flex` 下的主轴分配、对齐、gap |
| 表格 | `RenderTable::layout()` → `AutoTableLayout::layout()` / `FixedTableLayout::layout()`；`RenderTableSection::layoutRows()` | `src/webcore/rendering/RenderTable*.cpp`、`AutoTableLayout.cpp`、`FixedTableLayout.cpp` | 列宽、行高、`table-layout` |
| 层 / 定位 | `RenderLayer::updateLayerPositions()` / `paintLayer()`；`RenderBox::calcAbsoluteHorizontal()` | `src/webcore/rendering/RenderLayer.cpp`、`RenderBox.cpp` | z-order、裁剪、滚动、absolute 尺寸 |
| 绘制 | `WebView::paintView()`、`ScrollView::paint()`、`Frame::paint()`、`RenderLayer::paintLayer()` | `src/webview/WebView.cpp`、`src/webview/ScrollViewMC.cpp`、`src/webcore/page/Frame.cpp`、`src/webcore/platform/picasso/GraphicsContextPS.cpp` | 残影、clip、渐变/背景、后端坐标 |
| 命中测试 | `EventHandler::hitTestResultAtPoint()` | `src/webcore/page/EventHandler.cpp`、`RenderLayer.cpp`、`RenderBox.cpp` | 点不中、点错元素（和布局共用几何） |

## 3. 布局内部

### 3.1 display → RenderObject

`RenderObject::createObject()` 按 `style->display()` 分派：

| display | RenderObject |
|---|---|
| `none` | 不创建（`renderer=0`） |
| `inline` | `RenderInline` |
| `block` / `inline-block` / `run-in` / `compact` | `RenderBlock` |
| `-webkit-box`（= `flex`） | `RenderFlexibleBox` |
| `-webkit-inline-box`（= `inline-flex`） | `RenderFlexibleBox` |
| `table` / `inline-table` | `RenderTable` |
| `table-row-group` / `table-header-group` / `table-footer-group` | `RenderTableSection` |
| `table-row` / `table-cell` / `table-column(-group)` | `RenderTableRow` / `RenderTableCell` / `RenderTableCol` |
| `list-item` | `RenderListItem` |
| 文本节点 | `RenderText`（`Text::createRenderer()`） |

`display:flex` 在**解析期**就被翻译成 `-webkit-box`（`CSSGrammar.y` 的属性别名 +
`CSSParser::modernKeywordID()`），子项 blockify 在 `CSSStyleSelector::styleForElement()` 里补
（见 §8）。

### 3.2 一次 `layoutBlock` 做什么（顺序很重要）

1. `layoutOnlyPositionedObjects()`：只有 positioned 子项变脏时短路返回。
2. `calcWidth()` / `calcColumnWidth()`；宽度变了 → 本轮 `relayoutChildren = true`。
3. `clearFloats()`；`m_height = 0`。
4. `childrenInline() ? layoutInlineChildren() : layoutBlockChildren()`。
5. 用 `floatBottom()` 撑高、`layoutColumns()`、`calcHeight()`；高度变了 → `relayoutChildren`。
6. `layoutPositionedObjects()`：摆放 absolute/fixed 子项（它们不在正常流里）。
7. overflow 记账（`m_overflowWidth/Height`）+ 与旧 bounds 比较，把差额加进重绘队列。

### 3.3 块流（`RenderBlock::layoutBlockChildren`）

- 每个子项先 `handleSpecialChild()`：positioned → `insertPositionedObject()` + 记录静态位置；
  float → `positionNewFloats()`；compact/run-in → 特殊处理。**这些不参与普通块流**。
- 普通块：`calcVerticalMargins()` → `estimateVerticalPosition()` → `collapseMargins()`
  （`MarginInfo` 累积正/负 margin）→ `layoutIfNeeded()`。
- margin 折叠是布局问题的高发区：相邻块、父子块、空块（self-collapsing）、float 之间规则不同；
  改块间距先看 `RenderBlock::collapseMargins()`（配合 `MarginInfo`），别先怀疑高度计算。
- `clear` 由 `clearFloatsIfNeeded()` 处理；`overflow` 建立 BFC 近似（`avoidsFloats()` /
  `hasOverflowClip()` 分支决定是否绕开 float）。

### 3.4 行内（`bidi.cpp`）

- 块直接拥有行盒（行内内容不单独生成块；只有 inline 与 block 混排时才插匿名块）：`layoutInlineChildren()` 依次
  `determineStartPosition()` → `bidiReorderLine()` → `constructLine()`（`RootInlineBox` +
  `InlineFlowBox` + `InlineTextBox`）→ `computeHorizontalPositionsForLine()` →
  `computeVerticalPositionsForLine()`（`verticallyAlignBoxes()` + `position()` 写回每个行内盒的坐标）。
- 断行：`break_lines.cpp` 的 `nextBreakablePosition()`（WinIE 风格：空白、`-`、`?`、软连字符；
  非 ASCII 走 UAX-14 的 `lineBreakIterator`。没有现代 `word-break`/`overflow-wrap` 语义）。
- 文本宽度：`RenderText::calcPrefWidths()` / `width()` 走 `Font`（平台字体度量）；
  改了字体、字符或 `letter-spacing` 却没重排，先确认 `setPrefWidthsDirty()` 被调用。
- 垂直对齐：`RenderObject::baselinePosition()` / `verticalPositionHint()`；行盒内由
  `InlineFlowBox::verticallyAlignBoxes()` 统一排。
- **单行文本框的 placeholder 与光标都按「文字高度」而不是行盒/盒子高度**（R127）：placeholder 由
  `RenderTextControl::paint()` 自绘，空控件没有行盒，基线要自己加 half-leading
  （`(lineHeight - ascent - descent)/2`）；光标在空控件时走 `RenderTextControlInnerBlock::caretRect()`、
  有文字时走 `RenderText::caretRect()`，两者对 `<input>` 都返回「基线 - ascent，高 ascent+descent」
  （`<textarea>` 仍是行盒高度）。量它：`AGB_CARET_DEBUG=1` 把光标画红，`tools/caret_check.py` 读数。

### 3.5 flex（`RenderFlexibleBox`）

- 横向（`box-orient: horizontal`）只有**一套**算法：`layoutFlexLines(bool wrap, ...)`
  —— 单行与 `flex-wrap: wrap` 共用，区别只在是否断行；`layoutHorizontalBox()` 在 LTR 下直接委派。
- 三趟：①基名取 `flex-basis`（auto → 指定 `width` → max-content），同时算 floor
  （`min-width: auto` → min-content）与 ceiling（`max-width`）；②按 Flexbox §9.7 分配 free space
  （grow / `flex-shrink × base`），clamp 到 [floor, ceiling] 后按 violation 符号冻结、下一轮重分；
  ③行高、`align-items`/`align-content`/`justify-content` 摆放（`placeChild`），
  `align-items: stretch` 在这里把 auto 高的子项撑到行高（先定 `m_height` 再重排子项，
  期间必须保持 `m_flexingChildren`，否则子项会被当成普通块重排回容器宽）。
- **单行 + 定高容器：行高 = 容器内高**（Flexbox §9.4），不是「最高子项」。子项比容器高时
  Chrome 让它在行盒里按 `align-items` 对齐（`center` 就是上下等量溢出）；按子项定行高会把
  内容顶到内容区上沿（DeepSeek 登录输入框：48px 前缀 + 5px margin 放进 48px 盒，矮 6px）。
- **表单控件没有内在 margin**：2008 的 `addIntrinsicMargins()`（UA 的 `margin: 0__qem` 被
  当成 quirk → 2px）已删除；Chrome 的 `<input>`/`<button>` 默认 margin 就是 0，保留它会让
  控件右移 2px、可用宽度少 4px。
- 仍然缺：`align-self`、`order`（仍是 `box-ordinal-group` 的整数分组语义）、
  `flex-basis` 的百分比/`calc()`、RTL 单行（仍走 2008 放置代码）。
- `AGB_FLEX_DEBUG=1` 打印每行的 base/floor/ceiling/final 与 `block-end` 的 `w=/over=`。

### 3.6 表格

`RenderTable::layout()` 先 `calcWidth()`，再 `m_tableLayout->layout()`：
`AutoTableLayout::layout()`（按 cell 的 min/max 内容宽度分配）或
`FixedTableLayout::layout()`（用首行/列宽），然后 `setCellWidths()` →
`RenderTableSection::layoutRows()` → `RenderTableRow::layout()` → `RenderTableCell::layout()`。
列宽不对先确认走的是哪种算法。

### 3.7 定位与层

- absolute/fixed 子项在 `layoutBlockChildren` 里只被 `insertPositionedObject()` 收进
  containing block 的 `m_positionedObjects`，真正尺寸在 `layoutPositionedObjects()` →
  `calcAbsoluteHorizontal/Vertical()`；静态位置由父块的 `setStaticX/setStaticY` 写死。
- 布局结束后 `FrameView::layout()` 调 `RenderLayer::updateLayerPositions()`，刷新层的
  x/y/裁剪/滚动信息。
- z-order 在绘制时按负 z → 正常流 → 正 z 三趟走（`m_negZOrderList` / `m_posZOrderList`），不做合成。
- `position: sticky` 保持流内布局位置，只把**绘制/命中/几何**位置平移：`RenderBox::stickyPositionOffset()`
  在每次需要时现算（最近滚动容器 = overflow 盒或 RenderView；scrollport 是它的 padding box；insets 只往
  对应方向位移，再夹进 containing block），应用点是 `RenderLayer::convertToLayerCoords`（绘制 + 命中）、
  `RenderObject::absoluteBoundingBoxRect`（`getBoundingClientRect` 与脏矩形）；sticky 会建 layer，
  滚动时 `ScrollView::valueChanged` 在文档有 sticky 内容时整视口重绘而不是 blit（否则会被滚走）。
  它**不是** positioned：`isPositioned()` 仍为 false，不参与 `m_positionedObjects`。

### 3.8 尺寸从哪来

- `RenderBox::calcWidth()` / `calcHeight()`：声明值 + `box-sizing` + min/max + `margin:auto` +
  containing block 宽度；百分比相对 containing block。
- 替换元素走 `calcReplacedWidth/Height()`；float、inline-block、table、flex 子项还要用
  intrinsic 宽度（`calcPrefWidths()` 缓存的 min/max pref width）。
- 所有对外几何读数都按 zoom 修正（`adjustForAbsoluteZoom()` / `adjustForLocalZoom()`）。

## 4. 布局什么时候发生（脏标记与触发）

| 触发 | 路径 |
|---|---|
| 样式变更 | `Document::recalcStyle()` 末尾 `if (changed() && view()) view()->layout()`；同步 |
| 结构变更 | `RenderObject::setNeedsLayout(true)` → `markContainingBlocksForLayout()` 置 `m_normalChildNeedsLayout` / `m_posChildNeedsLayout`，一路标记到根（text field / textarea 这类 relayout boundary 提前停）并 `scheduleRelayout()` |
| 帧泵 | `WebView::paintView()` 每帧 `m_frameView->layoutIfNeededRecursive()`（主 frame + iframe） |
| JS 读几何 | `Element::offset*/client*/scroll*` → `document()->updateLayoutIgnorePendingStylesheets()` |
| 定时器 | `FrameView::scheduleRelayout()` 的 one-shot timer（`Document::minimumLayoutDelay()`） |
| 显式重排 | `WebView::reflow()`（min layout size / small view / `macross_view_reflow`）；zoom 走 `Frame::setPageZoomFactor()` / `setZoomFactor()` → `recalcStyle(Force)` + `view()->layout()` |
| 窗口/iframe 尺寸变化 | `FrameView::layout()` 开头把 `styleViewportSize` 和当前尺寸比：变了先 `Document::viewportSizeChanged()` + `updateStyleIfNeeded()`（重建 selector → `@media` 重估、vw/vh 变量换值），再正常布局；布局末尾尺寸变化时派发 `resize` 事件（R69） |

常见失败模式：

- **`renderer=0`**：`display:none`、父级样式错误（例如 `:empty` 没有触发失效）、或 attach 没发生。
- **`renderer=1` 但 `w=0`**：脏位没传到父级 / 子树 relayout 边界丢了；用 `__agbLayoutInfo()`
  看 `self` / `child` / `parent` 位（§5）。
- **改了文本/字体但没重排**：检查 `setPrefWidthsDirty()` 和 `setNeedsLayout()` 是否都调了。

## 5. JS 读几何走哪

| API | 实现 | 说明 |
|---|---|---|
| `offsetWidth/Height/Left/Top/Parent` | `Element.cpp` 访问器 → `RenderObject::offset*` | 先 `updateLayoutIgnorePendingStylesheets()`，再按 zoom 修正 |
| `clientWidth/Height/Left/Top` | `Element.cpp` → `RenderObject::client*` | 同上；不含 border/滚动条 |
| `scrollWidth/Height` | `Element.cpp` → `RenderObject::scroll*` | 同上 |
| `getBoundingClientRect()` | `AntRuntime.cpp` 的 `nodeGetBoundingClientRect()` → `RenderObject::absoluteBoundingBoxRect()` | **本身不触发布局**（依赖已有布局）；诊断时先读一次 `offsetWidth` 或等一帧 |
| `getComputedStyle()` | `CSSComputedStyleDeclaration` + `prelude.js` 的 shim | 桥返回空时，`width`/`height` 会回退到 `offsetWidth/Height`，所以可能拿到**已用像素**而不是 `auto`；判断声明值看 `el.style.*` |
| `__agbLayoutInfo(node)` | `AntRuntime.cpp` 的 `globalLayoutInfo()` | 不强制布局；输出 `inDoc/renderer/type/x/y/w/h/self/child/parent 尺寸/parent 脏位/disp` |

要看整棵渲染树时，`src/webcore/rendering/RenderTreeAsText.h` 的 `externalRepresentation()`
能 dump（当前没有开关，需临时加调用）。

## 6. 绘制（只用于划清边界）

- `WebView::paintView()` 先 `layoutIfNeededRecursive()` 再 `paint()`；`AGB_PAINT_FULL=1`
  强制整屏重绘（诊断脏区/裁剪，不改布局）。
- 主要 phase 顺序（`RenderObject.h` 的 `PaintPhase`）：`PaintPhaseBlockBackground` →
  `PaintPhaseChildBlockBackgrounds` → `PaintPhaseFloat` → `PaintPhaseForeground` →
  `PaintPhaseOutline` → `PaintPhaseSelection`；边框折叠另走 `PaintPhaseCollapsedTableBorders`。
- `RenderLayer::paintLayer()` 负责 clip 与 z-order；后端是
  `src/webcore/platform/picasso/GraphicsContextPS.cpp`（Picasso），坐标是设备像素。
  页面 zoom 在样式里折算（`RenderStyle::setZoom` / `effectiveZoom`，`Document::recalcStyle(Force)`
  时写入），渲染树坐标就是输出坐标，绘制阶段不再整体缩放。
- 打开的 `<select>` 菜单在**页面之后**由 `WebView::paintView()` 合成到同一块 surface：
  `PopupMenu::windowRect()` 区域先填白，再 `translate` 到菜单原点画条目，最后描 1px 灰边。
  菜单不是独立窗口（宿主没有 `cb_create_popup_menu`），所以它同时出现在无头截图里；
  输入也由 `WebView` 优先路由给它。设计与限制见 [design-select-popup.md](design-select-popup.md)。
- 判据：`AGB_PAINT_FULL=1` 后正常 = 失效/裁剪问题；仍不对 = 布局/样式问题
  （详见 [dev-debugging.md](dev-debugging.md) §4）。

## 7. 症状 → 先查（布局专用）

| 症状 | 先查 |
|---|---|
| 元素占位但看不见 / 量到 0×0 | `__agbLayoutInfo()` 分清 `renderer=0`（没渲染树）与 `w=0`（有树没布局）；再开 `AGB_STYLE_LOG=1 AGB_STYLE_FILTER=<子串>` 看 attach/recalc 时序 |
| 整体贴左 / 偏上 / 居中失效 | 绝对定位子项的 static position（`RenderBlock::adjustPositionedBlock`）、flex 的 `box-align`/`box-pack`、`margin:auto` |
| 上下间距不对（贴一起或多出空白） | margin 折叠（`MarginInfo` / `collapseMargins`）、空块 self-collapsing、行盒 `line-height`、`gap` |
| flex 子项不撑满 / 不居中 | `display:flex` 是否翻译成 `-webkit-box`、子项是否被 blockify、`box-align:stretch`、`box-flex` |
| 表格列宽不对 / 溢出 | `table-layout` 选中的算法（Auto/Fixed）、cell 的 min/max pref width、`border-collapse` |
| 滚动区/视口尺寸不对 | `FrameView::adjustViewSize()`、`RenderView::docWidth()/docHeight()`、`RenderLayer::updateScrollInfoAfterLayout()` |
| 布局正确但画面错（残影/缺块） | 不是布局：`AGB_PAINT_FULL=1` + clip/repaint（见 [dev-debugging.md](dev-debugging.md) §4） |
| 布局慢 | `AGB_PERF_LOG=1`（帧内 layout ms，layout+paint>100ms 才打）、`AGB_PROFILE=1`、`AGB_LAYOUT_DEBUG=1`（每 5 次布局 dump 总耗时） |

## 8. 与规范不同（会误导判断）

- `display:flex` 仍落在 `-webkit-box` 的渲染对象（`RenderFlexibleBox`），但横向布局已按现代语义：
  `flex-basis`/`flex-grow`/`flex-shrink`/`min-width:auto`/`min|max-width` 冻结/`align-content`/`gap`
  都生效（§3.5）；**缺** `align-self`、`order`（整数分组）、`flex-basis` 的百分比/calc、RTL 单行。
- `display:grid` 降级成 `block`（声明不丢，子项按块堆叠，不是 bug）。
- `position: sticky` 支持单轴/双轴 inset 与视图或 overflow 盒两种滚动容器（§3.7），但**只平移绘制/命中/几何**：
  布局位置不变，不产生滚动链上的"滚动祖先"语义；`scrollIntoView()` 仍是 no-op。
  `transform` 只影响绘制：translate/scale/rotate 在
  `RenderBlock::paint()` 里合成矩阵，文字字形也走同一矩阵（R80），但**不参与布局**，
  hit-test 不映射、不建 stacking context，`getComputedStyle().transform` 仍为空。
- `calc()` 只在解析期折叠**纯常量**；含 `%`/`em`/`vw` 的混合 calc 仍不支持。
- 没有 `contain`/`will-change`/合成层；多列是 2008 的部分实现（`RenderBlock::layoutColumns()`），
  无现代 `break-*` 语义；`RenderLayer` 只做 z-order 与裁剪。

能力矩阵与复测命令见 [arch-capability-gaps.md](arch-capability-gaps.md) §2。
