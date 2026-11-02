/*
 * AntBridge.c - the only translation unit that includes Ant's GNU C23 headers.
 * Everything above this file (agave's C++) sees AntBridge.h only.
 */

#include "AntBridge.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ant.h"
#include "internal.h"
#include "cfunc.h"     /* js_mkfun_dyn */
#include "reactor.h"   /* ant_uv_run, js_poll_events */
#include "runtime.h"
#include "bootstrap.h"
#include "ptr.h"        /* js_set_native / js_get_native (static inline) */
#include "gc/roots.h"   /* gc_register_root: keep wrapper objects alive */
#include "gc.h"         /* gc_run: forced collection in tests */

struct AntbEngine {
    ant_t* js;
    char   err[512];
};

struct AntbValue {
    ant_t*      js;
    ant_value_t v;
};

/* ------------------------------------------------------------------------- */

static void copy_cstr(AntbEngine* e, const char* what, ant_value_t v)
{
    char nbuf[128];
    char mbuf[384];
    js_cstr_t name;
    js_cstr_t msg;
    ant_value_t nv = js_get(e->js, v, "name");
    ant_value_t mv = js_get(e->js, v, "message");

    if (vtype(nv) == kTypeString && vtype(mv) == kTypeString) {
        /* Include the first stack line when the engine provides one: the
         * message alone does not say where a page script broke. */
        ant_value_t sv = js_get(e->js, v, "stack");
        char sbuf[512];
        js_cstr_t stk;
        const char* stack = "";

        name = js_to_cstr(e->js, nv, nbuf, sizeof(nbuf));
        msg  = js_to_cstr(e->js, mv, mbuf, sizeof(mbuf));

        if (vtype(sv) == kTypeString) {
            stk = js_to_cstr(e->js, sv, sbuf, sizeof(sbuf));
            if (stk.ptr && stk.len)
                stack = stk.ptr;
        }

        if (stack[0])
            snprintf(e->err, sizeof(e->err), "%s: %.*s: %.*s | %s", what,
                     (int)name.len, name.ptr ? name.ptr : "",
                     (int)msg.len, msg.ptr ? msg.ptr : "", stack);
        else
            snprintf(e->err, sizeof(e->err), "%s: %.*s: %.*s", what,
                     (int)name.len, name.ptr ? name.ptr : "",
                     (int)msg.len, msg.ptr ? msg.ptr : "");
    } else if (vtype(nv) == kTypeString) {
        name = js_to_cstr(e->js, nv, nbuf, sizeof(nbuf));
        snprintf(e->err, sizeof(e->err), "%s: %.*s", what,
                 (int)name.len, name.ptr ? name.ptr : "");
    } else {
        char buf[512];
        js_cstr_t s = js_inspect_cstr(e->js, v, buf, sizeof(buf));
        snprintf(e->err, sizeof(e->err), "%s: %.*s", what,
                 s.len ? (int)s.len : 0, s.ptr ? s.ptr : "(unknown)");
    }
}

AntbEngine* antb_engine_create(const void* stack_anchor)
{
    AntbEngine* e = (AntbEngine*)calloc(1, sizeof(AntbEngine));
    if (!e)
        return NULL;

    e->js = ant_create(NULL);
    if (!e->js) {
        free(e);
        return NULL;
    }

    /* Ant needs the caller's stack bounds; without them the VM can overflow. */
    js_setstackbase(e->js, (void*)stack_anchor);
    js_setstacklimit(e->js, 32u * 1024u * 1024u);

    {
        char* argv[2];
        argv[0] = (char*)"agave";
        argv[1] = NULL;
        ant_runtime_init(e->js, 1, argv, NULL);
    }
    ant_bootstrap_modules(e->js);
    js_set_filename(e->js, "agave://page.js");

    return e;
}

void antb_engine_destroy(AntbEngine* engine)
{
    if (!engine)
        return;
    if (engine->js)
        js_destroy(engine->js);
    free(engine);
}

void antb_set_filename(AntbEngine* engine, const char* name)
{
    if (engine && engine->js && name)
        js_set_filename(engine->js, name);
}

AntbValue* antb_eval(AntbEngine* engine, const char* src, int len, char** err)
{
    ant_value_t v;

    if (err)
        *err = NULL;
    if (!engine || !engine->js || !src || len < 0)
        return NULL;

    v = js_eval_bytecode_eval(engine->js, src, (size_t)len);

    if (is_err(v)) {
        /* The value returned by eval is an error marker; the real thrown value
         * lives in the isolate until it is taken. */
        ant_value_t thrown = js_take_thrown(engine->js, v);
        copy_cstr(engine, "evaluation failed", is_err(thrown) ? v : thrown);
        if (err) {
            *err = (char*)malloc(strlen(engine->err) + 1);
            if (*err)
                strcpy(*err, engine->err);
        }
        return NULL;
    }

    {
        AntbValue* out = (AntbValue*)calloc(1, sizeof(AntbValue));
        if (!out)
            return NULL;
        out->js = engine->js;
        out->v  = v;
        return out;
    }
}

double antb_to_number(const AntbValue* value)
{
    if (!value)
        return 0.0;
    return js_getnum(value->v);
}

int antb_is_function(const AntbValue* value)
{
    return value && value->js && vtype(value->v) == kTypeFunction;
}

int antb_is_string(const AntbValue* value)
{
    return value && value->js && vtype(value->v) == kTypeString;
}

int antb_is_number(const AntbValue* value)
{
    if (!value)
        return 0;
    return vtype(value->v) == kTypeNumber;
}

