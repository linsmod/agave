# ksvg2 复活设计（ENABLE_SVG）

> 状态：实施中（2026-11-07 立项）。本文是设计记录，不是现状；现状以 `docs/arch-*.md` 为准。

## 背景与决策

2026-11-07 决定：**复活仓库内 2008 版 ksvg2**，而不是移植 miniblink/Blink 的 `core/svg`，也不先用
NanoSVG 顶 SVG-as-image。

理由：

- ksvg2 与本仓库同代 WebCore（RenderObject / CSS 子集 / DOM 直接匹配），miniblink 的 `core/svg`
  依赖 Blink 布局（LayoutSVG*）、SkPicture 与整套 Skia，移植量级以月计（492 文件/3.4 万行只是
  `core/svg` 本身）；
- 图形底层已就位：`PathPS.cpp` 有完整路径/曲线/裁剪，`GraphicsContextPS.cpp` 有 fill/stroke/clip，
  picasso 有 `ps_gradient_create_linear/radial` 与 `ps_pattern_*`；
- JS 绑定（`buildKJS/JSSVG*`、`buildQJS/QJSSVG*`）已在树内预生成；
- 复活 ksvg2 也是将来 SVG-as-image（`<img>`/background）的正确底座：`SVGImage` 就是把 SVG 文档
  渲染树画进位图，A 阶段通了它才能落地。

## 现状盘点（2026-11-07）

- 开关：`ENABLE(FEATURE)` = `defined(ENABLE_FEATURE) && ENABLE_FEATURE`（`src/wtf/Platform.h:12`）。
  当前 `mconfig.h` 没有 `ENABLE_SVG`，1182 处 `#if ENABLE(SVG)` 全部编译为空。
- 代码生成：`src/webcore/ksvg2/scripts/make_names.pl` 在树内；`buildKJS/SVGNames.cpp`、
  `SVGElementFactory.cpp` 目前是**空桩**（禁用分支的产物），XLinkNames 同样缺失，必须重新生成。
- 缺失实现（按 `#include` 反向核对出的真缺口）：
  - 资源/绘制：`SVGResource`、`SVGPaintServer`、`SVGPaintServerSolidColor`、
    `SVGPaintServerGradient`（线性/径向）、`SVGPaintServerPattern`；
  - 资源元素：`SVGResourceClipper`、`SVGResourceMasker`、`SVGResourceMarker`、`SVGResourceFilter`；
  - 平台：`platform/graphics/PathTraversalState`（SVGPathSegList 依赖）；
  - filter 组（SVGFE* / SVGLightSource / SVGFilterEffect）全部受
    `ENABLE_SVG_EXPERIMENTAL_FEATURES` 保护，A 阶段不开。
- 已有、需验证：`rendering/RenderSVG*`（26 文件）、`ksvg2` DOM（376 文件）、`css/svg.css`、
  `ksvg2/misc/SVGImageLoader`、`Document::createElementNS` 的 SVGElementFactory 接线。
- HTML 解析器完全没有 SVG 分支（`HTMLParser.cpp` 无 svg 引用）→ A 阶段只覆盖 **XML/SVG 文档**；
  HTML 内联 `<svg>` 是 B 阶段。
- 2008 树没有 `SVGImage`（`<img>`/CSS 背景的 SVG 解码）→ C 阶段。

## 设计

- 分层：ksvg2 保持可移植（只依赖 GraphicsContext / RenderObject / Path）；picasso 专有绘制通过
  `GraphicsContext::platformContext()`（= `PlatformGraphicsContext` / `ps_context*`）完成。
- 资源模型：`SVGResource` 为 `RefCounted` 基类，持 client 集合与全局注册表；对齐现有调用点：
  `addClient` / `removeClient(SVGStyledElement*)`（静态，遍历注册表）/ `repaintClients()`（成员）与
  `repaintClients(HashSet<SVGStyledElement*>&)`（静态）。
- paint server：`fillPaintServer` / `strokePaintServer(RenderStyle*, RenderObject*)` 解析
  `SVGPaint`（颜色 / currentColor / `url(#id)`）。颜色走无状态的 `SVGPaintServerSolidColor`，
  颜色/透明度从 `object->style()` 现取；渐变与图案 A 阶段先降级（首停靠色/不绘制），并记缺口。
