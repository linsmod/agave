#!/usr/bin/env python3
"""Monthly commit report for the agave repo.

Groups commits by problem domain (category) and maps every changed file to a
component. Components follow the source tree, but a component is not always a
directory: media, frames, svg, forms and the Ant bindings span several
directories, while html/, rendering/, platform/, css/ and webview/ each hold
several components. The rules below encode that mapping.

Usage:
    python tools/commit_report.py                      # current month, source only
    python tools/commit_report.py --since 2026-11-01
    python tools/commit_report.py --since 2026-10-01 --until 2026-11-01 --out report.md
    python tools/commit_report.py --all                # include docs/tests/build commits
    python tools/commit_report.py --json rows.json     # per-commit assignments

By default only commits that touch product source (src/, include/, examples/)
are counted; docs, test fixtures, tooling, build files and the published ant
binaries are filtered out (a mixed commit keeps only its source files).

No third-party dependency: English keywords are matched on lower-cased words
(with a light plural/ing/ed stem), Chinese anchors on substrings.
"""

import argparse
import collections
import datetime
import json
import os
import re
import subprocess
import sys

try:
    import jieba
    jieba.setLogLevel(20)
except ImportError:  # Chinese terms are skipped without jieba.
    jieba = None

# --------------------------------------------------------------------------
# Components: (path regex, component id), first match wins.
# Order matters: cross-directory components are tested before their default
# directory rule; files inside one directory are split by their file name.
# --------------------------------------------------------------------------
COMPONENT_RULES = [
    # Non-source
    (r"^(docs/|AGENTS\.md$|handover\.md$|README|lessons|todo|launch|log|test/)", "docs"),
    (r"\.md$|时序图", "docs"),
    (r"^tools/", "tools-probe"),
    (r"^3rdparty/ant/", "ant-binary"),
    (r"^3rdparty/|^dist/|^packages/", "vendor-libs"),
    (r"^(CMakeLists\.txt$|src/src\.cmake$|build/|\.gitignore$|\.gitattributes$|.*\.cmake$|build_.*\.(bat|sh)$)", "build"),
    (r"^examples/browser_shell/", "shell-ui"),
    (r"^examples/", "demo-apps"),
    (r"^include/", "public-api"),
    (r"^src/wtf/|^src/core/|^src/text/", "platform-core"),
    # JavaScript engines / bindings
    (r"^src/javascript/kjs/|^src/javascript/API/|^src/javascript/pcre/|^src/javascript/bindings/|^src/webcore/bindings/", "legacy-kjs"),
    (r"^src/webcore/buildANT/", "ant-bindings-gen"),
    (r"^src/javascript/ant_backend/Realm", "realm"),
    (r"^src/javascript/ant_backend/(AgbGapJsdom|AgbSelector|AgbTextCodec|AntBridge)", "ant-bridge"),
    (r"^src/javascript/ant_backend/(prelude|polyfills|inspector|internal)\.js$|^src/javascript/ant_backend/vendor/", "ant-prelude"),
    (r"^src/javascript/ant_backend/", "ant-runtime"),
    # CSS (parser / style / values / generated / UA sheets)
    (r"^src/webcore/buildKJS/|^src/webcore/buildQJS/", "generated-css"),
    (r"^src/webcore/css/(CSSParser|CSSGrammar|tokenizer|CSSPropertyNames|CSSValueKeywords|make|maketokenizer|"
     r"CSSHelper|CSSNamespace|CSSImportRule|CSSCharsetRule|CSSMediaRule|MediaQuery|MediaFeatureNames|CSSPageRule|"
     r"CSSUnknownRule|Counter\.(h|idl)|Rect\.(h|idl)|Pair\.h|DashboardRegion)", "css-parser"),
    (r"^src/webcore/css/(CSSStyleSelector|CSSComputedStyleDeclaration|CSSStyleDeclaration|CSSMutableStyleDeclaration|"
     r"CSSStyleRule|CSSRule|CSSRuleList|StyleSheet|CSSStyleSheet|StyleSheetList|StyleBase|StyleList|CSSFontFaceRule)", "css-style"),
    (r"^src/webcore/css/(CSSValue|CSSPrimitiveValue|CSSImageValue|CSSGradientValue|CSSBorderImageValue|"
     r"CSSInheritedValue|CSSInitialValue|CSSCursorImageValue|CSSTransformValue|ShadowValue|CSSFontFace|"
     r"CSSFontFaceSource|CSSFontFaceSrcValue|CSSFontSelector|FontValue|FontFamilyValue|RGBColor|Quirk)", "css-values"),
    (r"^src/webcore/css/.*\.css$", "ua-stylesheets"),
    (r"^src/webcore/css/", "css-style"),
    # HTML (parser / elements / forms / media / canvas / frames / tables)
    (r"^src/webcore/html/(HTMLParser|HTMLTokenizer|HTMLParserErrorCodes|DocTypeStrings|HTMLEntityNames|"
     r"HTMLAttributeNames|HTMLTagNames)", "html-parser"),
    (r"^src/webcore/html/(HTMLMediaElement|HTMLAudioElement|HTMLVideoElement|HTMLSourceElement|HTMLTrackElement|TextTrack)", "media"),
    (r"^src/webcore/html/Canvas", "canvas"),
    (r"^src/webcore/html/(HTMLFrameElement|HTMLFrameElementBase|HTMLFrameOwnerElement|HTMLFrameSetElement|"
     r"HTMLIFrameElement)", "frames"),
    (r"^src/webcore/page/(Frame|FrameTree|FrameView|FramePrivate|FrameLoadRequest)", "frames"),
    (r"^src/webcore/loader/(FrameLoader|FrameLoaderClient|FrameLoaderTypes)", "frames"),
    (r"^src/webview/FrameLoaderClientMC", "frames"),
    (r"^src/webcore/html/(HTMLInputElement|HTMLFormElement|HTMLSelectElement|HTMLOptionElement|HTMLOptGroupElement|"
     r"HTMLTextAreaElement|HTMLButtonElement|HTMLFieldSetElement|HTMLLegendElement|HTMLLabelElement|"
     r"HTMLIsIndexElement|HTMLKeygenElement|HTMLGenericFormElement|HTMLOptionsCollection|HTMLFormCollection|"
     r"HTMLTextFieldInnerElement|FormDataList)", "forms"),
    (r"^src/webcore/html/HTML(Table|TableCaption|TableCell|TableCol|TableRow|TableSection|TablePart)", "tables"),
    (r"^src/webcore/html/(HTMLScriptElement|HTMLLinkElement|HTMLStyleElement|HTMLMetaElement|HTMLBaseElement|"
     r"HTMLTitleElement)", "document-head"),
    (r"^src/webcore/html/", "html-elements"),
    # DOM
    (r"^src/webcore/dom/(Event|UIEvent|MouseEvent|MouseRelatedEvent|KeyboardEvent|WheelEvent|ClipboardEvent|"
     r"MutationEvent|OverflowEvent|TextEvent|BeforeUnloadEvent|BeforeTextInsertedEvent|RegisteredEventListener)", "dom-events"),
    (r"^src/webcore/dom/(TreeWalker|NodeIterator|NodeFilter|Traversal)", "dom-traversal"),
    (r"^src/webcore/dom/(Range|Position|EditingText)", "editing"),
    (r"^src/webcore/dom/", "dom-core"),
    # Rendering (layout / paint / svg / table / form controls)
    (r"^src/webcore/rendering/(RenderSVG|SVG|RenderPath|RenderForeignObject)", "svg"),
    (r"^src/webcore/rendering/RenderFlexibleBox", "flex-layout"),
    (r"^src/webcore/rendering/RenderStyle", "render-style"),
    (r"^src/webcore/rendering/(RenderTable|AutoTableLayout|FixedTableLayout|TableLayout)", "tables"),
    (r"^src/webcore/rendering/(RenderTextControl|RenderButton|RenderMenuList|RenderListBox|RenderSlider|"
     r"RenderFileUploadControl|RenderFieldset|RenderLegend)", "forms"),
    (r"^src/webcore/rendering/(RenderMedia|RenderHTMLCanvas)", "media"),
    (r"^src/webcore/rendering/RenderImage", "image"),
    (r"^src/webcore/rendering/(RenderLayer|RenderTheme|RenderObject|RenderView|RenderArena|RenderCounter|"
     r"RenderTreeAsText|HitTest|GapRects|RenderContainer|RenderReplaced|RenderWidget|RenderApplet|RenderPart)", "paint"),
    (r"^src/webcore/rendering/", "block-layout"),
    # Platform (network / media / graphics / image / fonts)
    (r"^src/webcore/platform/network/", "network"),
    (r"^src/webcore/platform/media/", "media"),
    (r"^src/webcore/platform/image-decoders/", "image"),
    (r"^src/webcore/platform/picasso/(Image|ImageBuffer|Icon)", "image"),
    (r"^src/webcore/platform/picasso/", "graphics-picasso"),
    (r"^src/webcore/platform/graphics/(BitmapImage|Image|ImageSource|ImageBuffer|ImageObserver|ImageDecodeStats)", "image"),
    (r"^src/webcore/platform/graphics/", "graphics-core"),
    (r"^src/webcore/platform/(unicode/|Font|Glyph|TextBreak|SegmentedString)", "fonts-text"),
    (r"^src/webcore/platform/", "platform-core"),
    # Loader
    (r"^src/webcore/loader/(MediaDocument|MediaTemplate)", "media"),
    (r"^src/webcore/loader/(Cache|Cached)", "resource-cache"),
    (r"^src/webcore/loader/", "loader"),
    # Page
    (r"^src/webcore/page/(Page|Chrome|Settings|Plugin|ContextMenuController|FocusController|EventHandler|"
     r"DragController|DragActions|MouseEventWithHitTest|InspectorController)", "page-core"),
    (r"^src/webcore/page/", "window"),
    # SVG documents
    (r"^src/webcore/ksvg2/", "svg"),
    (r"^src/webcore/editing/", "editing"),
    (r"^src/webcore/xml/", "xml"),
    (r"^src/webcore/history/", "window"),
    # WebView control / engine
    (r"^src/webview_control/", "webview-control"),
    (r"^src/webview/(AgbCrash|AgbLog|NavLog)", "webview-diag"),
    (r"^src/webview/(KeyEventMC|MouseEventMC|WheelEventMC|ScanCodeMap)", "webview-input"),
    (r"^src/webview/(DownloadManager|DownloadEvents|InternalPages|internal/)", "downloads-internal"),
    (r"^src/webview/(WebEncoding)", "network"),
    (r"^src/webview/", "webview-core"),
    (r"^src/webcore/", "platform-core"),
]

