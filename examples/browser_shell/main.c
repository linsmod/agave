/*
 * main.c - Agave BrowserShell
 *
 * A small Win32 browser shell on top of the macross_* C API:
 *   - toolbar: back / forward / reload / stop / address bar / go
 *   - content area: the engine renders straight into a DIB section
 *   - status bar: title, progress and hovered link
 *   - keyboard: Ctrl+L (focus address), F5, Esc, Alt+Left/Right, Ctrl+0/+/-
 *
 * Build (from the repo root, after the engine has been built):
 *   zig cc -target x86_64-windows-gnu -O2 ^
 *       examples/browser_shell/main.c ^
 *       -o build-zig-native/agave_shell.exe ^
 *       -Iinclude -Ibuild-zig-native ^
 *       build-zig-native/libagave.dll.a -luser32 -lgdi32 -lshell32 -lcomdlg32
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "macross.h"

#define TOOLBAR_HEIGHT  38
#define STATUSBAR_HEIGHT 22
#define BUTTON_WIDTH    32

#define ID_ADDRESS  1001
#define ID_BACK     1002
#define ID_FORWARD  1003
#define ID_RELOAD   1004
#define ID_STOP     1005
#define ID_GO       1006

static HWND         g_hwnd;
static HWND         g_address;
static HWND         g_back;
static HWND         g_forward;
static HWND         g_reload;
static HWND         g_stop;
static HWND         g_go;

static MaCrossView* g_view;
static MC_BYTE*     g_bits;
static HBITMAP      g_bitmap;
static HDC          g_memdc;
static HGDIOBJ      g_oldBitmap;

static int          g_viewWidth;
static int          g_viewHeight;
static volatile int g_dirty;
static volatile int g_quit;

static char         g_url[2048];
static char         g_title[512];
static char         g_statusText[512];
static int          g_progress;
static int          g_loading;
static char         g_initialUrl[2048];

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */
/* ------------------------------------------------------------------------- */

