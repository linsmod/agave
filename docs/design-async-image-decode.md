# 设计：图片解码不要发生在 paint 里（异步/预算化解码）

状态：**L0/L1/L2/L3 均已实现（R136、R137、R138，2026-11-08）**；剩余项见文末。现状见
[arch-rendering.md](arch-rendering.md)，历史根因见 [hist-lessons.md](hist-lessons.md)，媒体侧已有的
异步解码先例见 [arch-media.md](arch-media.md)。

## 1. 现状：解码是 paint 的一部分，而 paint 跑在唯一的引擎线程上

调用链（全部在引擎线程）：

```
agwEngineThread (src/webview_control/AgaveWebView.c)
  └─ macross_view_update()            # src/webview/WebEngine.cpp
       └─ WebView::paintView()        # src/webview/WebView.cpp
            ├─ FrameView::layoutIfNeededRecursive()
            └─ FrameView::paint()
                 └─ RenderImage::paint()   → BitmapImage::draw()
                      └─ ImageSource::createFrameAtIndex()
                           └─ ImageDecoder::frameBufferAtIndex()   # 真解码
```

- 三个解码器都是**惰性**的：`setData()` 只读文件头拿尺寸（`decode(true)`），像素在
  `frameBufferAtIndex()` 里现解。
- **JPEG**：`JPEGImageDecoder::outputScanlines()`（`JPEGImageDecoder.cpp:462`）一次
  `while (output_scanline < output_height)` 把所有行解完，**没有行数/时间上限**。
- **PNG**：`PNGImageDecoder::decode()` 把整段 `m_data` 一次喂给 `png_process_data`，
  libpng 的 progressive 回调逐行填像素，同样一口气到 EOF。
- **GIF**：按帧，一次调用至少解一帧。
- `ImageSource` 的其它入口（`frameDurationAtIndex` / `frameHasAlphaAtIndex` /
  `frameIsCompleteAtIndex`）也会触发解码。
- UI 线程只把 `mirrorBits` BitBlt 到窗口（`agwPaint`，`AgaveWebView.c:1597`），
  所以引擎线程解码时**窗口不假死，但页面整体停摆**：JS、rAF、输入、滚动、下一帧
  全停，最长等于整张图的解码时间。

### 实测（`debug/img_decode_block.html` + `debug/big_image.jpg`，4000×3000 JPEG 551KB，显示 400×300）

```
[cache] frame idx=0 status=2 rows=3000/3000
[perf] layout=0ms paint=467ms total=467ms rect=400x300@0,0
[perf] update paint=468ms js=0ms
页面自测: frames=120 maxgap=100.0ms avg=18.9ms
```

一帧 467ms，脏矩形只有 400×300 —— **不是绘制慢，是解码同步发生**。这就是
"图片解码会阻塞页面"的量级。

## 2. 为什么这是架构问题

引擎里其它重活都已经离开引擎线程或有队列：网络在 `WinHttpSession` 线程 + 引擎线程
`pumpAsyncRequests()`；音视频在 `MediaPipeline` 的 decoder 线程/进程 + 引擎线程轮询
（`RenderMedia` 只取最新帧）；JS 留在引擎线程但按任务切片。**图片解码是最后一个
"按面积无限长、直接跑在引擎线程、还夹在绘制遍历中间"的同步段**。只在 libjpeg 参数上
抠（`JDCT_IFAST`、关 fancy upsampling）是常数优化，不改变"停摆"这件事。

## 3. 方案：四层，可独立上线、收益叠加

### L0 测量（已编码）

- 新增 `src/webcore/platform/graphics/ImageDecodeStats.{h,cpp}`：引擎线程本地累计
  `frameBufferAtIndex()` 的墙钟（QPC），`WebView::paintView()` 每帧 `reset()`，
  `[perf]` 行加 `decode=Nms`（`WebEngine.cpp` 的 `[perf] update` 行同样带）。