char* antb_to_utf8(const AntbValue* value)
{
    char buf[512];
    js_cstr_t s;
    char* out;

    if (!value)
        return NULL;

    s = js_to_cstr(value->js, value->v, buf, sizeof(buf));
    if (!s.ptr)
        return NULL;

    out = (char*)malloc(s.len + 1);
    if (!out)
        return NULL;
    memcpy(out, s.ptr, s.len);
    out[s.len] = '\0';
    return out;
}

void antb_value_release(AntbValue* value)
{
    free(value);
}

void antb_free(void* p)
{
    free(p);
}

const char* antb_last_error(const AntbEngine* engine)
{
    return engine ? engine->err : "";
}

/* ========================================================================= */
/* objects and native methods (task 2-1)                                     */
/* ========================================================================= */

#define AGB_TAG_METHOD 0x414D4554u   /* 'AMET' */
#define AGB_TAG_GETTER 0x41474554u   /* 'AGET' */

typedef struct AntbClass AntbClass;

typedef struct {
    AntbEngine*  engine;
    AntbGetterFn fn;      /* single-property getter (document.title) */
    AntbSetterFn setter;  /* optional setter */
    void*        user;
    AntbClass*   klass;   /* when set, dispatch by name through the class table */
} agb_getter_entry;

static AntbValue* alloc_value(ant_t* js, ant_value_t v)
{
    AntbValue* out = (AntbValue*)calloc(1, sizeof(AntbValue));
    if (!out)
        return NULL;
    out->js = js;
    out->v  = v;
    return out;
}

AntbValue* antb_make_object(AntbEngine* engine)
{
    if (!engine || !engine->js)
        return NULL;
    return alloc_value(engine->js, mkobj(engine->js, 0));
}

AntbValue* antb_make_string(AntbEngine* engine, const char* utf8)
{
    if (!engine || !engine->js || !utf8)
        return NULL;
    return alloc_value(engine->js, js_mkstr(engine->js, utf8, strlen(utf8)));
}

AntbValue* antb_make_number(AntbEngine* engine, double v)
{
    if (!engine || !engine->js)
        return NULL;
    return alloc_value(engine->js, js_mknum(v));
}

AntbValue* antb_make_array(AntbEngine* engine)
{
    AntbValue* out;

    if (!engine || !engine->js)
        return NULL;

    out = alloc_value(engine->js, js_mkarr(engine->js));
    return out;
}

AntbValue* antb_array_get(AntbValue* array, int index)
{
    char key[16];

    if (!array || !array->js)
        return NULL;

    snprintf(key, sizeof(key), "%d", index);
    return alloc_value(array->js, js_get(array->js, array->v, key));
}

void antb_array_push(AntbEngine* engine, AntbValue* array, const AntbValue* value)
{
    if (!engine || !array || !value)
        return;
    js_arr_push(engine->js, array->v, value->v);
}

void antb_delete_prop(AntbEngine* engine, AntbValue* obj, const char* name)
{
    ant_value_t key;

    if (!engine || !obj || !name)
        return;

    key = js_mkstr(engine->js, name, strlen(name));
    js_setprop(engine->js, obj->v, key, js_mkundef());
}

void antb_set_prop(AntbEngine* engine, AntbValue* obj, const char* name, const AntbValue* value)
{
    if (!engine || !obj || !name || !value)
        return;
    js_setprop(engine->js, obj->v,
               js_mkstr(engine->js, name, strlen(name)), value->v);
}

AntbValue* antb_get_prop(AntbEngine* engine, AntbValue* obj, const char* name)
{
    ant_value_t v;

    if (!engine || !obj || !name)
        return NULL;

    v = js_getprop_fallback(engine->js, obj->v, name);
    if (vtype(v) == kTypeUndefined || is_err(v))
        return NULL;

    return alloc_value(engine->js, v);
}

void antb_set_native(AntbValue* obj, void* ptr, unsigned tag)
{
    if (obj)
        js_set_native(obj->v, ptr, (uint32_t)tag);
}

void* antb_get_native(const AntbValue* obj, unsigned tag)
{
    if (!obj)
        return NULL;
    return js_get_native(obj->v, (uint32_t)tag);
}

/* --- native methods ------------------------------------------------------ */

/* Ant's cfunc values carry no per-instance identity (js_set_native and property
 * lookup both fail on T_CFUNC), and its own modules give every method its own C
 * function. We do the same with a generated pool of trampolines: each slot
 * dispatches to the (object, callback) pair registered for it. */
#define AGB_TRAMPOLINE_COUNT 256

typedef struct {
    AntbEngine*  engine;
    ant_t*       js;
    ant_value_t  obj;
    char         name[64];
    AntbNativeFn fn;
    void*        user;
} agb_method_entry;

static agb_method_entry g_methods[AGB_TRAMPOLINE_COUNT];

/* Diagnostics: name of the last bridged callback entered, so errors raised from
 * native code (which have no VM frame) can still be attributed. */
static const char* g_last_call = "";
static int g_trace_calls = -1;

/* Returns a malloc'd description of a pending exception and clears it, or NULL
 * when the engine is clean. Timer callbacks run outside evaluation, so their
 * exceptions would otherwise stay pending and poison later calls. */
char* antb_take_pending_error(AntbEngine* engine)
{
    ant_t* js;
    char* out;

    if (!engine || !(js = engine->js))
        return NULL;

    if (!Ant_Exception_Pending(js))
        return NULL;

    copy_cstr(engine, "uncaught", Ant_Exception_Current(js));
    Ant_Exception_Clear(js);

    out = (char*)malloc(strlen(engine->err) + 1);
    if (out)
        strcpy(out, engine->err);
    return out;
}

