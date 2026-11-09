/*
 * AgaveWebView.c - embeddable WebView control.
 *
 * One child window per control, one shared engine thread per process:
 *
 *   host thread  : owns the HWNDs, forwards input into a command queue
 *   engine thread: macross_initialize + every MaCrossView, drains the queue,
 *                  runs event dispatch and paints into per-control buffers
 *
 * macross_* callbacks are process-global, so they are registered once and
 * routed to the owning control by matching the MaCrossView pointer.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macross.h"
#include "agave_webview.h"
#include "AntBridge.h"   /* antb_interrupt_all_engines: abort a stuck page script */
#include "../webview/AgbLog.h"
#include "../webview/AgbRuntime.h"   /* agb_teardown_started_ms: budget the teardown */
#include "AgwViewBackend.h"          /* the render-backend seam (S3) */

/* Defined next to the command drain loop; needed here because agwPost() is the
 * single choke point every host call goes through. */
static volatile LONG g_engineBusy;
static DWORD         g_engineBusySince;

/* The responsiveness budgets (see the watchdog for how they are used):
 * a navigation blocked by a page script, a stop/search blocked by one, and a
 * page script with nobody waiting on it, plus an engine that stopped draining
 * without running JS (reported, not aborted). AGW_REPORT_GAP_MS rate-limits the
 * report so a script that ignores the abort cannot fill the log or pop a dialog
 * every tick.
 *
 * Navigations get a much shorter budget than the other user actions: a
 * navigation replaces the document, so aborting the outgoing page's script
 * cannot corrupt anything the user will see again. A stop/search keeps the
 * page, so it keeps the old budget - killing there leaves a half-run script in
 * a page that stays on screen. */
#define AGW_NAV_ABORT_MS       100u
#define AGW_USER_ABORT_MS     1500u
#define AGW_TEARDOWN_MS        100u
#define AGW_UNRESPONSIVE_MS  12000u
#define AGW_NATIVE_STALL_MS  10000u
#define AGW_REPORT_GAP_MS     5000u
static DWORD g_navAbortMs = AGW_NAV_ABORT_MS;
static DWORD g_userAbortMs = AGW_USER_ABORT_MS;
static DWORD g_teardownMs = AGW_TEARDOWN_MS;
static DWORD g_unresponsiveMs = AGW_UNRESPONSIVE_MS;
static DWORD g_nativeStallMs = AGW_NATIVE_STALL_MS;
static int   g_watchdogDebug;
static int agwQueueStuckFor(int minMs);
static DWORD agwQueueWaitType(int* type);

/* The control window class is registered as a Unicode class: see agwRegisterClass. */
#define AGW_MAX_VIEWS  32
#define AGW_QUEUE_SIZE 256
#define AGW_MSG_STATE  (WM_APP + 0x51)
#define AGW_MSG_REPAINT (WM_APP + 0x52)
#define AGW_MSG_DOWNLOAD (WM_APP + 0x53)
#define AGW_MSG_CONTEXT (WM_APP + 0x54)
#define AGW_MSG_UNRESPONSIVE (WM_APP + 0x55)
/* Destroy the control window on the thread that created it (see
 * agave_webview_destroy: the engine thread may be the caller for popups). */
#define AGW_MSG_RETIRE (WM_APP + 0x56)

#define AGW_MAX_DOWNLOADS 32

/* Per-view mirror of one engine download job. Fixed buffers: the engine's
 * strings are only valid during cb_download, and the UI thread reads this
 * copy later. */
typedef struct {
    int                id;
    int                state;
    int                error;
    unsigned long long received;
    unsigned long long total;
    char               url[1024];
    char               path[768];
    char               mime[96];
    int                pending;   /* state changed, UI thread has not seen it */
} AgwDownloadEntry;

/* Right-click payload handed from the engine thread to the UI thread: the menu
 * is built there (see agwOnContextMenu). Strings are heap copies because the
 * engine's own buffers are dead by the time the posted message is handled. */
typedef struct {
    int   x;
    int   y;
    char* link;
    char* image;
    char* text;
} AgwContextPayload;

enum {
    AGW_CMD_NAVIGATE, AGW_CMD_LOAD_HTML, AGW_CMD_BACK, AGW_CMD_FORWARD,
    AGW_CMD_RELOAD, AGW_CMD_STOP, AGW_CMD_EVAL, AGW_CMD_ZOOM,
    AGW_CMD_RESIZE, AGW_CMD_UA, AGW_CMD_DL_DIR, AGW_CMD_MOUSE, AGW_CMD_KEY,
    AGW_CMD_CHAR, AGW_CMD_FOCUS, AGW_CMD_SEARCH, AGW_CMD_CONTEXT,
    AGW_CMD_DL_CANCEL, AGW_CMD_VISIBLE
};

/* Commands that mean "the user wants the page to change": while one of them
 * waits, a page script is not merely slow, it is in the way, and aborting it is
 * exactly what the user asked for. Input/resize/download commands are not worth
 * killing a script for - and they are the ones a busy page keeps queuing. */
static int agwCommandIsUserAction(int type)
{
    switch (type) {
    case AGW_CMD_NAVIGATE:
    case AGW_CMD_LOAD_HTML:
    case AGW_CMD_BACK:
    case AGW_CMD_FORWARD:
    case AGW_CMD_RELOAD:
    case AGW_CMD_STOP:
    case AGW_CMD_SEARCH:
        return 1;
    default:
        return 0;
    }
}

/* The user-action commands that replace the document. Aborting the script
 * behind one of them costs nothing (the page is going away), so they get a
 * much shorter budget than stop/search, which keep the page on screen. */
static int agwCommandIsNavigation(int type)
{
    switch (type) {
    case AGW_CMD_NAVIGATE:
    case AGW_CMD_LOAD_HTML:
    case AGW_CMD_BACK:
    case AGW_CMD_FORWARD:
    case AGW_CMD_RELOAD:
        return 1;
    default:
        return 0;
    }
}

/* Why the watchdog spoke up. AGW_HANG_STALL is the one case where aborting is
 * pointless: the engine is not in JS, so nothing would poll the flag. */
enum { AGW_HANG_NONE, AGW_HANG_USER_ACTION, AGW_HANG_UNRESPONSIVE, AGW_HANG_STALL,
       AGW_HANG_TEARDOWN };

/* Watchdog -> UI thread payload (the reason text, the numbers behind it and the
 * busy-stack file of this report). */
typedef struct {
    int          kind;
    unsigned int jsDepth;
    int          headType;
    unsigned int busyMs;
    unsigned int queueMs;
    char         stackPath[MAX_PATH];
} AgwHangPayload;

typedef struct {
    int  type;
    /* Stable identity of the target view, not its slot in g_views: destroy()
     * swap-removes the slot, so an index captured when the command was posted
     * can point at a different view (or past the end) by the time the engine
     * thread drains the queue. */
    int  viewId;
    int  x, y, a, b;
    /* Payload for navigate/eval/search. Short payloads stay inline (the
     * engine thread uses the buffer synchronously); anything that does not
     * fit moves to heapText, which the queue owns until the command has run.
     * The old fixed 8 KB buffer silently truncated long eval scripts - an
     * injected inspector.js / vConsole is far larger than that - and the
     * engine then reported a bogus SyntaxError in the middle of the script. */
    char  text[8192];
    char* heapText;
    /* When the host queued it. A command that still sits at the queue head this
     * long after being posted means the engine thread is not draining at all -
     * the only reliable "stuck" signal: the engine being merely idle also stops
     * pumping window messages, and a page legitimately parsing for seconds looks
     * identical from the outside. */
    DWORD at;
} AgwCommand;

struct AgaveWebView {
    HWND               hwnd;
    MaCrossView*       view;        /* local engine view (callback identity) */
    /* Render backend (docs/design-render-backend.md). v1 is always `local`;
     * backendView is the backend-opaque view handle, which for local is the
     * same MaCrossView*. Every per-view call goes through backend->op(). */
    const AgwViewBackend* backend;
    void*              backendView;
    MC_BYTE*           bits;
    HBITMAP            bitmap;
    HDC                memdc;
    HGDIOBJ            oldBitmap;
    /* Complete-frame copy the UI thread blits from. The engine writes `bits`
     * from its own thread; painting straight out of that buffer tore the image
     * while scrolling, so each finished frame is copied here under the lock. */
    MC_BYTE*           mirrorBits;
    HBITMAP            mirrorBitmap;
    HDC                mirrorDc;
    HGDIOBJ            mirrorOldBitmap;
    int                width;
    int                height;
    int                wantWidth;
    int                wantHeight;
    int                zoom;
    int                pendingResize;
    /* Identity used by queued commands. Slots in g_views move when a view is
     * destroyed; this does not. */
    int                id;
    volatile int       destroyed;
    int                refs;

    /* Set on the engine thread while a queued command is executing. The UI
     * thread reads it to decide whether Reload should first abort the page
     * (a script stuck in a loop never returns to drain the queue). */
    volatile LONG      busy;
    DWORD              busySince;

    /* The thread that created `hwnd`: DestroyWindow() must run there. Tabs are
     * created by the host, popups by the engine thread (agwOnOpenWindow). */
    DWORD              hwndThread;
    /* Reap queue link (engine thread only). A destroyed view is torn down
     * between engine iterations, never inside the host's destroy() call. */
    struct AgaveWebView* reapNext;
    /* Signalled when the engine thread has finished tearing the view down.
     * destroy() waits on a duplicate of it, so a host that closes a tab and
     * exits (the probe, the last window) still gets the full teardown. */
    HANDLE             reapEvent;

    char               url[2048];
    char               title[512];
    char               status[512];
    int                progress;
    int                loading;
    /* Engine-loop accounting for the hidden-view skip (AGB_LOOP_DEBUG). */
    unsigned long      painted;
    unsigned long      skippedPaints;
    int                lastRecordedProgress;
    int                backCount;
    int                forwardCount;
    unsigned long      lastMirrorMs;   /* last InvalidateRect mirrored to the window */
    int                navigatePending; /* a CMD_NAVIGATE is queued for this view */
    /* Page Visibility as the engine loop sees it (host order: background tab,
     * minimized window). A hidden view keeps running JS and its async work, but
     * the engine thread skips its layout+paint: a background tab used to cost a
     * full layout and paint per engine iteration. `forceFullPaint` is set when
     * it comes back and the whole surface is stale (the skipped frames were
     * never drawn, and the dirty rects were consumed by the skip). */
    int                visible;
    int                forceFullPaint;

    char               downloadDir[MAX_PATH];
    char               userAgent[256];
    AgwDownloadEntry   downloads[AGW_MAX_DOWNLOADS];

    AgaveWebViewCallback callbacks;
    void*              user;

    /* High half of a surrogate pair, waiting for its low half (WM_CHAR and
     * WM_IME_CHAR deliver one UTF-16 code unit per message). */
    unsigned int       pendingHighSurrogate;
};

static char             g_userAgent[256];

static CRITICAL_SECTION g_lock;
static int              g_lockReady;
static AgaveWebView*    g_views[AGW_MAX_VIEWS];
static int              g_viewCount;
static AgaveWebView*    g_lastActive;
static AgwCommand       g_queue[AGW_QUEUE_SIZE];
static int              g_queueHead;
static int              g_queueTail;
static int              g_nextViewId = 1;
static volatile int     g_engineQuit;
static HANDLE           g_engineThread;
static DWORD            g_engineThreadId;
static volatile int     g_engineStarted;
/* The process-level backend chosen for this control (engine loop). NULL until
 * the engine thread resolves it from AGB_VIEW_BACKEND. */
static const AgwViewBackend* g_processBackend;
static int              g_classRegistered;
/* Views the host destroyed, waiting for the engine thread to tear them down
 * (guarded by g_lock). The engine thread owns WebCore, the Ant VM and the
 * surface buffers, so the teardown runs there - not in the host's thread while
 * macross_event_dispatch() is firing that page's timers (R141). */
static AgaveWebView*    g_reapHead;

/* ------------------------------------------------------------------------- */
/* small helpers                                                              */
/* ------------------------------------------------------------------------- */

static void agwLock(void)   { if (g_lockReady) EnterCriticalSection(&g_lock); }
static void agwUnlock(void) { if (g_lockReady) LeaveCriticalSection(&g_lock); }

static void agwCopyString(char* to, size_t cch, const char* from)
{
    if (!cch)
        return;

    if (!from) {
        to[0] = 0;
        return;
    }

    strncpy(to, from, cch - 1);
    to[cch - 1] = 0;
}

static int agwIndexOf(AgaveWebView* view)
{
    int i;

    for (i = 0; i < g_viewCount; ++i) {
        if (g_views[i] == view)
            return i;
    }
    return -1;
}

