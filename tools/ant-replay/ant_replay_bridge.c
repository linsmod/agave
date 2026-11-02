/*
 * ant_replay_bridge.c - same question as ant_replay.c, but through agave's own
 * bridge (antbridge.dll): antb_engine_create + antb_eval + antb_pump +
 * antb_set_native + antb_call, i.e. exactly how AntRuntime drives Ant.
 *
 * If a case here prints runs>1 while ant_replay.c prints 1, the trigger is in
 * the bridge/engine setup (prelude, per-frame pump, repeated document binding)
 * and not in Ant itself.
 *
 * Build with tools/build_ant_replay.ps1 (it builds both programs).
 */

#include "AntBridge.h"
#include <stdio.h>
#include <string.h>

#define AGB_TAG_NODE 0x4E4F4431u   /* 'NOD1', same tag AntRuntime uses */

static double runsOf(AntbEngine* e)
{
    AntbValue* global = antb_global(e);
    AntbValue* v = antb_get_prop(e, global, "runs");
    double n = v ? antb_to_number(v) : -1.0;

    antb_value_release(v);
    antb_value_release(global);
    return n;
}

static void resetRuns(AntbEngine* e)
{
    antb_eval(e, "globalThis.runs = 0;", 20, NULL);
}

static AntbValue* globalFn(AntbEngine* e, const char* name)
{
    AntbValue* global = antb_global(e);
    AntbValue* fn = antb_get_prop(e, global, name);

    antb_value_release(global);
    return fn;
}

static void callOnce(AntbEngine* e, const char* name)
{
    AntbValue* fn = globalFn(e, name);
    AntbValue* self = antb_global(e);
    AntbValue* arg = NULL;

    if (fn)
        antb_call(e, fn, self, &arg, 0, NULL);

    antb_value_release(self);
    antb_value_release(fn);
}

int main(void)
{
    volatile char stack_base;
    AntbEngine* e = antb_engine_create((const void*)&stack_base);
    const char* page =
        "globalThis.runs = 0;"
        "globalThis.cb = function () { globalThis.runs++; };";

    if (!e) {
        fprintf(stderr, "antb_engine_create failed\n");
        return 1;
    }

    /* 1. plain eval + call, like ant_replay.c */
    antb_eval(e, page, (int)strlen(page), NULL);
    resetRuns(e);
    callOnce(e, "cb");
    printf("case 1  bridge: eval + call                 runs=%.0f\n", runsOf(e));

    /* 2. after pumping the event loop a few times (agave pumps every frame) */
    resetRuns(e);
    for (int i = 0; i < 10; ++i)
        antb_pump(e);
    callOnce(e, "cb");
    printf("case 2  bridge: after 10 pumps              runs=%.0f\n", runsOf(e));

    /* 3. after re-pointing the global's native pointer, like a document rebind */
    resetRuns(e);
    {
        AntbValue* global = antb_global(e);

        for (int i = 0; i < 3; ++i)
            antb_set_native(global, (void*)(size_t)(0x1000 + i), AGB_TAG_NODE);
        antb_value_release(global);
    }
    callOnce(e, "cb");
    printf("case 3  bridge: after 3 native rebinds      runs=%.0f\n", runsOf(e));

    /* 4. after evaluating a prelude-like dispatcher and assigning onload */
    resetRuns(e);
    {
        const char* prelude =
            "(function (g) {"
            "  var slot = null, installed = false;"
            "  Object.defineProperty(g, 'onload', {"
            "    configurable: true, enumerable: true,"
            "    get: function () { return slot; },"
            "    set: function (fn) {"
            "      slot = fn;"
            "      if (!installed && typeof fn === 'function') {"
            "        installed = true;"
            "        g.addEventListener('load', function (ev) {"
            "          if (typeof slot === 'function') { slot.call(g, ev); }"
            "        });"
            "      }"
            "    }"
            "  });"
            "})(this);"
            "globalThis.onload = function () { globalThis.runs++; };";

        antb_eval(e, prelude, (int)strlen(prelude), NULL);
    }
    /* the bridge has no DOM; call the stored handler the way the dispatcher would */
    {
        AntbValue* global = antb_global(e);
        AntbValue* fn = antb_get_prop(e, global, "onload");

        if (fn) {
            AntbValue* arg = NULL;
            antb_call(e, fn, global, &arg, 0, NULL);
            antb_value_release(fn);
        }
        antb_value_release(global);
    }
    printf("case 4  bridge: prelude dispatcher + onload runs=%.0f\n", runsOf(e));

    antb_engine_destroy(e);
    return 0;
}
