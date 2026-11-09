/* AgwViewBackendProcess.c - the `process` render backend (S3 B2).
 *
 * One agave_render_host.exe per view. The control creates a named shared
 * mapping (AgwViewIpc.h: canvas + command ring + event ring), spawns the
 * renderer with the mapping's name, and from then on talks to it only through
 * the protocol: submit() writes a command, poll() drains events and frames.
 *
 * Crash: the renderer's process handle is part of the view handle. poll() checks
 * for it (INFINITE timeout) and reports AGW_VIEW_EVENT_EXIT with a = exit code,
 * b = 1 when it was not a requested close; destroy() sends EXIT and terminates
 * the process if it does not go away.
 */
#include "AgwViewBackend.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AgwViewIpc.h"
#include "../webview/AgbLog.h"

static int64_t qpcUs(void)
{
    LARGE_INTEGER c, f;

    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (int64_t)(c.QuadPart * 1000000 / f.QuadPart);
}

/* One kill-on-close job for every renderer this process starts: whatever ends
 * the control (a clean shutdown, a crash, the probe calling ExitProcess) takes
 * the rendersers with it, instead of leaving orphans behind. */
static HANDLE g_rendererJob;

static void ensureRendererJob(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;

    if (g_rendererJob || g_rendererJob == INVALID_HANDLE_VALUE)
        return;

    g_rendererJob = CreateJobObjectA(NULL, NULL);
    if (!g_rendererJob) {
        g_rendererJob = INVALID_HANDLE_VALUE;
        return;
    }

    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(g_rendererJob, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        CloseHandle(g_rendererJob);
        g_rendererJob = INVALID_HANDLE_VALUE;
    }
}

typedef struct {
    HANDLE        mapping;
    AgwIpcHeader* hdr;
    HANDLE        cmdEvent;
    HANDLE        intEvent;   /* watchdog: abort the renderer's page script */
    HANDLE        process;
    unsigned long pid;
    char          ipcName[160];
    char          exePath[MAX_PATH];
    int           width;
    int           height;
    int           exited;
    int           exitCode;
    /* One text slot per event of a poll batch: every AgwViewEvent in one batch
     * must keep its own text, or a later event would overwrite an earlier one
     * before the caller dispatches it. */
    char          evtTextBuf[8][1024];

    /* AGB_IPC_STATS=1: frame publish -> this process seeing the event (M1 of
     * the design), printed every 30 frames. */
    /* Document epoch the renderer last reported (events carry it). Commands
     * are stamped with this, and the renderer refuses one it has left. */
    uint32_t      docEpoch;

    unsigned long framesSeen;
    int64_t       frameLatencySumUs;
    int64_t       frameLatencyMaxUs;
} ProcessView;

/* The renderer sits next to the host executable (agave_shell / the embedder):
 * both are deployed together, so the control's own directory is the answer. */
static void renderHostPath(char* out, size_t cap)
{
    char module[MAX_PATH];
    char* slash;
    DWORD n = GetModuleFileNameA(NULL, module, (DWORD)sizeof(module));

    out[0] = 0;

    if (!n || n >= sizeof(module))
        return;

    slash = strrchr(module, '\\');
    if (!slash)
        return;

    *(slash + 1) = 0;
    snprintf(out, cap, "%sagave_render_host.exe", module);
}

static int ipcSize(int width, int height, uint32_t* frameOffset, uint32_t* frameBytes)
{
    uint32_t pitch = (uint32_t)AGW_IPC_FRAME_W_MAX * 4u;
    size_t header = sizeof(AgwIpcHeader);

    header = (header + 63u) & ~(size_t)63u;   /* 64-byte aligned canvas */

    *frameOffset = (uint32_t)header;
    *frameBytes = pitch * (uint32_t)AGW_IPC_FRAME_H_MAX;

    (void)width;
    (void)height;

    return (int)(header + *frameBytes);
}

