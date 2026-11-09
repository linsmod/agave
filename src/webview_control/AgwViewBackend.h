/* AgwViewBackend.h - the seam between the control and whatever runs a page.
 *
 * Design: docs/design-render-backend.md (S3). v1 ships exactly one backend,
 * `local`: the engine in this process, driven by macross_* - i.e. the calls the
 * control used to make inline. The process and RVVM backends implement the same
 * ops and are selected with AGB_VIEW_BACKEND; every op below is a command (or a
 * frame pump step) of the render-backend protocol v1 there, so a new backend
 * never needs to touch the shell or agave_webview.h.
 *
 * Scope of this interface (B0): per-view lifecycle/frame/input/navigation, plus
 * the three process-level steps the engine loop needs (init/pump/shutdown).
 * Process-global callback registration (macross_set_callback and the
 * open-window/post-message/dialog hooks), downloads and the probe hooks still
 * live in the control: they belong to the local backend and become messages in
 * the protocol (B2/B3), not calls.
 */
#ifndef AGW_VIEW_BACKEND_H
#define AGW_VIEW_BACKEND_H

#include <stddef.h>

#include "macross.h"

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

    /* Navigation / control */
    void  (*navigate)(void* view, const char* url);
    void  (*loadHtml)(void* view, const char* html, const char* baseUrl);
    void  (*reload)(void* view);
    void  (*stop)(void* view);
    void  (*back)(void* view);
    void  (*forward)(void* view);
    void  (*eval)(void* view, const char* script);

    /* View state */
    void  (*setVisible)(void* view, int visible);
    void  (*setFocus)(void* view);
    void  (*setScaleFactor)(void* view, int percent);

    /* Input */
    void  (*mouseEvent)(void* view, const MC_MOUSE_EVENT* event);
    void  (*keyEvent)(void* view, const MC_KEY_EVENT* event);
    void  (*inputText)(void* view, const char* utf8);
    void  (*searchText)(void* view, const char* utf8, int forward, int caseSensitive);
    void  (*contextMenu)(void* view, const MC_CONTEXT_EVENT* event);

    /* Frame pump: update = layout + paint + async work; pump = async work only
     * (a hidden view: no layout, no paint - R150). */
    void  (*update)(void* view, const MC_RECT* damage);
    void  (*pump)(void* view);

    /* Watchdog: abort the script this view is running, if any. The local
     * backend can only abort every engine (one engine thread runs them all);
     * a backend with its own instance aborts exactly its own. */
    void  (*interrupt)(void* view);
    void* (*jsContext)(void* view);
};

/* The only backend that exists today: the in-process engine. */
const AgwViewBackend* agwBackendLocal(void);

/* Backend selected by AGB_VIEW_BACKEND (default "local"). Returns NULL for an
 * unknown or not-yet-built name; the caller logs and falls back to local. */
const AgwViewBackend* agwBackendByName(const char* name);

#endif /* AGW_VIEW_BACKEND_H */
