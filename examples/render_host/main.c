/* agave_render_host - the engine side of the B2 process backend.
 *
 * One instance per view. It maps the control's named shared section
 * (src/webview_control/AgwViewIpc.h), runs a real MaCross view on the canvas in
 * that mapping, executes the protocol commands the control writes into the
 * command ring, and publishes frames + events back through the event ring.
 *
 * Not a control and not a window: it never touches GDI, it only writes BGRA
 * pixels - the same contract examples/view_host has, with the shared section as
 * the buffer and the ring as the host.
 *
 *   agave_render_host.exe --ipc <name> [--crash-after <ms>]
 *
 * Env: AGB_RENDER_LOG=1 traces commands to stderr.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "macross.h"
#include "../../src/webview_control/AgwViewIpc.h"

static AgwIpcHeader* g_hdr;
static HANDLE        g_map;
static HANDLE        g_cmdEvent;
static HANDLE        g_intEvent;
static volatile LONG g_stopWatchdog;
static MaCrossView*  g_view;
static MC_BYTE*      g_frame;
static int           g_visible = 1;
static int           g_log;

/* A spawned renderer has no console (CREATE_NO_WINDOW, no inherited handles):
 * AGB_RENDER_LOG=1 writes next to the exe instead of stderr. */
static void rlog(const char* fmt, ...)
{
    static FILE* logFile;
    va_list ap;

    if (!g_log)
        return;

    if (!logFile) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
        char* slash = n ? strrchr(path, '\\') : NULL;

        if (slash)
            *(slash + 1) = 0;
        snprintf(path + strlen(path), sizeof(path) - strlen(path), "agave_render_host.log");
        logFile = fopen(path, "a");
    }

    if (!logFile)
        return;

    va_start(ap, fmt);
    vfprintf(logFile, fmt, ap);
    va_end(ap);
    fflush(logFile);
}

/* ---- events toward the control ------------------------------------------ */

static void pushEvent(int kind, int a, int b, int c, int d,
                      const char* text, int textLen)
{
    int32_t slot = InterlockedIncrement(&g_hdr->eventWrite) - 1;
    AgwIpcEvent* ev = &g_hdr->events[slot & (AGW_IPC_EVT_SLOTS - 1)];

    memset(ev, 0, sizeof(*ev));
    ev->kind = kind;
    rlog("[render] evt kind=%d textLen=%d\n", kind, textLen);
    ev->a = a;
    ev->b = b;
    ev->c = c;
    ev->d = d;
    ev->timeUs = 0;

    if (text && textLen > 0) {
        int32_t off = InterlockedExchangeAdd(&g_hdr->evtTextWrite, textLen) & (AGW_IPC_TEXT_BYTES - 1);
        int32_t room = AGW_IPC_TEXT_BYTES - off;

        if (textLen > room) {
            off = 0;
            InterlockedExchange(&g_hdr->evtTextWrite, textLen);
        }
        if (textLen > AGW_IPC_TEXT_BYTES / 2)
            textLen = AGW_IPC_TEXT_BYTES / 2;

        memcpy(&g_hdr->evtText[off], text, (size_t)textLen);
        ev->textOffset = off;
        ev->textLen = textLen;
    }
}

static void pushFrameEvent(uint32_t seq, int width, int height)
{
    LONG slot = InterlockedIncrement(&g_hdr->eventWrite) - 1;
    AgwIpcEvent* ev = &g_hdr->events[slot & (AGW_IPC_EVT_SLOTS - 1)];

    memset(ev, 0, sizeof(*ev));
    ev->kind = AGW_VIEW_EVENT_FRAME;
    ev->frameBufferId = 1;
    ev->frameSeq = seq;
    ev->damage[0] = 0;
    ev->damage[1] = 0;
    ev->damage[2] = width;
    ev->damage[3] = height;
}

/* macross callbacks (process-global; this process runs one view) */
static void onTitle(MaCrossView* view, const char* title)
{
    (void)view;
    if (title)
        pushEvent(AGW_VIEW_EVENT_TITLE, 0, 0, 0, 0, title, (int)strlen(title));
}

