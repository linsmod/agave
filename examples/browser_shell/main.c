/*
 * main.c - Agave BrowserShell (dual-threaded, tabbed Win32 shell)
 *
 * Threading model
 * ---------------
 *   UI thread    : window, controls, message pump, BitBlt of the active tab.
 *   engine thread: owns every MaCrossView, drains the command queue, runs
 *                  macross_event_dispatch() + macross_view_update().
 *
 * Nothing in the engine is touched from the UI thread. Input and navigation are
 * posted into a locked command queue; engine callbacks only publish state into
 * per-tab fields and PostMessage() the UI so it repaints itself.
 *
 * Build: cmake -DOPT_SHELL=ON && ninja agave_shell
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "macross.h"

#define TOOLBAR_HEIGHT    38
#define TABBAR_HEIGHT     28
#define STATUSBAR_HEIGHT  22
#define BUTTON_WIDTH      32
#define MAX_TABS          12
#define MAX_PATH_CHARS    2048

#define ID_ADDRESS  1001
#define ID_BACK     1002
#define ID_FORWARD  1003
#define ID_RELOAD   1004
#define ID_STOP     1005
#define ID_GO       1006
#define ID_NEWTAB   1007
#define ID_FIND     1008
#define ID_MENU     1009

/* UI -> engine */
enum CommandType {
    CMD_NAVIGATE, CMD_BACK, CMD_FORWARD, CMD_RELOAD, CMD_STOP,
    CMD_MOUSE, CMD_KEY, CMD_CHAR, CMD_RESIZE, CMD_NEW_TAB, CMD_FIND, CMD_CLOSE_TAB, CMD_ZOOM
};

typedef struct {
    int  type;
    int  tab;
    int  x, y, button, modifier, key;
    char text[MAX_PATH_CHARS];
} ShellCommand;

/* engine -> UI */
#define WM_SHELL_STATE   (WM_APP + 1)

typedef struct Tab {
    MaCrossView* view;
    MC_BYTE*     bits;
    HBITMAP      bitmap;
    HDC          memdc;
    HGDIOBJ      oldBitmap;
    int          width;
    int          height;
    int          id;
    char         url[MAX_PATH_CHARS];
    char         title[256];
    char         status[512];
    int          progress;
    int          loading;
    int          wantWidth;
    int          wantHeight;
    int          closed;
} Tab;

static CRITICAL_SECTION   g_lock;
static ShellCommand       g_queue[512];
static int                g_queueHead;
static int                g_queueTail;
static volatile int       g_engineQuit;

static Tab                g_tabs[MAX_TABS];
static int                g_tabCount;
static int                g_activeTab;
static int                g_nextTabId = 1;

static HWND               g_hwnd;
static HWND               g_address;
static HWND               g_find;
static HWND               g_buttons[7];

#define AGW_MAX_BOOKMARKS 64
#define AGW_MAX_HISTORY   200

typedef struct { char url[1024]; char title[256]; } Bookmark;

static Bookmark           g_bookmarks[AGW_MAX_BOOKMARKS];
static int                g_bookmarkCount;
static char               g_history[AGW_MAX_HISTORY][1100];
static int                g_historyCount;
static int                g_zoom = 100;
static char               g_dataDir[MAX_PATH_CHARS];

static int                g_tabOrder[MAX_TABS];
static int                g_tabOrderCount;

static char               g_initialUrl[MAX_PATH_CHARS];
static char               g_downloadDir[MAX_PATH_CHARS];

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */
/* ------------------------------------------------------------------------- */

static void lockAcquire(void)   { EnterCriticalSection(&g_lock); }
static void lockRelease(void)   { LeaveCriticalSection(&g_lock); }

static Tab* tabByView(MaCrossView* view)
{
    int i;

    for (i = 0; i < g_tabCount; ++i) {
        if (g_tabs[i].view == view)
            return &g_tabs[i];
    }
    return NULL;
}

static void addTab(const char* url);
static void resizeActiveTab(void);
static void syncUiFromState(void);
static void postCommand(const ShellCommand* command);
static void notifyUi(void);
static void addBookmarkCurrent(void);
static void navigateTo(const char* text, int inNewTab);
static void addBookmark(const char* url, const char* title);
static void recordHistory(const char* url);
static int loadSession(char urls[][MAX_PATH_CHARS], int max);
static void saveSession(void);

static void addTab(const char* url);
static void resizeActiveTab(void);
static void syncUiFromState(void);

static void rebuildTabOrder(void)
{
    int i;

    g_tabOrderCount = 0;
    for (i = 0; i < g_tabCount; ++i) {
        if (!g_tabs[i].closed)
            g_tabOrder[g_tabOrderCount++] = i;
    }
}

static void activateTab(int index)
{
    if (index < 0 || index >= g_tabCount || g_tabs[index].closed)
        return;

    g_activeTab = index;
    resizeActiveTab();
    syncUiFromState();
}

static void closeTab(int index)
{
    ShellCommand command;
    int i;

    if (index < 0 || index >= g_tabCount || g_tabs[index].closed)
        return;

    g_tabs[index].closed = 1;

    memset(&command, 0, sizeof(command));
    command.type = CMD_CLOSE_TAB;
    command.tab = index;
    postCommand(&command);

    rebuildTabOrder();

    if (g_tabOrderCount == 0) {
        /* Closing the last tab opens a fresh one instead of leaving nothing. */
        addTab("about:blank");
        return;
    }

    if (g_activeTab == index) {
        for (i = 0; i < g_tabOrderCount; ++i) {
            if (g_tabOrder[i] > index) {
                activateTab(g_tabOrder[i]);
                break;
            }
        }
        if (g_activeTab == index)
            activateTab(g_tabOrder[g_tabOrderCount - 1]);
    } else {
        notifyUi();
    }
}

static void postCommand(const ShellCommand* command)
{
    lockAcquire();

    if (((g_queueTail + 1) % (int)(sizeof(g_queue) / sizeof(g_queue[0]))) != g_queueHead) {
        g_queue[g_queueTail] = *command;
        g_queueTail = (g_queueTail + 1) % (int)(sizeof(g_queue) / sizeof(g_queue[0]));
    }
    lockRelease();
}

static void postSimple(int type, int tab)
{
    ShellCommand command;

    memset(&command, 0, sizeof(command));
    command.type = type;
    command.tab = tab;
    postCommand(&command);
}

