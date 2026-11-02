/*
 * ant_replay.c - standalone proof (no agave, no DOM): does one call from C into
 * JS execute the JavaScript body more than once?
 *
 * Build:
 *   zig cc  -target x86_64-windows-gnu -O1 -std=gnu23 -I <ant>/include \
 *           -I <ant>/vendor/uthash-2.3.0/src -I <ant>/vendor/libuv-v1.53.0/include \
 *           -c ant_replay.c -o ant_replay.obj
 *   zig c++ -target x86_64-windows-gnu -o ant_replay.exe ant_replay.obj \
 *           <ant>/build-zig/libant.a <vendor *.a> <system libs>
 */

#include <ant.h>
#include <cfunc.h>
#include <stdio.h>
#include <string.h>

static int jsCounter(ant_t* js)
{
    ant_value_t global = js_glob(js);
    ant_value_t v = js_get(js, global, "runs");

    return (vtype(v) == kTypeNumber) ? (int)js_getnum(v) : -1;
}

static void resetCounter(ant_t* js)
{
    ant_value_t global = js_glob(js);

    js_set(js, global, "runs", js_mknum(0));
}

static ant_value_t getGlobalFunction(ant_t* js, const char* name)
{
    ant_value_t global = js_glob(js);

    return js_get(js, global, name);
}

/* nativeCall(fn): call the JS function back from C, exactly like the browser
 * bridge does when it dispatches a DOM event. */
static ant_value_t nativeCall(ant_params_t)
{
    ant_value_t fn;
    ant_value_t arg = js_mkundef();

    if (nargs < 1)
        return js_mkerr(js, "nativeCall(fn) needs a function");

    fn = args[0];
    js_call_value(js, fn, js_glob(js), &arg, 0);

    return js_mkundef();
}

int main(void)
{
    volatile char stack_base;
    static char* argv[] = { "ant_replay", NULL };
    ant_t* js;
    ant_value_t global;
    const char* setup =
        "globalThis.runs = 0;"
        "globalThis.cb = function () { globalThis.runs++; };"
        "globalThis.cbArg = function (e) { globalThis.runs++; };"
        "globalThis.makeCounter = function () {"
        "  return function (e) { globalThis.runs++; };"
        "};"
        "globalThis.made = makeCounter();"
        "globalThis.outer = function () { nativeCall(cb); };"
        "globalThis.outer2 = function () { nativeCall(outer); };";

    js = ant_create(NULL);
    if (!js) {
        fprintf(stderr, "ant_create failed\n");
        return 1;
    }
    js_setstackbase(js, (void*)&stack_base);
    ant_runtime_init(js, 1, argv, NULL);

    global = js_glob(js);
    js_set(js, global, "nativeCall", Ant_Function_Create(nativeCall));

    {
        ant_value_t r = js_eval_bytecode_eval(js, setup, strlen(setup));

        if (vtype(r) == kTypeError) {
            fprintf(stderr, "eval failed: %s\n", js_str(js, r));
            return 1;
        }
    }

    /* 1. plain C -> JS, no JS frames on the stack, no arguments */
    resetCounter(js);
    {
        ant_value_t fn = getGlobalFunction(js, "cb");
        ant_value_t arg = js_mkundef();

        js_call_value(js, fn, js_glob(js), &arg, 0);
        printf("case 1  C -> cb()                    runs=%d\n", jsCounter(js));
    }

    /* 2. one argument, like the browser passing the event object */
    resetCounter(js);
    {
        ant_value_t fn = getGlobalFunction(js, "cbArg");
        ant_value_t arg = js_mknum(1);

        js_call_value(js, fn, js_glob(js), &arg, 1);
        printf("case 2  C -> cbArg(event)            runs=%d\n", jsCounter(js));
    }

    /* 3. closure produced by a factory (like the prelude's dispatcher) */
    resetCounter(js);
    {
        ant_value_t fn = getGlobalFunction(js, "made");
        ant_value_t arg = js_mknum(1);

        js_call_value(js, fn, js_glob(js), &arg, 1);
        printf("case 3  C -> factory closure         runs=%d\n", jsCounter(js));
    }

    /* 4. C -> JS closure that calls a native, which calls cb back */
    resetCounter(js);
    {
        ant_value_t fn = getGlobalFunction(js, "outer");
        ant_value_t arg = js_mkundef();

        js_call_value(js, fn, js_glob(js), &arg, 0);
        printf("case 4  C -> outer -> native -> cb   runs=%d\n", jsCounter(js));
    }

    /* 5. one more nesting level */
    resetCounter(js);
    {
        ant_value_t fn = getGlobalFunction(js, "outer2");
        ant_value_t arg = js_mkundef();

        js_call_value(js, fn, js_glob(js), &arg, 0);
        printf("case 5  C -> outer2 -> ... -> cb     runs=%d\n", jsCounter(js));
    }

    /* 6. same nesting, started from JS only */
    resetCounter(js);
    {
        const char* code = "outer();";

        js_eval_bytecode_eval(js, code, strlen(code));
        printf("case 6  JS -> outer -> native -> cb  runs=%d\n", jsCounter(js));
    }

    js_destroy(js);
    return 0;
}