static void statusSet(const char* text)
{
    if (!text)
        text = "";

    strncpy(g_statusText, text, sizeof(g_statusText) - 1);
    g_statusText[sizeof(g_statusText) - 1] = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void urlNormalize(const char* input, char* out, size_t outSize)
{
    size_t i;
    int hasDot = 0;
    int hasColon = 0;
    int hasSpace = 0;

    if (!input || !input[0]) {
        _snprintf(out, outSize, "about:blank");
        out[outSize - 1] = 0;
        return;
    }

    for (i = 0; input[i]; ++i) {
        if (input[i] == '.') hasDot = 1;
        if (input[i] == ':') hasColon = 1;
        if (input[i] == ' ') hasSpace = 1;
    }

    if (hasSpace || (!hasDot && !hasColon)) {
        /* Not a URL: treat it as a search query. */
        _snprintf(out, outSize, "https://www.baidu.com/s?wd=%s", input);
        out[outSize - 1] = 0;
        return;
    }

    if (!hasColon) {
        _snprintf(out, outSize, "http://%s", input);
        out[outSize - 1] = 0;
        return;
    }

    strncpy(out, input, outSize - 1);
    out[outSize - 1] = 0;
}

static void navigate(const char* text)
{
    char normalized[2048];

    urlNormalize(text, normalized, sizeof(normalized));

    strncpy(g_url, normalized, sizeof(g_url) - 1);
    g_url[sizeof(g_url) - 1] = 0;

    SetWindowTextA(g_address, g_url);

    MaCrossView* view = g_view;
    if (view)
        macross_view_open_url(view, g_url);

    SetFocus(g_hwnd);
}

/* ------------------------------------------------------------------------- */
/* engine callbacks                                                           */
/* ------------------------------------------------------------------------- */

static void onInvalidate(MaCrossView* view, const MC_RECT* rect)
{
    (void)view; (void)rect;
    g_dirty = 1;
}

static void onUpdateNow(MaCrossView* view)
{
    (void)view;
    g_dirty = 1;
}

static void onTitle(MaCrossView* view, const char* title)
{
    (void)view;

    if (title) {
        strncpy(g_title, title, sizeof(g_title) - 1);
        g_title[sizeof(g_title) - 1] = 0;
    } else {
        g_title[0] = 0;
    }

    {
        char windowTitle[600];
        _snprintf(windowTitle, sizeof(windowTitle), "%s - Agave",
                  g_title[0] ? g_title : "BrowserShell");
        windowTitle[sizeof(windowTitle) - 1] = 0;
        SetWindowTextA(g_hwnd, windowTitle);
    }
}

static void onLocation(MaCrossView* view, const char* location)
{
    (void)view;

    if (!location)
        return;

    strncpy(g_url, location, sizeof(g_url) - 1);
    g_url[sizeof(g_url) - 1] = 0;

    /* Do not fight the user while they are typing. */
    if (GetFocus() != g_address)
        SetWindowTextA(g_address, g_url);
}

static void onStatus(MaCrossView* view, const char* text)
{
    (void)view;
    statusSet(text);
}

static void onProgress(MaCrossView* view, unsigned int percent, MC_BOOL finished)
{
    (void)view;

    g_progress = (int)percent;
    g_loading = finished ? 0 : 1;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void onError(int err, const char* failedUrl)
{
    char message[1024];

    _snprintf(message, sizeof(message), "Load failed (%d): %s", err,
              failedUrl ? failedUrl : "");
    message[sizeof(message) - 1] = 0;
    statusSet(message);
}

static void onCursor(MC_CURSOR cursor)
{
    LPCSTR name = IDC_ARROW;

    switch (cursor) {
    case HAND_CURSOR:            name = IDC_HAND; break;
    case IBEAM_CURSOR:           name = IDC_IBEAM; break;
    case CROSS_CURSOR:           name = IDC_CROSS; break;
    case WAIT_CURSOR:            name = IDC_WAIT; break;
    case NORTHSOUTH_CURSOR:      name = IDC_SIZENS; break;
    case EASTWEST_CURSOR:        name = IDC_SIZEWE; break;
    case NORTHWESTSOUTHEAST_CURSOR: name = IDC_SIZENWSE; break;
    case NORTHEASTSOUTHWEST_CURSOR: name = IDC_SIZENESW; break;
    case PROGRESS_CURSOR:        name = IDC_WAIT; break;
    default: break;
    }

    SetCursor(LoadCursorA(NULL, name));
}

/* ------------------------------------------------------------------------- */
/* view buffer                                                                */
/* ------------------------------------------------------------------------- */

static void destroySurface(void)
{
    if (g_view) {
        macross_view_destroy(g_view);
        g_view = NULL;
    }

    if (g_memdc) {
        SelectObject(g_memdc, g_oldBitmap);
        DeleteDC(g_memdc);
        g_memdc = NULL;
    }

    if (g_bitmap) {
        DeleteObject(g_bitmap);
        g_bitmap = NULL;
    }

    g_bits = NULL;
}

static void createSurface(HWND hwnd, int width, int height)
{
    BITMAPINFO info;
    HDC hdc;

    if (width < 1) width = 1;
    if (height < 1) height = 1;

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;   /* top-down */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    hdc = GetDC(hwnd);

    g_bitmap = CreateDIBSection(hdc, &info, DIB_RGB_COLORS, (void**)&g_bits, NULL, 0);
    if (g_bitmap) {
        g_memdc = CreateCompatibleDC(hdc);
        g_oldBitmap = SelectObject(g_memdc, g_bitmap);
    }

    ReleaseDC(hwnd, hdc);

    g_viewWidth = width;
    g_viewHeight = height;
}

static void resizeSurface(HWND hwnd, int width, int height)
{
    if (g_view && width == g_viewWidth && height == g_viewHeight)
        return;

    destroySurface();
    createSurface(hwnd, width, height);

    if (!g_bits)
        return;

    g_view = macross_view_create(g_bits, g_viewWidth, g_viewHeight, g_viewWidth * 4, NULL);

    if (g_view)
        macross_view_set_focus(g_view);

    g_dirty = 1;
}

/* ------------------------------------------------------------------------- */
/* input                                                                      */
/* ------------------------------------------------------------------------- */

static unsigned int currentModifiers(void)
{
    unsigned int modifier = 0;

    if (GetKeyState(VK_MENU) & 0x8000)    modifier |= MF_ALT;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifier |= MF_CTRL;
    if (GetKeyState(VK_SHIFT) & 0x8000)   modifier |= MF_SHIFT;

    return modifier;
}

static void sendMouse(MC_MOUSE_EVENT_TYPE type, int x, int y, MC_MOUSE_BUTTON button)
{
    MC_MOUSE_EVENT event;

    if (!g_view)
        return;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.button = button;
    event.point.x = x;
    event.point.y = y;
    event.modifier = currentModifiers();

    macross_mouse_event(g_view, &event);
}

/* ------------------------------------------------------------------------- */
/* painting                                                                   */
/* ------------------------------------------------------------------------- */

static void paintWindow(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    if (g_memdc && g_bits) {
        BitBlt(hdc, 0, TOOLBAR_HEIGHT, g_viewWidth, g_viewHeight,
               g_memdc, 0, 0, SRCCOPY);
    } else {
        RECT fill;

        SetRect(&fill, 0, TOOLBAR_HEIGHT, g_viewWidth, TOOLBAR_HEIGHT + g_viewHeight);
        FillRect(hdc, &fill, (HBRUSH)GetStockObject(WHITE_BRUSH));
    }

    /* Progress bar: a thin strip under the toolbar. */
    if (g_loading) {
        RECT strip;
        int width = (g_viewWidth * (g_progress > 0 ? g_progress : 5)) / 100;

        SetRect(&strip, 0, TOOLBAR_HEIGHT - 3, width, TOOLBAR_HEIGHT);
        FillRect(hdc, &strip, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }

    /* Status bar. */
    {
        RECT bar;
        char text[700];

        SetRect(&bar, 0, TOOLBAR_HEIGHT + g_viewHeight, g_viewWidth,
                TOOLBAR_HEIGHT + g_viewHeight + STATUSBAR_HEIGHT);

        FillRect(hdc, &bar, (HBRUSH)(COLOR_BTNFACE + 1));

        _snprintf(text, sizeof(text), "%s%s",
                  g_loading ? "Loading... " : "",
                  g_statusText[0] ? g_statusText : (g_url[0] ? g_url : "Ready"));
        text[sizeof(text) - 1] = 0;

        bar.left += 6;
        SetBkMode(hdc, TRANSPARENT);
        DrawTextA(hdc, text, -1, &bar, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }

    EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------------- */
/* window procedure                                                           */
/* ------------------------------------------------------------------------- */

static void layoutChildren(HWND hwnd, int width, int height)
{
    int x = 4;
    int editWidth;

    MoveWindow(g_back,    x, 7, BUTTON_WIDTH, 24, TRUE); x += BUTTON_WIDTH + 2;
    MoveWindow(g_forward, x, 7, BUTTON_WIDTH, 24, TRUE); x += BUTTON_WIDTH + 2;
    MoveWindow(g_reload,  x, 7, BUTTON_WIDTH, 24, TRUE); x += BUTTON_WIDTH + 2;
    MoveWindow(g_stop,    x, 7, BUTTON_WIDTH, 24, TRUE); x += BUTTON_WIDTH + 6;

    editWidth = width - x - BUTTON_WIDTH - 12;
    if (editWidth < 60)
        editWidth = 60;

    MoveWindow(g_address, x, 7, editWidth, 24, TRUE); x += editWidth + 6;
    MoveWindow(g_go,      x, 7, BUTTON_WIDTH + 16, 24, TRUE);

    (void)height;
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        int viewHeight = height - TOOLBAR_HEIGHT - STATUSBAR_HEIGHT;

        if (viewHeight < 1)
            viewHeight = 1;

        layoutChildren(hwnd, width, height);
        resizeSurface(hwnd, width, viewHeight);
        return 0;
    }

    case WM_PAINT:
        paintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SETFOCUS:
        if (g_view)
            macross_view_set_focus(g_view);
        return 0;

    case WM_TIMER:
        macross_event_dispatch();

        /* Paint every tick, like a real frame loop: the engine marks the dirty
         * rect internally and paintView() becomes a no-op when nothing changed. */
        if (g_view) {
            g_dirty = 0;
            macross_view_update(g_view, NULL);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
        MC_MOUSE_EVENT_TYPE type = EVT_MOUSE_MOVE;
        MC_MOUSE_BUTTON button = MOUSE_BTN_NONE;
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam) - TOOLBAR_HEIGHT;

        if (y < 0 || y >= g_viewHeight)
            return 0;

        if (message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN)
            type = EVT_MOUSE_DOWN;
        else if (message == WM_LBUTTONUP || message == WM_MBUTTONUP || message == WM_RBUTTONUP)
            type = EVT_MOUSE_UP;

        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)
            button = MOUSE_BTN_LEFT;
        else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)
            button = MOUSE_BTN_MIDDLE;
        else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
            button = MOUSE_BTN_RIGHT;

        if (message == WM_LBUTTONDOWN)
            SetFocus(hwnd);

        sendMouse(type, x, y, button);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT point;
        int delta = (short)HIWORD(wParam);

        point.x = (short)LOWORD(lParam);
        point.y = (short)HIWORD(lParam);
        ScreenToClient(hwnd, &point);

        sendMouse(EVT_MOUSE_WHEEL, point.x, point.y - TOOLBAR_HEIGHT,
                  delta > 0 ? MOUSE_WHEEL_UP : MOUSE_WHEEL_DOWN);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP: {
        MC_KEY_EVENT event;

        /* Alt+Left / Alt+Right: history navigation */
        if (message == WM_KEYDOWN && (GetKeyState(VK_MENU) & 0x8000)) {
            if (wParam == VK_LEFT) {
                if (g_view) macross_view_backward(g_view);
                return 0;
            }
            if (wParam == VK_RIGHT) {
                if (g_view) macross_view_forward(g_view);
                return 0;
            }
        }

        if (message == WM_KEYDOWN) {
            if (wParam == VK_F5) {
                if (g_view) macross_view_reload(g_view);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (g_view) macross_view_stop(g_view);
                return 0;
            }
            if (wParam == VK_F6 || ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'L')) {
                SetFocus(g_address);
                SendMessageA(g_address, EM_SETSEL, 0, -1);
                return 0;
            }
        }

        if (!g_view)
            return 0;

        memset(&event, 0, sizeof(event));
        event.type = (message == WM_KEYDOWN) ? EVT_KEY_DOWN : EVT_KEY_UP;
        event.key = (MC_VIRTUAL_KEY)wParam;
        event.modifier = currentModifiers();

        macross_keyboard_event(g_view, &event);
        return 0;
    }

    case WM_CHAR: {
        char utf8[8];
        int length = 0;
        unsigned int code = (unsigned int)wParam;

        if (!g_view)
            return 0;

        if (code < 0x80) {
            utf8[length++] = (char)code;
        } else if (code < 0x800) {
            utf8[length++] = (char)(0xC0 | (code >> 6));
            utf8[length++] = (char)(0x80 | (code & 0x3F));
        } else {
            utf8[length++] = (char)(0xE0 | (code >> 12));
            utf8[length++] = (char)(0x80 | ((code >> 6) & 0x3F));
            utf8[length++] = (char)(0x80 | (code & 0x3F));
        }
        utf8[length] = 0;

        macross_view_input_text(g_view, utf8);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);


        switch (id) {
        case ID_GO: {
            char buffer[2048];

            GetWindowTextA(g_address, buffer, sizeof(buffer));
            navigate(buffer);
            return 0;
        }
        case ID_BACK:    if (g_view) macross_view_backward(g_view); return 0;
        case ID_FORWARD: if (g_view) macross_view_forward(g_view); return 0;
        case ID_RELOAD:  if (g_view) macross_view_reload(g_view);   return 0;
        case ID_STOP:    if (g_view) macross_view_stop(g_view);     return 0;
        default: break;
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        destroySurface();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

/* ------------------------------------------------------------------------- */
/* entry point                                                                */
/* ------------------------------------------------------------------------- */

static WNDPROC g_editProc;

/* The address bar only navigates on Enter: handling EN_CHANGE would recurse,
 * because SetWindowTextA notifies us right back. */
static LRESULT CALLBACK addressProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            char buffer[2048];

            GetWindowTextA(hwnd, buffer, sizeof(buffer));
            navigate(buffer);
            return 0;
        }

        if (wParam == VK_ESCAPE) {
            SetFocus(g_hwnd);
            return 0;
        }
    }

    return CallWindowProcA(g_editProc, hwnd, message, wParam, lParam);
}