static AgaveWebView* agwByView(MaCrossView* view)
{
    int i;

    for (i = 0; i < g_viewCount; ++i) {
        if (g_views[i]->view == view)
            return g_views[i];
    }
    return NULL;
}

/* Protocol submission (defined next to the command drain). Declared here:
 * agwEnsureSurface() submits the first navigation right after creating the
 * view. */
static int agwSubmitSimple(AgaveWebView* item, int kind, int a, int b, int c, int d);
static int agwSubmitText(AgaveWebView* item, int kind, const char* text,
                         int a, int b, int c, int d);


/* Frees the control once the engine thread is done with it. destroy() may be
 * called while the engine is inside macross_view_update on this item, or while
 * macross_event_dispatch() is firing that page's timers. */
static void agwDestroySurface(AgaveWebView* item);

/* Window destruction belongs to the thread that created it: tabs are created by
 * the host, popups (agwOnOpenWindow) by the engine thread. A host that creates a
 * tab and an engine that creates a popup therefore each get their own thread to
 * run DestroyWindow() on. */
static void agwRetireWindow(AgaveWebView* item)
{
    HWND hwnd = item->hwnd;

    if (!hwnd)
        return;

    item->hwnd = NULL;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);

    if (GetCurrentThreadId() == item->hwndThread)
        DestroyWindow(hwnd);
    else
        PostMessageW(hwnd, AGW_MSG_RETIRE, 0, 0);
}

/* Under g_lock: takes the item out of the reap list. Returns 1 when this
 * caller now owns the teardown, so exactly one thread frees a view even when
 * the host retires it while the reaper is looking at it. */
static int agwClaimReapLocked(AgaveWebView* item)
{
    AgaveWebView** link = &g_reapHead;

    while (*link) {
        if (*link == item) {
            *link = item->reapNext;
            item->reapNext = NULL;
            return 1;
        }
        link = &(*link)->reapNext;
    }
    return 0;
}

/* Engine thread, after the caller claimed it out of the reap list and refs
 * reached 0: frees the engine view (DOM + JS + document teardown), the surface
 * buffers and the control itself. */
static void agwDestroyNow(AgaveWebView* item)
{
    if (g_lastActive == item)
        g_lastActive = NULL;

    if (item->backendView) {
        item->backend->destroy(item->backendView);
        item->view = NULL;
        item->backendView = NULL;
    }

    agwDestroySurface(item);
    agwRetireWindow(item);

    if (item->reapEvent) {
        /* Wake a host blocked in agave_webview_destroy(). The waiter holds its
         * own duplicate, so closing ours is safe even while it is waiting. */
        SetEvent(item->reapEvent);
        CloseHandle(item->reapEvent);
        item->reapEvent = NULL;
    }

    free(item);
}

/* Engine thread: tears down every retired view the engine is not using right
 * now. Called once per iteration after macross_event_dispatch(), so a view is
 * never destroyed while its own timers are being fired. */
static void agwReapViews(void)
{
    AgaveWebView* batch[AGW_MAX_VIEWS];
    int count = 0;
    int i;
    AgaveWebView** link;

    agwLock();
    link = &g_reapHead;
    while (*link) {
        AgaveWebView* item = *link;

        if (item->refs > 0) {
            link = &item->reapNext;
            continue;
        }

        if (count >= AGW_MAX_VIEWS)
            break;

        *link = item->reapNext;
        item->reapNext = NULL;
        batch[count++] = item;
    }
    agwUnlock();

    for (i = 0; i < count; ++i)
        agwDestroyNow(batch[i]);
}

static void agwRelease(AgaveWebView* item)
{
    int destroy = 0;

    if (!item)
        return;

    agwLock();
    if (--item->refs <= 0 && item->destroyed) {
        /* Last reference dropped and the host already asked for the control
         * to go away: this is the engine thread, the only place it may be
         * freed, and nobody else may claim the teardown. */
        destroy = agwClaimReapLocked(item);
    }
    agwUnlock();

    if (destroy)
        agwDestroyNow(item);
}

