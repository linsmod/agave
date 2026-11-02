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
 * Link with agave_webview.lib + libagave.dll.a, copy libagave.dll and
 * antbridge.dll next to the host executable.
 *
 * Callbacks are invoked on the control's engine thread. Do not call back into
 * the control synchronously from them beyond the functions documented as
 * thread safe (navigate/eval/set_zoom are queued and safe).
 */

#ifndef AGAVE_WEBVIEW_H
#define AGAVE_WEBVIEW_H

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
    /* A download finished, path is the file written by the control. */
    void (*on_download)(AgaveWebView* view, const char* url, const char* path, void* user);
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
} AgaveWebViewCallback;

/* Creates the control as a child of `parent`. Returns NULL on failure. */
AGW_API AgaveWebView* agave_webview_create(void* parent, int x, int y, int width, int height,
                                           const AgaveWebViewCallback* callbacks, void* user);

/* Destroys the control (stops its engine work, frees the buffer). */
AGW_API void agave_webview_destroy(AgaveWebView* view);

/* Creates a popup control as a child of the same host window as `from`, sized
 * like `from`. Used to implement window.open(); hosts may also call it. */
AGW_API AgaveWebView* agave_webview_create_popup(AgaveWebView* from, const char* url);

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
AGW_API void agave_webview_stop(AgaveWebView* view);

/* Runs a script in the page (thread safe, queued). */
AGW_API void agave_webview_eval(AgaveWebView* view, const char* script);

/* Zoom: 100 is normal. */
AGW_API void agave_webview_set_zoom(AgaveWebView* view, int percent);
AGW_API int  agave_webview_zoom(AgaveWebView* view);

/* Last known state (valid on any thread, copied under lock). */
AGW_API const char* agave_webview_url(AgaveWebView* view);
AGW_API const char* agave_webview_title(AgaveWebView* view);

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

/* Process-wide net log / debug switches (optional, call before create). */
AGW_API void agave_webview_set_log(const char* path_or_null);

#ifdef __cplusplus
}
#endif

#endif /* AGAVE_WEBVIEW_H */
