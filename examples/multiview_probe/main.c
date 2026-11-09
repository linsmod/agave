/* agave_multiview_probe - two views in one host, for the S3 acceptance test.
 *
 * The question it answers: while one page's script is stuck in an infinite
 * loop, how long does a command posted to *another* view take to run?
 *
 *   - backend local   : both views share the one engine thread, so view B's
 *                       command waits for the watchdog to abort A's script
 *                       (~1.5 s). That is the problem S3 exists for.
 *   - backend process : A runs in its own agave_render_host.exe; B answers
 *                       while A is still spinning.
 *
 * Output (one line each, parsed by tools/run_process_backend_tests.ps1):
 *   pong_ms=<n>     latency from posting the eval to B to its message arriving
 *   alive_ms=<n>    (--interrupt-test) latency until A answers after an
 *                   explicit agave_webview_interrupt(viewA)
 *
 * Usage:
 *   agave_multiview_probe.exe --backend process [--interrupt-test]
 *                             [--url-a <a>] [--url-b <b>]
 *
 * Env is not read: --backend sets AGB_VIEW_BACKEND here so the value is in
 * effect before the first view is created.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agave_webview.h"

typedef struct {
    AgaveWebView* view;
    volatile LONG loaded;
    volatile LONG pongAt;   /* GetTickCount() of the last "pong" */
    volatile LONG aliveAt;  /* GetTickCount() of the last "alive" */
} ProbeView;

/* document.title is per view in both backends (the local one routes the title
 * callback by MaCrossView, the process one carries the view's event), so it is
 * the probe channel here. on_message would not do: the local backend's
 * agavePostMessage hook is process-wide and has no view identity yet (gap
 * recorded for B2b/B3). */
static void onTitle(AgaveWebView* view, const char* title, void* user)
{
    ProbeView* pv = (ProbeView*)user;
    LONG now = (LONG)GetTickCount();

    (void)view;

    if (!title)
        return;


    if (!strcmp(title, "pong"))
        InterlockedExchange(&pv->pongAt, now);
    else if (!strcmp(title, "alive"))
        InterlockedExchange(&pv->aliveAt, now);
}

static void onMessage(AgaveWebView* view, const char* message, void* user)
{
    (void)view;
    (void)message;
    (void)user;
}

static void onProgress(AgaveWebView* view, int percent, int finished, void* user)
{
    ProbeView* pv = (ProbeView*)user;

    (void)view;
    (void)percent;

    if (finished)
        InterlockedExchange(&pv->loaded, 1);
}

/* Pump the host message queue: the controls post state messages and the engine
 * thread posts callbacks through it. */
static void pump(int ms)
{
    MSG msg;
    DWORD until = GetTickCount() + (DWORD)ms;

    while ((LONG)(until - GetTickCount()) > 0) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(1);
    }
}

static int waitFlag(volatile LONG* flag, int timeoutMs)
{
    int waited = 0;

    while (!*flag && waited < timeoutMs) {
        pump(5);
        waited += 5;
    }

    return *flag ? 1 : 0;
}

static LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY)
        PostQuitMessage(0);

    return DefWindowProcA(hwnd, msg, wp, lp);
}

int main(int argc, char** argv)
{
    const char* backend = "local";
    const char* urlA = "file:///E:/mybrowsers/agave/tools/pages/blank.html";
    const char* urlB = "file:///E:/mybrowsers/agave/tools/pages/cap_typing.html";
    int interruptTest = 0;
    int noBlock = 0;
    int allowBlocked = 0;
    int pongWaitMs = 6000;
    int destroyTest = 0;
    int i;
    WNDCLASSA wc;
    HWND host;
    AgaveWebViewCallback callbacks;
    ProbeView a, b;
    LONG pongMs = -1;
    LONG aliveMs = -1;
    LONG t0;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc)
            backend = argv[++i];
        else if (!strcmp(argv[i], "--url-a") && i + 1 < argc)
            urlA = argv[++i];
        else if (!strcmp(argv[i], "--url-b") && i + 1 < argc)
            urlB = argv[++i];
        else if (!strcmp(argv[i], "--interrupt-test"))
            interruptTest = 1;
        else if (!strcmp(argv[i], "--no-block"))
            noBlock = 1;
        else if (!strcmp(argv[i], "--allow-blocked"))
            allowBlocked = 1;
        else if (!strcmp(argv[i], "--pong-wait") && i + 1 < argc)
            pongWaitMs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--destroy-test"))
            destroyTest = 1;
    }

    /* The control resolves AGB_VIEW_BACKEND with getenv(), which reads the CRT
     * environment: SetEnvironmentVariable() is not guaranteed to reach it, so
     * put it in through the CRT (putenv / _putenv_s). */
