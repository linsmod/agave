# 媒体元素：`<video>` / `<audio>` / `<track>`

> 现状与实现边界。历史缺陷进 [hist-lessons.md](hist-lessons.md)，待办进 [plan-roadmap.md](plan-roadmap.md)。
> 目标不是做完整媒体播放栈，而是让**真实站点里用到的媒体 API 表面**存在、语义对、
> 尺寸对、事件（时间线样式）对。画面先做**首帧静帧**，真实播放留到后面。

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
4. **模拟播放状态**：`play()` 后 `paused=false`，用 WebCore `Timer` 推进 `currentTime`，
   到 `duration` 发 `ended`；`pause()` 停住。不产生声音、不真的解码视频流。
5. 元数据（`duration`/`videoWidth`/`videoHeight`）来自**内置解析器**（MP4/WebM/MKV/Ogg 头），
   解析不了再交给 `ffprobe`（可选外部进程，见 §6）。
6. 画面：默认只画 `poster`；`<video>` 有可解码首帧时画首帧（ffmpeg 抽帧，见 §7，默认关）。
7. `<track>` + WebVTT：`textTracks`、`addTextTrack`、cue 解析与 `cuechange`；字幕**不**绘制
   （没有字幕渲染器，页面自己用 `cue` 画）。

明确不做：音频输出、A/V 同步、MSE（`MediaSource`）、EME、WebAudio、
`requestVideoFrameCallback`、captureStream、picture-in-picture、全屏控件皮肤。

## 2. 分层

```
DOM 层   HTMLMediaElement / HTMLVideoElement / HTMLAudioElement / HTMLSourceElement / HTMLTrackElement
         src 解析、networkState / readyState / paused 状态机、事件派发（走 dispatchHTMLEvent → AntRuntime）
渲染层   RenderMedia（RenderReplaced 派生）：intrinsic size、poster/首帧绘制、object-fit
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
`grabFrame()` 只有 ffmpeg 一种实现，默认关闭（`AGB_MEDIA_FRAMES=1` 打开）。

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
- `play()` 返回 Promise（立刻 resolve），`paused=false`，用 50ms 的 `Timer` 推进
  `currentTime += 0.05 * playbackRate`（按真实经过时间算，避免掉帧漂移）；
  派发 `play` → `playing` → 周期性 `timeupdate`。
- `pause()`：`paused=true`，派发 `pause`。
- `currentTime` 赋值：夹到 `[0, duration]`，派发 `seeking` + `seeked`（同步派发，够用）。
- 到 `duration`：`ended=true`、`paused=true`，派发 `timeupdate` → `ended`；`loop` 时归零继续。
- `autoplay`：元数据到手后自动 `play()`。
- 没有源 / 解析失败：`error`（`MediaError` 对象，`code=4` `MEDIA_ERR_SRC_NOT_SUPPORTED`），
  `networkState=NO_SOURCE`，派发 `error`。**不要**静默：站点靠这个事件换源。
- 事件都走 `EventTargetNode::dispatchHTMLEvent(type, false, false)`（不冒泡，与 Chrome 一致），
  这样 `addEventListener` 与 `el.onxxx = fn` 两条路都会到（R27 的机制）。

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

## 7. 画面（可选，默认关）

`AGB_MEDIA_FRAMES=1` 时，`<video>` 元数据到手后按 `src` + `currentTime` 调 ffmpeg：

```
ffmpeg -v error -ss <t> -i <path> -frames:v 1 -f image2pipe -vcodec png -
```

- `CreateProcessA(CREATE_NO_WINDOW)` + 匿名管道，**在独立线程**跑（`jobThread`），
  引擎线程只在 `HTMLMediaElement::frameTimerFired()` 里 `pollFrame()` 取回结果，
  转成 `BitmapImage` → `CachedImage` 交给 `RenderMedia` 画。没有 WebCore 对象跨线程。
- 工具参数是**文件路径**而不是 URL（`file:` 直接给 ffprobe/ffmpeg 会 `No such file or directory`）：
  `toolArgumentForURL()` 做 `file:///E:/x` → `E:/x`，http(s) 原样传。
- 找不到 ffmpeg（`AGB_FFMPEG` / 巧克力/Program Files/PATH）：静默退化到只画 poster，
  **不要**报错打断页面。默认（不设 `AGB_MEDIA_FRAMES`）根本不抽帧，画黑底。
- URL 只接受 `file:` / `http(s):`，参数走 `CreateProcess` 的 argv（逐项加引号），
  禁止把 URL 拼进命令行字符串。
- 已知限制：只取首帧（`-ss 0`），`currentTime` 变化不重新抽帧；`object-fit` 不生效。

## 8. 已知缺口（记录，不在本次修）

- `object-fit` / `object-position` 不参与绘制与布局（只记录 computed）。站点用
  `object-fit: cover` 时首帧会被拉伸而不是裁剪。
- 没有原生控件皮肤：`controls` 只影响 `audio` 的 UA 尺寸与 `audio:not([controls])` 的可见性。
  `<video controls>` 不画播放条（页面自绘的播放器不受影响）。
- Chrome 的 `<audio controls>` 计算值是 `display: inline` 但仍有 300x54 的盒子；
  本引擎没有媒体控制盒，UA 表里用 `inline-block` 复现几何（computed 因此是 `inline-block`）。
- `overflow: clip` 用 `hidden` 近似（2008 没有 clip 关键字）；replaced 元素没有可滚动内容，
  实践中等价。
- `MediaSource` / `WebMediaPlayer` / EME / WebAudio 未实现；用它们的站点（hls.js、dash.js、
  直播）走"不支持"分支。
- 字幕不绘制，只有 API 与 cue 事件（`cues`/`activeCues` 是快照数组）。
- `playbackRate` 只影响模拟推进速度。
- `requestVideoFrameCallback` 不存在。
- `currentTime` 的精度受 50ms 定时器限制；`seeked` 同步派发（Chrome 是异步任务）。
- `buffered`/`played`/`seekable` 是快照（`[0, currentTime]` 或 `[0, duration]`），不是 live 对象。

## 9. 复测

```powershell
# DOM/API 表面 + 尺寸 + <source> 回退 + new Audio()
build-zig-native\agave_shell.exe --probe tools\pages\cap_media.html --wait 800 --eval "probeExit(window.reportMedia())"
# 播放状态机（play → 前进 → seek → ended），逐帧推进由页面定时器驱动
build-zig-native\agave_shell.exe --probe tools\pages\cap_media_state.html --wait 900 --eval-file tools\pages\cap_media_state.js --settle 2500
# 整组回归（脚本内断言上面两页 + source 回退）
pwsh -File tools\run_media_tests.ps1
```

强制外部解码器（验证 ffprobe 兜底）：`$env:AGB_MEDIA_PROBE='ffprobe'`。
抽帧画面：`$env:AGB_MEDIA_FRAMES='1'` + `--shot`。
对拍 Chrome 的 UA/尺寸用 `tools/ua_style_dump.py`（需要先起一个
`--remote-debugging-port=9333` 的 headless Chrome，见脚本头注释）。

测试素材由 ffmpeg 生成，放在 `debug/media/`（已 git-ignore）：
`clip640x360.mp4`（H.264 5s）、`clip320x240.webm`（VP9 3s）、`clip176x144.mkv`（2s）。