static void notifyUi(void)
{
    if (g_hwnd)
        PostMessageA(g_hwnd, WM_SHELL_STATE, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* URL handling                                                               */
/* ------------------------------------------------------------------------- */

static void urlNormalize(const char* input, char* out, size_t outSize)
{
    size_t i;
    int hasDot = 0, hasColon = 0, hasSpace = 0;

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
        _snprintf(out, outSize, "https://www.baidu.com/s?wd=%s", input);
        out[outSize - 1] = 0;
    } else if (!hasColon) {
        _snprintf(out, outSize, "http://%s", input);
        out[outSize - 1] = 0;
    } else {
        strncpy(out, input, outSize - 1);
        out[outSize - 1] = 0;
    }
}

static void navigateTo(const char* text, int inNewTab)
{
    char normalized[MAX_PATH_CHARS];
    ShellCommand command;

    urlNormalize(text, normalized, sizeof(normalized));

    if (inNewTab) {
        /* Tab bookkeeping lives on the UI thread; addTab() posts the command
         * that makes the engine create the view. */
        addTab(normalized);
        return;
    }

    memset(&command, 0, sizeof(command));
    command.type = CMD_NAVIGATE;
    command.tab = g_activeTab;
    strncpy(command.text, normalized, sizeof(command.text) - 1);
    postCommand(&command);
}

/* ------------------------------------------------------------------------- */
/* engine callbacks (engine thread)                                           */
/* ------------------------------------------------------------------------- */

static void onInvalidate(MaCrossView* view, const MC_RECT* rect)
{
    (void)view; (void)rect;
    notifyUi();
}

static void onUpdateNow(MaCrossView* view)
{
    (void)view;
    notifyUi();
}

static void publishText(MaCrossView* view, const char* text, int field)
{
    Tab* tab = tabByView(view);

    if (!tab || !text)
        return;

    lockAcquire();
    if (field == 0) {
        strncpy(tab->title, text, sizeof(tab->title) - 1);
        tab->title[sizeof(tab->title) - 1] = 0;
    } else if (field == 1) {
        strncpy(tab->url, text, sizeof(tab->url) - 1);
        tab->url[sizeof(tab->url) - 1] = 0;
    } else {
        strncpy(tab->status, text, sizeof(tab->status) - 1);
        tab->status[sizeof(tab->status) - 1] = 0;
    }
    lockRelease();

    notifyUi();
}

static void onTitle(MaCrossView* view, const char* title)   { publishText(view, title, 0); }
static void onLocation(MaCrossView* view, const char* url)  { publishText(view, url, 1); }
static void onStatus(MaCrossView* view, const char* text)   { publishText(view, text, 2); }

static void onProgress(MaCrossView* view, unsigned int percent, MC_BOOL finished)
{
    Tab* tab = tabByView(view);

    if (!tab)
        return;

    lockAcquire();
    tab->progress = (int)percent;
    tab->loading = finished ? 0 : 1;
    lockRelease();

    notifyUi();
}

static void onError(int err, const char* failedUrl)
{
    char message[600];

    _snprintf(message, sizeof(message), "Load failed (%d): %s", err,
              failedUrl ? failedUrl : "");
    message[sizeof(message) - 1] = 0;

    lockAcquire();
    if (g_tabCount > 0) {
        Tab* tab = &g_tabs[g_activeTab];

        strncpy(tab->status, message, sizeof(tab->status) - 1);
        tab->status[sizeof(tab->status) - 1] = 0;
        tab->loading = 0;
    }
    lockRelease();

    notifyUi();
}

static void onCursor(MC_CURSOR cursor)
{
    LPCSTR name = IDC_ARROW;

    switch (cursor) {
    case HAND_CURSOR:       name = IDC_HAND; break;
    case IBEAM_CURSOR:      name = IDC_IBEAM; break;
    case CROSS_CURSOR:      name = IDC_CROSS; break;
    case WAIT_CURSOR:       name = IDC_WAIT; break;
    case NORTHSOUTH_CURSOR: name = IDC_SIZENS; break;
    case EASTWEST_CURSOR:   name = IDC_SIZEWE; break;
    default: break;
    }

    SetCursor(LoadCursorA(NULL, name));
}

/* Downloads: write straight into %USERPROFILE%\Downloads. */
static MC_BOOL onWriteFile(int id, const char* url, const void* data,
                           unsigned long dataLen, unsigned long totalLen)
{
    static FILE* file = NULL;
    static int   currentId = -1;

    (void)totalLen;

    if (currentId != id) {
        char name[MAX_PATH_CHARS];
        char path[MAX_PATH];
        const char* slash = url ? strrchr(url, '/') : NULL;
        size_t i, n = 0;

        if (file) {
            fclose(file);
            file = NULL;
        }

        currentId = id;
        name[0] = 0;

        if (slash && slash[1]) {
            for (i = 0; slash[1 + i] && i < sizeof(name) - 1; ++i) {
                char c = slash[1 + i];

                if (c == '?' || c == '#' || c == '\\' || c == ':' || c == '*' ||
                    c == '"' || c == '<' || c == '>' || c == '|')
                    break;
                name[n++] = c;
            }
        }
        name[n] = 0;

        if (!name[0])
            strcpy(name, "download.bin");

        _snprintf(path, sizeof(path), "%s\\%s", g_downloadDir, name);
        path[sizeof(path) - 1] = 0;

        file = fopen(path, "wb");

        if (file) {
            char message[600];

            _snprintf(message, sizeof(message), "Downloading: %s", path);
            message[sizeof(message) - 1] = 0;

            lockAcquire();
            if (g_tabCount > 0) {
                Tab* tab = &g_tabs[g_activeTab];

                strncpy(tab->status, message, sizeof(tab->status) - 1);
                tab->status[sizeof(tab->status) - 1] = 0;
            }
            lockRelease();
            notifyUi();
        }
    }

    if (file && data && dataLen)
        fwrite(data, 1, dataLen, file);

    return file ? TRUE : FALSE;
}

/* ------------------------------------------------------------------------- */
/* surface management (GDI objects are created on the UI thread)              */
/* ------------------------------------------------------------------------- */

static void tabDestroySurface(Tab* tab)
{
    if (tab->memdc) {
        SelectObject(tab->memdc, tab->oldBitmap);
        DeleteDC(tab->memdc);
        tab->memdc = NULL;
    }
    if (tab->bitmap) {
        DeleteObject(tab->bitmap);
        tab->bitmap = NULL;
    }
    tab->bits = NULL;
}

static int tabCreateSurface(Tab* tab, int width, int height)
{
    BITMAPINFO info;
    HDC hdc;

    if (width < 1) width = 1;
    if (height < 1) height = 1;

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    hdc = GetDC(g_hwnd);
    tab->bitmap = CreateDIBSection(hdc, &info, DIB_RGB_COLORS, (void**)&tab->bits, NULL, 0);

    if (tab->bitmap) {
        tab->memdc = CreateCompatibleDC(hdc);
        tab->oldBitmap = SelectObject(tab->memdc, tab->bitmap);
    }
    ReleaseDC(g_hwnd, hdc);

    tab->width = width;
    tab->height = height;

    return tab->bits != NULL;
}

/* ------------------------------------------------------------------------- */
/* engine thread                                                              */
/* ------------------------------------------------------------------------- */

static void engineCreateView(Tab* tab, const char* url)
{
    if (!tab || !tab->bits)
        return;

    tab->view = macross_view_create(tab->bits, tab->width, tab->height,
                                    tab->width * 4, NULL);

    if (tab->view) {
        if (g_zoom != 100)
            macross_view_set_scale_factor(tab->view, g_zoom);

        if (url && url[0])
            macross_view_open_url(tab->view, url);
    }
}

/* Creates the DIB the engine renders into. Always called on the engine thread,
 * so the UI can never free a buffer that is being painted. */
static void engineApplyResize(Tab* tab, int width, int height)
{
    if (!tab)
        return;

    if (width < 1) width = 1;
    if (height < 1) height = 1;

    if (tab->bits && tab->width == width && tab->height == height && tab->view)
        return;

    tabDestroySurface(tab);

    if (!tabCreateSurface(tab, width, height))
        return;

    if (tab->view)
        macross_view_resize(tab->view, tab->bits, width, height, width * 4);
    else
        engineCreateView(tab, tab->url[0] ? tab->url : g_initialUrl);

    notifyUi();
}

static void engineDrainCommands(void)
{
    for (;;) {
        ShellCommand command;
        int have = 0;

        lockAcquire();
        if (g_queueHead != g_queueTail) {
            command = g_queue[g_queueHead];
            g_queueHead = (g_queueHead + 1) % (int)(sizeof(g_queue) / sizeof(g_queue[0]));
            have = 1;
        }
        lockRelease();

        if (!have)
            break;

        switch (command.type) {
        case CMD_NAVIGATE:
            if (command.tab >= 0 && command.tab < g_tabCount && g_tabs[command.tab].view)
                macross_view_open_url(g_tabs[command.tab].view, command.text);
            break;
        case CMD_NEW_TAB:
            if (command.tab >= 0 && command.tab < g_tabCount) {
                Tab* tab = &g_tabs[command.tab];

                if (command.text[0]) {
                    strncpy(tab->url, command.text, sizeof(tab->url) - 1);
                    tab->url[sizeof(tab->url) - 1] = 0;
                }
                engineApplyResize(tab, tab->wantWidth, tab->wantHeight);
            }
            break;
        case CMD_BACK:
            if (g_activeTab < g_tabCount && g_tabs[g_activeTab].view)
                macross_view_backward(g_tabs[g_activeTab].view);
            break;
        case CMD_FORWARD:
            if (g_activeTab < g_tabCount && g_tabs[g_activeTab].view)
                macross_view_forward(g_tabs[g_activeTab].view);
            break;
        case CMD_RELOAD:
            if (g_activeTab < g_tabCount && g_tabs[g_activeTab].view)
                macross_view_reload(g_tabs[g_activeTab].view);
            break;
        case CMD_STOP:
            if (g_activeTab < g_tabCount && g_tabs[g_activeTab].view)
                macross_view_stop(g_tabs[g_activeTab].view);
            break;
        case CMD_ZOOM:
            if (g_activeTab < g_tabCount && g_tabs[g_activeTab].view)
                macross_view_set_scale_factor(g_tabs[g_activeTab].view, g_zoom);
            break;
        case CMD_FIND:
            if (g_activeTab < g_tabCount && g_tabs[g_activeTab].view && command.text[0])
                macross_view_search_text(g_tabs[g_activeTab].view, command.text,
                                         TRUE, TRUE, FALSE);
            break;
        case CMD_MOUSE:
            if (command.tab >= 0 && command.tab < g_tabCount && g_tabs[command.tab].view) {
                MC_MOUSE_EVENT event;

                memset(&event, 0, sizeof(event));
                event.type = (MC_MOUSE_EVENT_TYPE)command.key;
                event.button = (MC_MOUSE_BUTTON)command.button;
                event.point.x = command.x;
                event.point.y = command.y;
                event.modifier = command.modifier;
                macross_mouse_event(g_tabs[command.tab].view, &event);
            }
            break;
        case CMD_KEY:
            if (command.tab >= 0 && command.tab < g_tabCount && g_tabs[command.tab].view) {
                MC_KEY_EVENT event;

                memset(&event, 0, sizeof(event));
                event.type = (MC_KEY_EVENT_TYPE)command.key;
                event.key = (MC_VIRTUAL_KEY)command.x;
                event.modifier = command.modifier;
                macross_keyboard_event(g_tabs[command.tab].view, &event);
            }
            break;
        case CMD_CHAR:
            if (g_activeTab < g_tabCount && g_tabs[g_activeTab].view)
                macross_view_input_text(g_tabs[g_activeTab].view, command.text);
            break;
        case CMD_CLOSE_TAB:
            if (command.tab >= 0 && command.tab < g_tabCount) {
                Tab* tab = &g_tabs[command.tab];

                if (tab->view) {
                    macross_view_destroy(tab->view);
                    tab->view = NULL;
                }
                tabDestroySurface(tab);
            }
            break;
        case CMD_RESIZE:
            if (command.tab >= 0 && command.tab < g_tabCount)
                engineApplyResize(&g_tabs[command.tab],
                                  g_tabs[command.tab].wantWidth,
                                  g_tabs[command.tab].wantHeight);
            break;
        default:
            break;
        }
    }
}

static DWORD WINAPI engineThread(LPVOID param)
{
    (void)param;

    if (macross_initialize(PIXEL_FORMAT_BGRA32, 1100, 800) != MC_STATUS_SUCCESS)
        return 0;

    lockAcquire();
    if (g_tabCount > 0)
        engineApplyResize(&g_tabs[0], g_tabs[0].wantWidth, g_tabs[0].wantHeight);
    lockRelease();

    notifyUi();

    while (!g_engineQuit) {
        engineDrainCommands();

        macross_event_dispatch();

        lockAcquire();
        if (g_activeTab < g_tabCount && g_tabs[g_activeTab].view)
            macross_view_update(g_tabs[g_activeTab].view, NULL);
        lockRelease();

        Sleep(6);
    }

    lockAcquire();
    {
        int i;

        for (i = 0; i < g_tabCount; ++i) {
            if (g_tabs[i].view) {
                macross_view_destroy(g_tabs[i].view);
                g_tabs[i].view = NULL;
            }
        }
    }
    lockRelease();

    macross_shutdown();
    return 0;
}

/* ------------------------------------------------------------------------- */
/* UI                                                                         */
/* ------------------------------------------------------------------------- */

static int viewTop(void)
{
    return TOOLBAR_HEIGHT + TABBAR_HEIGHT;
}

static int viewHeightFor(int clientHeight)
{
    int height = clientHeight - viewTop() - STATUSBAR_HEIGHT;

    return height > 0 ? height : 1;
}

/* The engine thread owns the DIB: the UI only records the size it wants. This
 * avoids freeing a buffer the engine may still be painting into. */
static void requestViewSize(int index, int width, int height)
{
    ShellCommand command;

    if (index < 0 || index >= g_tabCount)
        return;

    lockAcquire();
    g_tabs[index].wantWidth = width;
    g_tabs[index].wantHeight = height;
    lockRelease();

    memset(&command, 0, sizeof(command));
    command.type = CMD_RESIZE;
    command.tab = index;
    command.x = width;
    command.y = height;
    postCommand(&command);
}

static void resizeActiveTab(void)
{
    RECT client;
    int height;

    GetClientRect(g_hwnd, &client);
    height = viewHeightFor(client.bottom);
    requestViewSize(g_activeTab, client.right, height);

    notifyUi();
}

static void layoutChildren(HWND hwnd, int width, int height)
{
    int x = 4;

    MoveWindow(g_buttons[0], x, 7, BUTTON_WIDTH, 24, TRUE); x += BUTTON_WIDTH + 2;
    MoveWindow(g_buttons[1], x, 7, BUTTON_WIDTH, 24, TRUE); x += BUTTON_WIDTH + 2;
    MoveWindow(g_buttons[2], x, 7, BUTTON_WIDTH, 24, TRUE); x += BUTTON_WIDTH + 2;
    MoveWindow(g_buttons[3], x, 7, BUTTON_WIDTH, 24, TRUE); x += BUTTON_WIDTH + 6;

    {
        int reserved = (BUTTON_WIDTH + 16) * 3 + 30;
        int editWidth = width - x - reserved;

        if (editWidth < 80)
            editWidth = 80;

        MoveWindow(g_address, x, 7, editWidth, 24, TRUE);
        x += editWidth + 4;
        MoveWindow(g_buttons[4], x, 7, BUTTON_WIDTH + 16, 24, TRUE);
        x += BUTTON_WIDTH + 20;
        MoveWindow(g_buttons[5], x, 7, BUTTON_WIDTH + 16, 24, TRUE);
        x += BUTTON_WIDTH + 20;
        MoveWindow(g_buttons[6], x, 7, BUTTON_WIDTH + 16, 24, TRUE);
    }

    MoveWindow(g_find, 4, TOOLBAR_HEIGHT + 2, 260, 24, FALSE);

    (void)height;
}

static int tabFromPoint(int x, int y)
{
    int slot;
    int tabWidth = 170;

    if (y < TOOLBAR_HEIGHT || y >= viewTop())
        return -1;

    slot = (x - 4) / tabWidth;
    if (slot < 0 || slot >= g_tabOrderCount)
        return -1;

    return g_tabOrder[slot];
}

static void drawTabBar(HDC hdc, int width)
{
    int slot;
    int x = 4;
    int tabWidth = 170;

    for (slot = 0; slot < g_tabOrderCount; ++slot) {
        int i = g_tabOrder[slot];
        RECT box;
        RECT text;
        char label[400];
        HBRUSH brush;
        COLORREF oldText;

        SetRect(&box, x, TOOLBAR_HEIGHT + 2, x + tabWidth - 4, TOOLBAR_HEIGHT + TABBAR_HEIGHT - 2);

        brush = CreateSolidBrush(i == g_activeTab ? RGB(255, 255, 255) : RGB(226, 226, 226));
        FillRect(hdc, &box, brush);
        DeleteObject(brush);
        FrameRect(hdc, &box, (HBRUSH)GetStockObject(GRAY_BRUSH));

        lockAcquire();
        _snprintf(label, sizeof(label), "%d  %s", i + 1,
                  g_tabs[i].title[0] ? g_tabs[i].title :
                  (g_tabs[i].url[0] ? g_tabs[i].url : "New Tab"));
        lockRelease();
        label[sizeof(label) - 1] = 0;

        text = box;
        text.left += 8;
        text.right -= 30;

        oldText = SetTextColor(hdc, i == g_activeTab ? RGB(0, 0, 0) : RGB(90, 90, 90));
        SetBkMode(hdc, TRANSPARENT);
        DrawTextA(hdc, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        SetTextColor(hdc, oldText);

        /* close box */
        {
            RECT closeBox;
            RECT closeText;

            SetRect(&closeBox, box.right - 22, box.top + 4, box.right - 6, box.bottom - 4);
            FrameRect(hdc, &closeBox, (HBRUSH)GetStockObject(GRAY_BRUSH));

            closeText = closeBox;
            SetBkMode(hdc, TRANSPARENT);
            DrawTextA(hdc, "x", -1, &closeText, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }

        x += tabWidth;
    }

    (void)width;
}

static void paintWindow(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    lockAcquire();
    if (g_activeTab < g_tabCount) {
        Tab* tab = &g_tabs[g_activeTab];

        if (tab->memdc && tab->bits) {
            BitBlt(hdc, 0, viewTop(), tab->width, tab->height, tab->memdc, 0, 0, SRCCOPY);
        } else {
            RECT fill;

            SetRect(&fill, 0, viewTop(), tab->width, viewTop() + tab->height);
            FillRect(hdc, &fill, (HBRUSH)GetStockObject(WHITE_BRUSH));
        }

        if (tab->loading) {
            RECT strip;
            int width = (tab->width * (tab->progress > 0 ? tab->progress : 5)) / 100;

            SetRect(&strip, 0, viewTop() - 3, width, viewTop());
            FillRect(hdc, &strip, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
    }
    lockRelease();

    {
        RECT bar;
        RECT client;

        GetClientRect(hwnd, &client);
        SetRect(&bar, 0, TOOLBAR_HEIGHT, client.right, viewTop());
        FillRect(hdc, &bar, (HBRUSH)(COLOR_BTNFACE + 1));
        drawTabBar(hdc, client.right);
    }

    {
        RECT client;
        RECT bar;
        char text[800];
        int active = g_activeTab;

        GetClientRect(hwnd, &client);
        SetRect(&bar, 0, client.bottom - STATUSBAR_HEIGHT, client.right, client.bottom);
        FillRect(hdc, &bar, (HBRUSH)(COLOR_BTNFACE + 1));

        lockAcquire();
        if (active < g_tabCount) {
            Tab* tab = &g_tabs[active];

            _snprintf(text, sizeof(text), "%s%s%s",
                      tab->loading ? "Loading... " : "",
                      tab->status[0] ? tab->status : "",
                      (!tab->loading && !tab->status[0]) ? (tab->url[0] ? tab->url : "Ready") : "");
            if (g_zoom != 100) {
                char zoomText[32];

                _snprintf(zoomText, sizeof(zoomText), "  [%d%%]", g_zoom);
                strncat(text, zoomText, sizeof(text) - strlen(text) - 1);
            }
        } else {
            strcpy(text, "Ready");
        }
        lockRelease();
        text[sizeof(text) - 1] = 0;

        bar.left += 6;
        SetBkMode(hdc, TRANSPARENT);
        DrawTextA(hdc, text, -1, &bar, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }

    EndPaint(hwnd, &ps);
}

static void syncUiFromState(void)
{
    int active;
    char title[400];
    char url[MAX_PATH_CHARS];
    char tabTitle[300];
    static char lastRecorded[MAX_TABS][MAX_PATH_CHARS];

    lockAcquire();
    active = g_activeTab;
    if (active < g_tabCount) {
        strncpy(url, g_tabs[active].url, sizeof(url) - 1);
        url[sizeof(url) - 1] = 0;
        strncpy(tabTitle, g_tabs[active].title, sizeof(tabTitle) - 1);
        tabTitle[sizeof(tabTitle) - 1] = 0;
    } else {
        url[0] = 0;
        tabTitle[0] = 0;
    }
    lockRelease();

    if (active < g_tabCount && url[0] && strcmp(lastRecorded[active], url)) {
        strncpy(lastRecorded[active], url, MAX_PATH_CHARS - 1);
        lastRecorded[active][MAX_PATH_CHARS - 1] = 0;
        recordHistory(url);
    }

    if (GetFocus() != g_address)
        SetWindowTextA(g_address, url);

    _snprintf(title, sizeof(title), "%s - Agave BrowserShell",
              tabTitle[0] ? tabTitle : "BrowserShell");
    title[sizeof(title) - 1] = 0;
    SetWindowTextA(g_hwnd, title);

    InvalidateRect(g_hwnd, NULL, FALSE);
}

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
    ShellCommand command;

    memset(&command, 0, sizeof(command));
    command.type = CMD_MOUSE;
    command.tab = g_activeTab;
    command.key = (int)type;
    command.button = (int)button;
    command.x = x;
    command.y = y;
    command.modifier = (int)currentModifiers();
    postCommand(&command);
}

/* Main menu: bookmarks, history, zoom. */
static void showMainMenu(void)
{
    HMENU menu = CreatePopupMenu();
    HMENU bookmarks = CreatePopupMenu();
    HMENU history = CreatePopupMenu();
    POINT point;
    int choice;
    int i;

    for (i = 0; i < g_bookmarkCount && i < 20; ++i)
        AppendMenuA(bookmarks, MF_STRING, 100 + i,
                    g_bookmarks[i].title[0] ? g_bookmarks[i].title : g_bookmarks[i].url);

    if (g_bookmarkCount == 0)
        AppendMenuA(bookmarks, MF_STRING | MF_GRAYED, 0, "(empty)");

    for (i = g_historyCount - 1; i >= 0 && i >= g_historyCount - 20; --i) {
        char label[1200];

        _snprintf(label, sizeof(label), "%s", g_history[i]);
        label[sizeof(label) - 1] = 0;
        AppendMenuA(history, MF_STRING, 200 + (g_historyCount - 1 - i), label);
    }

    if (g_historyCount == 0)
        AppendMenuA(history, MF_STRING | MF_GRAYED, 0, "(empty)");

    AppendMenuA(menu, MF_STRING, 1, "Add bookmark\tCtrl+D");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)bookmarks, "Bookmarks");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)history, "History");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, 2, "Zoom in\tCtrl+=");
    AppendMenuA(menu, MF_STRING, 3, "Zoom out\tCtrl+-");
    AppendMenuA(menu, MF_STRING, 4, "Reset zoom\tCtrl+0");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, 5, "New tab\tCtrl+T");
    AppendMenuA(menu, MF_STRING, 6, "Find\tCtrl+F");
    AppendMenuA(menu, MF_STRING, 7, "Reload\tF5");

    GetCursorPos(&point);
    choice = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                 point.x, point.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);

    if (choice >= 200) {
        int index = g_historyCount - 1 - (choice - 200);

        if (index >= 0 && index < g_historyCount)
            navigateTo(g_history[index], 0);
        return;
    }

    if (choice >= 100) {
        int index = choice - 100;

        if (index >= 0 && index < g_bookmarkCount)
            navigateTo(g_bookmarks[index].url, 0);
        return;
    }

    switch (choice) {
    case 1: addBookmarkCurrent(); break;
    case 2: g_zoom = g_zoom < 400 ? g_zoom + 25 : g_zoom; postSimple(CMD_ZOOM, g_activeTab); break;
    case 3: g_zoom = g_zoom > 25 ? g_zoom - 25 : g_zoom; postSimple(CMD_ZOOM, g_activeTab); break;
    case 4: g_zoom = 100; postSimple(CMD_ZOOM, g_activeTab); break;
    case 5: navigateTo("about:blank", 1); break;
    case 6: ShowWindow(g_find, SW_SHOW); SetFocus(g_find); break;
    case 7: postSimple(CMD_RELOAD, g_activeTab); break;
    default: break;
    }
}

