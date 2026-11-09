/*
 * agave_webview.h - embeddable WebView control (C API)
 *
 * The control is a normal child window: create it with a parent HWND and a
 * rectangle, and the host's own message loop drives it. Everything else -
 * the engine thread, the render buffer, input translation - is internal.
 *
 *   AgaveWebView* view = agave_webview_create(hwnd, 0, 0, 800, 600, &cb, NULL);
 *   agave_webview_navigate(view, "https://example.com");
 *   ...
 *   agave_webview_destroy(view);
 *
 * Link with libagave.dll.a, copy libagave.dll and antjsb.dll next to the host
 * executable.
 *
 * Callbacks are invoked on the control's engine thread. Do not call back into
 * the control synchronously from them beyond the functions documented as
 * thread safe (navigate/eval/set_zoom are queued and safe).
 */

#ifndef AGAVE_WEBVIEW_H
#define AGAVE_WEBVIEW_H

/* MC_DOWNLOAD_INFO (download callbacks) and the _mc_error codes. */
#include "macross.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(AGW_BUILD_DLL)
#    define AGW_API __declspec(dllexport)
#  elif defined(AGW_USE_DLL)
#    define AGW_API __declspec(dllimport)
#  else
#    define AGW_API
#  endif
#else
#  define AGW_API
#endif

typedef struct AgaveWebView AgaveWebView;

typedef struct AgaveWebViewCallback {
    /* Page title changed (UTF-8). */
    void (*on_title)(AgaveWebView* view, const char* title, void* user);
    /* Main frame URL changed (UTF-8). */
    void (*on_url)(AgaveWebView* view, const char* url, void* user);
    /* Load progress, 0..100; finished is non-zero when the load completed. */
    void (*on_progress)(AgaveWebView* view, int percent, int finished, void* user);
    /* Status bar text (hovered link etc). */
    void (*on_status)(AgaveWebView* view, const char* text, void* user);
    /* Page asked to open a new window. `new_view` is a freshly created control
     * (already parented to the same host window); the host may reparent or
     * resize it (e.g. into a new tab) and must return non-zero to keep it.
     * Return 0 to block the popup - the control is destroyed for you. */
    int (*on_new_window)(AgaveWebView* view, AgaveWebView* new_view,
                         const char* url, void* user);
    /* Navigation failed. */
    void (*on_error)(AgaveWebView* view, int code, const char* url, void* user);
    /* The engine process for this view died (not a navigation error): code is
     * its exit code, `report` is what the dying process wrote about itself -
     * environment info and a symbol-resolved stack ("" when it left none).
     * Optional - when it is NULL a crash is reported through on_error instead,
     * and either way the view is respawn-ready: the next
     * navigate/reload/load_html brings a fresh renderer up. Hosts show their
     * "page crashed" UI here; the report is what makes that page useful. */
    void (*on_crashed)(AgaveWebView* view, int code, const char* report, void* user);
    /* A download job changed state (started / progress / finished / failed /
     * cancelled). `info` is only valid during the call; copy what you keep.
     * Runs on the UI thread, but the job itself lives in the engine, so it
     * survives the page navigating away. */
    void (*on_download)(AgaveWebView* view, const MC_DOWNLOAD_INFO* info, void* user);
    /* The page called window.agavePostMessage(text). */
    void (*on_message)(AgaveWebView* view, const char* message, void* user);
    /* alert()/confirm()/prompt(). type is 0/1/2. Return non-zero to accept
     * (confirm OK / prompt accepted); for prompt fill `result` with the answer. */
    int (*on_dialog)(AgaveWebView* view, int type, const char* message,
                     const char* default_value, char* result, int result_size,
                     void* user);
    /* Right click: the host decides whether/where to show a menu. Any of the
     * strings may be empty; `x`/`y` are in control coordinates. */
    void (*on_context_menu)(AgaveWebView* view, int x, int y,
                            const char* link_url, const char* image_url,
                            const char* text, void* user);
    /* The page called window.close(); runs on the engine thread, so keep the
     * work short (post to your own thread if you touch GUI state). */
    void (*on_close_window)(AgaveWebView* view, void* user);

    /* A page script has been running with nobody waiting on it for the whole
     * unresponsive budget (AGB_UNRESPONSIVE_MS, 12 s by default). Nothing was
     * killed: this is advisory, the host decides (browsers show an
     * "unresponsive page" prompt offering to wait or close the page). A
     * navigation or a stop request blocked by a script is a different verdict
     * and is still aborted, without this callback.
     *
     * Runs on the UI thread (a dialog belongs here). `busy_ms` is how long the
     * script had been running; `stack_path` is a fresh per-event file holding
     * the live JS stack ("" when no stack could be captured - e.g. the page was
     * busy outside JS). The control also logs the same facts to
     * agb_watchdog.txt. One notice per busy streak. */
    void (*on_unresponsive)(AgaveWebView* view, const char* reason,
                            unsigned int busy_ms, const char* stack_path,
                            void* user);

    /* A view the host already has should be shown now: a page navigated it
     * through a named target (target="foo" / window.open(url,"foo")) and the
     * browser semantics are "that tab comes forward". Fires on the engine
     * thread - post to your UI thread instead of switching widgets inline.
     * Optional; hosts with a single view or no tab strip can ignore it. */
    void (*on_activate_view)(AgaveWebView* view, void* user);

    /* A command the host issued could not be delivered to the page: the view
     * had no page yet, the engine was not in a state to run it, or - the case
     * this exists for - the document it was issued against was replaced by a
     * navigation before the command reached the front of the queue. The command
     * is dropped, never retried and never run against the page that replaced
     * it. Without this callback the same situations were silent: the command
     * simply never happened.
     *
     * `kind` is "eval", "mouse", "key", "text", "search", ... and `reason` is a
     * short English phrase ("document replaced by a navigation", "no live
     * document", "no page yet"). Runs on the engine thread: keep it cheap and
     * do not call back into the control synchronously. */
    void (*on_command_dropped)(AgaveWebView* view, const char* kind,
                               const char* reason, void* user);
} AgaveWebViewCallback;

