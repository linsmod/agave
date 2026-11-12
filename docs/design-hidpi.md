# 显示 DPI：页面按设备像素比渲染（design，2026-11-12）

隔离并记录"页面在高 DPI 屏上按 1:1 设备像素渲染"这件事的模型、实现和已知边界。
现状见 [arch-status.md](arch-status.md)，缺陷复盘见 [hist-lessons.md](hist-lessons.md) R194。

## 1. 问题

壳（`examples/browser_shell`）从 2025 起就是 Per-Monitor V2：`GetDpiForWindow` →
`UiTheme` 按 `dpi/96` 放大所有 chrome 度量和字体。**页面没有这半**：

- `ChromeClientMC::scaleFactor()` 是 `notImplemented(); return 1.0f;`，
  于是 `window.devicePixelRatio` 恒 1、`@media (-webkit-device-pixel-ratio: …)`
  恒按 1 判定、canvas backing store 恒按 1x 开。
- 页面按 1 CSS px = 1 设备 px 布局：150% 屏上工具栏 1.5 倍、页面内容只有应得尺寸的 2/3。
- 响应式站点（`dpr >= 2`、srcset 的 2x、按 dpr 分支的 polyfill）全部走 1x 分支。

## 2. 模型：一个比例，两个来源

```
device px per CSS px  ==  pageZoomFactor()  ==  pageZoom * deviceScale
deviceScale           ==  display DPI / 96        (100% / 125% / 150% …)
pageZoom              ==  用户的 Ctrl+= / Ctrl+-  (默认 1.0)
```

- **布局坐标 = 设备像素**。根 `RenderStyle` 的 zoom 设为 `pageZoomFactor()`
  （`Document::setZoom`），所以 `16px` 的 CSS 字在 150% 屏上按 **24 设备 px 布局**，
  字体是"重新栅格化"而不是"位图放大"，不存在模糊。
- **CSS 像素 = 布局像素 / pageZoomFactor()**。凡是把布局值交回 DOM 的地方都除一次：
  `innerWidth/innerHeight`（`__agbViewportWidth/Height`）、`clientX/pageX`、
  `elementFromPoint`、`getBoundingClientRect`、`@media width/height`。
  `elementScrollTop`、`offsetWidth` 等同族 API 早就是这样。
- **`window.devicePixelRatio` = pageZoomFactor()**（`Chrome::scaleFactor()` →
  `DOMWindow::devicePixelRatio()`），和 Chrome 一致：缩放也计入 dpr。
- 页面缩放与 DPI **互不覆盖**：`macross_view_set_scale_factor`（用户缩放）和
  `macross_view_set_device_scale_factor`（DPI）各存一半，只在 `pageZoomFactor()` 相乘。
  否则 150% 屏上按一次 Ctrl+= 会把 DPR 一起改掉，监视器切换又会重置用户的缩放。

### 为什么不加"绘制变换"

另一条路是布局仍按 CSS px、绘制时对 picasso context `ps_scale(dpr)`。这里不选它：

1. **字体会二次采样**：`ps_show_glyphs` 会先用 `text_matrix` 把字形栅格化，再由
   `render_glyph` 用 `world_matrix` 变换；world matrix 一缩，字形就是位图放大。
   走布局缩放则字形按目标尺寸直接生成（灰度 AA 路径 `init_raster_data(..., world_matrix)`
   只在 world 是单位阵时才 1:1）。
2. **坐标系会分裂**：脏矩形、命中测试、滚动偏移、`mirror` 缓冲、宿主 `BitBlt` 都在设备像素；
   一旦布局是 CSS px，这些全要乘/除 dpr，漏一处就是错位或残影。
3. **本引擎的页面缩放本来就是布局级**（`effectiveZoom` 乘进长度），DPI 走同一条路
   等于零新增机制，`--zoom` 的既有回归也顺带覆盖它。

代价：布局 px 里出现非整数 CSS 长度时会有 1px 级舍入差（`adjustForAbsoluteZoom` 的
`value++` 截断补偿），与页面缩放完全同源。

## 3. 各层改动