static void addBookmarkCurrent(void)
{
    char url[MAX_PATH_CHARS];
    char title[256];

    lockAcquire();
    strncpy(url, g_tabs[g_activeTab].url, sizeof(url) - 1);
    url[sizeof(url) - 1] = 0;
    strncpy(title, g_tabs[g_activeTab].title, sizeof(title) - 1);
    title[sizeof(title) - 1] = 0;
    lockRelease();

    addBookmark(url, title);

    lockAcquire();
    strncpy(g_tabs[g_activeTab].status, "Bookmark added",
            sizeof(g_tabs[0].status) - 1);
    g_tabs[g_activeTab].status[sizeof(g_tabs[0].status) - 1] = 0;
    lockRelease();

    notifyUi();
}

static void showContextMenu(int x, int y)
{
    HMENU menu = CreatePopupMenu();
    POINT point;
    int choice;

    AppendMenuA(menu, MF_STRING, 1, "Back");
    AppendMenuA(menu, MF_STRING, 2, "Forward");
    AppendMenuA(menu, MF_STRING, 3, "Reload");
    AppendMenuA(menu, MF_STRING, 4, "Stop");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, 5, "New Tab");
    AppendMenuA(menu, MF_STRING, 6, "Find...");

    point.x = x;
    point.y = y;
    ClientToScreen(g_hwnd, &point);

    choice = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                 point.x, point.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);

    switch (choice) {
    case 1: postSimple(CMD_BACK, g_activeTab); break;
    case 2: postSimple(CMD_FORWARD, g_activeTab); break;
    case 3: postSimple(CMD_RELOAD, g_activeTab); break;
    case 4: postSimple(CMD_STOP, g_activeTab); break;
    case 5: navigateTo("about:blank", 1); break;
    case 6: ShowWindow(g_find, SW_SHOW); SetFocus(g_find); break;
    default: break;
    }
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

