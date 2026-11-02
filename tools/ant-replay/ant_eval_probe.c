/*
 * ant_eval_probe.c - does a script evaluation leave something behind that makes
 * the next C -> JS call run its body more than once?
 *
 * Background: in agave, a page with N slow async <script>s runs its onload
 * handler N+1 times per dispatch (0 scripts -> 1, 1 -> 2, 2 -> 3, 3 -> 4),
 * while the dispatch itself happens once. This program checks whether plain
 * antb_eval calls - which is how agave runs every page script - are enough to
 * reproduce that, with no browser around.
 *
 * Build with tools/build_ant_replay.ps1 (it builds every .c in tools/ant-replay).
 */

#include "AntBridge.h"
#include <stdio.h>
#include <string.h>

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

static void callCb(AntbEngine* e)
{
    AntbValue* global = antb_global(e);
    AntbValue* fn = antb_get_prop(e, global, "cb");
    AntbValue* arg = NULL;

    if (fn)
        antb_call(e, fn, global, &arg, 0, NULL);

    antb_value_release(fn);
    antb_value_release(global);
}

/* the exact wrapper AntRuntime::evaluate uses for page scripts */
static void evalLikePageScript(AntbEngine* e, const char* body)
{
    char buf[512];

    snprintf(buf, sizeof(buf),
             "try{\n%s\n}catch(__agbE){}\n", body);
    antb_eval(e, buf, (int)strlen(buf), NULL);
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

    antb_eval(e, page, (int)strlen(page), NULL);

    resetRuns(e);
    callCb(e);
    printf("baseline (no extra eval)          runs=%.0f\n", runsOf(e));

    for (int n = 1; n <= 3; ++n) {
        resetRuns(e);

        for (int i = 0; i < n; ++i)
            evalLikePageScript(e, "window.__slowScripts = (window.__slowScripts || 0) + 1;");

        callCb(e);
        printf("%d page-style eval(s) then call      runs=%.0f\n", n, runsOf(e));
    }

    antb_engine_destroy(e);
    return 0;
}