static void agwPost(int type, AgaveWebView* view, const char* text,
                    int x, int y, int a, int b)
{
    AgwCommand command;
    int queued = 0;

    if (!g_lockReady)
        return;

    /* Every host entry point (address bar, link navigation, back/forward,
     * reload, new tab, eval) queues here. If the engine has been inside a
     * script for a while, the command would sit behind that script forever -
     * so this is where "navigation aborts the current page" belongs, not in
     * each caller.
     *
     * A navigation aborts whatever script is running *now*: the page is going
     * away, so there is nothing to preserve and no grace period to wait out
     * (the teardown that follows has its own budget, see the watchdog). The
     * flag is consumed by the aborted script; the teardown handlers run later
     * as fresh top-level evals and are unaffected. */
    if (queued && agwCommandIsNavigation(type) && antb_engines_js_depth() > 0)
        antb_interrupt_all_engines();

    memset(&command, 0, sizeof(command));
    command.type = type;
    command.at = GetTickCount();
    command.x = x;
    command.y = y;
    command.a = a;
    command.b = b;

    if (text) {
        size_t len = strlen(text);

        if (len < sizeof(command.text)) {
            memcpy(command.text, text, len);
            command.text[len] = 0;
        } else {
            /* Long scripts (injected JS) do not fit the inline buffer. */
            command.heapText = (char*)malloc(len + 1);
            if (command.heapText) {
                memcpy(command.heapText, text, len + 1);
            } else {
                agbLog(AGB_LOG_SHELL, "command %d out of memory for %lu-byte payload",
                       type, (unsigned long)len);
                len = sizeof(command.text) - 1;
                memcpy(command.text, text, len);
                command.text[len] = 0;
            }
        }
    }

    agwLock();
    command.viewId = view ? view->id : 0;
    if (command.viewId > 0 &&
        ((g_queueTail + 1) % AGW_QUEUE_SIZE) != g_queueHead) {
        g_queue[g_queueTail] = command;
        g_queueTail = (g_queueTail + 1) % AGW_QUEUE_SIZE;
        queued = 1;
    }
    agwUnlock();

    if (!queued && command.heapText)
        free(command.heapText);

    if (queued && view->hwnd)
        PostMessageA(view->hwnd, AGW_MSG_STATE, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* engine callbacks (engine thread)                                           */
/* ------------------------------------------------------------------------- */

static void agwOnInvalidate(MaCrossView* view, const MC_RECT* rect)
{
    AgaveWebView* item = agwByView(view);

    (void)rect;

    if (item && item->hwnd)
        PostMessageA(item->hwnd, AGW_MSG_REPAINT, 0, 0);
}

/* window.agavePostMessage(): the engine does not say which view produced the
 * message, so we use the control whose view was updated last. */
static void agwOnPostMessage(const char* message)
{
    AgaveWebView* item = g_lastActive;

    /* Headless hosts (probe mode) never focus the control, so g_lastActive
     * stays NULL; with a single view there is no ambiguity. */
    if (!item && g_viewCount == 1)
        item = g_views[0];

    if (!item)
        return;

    if (item->callbacks.on_message)
        item->callbacks.on_message(item, message ? message : "", item->user);
}

/* alert()/confirm()/prompt(): dispatched to the host, with sane defaults. */
static int agwDispatchDialog(AgaveWebView* item, int type, const char* message,
                             const char* defaultValue, char* result, int resultSize)
{
    if (result && resultSize > 0)
        result[0] = 0;

    if (!item)
        return type == 2 ? 0 : 1;

    if (item->callbacks.on_dialog)
        return item->callbacks.on_dialog(item, type, message, defaultValue,
                                         result, resultSize, item->user);

    return type == 2 ? 0 : 1;
}

static int agwOnDialog(int type, const char* message, const char* defaultValue,
                       char* result, int resultSize)
{
    return agwDispatchDialog(g_lastActive, type, message, defaultValue, result, resultSize);
}

/* Native (engine-side) alert/confirm/prompt come through a different callback
 * set than the shim's; both end up in the host's on_dialog. */
static void agwOnAlertBox(MaCrossView* view, const char* message)
{
    agwDispatchDialog(agwByView(view), 0, message, "", NULL, 0);
}

static MC_BOOL agwOnConfirmBox(MaCrossView* view, const char* text)
{
    return agwDispatchDialog(agwByView(view), 1, text, "", NULL, 0) ? TRUE : FALSE;
}

static char* agwOnPromptBox(MaCrossView* view, const char* message, const char* defaultValue)
{
    static char buffer[1024];

    buffer[0] = 0;
    agwDispatchDialog(agwByView(view), 2, message, defaultValue, buffer, sizeof(buffer));
    return buffer;
}

/* strdup(3) is not portable here (MSVC CRT naming), and the context menu is the
 * only place that needs it. */
static char* agwDupOrNull(const char* text)
{
    char* copy;

    if (!text || !text[0])
        return NULL;

    copy = (char*)malloc(strlen(text) + 1);

    if (copy)
        strcpy(copy, text);

    return copy;
}

static void agwOnContextMenu(MaCrossView* view, const MC_CONTEXT_MENU_DATA* menu)
{
    AgaveWebView* item = agwByView(view);

    if (!item)
        item = g_lastActive;

    if (!item || !item->callbacks.on_context_menu)
        return;

    /* The engine calls this from the engine thread, in the middle of dispatching
     * the DOM contextmenu event. The host answers by showing a menu: a
     * TrackPopupMenu against a window owned by the UI thread does nothing when
     * it is called from another thread, and OpenClipboard() then fails too. So
     * copy the payload and let the UI thread build the menu. The engine's
     * strings and the MC_CONTEXT_MENU_DATA itself die when this returns, which
     * is why the copies exist (same pattern as AgwDownloadEntry). */
    {
        AgwContextPayload* payload = (AgwContextPayload*)calloc(1, sizeof(AgwContextPayload));

        if (!payload)
            return;

        payload->x = menu ? menu->location.x : 0;
        payload->y = menu ? menu->location.y : 0;
        payload->link = agwDupOrNull(menu ? menu->link_url : NULL);
        payload->image = agwDupOrNull(menu ? menu->image_url : NULL);
        payload->text = agwDupOrNull(menu ? menu->text : NULL);

        agbLog(AGB_LOG_NAV, "context menu at %d,%d link=%s image=%s text=%d bytes",
               payload->x, payload->y,
               payload->link ? payload->link : "(none)",
               payload->image ? payload->image : "(none)",
               payload->text ? (int)strlen(payload->text) : 0);

        /* Dropped messages (control destroyed first) leak only this payload. */
        if (!item->hwnd || !PostMessageA(item->hwnd, AGW_MSG_CONTEXT, 0, (LPARAM)payload)) {
            free(payload->link);
            free(payload->image);
            free(payload->text);
            free(payload);
        }
    }
}

static void agwOnUpdateNow(MaCrossView* view)
{
    AgaveWebView* item = agwByView(view);

    if (item && item->hwnd)
        PostMessageA(item->hwnd, AGW_MSG_REPAINT, 0, 0);
}

static void agwOnTitle(MaCrossView* view, const char* title)
{
    AgaveWebView* item = agwByView(view);

    if (!item)
        return;

    agwLock();
    if (title) {
        strncpy(item->title, title, sizeof(item->title) - 1);
        item->title[sizeof(item->title) - 1] = 0;
    }
    agwUnlock();

    if (item->callbacks.on_title)
        item->callbacks.on_title(item, item->title, item->user);
}

static void agwOnLocation(MaCrossView* view, const char* url)
{
    AgaveWebView* item = agwByView(view);

    if (!item || !url)
        return;

    agwLock();
    strncpy(item->url, url, sizeof(item->url) - 1);
    item->url[sizeof(item->url) - 1] = 0;
    agwUnlock();

    if (item->callbacks.on_url)
        item->callbacks.on_url(item, item->url, item->user);

    if (item->hwnd)
        PostMessageA(item->hwnd, AGW_MSG_STATE, 0, 0);
}

static void agwOnStatus(MaCrossView* view, const char* text)
{
    AgaveWebView* item = agwByView(view);

    if (!item)
        return;

    agwLock();
    if (text) {
        strncpy(item->status, text, sizeof(item->status) - 1);
        item->status[sizeof(item->status) - 1] = 0;
    }
    agwUnlock();

    if (item->callbacks.on_status)
        item->callbacks.on_status(item, item->status, item->user);
}

static void agwOnHistoryStatus(MaCrossView* view, unsigned int backCount, unsigned int forwardCount)
{
    AgaveWebView* item = agwByView(view);

    /* Diagnostics for the "back/forward buttons flicker on reload" report: this
     * callback is the only thing that changes their enabled state. */
    if (getenv("AGB_HISTORY_DEBUG"))
        fprintf(stderr, "[hist] view=%p back=%u fwd=%u\n", (void*)view, backCount, forwardCount);

    if (!item)
        return;

    agwLock();
    item->backCount = (int)backCount;
    item->forwardCount = (int)forwardCount;
    agwUnlock();
}

static void agwOnProgress(MaCrossView* view, unsigned int percent, MC_BOOL finished)
{
    AgaveWebView* item = agwByView(view);
    int notify = 0;

    if (!item)
        return;

    agwLock();
    item->progress = (int)percent;
    item->loading = finished ? 0 : 1;

    if (finished && item->lastRecordedProgress != 100) {
        item->lastRecordedProgress = 100;
        notify = 1;
    } else if (!finished) {
        item->lastRecordedProgress = (int)percent;
    }
    agwUnlock();

    if (item->callbacks.on_progress)
        item->callbacks.on_progress(item, (int)percent, finished ? 1 : 0, item->user);

    if (notify && item->callbacks.on_url && item->url[0])
        ; /* page finished: hosts usually refresh their address bar */

    if (item->hwnd)
        PostMessageA(item->hwnd, AGW_MSG_STATE, 0, 0);
}

static void agwOnError(int err, const char* failedUrl)
{
    int i;

    for (i = 0; i < g_viewCount; ++i) {
        AgaveWebView* item = g_views[i];

        if (!item || !item->backendView)
            continue;

        agwLock();
        item->loading = 0;
        agwUnlock();

        if (item->callbacks.on_error)
            item->callbacks.on_error(item, err, failedUrl, item->user);
    }
}

static void agwOnCursor(MC_CURSOR cursor)
{
    LPCSTR name = IDC_ARROW;

    switch (cursor) {
    case HAND_CURSOR:       name = IDC_HAND; break;
    case IBEAM_CURSOR:      name = IDC_IBEAM; break;
    case CROSS_CURSOR:      name = IDC_CROSS; break;
    case WAIT_CURSOR:       name = IDC_WAIT; break;
    case NORTHSOUTH_CURSOR: name = IDC_SIZENS; break;
    case EASTWEST_CURSOR:   name = IDC_SIZEWE; break;
    default: break;
    }

    SetCursor(LoadCursorA(NULL, name));
}


/* Downloads are process-global in the engine (they outlive the page), so the
 * control keeps a per-view copy of every job's latest state and hands it to the
 * host on the UI thread. The engine never touches an HWND. */
static MC_BOOL agwOnDownload(int id, const MC_DOWNLOAD_INFO* info)
{
    int v, i;

    if (!info)
        return TRUE;

    agwLock();

    for (v = 0; v < g_viewCount; ++v) {
        AgaveWebView* item = g_views[v];
        AgwDownloadEntry* slot = NULL;

        if (!item || item->destroyed)
            continue;

        for (i = 0; i < AGW_MAX_DOWNLOADS; ++i) {
            if (item->downloads[i].id == id) {
                slot = &item->downloads[i];
                break;
            }
        }

        if (!slot) {
            for (i = 0; i < AGW_MAX_DOWNLOADS; ++i) {
                if (!item->downloads[i].id) {
                    slot = &item->downloads[i];
                    break;
                }
            }
        }

        if (!slot)
            continue;   /* table full: the job still runs, the UI just misses it */

        slot->id = id;
        slot->state = info->state;
        slot->error = info->error;
        slot->received = info->received;
        slot->total = info->total;
        agwCopyString(slot->url, sizeof(slot->url), info->utf8_url);
        agwCopyString(slot->path, sizeof(slot->path), info->utf8_path);
        agwCopyString(slot->mime, sizeof(slot->mime), info->utf8_mime);
        slot->pending = 1;

        PostMessageA(item->hwnd, AGW_MSG_DOWNLOAD, 0, 0);
    }

    agwUnlock();

    return TRUE;
}

/* UI thread: deliver every state change the engine queued for this view. */
static void agwDeliverDownloads(AgaveWebView* item)
{
    AgwDownloadEntry snapshot[AGW_MAX_DOWNLOADS];
    MC_DOWNLOAD_INFO info;
    int count = 0;
    int i;

    if (!item)
        return;

    agwLock();
    for (i = 0; i < AGW_MAX_DOWNLOADS; ++i) {
        if (item->downloads[i].id && item->downloads[i].pending) {
            item->downloads[i].pending = 0;
            snapshot[count++] = item->downloads[i];
        }
    }
    agwUnlock();

    for (i = 0; i < count; ++i) {
        memset(&info, 0, sizeof(info));
        info.id = snapshot[i].id;
        info.state = snapshot[i].state;
        info.error = snapshot[i].error;
        info.utf8_url = snapshot[i].url;
        info.utf8_path = snapshot[i].path;
        info.utf8_mime = snapshot[i].mime;
        info.received = snapshot[i].received;
        info.total = snapshot[i].total;

        agbLog(AGB_LOG_DL, "state id=%d state=%d received=%llu total=%llu path=%s",
               info.id, info.state, info.received, info.total,
               snapshot[i].path[0] ? snapshot[i].path : "(none)");

        if (item->callbacks.on_download)
            item->callbacks.on_download(item, &info, item->user);
    }
}

/* ------------------------------------------------------------------------- */
/* buffers (engine thread only)                                               */
/* ------------------------------------------------------------------------- */

static void agwDestroySurface(AgaveWebView* item)
{
    if (item->memdc) {
        SelectObject(item->memdc, item->oldBitmap);
        DeleteDC(item->memdc);
        item->memdc = NULL;
    }
    if (item->bitmap) {
        DeleteObject(item->bitmap);
        item->bitmap = NULL;
    }
    item->bits = NULL;

    if (item->mirrorDc) {
        SelectObject(item->mirrorDc, item->mirrorOldBitmap);
        DeleteDC(item->mirrorDc);
        item->mirrorDc = NULL;
    }
    if (item->mirrorBitmap) {
        DeleteObject(item->mirrorBitmap);
        item->mirrorBitmap = NULL;
    }
    item->mirrorBits = NULL;
}

static int agwCreateSurface(AgaveWebView* item, int width, int height)
{
    BITMAPINFO info;
    HDC hdc;

    if (width < 1) width = 1;
    if (height < 1) height = 1;

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    hdc = GetDC(item->hwnd);
    item->bitmap = CreateDIBSection(hdc, &info, DIB_RGB_COLORS, (void**)&item->bits, NULL, 0);

    if (item->bitmap) {
        item->memdc = CreateCompatibleDC(hdc);
        item->oldBitmap = SelectObject(item->memdc, item->bitmap);
    }

    /* Second buffer: the one the window paints from. */
    item->mirrorBitmap = CreateDIBSection(hdc, &info, DIB_RGB_COLORS, (void**)&item->mirrorBits, NULL, 0);

    if (item->mirrorBitmap) {
        item->mirrorDc = CreateCompatibleDC(hdc);
        item->mirrorOldBitmap = SelectObject(item->mirrorDc, item->mirrorBitmap);
    }
    ReleaseDC(item->hwnd, hdc);

    item->width = width;
    item->height = height;
    return item->bits != NULL && item->mirrorBits != NULL;
}

static void agwEnsureSurface(AgaveWebView* item, int width, int height)
{
    int created;

    if (width < 1) width = 1;
    if (height < 1) height = 1;

    if (item->bits && item->width == width && item->height == height)
        return;

    /* The UI thread blits the mirror DIB from agwPaint() under agwLock.
     * Replacing the surface without the lock can free the mirror DC while
     * BitBlt() is using it - and a window resize is exactly when the surface
     * is replaced. */
    agwLock();
    agwDestroySurface(item);
    created = agwCreateSurface(item, width, height);
    agwUnlock();

    if (!created)
        return;

    if (item->backendView) {
        item->backend->resize(item->backendView, item->bits, width, height, width * 4);
    } else {
        item->backendView = item->backend->create(item, item->bits, width, height, width * 4);
        /* Local: the same pointer; kept for the callback lookups (agwByView)
         * and for macross's own user_data convention. */
        item->view = (MaCrossView*)item->backendView;

        if (item->backendView) {
            if (item->zoom != 100)
                agwSubmitSimple(item, AGW_VIEW_CMD_ZOOM, item->zoom, 0, 0, 0);

            /* A queued CMD_NAVIGATE will load the URL; doing it here as well made
             * every first navigation fetch the page twice. */
            if (item->url[0] && !item->navigatePending)
                agwSubmitText(item, AGW_VIEW_CMD_NAVIGATE, item->url, 0, 0, 0, 0);
        }
    }

    if (item->hwnd)
        InvalidateRect(item->hwnd, NULL, FALSE);
}

/* Applies the last size queued for each view, once per drain instead of once
 * per WM_SIZE. A live drag posts a resize command for every mouse step; doing
 * the surface rebuild + resize right there made the engine drop behind the
 * drag on heavy pages. */
static void agwApplyPendingResizes(void)
{
    AgaveWebView* batch[AGW_MAX_VIEWS];
    int count = 0;
    int i;

    agwLock();
    for (i = 0; i < g_viewCount; ++i) {
        AgaveWebView* item = g_views[i];

        if (!item || item->destroyed || !item->pendingResize)
            continue;

        item->pendingResize = 0;
        item->refs++;
        batch[count++] = item;
    }
    agwUnlock();

    for (i = 0; i < count; ++i) {
        AgaveWebView* item = batch[i];

        if (!item->destroyed)
            agwEnsureSurface(item, item->wantWidth, item->wantHeight);

        agwRelease(item);
    }
}

/* ------------------------------------------------------------------------- */
/* engine thread                                                              */
/* ------------------------------------------------------------------------- */

/* Watchdog: is the page stuck, or just busy?
 *
 * Time alone cannot answer that, and neither can the command queue alone. What
 * separates the two is *what the engine is doing* and *who is waiting*:
 *
 *   JS on the stack (antb_engines_js_depth)  -> a page script is running, i.e.
 *       interruptible.  Load events and timers run in macross_event_dispatch(),
 *       outside any command, so the queue head ages while the page works.
 *   A navigation waiting (navigate/load/reload/back/forward)  -> the user asked
 *       to leave; the page is replaced, so its script can be killed after a
 *       very short budget (AGW_NAV_ABORT_MS, 100 ms).
 *   Any other user action waiting (stop/search) -> the page stays, so the
 *       longer budget applies (AGW_USER_ABORT_MS, 1.5 s).
 *   Neither JS nor a command running, and the queue still does not move ->
 *       the engine is wedged outside JS (modal dialog, deadlock, native loop).
 *       Aborting does nothing here: nobody polls the flag.
 *
 * Verdicts and actions:
 *
 *   navigation blocked by JS  -> abort after AGW_NAV_ABORT_MS    (100 ms, silent)
 *   user action blocked by JS -> abort after AGW_USER_ABORT_MS   (1.5 s)
 *   JS running with nobody waiting -> abort after AGW_UNRESPONSIVE_MS (12 s,
 *       the browser "page unresponsive" scale; a legitimate 2.9 s onload
 *       (css3test) is not touched)
 *   wedged outside JS          -> do NOT abort; record + tell the user
 *
 * A wall-clock budget still exists, because "unresponsive" is by definition a
 * patience threshold (Chrome settles on ~15 s) - but it is now applied to the
 * right clock. */
static HANDLE g_watchdog;
static volatile LONG g_watchdogStop;

static const char* agwHangReason(int kind)
{
    switch (kind) {
    case AGW_HANG_USER_ACTION:
        return "A page script was blocking a user action, so it was aborted.";
    case AGW_HANG_TEARDOWN:
        return "A page teardown handler (pagehide/visibilitychange/unload) did not "
               "return, so it was aborted.";
    case AGW_HANG_UNRESPONSIVE:
        return "The page script has not returned; the page is still running.";
    case AGW_HANG_STALL:
        return "The engine stopped processing commands while no script was running; "
               "aborting cannot help, so nothing was killed.";
    default:
        return "";
    }
}

/* GUI-only failures leave no stderr behind: the page simply stays broken. Keep
 * one line next to the exe (same idea as agb_crash.txt) so "it works in the
 * probe but not in the window" has evidence - including the command that was
 * waiting and the VM depth, which is what the three verdicts are made of. */
static void agwWatchdogRecord(int kind, unsigned int jsDepth, int headType,
                              DWORD busyMs, DWORD jsBusyMs, DWORD queueMs)
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, sizeof(path) - 1);
    SYSTEMTIME st;
    FILE* f;

    if (!len || len >= sizeof(path) - 1)
        return;

    path[len] = 0;
    while (len > 0 && path[len - 1] != '\\' && path[len - 1] != '/')
        --len;
    path[len] = 0;

    if (strlen(path) + sizeof("agb_watchdog.txt") >= sizeof(path))
        return;
    strcat(path, "agb_watchdog.txt");

    f = fopen(path, "a");
    if (!f)
        return;

    GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d pid=%lu kind=%d jsdepth=%u head=%d busy=%lums jsbusy=%lums queue=%lums\n",
            (int)st.wYear, (int)st.wMonth, (int)st.wDay,
            (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
            (unsigned long)GetCurrentProcessId(),
            kind, jsDepth, headType,
            (unsigned long)busyMs, (unsigned long)jsBusyMs, (unsigned long)queueMs);
    fclose(f);
}