/* Address bar: Enter navigates, Escape returns focus to the page. Never handle
 * EN_CHANGE here - SetWindowText would call straight back into navigate(). */
static WNDPROC    g_addressOldProc;
static WNDPROC    g_findOldProc;

static LRESULT CALLBACK addressProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            char buffer[MAX_PATH_CHARS];

            GetWindowTextA(hwnd, buffer, sizeof(buffer));
            navigateTo(buffer, 0);
            return 0;
        }

        if (wParam == VK_ESCAPE) {
            SetFocus(g_hwnd);
            return 0;
        }
    }

    return CallWindowProcA(g_addressOldProc, hwnd, message, wParam, lParam);
}

static LRESULT CALLBACK findProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            ShellCommand command;

            memset(&command, 0, sizeof(command));
            command.type = CMD_FIND;
            command.tab = g_activeTab;
            GetWindowTextA(hwnd, command.text, sizeof(command.text));
            postCommand(&command);
            return 0;
        }

        if (wParam == VK_ESCAPE) {
            ShowWindow(hwnd, SW_HIDE);
            SetFocus(g_hwnd);
            return 0;
        }
    }

    return CallWindowProcA(g_findOldProc, hwnd, message, wParam, lParam);
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    case WM_SIZE: {
        int width = LOWORD(lParam);

        if (getenv("AGB_SHELL_DEBUG")) {
            RECT frame;
            RECT client;

            GetWindowRect(hwnd, &frame);
            GetClientRect(hwnd, &client);
            fprintf(stderr, "[shell] WM_SIZE %dx%d frame=%ldx%ld client=%ldx%ld\n",
                    width, HIWORD(lParam),
                    frame.right - frame.left, frame.bottom - frame.top,
                    client.right, client.bottom);
        }

        layoutChildren(hwnd, width, HIWORD(lParam));
        resizeActiveTab();
        return 0;
    }

    case WM_PAINT:
        paintWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SHELL_STATE:
        syncUiFromState();
        return 0;

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);
        int index = tabFromPoint(x, y);

        if (index >= 0) {
            RECT tabBox;
            int slot = 0;
            int i;

            /* hit-test the close box first */
            for (i = 0; i < g_tabOrderCount; ++i) {
                if (g_tabOrder[i] == index) { slot = i; break; }
            }

            SetRect(&tabBox, 4 + slot * 170, TOOLBAR_HEIGHT + 2,
                    4 + slot * 170 + 166, TOOLBAR_HEIGHT + TABBAR_HEIGHT - 2);

            if (x >= tabBox.right - 22) {
                closeTab(index);
                return 0;
            }

            activateTab(index);
            return 0;
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        MC_MOUSE_EVENT_TYPE type = EVT_MOUSE_MOVE;
        MC_MOUSE_BUTTON button = MOUSE_BTN_NONE;
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);

        if (y < viewTop() || g_activeTab >= g_tabCount)
            return 0;

        y -= viewTop();

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

    case WM_CONTEXTMENU:
        showContextMenu((short)LOWORD(lParam), (short)HIWORD(lParam));
        return 0;

    case WM_MOUSEWHEEL: {
        POINT point;
        int delta = (short)HIWORD(wParam);

        point.x = (short)LOWORD(lParam);
        point.y = (short)HIWORD(lParam);
        ScreenToClient(hwnd, &point);

        if (point.y < viewTop())
            return 0;

        sendMouse(EVT_MOUSE_WHEEL, point.x, point.y - viewTop(),
                  delta > 0 ? MOUSE_WHEEL_UP : MOUSE_WHEEL_DOWN);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP: {
        ShellCommand command;

        if (message == WM_KEYDOWN) {
            if (GetKeyState(VK_MENU) & 0x8000) {
                if (wParam == VK_LEFT)  { postSimple(CMD_BACK, g_activeTab);    return 0; }
                if (wParam == VK_RIGHT) { postSimple(CMD_FORWARD, g_activeTab); return 0; }
            }
            if (wParam == VK_F5) { postSimple(CMD_RELOAD, g_activeTab); return 0; }
            if (wParam == VK_ESCAPE) { postSimple(CMD_STOP, g_activeTab); return 0; }
            if (wParam == VK_F6 || ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'L')) {
                SetFocus(g_address);
                SendMessageA(g_address, EM_SETSEL, 0, -1);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'D') {
                addBookmarkCurrent();
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == VK_ADD || wParam == 0xBB)) {
                g_zoom = g_zoom < 400 ? g_zoom + 25 : g_zoom;
                postSimple(CMD_ZOOM, g_activeTab);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == VK_SUBTRACT || wParam == 0xBD)) {
                g_zoom = g_zoom > 25 ? g_zoom - 25 : g_zoom;
                postSimple(CMD_ZOOM, g_activeTab);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == '0') {
                g_zoom = 100;
                postSimple(CMD_ZOOM, g_activeTab);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'T') {
                navigateTo("about:blank", 1);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'F') {
                ShowWindow(g_find, SW_SHOW);
                SetFocus(g_find);
                return 0;
            }
        }

        memset(&command, 0, sizeof(command));
        command.type = CMD_KEY;
        command.tab = g_activeTab;
        command.key = (message == WM_KEYDOWN) ? EVT_KEY_DOWN : EVT_KEY_UP;
        command.x = (int)wParam;
        command.modifier = (int)currentModifiers();
        postCommand(&command);
        return 0;
    }

    case WM_CHAR: {
        ShellCommand command;
        unsigned int code = (unsigned int)wParam;
        int length = 0;

        memset(&command, 0, sizeof(command));
        command.type = CMD_CHAR;
        command.tab = g_activeTab;

        if (code < 0x80) {
            command.text[length++] = (char)code;
        } else if (code < 0x800) {
            command.text[length++] = (char)(0xC0 | (code >> 6));
            command.text[length++] = (char)(0x80 | (code & 0x3F));
        } else {
            command.text[length++] = (char)(0xE0 | (code >> 12));
            command.text[length++] = (char)(0x80 | ((code >> 6) & 0x3F));
            command.text[length++] = (char)(0x80 | (code & 0x3F));
        }
        command.text[length] = 0;
        postCommand(&command);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        switch (id) {
        case ID_GO: {
            char buffer[MAX_PATH_CHARS];
            GetWindowTextA(g_address, buffer, sizeof(buffer));
            navigateTo(buffer, 0);
            return 0;
        }
        case ID_BACK:    postSimple(CMD_BACK, g_activeTab);    return 0;
        case ID_FORWARD: postSimple(CMD_FORWARD, g_activeTab); return 0;
        case ID_RELOAD:  postSimple(CMD_RELOAD, g_activeTab);  return 0;
        case ID_STOP:    postSimple(CMD_STOP, g_activeTab);    return 0;
        case ID_NEWTAB:  navigateTo("about:blank", 1);         return 0;
        case ID_MENU:    showMainMenu();                        return 0;
        case ID_FIND:    ShowWindow(g_find, SW_SHOW); SetFocus(g_find); return 0;
        default: break;
        }
        return 0;
    }

    case WM_DESTROY:
        saveSession();
        g_engineQuit = 1;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