static void closeView(ProcessView* pv, int requestExit)
{
    if (!pv)
        return;

    if (requestExit && pv->hdr) {
        LONG slot;
        AgwIpcCommand* cmd;

        /* Break a script that is spinning first: the renderer's watchdog thread
         * aborts it, so the EXIT command below is actually read. Closing a hung
         * tab used to sit here for the full grace period. */
        if (pv->intEvent)
            SetEvent(pv->intEvent);

        slot = InterlockedIncrement(&pv->hdr->commandWrite) - 1;
        cmd = &pv->hdr->commands[slot & (AGW_IPC_CMD_SLOTS - 1)];
        memset(cmd, 0, sizeof(*cmd));
        cmd->kind = AGW_IPC_CMD_EXIT;
        if (pv->cmdEvent)
            SetEvent(pv->cmdEvent);
    }

    if (pv->process) {
        /* Short grace: a responsive renderer exits in a frame or two; a renderer
         * that is still stuck after the interrupt is killed, not waited for. */
        if (WaitForSingleObject(pv->process, requestExit ? 300 : 0) != WAIT_OBJECT_0) {
            TerminateProcess(pv->process, 0);
            WaitForSingleObject(pv->process, 500);
        }
    }

    if (pv->hdr) {
        if (pv->exited == 0) {
            DWORD code = 0;

            if (pv->process && GetExitCodeProcess(pv->process, &code))
                pv->exitCode = (int)code;
        }
        UnmapViewOfFile(pv->hdr);
        pv->hdr = NULL;
    }

    if (pv->mapping) {
        CloseHandle(pv->mapping);
        pv->mapping = NULL;
    }
    if (pv->cmdEvent) {
        CloseHandle(pv->cmdEvent);
        pv->cmdEvent = NULL;
    }
    if (pv->intEvent) {
        CloseHandle(pv->intEvent);
        pv->intEvent = NULL;
    }
    if (pv->process) {
        CloseHandle(pv->process);
        pv->process = NULL;
    }
}

/* ---- process level ------------------------------------------------------ */

static int processInit(int width, int height)
{
    (void)width;
    (void)height;
    return 1;   /* each view owns its engine; nothing process wide to start */
}

static void processPump(void)
{
    /* The renderer processes have their own loops; macross_event_dispatch()
     * would run the *local* engine, which this backend never creates. */
}

static void processShutdown(void)
{
}

/* ---- one view ----------------------------------------------------------- */