- 绘制路径：RenderPath 已 `beginPath()+addPath()` 后调用 paint server 的 `draw(context, object, type)`；
  draw 内直接通过 platformContext 设填充/描边源并 `ps_fill`/`ps_stroke`，避免依赖不存在的
  `fillPath()` 无参重载。
- clipper/masker/marker：A 阶段给可编译的最小实现（clipper 保留 clip 数据但 applyClip 先做矩形联合；
  masker 暂不应用；marker 不绘制），逐项记 roadmap。

## 里程碑与验收

- **A1 构建转绿**：`tools/gen_svg_names.ps1` 生成真空白替换 + `ENABLE_SVG` 开关 + 补齐上述实现，
  `tools/build_project.ps1` 无错误。
- **A2 文档渲染**：`agave_shell --probe file.svg --shot out.bmp` 出现非空像素；rect/circle/path +
  fill/stroke 生效。
- **A3（可选）**：CSS 属性表（fill/stroke/stop-color…）需 gperf 重生成 `CSSPropertyNames`；
  本机无 gperf，先记 roadmap，A 阶段只依赖 presentation 属性。
- **B**：HTMLParser 外来内容支持 inline `<svg>`；**C**：`SVGImage`（img/background-image），
  复用 A 的渲染树。

## 风险

- 18 年未编译的代码与本仓库 2026 改动冲突，编译/运行时错误量未知；
- 生成物（SVGNames/CSS 表）与手写代码版本漂移，重生成规则要写进工具脚本；
- ksvg2 是 CPU 光栅（picasso），图标场景可接受，复杂页面需实测；
- text / marker / filter / pattern 的质量与覆盖需逐项验收，不能默认“能编译=能用”。

## 实施记录（2026-11-07，阶段 A）

**A1 构建转绿**

- `OPT_SVG`（默认 ON）→ `mconfig.h` 的 `ENABLE_SVG`；`src/src.cmake` 补 ksvg2 的 include 目录。
- `tools/gen_svg_names.ps1`：从 `ksvg2/svg/svgtags.in`/`svgattrs.in`/`misc/xlinkattrs.in` 生成真实的
  `SVGNames`/`XLinkNames`/`SVGElementFactory`（原来那几份是禁用分支的空桩），`-WithCSS` 可同时重生成
  带 SVG 属性的 `CSSPropertyNames.*`/`CSSValueKeywords.*`。
  踩坑：`.in` 是 CRLF、`gcc -E` 管道还会再带 CR，生成物出现过 `a<CR>Tag` 的断行 —— 脚本统一去 CR；
  CSS 表必须在临时目录里 cat 后跑 `makeprop.pl`/`makevalues.pl`，gperf 从本机 vcpkg 目录找。
- 补齐的实现（都在 ksvg2 的调用点契约上）：
  - 资源层：`SVGResource`（本树没有 `wtf/RefCounted`，自管 ref/deref + 全局注册表）、
    `SVGResourceClipper`（逐子路径 `clip()`）、`SVGResourceMasker`（暂不应用）、`SVGResourceMarker`
    （暂不绘制）、`SVGResourceFilter`（仅实验宏下的空壳）；
  - 绘制层：`SVGPaintServer` 工厂（颜色/currentColor/`url(#id)`）、`SVGPaintServerSolidColor`、
    `SVGPaintServerGradient`（线性/径向，暂退化为首停靠色）、`SVGPaintServerPattern`（暂不可用）；
  - 平台：picasso `ImageBuffer`（image + canvas 离屏）、`GraphicsContext::concatCTM` 与无参
    `fillPath()`/`strokePath()`、`Path::apply`/`PathElement`、`AffineTransform::map(double…)`、
    `PathTraversalState`、`FloatConversion` 的 float 特化、`DashArray.h`。