void antb_pump(AntbEngine* engine)
{
    ant_t* js;

    if (!engine || !(js = engine->js))
        return;

    if (js->loop)
        ant_uv_run(js->loop, UV_RUN_NOWAIT);
    js_poll_events(js);
}

AGB_API const char* antb_last_call(void)
{
    return g_last_call;
}
static int              g_method_count = 0;

static ant_value_t agb_dispatch(int slot, ant_t* js, ant_value_t* args, int nargs,
                                ant_value_t call_new_target);

#define AGB_TRAMP_DECL(n) \
    static ant_value_t agb_tramp_##n(ant_t* js, ant_value_t* args, int nargs, \
                                     ant_value_t call_new_target) \
    { return agb_dispatch(n, js, args, nargs, call_new_target); }

AGB_TRAMP_DECL(0)  AGB_TRAMP_DECL(1)  AGB_TRAMP_DECL(2)  AGB_TRAMP_DECL(3)
AGB_TRAMP_DECL(4)  AGB_TRAMP_DECL(5)  AGB_TRAMP_DECL(6)  AGB_TRAMP_DECL(7)
AGB_TRAMP_DECL(8)  AGB_TRAMP_DECL(9)  AGB_TRAMP_DECL(10)  AGB_TRAMP_DECL(11)
AGB_TRAMP_DECL(12)  AGB_TRAMP_DECL(13)  AGB_TRAMP_DECL(14)  AGB_TRAMP_DECL(15)
AGB_TRAMP_DECL(16)  AGB_TRAMP_DECL(17)  AGB_TRAMP_DECL(18)  AGB_TRAMP_DECL(19)
AGB_TRAMP_DECL(20)  AGB_TRAMP_DECL(21)  AGB_TRAMP_DECL(22)  AGB_TRAMP_DECL(23)
AGB_TRAMP_DECL(24)  AGB_TRAMP_DECL(25)  AGB_TRAMP_DECL(26)  AGB_TRAMP_DECL(27)
AGB_TRAMP_DECL(28)  AGB_TRAMP_DECL(29)  AGB_TRAMP_DECL(30)  AGB_TRAMP_DECL(31)
AGB_TRAMP_DECL(32)  AGB_TRAMP_DECL(33)  AGB_TRAMP_DECL(34)  AGB_TRAMP_DECL(35)
AGB_TRAMP_DECL(36)  AGB_TRAMP_DECL(37)  AGB_TRAMP_DECL(38)  AGB_TRAMP_DECL(39)
AGB_TRAMP_DECL(40)  AGB_TRAMP_DECL(41)  AGB_TRAMP_DECL(42)  AGB_TRAMP_DECL(43)
AGB_TRAMP_DECL(44)  AGB_TRAMP_DECL(45)  AGB_TRAMP_DECL(46)  AGB_TRAMP_DECL(47)
AGB_TRAMP_DECL(48)  AGB_TRAMP_DECL(49)  AGB_TRAMP_DECL(50)  AGB_TRAMP_DECL(51)
AGB_TRAMP_DECL(52)  AGB_TRAMP_DECL(53)  AGB_TRAMP_DECL(54)  AGB_TRAMP_DECL(55)
AGB_TRAMP_DECL(56)  AGB_TRAMP_DECL(57)  AGB_TRAMP_DECL(58)  AGB_TRAMP_DECL(59)
AGB_TRAMP_DECL(60)  AGB_TRAMP_DECL(61)  AGB_TRAMP_DECL(62)  AGB_TRAMP_DECL(63)
AGB_TRAMP_DECL(64)  AGB_TRAMP_DECL(65)  AGB_TRAMP_DECL(66)  AGB_TRAMP_DECL(67)
AGB_TRAMP_DECL(68)  AGB_TRAMP_DECL(69)  AGB_TRAMP_DECL(70)  AGB_TRAMP_DECL(71)
AGB_TRAMP_DECL(72)  AGB_TRAMP_DECL(73)  AGB_TRAMP_DECL(74)  AGB_TRAMP_DECL(75)
AGB_TRAMP_DECL(76)  AGB_TRAMP_DECL(77)  AGB_TRAMP_DECL(78)  AGB_TRAMP_DECL(79)
AGB_TRAMP_DECL(80)  AGB_TRAMP_DECL(81)  AGB_TRAMP_DECL(82)  AGB_TRAMP_DECL(83)
AGB_TRAMP_DECL(84)  AGB_TRAMP_DECL(85)  AGB_TRAMP_DECL(86)  AGB_TRAMP_DECL(87)
AGB_TRAMP_DECL(88)  AGB_TRAMP_DECL(89)  AGB_TRAMP_DECL(90)  AGB_TRAMP_DECL(91)
AGB_TRAMP_DECL(92)  AGB_TRAMP_DECL(93)  AGB_TRAMP_DECL(94)  AGB_TRAMP_DECL(95)
AGB_TRAMP_DECL(96)  AGB_TRAMP_DECL(97)  AGB_TRAMP_DECL(98)  AGB_TRAMP_DECL(99)
AGB_TRAMP_DECL(100)  AGB_TRAMP_DECL(101)  AGB_TRAMP_DECL(102)  AGB_TRAMP_DECL(103)
AGB_TRAMP_DECL(104)  AGB_TRAMP_DECL(105)  AGB_TRAMP_DECL(106)  AGB_TRAMP_DECL(107)
AGB_TRAMP_DECL(108)  AGB_TRAMP_DECL(109)  AGB_TRAMP_DECL(110)  AGB_TRAMP_DECL(111)
AGB_TRAMP_DECL(112)  AGB_TRAMP_DECL(113)  AGB_TRAMP_DECL(114)  AGB_TRAMP_DECL(115)
AGB_TRAMP_DECL(116)  AGB_TRAMP_DECL(117)  AGB_TRAMP_DECL(118)  AGB_TRAMP_DECL(119)
AGB_TRAMP_DECL(120)  AGB_TRAMP_DECL(121)  AGB_TRAMP_DECL(122)  AGB_TRAMP_DECL(123)
AGB_TRAMP_DECL(124)  AGB_TRAMP_DECL(125)  AGB_TRAMP_DECL(126)  AGB_TRAMP_DECL(127)
AGB_TRAMP_DECL(128)  AGB_TRAMP_DECL(129)  AGB_TRAMP_DECL(130)  AGB_TRAMP_DECL(131)
AGB_TRAMP_DECL(132)  AGB_TRAMP_DECL(133)  AGB_TRAMP_DECL(134)  AGB_TRAMP_DECL(135)
AGB_TRAMP_DECL(136)  AGB_TRAMP_DECL(137)  AGB_TRAMP_DECL(138)  AGB_TRAMP_DECL(139)
AGB_TRAMP_DECL(140)  AGB_TRAMP_DECL(141)  AGB_TRAMP_DECL(142)  AGB_TRAMP_DECL(143)
AGB_TRAMP_DECL(144)  AGB_TRAMP_DECL(145)  AGB_TRAMP_DECL(146)  AGB_TRAMP_DECL(147)
AGB_TRAMP_DECL(148)  AGB_TRAMP_DECL(149)  AGB_TRAMP_DECL(150)  AGB_TRAMP_DECL(151)
AGB_TRAMP_DECL(152)  AGB_TRAMP_DECL(153)  AGB_TRAMP_DECL(154)  AGB_TRAMP_DECL(155)
AGB_TRAMP_DECL(156)  AGB_TRAMP_DECL(157)  AGB_TRAMP_DECL(158)  AGB_TRAMP_DECL(159)
AGB_TRAMP_DECL(160)  AGB_TRAMP_DECL(161)  AGB_TRAMP_DECL(162)  AGB_TRAMP_DECL(163)
AGB_TRAMP_DECL(164)  AGB_TRAMP_DECL(165)  AGB_TRAMP_DECL(166)  AGB_TRAMP_DECL(167)
AGB_TRAMP_DECL(168)  AGB_TRAMP_DECL(169)  AGB_TRAMP_DECL(170)  AGB_TRAMP_DECL(171)
AGB_TRAMP_DECL(172)  AGB_TRAMP_DECL(173)  AGB_TRAMP_DECL(174)  AGB_TRAMP_DECL(175)
AGB_TRAMP_DECL(176)  AGB_TRAMP_DECL(177)  AGB_TRAMP_DECL(178)  AGB_TRAMP_DECL(179)
AGB_TRAMP_DECL(180)  AGB_TRAMP_DECL(181)  AGB_TRAMP_DECL(182)  AGB_TRAMP_DECL(183)
AGB_TRAMP_DECL(184)  AGB_TRAMP_DECL(185)  AGB_TRAMP_DECL(186)  AGB_TRAMP_DECL(187)
AGB_TRAMP_DECL(188)  AGB_TRAMP_DECL(189)  AGB_TRAMP_DECL(190)  AGB_TRAMP_DECL(191)
AGB_TRAMP_DECL(192)  AGB_TRAMP_DECL(193)  AGB_TRAMP_DECL(194)  AGB_TRAMP_DECL(195)
AGB_TRAMP_DECL(196)  AGB_TRAMP_DECL(197)  AGB_TRAMP_DECL(198)  AGB_TRAMP_DECL(199)
AGB_TRAMP_DECL(200)  AGB_TRAMP_DECL(201)  AGB_TRAMP_DECL(202)  AGB_TRAMP_DECL(203)
AGB_TRAMP_DECL(204)  AGB_TRAMP_DECL(205)  AGB_TRAMP_DECL(206)  AGB_TRAMP_DECL(207)
AGB_TRAMP_DECL(208)  AGB_TRAMP_DECL(209)  AGB_TRAMP_DECL(210)  AGB_TRAMP_DECL(211)
AGB_TRAMP_DECL(212)  AGB_TRAMP_DECL(213)  AGB_TRAMP_DECL(214)  AGB_TRAMP_DECL(215)
AGB_TRAMP_DECL(216)  AGB_TRAMP_DECL(217)  AGB_TRAMP_DECL(218)  AGB_TRAMP_DECL(219)
AGB_TRAMP_DECL(220)  AGB_TRAMP_DECL(221)  AGB_TRAMP_DECL(222)  AGB_TRAMP_DECL(223)
AGB_TRAMP_DECL(224)  AGB_TRAMP_DECL(225)  AGB_TRAMP_DECL(226)  AGB_TRAMP_DECL(227)
AGB_TRAMP_DECL(228)  AGB_TRAMP_DECL(229)  AGB_TRAMP_DECL(230)  AGB_TRAMP_DECL(231)
AGB_TRAMP_DECL(232)  AGB_TRAMP_DECL(233)  AGB_TRAMP_DECL(234)  AGB_TRAMP_DECL(235)
AGB_TRAMP_DECL(236)  AGB_TRAMP_DECL(237)  AGB_TRAMP_DECL(238)  AGB_TRAMP_DECL(239)
AGB_TRAMP_DECL(240)  AGB_TRAMP_DECL(241)  AGB_TRAMP_DECL(242)  AGB_TRAMP_DECL(243)
AGB_TRAMP_DECL(244)  AGB_TRAMP_DECL(245)  AGB_TRAMP_DECL(246)  AGB_TRAMP_DECL(247)
AGB_TRAMP_DECL(248)  AGB_TRAMP_DECL(249)  AGB_TRAMP_DECL(250)  AGB_TRAMP_DECL(251)
AGB_TRAMP_DECL(252)  AGB_TRAMP_DECL(253)  AGB_TRAMP_DECL(254)  AGB_TRAMP_DECL(255)