static void onLocation(MaCrossView* view, const char* url)
{
    (void)view;
    if (url)
        pushEvent(AGW_VIEW_EVENT_URL, 0, 0, 0, 0, url, (int)strlen(url));
}

static void onProgress(MaCrossView* view, unsigned int percent, MC_BOOL finished)
{
    (void)view;
    pushEvent(AGW_VIEW_EVENT_PROGRESS, (int)percent, finished ? 1 : 0, 0, 0, NULL, 0);
}

static void onStatus(MaCrossView* view, const char* text)
{
    (void)view;
    if (text)
        pushEvent(AGW_VIEW_EVENT_STATUS, 0, 0, 0, 0, text, (int)strlen(text));
}

static void onError(int err, const char* failedUrl)
{
    pushEvent(AGW_VIEW_EVENT_EXIT, err, 1 /* error, not a normal exit */, 0, 0,
              failedUrl, failedUrl ? (int)strlen(failedUrl) : 0);
}

/* agavePostMessage() is the page -> host channel (the probe reports its result
 * through it); it must cross the boundary like every other event. */
static void onPostMessage(const char* message)
{
    if (message)
        pushEvent(AGW_VIEW_EVENT_MESSAGE, 0, 0, 0, 0, message, (int)strlen(message));
}

static void onInvalidate(MaCrossView* view, const MC_RECT* rect)
{
    (void)view;
    (void)rect;
}

/* The watchdog: waits on the control's interrupt event and aborts the script
 * this process's engine thread is stuck in. Separate thread on purpose - the
 * engine thread is inside the script, so it cannot read the command ring. */
static DWORD WINAPI watchdogThread(LPVOID param)
{
    (void)param;

    while (!g_stopWatchdog) {
        if (WaitForSingleObject(g_intEvent, 200) == WAIT_OBJECT_0) {
            rlog("[render] watchdog: interrupting the page script\n");
            macross_interrupt_view(g_view);
        }
    }

    return 0;
}

/* ---- commands from the control ----------------------------------------- */

/* Copies this slot's text out: the arena is a ring, so a contiguous run is only
 * guaranteed inside the slot's own copy. */
static void commandText(const AgwIpcCommand* cmd, char* out, int outCap)
{
    int len = cmd->textLen;

    out[0] = 0;

    if (len <= 0 || !outCap)
        return;

    if (len > outCap - 1)
        len = outCap - 1;

    if (cmd->textOffset >= 0 && cmd->textOffset + len <= AGW_IPC_TEXT_BYTES)
        memcpy(out, &g_hdr->cmdText[cmd->textOffset], (size_t)len);
    else if (cmd->textOffset >= 0 && cmd->textOffset < AGW_IPC_TEXT_BYTES) {
        int first = AGW_IPC_TEXT_BYTES - cmd->textOffset;

        memcpy(out, &g_hdr->cmdText[cmd->textOffset], (size_t)first);
        memcpy(out + first, g_hdr->cmdText, (size_t)(len - first));
    }

    out[len] = 0;
}