- 目的：把"paint 慢"和"一张图解码慢"分开，后面每层的验收都靠它。

### L1 同线程、按预算切片（已实现，R136）

思路：解码变成"每帧解一小片，画已解出的行，下一帧继续"。这是 2009 WebKit 自己的模型，
不需要线程。

- `ImageDecoder` 增加 `setDecodeBudget(maxRows, deadline)` 与 `hasMoreWork()`；JPEG
  `outputScanlines()` 变成有界行循环、PNG 按 64KB 块喂 `png_process_data`（libpng progressive
  reader 自己缓冲，下一块从 `readOffset` 继续），GIF 仍是"一次一帧"（未预算化）。
- `ImageSource::frameBufferAtIndex()` 是所有解码入口的唯一门（含 `ImagePS` 的缩放绘制/图案填充，
  它们原来直接调 `decoder()->frameBufferAtIndex()` 绕过预算）；预算 = `AGB_IMG_DECODE_MS`
  （默认 12ms/帧，按所有图片共享）× `AGB_IMG_DECODE_ROWS`（默认 256 行）；`0` = 旧行为。
- 渐进显示已经现成：`PixelsBuffer::FramePartial` + `ensureHeight()`，`BitmapImage::draw`
  允许画部分帧（`IMAGE_DISPLAY_ONCE` 未定义），未解出的行 alpha=0，画出来就是空白。
- 续解驱动：paint 里画到未完成的帧 → `BitmapImage::requestFrameProgress()` →
  `CachedImage::notifyObservers()` → `RenderImage` 重绘该图 → 下一帧再解一片。
  **不靠 Timer**：paint 内请求的脏矩形现在能活到下一帧（`WebView::paintView()` 改成
  **先取/清脏矩形再 paint**；以前是 paint 完再清，paint 期间请求的重绘会被丢掉）。
  图滚出视口 → 不再被绘制 → 链条自然停止。计数器 4096 次是"解码器永远说还有活"的保险丝。
- 缩放绘制的采样也要跟着预算：`draw_scaled_image()` 的精确盒式滤波对大图是
  100 taps/像素（12MP→400×300 约 85ms，且**每帧**都会重采已解出的行）。现在
  未完成帧每轴 2 taps、完整帧 4 taps（`AGB_IMG_SAMPLE_MAX`，0=精确），并只采样已解出的行。
- 实测（`debug/img_decode_block.html`，4000×3000 JPEG 显示成 400×300，视口 400×800）：
  修前**一帧 467ms**（decode 460ms）、rAF `maxgap=100ms`；修后 35 个解码帧、
  单帧 paint 最大 **20ms**（decode ≤12ms）、rAF `maxgap≈38ms`，全部像素与一次性解码
  **逐字节一致**（`run_image_tests.ps1` 的 JPEG/PNG 两组）。
- 代价：总 CPU 略增（切片期间每帧重采一段已解出的行），12MP 场景约 0.55s → 0.65s；
  L3（按显示尺寸解码）会把这块也一起消掉。
- 风险：paint 内请求重绘的自激循环（预算 + 只有 `hasMoreWork()` 才续 + 计数器兜底）；
  GIF 帧率未动（GIF 一次一帧，大 GIF 仍可能单帧偏长）。

### L2 工作线程解码（已实现，R138）

- 新增 `src/webcore/platform/image-decoders/ImageDecodeService.{h,cpp}`：懒启动 2 条工作线程 +
  任务队列（`CRITICAL_SECTION` + 自动复位事件），模式与 `MediaPipeline`/`WinHttpSession` 一致。
  引擎线程只做三件事：`request(image, bytes, displaySize, generation)`、`poll()`（在
  `WebView::paintView()` 开头，交付的帧能在同一帧画出来）、`Image::~Image()` 里的 `cancel()`。