COMPONENT_GROUP = {
    "ant-runtime": "JS 运行时/绑定", "ant-prelude": "JS 运行时/绑定", "ant-bridge": "JS 运行时/绑定",
    "realm": "JS 运行时/绑定", "ant-bindings-gen": "JS 运行时/绑定", "legacy-kjs": "JS 运行时/绑定",
    "dom-core": "DOM/Web API", "dom-events": "DOM/Web API", "dom-traversal": "DOM/Web API",
    "window": "DOM/Web API", "page-core": "DOM/Web API", "editing": "DOM/Web API",
    "xml": "DOM/Web API", "history": "DOM/Web API",
    "html-parser": "HTML 解析/元素", "html-elements": "HTML 解析/元素", "document-head": "HTML 解析/元素",
    "frames": "Frame/iframe", "forms": "表单/控件",
    "css-parser": "CSS", "css-style": "CSS", "css-values": "CSS", "generated-css": "CSS",
    "ua-stylesheets": "CSS", "render-style": "CSS",
    "block-layout": "布局", "flex-layout": "布局", "tables": "布局",
    "paint": "绘制/图形", "graphics-core": "绘制/图形", "graphics-picasso": "绘制/图形", "canvas": "绘制/图形",
    "svg": "SVG", "image": "图片", "fonts-text": "字体/文本", "media": "媒体",
    "network": "网络", "loader": "加载/缓存", "resource-cache": "加载/缓存",
    "webview-control": "WebView 控件/后端", "webview-core": "WebView 控件/后端",
    "webview-input": "WebView 控件/后端", "webview-diag": "WebView 控件/后端",
    "downloads-internal": "WebView 控件/后端", "public-api": "WebView 控件/后端",
    "shell-ui": "外壳 UI", "demo-apps": "外壳 UI",
    "build": "工程", "tools-probe": "工程", "docs": "工程", "ant-binary": "工程",
    "vendor-libs": "工程", "platform-core": "工程",
}