typedef ant_value_t (*agb_cfunc)(ant_t*, ant_value_t*, int, ant_value_t);

static agb_cfunc const g_tramps[AGB_TRAMPOLINE_COUNT] = {
    agb_tramp_0,  agb_tramp_1,  agb_tramp_2,  agb_tramp_3,
    agb_tramp_4,  agb_tramp_5,  agb_tramp_6,  agb_tramp_7,
    agb_tramp_8,  agb_tramp_9,  agb_tramp_10,  agb_tramp_11,
    agb_tramp_12,  agb_tramp_13,  agb_tramp_14,  agb_tramp_15,
    agb_tramp_16,  agb_tramp_17,  agb_tramp_18,  agb_tramp_19,
    agb_tramp_20,  agb_tramp_21,  agb_tramp_22,  agb_tramp_23,
    agb_tramp_24,  agb_tramp_25,  agb_tramp_26,  agb_tramp_27,
    agb_tramp_28,  agb_tramp_29,  agb_tramp_30,  agb_tramp_31,
    agb_tramp_32,  agb_tramp_33,  agb_tramp_34,  agb_tramp_35,
    agb_tramp_36,  agb_tramp_37,  agb_tramp_38,  agb_tramp_39,
    agb_tramp_40,  agb_tramp_41,  agb_tramp_42,  agb_tramp_43,
    agb_tramp_44,  agb_tramp_45,  agb_tramp_46,  agb_tramp_47,
    agb_tramp_48,  agb_tramp_49,  agb_tramp_50,  agb_tramp_51,
    agb_tramp_52,  agb_tramp_53,  agb_tramp_54,  agb_tramp_55,
    agb_tramp_56,  agb_tramp_57,  agb_tramp_58,  agb_tramp_59,
    agb_tramp_60,  agb_tramp_61,  agb_tramp_62,  agb_tramp_63,
    agb_tramp_64,  agb_tramp_65,  agb_tramp_66,  agb_tramp_67,
    agb_tramp_68,  agb_tramp_69,  agb_tramp_70,  agb_tramp_71,
    agb_tramp_72,  agb_tramp_73,  agb_tramp_74,  agb_tramp_75,
    agb_tramp_76,  agb_tramp_77,  agb_tramp_78,  agb_tramp_79,
    agb_tramp_80,  agb_tramp_81,  agb_tramp_82,  agb_tramp_83,
    agb_tramp_84,  agb_tramp_85,  agb_tramp_86,  agb_tramp_87,
    agb_tramp_88,  agb_tramp_89,  agb_tramp_90,  agb_tramp_91,
    agb_tramp_92,  agb_tramp_93,  agb_tramp_94,  agb_tramp_95,
    agb_tramp_96,  agb_tramp_97,  agb_tramp_98,  agb_tramp_99,
    agb_tramp_100,  agb_tramp_101,  agb_tramp_102,  agb_tramp_103,
    agb_tramp_104,  agb_tramp_105,  agb_tramp_106,  agb_tramp_107,
    agb_tramp_108,  agb_tramp_109,  agb_tramp_110,  agb_tramp_111,
    agb_tramp_112,  agb_tramp_113,  agb_tramp_114,  agb_tramp_115,
    agb_tramp_116,  agb_tramp_117,  agb_tramp_118,  agb_tramp_119,
    agb_tramp_120,  agb_tramp_121,  agb_tramp_122,  agb_tramp_123,
    agb_tramp_124,  agb_tramp_125,  agb_tramp_126,  agb_tramp_127,
    agb_tramp_128,  agb_tramp_129,  agb_tramp_130,  agb_tramp_131,
    agb_tramp_132,  agb_tramp_133,  agb_tramp_134,  agb_tramp_135,
    agb_tramp_136,  agb_tramp_137,  agb_tramp_138,  agb_tramp_139,
    agb_tramp_140,  agb_tramp_141,  agb_tramp_142,  agb_tramp_143,
    agb_tramp_144,  agb_tramp_145,  agb_tramp_146,  agb_tramp_147,
    agb_tramp_148,  agb_tramp_149,  agb_tramp_150,  agb_tramp_151,
    agb_tramp_152,  agb_tramp_153,  agb_tramp_154,  agb_tramp_155,
    agb_tramp_156,  agb_tramp_157,  agb_tramp_158,  agb_tramp_159,
    agb_tramp_160,  agb_tramp_161,  agb_tramp_162,  agb_tramp_163,
    agb_tramp_164,  agb_tramp_165,  agb_tramp_166,  agb_tramp_167,
    agb_tramp_168,  agb_tramp_169,  agb_tramp_170,  agb_tramp_171,
    agb_tramp_172,  agb_tramp_173,  agb_tramp_174,  agb_tramp_175,
    agb_tramp_176,  agb_tramp_177,  agb_tramp_178,  agb_tramp_179,
    agb_tramp_180,  agb_tramp_181,  agb_tramp_182,  agb_tramp_183,
    agb_tramp_184,  agb_tramp_185,  agb_tramp_186,  agb_tramp_187,
    agb_tramp_188,  agb_tramp_189,  agb_tramp_190,  agb_tramp_191,
    agb_tramp_192,  agb_tramp_193,  agb_tramp_194,  agb_tramp_195,
    agb_tramp_196,  agb_tramp_197,  agb_tramp_198,  agb_tramp_199,
    agb_tramp_200,  agb_tramp_201,  agb_tramp_202,  agb_tramp_203,
    agb_tramp_204,  agb_tramp_205,  agb_tramp_206,  agb_tramp_207,
    agb_tramp_208,  agb_tramp_209,  agb_tramp_210,  agb_tramp_211,
    agb_tramp_212,  agb_tramp_213,  agb_tramp_214,  agb_tramp_215,
    agb_tramp_216,  agb_tramp_217,  agb_tramp_218,  agb_tramp_219,
    agb_tramp_220,  agb_tramp_221,  agb_tramp_222,  agb_tramp_223,
    agb_tramp_224,  agb_tramp_225,  agb_tramp_226,  agb_tramp_227,
    agb_tramp_228,  agb_tramp_229,  agb_tramp_230,  agb_tramp_231,
    agb_tramp_232,  agb_tramp_233,  agb_tramp_234,  agb_tramp_235,
    agb_tramp_236,  agb_tramp_237,  agb_tramp_238,  agb_tramp_239,
    agb_tramp_240,  agb_tramp_241,  agb_tramp_242,  agb_tramp_243,
    agb_tramp_244,  agb_tramp_245,  agb_tramp_246,  agb_tramp_247,
    agb_tramp_248,  agb_tramp_249,  agb_tramp_250,  agb_tramp_251,
    agb_tramp_252,  agb_tramp_253,  agb_tramp_254,  agb_tramp_255,
};
static ant_value_t agb_dispatch(int slot, ant_t* js, ant_value_t* args, int nargs,
                                ant_value_t call_new_target)
{
    agb_method_entry* e;
    AntbValue  self_v;
    AntbValue  arg_buf[16];
    AntbValue* arg_ptrs[16];
    AntbValue  out_v;
    ant_value_t thiz;
    AntbValue*  self_p = NULL;
    int i, n;

    (void)call_new_target;

    if (slot < 0 || slot >= g_method_count)
        return js_mkundef();
    e = &g_methods[slot];
    if (g_trace_calls < 0) g_trace_calls = getenv("AGB_JS_TRACE") ? 1 : 0;
    g_last_call = e->name;
    if (g_trace_calls) fprintf(stderr, "[call] %s nargs=%d\n", e->name, nargs);
    if (e->js != js) {
        return js_mkundef();
    }

    thiz = js_getthis(js);
    if (vtype(thiz) != kTypeUndefined && vtype(thiz) != kTypeNull) {
        self_v.js = js;
        self_v.v  = thiz;
        self_p    = &self_v;
    }

    n = nargs < 16 ? nargs : 16;
    for (i = 0; i < n; i++) {
        arg_buf[i].js = js;
        arg_buf[i].v  = args[i];
        arg_ptrs[i]   = &arg_buf[i];
    }

    out_v.js = js;
    out_v.v  = js_mkundef();

    if (!e->fn(e->engine, self_p, arg_ptrs, n, &out_v, e->user))
        return js_mkundef();

    return out_v.v;
}

