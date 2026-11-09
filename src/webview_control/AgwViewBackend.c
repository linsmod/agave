/* AgwViewBackend.c - the `local` render backend: the engine in this process,
 * driven by the macross_* API. Every command of the render-backend protocol
 * (AgwViewProtocol.h, docs/design-render-backend.md §4) maps back onto the
 * macross call the control used to make inline, so the control talks protocol
 * even when the engine is in the same address space.
 *
 * Events are the one asymmetry: local delivers them through the macross
 * callbacks the control registers once per process (titles, progress, errors,
 * dialogs), so poll() has nothing to hand out. A backend with its own instance
 * queues events instead.
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

/* One protocol command, executed as the macross call it stands for. */
static int localSubmit(void* view, const AgwViewCommand* cmd)
{
    MaCrossView* mcv = (MaCrossView*)view;
    const char* text = cmd ? cmd->text : NULL;

    if (!mcv || !cmd)
        return 0;

    switch (cmd->kind) {
    case AGW_VIEW_CMD_VISIBLE:
        macross_view_set_visible(mcv, cmd->a ? 1 : 0);
        return 1;
    case AGW_VIEW_CMD_FOCUS:
        macross_view_set_focus(mcv);
        return 1;
    case AGW_VIEW_CMD_MOUSE: {
        MC_MOUSE_EVENT event;

        memset(&event, 0, sizeof(event));
        event.type = (MC_MOUSE_EVENT_TYPE)cmd->a;
        event.button = (MC_MOUSE_BUTTON)cmd->b;
        event.point.x = cmd->c;
        event.point.y = cmd->d;
        macross_mouse_event(mcv, &event);
        return 1;
    }
    case AGW_VIEW_CMD_KEY: {
        MC_KEY_EVENT event;

        memset(&event, 0, sizeof(event));
        event.type = (MC_KEY_EVENT_TYPE)cmd->a;
        event.key = (MC_VIRTUAL_KEY)cmd->c;
        event.modifier = (unsigned int)cmd->b;
        macross_keyboard_event(mcv, &event);
        return 1;
    }
    case AGW_VIEW_CMD_TEXT:
        if (text)
            macross_view_input_text(mcv, text);
        return 1;
    case AGW_VIEW_CMD_PREEDIT:
        /* agave has no composition rendering yet; the commit path
         * (AGW_VIEW_CMD_TEXT) is what carries IME output today. */
        return 1;
    case AGW_VIEW_CMD_NAVIGATE:
        if (text)
            macross_view_open_url(mcv, text);
        return 1;
    case AGW_VIEW_CMD_LOAD_HTML:
        if (text)
            macross_view_load_html(mcv, text, "");
        return 1;
    case AGW_VIEW_CMD_RELOAD:
        macross_view_reload(mcv);
        return 1;
    case AGW_VIEW_CMD_STOP:
        macross_view_stop(mcv);
        return 1;
    case AGW_VIEW_CMD_BACK:
        macross_view_backward(mcv);
        return 1;
    case AGW_VIEW_CMD_FORWARD:
        macross_view_forward(mcv);
        return 1;
    case AGW_VIEW_CMD_EVAL:
        if (text)
            macross_view_eval_script(mcv, text);
        return 1;
    case AGW_VIEW_CMD_ZOOM:
        macross_view_set_scale_factor(mcv, cmd->a > 0 ? cmd->a : 100);
        return 1;
    case AGW_VIEW_CMD_SEARCH:
        if (text && text[0])
            macross_view_search_text(mcv, text,
                                     cmd->a ? 1 : 0, cmd->b ? 1 : 0, 1);
        return 1;
    case AGW_VIEW_CMD_CONTEXT_MENU: {
        MC_CONTEXT_EVENT event;

        memset(&event, 0, sizeof(event));
        event.point.x = cmd->c;
        event.point.y = cmd->d;
        event.modifier = (unsigned int)cmd->a;
        macross_context_menu_event(mcv, &event);
        return 1;
    }
    case AGW_VIEW_CMD_SERVICE_REPLY:
        /* Reserved for B4 (service request/reply). */
        return 0;
    default:
        return 0;
    }
}

static int localSubmitBatch(void* view, const AgwViewCommand* commands, int count)
{
    int i;

    for (i = 0; i < count; ++i) {
        if (!localSubmit(view, &commands[i]))
            return i;
    }

    return count;
}

static int localPoll(void* view, AgwViewEvent* out, int maxEvents)
{
    (void)view;
    (void)out;
    (void)maxEvents;
    return 0;   /* callback-driven; see the file comment */
}

static void localFrameAck(void* view, const AgwFrameAck* ack)
{
    (void)view;
    (void)ack;  /* local paints into the host's own buffer */
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
    localSubmit,
    localSubmitBatch,
    localPoll,
    localFrameAck,
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