# --------------------------------------------------------------------------
# Categories. Component -> category is a weak signal; the subject prefix is a
# strong one; crash/performance words cut across components.
# --------------------------------------------------------------------------
CATEGORIES = [
    "1 JS引擎与DOM/Web API", "2 CSS解析与样式计算", "3 布局/绘制/渲染", "4 媒体播放", "5 网络/加载/缓存",
    "6 浏览器外壳UI", "7 WebView控件/多视图/后端", "8 崩溃/卡死/内存安全", "9 性能优化",
    "10 构建/依赖/工具链", "11 测试/诊断/文档",
]

COMPONENT_CATEGORY = {
    "ant-runtime": 1, "ant-prelude": 1, "ant-bridge": 1, "realm": 1, "ant-bindings-gen": 1, "legacy-kjs": 1,
    "dom-core": 1, "dom-events": 1, "dom-traversal": 1, "editing": 1, "page-core": 1, "window": 1, "xml": 1,
    "html-parser": 1, "html-elements": 1, "document-head": 1, "frames": 1, "forms": 1, "tables": 1, "canvas": 1,
    "css-parser": 2, "css-style": 2, "css-values": 2, "generated-css": 2, "ua-stylesheets": 2, "render-style": 2,
    "block-layout": 3, "flex-layout": 3, "paint": 3, "graphics-core": 3, "graphics-picasso": 3, "svg": 3,
    "image": 3, "fonts-text": 3, "platform-core": 3,
    "media": 4,
    "network": 5, "loader": 5, "resource-cache": 5,
    "shell-ui": 6, "demo-apps": 6,
    "webview-control": 7, "webview-core": 7, "webview-input": 7, "webview-diag": 7,
    "downloads-internal": 7, "public-api": 7,
    "ant-binary": 10, "build": 10, "vendor-libs": 10,
    "docs": 11, "tools-probe": 11,
}

