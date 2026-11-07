# 媒体元素：`<video>` / `<audio>` / `<track>`

> 现状与实现边界。历史缺陷进 [hist-lessons.md](hist-lessons.md)，待办进 [plan-roadmap.md](plan-roadmap.md)。
> 目标不是做完整媒体播放栈，而是让**真实站点里用到的媒体 API 表面**存在、语义对、
> 尺寸对、事件（时间线样式）对，并且**真的播**：音频经 waveOut 出声，视频按解码帧逐帧绘制。

## 1. 目标（本次范围）

1. `document.createElement('video' | 'audio' | 'source' | 'track')` 得到**正确的元素类**
   （`HTMLVideoElement` / `HTMLAudioElement` / `HTMLSourceElement` / `HTMLTrackElement`），
   `constructor.name`、`instanceof` 一致。
2. 常用 API 存在且状态自洽：`play()`/`pause()`/`load()`/`canPlayType()`/`currentTime`/
   `duration`/`paused`/`ended`/`volume`/`muted`/`playbackRate`/`readyState`/`networkState`/
   `buffered`/`played`/`seekable`/`videoWidth`/`videoHeight`/`poster`/`controls`/`autoplay`/`loop`/
   `preload`/`src`/`currentSrc`/`error`/`textTracks`/`addTextTrack()`。
3. 挂载后**渲染尺寸正确**：走 WebCore 的 replaced-element 布局（`RenderReplaced` 派生），
   默认尺寸与 Chrome 对齐（见 §3），`width`/`height` 属性与 CSS 都生效，`getBoundingClientRect()`
   返回真实几何。
4. **真实播放**：音频经 waveOut 送到声卡，视频由 ffmpeg 解码成 BGRA 帧逐帧绘制；
   `currentTime` 由**播放时钟**（waveOut 设备位置 / 墙钟）驱动，`pause()` 冻结、
   seek 重开解码器、到 `duration` 发 `ended`（见 §4 与 §7）。装了 ffmpeg 才有真实播放，
   没有时退化成内部定时器模拟（§4.1），页面不会看到"死"元素。
5. 元数据（`duration`/`videoWidth`/`videoHeight`）来自**内置解析器**（MP4/WebM/MKV/Ogg 头），
   解析不了再交给 `ffprobe`（可选外部进程，见 §6）。
6. 画面：`<video>` 有解码帧画解码帧，没有帧时画 `poster`，两者都没有画黑底（Chrome 同样）。
7. **顶层导航到媒体 URL**（直接打开 `.mp4` / `.mp3` / `Content-Type: video|audio`）不下载，
   而是渲染内置播放器文档（§7.2）：黑底、单个 `<video controls autoplay>` / `<audio controls autoplay>`，
   标题是文件名 —— 与 Chrome 打开裸媒体链接的行为一致。
8. `<track>` + WebVTT：`textTracks`、`addTextTrack`、cue 解析与 `cuechange`；字幕**不**绘制
   （没有字幕渲染器，页面自己用 `cue` 画）。

明确不做：MSE（`MediaSource`）、EME、WebAudio、`requestVideoFrameCallback`、captureStream、
picture-in-picture、全屏控件皮肤、字幕渲染、渐进式（边下边播）网络流。

## 2. 分层

```
DOM 层   HTMLMediaElement / HTMLVideoElement / HTMLAudioElement / HTMLSourceElement / HTMLTrackElement
         src 解析、networkState / readyState / paused 状态机、事件派发（走 dispatchHTMLEvent → AntRuntime）
播放层   MediaPipeline（每个播放中的元素一个）：ffmpeg 进程 + waveOut + 时钟 + 最新帧
渲染层   RenderMedia（RenderReplaced 派生）：intrinsic size、poster / 解码帧绘制、object-fit（未实现）
解码层   MediaMetadata（内置 header 解析） + MediaDecoderBackend（ffprobe / ffmpeg，可替换、异步）
JS 层    AntRuntime 的 nodeGetter/nodeSetter 分支 + Node 类方法（play/pause/load/canPlayType/addTextTrack）
```

解码层是**唯一**允许起外部进程或长时间阻塞的地方，接口固定为：

```cpp
class MediaDecoderBackend {
public:
    // 同步读头，只解析文件/内存里的容器头，不解码。
    virtual bool probe(const String& url, MediaInfo& out) = 0;
    // 抽一帧 JPEG/PNG 到 out（异步由调用方决定是否用进程）。
    virtual bool grabFrame(const String& url, double time, SharedBuffer** out) = 0;
};
```

