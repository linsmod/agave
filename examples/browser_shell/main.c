/*
 * main.c - Agave BrowserShell, built on the AgaveWebView control.
 *
 * The shell used to drive macross_* directly (its own engine thread, DIB and
 * input translation). It now only owns UI: every tab is an AgaveWebView child
 * window, so the control gets exercised the way an embedding host would.
 *
 * Threading: the control owns the engine thread; this file never touches the
 * engine directly and only uses the control's C API (queued, thread safe).
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agave_webview.h"

#define MAX_TABS        16
#define MAX_PATH_CHARS  1024

#define ID_BACK       1001
#define ID_FORWARD    1002
#define ID_RELOAD     1003
#define ID_STOP       1004
#define ID_ADDRESS    1005
#define ID_GO         1006
#define ID_NEWTAB     1007
#define ID_MENU       1009

#define ID_FIND_EDIT  1010
#define ID_FIND_NEXT  1011
#define ID_FIND_PREV  1012
#define ID_FIND_CLOSE 1013

#define TAB_HEIGHT    28
#define TOOLBAR_Y     4
#define TOOLBAR_H     34
#define FIND_H        30
#define STATUS_H      22
#define BUTTON_W      34
#define TAB_W         172

typedef struct {
    AgaveWebView* view;
    HWND          hwnd;          /* the control's HWND */
    char          url[MAX_PATH_CHARS];
    char          title[256];
    char          status[512];
    int           loading;
    int           progress;
    int           used;
} Tab;

static HWND  g_hwnd;
static HWND  g_back, g_forward, g_reload, g_stop, g_address, g_go, g_newtab, g_menu;
static HWND  g_findEdit, g_findNext, g_findPrev, g_findClose;
static HINSTANCE g_instance;

static Tab   g_tabs[MAX_TABS];
static int   g_activeTab;
static int   g_zoom = 100;
static int   g_findVisible;
static char  g_status[1024];

/* Closed tabs, so ctrl+shift+T can bring the last one back. */
#define MAX_CLOSED_TABS 16
typedef struct { char url[MAX_PATH_CHARS]; char title[256]; } ClosedTab;
static ClosedTab g_closed[MAX_CLOSED_TABS];
static int       g_closedCount;

#define AGW_MAX_BOOKMARKS 64
#define AGW_MAX_HISTORY   200

typedef struct { char url[MAX_PATH_CHARS]; char title[256]; } Bookmark;

static Bookmark g_bookmarks[AGW_MAX_BOOKMARKS];
static int      g_bookmarkCount;
static char     g_history[AGW_MAX_HISTORY][MAX_PATH_CHARS];
static int      g_historyCount;
static char     g_dataDir[MAX_PATH_CHARS];

/* ------------------------------------------------------------------------- */
/* data directory + persisted state                                          */
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

    /* Portable mode: next to the executable. */
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
}

static void statePath(char* out, size_t size, const char* name)
{
    _snprintf(out, size, "%s\\%s", g_dataDir, name);
    out[size - 1] = 0;
}