- 顺带修的坑：
  - `CSSParser::parseValue` 缺少 `CSS_PROP_ZOOM` 的 case，默认分支会掉进 `parseSVGValue` 的
    “unimplemented propertyID” 断言 —— JS 里设 `style.zoom` 直接崩溃（调用栈落在
    `SVGCSSParser.cpp:292`）。已补解析分支。
  - `JSSVGAnimatedPoints` 用 `RefPtr<SVGAnimatedPoints>`，而该类没有 ref/deref；对象实际归 SVG 元素所有，
    先补 no-op ref/deref（阶段 D 再换正确绑定语义）。

**A2 验收（探针像素，`debug/svg_shapes.svg`）**

| 元素 | 期望 | 实测 |
|---|---|---|
| 红色矩形 80×60 | 4800 px | **4800** |
| 绿色圆 r=30 | ≈2827 px | **2804**（抗锯齿边缘） |
| 蓝色三角形 40×30/2 | 1200 px | **1200** |
| 黑色描边矩形（stroke-width 4） | 周长一圈 | **420** |
| 背景 | 其余全白 | 870344 白（另 432 抗锯齿灰） |

HTML 回归：`debug/imgbox_160.html` 深色占比 **37.8%**（R29 基线 37.7%，无退化）。

**已验证仍不支持的（阶段 B/C 与后续）**

- HTML 内联 `<svg>`：`debug/svg_inline.html` 0 像素（HTMLParser 没有外来内容支持）；
- `<img src="*.svg">` / CSS 背景 SVG：`debug/svg_img.html` 0 像素（没有 `SVGImage`）；
- 渐变退化为首停靠色；marker/clipPath(多子路径并集)/mask/pattern/filter 不绘制或只做近似；
- SVG 文本、CSS SVG 属性的 selector 应用/继承未验收。

**探针注意**：本代理环境给子进程的文件写权限只覆盖 `build-zig-native`，`--shot` 输出必须落在那；
`debug/`、`%TEMP%` 会 `fopen` 失败（exit 4，日志 `shot failed`）。

## 阶段 B（前置）：JS 创建的 inline SVG 已渲染（2026-11-08）

DeepSeek 登录页（`https://chat.deepseek.com/sign_in`）的 logo 不是静态 `<svg>` 标记，而是 React 用
`document.createElementNS('http://www.w3.org/2000/svg', 'svg')` 建的：DOM 侧本来就正确
（`namespaceURI` = SVG、11 个 `path`、`getBoundingClientRect()` = 182×28），但不显示。

定位过程与结论：

1. 布局/绘制日志显示 SVG 根**有 layout（w=182 h=28, viewport=182×29）但从未进入 `paint`**；
   它的祖先链是 `RenderBlock → FlexibleBox(0×0) → FlexibleBox(576×40) → 相对定位 FlexibleBox[L](1100×0) → FlexibleBox[L]`。
   父级块在 `RenderBlock::paintChildren` 里没有被遍历到（整段 flex 子树的绘制被跳过）。
2. 修复：`RenderSVGRoot::requiresLayer()` 强制为 true。SVG 根拥有自己的图层后，由 layer 树独立绘制，
   不再依赖可能被跳过的父块绘制链。验证：`paint` 正常进入（phase 0/2/3/4/6/7），logo 画出。
3. 裁剪修正：`RenderSVGRoot::paint` 里 overflow 裁剪原用 `viewport()`（SVG 用户单位 182×29），
   页面缩放时渲染盒是 227×35，两者不一致；改为按渲染盒 `IntRect(0,0,width(),height())`（布局 px）。

验收：DeepSeek 登录页 830/1100 宽都能完整显示 logo（蓝色像素 13692）；`debug/svg_shapes.svg` 仍
红 4800/绿 2804/蓝 1200/黑 420；`debug/svg_inline_js.html` 红 3200；`imgbox_160` 深色 37.8%。

已知仍未解决：

- **HTML 静态标记里的 `<svg>`** 仍不渲染（HTMLParser 没有外来内容解析，见阶段 B2）；
- **页面缩放 ≠100%** 时 inline SVG 内容相对盒子有偏移（125% 实测盒子 227×35、内容仍按 182×29 的用户
  单位变换），需要把 zoom 统一到 viewport/CTM 计算；