内置头解析器是 `probe()` 的默认实现（`MediaMetadata.cpp`），`ffprobe` 是兜底实现；
`grabFrame()` 只有 ffmpeg 一种实现（实时播放走 `MediaPipeline`，那个一次性的静帧路径
只在 `AGB_MEDIA_FRAMES=1` 时用）。

## 3. UA 版式（Chrome 实测，2026-11-07，`tools/ua_style_dump.py`）

Chrome 的 UA 规则（`CSS.getMatchedStylesForNode` 的 `origin=user-agent`）：

```css
audio:not([controls]) { display: none !important }
audio { width: 300px; height: 54px }
video { object-fit: contain }
video, canvas, img { overflow-x: clip; overflow-y: clip; overflow-clip-margin: content-box }
```

`video` 的 300x150 不来自 UA 规则，而是 **replaced 元素的缺省固有尺寸 300x150**
（`RenderReplaced` 的 `m_intrinsicSize` 初值就是 `300x150`，与 Chrome 一致）：

| 场景 | Chrome computed | rect |
|---|---|---|
| `<video>` 无属性 | `300x150` | `300x150` |
| `<audio>` 无 `controls` | `display:none` | `0x0` |
| `<audio controls>`（空） | `300x54` | `300x54` |
| `<video width=200 height=100>` | 属性映射成 CSS 宽高 | `200x100` |

落到 agave 的 `html4.css`（手写，不用生成器）：

```css
audio:not([controls]) { display: none !important }
audio { width: 300px; height: 54px }
video { object-fit: contain }
video, canvas, img { overflow-x: hidden; overflow-y: hidden }
```

- `overflow: clip` 不在 2008 语法里，用 `hidden` 近似（裁剪语义一致，只是可滚动性不同；
  replaced 元素本来没有可滚动内容）。
- `object-fit` 目前不参与布局，只记录 computed 值（见 §8 缺口）。
- 300x150 由 `HTMLVideoElement::createRenderer()` 传 `RenderReplaced(this, IntSize(300, 150))`
  实现，**不要**写进 UA 表（写进 UA 表会连 `width:auto` 的百分比场景一起锁死）。

## 4. 状态机

```
networkState: EMPTY(0) --src--> LOADING(2) --metadata--> IDLE(1) / NO_SOURCE(3)
readyState:   HAVE_NOTHING(0) -> HAVE_METADATA(1) -> HAVE_CURRENT_DATA(2) -> HAVE_FUTURE_DATA(3) -> HAVE_ENOUGH_DATA(4)
paused:       true --play()--> false --pause()--> true
```

- `src` 设定（含 `<source>` 回退、`<source>` 插入时）触发 `load()`：`networkState=LOADING`，
  派发 `loadstart`；元数据到手后 `duration`/`videoWidth`/`videoHeight` 有效，
  `readyState=HAVE_METADATA`，派发 `loadedmetadata` → `loadeddata` → `canplay`。
- `play()` 返回 Promise（立刻 resolve），`paused=false`；装了解码器时启动
  `MediaPipeline`（§4.1），`currentTime` 由**播放时钟**通过 50ms 的 `Timer` 报告；
  派发 `play` → `playing` → 周期性 `timeupdate`。
- `pause()`：`paused=true`，冻结时钟（`waveOutPause`），派发 `pause`。管线**保留**
  （热恢复：不重开进程、不重开设备），`play()` 只是 `setPaused(false)`。
- `currentTime` 赋值：夹到 `[0, duration]`，重开解码器到新位置（`-ss`），派发 `seeking` + `seeked`。
- 到 `duration`：`ended=true`、`paused=true`，停掉管线，派发 `timeupdate` → `ended`；`loop` 时归零继续。
- `autoplay`：元数据到手后自动 `play()`。
- 没有源 / 解析失败：`error`（`MediaError` 对象，`code=4` `MEDIA_ERR_SRC_NOT_SUPPORTED`），
  `networkState=NO_SOURCE`，派发 `error`。**不要**静默：站点靠这个事件换源。
  失败的元素 `play()` **不**启动解码器（与 Chrome 一致：宁可不播也不要"有声无时长"）。
- 事件都走 `EventTargetNode::dispatchHTMLEvent(type, false, false)`（不冒泡，与 Chrome 一致），
  这样 `addEventListener` 与 `el.onxxx = fn` 两条路都会到（R27 的机制）。

### 4.1 真实播放（`MediaPipeline`）

`src/webcore/platform/media/MediaPipeline.cpp`，一个播放中的元素一个实例：