/* ------------------------------------------------------------------------- */
/* tab management (UI thread)                                                 */
/* ------------------------------------------------------------------------- */

static void addTab(const char* url)
{
    Tab* tab;
    ShellCommand command;

    if (g_tabCount >= MAX_TABS)
        return;

    tab = &g_tabs[g_tabCount];
    memset(tab, 0, sizeof(*tab));
    tab->id = g_nextTabId++;

    if (url)
        strncpy(tab->url, url, sizeof(tab->url) - 1);

    {
        RECT client;

        GetClientRect(g_hwnd, &client);
        tab->wantWidth = client.right;
        tab->wantHeight = viewHeightFor(client.bottom);
    }

    lockAcquire();
    ++g_tabCount;
    g_activeTab = g_tabCount - 1;
    lockRelease();

    memset(&command, 0, sizeof(command));
    command.type = CMD_NEW_TAB;
    command.tab = g_activeTab;
    strncpy(command.text, tab->url[0] ? tab->url : g_initialUrl, sizeof(command.text) - 1);
    postCommand(&command);

    rebuildTabOrder();
    resizeActiveTab();
}

/* ------------------------------------------------------------------------- */
/* entry point                                                                */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* persistent state: bookmarks, history, session                              */
/* ------------------------------------------------------------------------- */