PREFIX_CATEGORY = {
    "dom": 1, "cssom": 1, "editing": 1, "realm": 1, "modules": 1, "js": 1, "engine": 1, "bridge": 1,
    "ant": 1, "context": 1, "window": 1, "prelude": 1, "webcore": 1,
    "reload": 5, "xhr": 5, "net": 5, "loader": 5, "textcodec": 5,
    "css": 2,
    "flex": 3, "svg": 3, "layout": 3, "render": 3, "rendering": 3, "paint": 3, "resize": 3, "fonts": 3, "text": 3,
    "media": 4,
    "shell": 6, "windows": 6,
    "webview": 7, "control": 7, "view": 7, "backend": 7, "inspector": 11, "vconsole": 11,
    "crash": 8, "watchdog": 8,
    "perf": 9,
    "build": 10, "3rdparty": 10, "antbridge": 10,
    "tools": 11, "test": 11, "tests": 11, "probe": 11, "diag": 11, "docs": 11, "roadmap": 11,
    "handover": 11, "agents": 11,
}

# Anchors: English ones match tokens (after a light stem), Chinese ones match
# as substrings of the subject.
ANCHORS = {
    1: """dom document element node attribute iframe window selection shadow template customelement
        fullscreen cssom history location form namednodemap nodelist event listener dispatch realm module
        worker script binding idl wrapper prototype interface prelude polyfill engine eval context
        localstorage postmessage contentwindow target child replace click focus keypress""".split() +
       ["模块", "脚本", "引擎", "绑定", "事件", "属性", "选择", "存储", "页面", "重入"],
    2: """css stylesheet computed getcomputedstyle selector gradient calc shorthand specificity mediaquery
        keyframe pseudo background borderradius rem color webkit unset percentage declaration property var
        style css3test""".split() + ["样式", "计算值", "选择器", "简写", "渐变", "颜色"],
    3: """layout render rendering paint repaint draw svg flex font glyph text line border shadow radius
        transform scroll scrolling viewport resize image decode antialias layer dirty clip opacity blur
        compositing viewbox placeholder lineheight metrics inlineblock""".split() +
       ["布局", "渲染", "绘制", "重绘", "脏矩形", "字体", "文本", "图片", "解码", "圆角", "阴影", "变换", "滚动"],
    4: """media video audio ffmpeg waveout playback player buffered codec stream volume playsinline""".split() +
       ["视频", "音频", "播放", "媒体"],
    5: """network net http https winhttp cache cookie charset redirect dns proxy connection pool loader load
        url data download xhr fetch request response gzip encoding wpad textcodec timeout retry""".split() +
       ["网络", "加载", "缓存", "超时", "重定向", "编码", "下载"],
    6: """shell chrome tab menu toolbar titlebar dwm findbar find status bubble pill hover theme shortcut
        bookmark zoom ux ui browsershell windows""".split() +
       ["外壳", "标签页", "菜单", "主题", "工具栏", "查找", "快捷键"],
    7: """webview control view backend ipc process renderer instance profile popup protocol command host
        respawn surface tabswitch embedding focus thread multiview""".split() +
       ["控件", "视图", "后端", "进程", "嵌入", "多视图", "宿主"],
    8: """crash uaf heap corruption corrupt leak memory segfault abort hang hung stuck freeze frozen watchdog
        unresponsive deadlock oom illegal overflow sanitizer""".split() +
       ["崩溃", "卡死", "内存", "泄漏", "越界", "无响应", "卡住", "死循环", "野指针", "堆损坏"],
    9: """perf performance optimize optimization latency ns budget prefetch lazy parallel throughput slower
        faster""".split() + ["性能", "优化", "提速", "耗时", "慢"],
    10: """build cmake zig mingw msvc compiler compile warning 3rdparty vendor picasso dll link dependency
         subproject crlf gitignore""".split() + ["构建", "编译", "依赖", "工具链", "忽略"],
    11: """test suite probe diag benchmark screenshot log logging docs record rule agent handover readme
         roadmap issue gap""".split() + ["测试", "探针", "诊断", "文档", "记录", "日志", "规则", "缺口"],
}