static int executeCommand(const AgwIpcCommand* cmd)
{
    char text[4096];

    commandText(cmd, text, sizeof(text));

    if (g_log)
        rlog("[render] cmd kind=%d a=%d b=%d text='%s'\n",
                cmd->kind, cmd->a, cmd->b, text);

    switch (cmd->kind) {
    case AGW_VIEW_CMD_VISIBLE:
        g_visible = cmd->a ? 1 : 0;
        macross_view_set_visible(g_view, g_visible);
        return 1;
    case AGW_VIEW_CMD_FOCUS:
        macross_view_set_focus(g_view);
        return 1;
    case AGW_VIEW_CMD_MOUSE: {
        MC_MOUSE_EVENT event;

        memset(&event, 0, sizeof(event));
        event.type = (MC_MOUSE_EVENT_TYPE)cmd->a;
        event.button = (MC_MOUSE_BUTTON)cmd->b;
        event.point.x = cmd->c;
        event.point.y = cmd->d;
        macross_mouse_event(g_view, &event);
        return 1;
    }
    case AGW_VIEW_CMD_KEY: {
        MC_KEY_EVENT event;

        memset(&event, 0, sizeof(event));
        event.type = (MC_KEY_EVENT_TYPE)cmd->a;
        event.key = (MC_VIRTUAL_KEY)cmd->c;
        event.modifier = (unsigned int)cmd->b;
        macross_keyboard_event(g_view, &event);
        return 1;
    }
    case AGW_VIEW_CMD_TEXT:
        if (text[0])
            macross_view_input_text(g_view, text);
        return 1;
    case AGW_VIEW_CMD_NAVIGATE:
        if (text[0])
            macross_view_open_url(g_view, text);
        return 1;
    case AGW_VIEW_CMD_LOAD_HTML:
        if (text[0])
            macross_view_load_html(g_view, text, "");
        return 1;
    case AGW_VIEW_CMD_RELOAD:  macross_view_reload(g_view);  return 1;
    case AGW_VIEW_CMD_STOP:    macross_view_stop(g_view);    return 1;
    case AGW_VIEW_CMD_BACK:    macross_view_backward(g_view); return 1;
    case AGW_VIEW_CMD_FORWARD: macross_view_forward(g_view);  return 1;
    case AGW_VIEW_CMD_EVAL:
        if (text[0])
            macross_view_eval_script(g_view, text);
        return 1;
    case AGW_VIEW_CMD_SEARCH:
        if (text[0])
            macross_view_search_text(g_view, text, cmd->a ? 1 : 0, cmd->b ? 1 : 0, 1);
        return 1;
    case AGW_VIEW_CMD_ZOOM:
        macross_view_set_scale_factor(g_view, cmd->a > 0 ? cmd->a : 100);
        return 1;
    case AGW_VIEW_CMD_CONTEXT_MENU: {
        MC_CONTEXT_EVENT event;

        memset(&event, 0, sizeof(event));
        event.point.x = cmd->c;
        event.point.y = cmd->d;
        event.modifier = (unsigned int)cmd->a;
        macross_context_menu_event(g_view, &event);
        return 1;
    }
    case AGW_IPC_CMD_RESIZE:
        if (cmd->a > 0 && cmd->b > 0) {
            InterlockedExchange(&g_hdr->width, cmd->a);
            InterlockedExchange(&g_hdr->height, cmd->b);
            macross_view_resize(g_view, g_frame, cmd->a, cmd->b,
                                (int)g_hdr->framePitch);
            pushEvent(AGW_VIEW_EVENT_READY, AGW_VIEW_PROTOCOL_VERSION, 0, 0, 0, NULL, 0);
        }
        return 1;
    case AGW_IPC_CMD_EXIT:
        return 0;
    default:
        return 1;
    }
}

static int drainCommands(void)
{
    LONG read = g_hdr->commandRead;

    while (read != g_hdr->commandWrite) {
        AgwIpcCommand* cmd = &g_hdr->commands[read & (AGW_IPC_CMD_SLOTS - 1)];

        if (!executeCommand(cmd))
            return 0;

        ++read;
        InterlockedExchange(&g_hdr->commandRead, read);
        /* A resize/zoom changes what the next frame must cover. */
    }

    return 1;
}

