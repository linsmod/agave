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
#include "../webview/AgbLog.h"

/* The control window class is registered as a Unicode class: see agwRegisterClass. */
#define AGW_MAX_VIEWS  32
#define AGW_QUEUE_SIZE 256
#define AGW_MSG_STATE  (WM_APP + 0x51)
#define AGW_MSG_REPAINT (WM_APP + 0x52)

enum {
    AGW_CMD_NAVIGATE, AGW_CMD_LOAD_HTML, AGW_CMD_BACK, AGW_CMD_FORWARD,
    AGW_CMD_RELOAD, AGW_CMD_STOP, AGW_CMD_EVAL, AGW_CMD_ZOOM,
    AGW_CMD_RESIZE, AGW_CMD_UA, AGW_CMD_DL_DIR, AGW_CMD_MOUSE, AGW_CMD_KEY,
    AGW_CMD_CHAR, AGW_CMD_FOCUS, AGW_CMD_SEARCH, AGW_CMD_CONTEXT
};

typedef struct {
    int  type;
    int  index;
    int  x, y, a, b;
    char text[2048];
} AgwCommand;

struct AgaveWebView {
    HWND               hwnd;
    MaCrossView*       view;
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
    volatile int       destroyed;
    int                refs;

    char               url[2048];
    char               title[512];
    char               status[512];
    int                progress;
    int                loading;
    int                lastRecordedProgress;
    int                backCount;
    int                forwardCount;
    unsigned long      lastMirrorMs;   /* last InvalidateRect mirrored to the window */
    int                navigatePending; /* a CMD_NAVIGATE is queued for this view */