static void loadBookmarks(void)
{
    char path[MAX_PATH_CHARS];
    char line[1400];
    FILE* file;

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

        strncpy(g_bookmarks[g_bookmarkCount].url, line, MAX_PATH_CHARS - 1);
        strncpy(g_bookmarks[g_bookmarkCount].title, tab + 1, sizeof(g_bookmarks[0].title) - 1);

        {
            char* title = g_bookmarks[g_bookmarkCount].title;
            size_t m = strlen(title);

            while (m && (title[m - 1] == '\n' || title[m - 1] == '\r'))
                title[--m] = 0;
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

    if (!url || !url[0] || g_bookmarkCount >= AGW_MAX_BOOKMARKS)
        return;

    for (i = 0; i < g_bookmarkCount; ++i) {
        if (!strcmp(g_bookmarks[i].url, url))
            return;
    }

    strncpy(g_bookmarks[g_bookmarkCount].url, url, MAX_PATH_CHARS - 1);
    strncpy(g_bookmarks[g_bookmarkCount].title, title ? title : "", sizeof(g_bookmarks[0].title) - 1);
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
        memmove(g_history[0], g_history[1], sizeof(g_history[0]) * (AGW_MAX_HISTORY - 1));
        --g_historyCount;
    }

    strncpy(g_history[g_historyCount], url, MAX_PATH_CHARS - 1);
    g_history[g_historyCount][MAX_PATH_CHARS - 1] = 0;
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

    for (i = 0; i < MAX_TABS; ++i) {
        if (g_tabs[i].used && g_tabs[i].url[0])
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

/* ------------------------------------------------------------------------- */
/* layout                                                                    */
/* ------------------------------------------------------------------------- */

/* The engine speaks UTF-8; Win32 controls and GDI speak UTF-16. Everything that
 * shows page-provided text goes through these helpers (the tab used to print
 * UTF-8 bytes with DrawTextA -> mojibake). */
static void utf8ToWide(const char* source, wchar_t* target, int count)
{
    if (count <= 0)
        return;

    if (!source) {
        target[0] = 0;
        return;
    }

    if (!MultiByteToWideChar(CP_UTF8, 0, source, -1, target, count))
        target[0] = 0;

    target[count - 1] = 0;
}

static void wideToUtf8(const wchar_t* source, char* target, int count)
{
    if (count <= 0)
        return;

    if (!source) {
        target[0] = 0;
        return;
    }

    if (!WideCharToMultiByte(CP_UTF8, 0, source, -1, target, count, NULL, NULL))
        target[0] = 0;

    target[count - 1] = 0;
}

/* Reads a control back as UTF-8. The address bar is a Unicode edit control
 * (CreateWindowExW), so GetWindowTextA would push paths through the ANSI code
 * page and lose every character outside it. */
static void readControlText(HWND control, char* out, size_t outSize)
{
    wchar_t wide[MAX_PATH_CHARS];

    if (!outSize)
        return;

    wide[0] = 0;
    GetWindowTextW(control, wide, MAX_PATH_CHARS);
    wideToUtf8(wide, out, (int)outSize);
}

static int nextFreeTab(void)
{
    int i;

    for (i = 0; i < MAX_TABS; ++i) {
        if (!g_tabs[i].used)
            return i;
    }

    return -1;
}

static void layoutTab(int index)
{
    RECT client;
    int top;

    if (!g_tabs[index].used || !g_tabs[index].view)
        return;

    GetClientRect(g_hwnd, &client);
    top = TAB_HEIGHT + TOOLBAR_H + (g_findVisible ? FIND_H : 0);

    agave_webview_set_rect(g_tabs[index].view, 0, top,
                           client.right, client.bottom - top - STATUS_H);
}

static void layoutAll(void)
{
    RECT client;
    int width, i;
    int y = TOOLBAR_Y + TAB_HEIGHT;
    int x = 6;
    int reserved;

    GetClientRect(g_hwnd, &client);
    width = client.right;

    MoveWindow(g_back, x, y, BUTTON_W, 26, TRUE);        x += BUTTON_W + 2;
    MoveWindow(g_forward, x, y, BUTTON_W, 26, TRUE);     x += BUTTON_W + 2;
    MoveWindow(g_reload, x, y, BUTTON_W, 26, TRUE);      x += BUTTON_W + 2;
    MoveWindow(g_stop, x, y, BUTTON_W, 26, TRUE);        x += BUTTON_W + 6;

    reserved = (BUTTON_W + 6) * 3 + 20;
    MoveWindow(g_address, x, y + 1, width - x - reserved, 24, TRUE);
    x = width - reserved + 6;
    MoveWindow(g_go, x, y, 46, 26, TRUE);                x += 46 + 4;
    MoveWindow(g_newtab, x, y, BUTTON_W, 26, TRUE);      x += BUTTON_W + 4;
    MoveWindow(g_menu, x, y, BUTTON_W, 26, TRUE);

    if (g_findVisible) {
        int fy = TAB_HEIGHT + TOOLBAR_H + 2;

        MoveWindow(g_findEdit, 8, fy, 260, 24, TRUE);
        MoveWindow(g_findPrev, 274, fy, 34, 24, TRUE);
        MoveWindow(g_findNext, 312, fy, 34, 24, TRUE);
        MoveWindow(g_findClose, 350, fy, 28, 24, TRUE);
    }

    for (i = 0; i < MAX_TABS; ++i)
        layoutTab(i);
}

static void updateStatus(void)
{
    Tab* tab = &g_tabs[g_activeTab];
    char text[1024];

    if (tab->used) {
        if (tab->loading)
            _snprintf(text, sizeof(text), "Loading %s... %d%%", tab->url, tab->progress);
        else if (tab->status[0])
            _snprintf(text, sizeof(text), "%s", tab->status);
        else
            _snprintf(text, sizeof(text), "%s", tab->url[0] ? tab->url : "Ready");
    } else {
        strcpy(text, "Ready");
    }

    text[sizeof(text) - 1] = 0;

    if (g_zoom != 100) {
        char zoom[32];

        _snprintf(zoom, sizeof(zoom), "  [%d%%]", g_zoom);
        strncat(text, zoom, sizeof(text) - strlen(text) - 1);
    }

    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void applyZoomToActive(void)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (tab->used && tab->view)
        agave_webview_set_zoom(tab->view, g_zoom);
}

/* ------------------------------------------------------------------------- */
/* tabs                                                                      */
/* ------------------------------------------------------------------------- */

static void setActiveTab(int index);
static void createTab(const char* url);
static void onCloseWindow(AgaveWebView* view, void* user);

static void setActiveTab(int index)
{
    int i;

    if (index < 0 || index >= MAX_TABS || !g_tabs[index].used)
        return;

    g_activeTab = index;

    for (i = 0; i < MAX_TABS; ++i) {
        if (g_tabs[i].used && g_tabs[i].hwnd)
            ShowWindow(g_tabs[i].hwnd, i == index ? SW_SHOW : SW_HIDE);
    }

    layoutTab(index);
    applyZoomToActive();

    if (g_tabs[index].view) {
        wchar_t wideAddress[MAX_PATH_CHARS];

        utf8ToWide(g_tabs[index].url, wideAddress, MAX_PATH_CHARS);
        SetWindowTextW(g_address, wideAddress);
        agave_webview_set_focus(g_tabs[index].view);
    }

    updateStatus();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void closeTab(int index)
{
    int i;

    if (index < 0 || index >= MAX_TABS || !g_tabs[index].used)
        return;

    /* Remember the page so ctrl+shift+T can reopen it. */
    if (g_tabs[index].url[0] && strcmp(g_tabs[index].url, "about:blank") != 0) {
        if (g_closedCount == MAX_CLOSED_TABS) {
            memmove(&g_closed[0], &g_closed[1], sizeof(ClosedTab) * (MAX_CLOSED_TABS - 1));
            g_closedCount--;
        }
        strncpy(g_closed[g_closedCount].url, g_tabs[index].url, MAX_PATH_CHARS - 1);
        g_closed[g_closedCount].url[MAX_PATH_CHARS - 1] = 0;
        strncpy(g_closed[g_closedCount].title, g_tabs[index].title, sizeof(g_closed[0].title) - 1);
        g_closed[g_closedCount].title[sizeof(g_closed[0].title) - 1] = 0;
        g_closedCount++;
    }

    if (g_tabs[index].view)
        agave_webview_destroy(g_tabs[index].view);

    memset(&g_tabs[index], 0, sizeof(Tab));

    if (g_activeTab == index) {
        for (i = 0; i < MAX_TABS; ++i) {
            if (g_tabs[i].used) {
                setActiveTab(i);
                break;
            }
        }

        if (i == MAX_TABS) {
            g_activeTab = 0;
            updateStatus();
        }
    }

    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* ctrl+shift+T: reopen the tab that was closed last. */
static void reopenClosedTab(void)
{
    ClosedTab closed;

    if (g_closedCount <= 0)
        return;

    closed = g_closed[--g_closedCount];
    createTab(closed.url);
}

/* ctrl+tab / ctrl+shift+tab: walk through the open tabs. */
static void switchTab(int direction)
{
    int i, count = 0, index = -1, slot = 0;

    for (i = 0; i < MAX_TABS; ++i) {
        if (!g_tabs[i].used)
            continue;

        if (i == g_activeTab)
            index = slot;

        ++slot;
        ++count;
    }

    if (count < 2)
        return;

    index = (index + direction + count) % count;

    slot = 0;
    for (i = 0; i < MAX_TABS; ++i) {
        if (!g_tabs[i].used)
            continue;

        if (slot == index) {
            setActiveTab(i);
            return;
        }

        ++slot;
    }
}

/* ------------------------------------------------------------------------- */
/* control callbacks (engine thread)                                         */
/* ------------------------------------------------------------------------- */

static void onTitle(AgaveWebView* view, const char* title, void* user)
{
    Tab* tab = (Tab*)user;

    (void)view;

    if (!tab)
        return;

    strncpy(tab->title, title ? title : "", sizeof(tab->title) - 1);
    tab->title[sizeof(tab->title) - 1] = 0;

    if (tab == &g_tabs[g_activeTab]) {
        wchar_t wide[256];

        utf8ToWide(tab->title[0] ? tab->title : "BrowserShell - Agave", wide, 256);
        SetWindowTextW(g_hwnd, wide);
    }

    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void onUrl(AgaveWebView* view, const char* url, void* user)
{
    Tab* tab = (Tab*)user;

    (void)view;

    if (!tab || !url)
        return;

    strncpy(tab->url, url, MAX_PATH_CHARS - 1);
    tab->url[MAX_PATH_CHARS - 1] = 0;

    if (tab == &g_tabs[g_activeTab]) {
        if (GetFocus() != g_address) {
            wchar_t wideUrl[MAX_PATH_CHARS];

            utf8ToWide(tab->url, wideUrl, MAX_PATH_CHARS);
            SetWindowTextW(g_address, wideUrl);
        }

        recordHistory(tab->url);
    }
}

static void onProgress(AgaveWebView* view, int percent, int finished, void* user)
{
    Tab* tab = (Tab*)user;

    (void)view;

    if (!tab)
        return;

    tab->progress = percent;
    tab->loading = finished ? 0 : 1;

    if (tab == &g_tabs[g_activeTab])
        updateStatus();
}

static void onStatus(AgaveWebView* view, const char* text, void* user)
{
    Tab* tab = (Tab*)user;

    (void)view;

    if (!tab)
        return;

    strncpy(tab->status, text ? text : "", sizeof(tab->status) - 1);
    tab->status[sizeof(tab->status) - 1] = 0;

    if (tab == &g_tabs[g_activeTab])
        updateStatus();
}

static void onError(AgaveWebView* view, int code, const char* url, void* user)
{
    Tab* tab = (Tab*)user;
    char text[512];

    (void)view;

    if (!tab)
        return;

    _snprintf(text, sizeof(text), "Failed to load (%d): %s", code, url ? url : "");
    text[sizeof(text) - 1] = 0;
    strncpy(tab->status, text, sizeof(tab->status) - 1);
    tab->loading = 0;

    if (tab == &g_tabs[g_activeTab])
        updateStatus();
}

static int onDialog(AgaveWebView* view, int type, const char* message,
                    const char* defaultValue, char* result, int resultSize, void* user)
{
    (void)view;
    (void)user;

    if (type == 0) {
        MessageBoxA(g_hwnd, message ? message : "", "Page says", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    if (type == 1) {
        return MessageBoxA(g_hwnd, message ? message : "", "Confirm",
                           MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
    }

    if (result && resultSize > 0) {
        _snprintf(result, resultSize, "%s", defaultValue ? defaultValue : "");
        result[resultSize - 1] = 0;
    }

    return 1;
}

static int onNewWindow(AgaveWebView* from, AgaveWebView* popup,
                       const char* url, void* user)
{
    int index = nextFreeTab();
    (void)from;
    (void)user;

    if (index < 0 || !url || !url[0]) {
        agave_webview_destroy(popup);
        return 0;
    }

    g_tabs[index].used = 1;
    g_tabs[index].view = popup;
    g_tabs[index].hwnd = (HWND)agave_webview_hwnd(popup);
    strncpy(g_tabs[index].url, url, MAX_PATH_CHARS - 1);
    strncpy(g_tabs[index].title, url, sizeof(g_tabs[index].title) - 1);

    /* The popup inherited the callbacks and the state pointer of the view it
     * was opened from: without this the new tab's title/url/progress would be
     * written into the old tab. */
    agave_webview_set_user(popup, &g_tabs[index]);

    /* Re-parent into the shell so it behaves like a tab, not a floating popup. */
    SetParent(g_tabs[index].hwnd, g_hwnd);
    ShowWindow(g_tabs[index].hwnd, SW_HIDE);

    agave_webview_set_zoom(popup, g_zoom);
    setActiveTab(index);
    return 1;
}

static void onDownload(AgaveWebView* view, const char* url, const char* path, void* user)
{
    (void)view;
    (void)user;
    (void)url;

    _snprintf(g_status, sizeof(g_status), "Downloaded: %s", path ? path : "");
    g_status[sizeof(g_status) - 1] = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Right click: the control reports the hit-test; the shell owns the menu. */
static void navigateActive(const char* text, int newTab);
static void addBookmarkCurrent(void);

static void onContextMenu(AgaveWebView* view, int x, int y,
                          const char* linkUrl, const char* imageUrl,
                          const char* text, void* user)
{
    HMENU menu = CreatePopupMenu();
    POINT point;
    Tab* tab = (Tab*)user;
    int choice;

    (void)view;
    (void)text;

    point.x = x;
    point.y = y;
    ClientToScreen(g_hwnd, &point);

    AppendMenuA(menu, MF_STRING, 1, "Back");
    AppendMenuA(menu, MF_STRING, 2, "Forward");
    AppendMenuA(menu, MF_STRING, 3, "Reload");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);

    if (linkUrl && linkUrl[0]) {
        AppendMenuA(menu, MF_STRING, 4, "Open link in this tab");
        AppendMenuA(menu, MF_STRING, 5, "Open link in new tab");
        AppendMenuA(menu, MF_STRING, 6, "Copy link address");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    }

    if (imageUrl && imageUrl[0])
        AppendMenuA(menu, MF_STRING, 7, "Save image (not implemented)");

    AppendMenuA(menu, MF_STRING, 8, "Bookmark this page");
    AppendMenuA(menu, MF_STRING, 9, "View page source (eval)");

    choice = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                 point.x, point.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);

    switch (choice) {
    case 1: if (tab->used) agave_webview_back(tab->view); break;
    case 2: if (tab->used) agave_webview_forward(tab->view); break;
    case 3: if (tab->used) agave_webview_reload(tab->view); break;
    case 4: if (linkUrl && linkUrl[0]) navigateActive(linkUrl, 0); break;
    case 5: if (linkUrl && linkUrl[0]) navigateActive(linkUrl, 1); break;
    case 6:
        if (linkUrl && linkUrl[0]) {
            if (OpenClipboard(g_hwnd)) {
                HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, strlen(linkUrl) + 1);

                if (memory) {
                    void* target = GlobalLock(memory);

                    if (target) {
                        strcpy((char*)target, linkUrl);
                        GlobalUnlock(memory);
                        EmptyClipboard();
                        SetClipboardData(CF_TEXT, memory);
                    }
                }
                CloseClipboard();
            }
        }
        break;
    case 8: addBookmarkCurrent(); break;
    case 9: if (tab->used) agave_webview_eval(tab->view, "document.documentElement.outerHTML.slice(0,2000)"); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* navigation helpers                                                        */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* address bar input -> URL                                                  */
/* ------------------------------------------------------------------------- */

static int startsWithNoCase(const char* text, const char* prefix)
{
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;

        if (!a)
            return 0;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

/* "E:\dir", "E:/dir", "E:" (drive current directory). */
static int isDrivePath(const char* text)
{
    if (!((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z')))
        return 0;
    if (text[1] != ':')
        return 0;
    return text[2] == '\\' || text[2] == '/' || text[2] == 0;
}

static int isUncPath(const char* text)
{
    return text[0] == '\\' && text[1] == '\\';
}

static int localPathExists(const char* text)
{
    wchar_t wide[MAX_PATH_CHARS];
    DWORD attributes;

    if (!text || !text[0])
        return 0;

    utf8ToWide(text, wide, MAX_PATH_CHARS);
    attributes = GetFileAttributesW(wide);
    return attributes != INVALID_FILE_ATTRIBUTES;
}

/* Bytes that may appear verbatim in the path of a file: URL. Everything else
 * (space, %, #, ?, backslash and every non-ASCII UTF-8 byte) gets escaped -
 * the engine parses the URL, so an unescaped space or '#' truncates it. */
static int fileUrlByteIsSafe(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;

    switch (c) {
    case '-': case '_': case '.': case '~': case '/': case ':':
    case '!': case '$': case '&': case '\'': case '(': case ')':
    case '*': case '+': case ',': case ';': case '=': case '@':
        return 1;
    default:
        return 0;
    }
}

static void appendUrlEncoded(char* out, size_t outSize, size_t* written, const char* text)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = *written;

    while (*text && o + 4 < outSize) {
        unsigned char c = (unsigned char)*text++;

        if (fileUrlByteIsSafe(c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }

    out[o] = 0;
    *written = o;
}

/* "E:\deepseek-webui\index.html" (also with forward slashes, a UNC path or a
 * relative path) -> "file:///E:/deepseek-webui/index.html". GetFullPathNameW
 * normalizes separators, "." and ".." against the shell's working directory. */
static void fileUrlFromLocalPath(const char* input, char* out, size_t outSize)
{
    wchar_t wideInput[MAX_PATH_CHARS];
    wchar_t wideFull[MAX_PATH_CHARS];
    char full[MAX_PATH_CHARS];
    size_t written = 0;
    char* p;

    if (!outSize)
        return;

    out[0] = 0;
    if (outSize < 8)
        return;

    utf8ToWide(input, wideInput, MAX_PATH_CHARS);
    if (!GetFullPathNameW(wideInput, MAX_PATH_CHARS, wideFull, NULL)) {
        wcsncpy(wideFull, wideInput, MAX_PATH_CHARS - 1);
        wideFull[MAX_PATH_CHARS - 1] = 0;
    }
    wideToUtf8(wideFull, full, MAX_PATH_CHARS);

    for (p = full; *p; ++p) {
        if (*p == '\\')
            *p = '/';
    }

    if (full[0] == '/' && full[1] == '/') {
        /* UNC: the share host is the URL authority (file://host/share/x). */
        char* share = strchr(full + 2, '/');

        if (share) {
            char host[MAX_PATH_CHARS];
            size_t hostLen = (size_t)(share - (full + 2));

            if (hostLen >= sizeof(host))
                hostLen = sizeof(host) - 1;
            memcpy(host, full + 2, hostLen);
            host[hostLen] = 0;

            appendUrlEncoded(out, outSize, &written, "file://");
            appendUrlEncoded(out, outSize, &written, host);
            appendUrlEncoded(out, outSize, &written, share);
        } else {
            appendUrlEncoded(out, outSize, &written, "file:");
            appendUrlEncoded(out, outSize, &written, full);
        }
        return;
    }

    /* "C:/dir/x" -> file:///C:/dir/x (the leading slash comes from the path). */
    appendUrlEncoded(out, outSize, &written, "file://");
    appendUrlEncoded(out, outSize, &written, full);
}

static void normalizeUrl(const char* input, char* out, size_t outSize)
{
    const char* text = input;
    const char* rest;

    if (!outSize)
        return;

    out[0] = 0;

    if (!text)
        return;

    while (*text == ' ')
        ++text;

    /* Explorer's "Copy as path" quotes the path; accept the pasted form. */
    if (text[0] == '"' && text[1]) {
        size_t len = strlen(text);

        if (len >= 2 && text[len - 1] == '"') {
            static char unquoted[MAX_PATH_CHARS];

            if (len - 1 < sizeof(unquoted)) {
                memcpy(unquoted, text + 1, len - 2);
                unquoted[len - 2] = 0;
                normalizeUrl(unquoted, out, outSize);
                return;
            }
        }
    }

    if (!*text) {
        strncpy(out, "about:blank", outSize - 1);
        out[outSize - 1] = 0;
        return;
    }

    /* Address-bar paths: Ctrl+L focuses the field without selecting its text,
     * so typing a path often lands behind the current scheme
     * ("http://E:\deepseek-webui\index.html"). A local path behind a scheme
     * wins over the scheme; otherwise the URL is taken as typed. */
    if (startsWithNoCase(text, "http://") || startsWithNoCase(text, "https://")) {
        rest = text + (startsWithNoCase(text, "https://") ? 8 : 7);

        if (isDrivePath(rest) || isUncPath(rest)) {
            fileUrlFromLocalPath(rest, out, outSize);
            return;
        }

        strncpy(out, text, outSize - 1);
        out[outSize - 1] = 0;
        return;
    }

    if (startsWithNoCase(text, "file:")) {
        const char* p = text + 5;
        const char* q;
        int slashes = 0;

        while (p[slashes] == '/')
            ++slashes;
        q = p + slashes;

        /* Windows separators, or a drive/UNC path without the third slash:
         * rebuild the canonical file:/// form (the engine needs it). */
        if (strchr(p, '\\') || (slashes < 3 && (isDrivePath(q) || isUncPath(q)))) {
            if (strchr(p, '\\') && slashes == 0)
                fileUrlFromLocalPath(p, out, outSize);
            else
                fileUrlFromLocalPath(q, out, outSize);
            return;
        }

        if (slashes < 2 && q[0]) {
            /* file:C:/x */
            fileUrlFromLocalPath(q, out, outSize);
            return;
        }

        strncpy(out, text, outSize - 1);
        out[outSize - 1] = 0;
        return;
    }

    if (startsWithNoCase(text, "about:") || startsWithNoCase(text, "data:") ||
        startsWithNoCase(text, "blob:") || startsWithNoCase(text, "agave:") ||
        startsWithNoCase(text, "javascript:") || startsWithNoCase(text, "chrome:")) {
        strncpy(out, text, outSize - 1);
        out[outSize - 1] = 0;
        return;
    }

    if (isDrivePath(text) || isUncPath(text)) {
        fileUrlFromLocalPath(text, out, outSize);
        return;
    }

    /* Relative path that resolves to something on disk ("./x.html", "debug\a.html"). */
    if ((strchr(text, '\\') || strchr(text, '/')) && localPathExists(text)) {
        fileUrlFromLocalPath(text, out, outSize);
        return;
    }

    /* "localhost:8971/x" has no dot but is a host, not a search term. */
    if (startsWithNoCase(text, "localhost") &&
        (text[9] == 0 || text[9] == ':' || text[9] == '/')) {
        _snprintf(out, outSize, "http://%s", text);
        out[outSize - 1] = 0;
        return;
    }

    if (strchr(text, ' ') || !strchr(text, '.')) {
        char encoded[512];
        size_t i, o = 0;

        for (i = 0; text[i] && o + 4 < sizeof(encoded); ++i)
            encoded[o++] = (text[i] == ' ') ? '+' : text[i];

        encoded[o] = 0;

        _snprintf(out, outSize, "https://www.baidu.com/s?wd=%s", encoded);
        out[outSize - 1] = 0;
        return;
    }

    _snprintf(out, outSize, "http://%s", text);
    out[outSize - 1] = 0;
}

static void createTab(const char* url)
{
    int index = nextFreeTab();
    AgaveWebViewCallback callbacks;
    Tab* tab;

    if (index < 0)
        return;

    tab = &g_tabs[index];
    memset(tab, 0, sizeof(Tab));
    memset(&callbacks, 0, sizeof(callbacks));

    callbacks.on_title = onTitle;
    callbacks.on_url = onUrl;
    callbacks.on_progress = onProgress;
    callbacks.on_status = onStatus;
    callbacks.on_error = onError;
    callbacks.on_dialog = onDialog;
    callbacks.on_new_window = onNewWindow;
    callbacks.on_close_window = onCloseWindow;
    callbacks.on_download = onDownload;
    callbacks.on_context_menu = onContextMenu;

    tab->used = 1;
    tab->view = agave_webview_create(g_hwnd, 0, TAB_HEIGHT + TOOLBAR_H,
                                     800, 400, &callbacks, tab);

    if (!tab->view) {
        tab->used = 0;
        return;
    }

    tab->hwnd = (HWND)agave_webview_hwnd(tab->view);
    strncpy(tab->url, url, MAX_PATH_CHARS - 1);
    ShowWindow(tab->hwnd, SW_HIDE);

    agave_webview_set_zoom(tab->view, g_zoom);
    setActiveTab(index);
    agave_webview_navigate(tab->view, url);
}

static void navigateActive(const char* text, int newTab)
{
    char url[MAX_PATH_CHARS];

    normalizeUrl(text, url, sizeof(url));

    if (newTab || !g_tabs[g_activeTab].used) {
        createTab(url);
        return;
    }

    strncpy(g_tabs[g_activeTab].url, url, MAX_PATH_CHARS - 1);
    recordHistory(url);

    {
        wchar_t wideUrl[MAX_PATH_CHARS];

        utf8ToWide(url, wideUrl, MAX_PATH_CHARS);
        SetWindowTextW(g_address, wideUrl);
    }

    agave_webview_navigate(g_tabs[g_activeTab].view, url);

    /* Hand the keyboard back to the page: with focus left in the address bar the
     * next keystroke edits the URL that was just typed. */
    agave_webview_set_focus(g_tabs[g_activeTab].view);
}

static void addBookmarkCurrent(void)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used)
        return;

    addBookmark(tab->url, tab->title);
    strncpy(tab->status, "Bookmark added", sizeof(tab->status) - 1);
    updateStatus();
}

static void toggleFind(void)
{
    g_findVisible = !g_findVisible;

    ShowWindow(g_findEdit, g_findVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_findPrev, g_findVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_findNext, g_findVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_findClose, g_findVisible ? SW_SHOW : SW_HIDE);

    if (g_findVisible)
        SetFocus(g_findEdit);

    layoutAll();
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void doFind(int forward)
{
    char text[256];
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view)
        return;

    readControlText(g_findEdit, text, sizeof(text));

    if (text[0])
        agave_webview_search(tab->view, text, forward, 0);
}

static void updateToolbarState(void)
{
    Tab* tab = &g_tabs[g_activeTab];

    EnableWindow(g_back, tab->used && agave_webview_can_back(tab->view));
    EnableWindow(g_forward, tab->used && agave_webview_can_forward(tab->view));
}

/* ------------------------------------------------------------------------- */
/* menu                                                                      */
/* ------------------------------------------------------------------------- */

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

    for (i = g_historyCount - 1; i >= 0 && i >= g_historyCount - 20; --i)
        AppendMenuA(history, MF_STRING, 200 + (g_historyCount - 1 - i), g_history[i]);

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
            navigateActive(g_history[index], 0);
        return;
    }

    if (choice >= 100) {
        int index = choice - 100;

        if (index >= 0 && index < g_bookmarkCount)
            navigateActive(g_bookmarks[index].url, 0);
        return;
    }

    switch (choice) {
    case 1: addBookmarkCurrent(); break;
    case 2: g_zoom = g_zoom < 400 ? g_zoom + 25 : g_zoom; applyZoomToActive(); updateStatus(); break;
    case 3: g_zoom = g_zoom > 25 ? g_zoom - 25 : g_zoom; applyZoomToActive(); updateStatus(); break;
    case 4: g_zoom = 100; applyZoomToActive(); updateStatus(); break;
    case 5: navigateActive("about:blank", 1); break;
    case 6: toggleFind(); break;
    case 7: if (g_tabs[g_activeTab].used) agave_webview_reload(g_tabs[g_activeTab].view); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* painting: tab bar + status bar                                            */
/* ------------------------------------------------------------------------- */

static void paintTabs(HDC hdc, int width)
{
    int i, x = 0;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(hdc, font);
    RECT strip;

    SetRect(&strip, 0, 0, width, TAB_HEIGHT);
    SetBkMode(hdc, TRANSPARENT);
    FillRect(hdc, &strip, (HBRUSH)(COLOR_BTNFACE + 1));

    for (i = 0; i < MAX_TABS; ++i) {
        RECT box;
        char label[128];

        if (!g_tabs[i].used)
            continue;

        SetRect(&box, x, 2, x + TAB_W, TAB_HEIGHT - 2);
        FillRect(hdc, &box, (HBRUSH)(INT_PTR)((i == g_activeTab) ? COLOR_WINDOW : COLOR_BTNSHADOW + 1));

        SetRect(&box, x + 6, 2, x + 146, TAB_HEIGHT - 2);
        _snprintf(label, sizeof(label), "%s", g_tabs[i].title[0] ? g_tabs[i].title : "New tab");
        label[sizeof(label) - 1] = 0;

        {
            wchar_t wideLabel[128];

            utf8ToWide(label, wideLabel, 128);
            DrawTextW(hdc, wideLabel, -1, &box,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        }

        SetRect(&box, x + 148, 2, x + 168, TAB_HEIGHT - 2);
        DrawTextW(hdc, L"x", -1, &box, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

        x += TAB_W;
    }

    SelectObject(hdc, old);
}

static void paintStatus(HDC hdc, int width, int height)
{
    RECT bar;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(hdc, font);

    SetRect(&bar, 0, height - STATUS_H, width, height);
    FillRect(hdc, &bar, (HBRUSH)(COLOR_BTNFACE + 1));

    bar.left = 8;
    bar.right = width - 8;
    SetBkMode(hdc, TRANSPARENT);

    {
        wchar_t wide[1024];

        utf8ToWide(g_status, wide, 1024);
        DrawTextW(hdc, wide, -1, &bar,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }

    SelectObject(hdc, old);
}

/* ------------------------------------------------------------------------- */
/* window procedure                                                          */
/* ------------------------------------------------------------------------- */

/* window.close(): the engine thread asks for a tab to go away. The tab table
 * is main-thread state, so the actual close happens on the main thread. */
#define AGW_SHELL_CLOSE_TAB (WM_APP + 21)

static void onCloseWindow(AgaveWebView* view, void* user)
{
    (void)user;

    if (g_hwnd)
        PostMessageA(g_hwnd, AGW_SHELL_CLOSE_TAB, (WPARAM)view, 0);
}

static LRESULT CALLBACK shellProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case AGW_SHELL_CLOSE_TAB: {
        int i;

        for (i = 0; i < MAX_TABS; ++i) {
            if (g_tabs[i].used && g_tabs[i].view == (AgaveWebView*)wParam) {
                closeTab(i);
                break;
            }
        }
        return 0;
    }

    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    case WM_SIZE:
        layoutAll();
        updateToolbarState();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT client;
        HDC hdc = BeginPaint(hwnd, &ps);

        GetClientRect(hwnd, &client);
        paintTabs(hdc, client.right);
        paintStatus(hdc, client.right, client.bottom);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);
        int i, slot = 0;

        if (y >= TAB_HEIGHT)
            break;

        for (i = 0; i < MAX_TABS; ++i) {
            int left;

            if (!g_tabs[i].used)
                continue;

            left = slot * TAB_W;

            if (x >= left && x < left + TAB_W) {
                if (x >= left + 148)
                    closeTab(i);
                else
                    setActiveTab(i);
                return 0;
            }

            ++slot;
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        Tab* tab = &g_tabs[g_activeTab];

        switch (id) {
        case ID_BACK:    if (tab->used) agave_webview_back(tab->view); break;
        case ID_FORWARD: if (tab->used) agave_webview_forward(tab->view); break;
        case ID_RELOAD:  if (tab->used) agave_webview_reload(tab->view); break;
        case ID_STOP:    if (tab->used) agave_webview_stop(tab->view); break;

        case ID_GO: {
            char text[MAX_PATH_CHARS];

            readControlText(g_address, text, sizeof(text));
            navigateActive(text, 0);
            break;
        }

        case ID_NEWTAB: navigateActive("about:blank", 1); break;
        case ID_MENU:   showMainMenu(); break;

        case ID_FIND_NEXT:  doFind(1); break;
        case ID_FIND_PREV:  doFind(-1); break;
        case ID_FIND_CLOSE: toggleFind(); break;
        default: break;
        }
        return 0;
    }

    case WM_DESTROY:
        saveSession();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

/* ------------------------------------------------------------------------- */
/* entry point                                                               */
/* ------------------------------------------------------------------------- */

static HWND makeButton(const char* text, int id)
{
    return CreateWindowA("BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)id, g_instance, NULL);
}

/* Exit forensics: if some library calls exit() from inside a callback, the
 * message loop never returns and we never see why. This handler runs on exit()
 * and prints who did it. */
static void shellExitTrace(void)
{
    void* frames[32];
    unsigned short count = CaptureStackBackTrace(0, 32, frames, NULL);
    int i;

    {
        unsigned long long exe = (unsigned long long)GetModuleHandleA(NULL);
        unsigned long long av  = (unsigned long long)GetModuleHandleA("libagave.dll");
        unsigned long long ab  = (unsigned long long)GetModuleHandleA("antbridge.dll");

        fprintf(stderr, "[shell] atexit fired, bases exe=%llx agave=%llx ant=%llx\n", exe, av, ab);

        for (i = 0; i < (int)count; ++i) {
            unsigned long long a = (unsigned long long)frames[i];
            const char* mod = "?";
            unsigned long long off = a;

            if (av && a >= av && a < av + 0x2000000ULL) { mod = "libagave"; off = a - av; }
            else if (ab && a >= ab && a < ab + 0x2000000ULL) { mod = "antbridge"; off = a - ab; }
            else if (exe && a >= exe && a < exe + 0x100000ULL) { mod = "exe"; off = a - exe; }

            fprintf(stderr, "  #%02d %s+0x%llx\n", i, mod, off);
        }
    }

    fflush(stderr);
}

/* ------------------------------------------------------------------------- */
/* probe mode: headless load + JS evaluation, for page debugging             */
/* ------------------------------------------------------------------------- */

#define PROBE_TIMER_EVAL    1
#define PROBE_TIMER_SETTLE  2
#define PROBE_TIMER_TIMEOUT 3
#define PROBE_TIMER_POLL    4   /* repeating: poll new document after reload */

/* Engine-thread callbacks post this to the probe window (main thread) to arm
 * the eval timer: window timers must be installed by the owning thread. */
#define AGW_PROBE_MSG (WM_APP + 77)

/* Probe exit codes (process exit status): scripts and CI can tell failure
 * classes apart without parsing the log.
 *   0  eval ran and the page called probeExit()
 *   1  usage error (missing URL etc.)
 *   2  timeout: the load never finished
 *   3  timeout: load finished but no [exit] arrived
 *   4  load finished and eval ran, but no [exit] arrived (script error?)
 *   5  the navigation itself failed (on_error), no [exit]            */
#define PROBE_EXIT_OK       0
#define PROBE_EXIT_USAGE    1
#define PROBE_EXIT_TIMEOUT_LOAD  2
#define PROBE_EXIT_TIMEOUT_NOEXIT 3
#define PROBE_EXIT_NOEXIT   4
#define PROBE_EXIT_LOADERROR 5
#define PROBE_EXIT_SANITY   6   /* eval ran against an empty document */

static HWND          g_probeWnd;
static AgaveWebView* g_probeView;
static AgaveWebView* g_probeNewView;   /* window opened by the page (target=_blank) */
static char          g_probeScript[8192] =
    "agavePostMessage('[dump] ' + document.documentElement.outerHTML)";
static int g_probeWaitMs    = 3000;   /* after load finished, before eval   */
static int g_probeSettleMs  = 1000;   /* after eval, before exit            */
static int g_probeTimeoutMs = 60000;  /* hard cap from start                */
static int g_probeFinished;
static int g_probeEvalDone;
static int g_probeGotExit;
static int g_probeSawError;
static int g_probeClosed;        /* window.close() reached the host */
static int g_probeTimedOut;
static int g_probeSanityFailed;
static int g_probeReloadsLeft;
static int g_probeReloadMode;    /* --reload was given                     */
static int g_probeRound      = 1; /* current round (1 = first document)      */
static int g_probeTotalRounds = 1; /* 1 + number of reloads                 */
static int g_probePollCount;      /* readyState polls in the current round  */
static int g_probeRoundsDone;     /* every round finished -> OK exit code   */
static char g_probeToken[64];     /* per-reload marker for the NEW document */
static int  g_probeVisible;       /* AGB_PROBE_SHOW: the window is on screen  */

/* Command line, parsed before any window exists: a --probe run must never
 * create the browser window (it used to be shown, then left on screen for the
 * whole capture because probeRun runs its own message loop). */
static int  g_probeMode;                        /* --probe was given          */
static int  g_showUsage;                        /* --help                     */
static int  g_argError;                         /* --probe without a URL       */
static char g_startupUrl[MAX_PATH_CHARS];       /* normalized, may be empty   */

static void probePrint(const char* tag, const char* text)
{
    printf("[%7.1fs] %s %s\n",
           (double)GetTickCount() / 1000.0, tag, text ? text : "");
    fflush(stdout);
}

static void probeOnMessage(AgaveWebView* view, const char* message, void* user)
{
    (void)view;
    (void)user;

    /* probeExit(result) sends this sentinel: print the payload and leave at
     * once instead of waiting for the settle timer. */
    if (message && !strncmp(message, "__agb_exit__", 12)) {
        g_probeGotExit = 1;
        probePrint("[exit]", message + 12);
        PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
        return;
    }

    /* Empty-document sanity failure: report and leave with the SANITY code. */
    if (message && !strncmp(message, "__agb_sanity__", 14)) {
        g_probeSanityFailed = 1;
        probePrint("[sanity]", message + 14);
        PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
        return;
    }

    /* The page's own window load event: the proper moment to run --eval.
     * Route through the main thread (PostMessage) - never SetTimer from the
     * engine thread. */
    if (message && !strncmp(message, "__agb_loaded__", 14)) {
        if (!g_probeFinished) {
            probePrint("[probe]", "window load event");
            PostMessageA(g_probeWnd, AGW_PROBE_MSG, g_probeWaitMs, 0);
        }
        return;
    }

    probePrint("[msg]", message);
}

static void probeOnError(AgaveWebView* view, int code, const char* url, void* user)
{
    (void)view;
    (void)user;

    g_probeSawError = 1;
    fprintf(stderr, "[probe] load error code=%d url=%s\n", code, url ? url : "");
}

static int probeOnNewWindow(AgaveWebView* view, AgaveWebView* newView,
                            const char* url, void* user)
{
    (void)view;
    (void)user;

    /* Keep it: the probe reports the URL the new window ended up on, which is
     * what tells a working new-window path from a blank one. */
    g_probeNewView = newView;
    fprintf(stderr, "[probe] new window requested url=%s\n", url ? url : "");
    fflush(stderr);
    return 1;
}

static int probeOnDialog(AgaveWebView* view, int type, const char* message,
                         const char* defaultValue, char* result, int resultSize,
                         void* user)
{
    (void)view;
    (void)type;
    (void)defaultValue;
    (void)result;
    (void)resultSize;
    (void)user;

    /* A beforeunload guard blocks the navigation; everything else (page
     * alert/confirm in other tests) auto-accepts. AGB_PROBE_ALLOW_UNLOAD=1
     * flips the guard answer, to check that allowing really does navigate. */
    fprintf(stderr, "[probe] dialog type=%d msg=%s\n", type, message ? message : "");
    fflush(stderr);

    if (message && strstr(message, "LEAVE-GUARD")) {
        const char* allow = getenv("AGB_PROBE_ALLOW_UNLOAD");

        return (allow && *allow && *allow != '0') ? 1 : 0;
    }

    return 1;
}

static void probeOnCloseWindow(AgaveWebView* view, void* user)
{
    (void)view;
    (void)user;

    /* window.close() reached the host: report and leave (a success exit, so
     * a script can assert the whole chain worked). */
    g_probeClosed = 1;
    g_probeGotExit = 1;
    probePrint("[close]", "window.close() reached the host");
    PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
}

static void probeOnProgress(AgaveWebView* view, int percent, int finished, void* user)
{
    (void)view;
    (void)percent;
    (void)user;

    /* Engine-thread callback: never SetTimer here (the probe window is owned
     * by the main thread; cross-thread window timers are undefined). Post the
     * request to the main loop instead; progress finished is only a FALLBACK
     * trigger with a longer delay - the real schedule comes from the page's
     * window load event. */
    if (finished && !g_probeFinished && !g_probeReloadMode) {
        probePrint("[probe]", g_probeSawError ? "progress finished (with error)" : "progress finished");
        PostMessageA(g_probeWnd, AGW_PROBE_MSG, g_probeWaitMs + 10000, 0);
    }
}

static LRESULT CALLBACK probeWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == AGW_PROBE_MSG) {
        /* Engine thread asks for the eval timer; wParam carries the delay.
         * Also stops the post-reload readyState poll: the first
         * __agb_loaded__ means the new document is done. */
        KillTimer(g_probeWnd, PROBE_TIMER_POLL);

        if (!g_probeFinished) {
            g_probeFinished = 1;
            /* Reload rounds: the document is already parsed, the extra
             * wait only exists for pages that paint after load. */
            SetTimer(g_probeWnd, PROBE_TIMER_EVAL,
                     (UINT)(g_probeRound > 1 ? 150 : wParam), NULL);
        }
        return 0;
    }

    /* The message loop must end when the window goes away: GetMessage(NULL)
     * only returns 0 on WM_QUIT, never because the last window was destroyed. */
    if (message == WM_DESTROY)
        PostQuitMessage(0);

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

static int probeRun(const char* url)
{
    WNDCLASSA wc;
    AgaveWebViewCallback callbacks;
    MSG msg;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = probeWndProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = "AgaveProbeWindow";
    RegisterClassA(&wc);

    /* Hidden popup: the control only needs a parent HWND. No frame, no caption
     * and WS_EX_TOOLWINDOW, so a probe never shows a taskbar entry or steals
     * the foreground - the capture is headless unless AGB_PROBE_SHOW is set. */
    g_probeWnd = CreateWindowExA(WS_EX_TOOLWINDOW, "AgaveProbeWindow", "agave probe",
                                 WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
                                 1100, 800, NULL, NULL, g_instance, NULL);
    if (!g_probeWnd) {
        fprintf(stderr, "[probe] CreateWindowEx failed: %lu\n", (unsigned long)GetLastError());
        return PROBE_EXIT_USAGE;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_message = probeOnMessage;
    callbacks.on_progress = probeOnProgress;
    callbacks.on_error = probeOnError;
    callbacks.on_new_window = probeOnNewWindow;
    callbacks.on_close_window = probeOnCloseWindow;
    callbacks.on_dialog = probeOnDialog;

    g_probeView = agave_webview_create(g_probeWnd, 0, 0, 1100, 800, &callbacks, NULL);
    if (!g_probeView) {
        fprintf(stderr, "[probe] agave_webview_create failed\n");
        return PROBE_EXIT_USAGE;
    }

    /* AGB_PROBE_SHOW=1 shows the window; debugging aid for load paths that
     * behave differently when the control is never visible. A hidden window
     * gets no WM_PAINT, so this is also what pixel checks need. */
    g_probeVisible = getenv("AGB_PROBE_SHOW") != NULL;
    if (g_probeVisible)
        ShowWindow(g_probeWnd, SW_SHOWNOACTIVATE);

    fprintf(stderr, "[probe] headless %s (pid %lu)\n",
            g_probeVisible ? "window shown (AGB_PROBE_SHOW)" : "window hidden, AGB_PROBE_SHOW=1 to show",
            (unsigned long)GetCurrentProcessId());

    SetTimer(g_probeWnd, PROBE_TIMER_TIMEOUT, g_probeTimeoutMs, NULL);

    /* Focus only makes sense for a visible window; a hidden control that grabs
     * focus activates a window nobody can see. */
    if (g_probeVisible)
        agave_webview_set_focus(g_probeView);

    agave_webview_navigate(g_probeView, url);

    /* Load hook: the page reports its own window load event; --eval runs on
     * that (plus --wait), not on the network-layer progress callback. */
    agave_webview_eval(g_probeView,
        "(function () {"
        " var f = function () { agavePostMessage('__agb_loaded__' + location.href); };"
        " if (document.readyState === 'complete') { f(); return; }"
        " window.addEventListener('load', f);"
        "})();");

    while (GetMessageA(&msg, NULL, 0, 0)) {
        /* Only our own window's timers: the webview control runs internal
         * SetTimer(id=1, ~10ms) frame-pump timers on its own child window,
         * and their WM_TIMER (same message, same wParam=1) used to fire the
         * EVAL branch instantly. Filter by hwnd. */
        if (msg.message == WM_TIMER && msg.hwnd == g_probeWnd) {
            if (msg.wParam == PROBE_TIMER_TIMEOUT) {
                g_probeTimedOut = 1;
                probePrint("[probe]", g_probeFinished ? "timeout, exiting" : "timeout, load never finished");
                break;
            }

            if (msg.wParam == PROBE_TIMER_EVAL) {
                KillTimer(g_probeWnd, PROBE_TIMER_EVAL);

                /* agave_webview_eval pumps the control's queue, which can
                 * re-enter this message loop while the EVAL timer message is
                 * still pending; without the flag the branch re-runs and the
                 * script gets evaluated once per nesting level. */
                if (g_probeEvalDone)
                    continue;

                g_probeEvalDone = 1;

                /* Commit check first: a failed/stalled navigation still
                 * reports progress finished=1, and the eval would then run
                 * against the initial about:blank document. Require the
                 * control's current URL to carry the requested host before
                 * trusting the document. */
                {
                    char host[256] = "";
                    const char* hs = strstr(url, "://");

                    if (hs) {
                        const char* he;
                        size_t n;

                        hs += 3;
                        he = strchr(hs, '/');
                        n = he ? (size_t)(he - hs) : strlen(hs);
                        if (n >= sizeof(host))
                            n = sizeof(host) - 1;
                        memcpy(host, hs, n);
                    }

                    if (host[0]) {
                        const char* cur = agave_webview_url(g_probeView);

                        if (!cur || !strstr(cur, host)) {
                            g_probeSanityFailed = 1;
                            printf("[sanity] load never committed: document url=%s, wanted host %s\n",
                                   cur && cur[0] ? cur : "(none)", host);
                            fflush(stdout);
                            PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
                            continue;
                        }
                    }
                }

                probePrint("[probe]", "running eval");
                /* Sanity first: a failed navigation still reports progress
                 * finished=1, and the eval would then run against an empty
                 * document. Install probeExit() only when the document really
                 * exists; otherwise probeExit() becomes a sanity reporter and
                 * the run ends with the SANITY exit code instead of a
                 * misleading OK. */
                agave_webview_eval(g_probeView,
                    /* A failed navigation can commit an empty document under
                     * the requested URL (host check alone can't catch that);
                     * a parsed page always has children under <body>. */
                    "window.__agbDocOk = ((typeof document.body === 'object') "
                    "&& (document.body !== null) "
                    "&& (document.body.children.length > 0));"
                    "if (window.__agbDocOk) {"
                    " window.probeExit = function (result) {"
                    "  agavePostMessage('__agb_exit__' + (result === undefined ? '' : String(result)));"
                    " };"
                    "} else {"
                    " agavePostMessage('__agb_sanity__ document is empty (page never parsed?)');"
                    " window.probeExit = function (result) {"
                    "  agavePostMessage('__agb_sanity__ skipped eval: ' + (result === undefined ? '' : String(result)));"
                    " };"
                    "}");
                /* Tag the document with the round number: the document itself
                 * is replaced on reload, so nothing else survives to tell
                 * the rounds apart. */
                {
                    char script[8300];

                    snprintf(script, sizeof(script),
                             "window.__agbRound=%d;window.__agbRounds=%d;%s",
                             g_probeRound, g_probeTotalRounds, g_probeScript);
                    agave_webview_eval(g_probeView, script);
                }
                SetTimer(g_probeWnd, PROBE_TIMER_SETTLE, g_probeSettleMs, NULL);
                continue;
            }

            if (msg.wParam == PROBE_TIMER_SETTLE) {
                KillTimer(g_probeWnd, PROBE_TIMER_SETTLE);

                /* --reload: start the next round. The new document has no
                 * load hook yet, so poll its readyState; each poll eval that
                 * sees 'complete' posts __agb_loaded__, which stops the poll
                 * (KillTimer in probeWndProc) and schedules the next eval. */
                if (g_probeReloadsLeft > 0) {
                    char mark[128];

                    g_probeReloadsLeft--;
                    g_probeRound++;
                    g_probeFinished = 0;
                    g_probeEvalDone = 0;
                    g_probePollCount = 0;
                    probePrint("[probe]", "reloading");

                    /* Mark the CURRENT document. The poll only fires on a
                     * document that does NOT carry the marker, i.e. on the
                     * freshly loaded one. If a reload reuses the old
                     * document, the poll never fires and the run says so
                     * instead of silently testing the stale document. */
                    snprintf(g_probeToken, sizeof(g_probeToken),
                             "R%d_%lu", g_probeRound, (unsigned long)GetTickCount());
                    snprintf(mark, sizeof(mark),
                             "window.__agbPreReload='%s';", g_probeToken);
                    agave_webview_eval(g_probeView, mark);

                    SetTimer(g_probeWnd, PROBE_TIMER_POLL, 400, NULL);
                    agave_webview_reload(g_probeView);
                    continue;
                }

                if (g_probeReloadMode)
                    g_probeRoundsDone = 1;

                break;
            }

            if (msg.wParam == PROBE_TIMER_POLL) {
                char poll[512];

                g_probePollCount++;

                /* Give up waiting for a new document: arm the eval anyway so
                 * the round still reports what the frame holds. */
                if (g_probePollCount > 30) {
                    KillTimer(g_probeWnd, PROBE_TIMER_POLL);
                    probePrint("[probe]", "poll exhausted, arming eval anyway");
                    PostMessageA(g_probeWnd, AGW_PROBE_MSG, 0, 0);
                    continue;
                }

                snprintf(poll, sizeof(poll),
                         "var m = typeof window.__agbPreReload;"
                         "if (document.readyState === 'complete' && m !== '%s') {"
                         " agavePostMessage('__agb_loaded__' + location.href);"
                         "} else {"
                         " agavePostMessage('[poll] rs=' + document.readyState"
                         " + ' marker=' + m + ' id=' + window.__docId);"
                         "}",
                         g_probeToken);
                agave_webview_eval(g_probeView, poll);
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (g_probeNewView) {
        const char* u = agave_webview_url(g_probeNewView);
        const char* t = agave_webview_title(g_probeNewView);

        printf("[probe] new window url=%s title=%s\n",
               u && u[0] ? u : "(none)", t && t[0] ? t : "(none)");
        fflush(stdout);
        agave_webview_destroy(g_probeNewView);
        g_probeNewView = NULL;
    }

    {
        const char* u = agave_webview_url(g_probeView);

        printf("[probe] main url=%s\n", u && u[0] ? u : "(none)");
        fflush(stdout);
    }

    agave_webview_destroy(g_probeView);

    {
        int code;

        if (g_probeSanityFailed)
            code = PROBE_EXIT_SANITY;
        else if (g_probeGotExit)
            code = PROBE_EXIT_OK;
        else if (g_probeReloadMode && g_probeRoundsDone)
            code = PROBE_EXIT_OK;   /* every reload round ran to the end */
        else if (g_probeTimedOut && !g_probeFinished)
            code = PROBE_EXIT_TIMEOUT_LOAD;
        else if (g_probeTimedOut)
            code = PROBE_EXIT_TIMEOUT_NOEXIT;
        else if (g_probeSawError)
            code = PROBE_EXIT_LOADERROR;
        else
            code = PROBE_EXIT_NOEXIT;

        fprintf(stderr, "[probe] exit code %d\n", code);
        fflush(stderr);
        return code;
    }
}

static void probeUsage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  agave_shell.exe [url]\n"
            "  agave_shell.exe --probe <url> [--eval <js>] [--wait <ms>]\n"
            "                  [--settle <ms>] [--timeout <ms>]\n"
            "  --eval    script to run after the load finished (default dumps outerHTML\n"
            "            via agavePostMessage; page console goes to stderr)\n"
            "            the script may call probeExit(result) to leave immediately\n"
            "  --wait    settle time after 'load finished' before eval (default 3000)\n"
            "  --settle  time after eval before exit (default 1000)\n"
            "  --timeout hard cap from start (default 60000)\n"
            "  --reload <n>  reload the page n times and run the eval after\n"
            "                every round (page lifecycle comparison)\n"
            "  --offline module fetches are served from the cache only\n"
            "            (no network; warm the cache with a normal run first)\n");
}

/* Parses the raw command line into argv/argc in place, honoring double quotes.
 * GetCommandLineA keeps the executable name quoting, so the old single-token
 * skipper swallowed a quoted URL argument whenever the exe path was quoted
 * (Start-Process, drag&drop, Explorer) and silently fell back to the session. */
static int parseCommandLine(char* cmdline, char** argv, int maxArgs)
{
    int argc = 0;
    char* p = cmdline;

    while (*p && argc < maxArgs) {
        while (*p == ' ' || *p == '\t')
            ++p;

        if (!*p)
            break;

        if (*p == '"') {
            ++p;
            argv[argc++] = p;

            while (*p && *p != '"')
                ++p;
        } else {
            argv[argc++] = p;

            while (*p && *p != ' ' && *p != '\t')
                ++p;
        }

        if (*p)
            *p++ = 0;
    }

    return argc;
}

/* Command line before any window exists: a --probe run must never create the
 * browser window. It used to be created and shown up front, so every headless
 * capture flashed a full browser and left it on screen for the whole run
 * (probeRun drives its own message loop on its own hidden window). */
static void parseShellArgv(void)
{
    static char cmdlineBuf[8192];
    char* argv[40];
    int argc = 0;
    const char* urlArg = NULL;
    const char* raw = GetCommandLineA();
    int i;

    if (raw && *raw) {
        strncpy(cmdlineBuf, raw, sizeof(cmdlineBuf) - 1);
        cmdlineBuf[sizeof(cmdlineBuf) - 1] = 0;
        argc = parseCommandLine(cmdlineBuf, argv, 40);
    }

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--probe")) {
            g_probeMode = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            g_showUsage = 1;
        } else if (!strcmp(argv[i], "--eval") && i + 1 < argc) {
            strncpy(g_probeScript, argv[++i], sizeof(g_probeScript) - 1);
            g_probeScript[sizeof(g_probeScript) - 1] = 0;
        } else if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            g_probeWaitMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--settle") && i + 1 < argc) {
            g_probeSettleMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            g_probeTimeoutMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--reload") && i + 1 < argc) {
            g_probeReloadsLeft = atoi(argv[++i]);
            g_probeReloadMode = 1;
            g_probeTotalRounds = g_probeReloadsLeft + 1;
        } else if (!strcmp(argv[i], "--offline")) {
            /* Module fetches come from the cache only (read by the Ant runtime
             * as AGB_OFFLINE). Set before any view exists. putenv, not
             * SetEnvironmentVariable: MinGW's getenv reads the CRT _environ
             * copy, which SetEnvironmentVariableA misses. */
            putenv("AGB_OFFLINE=1");
        } else if (argv[i][0] != '-') {
            urlArg = argv[i];
        }
    }

    if (urlArg && urlArg[0]) {
        /* Same rules as typing the address: a local path becomes a file:// URL,
         * "localhost:port" stays a host instead of a search term. */
        normalizeUrl(urlArg, g_startupUrl, sizeof(g_startupUrl));
    }

    if (g_probeMode && !g_startupUrl[0] && !g_showUsage) {
        fprintf(stderr, "[probe] missing URL\n");
        probeUsage();
        g_argError = 1;
    }
}

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show)
{
    WNDCLASSA wc;
    MSG msg;
    char initial[MAX_PATH_CHARS];
    static char session[8][MAX_PATH_CHARS];
    int restored = 0;

    (void)previous;
    (void)show;
    (void)commandLine;

    atexit(shellExitTrace);
    g_instance = instance;

    /* Headless first: no window, no toolbar, no session/bookmark files. */
    parseShellArgv();

    if (g_showUsage) {
        probeUsage();
        return 0;
    }

    if (g_argError)
        return PROBE_EXIT_USAGE;

    if (g_probeMode)
        return probeRun(g_startupUrl);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = shellProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AgaveShellWindow";

    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, "AgaveShellWindow", "BrowserShell - Agave",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1100, 800, NULL, NULL, instance, NULL);

    if (!g_hwnd) {
        fprintf(stderr, "[shell] CreateWindowEx failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }

    fprintf(stderr, "[shell] window ready hwnd=%p\n", (void*)g_hwnd);

    dataDirInit();
    loadBookmarks();

    g_back = makeButton("<", ID_BACK);
    g_forward = makeButton(">", ID_FORWARD);
    g_reload = makeButton("R", ID_RELOAD);
    g_stop = makeButton("X", ID_STOP);
    g_go = makeButton("Go", ID_GO);
    g_newtab = makeButton("+", ID_NEWTAB);
    g_menu = makeButton("=", ID_MENU);

    /* Unicode edit controls: CreateWindowExW + L"EDIT" (the ANSI variant would
     * push typed paths through the ANSI code page, and GetWindowTextA with it). */
    g_address = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)ID_ADDRESS, instance, NULL);

    g_findEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | ES_AUTOHSCROLL,
                                 0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)ID_FIND_EDIT, instance, NULL);
    g_findNext = makeButton("v", ID_FIND_NEXT);
    g_findPrev = makeButton("^", ID_FIND_PREV);
    g_findClose = makeButton("x", ID_FIND_CLOSE);

    ShowWindow(g_findEdit, SW_HIDE);
    ShowWindow(g_findNext, SW_HIDE);
    ShowWindow(g_findPrev, SW_HIDE);
    ShowWindow(g_findClose, SW_HIDE);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    /* The command line was already parsed in parseShellArgv(): g_startupUrl
     * wins over the restored session. */
    if (g_startupUrl[0]) {
        strncpy(initial, g_startupUrl, sizeof(initial) - 1);
        initial[sizeof(initial) - 1] = 0;
    } else {
        restored = loadSession(session, 8);
        strcpy(initial, restored > 0 ? session[0] : "http://www.163.com");
    }

    if (restored > 1) {
        int i;

        for (i = 0; i < restored; ++i)
            createTab(session[i]);

        setActiveTab(g_activeTab);
    } else {
        createTab(initial);
    }

    fprintf(stderr, "[shell] entering message loop\n");

    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.hwnd == g_address && msg.wParam == VK_RETURN) {
                char text[MAX_PATH_CHARS];

                readControlText(g_address, text, sizeof(text));
                navigateActive(text, 0);
                continue;
            }

            /* Escape in the address bar drops the edit: show the URL the tab is
             * really on and hand the keyboard back to the page. */
            if (msg.hwnd == g_address && msg.wParam == VK_ESCAPE) {
                Tab* escapeTab = &g_tabs[g_activeTab];

                if (escapeTab->used) {
                    wchar_t wideUrl[MAX_PATH_CHARS];

                    utf8ToWide(escapeTab->url, wideUrl, MAX_PATH_CHARS);
                    SetWindowTextW(g_address, wideUrl);
                    if (escapeTab->view)
                        agave_webview_set_focus(escapeTab->view);
                }
                continue;
            }

            if (msg.hwnd == g_findEdit && msg.wParam == VK_RETURN) {
                doFind(1);
                continue;
            }

            if (GetKeyState(VK_CONTROL) & 0x8000) {
                int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

                if (msg.wParam == 'D' && !shift) { addBookmarkCurrent(); continue; }
                if (msg.wParam == 'F' && !shift) { toggleFind(); continue; }
                if (msg.wParam == 'T') {
                    /* ctrl+shift+T reopens the last closed tab, ctrl+T a new one. */
                    if (shift) reopenClosedTab();
                    else navigateActive("about:blank", 1);
                    continue;
                }
                if (msg.wParam == 'W' && !shift) { closeTab(g_activeTab); continue; }
                if (msg.wParam == 'R' && !shift) { if (g_tabs[g_activeTab].used) agave_webview_reload(g_tabs[g_activeTab].view); continue; }
                if (msg.wParam == VK_TAB) { switchTab(shift ? -1 : 1); continue; }
                /* Ctrl+L selects the current URL: typing then replaces it, which
                 * is also what stops "http://" + a typed path from being glued
                 * onto the old one. */
                if (msg.wParam == 'L' && !shift) {
                    SetFocus(g_address);
                    SendMessageW(g_address, EM_SETSEL, (WPARAM)0, (LPARAM)-1);
                    continue;
                }
                if (msg.wParam == VK_ADD || msg.wParam == 0xBB) {
                    g_zoom = g_zoom < 400 ? g_zoom + 25 : g_zoom;
                    applyZoomToActive();
                    updateStatus();
                    continue;
                }
                if (msg.wParam == VK_SUBTRACT || msg.wParam == 0xBD) {
                    g_zoom = g_zoom > 25 ? g_zoom - 25 : g_zoom;
                    applyZoomToActive();
                    updateStatus();
                    continue;
                }
                if (msg.wParam == '0') {
                    g_zoom = 100;
                    applyZoomToActive();
                    updateStatus();
                    continue;
                }
            }

            if (msg.wParam == VK_F5 && g_tabs[g_activeTab].used) {
                agave_webview_reload(g_tabs[g_activeTab].view);
                continue;
            }

            /* alt+left / alt+right: back and forward. */
            if ((GetKeyState(VK_MENU) & 0x8000) && g_tabs[g_activeTab].used) {
                if (msg.wParam == VK_LEFT) { agave_webview_back(g_tabs[g_activeTab].view); continue; }
                if (msg.wParam == VK_RIGHT) { agave_webview_forward(g_tabs[g_activeTab].view); continue; }
            }
        }

        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    fprintf(stderr, "[shell] message loop ended\n");

    return 0;
}