/* The VM's stack renderer writes ANSI colors even when the target is a file
 * (it renders "pretty" frames). The dump file is meant to be read by a human
 * and grepped by the tests, so the escape sequences are stripped here. */
static void agwStripAnsiInFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    char* data;
    char* out;
    long size;
    size_t i, o = 0;

    if (!f)
        return;

    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 || size > 4 * 1024 * 1024) {
        fclose(f);
        return;
    }

    fseek(f, 0, SEEK_SET);
    data = (char*)malloc((size_t)size);

    if (!data) {
        fclose(f);
        return;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return;
    }

    fclose(f);

    out = (char*)malloc((size_t)size + 1);

    if (!out) {
        free(data);
        return;
    }

    for (i = 0; i < (size_t)size; ++i) {
        if ((unsigned char)data[i] == 0x1b && i + 1 < (size_t)size && data[i + 1] == '[') {
            i += 2;

            while (i < (size_t)size && data[i] >= '0' && data[i] <= '9')
                ++i;
            while (i < (size_t)size && (data[i] == ';' || data[i] == ':'))
                ++i;
            while (i < (size_t)size && !((unsigned char)data[i] >= 0x40 && (unsigned char)data[i] <= 0x7e))
                ++i;

            continue;   /* drop ESC[...<final> */
        }

        out[o++] = data[i];
    }

    out[o] = 0;

    f = fopen(path, "wb");

    if (f) {
        fwrite(out, 1, o, f);
        fclose(f);
    }

    free(out);
    free(data);
}

/* Captures the live JS stack of the running script into its own file next to
 * the exe (`agb_busy_<time>_pid<pid>.txt`), one file per report, so a second
 * episode never overwrites the evidence of the first. The engine thread is
 * frozen for the walk: the VM must not be moving under it, and the page is
 * stuck anyway. Returns 0 when no stack could be taken (e.g. the page was busy
 * outside JS); the metrics are still written. */