- **所有权不共享**：工作线程拿到的是编码字节的副本，自己 `createDecoder()`、自己解码，
  产出独立的 `PixelsBuffer`；引擎线程从不碰这个解码器。`Image` 只以裸指针参与"结果是不是你的"
  匹配——`Image` 不是 refcounted，所以 `request()` 不持引用，改为 `cancel()`：图销毁时把它的
  槽位置空，`poll()` 看到就丢弃结果（工作线程从不解引用 Image）。
- 引擎侧：`ImageSource` 增加 adopt 模式（`adoptDecodedFrame()` / `hasDecodedFrame()`），
  画面、尺寸、alpha、动画元数据都从这块帧读；`size()`/`naturalWidth` 仍是原尺寸。
  `BitmapImage` 用 `m_decodePending`/`m_decodeGeneration` 防止重复请求、丢弃过期结果。
- 只对"绘制时"异步：`BitmapImage::draw` 里用 `imageDecodeStatsInFrame()` 判断是不是在 paint
  遍历里。canvas `drawImage()`、打印这些**没有后续重绘**的调用走内联同步路径，并且
  `ImageSource::setDecodeBudgetIgnored(true)` 绕过每帧预算，保证一次拿到整帧。
- **关键修复（背景图消失）**：`Image::drawTiled` 在真正 draw 之前就调
  `nativeImageForCurrentFrame()`；如果这里退回内联解码，会解出一个"预算切片"并被 `m_frames`
  缓存成 ImageRef，之后 adopt 的帧就被这份过期缓存挡住（`background-size:cover` 的图永远不出现）。
  现在 `BitmapImage::nativeImageForCurrentFrame()` 也走异步门：没有 adopt 帧就请求并返回 0，
  绝不缓存半帧。重复背景（`drawPattern`）仍然同步按 natural 尺寸解（瓷砖要按图像像素重复）。
- 动画（GIF 等）仍走原来的逐帧同步路径：服务只产出单帧。
- 开关：`AGB_IMG_ASYNC=0` 回到内联解码（L1 + L3 仍然生效）。
- 实测（4000×3000 JPEG，400×300 显示）：`[perf]` 里不再有 `decode=`（paint 里 0 解码），
  图像由工作线程解好后**逐字节同像素**出现（`run_image_tests.ps1` 的 "off-thread" 组）；
  8 张大图 + 3 次 reload 的压测无崩溃、无新增泄漏；deepseek 登录页（iframe + 二维码）不退化。

### L3 按显示尺寸解码（已实现，R137）

- 收益：12MP JPEG 只解 1/8（500×375）→ decode 从 424ms 降到 **35ms**；采样、内存带宽、
  切片重采的 CPU 一起消失（L1 的 +20% 总量回到基线以下）。
- `BitmapImage::draw()` 在解码前算出 dst 的设备像素尺寸（`ps_world_to_viewport`），
  `ImageSource::setScaleHint()` 存下来并转给解码器；JPEG 在 `jpeg_start_decompress` 之前
  选 `scale_denom`（1/2、1/4、1/8 里"仍然覆盖显示尺寸"的最强那个），libjpeg 用缩小 IDCT
  直接出小图。`size()`/`naturalWidth` 永远是原尺寸，布局不变。
- 缓冲尺寸与图像尺寸不再相同：`PixelsBuffer` 带 `width()/fullHeight()`，`ImageRef` 同时带
  缓冲尺寸和 natural 尺寸，`ImagePS` 画之前用 `bufferRectForSource()` 把 CSS/图像坐标映射到
  缓冲坐标（缩放绘制、1:1 快路径、`drawPattern` 都走它）。
- 重解码只在"已有缓冲比当前显示小"时发生（放大/缩放/窗口变大），且**不会超过 natural**
  （否则显示比原图大的图会每帧重解码）；`AGB_IMG_NO_SCALE=1` 全局关闭（参考/对拍用）。
- 重复背景（`drawPattern`）需要瓷砖按图像像素重复：这种图会切回 natural 解码
  （`BitmapImage::m_useNaturalDecode`），避免瓷砖尺寸被缩小。