static void* processCreate(void* owner, MC_BYTE* buffer, int width, int height, int pitch)
{
    ProcessView* pv;
    uint32_t frameOffset = 0;
    uint32_t frameBytes = 0;
    int size;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[512];
    int waited = 0;

    (void)owner;
    (void)buffer;
    (void)pitch;

    if (width < 1) width = 1100;
    if (height < 1) height = 800;

    pv = (ProcessView*)calloc(1, sizeof(*pv));
    if (!pv)
        return NULL;

    renderHostPath(pv->exePath, sizeof(pv->exePath));
    if (!pv->exePath[0]) {
        agbLog(AGB_LOG_SHELL, "process backend: agave_render_host.exe not found next to the host");
        free(pv);
        return NULL;
    }

    snprintf(pv->ipcName, sizeof(pv->ipcName), "Local\\agave_view_%lu_%p",
             (unsigned long)GetCurrentProcessId(), owner);

    size = ipcSize(width, height, &frameOffset, &frameBytes);

    pv->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                     0, (DWORD)size, pv->ipcName);
    if (!pv->mapping) {
        agbLog(AGB_LOG_SHELL, "process backend: CreateFileMapping failed (err %lu)",
               (unsigned long)GetLastError());
        free(pv);
        return NULL;
    }

    pv->hdr = (AgwIpcHeader*)MapViewOfFile(pv->mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!pv->hdr) {
        agbLog(AGB_LOG_SHELL, "process backend: MapViewOfFile failed (err %lu)",
               (unsigned long)GetLastError());
        closeView(pv, 0);
        free(pv);
        return NULL;
    }

    memset(pv->hdr, 0, sizeof(*pv->hdr));
    pv->hdr->magic = AGW_IPC_MAGIC;
    pv->hdr->version = AGW_IPC_VERSION;
    pv->hdr->width = width;
    pv->hdr->height = height;
    pv->hdr->visible = 1;
    pv->hdr->frameOffset = frameOffset;
    pv->hdr->framePitch = (uint32_t)AGW_IPC_FRAME_W_MAX * 4u;
    pv->hdr->frameBytes = frameBytes;

    {
        char eventName[160];

        snprintf(eventName, sizeof(eventName), "%s_evt", pv->ipcName);
        pv->cmdEvent = CreateEventA(NULL, FALSE, FALSE, eventName);

        snprintf(eventName, sizeof(eventName), "%s_int", pv->ipcName);
        pv->intEvent = CreateEventA(NULL, FALSE, FALSE, eventName);
    }

    {
        /* AGB_RENDER_CRASH_MS is a one-shot test hook: only the first renderer
         * of the process gets it, so the respawned one after a crash is
         * healthy - otherwise a crash test would loop forever. */
        static int crashUsed = 0;
        const char* crash = crashUsed ? NULL : getenv("AGB_RENDER_CRASH_MS");

        /* --owner: the control that spawned this renderer (diagnostics).
         * --tag: AGB_RENDER_TAG passed straight through, so a test can tell its
         * own renderers from ones a developer's shell is using. A test only
         * ever touches renderers carrying its own tag; everything else is
         * somebody else's process and must be left alone. */
        {
            char tag[80] = "";
            const char* env = getenv("AGB_RENDER_TAG");
            size_t k;

            if (env) {
                for (k = 0; env[k] && k < sizeof(tag) - 1; ++k) {
                    char ch = env[k];

                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')
                        tag[k] = ch;
                    else
                        tag[k] = '_';
                }
                tag[k] = 0;
            }

            if (crash && atoi(crash) > 0) {
                crashUsed = 1;
                snprintf(cmdline, sizeof(cmdline),
                         "\"%s\" --ipc \"%s\" --owner %lu%s%s --crash-after %s",
                         pv->exePath, pv->ipcName, (unsigned long)GetCurrentProcessId(),
                         tag[0] ? " --tag " : "", tag, crash);
            } else {
                snprintf(cmdline, sizeof(cmdline),
                         "\"%s\" --ipc \"%s\" --owner %lu%s%s",
                         pv->exePath, pv->ipcName, (unsigned long)GetCurrentProcessId(),
                         tag[0] ? " --tag " : "", tag);
            }
        }
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agbLog(AGB_LOG_SHELL, "process backend: cannot start renderer '%s' (err %lu)",
               pv->exePath, (unsigned long)GetLastError());
        closeView(pv, 0);
        free(pv);
        return NULL;
    }

    pv->process = pi.hProcess;
    pv->pid = pi.dwProcessId;
    CloseHandle(pi.hThread);

    ensureRendererJob();
    if (g_rendererJob && g_rendererJob != INVALID_HANDLE_VALUE)
        AssignProcessToJobObject(g_rendererJob, pv->process);

    pv->width = width;
    pv->height = height;

    /* Wait for the renderer to boot the engine (READY may come later; running
     * is the first thing it sets after macross_view_create). */
    while (!pv->hdr->running && waited < 5000) {
        Sleep(10);
        waited += 10;
        if (WaitForSingleObject(pv->process, 0) == WAIT_OBJECT_0)
            break;
    }

    if (!pv->hdr->running) {
        agbLog(AGB_LOG_SHELL, "process backend: renderer did not start (pid %lu)", pv->pid);
        closeView(pv, 1);
        free(pv);
        return NULL;
    }

    agbLog(AGB_LOG_SHELL, "process backend: view %s pid=%lu %dx%d", pv->ipcName, pv->pid,
           width, height);

    return pv;
}

static void processDestroy(void* view)
{
    ProcessView* pv = (ProcessView*)view;

    if (!pv)
        return;

    closeView(pv, 1);
    free(pv);
}

static void processResize(void* view, MC_BYTE* buffer, int width, int height, int pitch)
{
    ProcessView* pv = (ProcessView*)view;
    LONG slot;
    AgwIpcCommand* cmd;

    (void)buffer;
    (void)pitch;

    if (!pv || !pv->hdr || width < 1 || height < 1)
        return;

    pv->width = width;
    pv->height = height;

    slot = InterlockedIncrement(&pv->hdr->commandWrite) - 1;
    cmd = &pv->hdr->commands[slot & (AGW_IPC_CMD_SLOTS - 1)];
    memset(cmd, 0, sizeof(*cmd));
    cmd->kind = AGW_IPC_CMD_RESIZE;
    cmd->a = width;
    cmd->b = height;
    if (pv->cmdEvent)
        SetEvent(pv->cmdEvent);
}