void antb_define_method(AntbEngine* engine, AntbValue* obj, const char* name,
                        AntbNativeFn fn, void* user)
{
    agb_method_entry* e;
    ant_value_t f;
    int slot;

    if (!engine || !obj || !name || !fn)
        return;
    if (g_method_count >= AGB_TRAMPOLINE_COUNT)
        return;

    slot = g_method_count++;
    e = &g_methods[slot];
    e->engine = engine;
    e->js   = engine->js;
    e->obj  = obj->v;
    e->fn   = fn;
    e->user = user;
    strncpy(e->name, name, sizeof(e->name) - 1);

    f = js_mkfun_dyn(g_tramps[slot]);
    /* js_set_function_name needs a real object; T_CFUNC rejects it */
    js_setprop(engine->js, obj->v, js_mkstr(engine->js, name, strlen(name)), f);
}

/* --- accessor properties ------------------------------------------------- */

/* defined further down (class property tables) */
static int agb_class_lookup_prop(AntbClass* k, const char* name,
                                 AntbGetterFn* fn, void** user);

static ant_value_t agb_getter_trampoline(ant_t* js, ant_value_t obj,
                                         const char* key, size_t klen)
{
    agb_getter_entry* entry = (agb_getter_entry*)js_get_native(obj, AGB_TAG_GETTER);
    AntbValue  self_v;
    AntbValue  out_v;
    AntbGetterFn fn = NULL;
    void*      user = NULL;
    char name[128];

    if (!entry)
        return js_mkundef();

    if (klen >= sizeof(name))
        klen = sizeof(name) - 1;
    memcpy(name, key, klen);
    name[klen] = '\0';

    if (entry->klass) {
        if (!agb_class_lookup_prop(entry->klass, name, &fn, &user))
            return js_mkundef();
    } else {
        fn   = entry->fn;
        user = entry->user;
        if (!fn)
            return js_mkundef();
    }

    self_v.js = js;
    self_v.v  = obj;
    out_v.js  = js;
    out_v.v   = js_mkundef();

    if (!fn(entry->engine, &self_v, name, &out_v, user))
        return js_mkundef();

    return out_v.v;
}