CRASH_RE = re.compile(
    r"\bcrash|\bhang\b|\bhung\b|\bfreeze|\bstuck\b|watchdog|unresponsive|use-after-free|\buaf\b|\bheap\b|"
    r"崩溃|卡死|冻结|无响应|卡住|死循环|100% cpu|0xc0000005|内存|泄漏|越界|\boom\b|非法释放|指针复用|堆损坏", re.I)
PERF_RE = re.compile(r"\bperf|\bperformance|optimi[sz]|latency|faster|slower|\bns\b|\bms\b|性能|优化|提速|耗时", re.I)

DOC_FILE = re.compile(r"(^docs/|^AGENTS\.md$|^handover\.md$|^README|\.md$)")
TOOL_FILE = re.compile(r"^(tools/|test/)|\.ps1$|\.py$")
BUILD_FILE = re.compile(r"^(build/|CMakeLists\.txt$|src/src\.cmake$)|\.cmake$|^\.git")
ANT_BIN = re.compile(r"^3rdparty/ant/")
# Product source: engine/webcore/control code and the example hosts.
SOURCE_FILE = re.compile(r"^(src/|include/|examples/)")


def comp_of(path):
    for rx, name in COMPONENT_RULES:
        if re.match(rx, path):
            return name
    return "other"


def stem(word):
    for suffix in ("ing", "ed", "es", "s"):
        if len(word) > len(suffix) + 3 and word.endswith(suffix):
            return word[: -len(suffix)] if suffix != "es" else word[:-2]
    return word


def english_tokens(text):
    toks = set()
    for m in re.finditer(r"[a-z][a-z0-9_+.\-]{1,}", text.lower()):
        w = m.group(0).strip("._-")
        if len(w) >= 2:
            toks.add(w)
            toks.add(stem(w))
    return toks


def classify(subject, files):
    files = set(files)
    low = subject.lower()
    m = re.match(r"^([a-z0-9_\-]+(?:/[a-z0-9_\-]+)?)(\([^)]*\))?:", low)
    prefix = m.group(1).split("/")[0] if m else None

    # Pure documentation / tooling / build changes first: they touch no product code.
    if files and all(DOC_FILE.search(f) for f in files):
        return 11, {"kind": "docs"}
    if files and all(TOOL_FILE.search(f) for f in files):
        return 11, {"kind": "tools"}
    if files and all(BUILD_FILE.search(f) for f in files):
        return 10, {"kind": "build"}
    # A crash/hang in any subsystem is filed as a stability problem. Pure
    # publish/build/doc commits already returned above, so those stay put.
    if CRASH_RE.search(subject):
        return 8, {"kind": "crash"}

    score = collections.Counter()
    if prefix in PREFIX_CATEGORY:
        score[PREFIX_CATEGORY[prefix]] += 12

    comps = {comp_of(f) for f in files}
    # docs/tools are touched by most commits and the ant binaries are only the
    # delivered build; none of them says much about the problem domain.
    seen_cats = collections.Counter()
    for c in sorted(comps - {"docs", "tools-probe", "ant-binary"}):
        cat = COMPONENT_CATEGORY.get(c)
        if cat:
            seen_cats[cat] += 1
    for cat, n in seen_cats.items():
        score[cat] += min(3 * n, 6)

    toks = english_tokens(subject)
    for cat, anchors in ANCHORS.items():
        hits = 0
        for a in anchors:
            if re.search(r"[\u4e00-\u9fff]", a):
                if a in subject:
                    hits += 1
            elif a in toks:
                hits += 1
        if hits:
            score[cat] += min(3 * hits, 9)

    if CRASH_RE.search(subject):
        score[8] += 6
    if PERF_RE.search(subject):
        score[9] += 6

    if not score:
        return 11, {"kind": "none"}
    # Deterministic tie-break: higher score first, then lower category number.
    cat = sorted(score.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]
    return cat, {"score": score.most_common(3)}


def is_source(path):
    return bool(SOURCE_FILE.match(path)) and not path.endswith(".md") and not BUILD_FILE.search(path)