- **依赖**：仓库自带的 libjpeg 默认把 `IDCT_SCALING_SUPPORTED` 关掉了（`dist/include/jmorecfg.h`
  的 Mozilla 裁剪），所以之前设 `scale_denom` 完全无效。修法：`jmorecfg.h` 打开该宏 +
  `dist/include/jconfig.h` 定义它 + 从 `vcproj/.../libjpeg-turbo-1.4.1/jidctred.c` 补回缺失的
  `jidctred.c`（1/2、1/4、1/8 的 IDCT）。⚠ 这个源码树是按 glob 收文件的，新增 .cpp/.c 后要
  重新跑一次 `cmake -S . -B build-zig-native`（`dist/allinone.cmake` 的 glob 没有
  `CONFIGURE_DEPENDS`）。
- 调试：`AGB_IMG_SCALE_DEBUG=1` 打 `[img-scale] jpeg natural=… display=… denom=… out=…`。


### 不建议的路线

- **多进程 image service**（Chrome 的做法）：这里没有隔离需求，媒体已经有 ffmpeg 子进程；
  一条解码线程足够，复杂度低一个量级。
- **只调 libjpeg 参数**：`JDCT_IFAST`/关 fancy upsampling 约 1.5–2×，量级不够，且伤画质。
- **把图片画成占位不重绘**：等于 L1 的退化版，会丢渐进效果。

## 4. 顺序、工作量与验收

| 层 | 工作量 | 验收 |
|---|---|---|
| L0 | 1h | `AGB_PERF_LOG=1` 的 `[perf]` 行出现 `decode=`；大图页 `decode≈paint` ✅ |
| L1 | ~0.5d | `debug/img_decode_block.html`：`paint<20ms`、rAF `maxgap<40ms`、像素与一次性解码逐字节一致 ✅（`run_image_tests.ps1`） |
| L3 | ~1d | 大图按 1/8 解码后 `decode` 从 424ms 降到 35ms（`run_image_tests.ps1` 的 L3 组）✅ |
| L2 | 1–2d | 同页 paint 里 `decode=0ms`、off-thread 帧与内联帧逐字节一致、8 张图 × 3 reload 无崩溃 ✅（`run_image_tests.ps1` 的 L2 组） |

每层都进 `tools/run_image_tests.ps1`：预算上限、渐进可见、最终像素、off-thread 像素、L3 加速比。
L1 与 L2 是互补的：`AGB_IMG_ASYNC=0` 时 L1 仍是完整可用的内联路径（也是动画/canvas 的后备）。

## 5. 剩余与风险

- **动画（GIF/APNG）仍在 paint 里逐帧同步解码**：服务只产出单帧；下一步可以按帧请求
  （`advanceAnimation` 只请求、不等待），但 GIF 帧依赖前帧合成，需要把帧序列搬进 job。
- **canvas `drawImage(<img>)` 目前画不出东西**（`getImageData` 也没有）：与本次无关的既有缺口
  （`CanvasRenderingContext2D::drawImage` 走到 `GraphicsContext::drawImage` 后没有任何像素落地），
  已记进 [arch-capability-gaps.md](arch-capability-gaps.md)。L2 已经为它留好同步路径
  （canvas 不在 paint 遍历里 → 内联 + 无预算），修好 canvas 本身即可。
- **PNG 没有 DCT 级缩放**：L3 只对 JPEG 生效；PNG 的 inflate 成本省不了，但可以在 worker 上跑
  （L2 已经把它挪出引擎线程）。
- **图片内存**：`decodedSize` 与 `Cache::prune` 没变；worker 手上的那份帧在 adopt 前不计入，
  极端情况下峰值多一帧（L3 之后很小）。
- **`img.decode()`** 仍是立即 resolve 的 stub；L2 之后更有意义的是"等 adopt"，
  但这需要从 C++ 通知 JS promise（`CachedImage::imageFrameReady` 已经是个现成的挂点）。