```
音频： ffmpeg -ss <seek> -i <file> -vn -f s16le -acodec pcm_s16le -ar 44100 -ac 2 -
        → 音频线程按 6×100ms 的 waveOut 环形缓冲喂设备（设备自己限速）
视频： ffmpeg -ss <seek> -i <file> -an -vf fps=25,scale=<WxH> -f rawvideo -pix_fmt bgra -
        → 视频线程按**帧时间戳**等时钟到点后才发布最新帧（只保留最新一帧）
```

- **时钟**：`currentTime()` = `max(墙钟 - 暂停时长 + seekBase, waveOut 设备位置 + seekBase)`。
  设备位置就是"用户实际听到的位置"，所以它领先时以它为准；视频线程按同一个时钟节流，
  于是音画大致同步，且**不会**出现"ffmpeg 全速解码到 EOF、元素从第一秒就显示最后一帧"。
- `-ss` 必须传给 ffmpeg（在 `-i` 之前）：seek 就是重开两个解码器，`seekBase` 只用来对齐时间戳。
- `pause` 用 `waveOutPause`/`waveOutRestart`，并在墙钟里累计暂停时长；视频线程的等待
  看到时钟不动就不发布新帧，所以暂停立即生效。
- `volume`/`muted` 走 `waveOutSetVolume`（0..1 → 16bit 双声道）。
- 线程契约：WebCore 对象只在引擎线程；解码线程只碰 `Impl`；最新帧用临界区保护。
  `RenderMedia` 每次 paint 拷一份当前帧（复用成员缓冲，避免 30Hz 反复分配）。
- `_agbRealPlayback`（JS 诊断属性，非标准）：1 = 解码器在驱动这个元素，0 = 定时器模拟。
  `tools/run_media_real_tests.ps1` 用它区分两条路径。
- 没有 ffmpeg / 起不来：`start()` 返回 false，元素退回 §4 的定时器模拟（页面可用，不出声）。

## 5. `canPlayType()`

按扩展名/`Content-Type` 给答案，与 Chrome 对 `<video src="x.mp4">` 的观感一致：

| 输入 | 返回 |
|---|---|
| `video/mp4`、`video/webm`、`audio/mp4`、`audio/mpeg`、`audio/ogg`、`audio/wav` | `"probably"` |
| `video/ogg`、`video/x-matroska`、`audio/flac` | `"maybe"` |
| `application/x-mpegURL`（HLS）、`video/mp2t` | `"maybe"`（真的播放不了，但别让站点走 `''` 分支禁用播放器） |
| 空串 / 未知 | `""` |

`canPlayType('')` 返回 `""`（Chrome 亦然）。这是**能力声明**，与 §7 的真实解码能力解耦：
播放器逻辑要的是"能不能选这个源"，不是"字节能不能解"。

## 6. 元数据解析

`src/webcore/platform/media/MediaMetadata.cpp`（新目录 `src/webcore/platform/media/`，
`src.cmake` 的 `platform/*.cpp` **不含**子目录，需要在 CMake 里补一行 glob）。

- **MP4/MOV**（`ftyp`）：遍历 box，取 `mvhd`（duration/timescale）、`tkhd`
  （width/height，16.16 定点先取整数部分，再乘矩阵缩放）。
- **WebM/MKV**（EBML）：`Segment → Info`（`TimecodeScale`、`Duration`）、
  `TrackEntry → Video`（`PixelWidth`/`PixelHeight`）；EBML vint 解析。
- **Ogg**：`OggS` 页头 + `vorbis`/`opus`/`theora` 的 id header 取采样率/时长（时长靠末页 granule，
  拿不到就退化到 `Infinity`）。
- 其余容器返回失败，交给 ffprobe。

`MediaInfo { bool ok; double duration; int width, height; bool hasVideo, hasAudio; String mimeType; }`。

## 7. 画面

实时播放时 `RenderMedia::paint` 直接从管线取**当前帧**（BGRA，尺寸就是管线的
`videoWidth × videoHeight`），按内容盒缩放后绘制：

- 缩放在本函数里自己做盒式滤波，然后 `ps_set_source_image` + `ps_fill` 画 1:1。
  这是 ImagePS 对 `<img>` 的同一条路：picasso 的采样器画缩放源会"鬼影"，
  而 `ps_canvas_create_with_data` 包装外部缓冲再 `ps_canvas_bitblt` 在本引擎里
  **画不出来**（试过，见 hist-lessons R99），别再用那条路。
- 每帧 921KB（640×360）在 30Hz 下反复分配太浪费，`RenderMedia` 复用一个成员缓冲。
- 没有公网/本地可达的解码源时：画 `poster`，没有 poster 画黑底。