# --------------------------------------------------------------------------
# Terms for the per-category "top terms" lists. English identifiers/words are
# split into sub-words (RenderFlexibleBox -> render/flexible/box); Chinese
# words use jieba when installed. Source terms come from added diff lines only,
# with comments and string literals stripped first.
# --------------------------------------------------------------------------
CJK_RE = re.compile(r"[\u4e00-\u9fff]")
TERM_WORD = re.compile(r"[A-Za-z][A-Za-z0-9_+\-]{1,}")
CAMEL = re.compile(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])|_+")
STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'')
LINE_COMMENT_RE = re.compile(r"//.*$")

TERM_STOP_EN = set("""a an the and or but if then else for with without to of in on at by from into over under
is are was were be been being do does did done doing have has had having not no nor so as than that this
these those it its they them their we our you your he she his her i me my
fix fixes fixed feat add adds added support supports make makes made use uses using
after before when while where which who whom what how why all any some more most other
up down out off again further once here there just only very too also now new old still same
per via vs etc r1 r2 r3 r4 r5 r6 r7 r8 r9 r10
also into like first one two three may can should would could
every never own instead through whole both keeps keep goes going get gets got
needs need see seen base based hold holds next last real step steps issues issue runs run
agb antb
verified verify suite suites assertion assertions regression expected actual diff output exit
""".split())

TERM_STOP_CN = set("""了 的 和 与 在 是 把 被 让 给 从 到 为 对 而 也 都 就 还 又 再 已 并 且 但 或 一个 一种
这个 那个 我们 你们 他们 这 那 有 没有 不是 可以 需要 应该 现在 之前 之后 已经 还是 只是 因为 所以
如果 这样 那样 时候 里面 一些 这些 那些 依然 仍然 属于 用于 通过 使用 支持 实现 改为 变成 修复 解决
问题 情况 以及 其中 每个 所有 不会 不再 不能 可能 部分 直接 导致 造成 使得 起来 出来 下来 上来 进来
一下 一样 进行 采用 提供 增加 新增 改成 这次 本次 同时 然后 目前 没有 两个 三个 当前""".split())

TERM_STOP_SRC = set("""int char void bool long short float double unsigned signed const static struct class public
private protected virtual inline explicit template typename namespace using return if else for while do switch
case break continue default new delete this nullptr true false include define ifdef ifndef endif pragma error
warning size sizeof typedef union enum extern mutable friend operator throw try catch goto volatile register
wchar auto decltype noexcept constexpr thread_local aligned alignas static_assert
std string vector map set list pair hash unique shared make cout endl printf fprintf sprintf snprintf vprintf
memset memcpy memmove strcmp strncmp strcpy strncpy strlen strstr strchr atoi atol strtol strtoul malloc
calloc realloc free assert getenv stderr stdout stdin size_t ssize_t uint32 uint64 int32 int64 intptr
uintptr ptrdiff wchar_t int8 uint8 int16 uint16
agb antb eng macross g_engine g_webcore webcore web
value values data type types name names text size length count index number item items entry entries
set get add remove create destroy update init run runs call calls used user users
must already nothing inside back end start left right current new old next last first
return based line lines file files code source impl implementation info
utf8 nullptr null empty max min result second self top lower upper bottom kind root header src var refptr
nodeof wrapnode iselementnode fromutf8 towstring tostring
dword hwnd msg wparam lparam argc argv str ptr buf tmp len ref prop tag evt doc args cast
static_cast push_back push back begin end clear append find assign isempty c_str
""".split())

SOURCE_EXTS = {".c", ".cc", ".cpp", ".h", ".hpp", ".js", ".idl", ".y", ".in"}
SKIP_GENERATED = ("src/webcore/buildKJS/", "src/webcore/buildQJS/", "src/webcore/buildANT/")


def split_ident(tok):
    return [p.lower() for p in CAMEL.split(tok) if p]


def term_words(text, stop=TERM_STOP_EN, compound_weight=1, split_compounds=True):
    """Return (term, weight) pairs.

    Compound identifiers keep their original case (AntRuntime, HTMLMediaElement,
    on_command_dropped, m_frame) because that is the symbol you would grep for;
    their lower-case sub-words are emitted with weight 1 when asked. Plain
    words (Node, document) are lower-cased so they do not split the list.
    """
    out = []
    for m in TERM_WORD.finditer(text):
        raw = m.group(0).strip("-_")
        if len(raw) < 3:
            continue
        parts = split_ident(raw)
        if parts and parts[0] in ("antb", "agw", "agb", "g"):
            continue  # project plumbing, not a domain term
        compound = len(parts) > 1 or "_" in raw
        if compound:
            low = raw.lower()
            if low not in stop:
                out.append((raw, compound_weight))
            if split_compounds:
                for p in parts:
                    if len(p) >= 3 and p not in stop:
                        out.append((p, 1))
        else:
            for p in parts:
                if len(p) >= 3 and p not in stop:
                    out.append((p, 1))
    return out


