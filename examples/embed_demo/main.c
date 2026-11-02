/*
 * main.c - AgaveWebView embedding demo
 *
 * Two independent WebView controls inside one host window: each has its own
 * engine view, buffer and callbacks, which is what an embedding host needs.
 *
 * Build: cmake -DOPT_SHELL=ON  (targets agave_embed)
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "agave_webview.h"

#define ID_NAV_LEFT   2001
#define ID_NAV_RIGHT  2002
#define ID_ADDR_LEFT  2003
#define ID_ADDR_RIGHT 2004

static HWND          g_hwnd;
static HWND          g_addrLeft;
static HWND          g_addrRight;
static char          g_leftUrl[2048];
static char          g_rightUrl[2048];
static char          g_status[600];
static AgaveWebView* g_left;
static AgaveWebView* g_right;

static void paintStatus(HDC hdc)
{
    RECT client;
    RECT bar;

    GetClientRect(g_hwnd, &client);
    SetRect(&bar, 0, client.bottom - 24, client.right, client.bottom);
    FillRect(hdc, &bar, (HBRUSH)(COLOR_BTNFACE + 1));

    bar.left += 8;
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, g_status, -1, &bar, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
}

static void onTitle(AgaveWebView* view, const char* title, void* user)
{
    (void)view;
    (void)user;

    SetWindowTextA(g_hwnd, title && title[0] ? title : "Agave embed demo");
}

static void onUrl(AgaveWebView* view, const char* url, void* user)
{
    (void)user;

    if (view == g_left) {
        strncpy(g_leftUrl, url, sizeof(g_leftUrl) - 1);
        if (GetFocus() != g_addrLeft)
            SetWindowTextA(g_addrLeft, g_leftUrl);
    } else {
        strncpy(g_rightUrl, url, sizeof(g_rightUrl) - 1);
        if (GetFocus() != g_addrRight)
            SetWindowTextA(g_addrRight, g_rightUrl);
    }
}

static void onProgress(AgaveWebView* view, int percent, int finished, void* user)
{
    (void)view;
    (void)user;

    _snprintf(g_status, sizeof(g_status), "%s: %d%%%s",
              view == g_left ? "left" : "right", percent, finished ? " (done)" : "");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* window.open(): the control already created the popup; place it on top and
 * keep it (returning 0 would close it). */
static int onNewWindow(AgaveWebView* view, AgaveWebView* popup, const char* url, void* user)
{
    (void)view;
    (void)user;

    agave_webview_set_rect(popup, 200, 100, 640, 520);
    agave_webview_set_zoom(popup, 100);

    _snprintf(g_status, sizeof(g_status), "popup: %s", url ? url : "");
    InvalidateRect(g_hwnd, NULL, FALSE);

    return 1;
}

static void onError(AgaveWebView* view, int code, const char* url, void* user)
{
    (void)view;
    (void)user;

    _snprintf(g_status, sizeof(g_status), "error %d: %s", code, url ? url : "");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK hostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        int paneWidth = (width - 12) / 2;
        int paneHeight = height - 80;

        if (paneWidth < 40) paneWidth = 40;
        if (paneHeight < 40) paneHeight = 40;

        MoveWindow(g_addrLeft, 8, 8, paneWidth, 24, TRUE);
        MoveWindow(g_addrRight, paneWidth + 16, 8, paneWidth, 24, TRUE);

        if (g_left)
            agave_webview_set_rect(g_left, 8, 40, paneWidth, paneHeight);
        if (g_right)
            agave_webview_set_rect(g_right, paneWidth + 16, 40, paneWidth, paneHeight);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        paintStatus(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND: {
        char buffer[2048];

        if (LOWORD(wParam) == ID_NAV_LEFT) {
            GetWindowTextA(g_addrLeft, buffer, sizeof(buffer));
            if (g_left)
                agave_webview_navigate(g_left, buffer);
            return 0;
        }

        if (LOWORD(wParam) == ID_NAV_RIGHT) {
            GetWindowTextA(g_addrRight, buffer, sizeof(buffer));
            if (g_right)
                agave_webview_navigate(g_right, buffer);
            return 0;
        }

        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show)
{
    WNDCLASSA wc;
    MSG message;
    AgaveWebViewCallback callbacks;
    HINSTANCE host = instance;

    (void)previous;
    (void)show;
    (void)commandLine;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = hostProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AgaveEmbedDemo";

    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, "AgaveEmbedDemo", "Agave embed demo",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1200, 800, NULL, NULL, instance, NULL);

    if (!g_hwnd)
        return 1;

    g_addrLeft = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 0, 0, 0, 0, g_hwnd, (HMENU)ID_ADDR_LEFT, host, NULL);
    g_addrRight = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  0, 0, 0, 0, g_hwnd, (HMENU)ID_ADDR_RIGHT, host, NULL);

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_title = onTitle;
    callbacks.on_url = onUrl;
    callbacks.on_progress = onProgress;
    callbacks.on_error = onError;
    callbacks.on_new_window = onNewWindow;

    g_left = agave_webview_create(g_hwnd, 8, 40, 560, 660, &callbacks, NULL);
    g_right = agave_webview_create(g_hwnd, 600, 40, 560, 660, &callbacks, NULL);

    if (!g_left || !g_right) {
        MessageBoxA(NULL, "agave_webview_create failed", "Agave embed demo", MB_ICONERROR);
        return 1;
    }

    {
        const char* leftUrl = "http://127.0.0.1:8899/css3.html";
        char** argv = __argv;
        int argc = 0;

        while (argv && argv[argc])
            ++argc;

        if (argc > 1)
            leftUrl = argv[1];

        strncpy(g_leftUrl, leftUrl, sizeof(g_leftUrl) - 1);
        SetWindowTextA(g_addrLeft, g_leftUrl);
        {
            const char* rightUrl = (argc > 2) ? argv[2] : "https://www.163.com";

            SetWindowTextA(g_addrRight, rightUrl);
            agave_webview_navigate(g_left, g_leftUrl);
            agave_webview_navigate(g_right, rightUrl);
        }
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    while (GetMessageA(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    if (g_left)  agave_webview_destroy(g_left);
    if (g_right) agave_webview_destroy(g_right);

    return 0;
}
