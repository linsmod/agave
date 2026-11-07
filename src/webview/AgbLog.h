/*
 * AgbLog.h - the one sink for browser diagnostics.
 *
 * Every line is printed as
 *
 *   [   12.3ms] [nav] request main type=link url=file:///...
 *   [  456.7ms] [evt] realm start gen=3 engine=...
 *   [ 1234.5ms] [ui]  mouse down left at 320,180
 *
 * The time is milliseconds since the first log line of the process. The origin
 * lives in a named mapping keyed by pid, so libagave, antjsb.dll, the
 * webview control and the shell all print on the same clock; a per-TU origin
 * would differ by up to ~15ms and reorder closely-spaced events.
 *
 * Header-only on purpose: antjsb.dll is loaded *by* libagave, so it cannot
 * link a logger that lives there. The format and the channel switches still
 * live in this one file.
 *
 * Channels (opt out with AGB_EVT_LOG=0 / AGB_NAV_LOG=0 / AGB_UI_LOG=0; the
 * host/probe/flag lines are few and always on):
 *
 *   [evt]   page lifecycle   commit, realm start/stop, bindDocument, DCL/load
 *   [nav]   navigation       host open/back/forward/reload/stop, request,
 *                            submit, redirect, popup
 *   [ui]    user input       resize/focus/blur/mouse/wheel/key/char
 *   [shell] host lifecycle   window ready, message loop, exit bases
 *   [probe] headless probe   userdata, load error, dialog, exit code
 *   [flag]  engine flags     bridge switches (AntBridge.c)
 *   [con-log] / [con-info] / [con-warn] / [con-error] / [con-debug]
 *             page console  console.*, via the polyfills guard. The level is
 *                            part of the tag; all levels share one channel and
 *                            one switch (AGB_CON_LOG=0)
 *   [style] render tree      opt-in (AGB_STYLE_LOG=1, AGB_STYLE_FILTER=<substr>):
 *                            attach/detach/style-recalc of the matching elements
 *   [cache] resource cache   memory-cache and HTTP-cache hit/miss per resource
 *                            (default on, AGB_CACHE_LOG=0 silences it)
 *   [gap-jsdom] capability   the FIRST access of a known JS/DOM gap, once per
 *                            gap per process (default on, AGB_GAP_LOG=0
 *                            silences it). The registry lives in
 *                            src/javascript/ant_backend/AgbGapJsdom.h
 *   [dl]    downloads        start/progress/finish/fail/cancel of a download
 *                            job (default on, AGB_DL_LOG=0 silences it)
 *   [parse] HTML parser      opt-in (AGB_PARSE_LOG=1): each <script>/<style>/
 *                            <link> start tag with its attributes (src/href/
 *                            rel/type/media/defer/async/nomodule), plus one line
 *                            per script execution (classic/module, deferred or
 *                            now)
 *
 * This is not the place for the opt-in debug channels ([netlog], [modfetch],
 * [snippet], [call], ...): they are separate tools with their own switches and
 * sinks. See docs/dev-debugging.md.
 */

#ifndef AgbLog_h
#define AgbLog_h

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* Keep windows.h from defining min/max macros: WebCore headers are included
 * after this one in several translation units and use their own templates. */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

enum AgbLogChannel {
    AGB_LOG_EVT = 0,
    AGB_LOG_NAV,
    AGB_LOG_UI,
    AGB_LOG_SHELL,
    AGB_LOG_PROBE,
    AGB_LOG_FLAG,
    AGB_LOG_CONSOLE,
    AGB_LOG_STYLE,
    AGB_LOG_CACHE,
    AGB_LOG_GAP,
    AGB_LOG_DL,
    AGB_LOG_PARSE,
    AGB_LOG_CHANNEL_COUNT
};

static const char* agbLogChannelName(int channel)
{
    switch (channel) {
    case AGB_LOG_EVT:   return "evt";
    case AGB_LOG_NAV:   return "nav";
    case AGB_LOG_UI:    return "ui";
    case AGB_LOG_SHELL: return "shell";
    case AGB_LOG_PROBE: return "probe";
    case AGB_LOG_FLAG:  return "flag";
    case AGB_LOG_CONSOLE: return "con-log";
    case AGB_LOG_STYLE: return "style";
    case AGB_LOG_CACHE: return "cache";
    case AGB_LOG_GAP:   return "gap-jsdom";
    case AGB_LOG_DL:    return "dl";
    case AGB_LOG_PARSE: return "parse";
    default:            break;
    }

    return "log";
}

/* The line is assembled here and printed with one fprintf: two threads
 * logging at the same time must not interleave halves of a line. */
#define AGB_LOG_LINE_MAX 2048

#if defined(_WIN32)

/* Process-wide clock origin. Keyed by pid so a second process never shares it;
 * the mapping dies with the process. */
typedef struct {
    volatile LONG     ready;   /* 0 = free, 1 = initializing, 2 = origin valid */
    volatile LONG     pad;
    volatile LONGLONG origin;  /* QueryPerformanceCounter at the first line    */
} AgbLogClock;