def term_cn(text):
    if not jieba:
        return []
    out = []
    for w in jieba.cut(text):
        w = w.strip()
        if len(w) >= 2 and CJK_RE.search(w) and w not in TERM_STOP_CN:
            out.append(w)
    return out


def strip_code(line):
    s = line.strip()
    if s.startswith(("//", "/*", "*", "<!--")):
        return ""
    code = STRING_RE.sub(" ", s)
    cut = LINE_COMMENT_RE.search(code)
    if cut and not re.search(r":$", code[:cut.start()]):
        code = code[:cut.start()]
    return code


def diff_terms(repo, since, until):
    """hash -> (term set, term Counter) from added lines of source diffs."""
    cmd = ["git", "-C", repo, "log", "-p", "-U0", f"--since={since}",
           "--pretty=format:COMMIT|%h", "--", "src", "include", "examples"]
    if until:
        cmd.insert(4, f"--until={until}")
    raw = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    out = collections.defaultdict(lambda: collections.Counter())
    commit = None
    path = None
    for line in raw.splitlines():
        if line.startswith("COMMIT|"):
            commit, path = line[7:], None
        elif line.startswith("diff --git "):
            m = re.search(r" b/(.+)$", line)
            path = m.group(1) if m else None
        elif line.startswith("+++ ") or line.startswith("--- "):
            continue
        elif line.startswith("+") and commit and path:
            if re.match(r"\+?\s*#\s*(include|pragma)\b", line):
                continue
            if not is_source(path) or path.startswith(SKIP_GENERATED):
                continue
            if os.path.splitext(path)[1].lower() not in SOURCE_EXTS:
                continue
            code = strip_code(line[1:])
            for term, weight in term_words(code, TERM_STOP_SRC | TERM_STOP_EN,
                                           compound_weight=4, split_compounds=False):
                out[commit][term] = min(out[commit][term] + weight, 8)
            for term in term_cn(code):
                out[commit][term] += 1
    return out


def rank_terms(df, tf, n, score=None):
    score = score or {}
    return sorted(df.items(), key=lambda kv: (-score.get(kv[0], 0), -kv[1], -tf.get(kv[0], 0), kv[0]))[:n]


def format_terms(items, per_line=8):
    cells = [f"{t}({n})" for t, n in items]
    lines = []
    for i in range(0, len(cells), per_line):
        lines.append("  " + "，".join(cells[i:i + per_line]))
    return lines or ["  （无）"]