static int agwCaptureBusyStack(const char* reason, unsigned int busyMs,
                               unsigned int jsBusyMs, unsigned int jsDepth,
                               int headType, DWORD queueMs, char* out, size_t cap)
{
    char path[MAX_PATH];
    char stack[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, sizeof(path) - 1);
    SYSTEMTIME st;
    int captured = 0;

    if (out && cap)
        out[0] = 0;

    if (!len || len >= sizeof(path) - 1)
        return 0;

    path[len] = 0;
    while (len > 0 && path[len - 1] != '\\' && path[len - 1] != '/')
        --len;
    path[len] = 0;

    GetLocalTime(&st);
    _snprintf(path + len, sizeof(path) - len,
              "agb_busy_%04d%02d%02d-%02d%02d%02d_pid%lu.txt",
              (int)st.wYear, (int)st.wMonth, (int)st.wDay,
              (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
              (unsigned long)GetCurrentProcessId());
    path[sizeof(path) - 1] = 0;

    if (out && cap) {
        strncpy(out, path, cap - 1);
        out[cap - 1] = 0;
    }

    /* The stack goes to the same file, but the bridge writer truncates, so the
     * metrics are appended afterwards. */
    _snprintf(stack, sizeof(stack), "%s", path);
    stack[sizeof(stack) - 1] = 0;

    if (jsDepth && g_engineThread) {
        void* context = agb_busy_js_context();

        if (context && SuspendThread(g_engineThread) != (DWORD)-1) {
            captured = agb_dump_js_stack_for(context, stack);
            ResumeThread(g_engineThread);
        }
    }

    {
        FILE* f = fopen(path, "a");

        if (!f)
            return 0;

        if (!captured)
            fprintf(f, "(no JS on the stack at the time of the report)\n");

        fprintf(f, "\n# agb busy report\n# time=%04d-%02d-%02d %02d:%02d:%02d pid=%lu\n"
                   "# reason=%s\n"
                   "# busy=%lums jsbusy=%lums jsdepth=%u head=%d queue=%lums\n",
                (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
                (unsigned long)GetCurrentProcessId(),
                reason ? reason : "",
                (unsigned long)busyMs, (unsigned long)jsBusyMs, jsDepth,
                headType, (unsigned long)queueMs);
        fclose(f);
    }

    agwStripAnsiInFile(path);

    return 1;
}

/* Hand the verdict to the UI thread: postMessage is the only thing the
 * engine thread is allowed to do to a window.
 *
 * One engine thread runs every view, so the script that is running belongs to
 * exactly one of them: agb_busy_js_context() names it. Before this existed the
 * verdict was broadcast, which popped a dialog per view (and the wrong tab
 * answered it). With no honest owner nothing is posted - the log line still
 * carries the numbers. */
static AgaveWebView* agwBusyViewLocked(void)
{
    void* context = agb_busy_js_context();
    int i;

    if (!context)
        return NULL;

    for (i = 0; i < g_viewCount; ++i) {
        AgaveWebView* item = g_views[i];

        if (!item || item->destroyed || !item->backendView)
            continue;

        if (item->backend->jsContext(item->backendView) != context)
            continue;

        if (!item->hwnd || !item->callbacks.on_unresponsive)
            continue;

        return item;
    }

    return NULL;
}

static void agwNotifyHang(int kind, unsigned int jsDepth, int headType,
                          DWORD busyMs, DWORD queueMs, const char* stackPath)
{
    AgaveWebView* target;
    AgwHangPayload* payload;

    agwLock();
    target = agwBusyViewLocked();
    agwUnlock();

    if (!target) {
        agbLog(AGB_LOG_EVT, "watchdog: no view owns the running JS context; notice dropped");
        return;
    }

    payload = (AgwHangPayload*)malloc(sizeof(AgwHangPayload));

    if (!payload)
        return;

    payload->kind = kind;
    payload->jsDepth = jsDepth;
    payload->headType = headType;
    payload->busyMs = (unsigned int)busyMs;
    payload->queueMs = (unsigned int)queueMs;
    payload->stackPath[0] = 0;

    if (stackPath && stackPath[0]) {
        strncpy(payload->stackPath, stackPath, sizeof(payload->stackPath) - 1);
        payload->stackPath[sizeof(payload->stackPath) - 1] = 0;
    }

    if (!PostMessageA(target->hwnd, AGW_MSG_UNRESPONSIVE, 0, (LPARAM)payload))
        free(payload);
}

static DWORD WINAPI agwWatchdogThread(LPVOID param)
{
    DWORD jsBusyMs = 0;      /* how long JS has been continuously on the stack */
    DWORD lastTick = GetTickCount();
    DWORD lastReport = 0;    /* rate limit: one verdict per AGW_REPORT_GAP_MS */
    int stallReported = 0;
    int noticeSent = 0;      /* one advisory notice per busy streak */

    (void)param;

    while (!InterlockedCompareExchange(&g_watchdogStop, 0, 0)) {
        DWORD now, delta, busyMs = 0, queueMs = 0, teardownMs = 0;
        unsigned int jsDepth;
        int headType = -1;
        int kind = AGW_HANG_NONE;

        /* 50 ms, not 200: the navigation budget is 100 ms, so a coarser tick
         * would make the real reaction 100-300 ms. The tick only reads two
         * integers and the queue head. */
        Sleep(50);

        now = GetTickCount();
        delta = now - lastTick;
        lastTick = now;

        jsDepth = antb_engines_js_depth();
        if (jsDepth)
            jsBusyMs += delta;
        else
            jsBusyMs = 0;

        if (InterlockedCompareExchange(&g_engineBusy, 0, 0))
            busyMs = (DWORD)(now - g_engineBusySince);

        queueMs = agwQueueWaitType(&headType);
        if (headType < 0)
            stallReported = 0;

        /* AGB_WD_DEBUG=1: one line per tick. This is the only way to see what
         * the three verdicts are made of while a page is being slow-but-fine. */
        if (g_watchdogDebug)
            agbLog(AGB_LOG_EVT, "wd js=%u jsbusy=%lums busy=%lums queue=%lums head=%d",
                   jsDepth, (unsigned long)jsBusyMs, (unsigned long)busyMs,
                   (unsigned long)queueMs, headType);

        if (jsDepth) {
            unsigned long teardownAt = agb_teardown_started_ms();

            teardownMs = teardownAt ? (DWORD)(now - (DWORD)teardownAt) : 0;
        } else {
            teardownMs = 0;
        }

        if (jsDepth && teardownMs && teardownMs >= g_teardownMs) {
            /* The outgoing document's pagehide/visibilitychange/unload handler
             * is not returning. Cutting it is safe: each handler is a separate
             * top-level eval, so the next event still fires. Re-armed every
             * tick so a second stuck handler is cut too. */
            kind = AGW_HANG_TEARDOWN;
        } else if (jsDepth && headType >= 0 &&
            ((agwCommandIsNavigation(headType) && queueMs >= g_navAbortMs) ||
             (agwCommandIsUserAction(headType) && queueMs >= g_userAbortMs))) {
            kind = AGW_HANG_USER_ACTION;
        } else if (jsBusyMs >= g_unresponsiveMs || busyMs >= g_unresponsiveMs) {
            kind = AGW_HANG_UNRESPONSIVE;
        } else if (!jsDepth && !busyMs && headType >= 0 && queueMs >= g_nativeStallMs && !stallReported) {
            kind = AGW_HANG_STALL;
        }

        /* An idle engine ends the busy streak: a new hang gets a new notice. */
        if (!jsDepth && !busyMs)
            noticeSent = 0;

        if (kind == AGW_HANG_NONE)
            continue;

        /* The advisory verdict (nobody is waiting on the page) must not kill
         * anything: the host asks the user to wait or close the page. Every
         * other verdict here is a user action that is blocked, and aborting is
         * what makes reload/navigate/stop work on a hung page. */
        if (kind == AGW_HANG_UNRESPONSIVE) {
            if (noticeSent)
                continue;               /* one notice per busy streak */
        } else if (kind == AGW_HANG_STALL) {
            stallReported = 1;
        } else {
            antb_interrupt_all_engines();
        }

        /* A script that ignores the abort would otherwise report every tick. */
        if (lastReport && (DWORD)(now - lastReport) < AGW_REPORT_GAP_MS)
            continue;
        lastReport = now;

        if (kind == AGW_HANG_UNRESPONSIVE)
            noticeSent = 1;

        {
            /* The unresponsive budget is served by two clocks: JS on the VM
             * stack (timers/onload, outside any command) and the engine's
             * command clock. Report the one that tripped - showing only the
             * command clock is how the dialog used to say "(0.0 s)". */
            DWORD reportBusy = busyMs;

            if (kind == AGW_HANG_UNRESPONSIVE && jsBusyMs > reportBusy)
                reportBusy = jsBusyMs;

            agbLog(AGB_LOG_EVT,
                   "watchdog: %s (jsdepth=%u head=%d busy=%lums jsbusy=%lums queue=%lums teardown=%lums ctx=%p)",
                   kind == AGW_HANG_TEARDOWN ? "aborting a teardown handler that does not return"
                   : kind == AGW_HANG_USER_ACTION
                       ? (agwCommandIsNavigation(headType) ? "aborting a script that blocks a navigation"
                                                           : "aborting a script that blocks a user action")
                   : kind == AGW_HANG_UNRESPONSIVE ? "reporting an unresponsive page script (not aborting)"
                   : "engine stalled outside JS",
                   jsDepth, headType, (unsigned long)reportBusy, (unsigned long)jsBusyMs,
                   (unsigned long)queueMs, (unsigned long)teardownMs, agb_busy_js_context());
            agwWatchdogRecord(kind, jsDepth, headType, reportBusy, jsBusyMs, queueMs);

            if (kind == AGW_HANG_UNRESPONSIVE) {
                char stackPath[MAX_PATH];

                stackPath[0] = 0;
                agwCaptureBusyStack(agwHangReason(kind), (unsigned int)reportBusy,
                                    (unsigned int)jsBusyMs, jsDepth, headType,
                                    queueMs, stackPath, sizeof(stackPath));
                agwNotifyHang(kind, jsDepth, headType, reportBusy, queueMs, stackPath);
            }
        }
    }

    return 0;
}

static void agwWatchdogStart(void)
{
    const char* env;

    if (g_watchdog) return;

    if ((env = getenv("AGB_NAV_ABORT_MS")) != NULL && atoi(env) > 0)
        g_navAbortMs = (DWORD)atoi(env);
    if ((env = getenv("AGB_TEARDOWN_MS")) != NULL && atoi(env) > 0)
        g_teardownMs = (DWORD)atoi(env);
    if ((env = getenv("AGB_USER_ABORT_MS")) != NULL && atoi(env) > 0)
        g_userAbortMs = (DWORD)atoi(env);
    if ((env = getenv("AGB_UNRESPONSIVE_MS")) != NULL && atoi(env) > 0)
        g_unresponsiveMs = (DWORD)atoi(env);
    if ((env = getenv("AGB_STUCK_EXEC_MS")) != NULL && atoi(env) > 0) {
        /* Older name for the unresponsive budget, kept so existing scripts and
         * the hang regression keep working. */
        g_unresponsiveMs = (DWORD)atoi(env);
    }
    if ((env = getenv("AGB_NATIVE_STALL_MS")) != NULL && atoi(env) > 0)
        g_nativeStallMs = (DWORD)atoi(env);

    g_watchdogDebug = getenv("AGB_WD_DEBUG") != NULL;

    g_watchdog = CreateThread(NULL, 0, agwWatchdogThread, NULL, 0, NULL);
}

/* 1 when the oldest queued command has been waiting at least minMs. */
static int agwQueueStuckFor(int minMs)
{
    DWORD at = 0;

    agwLock();
    if (g_queueHead != g_queueTail) at = g_queue[g_queueHead].at;
    agwUnlock();

    if (!at) return 0;
    if (minMs <= 0) return 1;
    return (int)(GetTickCount() - at) >= minMs;
}

/* How long the command at the head has been waiting and what it is; 0/0 when
 * the queue is empty (an empty queue means the engine is simply doing its own
 * work, which is not the host's business). */
static DWORD agwQueueWaitType(int* type)
{
    DWORD at = 0;
    int queuedType = 0;

    agwLock();
    if (g_queueHead != g_queueTail) {
        at = g_queue[g_queueHead].at;
        queuedType = g_queue[g_queueHead].type;
    }
    agwUnlock();

    /* -1, not 0: AGW_CMD_NAVIGATE *is* 0, so 0 cannot double as "empty". */
    if (type)
        *type = at ? queuedType : -1;

    if (!at) return 0;
    return (DWORD)(GetTickCount() - at);
}

/* Commands that replace or start page execution: they are what a browser makes
 * "abort whatever the page is doing" mean. Mouse/key/resize must not abort. */
/* ---- protocol submission ------------------------------------------------ *
 * Every per-view command goes to the backend as one AgwViewCommand
 * (AgwViewProtocol.h): the local backend maps it back onto macross, a remote
 * one serializes it. The helpers keep the drain switch readable and are the
 * single place that guards "no view yet". */
static int agwSubmitSimple(AgaveWebView* item, int kind, int a, int b, int c, int d)
{
    AgwViewCommand cmd;

    if (!item || !item->backendView || !item->backend)
        return 0;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = kind;
    cmd.a = a;
    cmd.b = b;
    cmd.c = c;
    cmd.d = d;
    return item->backend->submit(item->backendView, &cmd);
}

static int agwSubmitText(AgaveWebView* item, int kind, const char* text,
                         int a, int b, int c, int d)
{
    AgwViewCommand cmd;

    if (!item || !item->backendView || !item->backend)
        return 0;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = kind;
    cmd.a = a;
    cmd.b = b;
    cmd.c = c;
    cmd.d = d;
    cmd.text = text;
    cmd.textLen = text ? (int)strlen(text) : 0;
    return item->backend->submit(item->backendView, &cmd);
}

static void agwDrainCommands(void)
{
    for (;;) {
        AgwCommand command;
        const char* payload;
        int have = 0;
        int i;
        AgaveWebView* item;

        agwLock();
        if (g_queueHead != g_queueTail) {
            command = g_queue[g_queueHead];
            g_queue[g_queueHead].heapText = NULL;
            g_queueHead = (g_queueHead + 1) % AGW_QUEUE_SIZE;
            have = 1;
        }
        agwUnlock();

        if (!have)
            break;

        payload = command.heapText ? command.heapText : command.text;

        /* Resolve the target and pin it for the whole command. EVAL can run
         * arbitrary JS for an arbitrarily long time, and the host may call
         * agave_webview_destroy() meanwhile: without the pin it would see
         * refs == 0 and free the engine under the running script, which is
         * exactly the 0xC0000005 that looked like a JIT loop crash (R61).
         * The same lookup also keeps a queued command from running against
         * whichever view now occupies the destroyed view's slot. */
        item = NULL;
        agwLock();
        for (i = 0; i < g_viewCount; ++i) {
            if (g_views[i] && g_views[i]->id == command.viewId &&
                !g_views[i]->destroyed) {
                item = g_views[i];
                item->refs++;
                break;
            }
        }
        agwUnlock();

        if (!item) {
            free(command.heapText);
            continue;
        }

        g_engineBusySince = GetTickCount();
        InterlockedExchange(&g_engineBusy, 1);
        item->busySince = g_engineBusySince;
        InterlockedExchange(&item->busy, 1);

        switch (command.type) {
        case AGW_CMD_NAVIGATE:
            item->navigatePending = 0;

            if (item->backendView) {
                agwSubmitText(item, AGW_VIEW_CMD_NAVIGATE, payload, 0, 0, 0, 0);
            } else {
                strncpy(item->url, payload, sizeof(item->url) - 1);
                item->url[sizeof(item->url) - 1] = 0;
                agwEnsureSurface(item, item->wantWidth, item->wantHeight);
            }
            break;

        case AGW_CMD_LOAD_HTML:
            agwSubmitText(item, AGW_VIEW_CMD_LOAD_HTML, payload, 0, 0, 0, 0);
            break;

        case AGW_CMD_BACK:    agwSubmitSimple(item, AGW_VIEW_CMD_BACK, 0, 0, 0, 0); break;
        case AGW_CMD_FORWARD: agwSubmitSimple(item, AGW_VIEW_CMD_FORWARD, 0, 0, 0, 0); break;
        case AGW_CMD_RELOAD:  agwSubmitSimple(item, AGW_VIEW_CMD_RELOAD, 0, 0, 0, 0); break;
        case AGW_CMD_STOP:    agwSubmitSimple(item, AGW_VIEW_CMD_STOP, 0, 0, 0, 0); break;

        case AGW_CMD_EVAL:
            agwSubmitText(item, AGW_VIEW_CMD_EVAL, payload, 0, 0, 0, 0);
            break;

        case AGW_CMD_VISIBLE:
            /* Page Visibility: the document state and the event. The engine
             * loop also uses this flag to skip layout+paint for a view nobody
             * can see (background tab / minimized window); the first frame
             * after it comes back has to repaint the whole surface, because
             * the frames in between were never drawn. */
            if (item->visible != (command.a ? 1 : 0)) {
                item->visible = command.a ? 1 : 0;

                if (item->visible)
                    item->forceFullPaint = 1;
            }
            agwSubmitSimple(item, AGW_VIEW_CMD_VISIBLE, item->visible, 0, 0, 0);
            break;

        case AGW_CMD_ZOOM:
            item->zoom = command.a > 0 ? command.a : 100;
            agwSubmitSimple(item, AGW_VIEW_CMD_ZOOM, item->zoom, 0, 0, 0);
            if (item->hwnd)
                InvalidateRect(item->hwnd, NULL, FALSE);
            break;

        case AGW_CMD_RESIZE:
            /* Only remember the size: a live drag queues one of these per
             * WM_SIZE, and recreating the DIB (plus a restyle) for every one of
             * them is wasted work - the page only ever sees the last size of
             * the batch. agwApplyPendingResizes() applies it after the queue
             * has drained. */
            item->wantWidth = command.a;
            item->wantHeight = command.b;
            item->pendingResize = 1;
            break;

        case AGW_CMD_UA:
            if (payload[0]) {
                strncpy(g_userAgent, payload, sizeof(g_userAgent) - 1);
                g_userAgent[sizeof(g_userAgent) - 1] = 0;
            }
            break;

        case AGW_CMD_DL_DIR:
            if (payload[0])
                macross_set_download_dir(payload);
            break;

        case AGW_CMD_DL_CANCEL:
            macross_cancel_download(command.x);
            break;

        case AGW_CMD_MOUSE:
            agwSubmitSimple(item, AGW_VIEW_CMD_MOUSE,
                            command.a, command.b, command.x, command.y);
            break;

        case AGW_CMD_KEY:
            agwSubmitSimple(item, AGW_VIEW_CMD_KEY,
                            command.a, command.b, command.x, 0);
            break;

        case AGW_CMD_CHAR:
            agwSubmitText(item, AGW_VIEW_CMD_TEXT, payload, 0, 0, 0, 0);
            break;

        case AGW_CMD_FOCUS:
            agwSubmitSimple(item, AGW_VIEW_CMD_FOCUS, 1, 0, 0, 0);
            break;

        case AGW_CMD_SEARCH:
            if (payload[0])
                agwSubmitText(item, AGW_VIEW_CMD_SEARCH, payload,
                              command.a ? 1 : 0, command.b ? 1 : 0, 0, 0);
            break;

        case AGW_CMD_CONTEXT:
            agwSubmitSimple(item, AGW_VIEW_CMD_CONTEXT_MENU,
                            command.a, 0, command.x, command.y);
            break;

        default:
            break;
        }

        free(command.heapText);
        InterlockedExchange(&item->busy, 0);
        InterlockedExchange(&g_engineBusy, 0);
        /* Drop the pin: if the host destroyed the control while the command
         * was running, this is where the engine view is finally torn down
         * (on the engine thread, never under a running script). */
        agwRelease(item);
    }
}

/* ---- events from a remote backend --------------------------------------- *
 * A backend with its own process reports titles/progress/frames through
 * poll(); the local backend never does (its events are the macross callbacks
 * above). Same fields and callbacks either way, so a host cannot tell them
 * apart. */
static void agwDispatchBackendEvent(AgaveWebView* item, const AgwViewEvent* ev)
{
    if (!item || !ev)
        return;

    switch (ev->kind) {
    case AGW_VIEW_EVENT_TITLE:
        agwLock();
        if (ev->text) {
            strncpy(item->title, ev->text, sizeof(item->title) - 1);
            item->title[sizeof(item->title) - 1] = 0;
        }
        agwUnlock();

        if (item->callbacks.on_title)
            item->callbacks.on_title(item, item->title, item->user);
        if (item->hwnd)
            PostMessageA(item->hwnd, AGW_MSG_STATE, 0, 0);
        break;

    case AGW_VIEW_EVENT_STATUS:
        if (item->callbacks.on_status && ev->text)
            item->callbacks.on_status(item, ev->text, item->user);
        break;

    case AGW_VIEW_EVENT_URL:
        agwLock();
        if (ev->text) {
            strncpy(item->url, ev->text, sizeof(item->url) - 1);
            item->url[sizeof(item->url) - 1] = 0;
        }
        agwUnlock();

        if (item->callbacks.on_url)
            item->callbacks.on_url(item, item->url, item->user);
        break;

    case AGW_VIEW_EVENT_PROGRESS: {
        int notify = 0;

        agwLock();
        item->progress = ev->a;
        item->loading = ev->b ? 0 : 1;

        if (ev->b && item->lastRecordedProgress != 100) {
            item->lastRecordedProgress = 100;
            notify = 1;
        } else if (!ev->b) {
            item->lastRecordedProgress = ev->a;
        }
        agwUnlock();

        if (item->callbacks.on_progress)
            item->callbacks.on_progress(item, ev->a, ev->b ? 1 : 0, item->user);
        if (notify && item->hwnd)
            PostMessageA(item->hwnd, AGW_MSG_STATE, 0, 0);
        break;
    }

    case AGW_VIEW_EVENT_MESSAGE:
        if (item->callbacks.on_message)
            item->callbacks.on_message(item, ev->text ? ev->text : "", item->user);
        break;

    case AGW_VIEW_EVENT_EXIT:
        agwLock();
        item->loading = 0;
        agwUnlock();

        if (item->callbacks.on_error)
            item->callbacks.on_error(item, ev->a, ev->text ? ev->text : item->url, item->user);
        if (item->hwnd)
            PostMessageA(item->hwnd, AGW_MSG_STATE, 0, 0);
        break;

    default:
        break;
    }
}

/* Copies a published remote frame into the mirror the window paints from. One
 * copy, straight from the backend's canvas - the host-side DIB is not used by
 * a remote backend. Returns 1 when a frame was consumed and acked. */
static int agwCopyRemoteFrame(AgaveWebView* item)
{
    const MC_BYTE* src;
    int w = 0, h = 0, pitch = 0;
    uint32_t seq = 0;
    int copied = 0;

    if (!item || !item->backendView || !item->backend || !item->backend->frame)
        return 0;

    src = item->backend->frame(item->backendView, &w, &h, &pitch, &seq);
    if (!src)
        return 0;

    agwLock();
    if (item->mirrorBits && item->width > 0 && item->height > 0 && w > 0 && h > 0) {
        int rows = item->height < h ? item->height : h;
        int rowBytes = item->width * 4;
        int copyBytes = w * 4;
        int y;

        if (copyBytes > rowBytes)
            copyBytes = rowBytes;

        for (y = 0; y < rows; ++y)
            memcpy(item->mirrorBits + (size_t)y * (size_t)rowBytes,
                   src + (size_t)y * (size_t)pitch, (size_t)copyBytes);

        item->lastMirrorMs = GetTickCount();
        copied = 1;
    }
    agwUnlock();

    if (copied) {
        AgwFrameAck ack;

        ack.bufferId = 1;
        ack.seq = seq;
        item->backend->frameAck(item->backendView, &ack);

        if (item->hwnd && IsWindowVisible(item->hwnd))
            InvalidateRect(item->hwnd, NULL, FALSE);
    }

    return copied;
}

static DWORD WINAPI agwEngineThread(LPVOID param)
{
    (void)param;

    g_engineThreadId = GetCurrentThreadId();

    /* The process-level half of the backend: engine init, the global timer/
     * network pump, shutdown. `local` initializes macross and pumps its event
     * queue; a backend with its own instance will own those instead. */
    g_processBackend = agwBackendByName(getenv("AGB_VIEW_BACKEND"));
    if (!g_processBackend)
        g_processBackend = agwBackendLocal();

    if (g_processBackend != agwBackendLocal()) {
        /* Mixed runs (a local view next to a remote one) still need the
         * in-process engine; a pure remote run pays for it but stays simple. */
        if (!agwBackendLocal()->processInit(1024, 768))
            return 0;
        g_processBackend->processInit(1024, 768);
    } else if (!g_processBackend->processInit(1024, 768)) {
        return 0;
    }

    for (;;) {
        int i;
        int active = 0;

        if (g_engineQuit)
            break;

        /* Popups created from window.open() live on this thread, so their
         * messages have to be pumped here or they never paint. */
        {
            MSG msg;

            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        agwDrainCommands();
        agwApplyPendingResizes();

        g_processBackend->processPump();

        /* Snapshot the live views under the lock, then update them *without* it:
         * a slow paint (heavy page) used to block the host's UI thread, which
         * takes the same lock whenever it posts a command. */
        {
            AgaveWebView* batch[AGW_MAX_VIEWS];
            int count = 0;

            agwLock();
            for (i = 0; i < g_viewCount; ++i) {
                AgaveWebView* item = g_views[i];

                if (!item || !item->backendView || item->destroyed)
                    continue;

                item->refs++;
                batch[count++] = item;
            }
            agwUnlock();

            for (i = 0; i < count; ++i) {
                AgaveWebView* item = batch[i];

                if (!item->destroyed) {
                    unsigned long began = GetTickCount();

                    /* Remote backends report everything the local one calls back
                     * about (title, progress, url, frames, death) through the
                     * event ring; drain it before the frame work below. */
                    if (item->backend->poll) {
                        AgwViewEvent events[8];
                        int drained;

                        while ((drained = item->backend->poll(item->backendView,
                                                              events, 8)) > 0) {
                            int e;

                            for (e = 0; e < drained; ++e)
                                agwDispatchBackendEvent(item, &events[e]);
                        }
                    }

                    /* A hidden view (background tab, minimized window) keeps
                     * running JS, timers and network in macross_view_pump(), but
                     * its layout+paint is skipped: a background tab used to cost
                     * a full layout and paint per engine iteration, for pixels
                     * nobody could see (and it is what S3 - one engine thread
                     * per view - has to fix first: K threads must not all paint
                     * a hidden view). The page still lays out on demand: every
                     * geometry read in WebCore calls updateLayout(). */
                    if (item->visible) {
                        if (item->forceFullPaint && item->width > 0 && item->height > 0) {
                            MC_RECT full;

                            /* Coming back from hidden: the frames in between
                             * were never drawn, so the accumulated dirty rects
                             * are not enough - repaint the whole surface. */
                            full.x = 0;
                            full.y = 0;
                            full.w = item->width;
                            full.h = item->height;
                            item->backend->update(item->backendView, &full);
                            item->forceFullPaint = 0;
                        } else {
                            item->backend->update(item->backendView, NULL);
                        }

                        item->painted++;
                    } else {
                        item->backend->pump(item->backendView);
                        item->skippedPaints++;
                    }

                    if (getenv("AGB_LOOP_DEBUG")) {
                        static int frames = 0;
                        static unsigned long total = 0;

                        total += GetTickCount() - began;
                        ++frames;

                        if (frames % 100 == 0)
                            fprintf(stderr, "[loop] frames=%d update=%u ms (avg %u ms)\n",
                                    frames, (unsigned)total, (unsigned)(total / frames));

                        if ((item->painted + item->skippedPaints) % 20 == 0)
                            fprintf(stderr, "[loop] view=%p paints=%lu skips=%lu\n",
                                    (void*)item, item->painted, item->skippedPaints);
                    }
                }

                /* Copy the finished frame into the mirror buffer the window
                 * paints from. Doing this under the lock keeps the UI thread
                 * from blitting a half-written frame (that tore while
                 * scrolling); the copy is ~1ms, the engine paints outside it.
                 * The size check is inside the lock too: agwEnsureSurface()
                 * replaces both buffers (and their dimensions) under it.
                 * A hidden view paints nothing, so it has nothing to copy. */
                if (item->visible) {
                    /* A backend that keeps its canvas in shared memory hands out
                     * the frame here (one copy, straight into the mirror); the
                     * local backend has none and painted into item->bits. */
                    if (agwCopyRemoteFrame(item)) {
                        /* copied + acked + invalidated inside */
                    } else if (!item->backend->frame) {
                        int copied = 0;

                        agwLock();
                        if (item->mirrorBits && item->bits && item->width > 0 && item->height > 0) {
                            size_t bytes = (size_t)item->width * (size_t)item->height * 4u;

                            memcpy(item->mirrorBits, item->bits, bytes);
                            item->lastMirrorMs = GetTickCount();
                            copied = 1;
                        }
                        agwUnlock();

                        if (copied && item->hwnd && IsWindowVisible(item->hwnd))
                            InvalidateRect(item->hwnd, NULL, FALSE);
                    }
                }

                g_lastActive = item;
                active = 1;

                agwRelease(item);
            }
        }

        /* Retired views are torn down here: after event dispatch (their timers
         * have run) and after this iteration's paint (refs are dropped). */
        agwReapViews();

        Sleep(active ? 6 : 12);
    }

    /* Shutdown: views the host retired but nobody reaped yet go through the
     * same teardown, then the ones still alive. */
    agwReapViews();
    for (;;) {
        AgaveWebView* item = NULL;

        agwLock();
        if (g_viewCount > 0) {
            item = g_views[--g_viewCount];
            if (item) {
                item->destroyed = 1;
                item->refs = 0;          /* nobody pins a view at shutdown */
                item->reapNext = g_reapHead;
                g_reapHead = item;
            }
        }
        agwUnlock();

        if (!item)
            break;

        agwReapViews();
    }

    if (g_processBackend)
        g_processBackend->processShutdown();
    return 0;
}

static void agwEnsureEngine(void)
{
    if (g_engineStarted)
        return;

    g_engineStarted = 1;
    g_engineThread = CreateThread(NULL, 0, agwEngineThread, NULL, 0, NULL);
}

/* ------------------------------------------------------------------------- */
/* window procedure                                                           */
/* ------------------------------------------------------------------------- */

/* User-input events print by default: together with the page lifecycle log
 * they answer "what did the user do, and what did the page do about it" from
 * one stderr capture. AGB_UI_LOG=0 silences them; AGB_UI_DEBUG=1 adds
 * mousemove chatter (otherwise too noisy to be useful). */
static int agwUiLogVerbose(void)
{
    static int verbose = -1;

    if (verbose < 0)
        verbose = getenv("AGB_UI_DEBUG") ? 1 : 0;

    return verbose;
}

/* Modifier bits the engine expects (see PlatformKeyboardEvent): alt=1,
 * ctrl=2, shift=4. Passing 0 for every key made event.shiftKey/ctrlKey false
 * in the page and broke Shift+arrow selection and Ctrl+shortcuts. */
static unsigned agwKeyboardModifiers(void)
{
    unsigned modifiers = 0;

    if (GetKeyState(VK_MENU) & 0x8000)
        modifiers |= 1;
    if (GetKeyState(VK_CONTROL) & 0x8000)
        modifiers |= 2;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        modifiers |= 4;

    return modifiers;
}

static const char* agwButtonName(int button)
{
    switch (button) {
    case MOUSE_BTN_LEFT:   return "left";
    case MOUSE_BTN_MIDDLE: return "middle";
    case MOUSE_BTN_RIGHT:  return "right";
    default:               return "none";
    }
}

static void agwPaint(AgaveWebView* item, HDC hdc)
{
    if (!item)
        return;

    agwLock();
    if (item->mirrorDc && item->mirrorBits) {
        BitBlt(hdc, 0, 0, item->width, item->height, item->mirrorDc, 0, 0, SRCCOPY);
    } else {
        RECT fill;

        SetRect(&fill, 0, 0, item->width, item->height);
        FillRect(hdc, &fill, (HBRUSH)GetStockObject(WHITE_BRUSH));
    }
    agwUnlock();
}

int agave_webview_paint_to(AgaveWebView* item, void* hdc, int width, int height)
{
    if (!item || !hdc)
        return 0;

    agwLock();
    if (item->mirrorDc && item->mirrorBits && item->width > 0 && item->height > 0) {
        int w = width > 0 ? width : item->width;
        int h = height > 0 ? height : item->height;

        if (w != item->width || h != item->height) {
            SetStretchBltMode((HDC)hdc, HALFTONE);
            SetBrushOrgEx((HDC)hdc, 0, 0, NULL);
            StretchBlt((HDC)hdc, 0, 0, w, h, item->mirrorDc,
                       0, 0, item->width, item->height, SRCCOPY);
        } else {
            BitBlt((HDC)hdc, 0, 0, w, h, item->mirrorDc, 0, 0, SRCCOPY);
        }
    } else {
        RECT fill;

        SetRect(&fill, 0, 0, width, height);
        FillRect((HDC)hdc, &fill, (HBRUSH)GetStockObject(WHITE_BRUSH));
    }
    agwUnlock();
    return 1;
}

/* Encodes one UTF-16 code unit as UTF-8.
 *
 * A character outside the BMP arrives as a surrogate pair, i.e. as two separate
 * WM_CHAR/WM_IME_CHAR messages; the high half is parked on the control until
 * its low half shows up. Returns the byte count, 0 while waiting for the low
 * half (or for an orphaned one). */
static int agwUtf16ToUtf8(AgaveWebView* item, unsigned int unit, char* utf8)
{
    unsigned int code = unit;

    if (unit >= 0xD800 && unit <= 0xDBFF) {
        item->pendingHighSurrogate = unit;
        return 0;
    }

    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (!item->pendingHighSurrogate)
            return 0;                   /* orphaned low half: drop it */
        code = 0x10000 + ((item->pendingHighSurrogate - 0xD800) << 10) + (unit - 0xDC00);
        item->pendingHighSurrogate = 0;
    } else {
        item->pendingHighSurrogate = 0;
    }

    if (code < 0x80) {
        utf8[0] = (char)code;
        utf8[1] = 0;
        return 1;
    }
    if (code < 0x800) {
        utf8[0] = (char)(0xC0 | (code >> 6));
        utf8[1] = (char)(0x80 | (code & 0x3F));
        utf8[2] = 0;
        return 2;
    }
    if (code < 0x10000) {
        utf8[0] = (char)(0xE0 | (code >> 12));
        utf8[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (code & 0x3F));
        utf8[3] = 0;
        return 3;
    }

    utf8[0] = (char)(0xF0 | (code >> 18));
    utf8[1] = (char)(0x80 | ((code >> 12) & 0x3F));
    utf8[2] = (char)(0x80 | ((code >> 6) & 0x3F));
    utf8[3] = (char)(0x80 | (code & 0x3F));
    utf8[4] = 0;
    return 4;
}

static LRESULT CALLBACK agwProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    AgaveWebView* item = (AgaveWebView*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);


    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        agwPaint(item, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case AGW_MSG_REPAINT:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case AGW_MSG_STATE:
        return 0;

    case AGW_MSG_RETIRE:
        /* agave_webview_destroy() was called from a thread other than the one
         * that created this window (popups live on the engine thread). Destroy
         * it here, where DestroyWindow() is allowed. The user data is already
         * cleared, so a later message for this window finds no control. */
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        return 0;

    case AGW_MSG_DOWNLOAD:
        agwDeliverDownloads(item);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case AGW_MSG_UNRESPONSIVE: {
        /* UI thread: tell the host. A modal notice belongs to the thread that
         * owns the window, so the engine thread only posts the payload. */
        AgwHangPayload* payload = (AgwHangPayload*)(INT_PTR)lParam;

        if (item && payload && item->callbacks.on_unresponsive) {
            item->callbacks.on_unresponsive(item, agwHangReason(payload->kind),
                                            payload->busyMs, payload->stackPath,
                                            item->user);
        }

        free(payload);
        return 0;
    }

    case AGW_MSG_CONTEXT: {
        /* UI thread: the host shows its menu here. TrackPopupMenu and the
         * clipboard belong to the thread that owns the window. */
        AgwContextPayload* payload = (AgwContextPayload*)(INT_PTR)lParam;

        if (item && payload && item->callbacks.on_context_menu)
            item->callbacks.on_context_menu(item, payload->x, payload->y,
                                            payload->link ? payload->link : "",
                                            payload->image ? payload->image : "",
                                            payload->text ? payload->text : "",
                                            item->user);

        if (payload) {
            free(payload->link);
            free(payload->image);
            free(payload->text);
            free(payload);
        }
        return 0;
    }

    case WM_SIZE:
        agbLog(AGB_LOG_UI, "resize %dx%d",
               (int)LOWORD(lParam), (int)HIWORD(lParam));
        if (item) {
            item->wantWidth = LOWORD(lParam);
            item->wantHeight = HIWORD(lParam);
            agwPost(AGW_CMD_RESIZE, item, NULL, 0, 0, item->wantWidth, item->wantHeight);
        }
        return 0;

    case WM_SETFOCUS:
        agbLog(AGB_LOG_UI, "focus");
        if (item)
            agwPost(AGW_CMD_FOCUS, item, NULL, 0, 0, 0, 0);
        return 0;

    case WM_KILLFOCUS:
        agbLog(AGB_LOG_UI, "blur");
        return 0;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        int type = EVT_MOUSE_MOVE;
        int button = MOUSE_BTN_NONE;
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);

        if (!item)
            return 0;

        if (message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN)
            type = EVT_MOUSE_DOWN;
        else if (message == WM_LBUTTONUP || message == WM_MBUTTONUP || message == WM_RBUTTONUP)
            type = EVT_MOUSE_UP;

        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)
            button = MOUSE_BTN_LEFT;
        else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)
            button = MOUSE_BTN_MIDDLE;
        else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
            button = MOUSE_BTN_RIGHT;

        if (message == WM_LBUTTONDOWN)
            SetFocus(hwnd);

        if (type != EVT_MOUSE_MOVE || agwUiLogVerbose())
            agbLog(AGB_LOG_UI, "mouse %s %s at %d,%d",
                   type == EVT_MOUSE_DOWN ? "down" :
                   type == EVT_MOUSE_UP ? "up" : "move",
                   agwButtonName(button), x, y);

        agwPost(AGW_CMD_MOUSE, item, NULL, x, y, type, button);

        /* The engine only builds a context menu through its own event entry. */
        if (message == WM_RBUTTONUP)
            agwPost(AGW_CMD_CONTEXT, item, NULL, x, y, 0, 0);

        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT point;
        int delta = (short)HIWORD(wParam);

        if (!item)
            return 0;

        point.x = (short)LOWORD(lParam);
        point.y = (short)HIWORD(lParam);
        ScreenToClient(hwnd, &point);

        agbLog(AGB_LOG_UI, "wheel %s at %d,%d",
               delta > 0 ? "up" : "down", point.x, point.y);

        agwPost(AGW_CMD_MOUSE, item, NULL, point.x, point.y, EVT_MOUSE_WHEEL,
                delta > 0 ? MOUSE_WHEEL_UP : MOUSE_WHEEL_DOWN);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP:
        agbLog(AGB_LOG_UI, "key %s vk=0x%02x",
               message == WM_KEYDOWN ? "down" : "up", (unsigned)wParam);
        if (item)
            agwPost(AGW_CMD_KEY, item, NULL, (int)wParam, (int)agwKeyboardModifiers(),
                    (message == WM_KEYDOWN) ? EVT_KEY_DOWN : EVT_KEY_UP, 0);
        return 0;

    case WM_CHAR:
    case WM_IME_CHAR: {
        char utf8[8];
        int length;

        if (!item)
            return 0;

        /* WM_CHAR and WM_IME_CHAR both carry one UTF-16 code unit. The control
         * window is Unicode (RegisterClassW), so no ANSI conversion happens on
         * the way: an ANSI window turned every IME character into code-page
         * bytes (one WM_CHAR per DBCS byte), which is why Chinese input used to
         * come out as "ÖÐÎÄ". */
        length = agwUtf16ToUtf8(item, (unsigned int)wParam, utf8);
        if (!length)
            return 0;

        agbLog(AGB_LOG_UI, "char '%s'", utf8);

        agwPost(AGW_CMD_CHAR, item, utf8, 0, 0, 0, 0);
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */
/* ------------------------------------------------------------------------- */

static void agwRegisterClass(HINSTANCE instance)
{
    WNDCLASSW wc;

    if (g_classRegistered)
        return;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = agwProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    /* Unicode window: WM_CHAR / WM_IME_CHAR then carry a UTF-16 code unit
     * instead of an ANSI code-page byte (see agwUtf16ToUtf8). */
    wc.lpszClassName = L"AgaveWebViewControl";

    RegisterClassW(&wc);
    g_classRegistered = 1;
}

static char* agwOnUserAgent(const char* url)
{
    static char buffer[256];

    (void)url;

    if (!g_userAgent[0])
        return NULL;

    strncpy(buffer, g_userAgent, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = 0;
    return buffer;
}

/* Creates the popup control (no navigation).
 *
 * The control is a child of the same host window as `from` and is sized like
 * it; the host decides what to do with it (a tab, a window, ...). */
static AgaveWebView* agwCreatePopupControl(AgaveWebView* from)
{
    HWND parent;
    RECT rect;
    AgaveWebView* created;

    if (!from || !from->hwnd)
        return NULL;

    parent = GetParent(from->hwnd);

    if (!parent)
        return NULL;

    GetWindowRect(from->hwnd, &rect);
    MapWindowPoints(HWND_DESKTOP, parent, (POINT*)&rect, 2);

    created = agave_webview_create(parent,
                                   rect.left, rect.top,
                                   rect.right - rect.left,
                                   rect.bottom - rect.top,
                                   &from->callbacks, from->user);

    return created;
}

/* The engine asks for a new window (a link with target=_blank).
 *
 * It needs a live _mc_view* back: the engine loads the URL into the frame it
 * gets (FrameLoaderClientMC::dispatchCreatePage asks for "about:blank" and
 * then loads the real URL itself). Creating the engine view is queued work,
 * so without doing it here the callback returned a NULL view, the engine
 * gave up on the new window and the tab stayed blank.
 *
 * We are on the engine thread here, so creating the view right away is the
 * same work the queued resize command would do, only in time. */
static MaCrossView* agwOnOpenWindow(const char* url, unsigned int flags, const MC_RECT* rect)
{
    AgaveWebView* origin = NULL;
    AgaveWebView* created = NULL;
    int i;

    (void)flags;
    (void)rect;

    /* macross does not tell us which view opened the window; use the first live
     * control as the reference for the host window. */
    for (i = 0; i < g_viewCount; ++i) {
        if (g_views[i] && g_views[i]->hwnd && !g_views[i]->destroyed) {
            origin = g_views[i];
            break;
        }
    }

    if (!origin)
        return NULL;

    created = agwCreatePopupControl(origin);

    if (!created)
        return NULL;

    /* Create the engine view now - the caller loads the URL into it. */
    agwEnsureSurface(created, created->wantWidth, created->wantHeight);

    if (!created->view) {
        agave_webview_destroy(created);
        return NULL;
    }

    /* Host side navigation: only when the engine does not load the URL
     * itself. The engine's new-window path always asks for "about:blank" and
     * loads the real URL into the frame it gets back; navigating the popup to
     * about:blank as well would cancel that load. The context menu path
     * (ChromeClient::createWindow) passes the real URL and nobody loads it
     * for us, so we do it here. */
    if (url && url[0] && strcmp(url, "about:blank") != 0)
        agave_webview_navigate(created, url);

    if (origin->callbacks.on_new_window) {
        if (!origin->callbacks.on_new_window(origin, created, url ? url : "", origin->user)) {
            agbLog(AGB_LOG_NAV, "popup blocked by host");
            agave_webview_destroy(created);
            return NULL;
        }
    }

    return created->view;
}

/* window.open(): create a sibling control and let the host decide. */

int agave_control_open_window(const char* url)
{
    AgaveWebView* origin = NULL;
    AgaveWebView* created = NULL;
    int i;

    for (i = 0; i < g_viewCount; ++i) {
        if (g_views[i] && g_views[i]->hwnd && !g_views[i]->destroyed) {
            origin = g_views[i];
            break;
        }
    }

    if (!origin)
        return 0;

    created = agave_webview_create_popup(origin, url && url[0] ? url : "about:blank");

    if (!created)
        return 0;

    if (origin->callbacks.on_new_window) {
        if (!origin->callbacks.on_new_window(origin, created, url ? url : "", origin->user)) {
            agbLog(AGB_LOG_NAV, "popup blocked by host");
            agave_webview_destroy(created);
            return 0;
        }
    }

    return 1;
}

/* ------------------------------------------------------------------------- */
/* clipboard (engine thread)                                                 */
/* ------------------------------------------------------------------------- */
/* The engine hands over UTF-8, Windows wants UTF-16. Without these callbacks
 * the engine's copy/paste (Editor -> Pasteboard) had nowhere to go, so
 * ctrl+c / ctrl+v in a page did nothing. */

static void agwOnClipboardSetData(void* data, unsigned int len)
{
    const char* text = (const char*)data;
    int chars;
    HGLOBAL memory;
    wchar_t* target;

    if (!text || !len)
        return;

    chars = MultiByteToWideChar(CP_UTF8, 0, text, (int)len, NULL, 0);
    if (chars <= 0)
        return;

    memory = GlobalAlloc(GMEM_MOVEABLE, (chars + 1) * sizeof(wchar_t));
    if (!memory)
        return;

    target = (wchar_t*)GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        return;
    }

    MultiByteToWideChar(CP_UTF8, 0, text, (int)len, target, chars);
    target[chars] = 0;
    GlobalUnlock(memory);

    if (!OpenClipboard(NULL)) {
        GlobalFree(memory);
        return;
    }

    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory))
        GlobalFree(memory);
    CloseClipboard();
}