static void dataDirInit(void)
{
    const char* appdata = getenv("APPDATA");

    if (appdata && appdata[0]) {
        _snprintf(g_dataDir, sizeof(g_dataDir), "%s\\AgaveShell", appdata);
        g_dataDir[sizeof(g_dataDir) - 1] = 0;

        if (CreateDirectoryA(g_dataDir, NULL) ||
            GetFileAttributesA(g_dataDir) != INVALID_FILE_ATTRIBUTES)
            return;
    }

    /* Portable / restricted environments: keep the data next to the exe. */
    {
        char module[MAX_PATH];

        if (GetModuleFileNameA(NULL, module, sizeof(module))) {
            char* slash = strrchr(module, '\\');

            if (slash)
                *(slash + 1) = 0;

            _snprintf(g_dataDir, sizeof(g_dataDir), "%sAgaveShellData", module);
            g_dataDir[sizeof(g_dataDir) - 1] = 0;
            CreateDirectoryA(g_dataDir, NULL);
        }
    }

    if (getenv("AGAVE_SHELL_DEBUG"))
        fprintf(stderr, "[shell] data dir=%s (created=%d)\n", g_dataDir,
                GetFileAttributesA(g_dataDir) != INVALID_FILE_ATTRIBUTES);
}

static void statePath(char* out, size_t size, const char* name)
{
    _snprintf(out, size, "%s\\%s", g_dataDir, name);
    out[size - 1] = 0;
}