def git_commits(repo, since, until):
    cmd = ["git", "-C", repo, "log", f"--since={since}", "--pretty=format:COMMIT|%h|%ad|%s",
           "--date=short", "--name-only"]
    if until:
        cmd.insert(4, f"--until={until}")
    raw = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    commits, cur = [], None
    for line in raw.splitlines():
        if line.startswith("COMMIT|"):
            if cur:
                commits.append(cur)
            _, sha, date, subject = line.split("|", 3)
            cur = {"hash": sha, "date": date, "subject": subject, "files": []}
        elif line.strip() and cur is not None:
            cur["files"].append(line.strip())
    if cur:
        commits.append(cur)
    return commits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=".", help="repository root (default: .)")
    ap.add_argument("--since", default=None, help="git --since, e.g. 2026-11-01")
    ap.add_argument("--until", default=None, help="git --until")
    ap.add_argument("--out", default=None, help="write the Markdown report to this file")
    ap.add_argument("--json", default=None, help="write per-commit assignments to this JSON file")
    ap.add_argument("--include-fixtures", action="store_true",
                    help="keep tools/ pages in per-category file lists (excluded by default)")
    ap.add_argument("--all", action="store_true",
                    help="also count commits that only touch docs/tests/build/vendor files")
    ap.add_argument("--terms", type=int, default=100,
                    help="terms per category in the two term lists (0 disables the extraction)")
    args = ap.parse_args()
    if not args.since:
        today = datetime.date.today()
        args.since = today.replace(day=1).isoformat()

    commits = git_commits(args.repo, args.since, args.until)
    if not commits:
        sys.exit("no commits in range")

    total = len(commits)
    if not args.all:
        for c in commits:
            c["files"] = [f for f in c["files"] if is_source(f)]
        commits = [c for c in commits if c["files"]]
        if not commits:
            sys.exit("no source changes in range")
    excluded = total - len(commits)

    for c in commits:
        c["cat"], c["info"] = classify(c["subject"], c["files"])
        c["comps"] = sorted({comp_of(f) for f in c["files"] if f})

    # Per-category term lists: message terms (subject x3 + body x1) and terms
    # from added diff lines.
    msg_terms = {}
    src_terms = {}
    if args.terms:
        src_terms = diff_terms(args.repo, args.since, args.until)
        for c in commits:
            msg_terms[c["hash"]] = {t for t, _ in term_words(c["subject"])} | set(term_cn(c["subject"]))

    cat_count = collections.Counter(c["cat"] for c in commits)
    comp_count = collections.Counter()
    comp_files = collections.defaultdict(collections.Counter)
    for c in commits:
        for comp in c["comps"]:
            comp_count[comp] += 1
            for f in c["files"]:
                if comp_of(f) == comp:
                    comp_files[comp][f] += 1

    lines = []
    add = lines.append
    scope = "个提交" if args.all else "个源码提交"
    add(f"# 提交分析 {commits[-1]['date']} ~ {commits[0]['date']}（{len(commits)} {scope}）")
    add("")
    if args.all:
        add("统计口径：包含所有提交（含仅文档/测试/构建改动）。")
    else:
        add(f"统计口径：只统计含产品源码（src/、include/、examples/）改动的提交；"
            f"已排除 {excluded} 个仅文档/测试/构建/发布件提交。")
    add("分类按提交主题归类；组件按改动文件映射（跨目录组件已合并），")
    add("一个提交可同时触及多个组件，因此组件计数之和大于提交数。")
    add("")
    add("## 分类")
    add("")
    add("| 分类 | 提交 |")
    add("|---|---:|")
    for idx, cat in enumerate(CATEGORIES, 1):
        add(f"| {cat} | {cat_count.get(idx, 0)} |")
    add("")

    add("## 组件")
    add("")
    add("| 组 | 组件 | 提交 | 主要文件 |")
    add("|---|---|---:|---|")
    for comp, n in comp_count.most_common():
        files = [f for f, _ in comp_files[comp].most_common(2) if not f.startswith(("docs/", "tools/"))]
        sample = ", ".join(files)
        add(f"| {COMPONENT_GROUP.get(comp, '?')} | {comp} | {n} | {sample} |")
    add("")

    add("## 分类 → 组件")
    add("")
    for idx in range(1, len(CATEGORIES) + 1):
        rows = [c for c in commits if c["cat"] == idx]
        if not rows:
            continue
        per = collections.Counter()
        per_files = collections.defaultdict(collections.Counter)
        for c in rows:
            for comp in c["comps"]:
                if comp in ("docs", "tools-probe"):
                    continue
                per[comp] += 1
                for f in c["files"]:
                    if comp_of(f) == comp:
                        per_files[comp][f] += 1
        add(f"### {CATEGORIES[idx - 1]}（{len(rows)}）")
        add("")
        add("组件：" + "、".join(f"`{c}` {n}" for c, n in per.most_common(8)))
        files = []
        for comp, _ in per.most_common(4):
            for f, _ in per_files[comp].most_common(2):
                if f.startswith(("docs/", "tools/")) and not args.include_fixtures:
                    continue
                if f not in files:
                    files.append(f)
        add("主要文件：" + "、".join(f"`{f}`" for f in files[:6]))
        if args.terms:
            mdf, mtf, sdf, stf = (collections.Counter() for _ in range(4))
            for c in rows:
                m = msg_terms.get(c["hash"], set())
                mtf.update(m)
                for t in m:
                    mdf[t] += 1
                s = src_terms.get(c["hash"], {})
                stf.update(s)
                for t in set(s):
                    sdf[t] += 1
            add(f"Top {args.terms} Related terms of Commit Messages：")
            lines.extend(format_terms(rank_terms(mdf, mtf, args.terms)))
            add(f"Top {args.terms} Related terms of Source Codes：")
            lines.extend(format_terms(rank_terms(sdf, stf, args.terms, stf)))
        add("")

    report = "\n".join(lines)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(report + "\n")
        print(f"wrote {args.out}")
    else:
        print(report)

    if args.json:
        slim = [{"hash": c["hash"], "date": c["date"], "subject": c["subject"],
                 "category": c["cat"], "components": c["comps"]} for c in commits]
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(slim, fh, ensure_ascii=False, indent=1)
        print(f"wrote {args.json}")


if __name__ == "__main__":
    main()