static inline double agbLogElapsedMs(void)
{
    static LONGLONG freq = 0;
    static LONGLONG origin = 0;
    LARGE_INTEGER now;

    if (!freq) {
        LARGE_INTEGER f;

        if (QueryPerformanceFrequency(&f) && f.QuadPart)
            freq = f.QuadPart;
        else
            freq = 1;
    }

    QueryPerformanceCounter(&now);

    if (!origin) {
        char name[64];
        HANDLE map;
        AgbLogClock* shared;

        sprintf(name, "Local\\agb-log-%lu", (unsigned long)GetCurrentProcessId());
        map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                 (DWORD)sizeof(AgbLogClock), name);
        shared = map ? (AgbLogClock*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                   sizeof(AgbLogClock)) : NULL;

        if (shared) {
            if (InterlockedCompareExchange(&shared->ready, 1, 0) == 0) {
                shared->origin = now.QuadPart;
                InterlockedExchange(&shared->ready, 2);
            } else {
                /* Another module got here first: wait for its origin. */
                int spin = 0;

                while (shared->ready != 2 && spin < 10000) {
                    Sleep(0);
                    ++spin;
                }
            }

            origin = shared->origin;
        }

        if (map) {
            /* Closing the last handle drops the name even though the view
             * keeps the section alive, so the next module would create a new
             * clock. One leaked handle per translation unit is the price of a
             * shared origin. */
            static HANDLE s_map;

            if (!s_map)
                s_map = map;
        }

        if (!origin)
            origin = now.QuadPart;
    }

    return (double)(now.QuadPart - origin) * 1000.0 / (double)freq;
}

#else /* !_WIN32 */

static inline double agbLogElapsedMs(void)
{
    static int started = 0;
    static clock_t origin;
    clock_t now = clock();

    if (!started) {
        started = 1;
        origin = now;
    }

    return (double)(now - origin) * 1000.0 / (double)CLOCKS_PER_SEC;
}

#endif

static inline int agbLogEnabled(int channel)
{
    const char* value;

    switch (channel) {
    case AGB_LOG_EVT: value = getenv("AGB_EVT_LOG"); break;
    case AGB_LOG_NAV: value = getenv("AGB_NAV_LOG"); break;
    case AGB_LOG_UI:  value = getenv("AGB_UI_LOG");  break;
    case AGB_LOG_CACHE: value = getenv("AGB_CACHE_LOG"); break;
    case AGB_LOG_GAP: value = getenv("AGB_GAP_LOG"); break;
    case AGB_LOG_DL:  value = getenv("AGB_DL_LOG");  break;
    case AGB_LOG_CONSOLE: value = getenv("AGB_CON_LOG"); break;
    /* Render-tree tracing is per element, so it is opt-in twice: the switch
     * must be "1" (default off) and AGB_STYLE_FILTER narrows it to the ids,
     * classes or tags that contain that substring. */
    case AGB_LOG_STYLE: value = getenv("AGB_STYLE_LOG");
                        return value && value[0] == '1' && value[1] == 0;
    /* Parser trace: <script>/<style>/<link> with their attributes. Per tag, so
     * opt-in (AGB_PARSE_LOG=1) like the render-tree trace. */
    case AGB_LOG_PARSE: value = getenv("AGB_PARSE_LOG");
                        return value && value[0] == '1' && value[1] == 0;
    default:          return 1;
    }

    return !(value && value[0] == '0' && value[1] == 0);
}

/* Callers pass the rest of the line (no trailing newline, no channel tag). */
static inline void agbLog(int channel, const char* format, ...)
{
    char line[AGB_LOG_LINE_MAX];
    va_list args;

    if (channel < 0 || channel >= AGB_LOG_CHANNEL_COUNT)
        return;

    if (!agbLogEnabled(channel))
        return;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    fprintf(stderr, "[%9.1fms] [%s] %s\n", agbLogElapsedMs(),
            agbLogChannelName(channel), line);
}

/* Page console.*: the level is part of the tag so an error can be grepped
 * without reading the message (`[con-error]`). Unknown or missing levels fall
 * back to `con-log`. One channel on purpose: AGB_CON_LOG=0 silences every
 * level at once, and call sites never pick a channel number. */
static inline void agbLogConsole(const char* level, const char* format, ...)
{
    char line[AGB_LOG_LINE_MAX];
    char tag[16];
    va_list args;
    const char* name = "log";

    if (!agbLogEnabled(AGB_LOG_CONSOLE))
        return;

    if (level) {
        if (!strcmp(level, "info") || !strcmp(level, "warn") ||
            !strcmp(level, "error") || !strcmp(level, "debug"))
            name = level;
    }

    snprintf(tag, sizeof(tag), "con-%s", name);

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    fprintf(stderr, "[%9.1fms] [%s] %s\n", agbLogElapsedMs(), tag, line);
}

#endif /* AgbLog_h */
