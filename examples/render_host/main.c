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
#include "../../src/webview/AgbCrash.h"
#include "agave_webview.h"   /* AGW_REASON_*: one vocabulary for drop reasons */

/* Crash report sink: copy the text into the shared section. The section
 * outlives this process, so the control reads the report after we are gone and
 * shows it on the "Page crashed" page. Raw memcpy only - this runs on a dying
 * thread. */
static void crashSink(const char* text, unsigned int len, void* user)
{
    AgwIpcHeader* hdr = (AgwIpcHeader*)user;

    if (!hdr || !text || !len)
        return;

    if (len >= AGW_IPC_CRASH_BYTES)
        len = AGW_IPC_CRASH_BYTES - 1;

    memcpy(hdr->crashText, text, len);
    hdr->crashText[len] = 0;
    InterlockedExchange(&hdr->crashLen, (LONG)len);
}

static AgwIpcHeader* g_hdr;
static HANDLE        g_map;
static HANDLE        g_cmdEvent;
static HANDLE        g_intEvent;
static HANDLE        g_txtEvent;   /* text arena space / ring slot freed */
static HANDLE        g_payEvent;   /* payload section copied */
static volatile LONG g_stopWatchdog;
static MaCrossView*  g_view;
static MC_BYTE*      g_frame;
static int           g_visible = 1;
static int           g_log;

/* Full-size command text buffer: one command's arena text is copied here whole
 * (the control never sends more than AGW_IPC_TEXT_BYTES). The old fixed 4 KiB
 * stack buffer silently cut every injected script in half. */
static char          g_cmdText[AGW_IPC_TEXT_BYTES + 1];

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
    ev->docEpoch = macross_view_document_epoch(g_view);

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

/* Microseconds from QueryPerformanceCounter: both sides run on one machine and
 * share the counter, so a frame event's stamp is comparable in the control
 * (the M1 measurement of the design). */
static int64_t qpcUs(void)
{
    LARGE_INTEGER c, f;

    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (int64_t)(c.QuadPart * 1000000 / f.QuadPart);
}