static bool agb_setter_trampoline(ant_t* js, ant_value_t obj,
                                  const char* key, size_t klen, ant_value_t value)
{
    agb_getter_entry* entry = (agb_getter_entry*)js_get_native(obj, AGB_TAG_GETTER);
    AntbValue self_v, val_v;
    char name[128];

    if (!entry || !entry->setter)
        return false;

    if (klen >= sizeof(name))
        klen = sizeof(name) - 1;
    memcpy(name, key, klen);
    name[klen] = '\0';

    self_v.js = js;
    self_v.v  = obj;
    val_v.js  = js;
    val_v.v   = value;

    return entry->setter(entry->engine, &self_v, name, &val_v, entry->user) != 0;
}

void antb_define_property_rw(AntbEngine* engine, AntbValue* obj, const char* name,
                             AntbGetterFn getter, AntbSetterFn setter, void* user)
{
    agb_getter_entry* entry;

    if (!engine || !obj || !name)
        return;

    entry = (agb_getter_entry*)js_get_native(obj->v, AGB_TAG_GETTER);
    if (!entry) {
        entry = (agb_getter_entry*)calloc(1, sizeof(*entry));
        if (!entry)
            return;
        js_set_native(obj->v, entry, AGB_TAG_GETTER);
        js_set_getter(obj->v, agb_getter_trampoline);
        js_set_setter(obj->v, agb_setter_trampoline);
    }
    entry->engine = engine;
    entry->fn     = getter;
    entry->setter = setter;
    entry->user   = user;
    entry->klass  = NULL;   /* take over from the class level dispatcher */

    /* the object may already carry the class-installed hooks; the setter one
     * has to be (re)installed here or assignments would fall through */
    js_set_getter(obj->v, agb_getter_trampoline);
    js_set_setter(obj->v, agb_setter_trampoline);
}