static void loadBookmarks(void)
{
    char path[MAX_PATH_CHARS];
    FILE* file;
    char line[1400];

    g_bookmarkCount = 0;
    statePath(path, sizeof(path), "bookmarks.txt");
    file = fopen(path, "rb");

    if (!file)
        return;

    while (g_bookmarkCount < AGW_MAX_BOOKMARKS && fgets(line, sizeof(line), file)) {
        char* tab = strchr(line, '\t');
        size_t n;

        if (!tab)
            continue;

        *tab = 0;

        n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;

        strncpy(g_bookmarks[g_bookmarkCount].url, line,
                sizeof(g_bookmarks[0].url) - 1);
        strncpy(g_bookmarks[g_bookmarkCount].title, tab + 1,
                sizeof(g_bookmarks[0].title) - 1);

        {
            char* tail = g_bookmarks[g_bookmarkCount].title;
            size_t m = strlen(tail);

            while (m && (tail[m - 1] == '\n' || tail[m - 1] == '\r'))
                tail[--m] = 0;
        }

        ++g_bookmarkCount;
    }

    fclose(file);
}

static void saveBookmarks(void)
{
    char path[MAX_PATH_CHARS];
    FILE* file;
    int i;

    statePath(path, sizeof(path), "bookmarks.txt");
    file = fopen(path, "wb");

    if (!file)
        return;

    for (i = 0; i < g_bookmarkCount; ++i)
        fprintf(file, "%s\t%s\n", g_bookmarks[i].url,
                g_bookmarks[i].title[0] ? g_bookmarks[i].title : g_bookmarks[i].url);

    fclose(file);
}

static void addBookmark(const char* url, const char* title)
{
    int i;

    if (!url || !url[0])
        return;

    for (i = 0; i < g_bookmarkCount; ++i) {
        if (!strcmp(g_bookmarks[i].url, url))
            return;   /* already there */
    }

    if (g_bookmarkCount >= AGW_MAX_BOOKMARKS)
        return;

    strncpy(g_bookmarks[g_bookmarkCount].url, url, sizeof(g_bookmarks[0].url) - 1);
    strncpy(g_bookmarks[g_bookmarkCount].title, title ? title : "",
            sizeof(g_bookmarks[0].title) - 1);
    ++g_bookmarkCount;

    saveBookmarks();
}