static int pushWireText(ProcessView* pv, const char* text, int len, int32_t* outOffset)
{
    int32_t off;

    *outOffset = 0;
    if (!text || len <= 0)
        return 0;

    if (len > AGW_IPC_TEXT_BYTES / 2)
        len = AGW_IPC_TEXT_BYTES / 2;

    off = InterlockedExchangeAdd(&pv->hdr->cmdTextWrite, len);
    /* Keep it contiguous: the renderer copies in one memcpy when it can and
     * wraps otherwise (see commandText()). */
    if ((off & (AGW_IPC_TEXT_BYTES - 1)) + len > AGW_IPC_TEXT_BYTES) {
        off = 0;
        InterlockedExchange(&pv->hdr->cmdTextWrite, len);
    } else {
        off &= (AGW_IPC_TEXT_BYTES - 1);
    }

    memcpy(&pv->hdr->cmdText[off], text, (size_t)len);
    *outOffset = off;
    return len;
}

static int processSubmit(void* view, const AgwViewCommand* command)
{
    ProcessView* pv = (ProcessView*)view;
    LONG slot;
    AgwIpcCommand* wire;
    int32_t textOffset = 0;
    int textLen = 0;

    if (!pv || !pv->hdr || !command)
        return 0;

    if (command->text && command->textLen > 0)
        textLen = pushWireText(pv, command->text, command->textLen, &textOffset);

    slot = InterlockedIncrement(&pv->hdr->commandWrite) - 1;
    wire = &pv->hdr->commands[slot & (AGW_IPC_CMD_SLOTS - 1)];

    memset(wire, 0, sizeof(*wire));
    wire->kind = command->kind;
    wire->a = command->a;
    wire->b = command->b;
    wire->c = command->c;
    wire->d = command->d;
    wire->timeUs = command->timeUs;
    wire->textOffset = textOffset;
    wire->textLen = textLen;
    wire->docEpoch = command->docEpoch;

    if (pv->cmdEvent)
        SetEvent(pv->cmdEvent);

    return 1;
}

static int processSubmitBatch(void* view, const AgwViewCommand* commands, int count)
{
    int i;

    for (i = 0; i < count; ++i) {
        if (!processSubmit(view, &commands[i]))
            return i;
    }

    return count;
}

