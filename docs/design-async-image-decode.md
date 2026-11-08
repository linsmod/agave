# 设计：图片解码不要发生在 paint 里（异步/预算化解码）

状态：**L0 + L1 已实现（R136，2026-11-08）；L2/L3 待做**。现状见 [arch-rendering.md](arch-rendering.md)，
历史根因见 [hist-lessons.md](hist-lessons.md)，媒体侧已有的异步解码先例见 [arch-media.md](arch-media.md)。

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

### L2 工作线程解码（待做）

- 新增 `ImageDecodeService`（`src/webcore/platform/image-decoders/`）：懒启动 1 条线程 + 任务队列。
  `DecodeJob { RefPtr<Image> keepAlive; ImageSource* source; generation; rowsBudget; }`。
- 所有权移交：引擎线程把该图的解码交给队列并置 `DecodeRunning`；paint **只 peek**，不再调
  `frameBufferAtIndex()`。
- 行级发布：worker 每解完一段用 `InterlockedExchange` 发布 `publishedHeight`，引擎线程只读
  ≤ 该高度的行（x86 写后发布天然有序）；alpha 标志同样发布。
- 完成/进度通知：worker 只把结果入队；引擎线程在 `macross_event_dispatch()` 或
  `paintView()` 开头 `ImageDecodeService::poll()` → `CachedImage::notifyObservers()` → 重绘。
  与 `pumpAsyncRequests()`/`RenderMedia` 轮询是同一套模式。
- 生命周期：job 持 `RefPtr<Image>`；图被销毁/换源时置 cancel，worker 解完丢弃结果。
  `BitmapImage::destroyDecodedData()` 遇到 running 只清"已解出的帧"，不碰 decoder。
- 预解码：`RenderImage::attach()`/布局后把视口内的图按文档顺序入队，首帧前大概率已就绪
  （日志 `[img] ahead hit/miss`）。
- 线程边界：worker 只能碰 `ImageDecoder`/`PixelsBuffer`/`SharedBuffer` 引用，绝不碰
  DOM/RenderObject/RenderArena；libjpeg/libpng 每实例独立、`FastMalloc` 有锁，可以并发。
- 风险：线程生命周期与取消路径（最难测试的一层）；退出/挂起要按 `MediaPipeline::stop()`
  的模式停线程。测试必须包含反复导航、关窗、`AGB_*` 压力开关。
- 收益：paint 的 `decode=` 掉到 0，解码与 JS/布局完全并行；配合 L1 的切片，worker 也不会
  单次霸占 460ms（对大图更平滑）。

### L3 按显示尺寸解码（待做，下一个杠杆）

- 收益现在更清楚了：L1 之后单帧停摆已经解决，剩下的痛点是**总量**——12MP 源即使只显示
  400×300，也要解 12M 像素（≈450ms）+ 采样。按显示尺寸解码（`jpeg_start_decompress` 前设
  `scale_denom` 1/2、1/4、1/8）把两者同时降到 1/16 甚至更多。
- `BitmapImage::draw()` 知道 dst，第一次解码前把目标尺寸 hint 给 `ImageSource`：
  long side ≤ dst*1.15（按 zoom 折算）。
- JPEG：`jpeg_start_decompress` 之前设 `scale_num/scale_denom`（1/1,1/2,1/4,1/8）；
  PNG：zlib inflate 省不了，但可以按行抽样写像素（省内存带宽和后续采样）。
- `naturalWidth/Height` 仍报原尺寸（布局不变），只改像素缓冲尺寸；`ImagePS` 的
  src→dst 映射改用缓冲实际尺寸（`ImageRef::width/height/pitch` 从 buffer 取）。
- 只在需要的 scale 变化 ≥2× 时重解码（滞后），`AGB_IMG_NO_SCALE=1` 兜底。
- 收益：12MP 显示 400×300 → libjpeg 工作量 ≈1/16；缩略图场景接近免费。

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
| L2 | 1–2d | 同页 `[perf] ... decode=0ms`；`[img] ahead hit` 占多数；反复导航/关窗 50 次无线程泄漏或 UAF；`fuzz_layer` + v.qq/deepseek smoke |
| L3 | ~1d | 大图按 1/8 解码后 `decode` 降至 1/4 以下；`getBoundingClientRect`/`naturalWidth` 不变；`AGB_IMG_NO_SCALE=1` 行为退化到 L2 |

每层都进 `tools/run_image_tests.ps1`（新增）：预算上限、渐进可见、最终像素、动画帧、
取消路径。L1 与 L2 是互补的：L2 里去线程化的解码器仍然要能切片，否则 worker 单张图
会占满整条线程，取消/关闭时的等待也会变长。

## 5. 风险与开放问题

- **部分帧的观感**：L1/L2 都会先显示已解出的行（下方为空/黑）。Chrome 也这样；若要更平滑，
  可以在部分帧上撒一次缩略图（先用低分辨率整帧，再逐行细化），但这需要 decoder 支持多分辨率
  输出，本期不做。
- **GIF/APNG 动画**：`BitmapImage` 的 `Timer` 现在直接 `frameAtIndex()`（同步解码下一帧）。
  L2 之后动画帧也走队列，`advanceAnimation` 只请求、不等待；帧率以"解完即推进"为准。
- **图片内存**：`decodedSize` 与 `Cache::prune` 已经存在，异步化不改变预算，但要保证
  "worker 正在解的帧"计入 decoded 预算（否则会被 prune 掉一半）。
- **测试稳定性**：L2 引入线程，几何/像素断言要等"首帧解码完成"（`img.decode()` 的 Promise
  正好可以在这里落地：目前 `img.decode` 的路径见 `AntRuntime.cpp` 的 `imgDecode`）。
