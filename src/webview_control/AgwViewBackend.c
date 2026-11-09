/* AgwViewBackend.c - the `local` render backend: the engine in this process,
 * driven by the macross_* API exactly as the control used to call it before the
 * seam existed.
 *
 * Every op is one command of the render-backend protocol in
 * docs/design-render-backend.md, so the process and RVVM backends of that
 * design can be dropped in without touching the control. Nothing here adds
 * behavior: the wrappers are the previous call sites, moved.
 */
#include "AgwViewBackend.h"

#include <stdio.h>
#include <string.h>

#include "AntBridge.h"   /* antb_interrupt_all_engines: the local watchdog path */
#include "../webview/AgbLog.h"

static int localProcessInit(int width, int height)
{
    return macross_initialize(PIXEL_FORMAT_BGRA32,
                              width > 0 ? width : 1024,
                              height > 0 ? height : 768) == MC_STATUS_SUCCESS;
}

static void localProcessPump(void)
{
    macross_event_dispatch();
}

static void localProcessShutdown(void)
{
    macross_shutdown();
}

static void* localCreate(void* owner, MC_BYTE* buffer, int width, int height, int pitch)
{
    return macross_view_create(buffer, width, height, pitch, owner);
}

static void localDestroy(void* view)
{
    if (view)
        macross_view_destroy((MaCrossView*)view);
}

static void localResize(void* view, MC_BYTE* buffer, int width, int height, int pitch)
{
    if (view)
        macross_view_resize((MaCrossView*)view, buffer, width, height, pitch);
}

static void localNavigate(void* view, const char* url)
{
    if (view && url)
        macross_view_open_url((MaCrossView*)view, url);
}

static void localLoadHtml(void* view, const char* html, const char* baseUrl)
{
    if (view && html)
        macross_view_load_html((MaCrossView*)view, html, baseUrl ? baseUrl : "");
}

static void localReload(void* view)
{
    if (view)
        macross_view_reload((MaCrossView*)view);
}

static void localStop(void* view)
{
    if (view)
        macross_view_stop((MaCrossView*)view);
}

static void localBack(void* view)
{
    if (view)
        macross_view_backward((MaCrossView*)view);
}

static void localForward(void* view)
{
    if (view)
        macross_view_forward((MaCrossView*)view);
}

static void localEval(void* view, const char* script)
{
    if (view && script)
        macross_view_eval_script((MaCrossView*)view, script);
}

static void localSetVisible(void* view, int visible)
{
    if (view)
        macross_view_set_visible((MaCrossView*)view, visible ? 1 : 0);
}

static void localSetFocus(void* view)
{
    if (view)
        macross_view_set_focus((MaCrossView*)view);
}

static void localSetScaleFactor(void* view, int percent)
{
    if (view)
        macross_view_set_scale_factor((MaCrossView*)view, percent);
}

static void localMouseEvent(void* view, const MC_MOUSE_EVENT* event)
{
    if (view && event)
        macross_mouse_event((MaCrossView*)view, event);
}

static void localKeyEvent(void* view, const MC_KEY_EVENT* event)
{
    if (view && event)
        macross_keyboard_event((MaCrossView*)view, event);
}

static void localInputText(void* view, const char* utf8)
{
    if (view && utf8)
        macross_view_input_text((MaCrossView*)view, utf8);
}

static void localSearchText(void* view, const char* utf8, int forward, int caseSensitive)
{
    if (view && utf8 && utf8[0])
        macross_view_search_text((MaCrossView*)view, utf8,
                                 forward ? 1 : 0, caseSensitive ? 1 : 0, 1);
}

static void localContextMenu(void* view, const MC_CONTEXT_EVENT* event)
{
    if (view && event)
        macross_context_menu_event((MaCrossView*)view, event);
}

static void localUpdate(void* view, const MC_RECT* damage)
{
    if (view)
        macross_view_update((MaCrossView*)view, damage);
}

static void localPump(void* view)
{
    if (view)
        macross_view_pump((MaCrossView*)view);
}

static void localInterrupt(void* view)
{
    /* One engine thread runs every view: the abort is process wide until a
     * backend owns its own instance (B2/B3 in the design). */
    (void)view;
    antb_interrupt_all_engines();
}

static void* localJsContext(void* view)
{
    return view ? macross_view_js_context((MaCrossView*)view) : NULL;
}

static const AgwViewBackend g_localBackend = {
    "local",
    localProcessInit,
    localProcessPump,
    localProcessShutdown,
    localCreate,
    localDestroy,
    localResize,
    localNavigate,
    localLoadHtml,
    localReload,
    localStop,
    localBack,
    localForward,
    localEval,
    localSetVisible,
    localSetFocus,
    localSetScaleFactor,
    localMouseEvent,
    localKeyEvent,
    localInputText,
    localSearchText,
    localContextMenu,
    localUpdate,
    localPump,
    localInterrupt,
    localJsContext
};

const AgwViewBackend* agwBackendLocal(void)
{
    return &g_localBackend;
}

const AgwViewBackend* agwBackendByName(const char* name)
{
    if (!name || !name[0] || strcmp(name, "local") == 0)
        return &g_localBackend;

    /* "process" (B2) and "rvvm" (B3) are in the design; until they land, asking
     * for one must be loud and must not silently run something else. Logged
     * once: every view and the engine loop resolve the name. */
    {
        static int logged = 0;

        if (!logged) {
            logged = 1;
            agbLog(AGB_LOG_SHELL,
                   "AGB_VIEW_BACKEND=%s is not built yet (design-render-backend.md B2/B3); using local", name);
        }
    }

    return NULL;
}
