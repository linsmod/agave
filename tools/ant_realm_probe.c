/*
 * ant_realm_probe.c - does one isolate give each realm its own global? (S0)
 *
 * This is the foundation of the per-frame realm work (agave
 * docs/design-per-frame-realm.md): agave wants one engine per view and one
 * realm per document/frame. Before touching the browser, prove on the bare
 * bridge that a realm really is an independent global with its own intrinsics.
 *
 * Build (from the agave repo, after tools/build_antjsb.ps1):
 *   zig cc -target x86_64-windows-gnu -O1 -g -I 3rdparty/ant/include ^
 *     tools/ant_realm_probe.c 3rdparty/ant/lib/antjsb.dll.a ^
 *     -o build-zig-native/ant_realm_probe.exe
 *
 * Run (antjsb.dll next to the exe): build-zig-native/ant_realm_probe.exe
 * Exit 0 = all realm invariants hold.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AntBridge.h"

static int failures;

static void check(int ok, const char* what)
{
    if (ok) {
        fprintf(stderr, "PASS  %s\n", what);
    } else {
        fprintf(stderr, "FAIL  %s\n", what);
        failures++;
    }
}

/* Evaluate in the current realm and return the value as text (or "<err>").
 * antb_eval (direct-eval completion value), not antb_eval_script: a program's
 * completion value is undefined, which would make every probe look empty. */
static char* evalText(AntbEngine* engine, const char* src)
{
    char* err = NULL;
    AntbValue* v = antb_eval(engine, src, (int)strlen(src), &err);
    char* text;

    if (!v) {
        text = (char*)malloc(256);
        snprintf(text, 256, "<err:%s>", err ? err : "?");
        if (err) antb_free(err);
        return text;
    }

    text = antb_to_utf8(v);
    antb_value_release(v);
    return text ? text : strdup("<null>");
}

static void expect(AntbEngine* engine, const char* src, const char* want, const char* what)
{
    char* got = evalText(engine, src);
    int ok = got && strcmp(got, want) == 0;

    check(ok, what);
    if (!ok)
        fprintf(stderr, "      %s -> '%s' (wanted '%s')\n", src, got ? got : "?", want);
    if (got) antb_free(got);
}

int main(void)
{
    int anchor = 0;
    AntbEngine* engine = antb_engine_create(&anchor);
    AntbRealm* mainRealm;
    AntbRealm* realmA;
    AntbRealm* realmB;
    AntbValue* mainArray;
    AntbValue* aArray;
    AntbValue* bArray;

    if (!engine) {
        fprintf(stderr, "engine_create failed\n");
        return 2;
    }

    mainRealm = antb_realm_current(engine);
    check(mainRealm != NULL, "engine starts in a realm");

    /* Main realm: a global var and the Array intrinsic. */
    expect(engine, "globalThis.tag = 'main'; var mainVar = 1; typeof mainVar", "number", "main realm var");
    mainArray = antb_eval(engine, "Array", 5, NULL);
    check(mainArray != NULL, "main realm Array");

    realmA = antb_realm_create(engine);
    check(realmA != NULL, "create realm A");

    antb_realm_enter(engine, realmA);
    expect(engine, "typeof tag", "undefined", "realm A does not see main's global");
    expect(engine, "typeof mainVar", "undefined", "realm A does not see main's var");
    expect(engine, "var aVar = 2; aVar", "2", "realm A has its own var");
    expect(engine, "this === globalThis", "true", "realm A globalThis works");
    aArray = antb_eval(engine, "Array", 5, NULL);
    check(aArray != NULL, "realm A Array");

    realmB = antb_realm_create(engine);
    check(realmB != NULL, "create realm B");

    antb_realm_enter(engine, realmB);
    expect(engine, "typeof tag", "undefined", "realm B does not see main's global");
    expect(engine, "typeof aVar", "undefined", "realm B does not see realm A's var");
    bArray = antb_eval(engine, "Array", 5, NULL);
    check(bArray != NULL, "realm B Array");

    check(!antb_value_identical(mainArray, aArray), "Array differs between main and A");
    check(!antb_value_identical(aArray, bArray), "Array differs between A and B");

    /* Back to the main realm: A/B bindings must be gone. */
    antb_realm_enter(engine, mainRealm);
    expect(engine, "typeof aVar", "undefined", "main does not see realm A's var");
    expect(engine, "tag", "main", "main global still intact");

    /* Cross-realm calls work because it is one VM: give A a function, call it
       from B through a value we pass explicitly (no proxy needed here). */
    antb_realm_enter(engine, realmA);
    expect(engine, "globalThis.hello = function (x) { return 'hi:' + x; }; typeof hello",
           "function", "realm A function");
    antb_realm_enter(engine, realmB);
    expect(engine, "typeof hello", "undefined", "realm B does not see A's function");

    antb_realm_enter(engine, mainRealm);
    antb_realm_collect(engine);
    check(antb_realm_current(engine) == mainRealm, "collect keeps the current realm");

    if (mainArray) antb_value_release(mainArray);
    if (aArray) antb_value_release(aArray);
    if (bArray) antb_value_release(bArray);

    antb_engine_destroy(engine);

    if (failures) {
        fprintf(stderr, "%d realm check(s) failed\n", failures);
        return 1;
    }

    fprintf(stderr, "ALL REALM CHECKS PASSED\n");
    return 0;
}