    char               downloadDir[MAX_PATH];
    char               userAgent[256];

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
static volatile int     g_engineQuit;
static HANDLE           g_engineThread;
static volatile int     g_engineStarted;
static int              g_classRegistered;

/* ------------------------------------------------------------------------- */
/* small helpers                                                              */
/* ------------------------------------------------------------------------- */

static void agwLock(void)   { if (g_lockReady) EnterCriticalSection(&g_lock); }
static void agwUnlock(void) { if (g_lockReady) LeaveCriticalSection(&g_lock); }

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


/* Frees the control once the engine thread is done with it. destroy() may be
 * called while the engine is inside macross_view_update on this item. */
static void agwDestroySurface(AgaveWebView* item);

static void agwFreeItem(AgaveWebView* item)
{
    agwDestroySurface(item);

    if (item->hwnd) {
        SetWindowLongPtrW(item->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(item->hwnd);
        item->hwnd = NULL;
    }

    free(item);
}

static void agwRelease(AgaveWebView* item)
{
    int shouldFree = 0;

    if (!item)
        return;

    agwLock();
    if (--item->refs <= 0 && item->destroyed) {
        /* Last reference dropped and the host already asked for the control
         * to go away: the engine view can be destroyed here. This runs on the
         * engine thread, which is the only place it may be freed - the host
         * thread can be inside macross_view_update() on it. */
        if (item->view) {
            macross_view_destroy(item->view);
            item->view = NULL;
        }
        agwDestroySurface(item);
        shouldFree = 1;
    }
    agwUnlock();

    if (shouldFree)
        agwFreeItem(item);
}

static void agwPost(int type, AgaveWebView* view, const char* text,
                    int x, int y, int a, int b)
{
    AgwCommand command;

    if (!g_lockReady)
        return;

    memset(&command, 0, sizeof(command));
    command.type = type;
    command.x = x;
    command.y = y;
    command.a = a;
    command.b = b;

    if (text) {
        strncpy(command.text, text, sizeof(command.text) - 1);
    }

    agwLock();
    command.index = agwIndexOf(view);
    if (command.index >= 0 &&
        ((g_queueTail + 1) % AGW_QUEUE_SIZE) != g_queueHead) {
        g_queue[g_queueTail] = command;
        g_queueTail = (g_queueTail + 1) % AGW_QUEUE_SIZE;
    }
    agwUnlock();

    if (command.index >= 0 && view->hwnd)
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

static void agwOnContextMenu(MaCrossView* view, const MC_CONTEXT_MENU_DATA* menu)
{
    AgaveWebView* item = agwByView(view);

    if (getenv("AGW_CTX_DEBUG"))
        fprintf(stderr, "[ctx] fired view=%p item=%p\n", (void*)view, (void*)item);

    if (!item)
        item = g_lastActive;

    if (!item || !item->callbacks.on_context_menu)
        return;

    item->callbacks.on_context_menu(item,
                                    menu ? menu->location.x : 0,
                                    menu ? menu->location.y : 0,
                                    menu && menu->link_url ? menu->link_url : "",
                                    menu && menu->image_url ? menu->image_url : "",
                                    menu && menu->text ? menu->text : "",
                                    item->user);
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

        if (!item || !item->view)
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


static MC_BOOL agwOnWriteFile(int id, const char* url, const void* data,
                              unsigned long dataLen, unsigned long totalLen)
{
    static FILE* file = NULL;
    static int   currentId = -1;
    (void)totalLen;

    if (currentId != id) {
        char name[512];
        char path[MAX_PATH];
        const char* slash = url ? strrchr(url, '/') : NULL;
        size_t i, n = 0;

        if (file) {
            fclose(file);
            file = NULL;
        }

        currentId = id;
        name[0] = 0;

        if (slash && slash[1]) {
            for (i = 0; slash[1 + i] && i < sizeof(name) - 1; ++i) {
                char c = slash[1 + i];

                if (c == '?' || c == '#' || c == '\\' || c == ':' ||
                    c == '*' || c == '"' || c == '<' || c == '>' || c == '|')
                    break;
                name[n++] = c;
            }
        }
        name[n] = 0;

        if (!name[0])
            strcpy(name, "download.bin");

        {
            const char* home = getenv("USERPROFILE");
            const char* dir = home ? home : ".";

            _snprintf(path, sizeof(path), "%s\\Downloads\\%s", dir, name);
        }
        path[sizeof(path) - 1] = 0;

        file = fopen(path, "wb");

        if (file) {
            int i2;

            agbLog(AGB_LOG_NAV, "download url=%s path=%s", url ? url : "", path);

            for (i2 = 0; i2 < g_viewCount; ++i2) {
                AgaveWebView* item = g_views[i2];

                if (item && item->callbacks.on_download)
                    item->callbacks.on_download(item, url, path, item->user);
            }
        }
    }

    if (file && data && dataLen)
        fwrite(data, 1, dataLen, file);

    return file ? TRUE : FALSE;
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
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    if (item->bits && item->width == width && item->height == height)
        return;

    agwDestroySurface(item);

    if (!agwCreateSurface(item, width, height))
        return;

    if (item->view) {
        macross_view_resize(item->view, item->bits, width, height, width * 4);
    } else {
        item->view = macross_view_create(item->bits, width, height, width * 4, item);

        if (item->view) {
            if (item->zoom != 100)
                macross_view_set_scale_factor(item->view, item->zoom);

            /* A queued CMD_NAVIGATE will load the URL; doing it here as well made
             * every first navigation fetch the page twice. */
            if (item->url[0] && !item->navigatePending)
                macross_view_open_url(item->view, item->url);
        }
    }

    if (item->hwnd)
        InvalidateRect(item->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------------- */
/* engine thread                                                              */
/* ------------------------------------------------------------------------- */

static void agwDrainCommands(void)
{
    for (;;) {
        AgwCommand command;
        int have = 0;
        AgaveWebView* item;

        agwLock();
        if (g_queueHead != g_queueTail) {
            command = g_queue[g_queueHead];
            g_queueHead = (g_queueHead + 1) % AGW_QUEUE_SIZE;
            have = 1;
        }
        agwUnlock();

        if (!have)
            break;

        if (command.index < 0 || command.index >= g_viewCount)
            continue;

        item = g_views[command.index];

        if (!item || item->destroyed)
            continue;

        switch (command.type) {
        case AGW_CMD_NAVIGATE:
            item->navigatePending = 0;

            if (item->view) {
                macross_view_open_url(item->view, command.text);
            } else {
                strncpy(item->url, command.text, sizeof(item->url) - 1);
                item->url[sizeof(item->url) - 1] = 0;
                agwEnsureSurface(item, item->wantWidth, item->wantHeight);
            }
            break;

        case AGW_CMD_LOAD_HTML:
            if (item->view)
                macross_view_load_html(item->view, command.text,
                                       command.text[0] ? NULL : NULL);
            break;

        case AGW_CMD_BACK:    if (item->view) macross_view_backward(item->view); break;
        case AGW_CMD_FORWARD: if (item->view) macross_view_forward(item->view);  break;
        case AGW_CMD_RELOAD:  if (item->view) macross_view_reload(item->view);   break;
        case AGW_CMD_STOP:    if (item->view) macross_view_stop(item->view);     break;

        case AGW_CMD_EVAL:
            if (item->view)
                macross_view_eval_script(item->view, command.text);
            break;

        case AGW_CMD_ZOOM:
            item->zoom = command.a > 0 ? command.a : 100;
            if (item->view)
                macross_view_set_scale_factor(item->view, item->zoom);
            if (item->hwnd)
                InvalidateRect(item->hwnd, NULL, FALSE);
            break;

        case AGW_CMD_RESIZE:
            item->wantWidth = command.a;
            item->wantHeight = command.b;
            agwEnsureSurface(item, command.a, command.b);
            break;

        case AGW_CMD_UA:
            if (command.text[0]) {
                strncpy(g_userAgent, command.text, sizeof(g_userAgent) - 1);
                g_userAgent[sizeof(g_userAgent) - 1] = 0;
            }
            break;

        case AGW_CMD_MOUSE:
            if (item->view) {
                MC_MOUSE_EVENT event;

                memset(&event, 0, sizeof(event));
                event.type = (MC_MOUSE_EVENT_TYPE)command.a;
                event.button = (MC_MOUSE_BUTTON)command.b;
                event.point.x = command.x;
                event.point.y = command.y;
                macross_mouse_event(item->view, &event);
            }
            break;

        case AGW_CMD_KEY:
            if (item->view) {
                MC_KEY_EVENT event;

                memset(&event, 0, sizeof(event));
                event.type = (MC_KEY_EVENT_TYPE)command.a;
                event.key = (MC_VIRTUAL_KEY)command.x;
                event.modifier = command.b;
                macross_keyboard_event(item->view, &event);
            }
            break;

        case AGW_CMD_CHAR:
            if (item->view)
                macross_view_input_text(item->view, command.text);
            break;

        case AGW_CMD_FOCUS:
            if (item->view)
                macross_view_set_focus(item->view);
            break;

        case AGW_CMD_SEARCH:
            if (item->view && command.text[0])
                macross_view_search_text(item->view, command.text,
                                         command.a ? TRUE : FALSE,
                                         command.b ? TRUE : FALSE, TRUE);
            break;

        case AGW_CMD_CONTEXT:
            if (item->view) {
                MC_CONTEXT_EVENT event;

                memset(&event, 0, sizeof(event));
                event.point.x = command.x;
                event.point.y = command.y;
                event.modifier = (unsigned int)command.a;
                macross_context_menu_event(item->view, &event);
            }
            break;

        default:
            break;
        }
    }
}

static DWORD WINAPI agwEngineThread(LPVOID param)
{
    (void)param;

    if (macross_initialize(PIXEL_FORMAT_BGRA32, 1024, 768) != MC_STATUS_SUCCESS)
        return 0;

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

        macross_event_dispatch();

        /* Snapshot the live views under the lock, then update them *without* it:
         * a slow paint (heavy page) used to block the host's UI thread, which
         * takes the same lock whenever it posts a command. */
        {
            AgaveWebView* batch[AGW_MAX_VIEWS];
            int count = 0;

            agwLock();
            for (i = 0; i < g_viewCount; ++i) {
                AgaveWebView* item = g_views[i];

                if (!item || !item->view || item->destroyed)
                    continue;

                item->refs++;
                batch[count++] = item;
            }
            agwUnlock();

            for (i = 0; i < count; ++i) {
                AgaveWebView* item = batch[i];

                if (!item->destroyed) {
                    unsigned long began = GetTickCount();

                    macross_view_update(item->view, NULL);

                    if (getenv("AGB_LOOP_DEBUG")) {
                        static int frames = 0;
                        static unsigned long total = 0;

                        total += GetTickCount() - began;
                        ++frames;

                        if (frames % 100 == 0)
                            fprintf(stderr, "[loop] frames=%d update=%u ms (avg %u ms)\n",
                                    frames, (unsigned)total, (unsigned)(total / frames));
                    }
                }

                /* Copy the finished frame into the mirror buffer the window
                 * paints from. Doing this under the lock keeps the UI thread
                 * from blitting a half-written frame (that tore while
                 * scrolling); the copy is ~1ms, the engine paints outside it. */
                if (item->mirrorBits && item->bits && item->width > 0 && item->height > 0) {
                    size_t bytes = (size_t)item->width * (size_t)item->height * 4u;

                    agwLock();
                    memcpy(item->mirrorBits, item->bits, bytes);
                    item->lastMirrorMs = GetTickCount();
                    agwUnlock();

                    if (item->hwnd && IsWindowVisible(item->hwnd))
                        InvalidateRect(item->hwnd, NULL, FALSE);
                }

                g_lastActive = item;
                active = 1;

                agwRelease(item);
            }
        }

        Sleep(active ? 6 : 12);
    }

    agwLock();
    for (; g_viewCount > 0; ) {
        AgaveWebView* item = g_views[--g_viewCount];

        if (item && item->view) {
            macross_view_destroy(item->view);
            item->view = NULL;
        }
    }
    agwUnlock();

    macross_shutdown();
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
    info.cb_write_file_data  = agwOnWriteFile;
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

    item->zoom = 100;
    item->wantWidth = width > 0 ? width : 1;
    item->wantHeight = height > 0 ? height : 1;
    item->user = user;

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

    agwLock();
    if (g_viewCount < AGW_MAX_VIEWS)
        g_views[g_viewCount++] = item;
    agwUnlock();

    /* The engine thread creates the surface on first resize. */
    agwPost(AGW_CMD_RESIZE, item, NULL, 0, 0, width, height);

    return item;
}

void agave_webview_destroy(AgaveWebView* item)
{
    int index;
    int shouldFree = 0;

    if (!item)
        return;

    agwLock();
    item->destroyed = 1;

    index = agwIndexOf(item);
    if (index >= 0)
        g_views[index] = g_views[--g_viewCount];

    /* The engine thread may be inside macross_view_update() on this view
     * right now (it holds a reference while it does), and freeing the view
     * under it crashes the engine. With a reference in flight the view is
     * destroyed by the engine thread instead (agwRelease). */
    if (item->refs <= 0) {
        if (item->view) {
            macross_view_destroy(item->view);
            item->view = NULL;
        }
        agwDestroySurface(item);
        shouldFree = 1;
    }
    agwUnlock();

    if (item->hwnd) {
        SetWindowLongPtrW(item->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(item->hwnd);
        item->hwnd = NULL;
    }

    if (shouldFree)
        agwFreeItem(item);
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
}

void agave_webview_set_log(const char* path)
{
    if (path && path[0])
        SetEnvironmentVariableA("AGB_NET_LOG", path);
}