static unsigned int agwOnClipboardDataLen(void)
{
    HANDLE handle;
    wchar_t* text;
    int bytes = 0;

    if (!OpenClipboard(NULL))
        return 0;

    handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        text = (wchar_t*)GlobalLock(handle);
        if (text) {
            bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
            if (bytes > 0)
                --bytes;   /* the terminating NUL is not part of the text */
            GlobalUnlock(handle);
        }
    }

    CloseClipboard();
    return bytes > 0 ? (unsigned int)bytes : 0;
}

static unsigned int agwOnClipboardGetData(void* buffer, unsigned int len)
{
    HANDLE handle;
    wchar_t* text;
    int bytes = 0;

    if (!buffer || !len)
        return 0;

    if (!OpenClipboard(NULL))
        return 0;

    handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        text = (wchar_t*)GlobalLock(handle);
        if (text) {
            int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, (char*)buffer, (int)len, NULL, NULL);

            bytes = size > 0 ? (unsigned int)(size - 1) : 0;
            GlobalUnlock(handle);
        }
    }

    CloseClipboard();
    return bytes;
}

/* window.close(): the engine asked for its view to go away. Runs on the
 * engine thread; the host decides what "close" means (a tab, a window). */
static void agwOnCloseWindow(MaCrossView* view)
{
    AgaveWebView* item = agwByView(view);

    if (item && item->callbacks.on_close_window)
        item->callbacks.on_close_window(item, item->user);
}