/* Creates the control as a child of `parent`. Returns NULL on failure. */
AGW_API AgaveWebView* agave_webview_create(void* parent, int x, int y, int width, int height,
                                           const AgaveWebViewCallback* callbacks, void* user);

/* Destroys the control (stops its engine work, frees the buffer). */
AGW_API void agave_webview_destroy(AgaveWebView* view);

/* Creates a popup control as a child of the same host window as `from`, sized
 * like `from`. Used to implement window.open(); hosts may also call it. */
AGW_API AgaveWebView* agave_webview_create_popup(AgaveWebView* from, const char* url);

/* Re-points the control's callbacks at another owner (the `user` pointer
 * passed to the callbacks). A popup inherits it from the view it was opened
 * from; when the host puts the popup in its own tab it calls this with the
 * new tab's state. */
AGW_API void agave_webview_set_user(AgaveWebView* view, void* user);

/* Moves / resizes the control. */
AGW_API void agave_webview_set_rect(AgaveWebView* view, int x, int y, int width, int height);

/* The HWND of the control, for hosts that want to subclass or focus it. */
AGW_API void* agave_webview_hwnd(AgaveWebView* view);

/* Navigation (thread safe, queued). */
AGW_API void agave_webview_navigate(AgaveWebView* view, const char* url);
AGW_API void agave_webview_load_html(AgaveWebView* view, const char* html, const char* base_url);
AGW_API void agave_webview_back(AgaveWebView* view);
AGW_API void agave_webview_forward(AgaveWebView* view);
AGW_API void agave_webview_reload(AgaveWebView* view);
/* Abort the running page script (does not need the command queue, so it works
 * while a script is stuck in a loop). Call agave_webview_reload() afterwards:
 * the aborted page's state is not trustworthy. */
AGW_API void agave_webview_interrupt(AgaveWebView* view);
/* 1 when a queued command has been running for at least minMs (0 = busy at all).
 * Lets the host decide whether Reload means "interrupt, then reload". */
AGW_API int  agave_webview_busy(AgaveWebView* view, int minMs);
AGW_API void agave_webview_stop(AgaveWebView* view);

/* Runs a script in the page (thread safe, queued). */
AGW_API void agave_webview_eval(AgaveWebView* view, const char* script);

/* Zoom: 100 is normal. */
AGW_API void agave_webview_set_zoom(AgaveWebView* view, int percent);
AGW_API int  agave_webview_zoom(AgaveWebView* view);

/* Page Visibility (tab switch, minimize): sets document.hidden /
 * document.visibilityState and fires `visibilitychange`. Queued like the other
 * commands; call it whenever the host shows/hides a tab. */
AGW_API void agave_webview_set_visible(AgaveWebView* view, int visible);

/* Last known state (valid on any thread, copied under lock). */
AGW_API const char* agave_webview_url(AgaveWebView* view);
AGW_API const char* agave_webview_title(AgaveWebView* view);

/* Paints the last rendered frame into `hdc`, scaled to width x height. The
 * control keeps its own mirror buffer, so this works while the window is
 * hidden: it is the headless screenshot path (PrintWindow needs a window that
 * gets WM_PAINT, which a hidden one never does). */
AGW_API int agave_webview_paint_to(AgaveWebView* view, void* hdc, int width, int height);

/* Focus handling, mostly for hosts with several controls. */
AGW_API void agave_webview_set_focus(AgaveWebView* view);

/* Host -> page: calls window.onAgaveMessage(text) in the page (if defined). */
AGW_API void agave_webview_post_message(AgaveWebView* view, const char* message);

/* Find in page (queued, fire-and-forget): highlights and scrolls to the next
 * match. Returns non-zero when the request was queued. */
AGW_API int agave_webview_search(AgaveWebView* view, const char* text,
                                 int forward, int case_sensitive);

/* History availability, driven by the engine's history status callback. */
AGW_API int agave_webview_can_back(AgaveWebView* view);
AGW_API int agave_webview_can_forward(AgaveWebView* view);

/* Per-control overrides (thread safe, queued). */
AGW_API void agave_webview_set_user_agent(AgaveWebView* view, const char* user_agent);
AGW_API void agave_webview_set_download_dir(AgaveWebView* view, const char* directory);
/* Cancel a download job by the id on_download reported (thread safe, queued). */
AGW_API void agave_webview_cancel_download(AgaveWebView* view, int id);

/* Process-wide net log / debug switches (optional, call before create). */
AGW_API void agave_webview_set_log(const char* path_or_null);

#ifdef __cplusplus
}
#endif

#endif /* AGAVE_WEBVIEW_H */
