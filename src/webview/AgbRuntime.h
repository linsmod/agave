/*
 * AgbRuntime.h - engine-side state the webview control polls.
 *
 * The control (src/webview_control/AgaveWebView.c) is a C translation unit, so
 * it cannot include the C++ runtime header (namespace AntRuntime). These are
 * the few facts it needs from the engine, exported with C linkage from
 * src/javascript/ant_backend/AntRuntime.cpp.
 */

#ifndef AgbRuntime_h
#define AgbRuntime_h

#ifdef __cplusplus
extern "C" {
#endif

/* GetTickCount() when the outgoing document's teardown (pagehide,
 * visibilitychange, unload, realm stop) started, 0 when none is running. The
 * watchdog bounds the teardown with it: a handler that never returns must not
 * keep a navigation waiting. */
unsigned long agb_teardown_started_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* AgbRuntime_h */