static void agwInstallCallbacks(void)
{
    static int installed = 0;
    MC_CALLBACK_INFO info;

    if (installed)
        return;
    installed = 1;

    memset(&info, 0, sizeof(info));
    info.cb_invalidate_rect  = agwOnInvalidate;
    info.cb_update_view_now  = agwOnUpdateNow;
    info.cb_set_title        = agwOnTitle;
    info.cb_set_location     = agwOnLocation;
    info.cb_set_status       = agwOnStatus;
    info.cb_loading_progress = agwOnProgress;
    info.cb_history_status   = agwOnHistoryStatus;
    info.cb_alert_box        = agwOnAlertBox;
    info.cb_confirm_box      = agwOnConfirmBox;
    info.cb_prompt_box       = agwOnPromptBox;
    info.cb_context_menu     = agwOnContextMenu;
    info.cb_error            = agwOnError;
    info.cb_set_cursor       = agwOnCursor;
    info.cb_download         = agwOnDownload;
    info.cb_user_agent_custom = agwOnUserAgent;
    info.cb_open_window      = agwOnOpenWindow;
    info.cb_close_window     = agwOnCloseWindow;
    info.cb_clipboard_set_data = agwOnClipboardSetData;
    info.cb_clipboard_data_len = agwOnClipboardDataLen;
    info.cb_clipboard_get_data = agwOnClipboardGetData;

    macross_set_callback(&info);
}