static void pushFrameEvent(uint32_t seq, int width, int height)
{
    LONG slot = InterlockedIncrement(&g_hdr->eventWrite) - 1;
    AgwIpcEvent* ev = &g_hdr->events[slot & (AGW_IPC_EVT_SLOTS - 1)];

    memset(ev, 0, sizeof(*ev));
    ev->kind = AGW_VIEW_EVENT_FRAME;
    ev->timeUs = qpcUs();
    ev->docEpoch = macross_view_document_epoch(g_view);
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

/* The OS clipboard belongs to the host process (it already owns the Win32
 * writer for the local backend). The renderer only reports the text the page
 * copied; this is one-way, no reply is needed. */
static void onClipboardSetData(void* data, unsigned int len)
{
    if (!data || !len)
        return;

    if ((int)len > AGW_IPC_EVENT_TEXT_MAX) {
        /* One event carries at most this much (see AgwViewIpc.h). Report the
         * size so the host does not write a silently truncated clipboard. */
        rlog("[render] clipboard text too large for one event (%u bytes)\n", len);
        pushEvent(AGW_VIEW_EVENT_CLIPBOARD, (int)len, 1, 0, 0, NULL, 0);
        return;
    }

    pushEvent(AGW_VIEW_EVENT_CLIPBOARD, 0, 0, 0, 0, (const char*)data, (int)len);
}

/* Cursor and back/forward state are host UI: the renderer reports them and the
 * control applies them (same callbacks the local backend hands to the host). */
static void onSetCursor(MC_CURSOR cursor)
{
    pushEvent(AGW_VIEW_EVENT_CURSOR, (int)cursor, 0, 0, 0, NULL, 0);
}

static void onHistoryStatus(MaCrossView* view, unsigned int backCount,
                            unsigned int forwardCount)
{
    (void)view;
    pushEvent(AGW_VIEW_EVENT_HISTORY, (int)backCount, (int)forwardCount, 0, 0, NULL, 0);
}

/* agavePostMessage() is the page -> host channel (the probe reports its result
 * through it); it must cross the boundary like every other event. */
static void onPostMessage(const char* message)
{
    if (message)
        pushEvent(AGW_VIEW_EVENT_MESSAGE, 0, 0, 0, 0, message, (int)strlen(message));
}

/* The engine wants a context menu: it cannot show one (no windows in this
 * process), so the request travels to the control, which asks the host. The
 * payload is link + image + selection text, split by the two lengths carried in
 * c and d (a NUL separator would truncate a selection that contains one). */
static void onContextMenu(MaCrossView* view, const MC_CONTEXT_MENU_DATA* menu)
{
    char blob[4096];
    int linkLen = 0;
    int imageLen = 0;
    int textLen = 0;
    int x = 0;
    int y = 0;

    (void)view;

    if (menu) {
        x = menu->location.x;
        y = menu->location.y;

        if (menu->link_url)
            linkLen = (int)strlen(menu->link_url);
        if (menu->image_url)
            imageLen = (int)strlen(menu->image_url);
        if (menu->text)
            textLen = (int)strlen(menu->text);
    }

    if (linkLen + imageLen + textLen > (int)sizeof(blob) - 1) {
        int room = (int)sizeof(blob) - 1;

        if (linkLen > room) { linkLen = room; imageLen = 0; textLen = 0; room = 0; }
        else room -= linkLen;

        if (imageLen > room) { imageLen = room; textLen = 0; room = 0; }
        else room -= imageLen;

        if (textLen > room)
            textLen = room;
    }

    if (linkLen && menu->link_url)
        memcpy(blob, menu->link_url, (size_t)linkLen);
    if (imageLen && menu->image_url)
        memcpy(blob + linkLen, menu->image_url, (size_t)imageLen);
    if (textLen && menu->text)
        memcpy(blob + linkLen + imageLen, menu->text, (size_t)textLen);

    blob[linkLen + imageLen + textLen] = 0;

    pushEvent(AGW_VIEW_EVENT_CONTEXT_MENU, x, y, linkLen, imageLen, blob,
              linkLen + imageLen + textLen);
}

/* Window lifecycle. This process cannot create a window (the control owns the
 * host-facing API and the popup would live in yet another process), so both
 * requests travel as service events: open_window carries "name\nurl", and the
 * control reuses a named window or makes a new tab. */
static MaCrossView* onOpenWindow(const char* url, const char* name,
                                 unsigned int flags, const MC_RECT* rect)
{
    char payload[2048];

    (void)rect;

    snprintf(payload, sizeof(payload), "%s\n%s", name ? name : "", url ? url : "");
    pushEvent(AGW_VIEW_EVENT_SERVICE_REQUEST, AGW_VIEW_SERVICE_OPEN_WINDOW,
              (int)flags, 0, 0, payload, (int)strlen(payload));

    /* No view to return: the window is created by the control, in its own
     * process. The request has already carried the URL there. */
    return NULL;
}

static void onCloseWindow(MaCrossView* view)
{
    (void)view;
    pushEvent(AGW_VIEW_EVENT_SERVICE_REQUEST, AGW_VIEW_SERVICE_CLOSE_WINDOW,
              0, 0, 0, NULL, 0);
}

/* window.open(): same request over the same channel; returning 1 gives the page
 * its (stub) window object instead of null. */
static int onOpenWindowHook(const char* url, const char* name)
{
    char payload[2048];

    snprintf(payload, sizeof(payload), "%s\n%s", name ? name : "", url ? url : "");
    pushEvent(AGW_VIEW_EVENT_SERVICE_REQUEST, AGW_VIEW_SERVICE_OPEN_WINDOW,
              0, 0, 0, payload, (int)strlen(payload));
    return 1;
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
 * guaranteed inside the slot's own copy. The caller passes a buffer of at least
 * AGW_IPC_TEXT_BYTES + 1; the copy is never truncated. */
static void commandText(const AgwIpcCommand* cmd, char* out, int outCap)
{
    int len = cmd->textLen;

    out[0] = 0;

    if (len <= 0 || !outCap)
        return;

    if (cmd->textOffset < 0 || cmd->textOffset >= AGW_IPC_TEXT_BYTES) {
        len = 0;
        return;
    }

    if (len > AGW_IPC_TEXT_BYTES - cmd->textOffset)
        len = AGW_IPC_TEXT_BYTES - cmd->textOffset;   /* corrupt slot: clamp */
    if (len > outCap - 1)
        len = outCap - 1;

    memcpy(out, &g_hdr->cmdText[cmd->textOffset], (size_t)len);
    out[len] = 0;
}

/* The renderer has copied (or will never use) the text of this command: publish
 * the arena read pointer so the producer may reuse the bytes even if the
 * command itself runs for a long time. */
static void ackCommandText(const AgwIpcCommand* cmd)
{
    int end;

    if (cmd->textLen <= 0 || cmd->textOffset < 0)
        return;

    end = cmd->textOffset + cmd->textLen;
    if (end < cmd->textOffset || end > AGW_IPC_TEXT_BYTES)
        return;   /* corrupt slot: leave the read pointer alone */

    InterlockedExchange(&g_hdr->cmdTextRead, end);
    if (g_txtEvent)
        SetEvent(g_txtEvent);
}

static void ackPayload(void)
{
    if (g_payEvent)
        SetEvent(g_payEvent);
}

static void reportPayloadFailure(const char* why, int kind, unsigned epoch)
{
    pushEvent(AGW_VIEW_EVENT_COMMAND_DROPPED, kind, 0, (int)epoch, 0,
              why, (int)strlen(why));
}

/* AGW_IPC_CMD_PAYLOAD: the text arrived through a one-shot named section, not
 * the arena. Copy it into our heap, ack the section (the host frees it now),
 * then execute the command that asked for it. */
static int executePayload(const AgwIpcCommand* cmd)
{
    static const char noSection[] = AGW_REASON_PAYLOAD;
    unsigned epoch = g_view ? macross_view_document_epoch(g_view) : 0;
    HANDLE map;
    const char* base = NULL;
    char* text;
    int len = cmd->b;

    if (cmd->payloadName[AGW_IPC_PAYLOAD_NAME - 1] != 0 || len <= 0 ||
        len > AGW_IPC_PAYLOAD_MAX) {
        ackPayload();
        reportPayloadFailure(noSection, (int)cmd->a, epoch);
        return 1;
    }

    map = OpenFileMappingA(FILE_MAP_READ, FALSE, cmd->payloadName);
    if (map)
        base = (const char*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);

    if (!base) {
        rlog("[render] payload '%s' open failed (err %lu)\n",
             cmd->payloadName, (unsigned long)GetLastError());
        if (map)
            CloseHandle(map);
        ackPayload();
        reportPayloadFailure(noSection, (int)cmd->a, epoch);
        return 1;
    }

    text = (char*)malloc((size_t)len + 1);
    if (!text) {
        UnmapViewOfFile((void*)base);
        CloseHandle(map);
        ackPayload();
        reportPayloadFailure(noSection, (int)cmd->a, epoch);
        return 1;
    }

    memcpy(text, base, (size_t)len);
    text[len] = 0;
    UnmapViewOfFile((void*)base);
    CloseHandle(map);

    /* The payload is ours now; the host may unmap its section. Ack before the
     * command runs: an eval can take arbitrarily long. */
    ackPayload();

    if (g_log)
        rlog("[render] payload kind=%d bytes=%d text='%.120s'\n", cmd->a, len, text);

    switch (cmd->a) {
    case AGW_VIEW_CMD_EVAL:
        macross_view_eval_script(g_view, text);
        break;
    case AGW_VIEW_CMD_LOAD_HTML:
        macross_view_load_html(g_view, text, "");
        break;
    case AGW_VIEW_CMD_NAVIGATE:
        macross_view_open_url(g_view, text);
        break;
    default:
        /* The control-side validation refuses this; be loud if it happens. */
        reportPayloadFailure(noSection, (int)cmd->a, epoch);
        break;
    }

    free(text);
    return 1;
}

static int executeCommand(const AgwIpcCommand* cmd)
{
    unsigned epoch = g_view ? macross_view_document_epoch(g_view) : 0;
    unsigned cmdEpoch = cmd->docEpoch;
    const char* text = g_cmdText;

    /* Test hook: pretend every stamped command was issued for a different
     * document, so the "renderer refuses a stale command" path can be exercised
     * deterministically (the real window - a command that waits out a commit -
     * is too narrow to hit on purpose). */
    if (getenv("AGB_IPC_STALE_EPOCH") && cmdEpoch)
        cmdEpoch += 0x1000;

    commandText(cmd, g_cmdText, sizeof(g_cmdText));

    /* Release the arena bytes before running the command: a script can run for
     * seconds, and the producer must not be blocked on bytes we already own. */
    ackCommandText(cmd);

    if (g_log)
        rlog("[render] cmd kind=%d a=%d b=%d epoch=%u cur=%u text='%s'\n",
                cmd->kind, cmd->a, cmd->b, cmdEpoch, epoch, text);

    /* A command stamped with a document that is no longer current was issued
     * for a page that has since been replaced: running it would script the new
     * page behind the host's back, doing nothing would be invisible. Report it
     * so the control can tell its host (the local backend answers 0 instead). */
    if (cmdEpoch && epoch && cmdEpoch != epoch) {
        static const char why[] = AGW_REASON_DOCUMENT_REPLACED;
        int kind = (int)cmd->kind;

        if (cmd->kind == AGW_IPC_CMD_PAYLOAD) {
            ackPayload();   /* the control is waiting on the section copy */
            kind = (int)cmd->a;   /* report the command the payload was for */
        }

        pushEvent(AGW_VIEW_EVENT_COMMAND_DROPPED, kind,
                  (int)cmdEpoch, (int)epoch, 0, why, (int)sizeof(why) - 1);
        return 1;
    }

    switch (cmd->kind) {
    case AGW_IPC_CMD_PAYLOAD:
        return executePayload(cmd);
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
        if (text[0] && !macross_view_eval_script(g_view, text)) {
            static const char why[] = AGW_REASON_NO_DOCUMENT;

            pushEvent(AGW_VIEW_EVENT_COMMAND_DROPPED, AGW_VIEW_CMD_EVAL, 0,
                      (int)epoch, 0, why, (int)sizeof(why) - 1);
        }
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
        /* A consumed command frees its ring slot; the producer may be waiting
         * for one (or for arena space it reclaimed while copying the text). */
        if (g_txtEvent)
            SetEvent(g_txtEvent);
        /* A resize/zoom changes what the next frame must cover. */
    }

    return 1;
}

int main(int argc, char** argv)
{
    const char* ipcName = NULL;
    int width = 1100, height = 800;
    int crashAfterMs = 0;
    int crashAvAfterMs = 0;
    const char* crashAvEnv;
    HANDLE map;
    ULONGLONG started;
    char eventName[128];

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--ipc") && i + 1 < argc)
            ipcName = argv[++i];
        else if (!strcmp(argv[i], "--crash-after") && i + 1 < argc)
            crashAfterMs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--crash-av") && i + 1 < argc)
            crashAvEnv = argv[++i];
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

    if (g_hdr->version != AGW_IPC_VERSION) {
        /* Both executables are deployed together; a mismatch means a stale
         * renderer would read a layout it does not know. Say so instead of
         * running with garbage offsets. */
        rlog("render_host: ipc version %u, expected %u\n",
             (unsigned)g_hdr->version, (unsigned)AGW_IPC_VERSION);
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

    snprintf(eventName, sizeof(eventName), "%s_txt", ipcName);
    g_txtEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName);

    snprintf(eventName, sizeof(eventName), "%s_pay", ipcName);
    g_payEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName);

    g_frame = agwIpcFrame(g_hdr);

    /* Crash reporting: the report lands in the shared section, so the control
     * can show it on the "Page crashed" page even though this process is gone
     * by then. dbghelp is loaded here (not in the handler), which is why the
     * report can carry resolved symbols. */
    {
        char info[512];

        snprintf(info, sizeof(info), "backend=process ipc=%s size=%dx%d",
                 ipcName, width, height);
        agbCrashInstall("render-host", crashSink, g_hdr);
        agbCrashSetInfo(info);
    }

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
        info.cb_context_menu     = onContextMenu;
        info.cb_open_window      = onOpenWindow;
        info.cb_close_window     = onCloseWindow;
        /* Host-owned UI callbacks: the copy path (clipboard), the mouse cursor
         * and the back/forward counts. Without them a process view silently
         * lost "copy from the page" (inspector's Copy element info), never
         * changed the cursor and kept the shell's back/forward buttons
         * disabled. */
        info.cb_clipboard_set_data = onClipboardSetData;
        info.cb_set_cursor         = onSetCursor;
        info.cb_history_status     = onHistoryStatus;
        macross_set_callback(&info);
    }

    macross_set_post_message_hook(onPostMessage);
    macross_set_open_window_hook(onOpenWindowHook);

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

    if (crashAvEnv)
        crashAvAfterMs = atoi(crashAvEnv);

    started = GetTickCount64();

    for (;;) {
        if (!drainCommands())
            break;

        /* The host stamps its commands with the epoch of the page it is looking
         * at, so it has to hear about a commit even when the new page publishes
         * no other event (a static page used to leave the host on the old
         * epoch forever, and then it dropped the first command after the load). */
        {
            static unsigned lastEpoch;

            unsigned now = macross_view_document_epoch(g_view);

            if (now != lastEpoch) {
                lastEpoch = now;
                pushEvent(AGW_VIEW_EVENT_DOCUMENT, (int)now, 0, 0, 0, NULL, 0);
            }
        }

        if (crashAfterMs > 0 && (int)(GetTickCount64() - started) >= crashAfterMs) {
            rlog("render_host: crashing on request (%d ms)\n", crashAfterMs);
            ExitProcess(42);
        }

        /* Test hook: a *real* access violation, so the crash report path (env,
         * stack, symbols, the page the control shows) can be exercised. The
         * plain --crash-after above is a clean exit and produces no report. */
        if (crashAvAfterMs > 0 && (int)(GetTickCount64() - started) >= crashAvAfterMs) {
            rlog("render_host: faking an access violation (%d ms)\n", crashAvAfterMs);
            *(volatile int*)0 = 1;
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