static void recordHistory(const char* url)
{
    char path[MAX_PATH_CHARS];
    FILE* file;

    if (!url || !url[0])
        return;

    if (g_historyCount > 0 && !strcmp(g_history[g_historyCount - 1], url))
        return;

    if (g_historyCount == AGW_MAX_HISTORY) {
        memmove(&g_history[0][0], &g_history[1][0],
                sizeof(g_history[0]) * (AGW_MAX_HISTORY - 1));
        --g_historyCount;
    }

    strncpy(g_history[g_historyCount], url, sizeof(g_history[0]) - 1);
    g_history[g_historyCount][sizeof(g_history[0]) - 1] = 0;
    ++g_historyCount;

    statePath(path, sizeof(path), "history.txt");
    file = fopen(path, "ab");

    if (file) {
        fprintf(file, "%s\n", url);
        fclose(file);
    }
}

static void saveSession(void)
{
    char path[MAX_PATH_CHARS];
    FILE* file;
    int i;

    statePath(path, sizeof(path), "session.txt");
    file = fopen(path, "wb");

    if (!file)
        return;

    for (i = 0; i < g_tabCount; ++i) {
        if (!g_tabs[i].closed && g_tabs[i].url[0])
            fprintf(file, "%s\n", g_tabs[i].url);
    }

    fclose(file);
}

static int loadSession(char urls[][MAX_PATH_CHARS], int max)
{
    char path[MAX_PATH_CHARS];
    FILE* file;
    int count = 0;

    statePath(path, sizeof(path), "session.txt");
    file = fopen(path, "rb");

    if (!file)
        return 0;

    while (count < max && fgets(urls[count], MAX_PATH_CHARS, file)) {
        size_t n = strlen(urls[count]);

        while (n && (urls[count][n - 1] == '\n' || urls[count][n - 1] == '\r'))
            urls[count][--n] = 0;

        if (urls[count][0])
            ++count;
    }

    fclose(file);
    return count;
}

static void makeDownloadDir(void)
{
    const char* home = getenv("USERPROFILE");

    if (!home)
        home = ".";

    _snprintf(g_downloadDir, sizeof(g_downloadDir), "%s\\Downloads", home);
    g_downloadDir[sizeof(g_downloadDir) - 1] = 0;
    CreateDirectoryA(g_downloadDir, NULL);
}

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show)
{
    WNDCLASSA wc;
    MSG message;
    MC_CALLBACK_INFO callbacks;
    HANDLE thread;
    int argc = 0;
    char** argv = __argv;

    (void)previous;
    (void)show;
    (void)commandLine;

    while (argv && argv[argc])
        ++argc;

    if (argc > 1) {
        strncpy(g_initialUrl, argv[1], sizeof(g_initialUrl) - 1);
        g_initialUrl[sizeof(g_initialUrl) - 1] = 0;
    } else {
        strcpy(g_initialUrl, "http://www.163.com");
    }

    InitializeCriticalSection(&g_lock);
    makeDownloadDir();
    dataDirInit();
    loadBookmarks();

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AgaveShellWindow";

    if (!RegisterClassA(&wc))
        return 1;

    g_hwnd = CreateWindowExA(0, "AgaveShellWindow", "Agave BrowserShell",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
                             NULL, NULL, instance, NULL);

    if (!g_hwnd)
        return 1;

    g_buttons[0] = CreateWindowA("BUTTON", "<",  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_BACK,    instance, NULL);
    g_buttons[1] = CreateWindowA("BUTTON", ">",  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_FORWARD, instance, NULL);
    g_buttons[2] = CreateWindowA("BUTTON", "R",  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_RELOAD,  instance, NULL);
    g_buttons[3] = CreateWindowA("BUTTON", "X",  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_STOP,    instance, NULL);
    g_buttons[4] = CreateWindowA("BUTTON", "Go", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_GO,     instance, NULL);
    g_buttons[5] = CreateWindowA("BUTTON", "+",  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_NEWTAB, instance, NULL);
    g_buttons[6] = CreateWindowA("BUTTON", "=",  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_hwnd, (HMENU)ID_MENU,   instance, NULL);

    g_address = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                0, 0, 0, 0, g_hwnd, (HMENU)ID_ADDRESS, instance, NULL);

    g_find = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                             WS_CHILD | ES_AUTOHSCROLL,
                             0, 0, 0, 0, g_hwnd, (HMENU)ID_FIND, instance, NULL);

    g_addressOldProc = (WNDPROC)SetWindowLongPtrA(g_address, GWLP_WNDPROC, (LONG_PTR)addressProc);
    g_findOldProc    = (WNDPROC)SetWindowLongPtrA(g_find, GWLP_WNDPROC, (LONG_PTR)findProc);

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.cb_invalidate_rect  = onInvalidate;
    callbacks.cb_update_view_now  = onUpdateNow;
    callbacks.cb_set_title        = onTitle;
    callbacks.cb_set_location     = onLocation;
    callbacks.cb_set_status       = onStatus;
    callbacks.cb_loading_progress = onProgress;
    callbacks.cb_error            = onError;
    callbacks.cb_set_cursor       = onCursor;
    callbacks.cb_write_file_data  = onWriteFile;
    macross_set_callback(&callbacks);

    {
        static char sessionUrls[8][MAX_PATH_CHARS];
        int restored = 0;

        if (argc <= 1)
            restored = loadSession(sessionUrls, 8);

        if (restored > 0) {
            int i;

            for (i = 0; i < restored; ++i)
                addTab(sessionUrls[i]);
        } else {
            addTab(g_initialUrl);
        }
    }

    if (getenv("AGB_SHELL_DEBUG")) {
        RECT frame;

        GetWindowRect(g_hwnd, &frame);
        fprintf(stderr, "[shell] before show frame=%ldx%ld\n",
                frame.right - frame.left, frame.bottom - frame.top);
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (getenv("AGB_SHELL_DEBUG")) {
        RECT frame;

        GetWindowRect(g_hwnd, &frame);
        fprintf(stderr, "[shell] after show frame=%ldx%ld\n",
                frame.right - frame.left, frame.bottom - frame.top);
    }

    thread = CreateThread(NULL, 0, engineThread, NULL, 0, NULL);

    SetFocus(g_address);
    SendMessageA(g_address, EM_SETSEL, 0, -1);

    while (GetMessageA(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    g_engineQuit = 1;
    if (thread) {
        WaitForSingleObject(thread, 3000);
        CloseHandle(thread);
    }

    DeleteCriticalSection(&g_lock);
    return 0;
}