AgaveWebView* agave_webview_create(void* parent, int x, int y, int width, int height,
                                   const AgaveWebViewCallback* callbacks, void* user)
{
    AgaveWebView* item;

    if (!parent)
        return NULL;

    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = 1;
        agwWatchdogStart();
    }

    agwRegisterClass((HINSTANCE)GetWindowLongPtrW((HWND)parent, GWLP_HINSTANCE));

    macross_set_open_window_hook(agave_control_open_window);
    macross_set_post_message_hook(agwOnPostMessage);
    macross_set_dialog_hook(agwOnDialog);
    agwInstallCallbacks();
    agwEnsureEngine();

    item = (AgaveWebView*)calloc(1, sizeof(AgaveWebView));

    if (!item)
        return NULL;

    item->reapEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    item->zoom = 100;
    item->wantWidth = width > 0 ? width : 1;
    item->wantHeight = height > 0 ? height : 1;
    item->user = user;
    /* A fresh view is visible until the host says otherwise (a new tab starts
     * active; the probe never calls set_visible). */
    item->visible = 1;

    /* Which backend runs the page (AGB_VIEW_BACKEND, default local). A name
     * that is not built yet logs and falls back rather than silently running
     * something else. */
    {
        const AgwViewBackend* backend = agwBackendByName(getenv("AGB_VIEW_BACKEND"));

        item->backend = backend ? backend : agwBackendLocal();
    }

    if (callbacks)
        item->callbacks = *callbacks;

    item->hwnd = CreateWindowExW(0, L"AgaveWebViewControl", L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                 x, y, width, height,
                                 (HWND)parent, NULL,
                                 (HINSTANCE)GetWindowLongPtrW((HWND)parent, GWLP_HINSTANCE),
                                 NULL);

    if (!item->hwnd) {
        free(item);
        return NULL;
    }

    SetWindowLongPtrW(item->hwnd, GWLP_USERDATA, (LONG_PTR)item);
    item->hwndThread = GetCurrentThreadId();

    agwLock();
    if (g_viewCount < AGW_MAX_VIEWS) {
        item->id = g_nextViewId++;
        g_views[g_viewCount++] = item;
    }
    agwUnlock();

    /* The engine thread creates the surface on first resize. */
    agwPost(AGW_CMD_RESIZE, item, NULL, 0, 0, width, height);

    return item;
}

void agave_webview_destroy(AgaveWebView* item)
{
    HANDLE reap = NULL;
    int id;
    int index;

    if (!item)
        return;

    id = item->id;

    /* Everything WebCore / Ant owns is torn down on the engine thread, between
     * frames. The host may call this while the engine thread is inside
     * macross_event_dispatch() firing this page's timers, or inside a command
     * for another view: tearing the engine view down here was R141 (a font
     * load callback landed on a document the host had half destroyed). The
     * window itself still belongs to the thread that created it. */
    agwLock();
    if (item->destroyed) {
        agwUnlock();
        return;                     /* destroy() twice is a no-op */
    }
    item->destroyed = 1;

    index = agwIndexOf(item);
    if (index >= 0)
        g_views[index] = g_views[--g_viewCount];

    if (item->reapEvent && GetCurrentThreadId() != g_engineThreadId) {
        /* Wait for the engine thread to finish the teardown, so the caller can
         * release everything it owns (the probe exits right after this call;
         * the shell frees the Tab slot). A duplicate keeps the handle valid if
         * the reaper closes the item's own handle first. */
        DuplicateHandle(GetCurrentProcess(), item->reapEvent,
                        GetCurrentProcess(), &reap, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }

    item->reapNext = g_reapHead;
    g_reapHead = item;
    agwUnlock();

    agwRetireWindow(item);

    if (reap) {
        /* A page whose unload handler never returns must not wedge the host:
         * the reaper finishes the teardown when the script is aborted. */
        if (WaitForSingleObject(reap, 2000) == WAIT_TIMEOUT)
            agbLog(AGB_LOG_SHELL, "view destroy: engine did not reap id=%d in 2s", id);

        CloseHandle(reap);
    }
}

AgaveWebView* agave_webview_create_popup(AgaveWebView* from, const char* url)
{
    AgaveWebView* created;

    agbLog(AGB_LOG_NAV, "popup create url=%s", (url && url[0]) ? url : "about:blank");

    created = agwCreatePopupControl(from);

    if (!created)
        return NULL;

    if (url && url[0])
        agave_webview_navigate(created, url);

    return created;
}

/* Points the control's callbacks at a new owner. A popup inherits the
 * callbacks and the user pointer of the view it was opened from, but it ends
 * up in a different tab - the host uses this to re-attach it. */
void agave_webview_set_user(AgaveWebView* item, void* user)
{
    if (!item)
        return;

    agwLock();
    item->user = user;
    agwUnlock();
}

void agave_webview_set_rect(AgaveWebView* item, int x, int y, int width, int height)
{
    if (!item || !item->hwnd)
        return;

    MoveWindow(item->hwnd, x, y, width > 0 ? width : 1, height > 0 ? height : 1, TRUE);
}

void* agave_webview_hwnd(AgaveWebView* item)
{
    return item ? item->hwnd : NULL;
}

void agave_webview_navigate(AgaveWebView* item, const char* url)
{
    if (!item || !url)
        return;

    agwLock();
    strncpy(item->url, url, sizeof(item->url) - 1);
    item->url[sizeof(item->url) - 1] = 0;
    item->navigatePending = 1;
    agwUnlock();

    agwPost(AGW_CMD_NAVIGATE, item, url, 0, 0, 0, 0);
}

void agave_webview_load_html(AgaveWebView* item, const char* html, const char* baseUrl)
{
    if (!item || !html)
        return;

    (void)baseUrl;
    agwPost(AGW_CMD_LOAD_HTML, item, html, 0, 0, 0, 0);
}

void agave_webview_back(AgaveWebView* item)    { agwPost(AGW_CMD_BACK, item, NULL, 0, 0, 0, 0); }
void agave_webview_forward(AgaveWebView* item) { agwPost(AGW_CMD_FORWARD, item, NULL, 0, 0, 0, 0); }
/* Abort whatever script the engine is running (Reload/Stop on a page whose JS
 * never returns). Runs on the caller's thread: the bridge only sets an atomic
 * flag the VM polls, so this does not need the command queue - which is exactly
 * the queue a stuck script never drains. */
void agave_webview_interrupt(AgaveWebView* item)
{
    /* Per-view through the backend: local can only abort every engine (one
     * engine thread runs them all); a backend with its own instance aborts
     * exactly its own (docs/design-render-backend.md). */
    if (item && item->backend && item->backendView) {
        item->backend->interrupt(item->backendView);
        return;
    }

    antb_interrupt_all_engines();
}

/* 1 when a queued command has been running for at least minMs (0 = any). */
/* Process-wide on purpose: one engine thread runs every view, so a script stuck
 * in tab A also freezes a command posted by tab B. */
int agave_webview_busy(AgaveWebView* item, int minMs)
{
    (void)item;
    return agwQueueStuckFor(minMs < 300 ? 300 : minMs);
}

void agave_webview_reload(AgaveWebView* item)  { agwPost(AGW_CMD_RELOAD, item, NULL, 0, 0, 0, 0); }
void agave_webview_stop(AgaveWebView* item)    { agwPost(AGW_CMD_STOP, item, NULL, 0, 0, 0, 0); }

void agave_webview_eval(AgaveWebView* item, const char* script)
{
    if (item && script)
        agwPost(AGW_CMD_EVAL, item, script, 0, 0, 0, 0);
}

void agave_webview_set_zoom(AgaveWebView* item, int percent)
{
    if (!item)
        return;

    agwPost(AGW_CMD_ZOOM, item, NULL, 0, 0, percent, 0);
}

int agave_webview_zoom(AgaveWebView* item)
{
    return item ? item->zoom : 100;
}

void agave_webview_set_visible(AgaveWebView* item, int visible)
{
    if (!item)
        return;

    agwPost(AGW_CMD_VISIBLE, item, NULL, 0, 0, visible ? 1 : 0, 0);
}

int agave_webview_search(AgaveWebView* item, const char* text, int forward, int case_sensitive)
{
    if (!item || !text || !text[0])
        return 0;

    agwPost(AGW_CMD_SEARCH, item, text, 0, 0, forward ? 1 : 0, case_sensitive ? 1 : 0);
    return 1;
}

int agave_webview_can_back(AgaveWebView* item)
{
    return item ? item->backCount > 0 : 0;
}

int agave_webview_can_forward(AgaveWebView* item)
{
    return item ? item->forwardCount > 0 : 0;
}

const char* agave_webview_url(AgaveWebView* item)
{
    return item ? item->url : "";
}

const char* agave_webview_title(AgaveWebView* item)
{
    return item ? item->title : "";
}

void agave_webview_set_focus(AgaveWebView* item)
{
    if (item && item->hwnd)
        SetFocus(item->hwnd);
}

/* Host -> page. The text is passed as a JS string literal; quotes, backslashes
 * and newlines are escaped, everything else is passed through as UTF-8. */
void agave_webview_post_message(AgaveWebView* item, const char* message)
{
    char script[2048];
    char escaped[1400];
    size_t i = 0;
    size_t out = 0;

    if (!item || !message)
        return;

    while (message[i] && out + 6 < sizeof(escaped)) {
        unsigned char c = (unsigned char)message[i++];

        if (c == '\\' || c == '\'') {
            escaped[out++] = '\\';
            escaped[out++] = (char)c;
        } else if (c == '\n') {
            escaped[out++] = '\\';
            escaped[out++] = 'n';
        } else if (c == '\r') {
            escaped[out++] = '\\';
            escaped[out++] = 'r';
        } else {
            escaped[out++] = (char)c;
        }
    }
    escaped[out] = 0;

    _snprintf(script, sizeof(script),
              "try { if (typeof window.onAgaveMessage === 'function') "
              "window.onAgaveMessage('%s'); } catch (e) {}",
              escaped);
    script[sizeof(script) - 1] = 0;

    agave_webview_eval(item, script);
}

void agave_webview_set_user_agent(AgaveWebView* item, const char* userAgent)
{
    if (item && userAgent)
        agwPost(AGW_CMD_UA, item, userAgent, 0, 0, 0, 0);
}

void agave_webview_set_download_dir(AgaveWebView* item, const char* directory)
{
    if (!item || !directory)
        return;

    strncpy(item->downloadDir, directory, sizeof(item->downloadDir) - 1);
    item->downloadDir[sizeof(item->downloadDir) - 1] = 0;

    /* The engine owns the job, so the directory is set there, on its thread. */
    agwPost(AGW_CMD_DL_DIR, item, directory, 0, 0, 0, 0);
}

void agave_webview_cancel_download(AgaveWebView* item, int id)
{
    if (!item || id <= 0)
        return;

    agwPost(AGW_CMD_DL_CANCEL, item, NULL, id, 0, 0, 0);
}

void agave_webview_set_log(const char* path)
{
    if (path && path[0])
        SetEnvironmentVariableA("AGB_NET_LOG", path);
}