void antb_define_property(AntbEngine* engine, AntbValue* obj, const char* name,
                          AntbGetterFn fn, void* user)
{
    agb_getter_entry* entry;

    if (!engine || !obj || !name || !fn)
        return;

    entry = (agb_getter_entry*)js_get_native(obj->v, AGB_TAG_GETTER);
    if (!entry) {
        entry = (agb_getter_entry*)calloc(1, sizeof(*entry));
        if (!entry)
            return;
        js_set_native(obj->v, entry, AGB_TAG_GETTER);
        js_set_getter(obj->v, agb_getter_trampoline);
    }
    entry->engine = engine;
    entry->fn   = fn;
    entry->user = user;
}

AntbValue* antb_global(AntbEngine* engine)
{
    if (!engine || !engine->js)
        return NULL;
    return alloc_value(engine->js, js_glob(engine->js));
}

AntbValue* antb_call(AntbEngine* engine, const AntbValue* fn, const AntbValue* self,
                     AntbValue** args, int argc, char** err)
{
    ant_value_t argv[16];
    ant_value_t this_v;
    ant_value_t r;
    int i;

    if (err)
        *err = NULL;
    if (!engine || !engine->js || !fn)
        return NULL;

    if (argc > 16)
        argc = 16;
    for (i = 0; i < argc; i++)
        argv[i] = args[i] ? args[i]->v : js_mkundef();

    this_v = (self && self->js) ? self->v : js_glob(engine->js);

    r = js_call_value(engine->js, fn->v, this_v, argv, argc);
    if (is_err(r)) {
        copy_cstr(engine, "call failed", js_take_thrown(engine->js, r));
        if (err) {
            *err = (char*)malloc(strlen(engine->err) + 1);
            if (*err)
                strcpy(*err, engine->err);
        }
        return NULL;
    }
    return alloc_value(engine->js, r);
}

void antb_value_assign(AntbValue* out, const AntbValue* value)
{
    if (out && value) {
        out->js = value->js;
        out->v  = value->v;
    }
}

/* --- wrapper cache ------------------------------------------------------- */

#define AGB_MAX_WRAPPERS 65536

typedef struct {
    ant_t*      js;
    void*       ptr;
    unsigned    tag;
    ant_value_t value;   /* GC root: registered with gc_register_root() */
    int         used;
    int         next;    /* hash chain index+1 */
} agb_wrapper_entry;

static agb_wrapper_entry g_wrappers[AGB_MAX_WRAPPERS];
#define AGB_WRAPPER_HASH_SIZE 65521   /* prime */

static int                g_wrapperHash[AGB_WRAPPER_HASH_SIZE];   /* index+1 */
static int                g_wrapper_count = 0;

static unsigned agbWrapperBucket(ant_t* js, void* ptr, unsigned tag)
{
    unsigned long long h = ((unsigned long long)(size_t)ptr >> 4) ^ tag;

    h ^= (unsigned long long)(size_t)js >> 4;
    h *= 1099511628211ULL;
    return (unsigned)(h % AGB_WRAPPER_HASH_SIZE);
}

AntbValue* antb_wrap_native(AntbEngine* engine, void* ptr, unsigned tag)
{
    int i;

    if (!engine || !engine->js || !ptr)
        return NULL;

    /* O(1) lookup: the old linear scan over every wrapper turned node-heavy
     * pages into O(n^2) (163.com spent minutes here). */
    {
        unsigned bucket = agbWrapperBucket(engine->js, ptr, tag);
        int idx = g_wrapperHash[bucket];

        while (idx) {
            agb_wrapper_entry* e = &g_wrappers[idx - 1];

            if (e->used && e->js == engine->js && e->ptr == ptr && e->tag == tag)
                return alloc_value(engine->js, e->value);
            idx = e->next;
        }
    }

    if (g_wrapper_count >= AGB_MAX_WRAPPERS)
        return NULL;

    {
        agb_wrapper_entry* e = &g_wrappers[g_wrapper_count];
        ant_value_t obj = mkobj(engine->js, 0);

        js_set_native(obj, ptr, (uint32_t)tag);

        e->js    = engine->js;
        e->ptr   = ptr;
        e->tag   = tag;
        e->value = obj;
        e->used  = 1;

        /* keep the wrapper alive for the lifetime of the engine */
        gc_pin_permanent(e->js, e->value);

        {
            unsigned bucket = agbWrapperBucket(e->js, e->ptr, e->tag);

            e->next = g_wrapperHash[bucket];
            g_wrapperHash[bucket] = g_wrapper_count + 1;
        }
        g_wrapper_count++;

        return alloc_value(engine->js, obj);
    }
}