static void addressSubclass(HWND hwnd)
{
    g_editProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)addressProc);
}

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show)
{
    WNDCLASSA wc;
    MSG message;
    MC_CALLBACK_INFO callbacks;
    int argc = 0;
    char** argv = __argv;

    (void)previous;
    (void)show;

    while (argv && argv[argc])
        ++argc;

    if (argc > 1) {
        strncpy(g_initialUrl, argv[1], sizeof(g_initialUrl) - 1);
        g_initialUrl[sizeof(g_initialUrl) - 1] = 0;
    } else {
        strcpy(g_initialUrl, "http://www.163.com");
    }

    (void)commandLine;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AgaveShellWindow";
    wc.lpszMenuName = NULL;

    if (!RegisterClassA(&wc))
        return 1;

    g_hwnd = CreateWindowExA(0, "AgaveShellWindow", "Agave BrowserShell",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
                             NULL, NULL, instance, NULL);

    if (!g_hwnd)
        return 1;

    if (getenv("AGAVE_SHELL_DEBUG")) fprintf(stderr, "[shell] window created\n");

    g_back    = CreateWindowA("BUTTON", "<",     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_BACK,    instance, NULL);
    g_forward = CreateWindowA("BUTTON", ">",     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_FORWARD, instance, NULL);
    g_reload  = CreateWindowA("BUTTON", "R",     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_RELOAD,  instance, NULL);
    g_stop    = CreateWindowA("BUTTON", "X",     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_STOP,    instance, NULL);
    g_address = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                0, 0, 0, 0, g_hwnd, (HMENU)ID_ADDRESS, instance, NULL);
    g_go      = CreateWindowA("BUTTON", "Go",    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_GO,      instance, NULL);

    addressSubclass(g_address);

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.cb_invalidate_rect  = onInvalidate;
    callbacks.cb_update_view_now  = onUpdateNow;
    callbacks.cb_set_title        = onTitle;
    callbacks.cb_set_location     = onLocation;
    callbacks.cb_set_status       = onStatus;
    callbacks.cb_loading_progress = onProgress;
    callbacks.cb_error            = onError;
    callbacks.cb_set_cursor       = onCursor;

    macross_set_callback(&callbacks);

    if (getenv("AGAVE_SHELL_DEBUG")) fprintf(stderr, "[shell] callbacks set\n");

    if (macross_initialize(PIXEL_FORMAT_BGRA32, 1100, 800) != MC_STATUS_SUCCESS) {
        MessageBoxA(NULL, "macross_initialize failed", "Agave BrowserShell", MB_ICONERROR);
        return 1;
    }

    if (getenv("AGAVE_SHELL_DEBUG")) fprintf(stderr, "[shell] initialized\n");

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (getenv("AGAVE_SHELL_DEBUG")) fprintf(stderr, "[shell] shown\n");

    /* WM_SIZE already built the surface; now start loading. */
    navigate(g_initialUrl);

    if (g_view)
        macross_view_repaint(g_view);

    if (getenv("AGAVE_SHELL_DEBUG")) fprintf(stderr, "[shell] navigate issued\n");

    SetFocus(g_address);
    SendMessageA(g_address, EM_SETSEL, 0, -1);

    SetTimer(g_hwnd, 1, 8, NULL);

    while (!g_quit) {
        DWORD result = MsgWaitForMultipleObjects(0, NULL, FALSE, 4, QS_ALLINPUT);

        if (result == WAIT_OBJECT_0) {
            while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    g_quit = 1;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
        }

        macross_event_dispatch();
    }

    macross_shutdown();
    return 0;
}