#ifdef _MSC_VER
    _putenv_s("AGB_VIEW_BACKEND", backend);
#else
    {
        static char envbuf[64];

        snprintf(envbuf, sizeof(envbuf), "AGB_VIEW_BACKEND=%s", backend);
        putenv(envbuf);
    }
#endif

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = hostProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "AgaveMultiViewProbe";
    RegisterClassA(&wc);

    host = CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP,
                           0, 0, 1100, 800, NULL, NULL, wc.hInstance, NULL);
    if (!host) {
        printf("host_ms=-1\n");
        return 1;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_message = onMessage;
    callbacks.on_title = onTitle;
    callbacks.on_progress = onProgress;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    a.view = agave_webview_create(host, 0, 0, 1100, 800, &callbacks, &a);
    b.view = agave_webview_create(host, 0, 0, 1100, 800, &callbacks, &b);

    if (!a.view || !b.view) {
        printf("views=-1\n");
        return 1;
    }

    agave_webview_navigate(a.view, urlA);
    agave_webview_navigate(b.view, urlB);

    if (!waitFlag(&a.loaded, 15000) || !waitFlag(&b.loaded, 15000)) {
        printf("loads=%ld/%ld titles='%s'/'%s' urls='%s'/'%s'\n",
               (long)a.loaded, (long)b.loaded,
               agave_webview_title(a.view) ? agave_webview_title(a.view) : "?",
               agave_webview_title(b.view) ? agave_webview_title(b.view) : "?",
               agave_webview_url(a.view) ? agave_webview_url(a.view) : "?",
               agave_webview_url(b.view) ? agave_webview_url(b.view) : "?");
        printf("pong_ms=-1\n");   /* a page never loaded: report the failure */
        return 1;
    }

    /* A gets stuck; B must still answer. */
    if (!noBlock) {
        agave_webview_eval(a.view, "while (true) {}");
        pump(150);
    }

    t0 = (LONG)GetTickCount();
    agave_webview_eval(b.view, "document.title='pong'");

    if (waitFlag(&b.pongAt, pongWaitMs > 0 ? pongWaitMs : 6000))
        pongMs = b.pongAt - t0;

    printf("pong_ms=%ld\n", (long)pongMs);

    /* Local backend baseline: the shared engine thread is inside view A's
     * script, and an eval is not a "user action" the watchdog aborts for, so B
     * does not answer at all here. That is the serialisation S3 removes; the
     * suite expects it from the local run. */
    if (pongMs < 0 && allowBlocked)
        printf("blocked=1\n");

    if (interruptTest) {
        t0 = (LONG)GetTickCount();
        agave_webview_interrupt(a.view);
        agave_webview_eval(a.view, "document.title='alive'");

        if (waitFlag(&a.aliveAt, 3000))
            aliveMs = a.aliveAt - t0;

        printf("alive_ms=%ld\n", (long)aliveMs);
    }

    if (destroyTest && a.view) {
        LONG t = (LONG)GetTickCount();

        agave_webview_destroy(a.view);
        a.view = NULL;
        printf("destroy_ms=%ld\n", (long)((LONG)GetTickCount() - t));
    }

    if (a.view)
        agave_webview_destroy(a.view);
    agave_webview_destroy(b.view);
    DestroyWindow(host);

    if (pongMs < 0 && !allowBlocked)
        return 1;

    if (interruptTest && aliveMs < 0)
        return 1;

    return 0;
}