AntbValue* antb_retain(const AntbValue* value)
{
    AntbValue* out;

    if (!value || !value->js)
        return NULL;

    out = alloc_value(value->js, value->v);
    if (!out)
        return NULL;

    gc_pin_permanent(value->js, out->v);
    return out;
}

int antb_to_boolean(const AntbValue* value)
{
    if (!value)
        return 0;
    return js_truthy(value->js, value->v) ? 1 : 0;
}

void antb_gc(AntbEngine* engine)
{
    if (engine && engine->js)
        gc_run(engine->js);
}
/* --- classes / prototypes ------------------------------------------------ */

#define AGB_MAX_CLASSES 128

struct AntbClass {
    AntbEngine* engine;
    ant_t*      js;
    ant_value_t proto;      /* GC root */
    char        name[64];
};

static struct AntbClass* g_classes[AGB_MAX_CLASSES];
static int               g_class_count = 0;

AntbClass* antb_class_create(AntbEngine* engine, const char* name)
{
    struct AntbClass* k;

    if (!engine || !engine->js || !name)
        return NULL;
    if (g_class_count >= AGB_MAX_CLASSES)
        return NULL;

    k = (struct AntbClass*)calloc(1, sizeof(struct AntbClass));
    if (!k)
        return NULL;

    k->engine = engine;
    k->js     = engine->js;
    k->proto  = mkobj(engine->js, 0);
    strncpy(k->name, name, sizeof(k->name) - 1);

    /* the prototype must outlive every instance */
    gc_pin_permanent(engine->js, k->proto);

    g_classes[g_class_count++] = k;
    return k;
}

void antb_class_add_method(AntbEngine* engine, AntbClass* klass,
                           const char* name, AntbNativeFn fn, void* user)
{
    AntbValue proto;

    if (!engine || !klass || !name || !fn)
        return;

    proto.js = klass->js;
    proto.v  = klass->proto;
    antb_define_method(engine, &proto, name, fn, user);
}

void antb_class_add_property(AntbEngine* engine, AntbClass* klass,
                             const char* name, AntbGetterFn fn, void* user)
{
    /* stored in the class table; installed per instance by antb_wrap_native_as */
    antb_class_set_property(engine, klass, name, fn, user);
}

AntbValue* antb_wrap_native_as(AntbEngine* engine, AntbClass* klass,
                               void* ptr, unsigned tag)
{
    AntbValue* v;

    if (!engine || !klass || !ptr)
        return NULL;

    v = antb_wrap_native(engine, ptr, tag);
    if (!v)
        return NULL;

    js_set_proto(engine->js, v->v, klass->proto);

    /* one dispatching getter per instance for the class' properties; only on
     * first wrap (the cache may hand back an already prepared wrapper) */
    if (!js_get_native(v->v, AGB_TAG_GETTER))
        antb_class_install_properties(engine, klass, v);

    return v;
}
/* --- class property tables ----------------------------------------------- *
 * ant does not run an object's exotic getter when the property is found on a
 * prototype, so class level properties are installed as one dispatching
 * getter per instance instead. */

#define AGB_MAX_CLASS_PROPS 32

typedef struct {
    char         name[64];
    AntbGetterFn fn;
    void*        user;
} agb_class_prop;

static agb_class_prop g_class_props[AGB_MAX_CLASSES][AGB_MAX_CLASS_PROPS];
static int            g_class_prop_count[AGB_MAX_CLASSES];

static int agb_class_index(AntbClass* k)
{
    int i;
    for (i = 0; i < g_class_count; i++)
        if (g_classes[i] == k)
            return i;
    return -1;
}

static int agb_class_lookup_prop(AntbClass* k, const char* name,
                                 AntbGetterFn* fn, void** user)
{
    int idx = agb_class_index(k);
    int i;

    if (idx < 0)
        return 0;

    for (i = 0; i < g_class_prop_count[idx]; i++) {
        if (strcmp(g_class_props[idx][i].name, name) == 0) {
            *fn   = g_class_props[idx][i].fn;
            *user = g_class_props[idx][i].user;
            return 1;
        }
    }
    return 0;
}

int antb_class_set_property(AntbEngine* engine, AntbClass* klass,
                            const char* name, AntbGetterFn fn, void* user)
{
    int idx;
    int n;

    if (!engine || !klass || !name || !fn)
        return 0;

    idx = agb_class_index(klass);
    if (idx < 0)
        return 0;

    n = g_class_prop_count[idx];
    if (n >= AGB_MAX_CLASS_PROPS)
        return 0;

    strncpy(g_class_props[idx][n].name, name, sizeof(g_class_props[idx][n].name) - 1);
    g_class_props[idx][n].fn   = fn;
    g_class_props[idx][n].user = user;
    g_class_prop_count[idx] = n + 1;
    return 1;
}

void antb_class_install_properties(AntbEngine* engine, AntbClass* klass, AntbValue* instance)
{
    agb_getter_entry* entry;

    if (!engine || !klass || !instance)
        return;

    entry = (agb_getter_entry*)calloc(1, sizeof(*entry));
    if (!entry)
        return;

    entry->engine = engine;
    entry->klass  = klass;
    entry->fn     = NULL;

    js_set_native(instance->v, entry, AGB_TAG_GETTER);
    js_set_getter(instance->v, agb_getter_trampoline);
}