| 层 | 位置 | 内容 |
|---|---|---|
| 内核 | `page/Frame.{h,cpp}`、`page/FramePrivate.h` | 新增 `m_deviceScale` / `deviceScaleFactor()` / `setDeviceScaleFactor()`；`pageZoomFactor()` 返回 `pageZoom * deviceScale`；两者变化都 `viewportSizeChanged()` + `recalcStyle(Force)` + 布局（DPI/缩放都移动 CSS 视口，`@media` 必须重新求值） |
| 内核 | `page/ChromeClient.h`、`ChromeClientMC.cpp` | `scaleFactor()` 从 stub 改为读 `mainFrame()->pageZoomFactor()`；`DOMWindow::devicePixelRatio()` 无 page 时兜底 1（原来是 0） |
| 内核 | `css/MediaQueryEvaluator.cpp` | `width/height` 媒体特征按 CSS px 比较（`adjustForAbsoluteZoom`），和 `innerWidth` 同一个数；`-webkit-device-pixel-ratio` 读 `scaleFactor()` |
| 内核 | `html/HTMLCanvasElement.cpp` | 不用改：它本来就乘 `chrome()->scaleFactor()`，现在拿到的是真实 dpr |
| 桥/JS | `AntRuntime.cpp` | 新增 `__agbDevicePixelRatio(document)`（读 `Chrome::scaleFactor()`，iframe 用自己的文档）；`nodeGetBoundingClientRect()` 除以 `effectiveZoom` → 真 CSS px |
| 预置 | `prelude.js` | `window.devicePixelRatio` getter 从硬编码 1 改成读 `__agbDevicePixelRatio`；`matchMedia` shim 先剥 `-webkit-` 前缀、支持 `device-pixel-ratio` / `min-` / `max-` |
| API | `include/macross.h`、`webview/WebEngine.cpp` | `macross_view_set_device_scale_factor` / `_get_`（percent of 96 DPI） |
| 控件 | `webview_control/AgaveWebView.c`、`AgwViewProtocol.h`、`AgwViewBackend.c` | `deviceScale` 字段 + `agave_webview_set_device_scale()`；命令 `AGW_VIEW_CMD_DEVICE_SCALE`（**追加在协议末尾**，老命令的线值不变）；新建/重建 backend view 时重放 |
| 渲染进程 | `examples/render_host/main.c` | 同一命令 → `macross_view_set_device_scale_factor` |
| 壳 | `examples/browser_shell/main.c` | `deviceScalePercent(dpi)`、`syncTabsDeviceScale()`；建 tab、`WM_DPICHANGED`、窗口 DPI 细化后各同步一次（DPI 是**每窗口**的，页面跟着窗口走）；`--device-scale <percent>` 给无头探针；`--shot-rect` 的 CSS→设备换算乘 `zoom * deviceScale` |

## 4. 壳与探针

- GUI：`dpi_for_window()` → `percent = round(dpi * 100 / 96)`（96→100、120→125、144→150、
  192→200）。窗口跨屏拖动时 `WM_DPICHANGED` 把新比例推给**该窗口所有 tab**；新 tab 创建时
  立即得到当前比例，不必等下一次换屏。
- 无头：探针进程保持 DPI-unaware（`send_ui_events.ps1` 依赖跨进程坐标不被系统缩放），
  所以 DPI 由 `--device-scale` 显式给出，默认 100。
- `--shot` / `--shot-rect`：镜像是设备像素，`--shot-rect` 收 CSS px，
  换算系数 = `zoom * deviceScale`（`probeRectCssToClient`）。

## 5. 验证

`tools/run_hidpi_tests.ps1`（6 例，约 3 s）：见 [dev-tests.md](dev-tests.md) 的表格。
关键判据不是"`devicePixelRatio` 读到 1.5"，而是**同一张截图里 100 CSS px 的方块占 150 设备像素**
（`149,149` 红 / `152,152` 白）、CSS `left:200px` 的方块从设备 300 开始，以及
`--shot-rect 200,0,120,60` 产出 180x90 的裁图。

## 6. 已知边界（未做）

- `screen.width/height`、`device-width/device-height`、`screenRect()` 仍是设备像素，
  没有除 dpr；只有 `width/height` 媒体特征和 `innerWidth` 这套做了。
- `Range.getBoundingClientRect()`（`AntRuntime.cpp` 的 `rangeMethod`）仍返回布局 px，
  缩放/高 DPI 下与 `getBoundingClientRect` 不一致；`getClientRects()` 同理。
- 图片的 `srcset` / `x` 描述符选源没有实现（与 dpr 无关，但 dpr 正确后才谈得上选对）。
- 混合渲染的 GIF/APNG、`AGB_IMG_SAMPLE_MAX` 的采样口径都按显示尺寸（设备像素）算，
  现在显示尺寸随 dpr 变大，大图解码档位会比 1x 时更保守。
- 帧率/性能：布局 px 变大意味着 150% 屏上同样的窗口有 2.25 倍的像素量，paint 成本随之上升；
  这是"按 DPI 渲染"的固有代价，不是回退。