int main(int argc, char** argv)
{
    const char* ipcName = NULL;
    int width = 1100, height = 800;
    int crashAfterMs = 0;
    HANDLE map;
    ULONGLONG started;
    char eventName[128];

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--ipc") && i + 1 < argc)
            ipcName = argv[++i];
        else if (!strcmp(argv[i], "--crash-after") && i + 1 < argc)
            crashAfterMs = atoi(argv[++i]);
    }

    if (!ipcName || !ipcName[0]) {
        rlog("render_host: --ipc <name> is required\n");
        return 2;
    }

    g_log = getenv("AGB_RENDER_LOG") ? 1 : 0;

    map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, ipcName);
    if (!map) {
        rlog("render_host: cannot open mapping '%s' (err %lu)\n",
                ipcName, (unsigned long)GetLastError());
        return 3;
    }

    g_hdr = (AgwIpcHeader*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!g_hdr) {
        rlog("render_host: cannot map '%s' (err %lu)\n",
                ipcName, (unsigned long)GetLastError());
        return 3;
    }

    if (g_hdr->magic != AGW_IPC_MAGIC) {
        rlog("render_host: bad magic in '%s'\n", ipcName);
        return 3;
    }

    width = (int)g_hdr->width;
    height = (int)g_hdr->height;
    if (width <= 0 || height <= 0)
        return 3;

    snprintf(eventName, sizeof(eventName), "%s_evt", ipcName);
    g_cmdEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, eventName);

    snprintf(eventName, sizeof(eventName), "%s_int", ipcName);
    g_intEvent = OpenEventA(SYNCHRONIZE, FALSE, eventName);

    g_frame = agwIpcFrame(g_hdr);

    if (macross_initialize(PIXEL_FORMAT_BGRA32, width, height) != MC_STATUS_SUCCESS) {
        rlog("render_host: macross_initialize failed\n");
        return 4;
    }

    {
        MC_CALLBACK_INFO info;

        memset(&info, 0, sizeof(info));
        info.cb_invalidate_rect  = onInvalidate;
        info.cb_set_title        = onTitle;
        info.cb_set_location     = onLocation;
        info.cb_set_status       = onStatus;
        info.cb_loading_progress = onProgress;
        info.cb_error            = onError;
        macross_set_callback(&info);
    }

    macross_set_post_message_hook(onPostMessage);

    {
    }

    g_view = macross_view_create(g_frame, width, height, (int)g_hdr->framePitch, NULL);
    if (!g_view) {
        rlog("render_host: macross_view_create failed\n");
        return 4;
    }

    g_visible = g_hdr->visible ? 1 : 0;
    macross_view_set_visible(g_view, g_visible);

    InterlockedExchange(&g_hdr->running, 1);
    pushEvent(AGW_VIEW_EVENT_READY, AGW_VIEW_PROTOCOL_VERSION,
              AGW_VIEW_CAP_DAMAGE, 0, 0, NULL, 0);

    if (g_intEvent)
        CreateThread(NULL, 0, watchdogThread, NULL, 0, NULL);

    started = GetTickCount64();

    for (;;) {
        if (!drainCommands())
            break;

        if (crashAfterMs > 0 && (int)(GetTickCount64() - started) >= crashAfterMs) {
            rlog("render_host: crashing on request (%d ms)\n", crashAfterMs);
            ExitProcess(42);
        }

        macross_event_dispatch();

        if (!g_visible) {
            /* Hidden view: timers/network keep running, no layout or paint
             * (the same rule as R150 in-process). */
            macross_view_pump(g_view);
            if (g_cmdEvent)
                WaitForSingleObject(g_cmdEvent, 10);
            else
                Sleep(10);
            continue;
        }

        /* Publish only when the previous frame was consumed: the canvas is one
         * buffer, so painting over an unacked frame would tear it (the ack is
         * the frame backpack of the design). */
        if (g_hdr->frameSeq == g_hdr->ackSeq) {
            uint32_t seq;

            macross_view_update(g_view, NULL);

            seq = (uint32_t)InterlockedIncrement(&g_hdr->frameSeq);
            pushFrameEvent(seq, (int)g_hdr->width, (int)g_hdr->height);
            if (g_cmdEvent)
                SetEvent(g_cmdEvent);
        } else if (g_cmdEvent) {
            /* Waiting for the host to composite; still wake for commands. */
            WaitForSingleObject(g_cmdEvent, 4);
        } else {
            Sleep(4);
        }
    }

    g_stopWatchdog = 1;
    InterlockedExchange(&g_hdr->running, 0);
    macross_view_destroy(g_view);
    macross_shutdown();
    return 0;
}