一次性静帧路径（`AGB_MEDIA_FRAMES=1`，默认关）仍在，供"没有实时管线也要看一帧"的调试：
`<video>` 元数据到手后按 `src` + `currentTime` 调 ffmpeg 出一张 PNG
（`ffmpeg -v error -ss <t> -i <path> -frames:v 1 -f image2pipe -vcodec png -`），
在独立线程跑、引擎线程 `pollFrame()` 取回，转 `BitmapImage` → `CachedImage` 交给渲染器。

## 7.1 网络媒体：引擎自己下载，工具只读本地文件

ffmpeg/ffprobe 是本引擎 fork 出来的子进程。**在本进程树里它们完成不了 TLS 握手**
（报 `Peer certificate failed verification`，而同一个 ffprobe 从普通命令行跑没问题），
所以 http(s) 媒体不交给工具去联网：

1. `HTMLMediaElement` 的元数据探测对 http(s) 走引擎自己的网络栈（`ResourceHandle::
   loadResourceSynchronously`），拿到 body 后既解析容器头，也写一份本地副本；
2. 副本放在 `AGB_MEDIA_CACHE`（显式指定）→ `%LOCALAPPDATA%\Agave\cache\media` →
   `%TEMP%\agave-media-cache` → `%LOCALAPPDATA%\Agave\probe\media-cache` → exe 旁的
   `media-cache`（第一个**实测可写**的目录），文件名是 URL 的 FNV-1a 哈希 + 原扩展名；
3. `MediaPipeline` 用这份本地副本起解码进程，`-ss` 因此是瞬时的。

代价（写进 §8）：远程媒体要**整段下载完**才开始播，没有渐进式流；远程 body 还会
在内存里过一遍（同步 API 的限制）。探测是同步的，大文件会卡住引擎线程。

## 7.2 顶层导航到媒体 URL：媒体文档（`MediaDocument`）

`src/webcore/loader/MediaDocument.cpp`（与 `ImageDocument` / `PluginDocument` 同一套路）：

- 判定在 `FrameLoaderClientMC::canShowMIMEType()`：`MIMETypeRegistry::isSupportedMediaMIMEType()`
  对 `audio/*`、`video/*`（加 `application/ogg`、HLS 的 `application/vnd.apple.mpegurl`）返回真，
  于是策略是 `PolicyUse` 而不是 `PolicyDownload`（`Content-Disposition: attachment` 仍然优先）。
- `DOMImplementation::createDocument()` 遇到媒体类型返回 `MediaDocument`；它的 tokenizer
  `wantsRawData()`，把字节全部丢弃（不解析二进制），第一个字节到达时建结构：
  ```
  <html><head><title>文件名</title><style>…黑底、居中、max-width/height 100%…</style></head>
        <body><video|audio controls autoplay src="文档 URL"></body></html>
  ```
- `autoplay` 让元素在元数据到手后自己 `play()`，解码走 §4.1 的管线；音频文档的元素宽度 100%。
- 元素是**自己再拉一次**资源的（文档那份字节被丢掉），所以远程直开会下载两遍；
  `file:` 直开没有代价。见 §8。

## 8. 已知缺口（记录，不在本次修）

- **媒体文档不居中（垂直方向）**：`body { display: flex; align-items: center }` 在本引擎里
  只居中了水平方向（`justify-content` 生效，`align-items` 没有作用到 replaced 元素），
  所以视频贴顶；Chrome 是水平+垂直居中。
- **媒体文档会把资源下载两遍**（文档主资源 + 元素自己的加载）。远程直开时是双倍流量；
  正确做法是把主资源的字节交给元素（`setLoadManually` 式的通道），或者边下边解。
- **没有渐进式/流式播放**：http(s) 媒体先整段下载（§7.1），大文件既卡加载又占内存；
  需要 Range 请求 + 边下边解（或 `MediaSource`）。
- **没有音画严格同步**：视频按共享时钟节流，长时间抖动会累积（设备时钟领先时以它为准，
  但视频帧不按 PTS 精确对齐）。
- **解码全在 CPU**：每路播放一个 ffmpeg 进程 + 每帧一次缩放拷贝，4K 会很吃力；
  没有硬解、没有零拷贝。
- `playbackRate` 只影响视频节流与模拟推进；音频解码仍是原速（变速会音画不同步）。
- `object-fit` / `object-position` 不参与绘制与布局（只记录 computed）。站点用
  `object-fit: cover` 时画面会被拉伸而不是裁剪。
