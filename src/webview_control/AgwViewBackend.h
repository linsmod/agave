/* AgwViewBackend.h - the seam between the control and whatever runs a page.
 *
 * Design: docs/design-render-backend.md (S3). v1 ships exactly one backend,
 * `local`: the engine in this process, driven by macross_* - i.e. the calls the
 * control used to make inline. The process and RVVM backends implement the same
 * interface and are selected with AGB_VIEW_BACKEND.
 *
 * The interface is the protocol from that document: one `AgwViewCommand` in
 * (AgwViewProtocol.h), one `AgwViewEvent` out, plus the three operations that
 * cannot be a message - creating a view around the host's pixel buffer,
 * resizing it, and "pump this view's frame".
 *
 * Scope (B1): per-view commands and frame pump, plus the three process-level
 * steps the engine loop needs (init/pump/shutdown). Process-global callback
 * registration (macross_set_callback and the open-window/post-message/dialog
 * hooks) and downloads still live in the control: they belong to the local
 * backend and become protocol messages in B2/B3.
 */
#ifndef AGW_VIEW_BACKEND_H
#define AGW_VIEW_BACKEND_H

#include <stddef.h>

#include "macross.h"
#include "AgwViewProtocol.h"

typedef struct AgwViewBackend AgwViewBackend;

struct AgwViewBackend {
    const char* name;

    /* ---- process level (one engine loop per control) ------------------- */
    int  (*processInit)(int width, int height);
    void (*processPump)(void);       /* timers/network of every view block */
    void (*processShutdown)(void);

    /* ---- one view ----------------------------------------------------- */
    /* `owner` is the AgaveWebView this view's callbacks belong to (the local
     * backend hands it to macross_view_create as user_data). Returns the
     * backend-opaque view handle, NULL on failure. */
    void* (*create)(void* owner, MC_BYTE* buffer, int width, int height, int pitch);
    void  (*destroy)(void* view);
    void  (*resize)(void* view, MC_BYTE* buffer, int width, int height, int pitch);

    /* Protocol v1: submit one command, or a batch of them. A batch is one ring
     * slot / one boundary crossing - the measured cost per call is why the
     * protocol is batch-friendly (design §6.2). `text` is borrowed. */
    int  (*submit)(void* view, const AgwViewCommand* command);
    int  (*submitBatch)(void* view, const AgwViewCommand* commands, int count);

    /* Events toward the host, drained by the control's engine loop. Local
     * returns 0: its events arrive through the macross callbacks the control
     * registers once per process (titles, progress, errors, dialogs). A backend
     * with its own instance queues them here instead. */
    int  (*poll)(void* view, AgwViewEvent* out, int maxEvents);

    /* Frame backpack: the host finished compositing bufferId@seq, the view may
     * reuse that buffer. Local paints straight into the caller's buffer, so
     * there is no backpack to acknowledge yet. */
    void (*frameAck)(void* view, const AgwFrameAck* ack);

    /* Frame pump: update = layout + paint + async work; pump = async work only
     * (a hidden view: no layout, no paint - R150). */
    void  (*update)(void* view, const MC_RECT* damage);
    void  (*pump)(void* view);

    /* Watchdog: abort the script this view is running, if any. The local
     * backend can only abort every engine (one engine thread runs them all);
     * a backend with its own instance aborts exactly its own. */
    void  (*interrupt)(void* view);
    void* (*jsContext)(void* view);

    /* The document epoch of the page this view shows (0 when unknown). Local
     * reads it straight from the engine; process/rvvm return the last epoch
     * that came back with an event, which is also what they stamp commands
     * with. Used to refuse executing a command against a document that
     * replaced the one it was issued for. */
    unsigned (*documentEpoch)(void* view);

    /* The latest published canvas, for backends that keep the frame in shared
     * memory instead of the host's buffer (process/rvvm). NULL when there is
     * nothing new to copy - local always returns NULL and paints in place. */
    const MC_BYTE* (*frame)(void* view, int* width, int* height, int* pitch,
                            uint32_t* seq);
};

/* The in-process engine. */
const AgwViewBackend* agwBackendLocal(void);

/* One agave_render_host.exe per view (B2). */
const AgwViewBackend* agwBackendProcess(void);

/* Backend selected by AGB_VIEW_BACKEND (default "local"). Returns NULL for an
 * unknown or not-yet-built name; the caller logs and falls back to local. */
const AgwViewBackend* agwBackendByName(const char* name);

#endif /* AGW_VIEW_BACKEND_H */
