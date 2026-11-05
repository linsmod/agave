/*
 * ant_pin_repro.c - does antb_retain() actually root a JS function? (R67)
 *
 * The bridge keeps long-lived JS callbacks as AntbValue handles and roots them
 * with gc_pin_permanent(). That call only accepts GC_OBJ_TYPE_MASK types
 * (object/error/array/promise/generator): a function value is rejected and the
 * failure is ignored, so an event listener held only by the C++ side gets
 * collected and the next antb_call() reads a freed closure/object.
 *
 * This program proves it without the browser: create an engine, take a
 * function that nothing else references, retain it, force GC, call it.
 *
 * Build (from the agave repo, after tools/build_antjsb.ps1):
 *   zig cc -target x86_64-windows-gnu -O1 -g -I 3rdparty/ant/include ^
 *     tools/ant_pin_repro.c 3rdparty/ant/lib/antjsb.dll.a ^
 *     -o build-zig-native/ant_pin_repro.exe
 *
 * Run (antjsb.dll sits next to the exe):
 *   build-zig-native/ant_pin_repro.exe
 *
 * Exit 0 = the retained function still ran. Crash / non-zero = not rooted.
 * ANT_PIN_KIND=object keeps an object instead (control: that path is rooted).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AntBridge.h"

static void on_log(const char *line, void *user)
{
    (void)user;
    fprintf(stderr, "[ant] %s\n", line);
}

int main(void)
{
    int anchor = 0;
    const char *kind = getenv("ANT_PIN_KIND");
    int want_object = kind && !strcmp(kind, "object");
    const char *src = want_object
        ? "({ answer: function () { return 42; } })"
        : "(function () { var q = 42; return function () { return q; }; })()";

    antb_set_logger(on_log, NULL);

    AntbEngine *engine = antb_engine_create(&anchor);

    if (!engine) {
        fprintf(stderr, "engine_create failed\n");
        return 2;
    }

    char *err = NULL;
    AntbValue *v = antb_eval(engine, src, (int)strlen(src), &err);

    if (!v) {
        fprintf(stderr, "eval failed: %s\n", err ? err : "(none)");
        return 3;
    }

    /* Keep it only from C: no global, no object property, nothing else. */
    AntbValue *held = antb_retain(v);

    antb_value_release(v);

    /* Churn so a nursery + major collection both certainly happen. */
    for (int i = 0; i < 8; i++) {
        char *gerr = NULL;
        AntbValue *g = antb_eval(engine,
                                 "var junk = []; for (var i = 0; i < 2000; i++) "
                                 "junk.push({ k: i, s: 'x' + i }); junk.length",
                                 -1, &gerr);

        if (g)
            antb_value_release(g);

        antb_gc(engine);
    }

    AntbValue *r = antb_call(engine, held, NULL, NULL, 0, &err);
    char *text = r ? antb_to_utf8(r) : NULL;

    fprintf(stderr, "result: %s\n", text ? text : (err ? err : "(null)"));

    if (text)
        antb_free(text);

    antb_engine_destroy(engine);
    return r ? 0 : 4;
}