- 没有原生控件皮肤：`controls` 只影响 `audio` 的 UA 尺寸与 `audio:not([controls])` 的可见性。
  `<video controls>` 不画播放条（页面自绘的播放器不受影响）。
- Chrome 的 `<audio controls>` 计算值是 `display: inline` 但仍有 300x54 的盒子；
  本引擎没有媒体控制盒，UA 表里用 `inline-block` 复现几何（computed 因此是 `inline-block`）。
- `overflow: clip` 用 `hidden` 近似（2008 没有 clip 关键字）；replaced 元素没有可滚动内容，
  实践中等价。
- `MediaSource` / `WebMediaPlayer` / EME / WebAudio 未实现；用它们的站点（hls.js、dash.js、
  直播）走"不支持"分支。
- 字幕不绘制，只有 API 与 cue 事件（`cues`/`activeCues` 是快照数组）。
- `requestVideoFrameCallback` 不存在。
- `currentTime` 的精度受 50ms 定时器限制；`seeked` 同步派发（Chrome 是异步任务）。
- `buffered`/`played`/`seekable` 是快照（`[0, currentTime]` 或 `[0, duration]`），不是 live 对象。
- `ended` 之后管线被销毁（`real` 变 0），`currentTime` 停在 `duration`；再次 `play()` 从头开始。

## 9. 复测

```powershell
# DOM/API 表面 + 尺寸 + <source> 回退 + new Audio()
build-zig-native\agave_shell.exe --probe tools\pages\cap_media.html --wait 800 --eval "probeExit(window.reportMedia())"
# 传输状态机（play → 前进 → seek → ended）
build-zig-native\agave_shell.exe --probe tools\pages\cap_media_state.html --wait 900 --eval-file tools\pages\cap_media_state.js --settle 2500
# 整组回归（上面两页 + source 回退）
pwsh -File tools\run_media_tests.ps1
# 真实播放：本地视频 / 远程视频 / 本地 MP3，含画面与 ffmpeg 参考帧对拍
pwsh -File tools\run_media_real_tests.ps1
```

真实播放的单项探针（`real=1` 表示解码器在驱动）：

```powershell
$page='tools\pages\cap_media_real.html'
# 本地视频：play/pause/seek/resume
build-zig-native\agave_shell.exe --probe "$page`?src=../../debug/media/clip640x360.mp4" `
  --eval-file tools\pages\cap_media_real_state.js --wait 1200 --settle 6000
# 远程视频
build-zig-native\agave_shell.exe --probe "$page`?src=https%3A%2F%2Fartplayer.org%2Fassets%2Fsample%2Fvideo.mp4" `
  --eval-file tools\pages\cap_media_real.js --wait 3000 --settle 6000
# 本地音频（绝对 file: URL 也支持）
build-zig-native\agave_shell.exe --probe "$page`?kind=audio&src=file%3A%2F%2F%2FH%3A%2Fmusic.mp3" `
  --eval-file tools\pages\cap_media_real_state.js --wait 1500 --settle 7000
# 画面：?t=<秒>&hold=1 让页面自己停在那个位置，--shot 再抓图
build-zig-native\agave_shell.exe --probe "$page`?src=../../debug/media/clip640x360.mp4&t=0&hold=1" --eval 0 --wait 3000 --shot build-zig-native\frame.bmp
# 直接打开媒体链接/文件：应该是内置播放器而不是下载
build-zig-native\agave_shell.exe --probe "https://artplayer.org/assets/sample/video.mp4" --wait 4000 `
  --eval "var v=document.querySelector('video'); probeExit(document.title+' '+v.tagName+' ct='+v.currentTime.toFixed(2)+' real='+v._agbRealPlayback)"
```

诊断开关：`AGB_MEDIA_DEBUG=1`（管线启动、时钟/设备位置、首帧、ffprobe 输出）、
`AGB_MEDIA_PROBE=ffprobe`（强制外部探测）、`AGB_MEDIA_FRAMES=1`（一次性静帧）、
`AGB_MEDIA_CACHE=<dir>`（下载副本目录）。
对拍 Chrome 的 UA/尺寸用 `tools/ua_style_dump.py`（需要先起一个
`--remote-debugging-port=9333` 的 headless Chrome，见脚本头注释）。

测试素材由 ffmpeg 生成，放在 `debug/media/`（已 git-ignore）：
`clip640x360.mp4`（H.264 5s）、`clip320x240.webm`（VP9 3s）、`clip176x144.mkv`（2s）、
`artplayer_video.mp4`（远程样片的本地副本，90s）。
