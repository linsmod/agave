# Web font（@font-face）支持链路

> 状态：truetype/opentype 可用（2026-11-03 落地并验证）。**woff / woff2 / eot / svg 仍不支持**。

## 1. 链路总览

```
CSS @font-face
  └─ CSSParser::parseFontFaceSrc()        src 列表 → CSSFontFaceSrcValue[]
       └─ CSSFontSelector::addFontFaceRule()
            ├─ isSupportedFormat()         只放行 truetype / opentype
            ├─ DocLoader::requestFont()    → CachedFont（登记进 DocLoader）
            └─ m_fonts[hash(family.lower(), bold, italic)] = CSSFontFace
                 └─ CSSFontSelector::getFontData()  → CSSFontFace::getFontData()
                      └─ CachedFont::data()/ensureCustomFontData()
                           ├─ sfnt 'name' 表解析 → 内部 family 名（nameID 1）
                           ├─ AddFontMemResourceEx()  注册到 GDI（进程级）
                           └─ FontPlatformData → picasso 按 family 名创建字体
```

资源侧（native 后端）：

```
CachedFont → ResourceLoader::load() → ResourceHandle → ResourceHandleManager::add()
  ├─ file://        → 读盘 → 进 m_cached
  ├─ HTTP 缓存命中  → 进 m_cached
  └─ 真正走网络     → Net::Session
  ↓ （downloadTimerCallback 统一交付）
CachedFont::data() → checkNotify() → CSSFontFaceSource::fontLoaded() → 重排版
```

## 2. 三个已修的缺陷（2026-11-03）

### 2.1 多 item `src` 列表整条作废
`CSSParser::parseFontFaceSrc()` 的 URL 分支接受一个 item 后**没有置 `expectComma`**，
于是 `url(a) format('truetype'), url(b)` 里的逗号既进不了逗号分支、又不是合法 item，
`failed=true` → `src` 变成 null → `@font-face` 规则被整条丢弃。

真实站点（百度 superman iconfont）正是这种写法，所以它们的字体**从未注册**。
修：`src/webcore/css/CSSParser.cpp` URL 分支补 `expectComma = true`。

### 2.2 缓存命中的资源被搁浅（字体永远 loading）
`ResourceHandleManager::downloadTimerCallback()` 尾部按 `pending()==0` 停表，
但**回调执行期间**新排版会请求字体并塞进 `m_cached`（swap 发生在回调开头），
定时器一停，这批缓存命中就永远没人投递 → `getFontData ... loaded=0` 死循环。

修：`src/webcore/platform/network/native/ResourceHandleManagerNet.cpp`
停止前确认 `m_cached` / `m_skipped` 真的为空。

### 2.3 `Timer::isActive()` 不能回答「还会不会再触发」
它在「已收集进 ready-to-fire 集合、尚未触发」时也为真。
新增 `TimerBase::isScheduled()`（判堆内）并用它给下载定时器排程。

## 3. 验证方法

```powershell
# 回归页（真实多格式 @font-face）
$env:AGB_FONT_LOG = '1'
agave_view_host.exe file:///e:/mybrowsers/agave/tools/pages/iconfont_render.html out.bmp `
    --eval 'window.__iconfont?JSON.stringify(window.__iconfont):"no"'
```

期望：`[fontsel] src[0..4] ... supported=0/1`（只有 ttf 那项 supported=1）、
`[font] beginLoad` → `[netmgr] cache hit` → `[font] data size=42360` →
`[font] ensure: registered 'iconfont'`，页面状态栏显示「字体生效 = 64 个」。

日志开关：`AGB_FONT_LOG=1`（`[font]` / `[fontsel]` / `[netmgr]` / `[subres]` / `[loader]` / `[handlecb]`）。

## 4. 已知限制与待办

- **只支持 truetype/opentype**（`CSSFontFaceSrcValue::isSupportedFormat()`）。
  只提供 woff2 的站点仍然拿不到字体 → 需要补 woff2 解压（brotli）+ woff（zlib）。
- GDI 注册（`AddFontMemResourceEx`）是**进程级且不回收**：字形页与缓存的 FontData
  可能比 CachedFont 活得久，代价是字体常驻。
- 匹配用的是 sfnt 内部 family 名（nameID 1），不是 CSS 里声明的族名；
  同一 family 的多个 face 需要靠 bold/italic 区分（目前只区分两档字重）。