static int processPoll(void* view, AgwViewEvent* out, int maxEvents)
{
    ProcessView* pv = (ProcessView*)view;
    LONG read;
    int n = 0;

    if (!pv || !pv->hdr || !out || maxEvents <= 0)
        return 0;

    /* A renderer that died is an event, not a silent stall. Reported once. */
    if (!pv->exited && pv->process &&
        WaitForSingleObject(pv->process, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;

        pv->exited = 1;
        if (GetExitCodeProcess(pv->process, &code))
            pv->exitCode = (int)code;

        memset(&out[n], 0, sizeof(out[n]));
        out[n].kind = AGW_VIEW_EVENT_EXIT;
        out[n].a = pv->exitCode;
        out[n].b = 1;   /* not a requested close */
        return n + 1;
    }

    read = pv->hdr->eventRead;
    while (read != pv->hdr->eventWrite && n < maxEvents) {
        AgwIpcEvent* wire = &pv->hdr->events[read & (AGW_IPC_EVT_SLOTS - 1)];
        AgwViewEvent* ev = &out[n];

        memset(ev, 0, sizeof(*ev));
        ev->kind = wire->kind;
        ev->a = wire->a;
        ev->b = wire->b;
        ev->c = wire->c;
        ev->d = wire->d;
        ev->timeUs = wire->timeUs;
        ev->frameBufferId = wire->frameBufferId;
        ev->frameSeq = wire->frameSeq;
        memcpy(ev->damage, wire->damage, sizeof(ev->damage));
        ev->docEpoch = wire->docEpoch;

        /* Later commands are stamped with the epoch of the page the host is
         * currently looking at; every event carries it. */
        if (wire->docEpoch)
            pv->docEpoch = wire->docEpoch;


        if (wire->textLen > 0 && wire->textLen < (int)sizeof(pv->evtTextBuf[0])) {
            char* slot = pv->evtTextBuf[n];

            if (wire->textOffset >= 0 &&
                wire->textOffset + wire->textLen <= AGW_IPC_TEXT_BYTES) {
                memcpy(slot, &pv->hdr->evtText[wire->textOffset], (size_t)wire->textLen);
                slot[wire->textLen] = 0;
                ev->text = slot;
                ev->textLen = wire->textLen;
            }
        }


        if (ev->kind == AGW_VIEW_EVENT_FRAME && wire->timeUs > 0 && getenv("AGB_IPC_STATS")) {
            int64_t latency = qpcUs() - wire->timeUs;

            if (latency < 0)
                latency = 0;

            pv->frameLatencySumUs += latency;
            if (latency > pv->frameLatencyMaxUs)
                pv->frameLatencyMaxUs = latency;
            if (++pv->framesSeen % 30 == 0) {
                fprintf(stderr, "[ipc] frames=%lu latency_avg_us=%lld max_us=%lld\n",
                        pv->framesSeen,
                        (long long)(pv->frameLatencySumUs / (int64_t)pv->framesSeen),
                        (long long)pv->frameLatencyMaxUs);
                fflush(stderr);
            }
        }

        ++read;
        InterlockedExchange(&pv->hdr->eventRead, read);
        ++n;
    }

    return n;
}

static void processFrameAck(void* view, const AgwFrameAck* ack)
{
    ProcessView* pv = (ProcessView*)view;

    if (!pv || !pv->hdr || !ack)
        return;

    InterlockedExchange(&pv->hdr->ackSeq, (LONG)ack->seq);
}

static void processUpdate(void* view, const MC_RECT* damage)
{
    (void)view;
    (void)damage;   /* the renderer paints on its own loop */
}

static void processViewPump(void* view)
{
    (void)view;   /* the renderer drives its own timers/paint loop */
}

/* Aborts the page script in *this* renderer only: the event is watched by a
 * thread inside the renderer, which is the only way to reach it while its engine
 * thread is stuck in the script. */
static void processInterrupt(void* view)
{
    ProcessView* pv = (ProcessView*)view;

    if (pv && pv->intEvent)
        SetEvent(pv->intEvent);
}

static void* processJsContext(void* view)
{
    (void)view;
    return NULL;   /* the engine lives in another process */
}

static unsigned processDocumentEpoch(void* view)
{
    ProcessView* pv = (ProcessView*)view;

    return pv ? pv->docEpoch : 0;
}

/* The canvas the control copies into its mirror: no host-side image of the
 * frame, the shared mapping IS the surface. Returns NULL until the first frame
 * has been published and not acked. */
static const MC_BYTE* processFrame(void* view, int* width, int* height,
                                   int* pitch, uint32_t* seq)
{
    ProcessView* pv = (ProcessView*)view;

    if (!pv || !pv->hdr || !pv->hdr->running)
        return NULL;

    if (pv->hdr->frameSeq == pv->hdr->ackSeq)
        return NULL;

    if (width) *width = (int)pv->hdr->width;
    if (height) *height = (int)pv->hdr->height;
    if (pitch) *pitch = (int)pv->hdr->framePitch;
    if (seq) *seq = (uint32_t)pv->hdr->frameSeq;
    return agwIpcFrame(pv->hdr);
}

static const AgwViewBackend g_processBackend = {
    "process",
    processInit,
    processPump,
    processShutdown,
    processCreate,
    processDestroy,
    processResize,
    processSubmit,
    processSubmitBatch,
    processPoll,
    processFrameAck,
    processUpdate,
    processViewPump,
    processInterrupt,
    processJsContext,
    processDocumentEpoch,
    processFrame
};

const AgwViewBackend* agwBackendProcess(void)
{
    return &g_processBackend;
}
