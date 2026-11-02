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

    /* Re-parent into the shell so it behaves like a tab, not a floating popup. */
    SetParent(g_tabs[index].hwnd, g_hwnd);
    ShowWindow(g_tabs[index].hwnd, SW_HIDE);

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

static void normalizeUrl(const char* input, char* out, size_t outSize)
{
    const char* text = input;

    while (*text == ' ')
        ++text;

    if (!strncmp(text, "http://", 7) || !strncmp(text, "https://", 8) ||
        !strncmp(text, "about:", 6) || !strncmp(text, "file://", 7) ||
        !strncmp(text, "data:", 5)) {
        strncpy(out, text, outSize - 1);
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

    GetWindowTextA(g_findEdit, text, sizeof(text));

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
        FillRect(hdc, &box, (HBRUSH)((i == g_activeTab) ? COLOR_WINDOW : COLOR_BTNSHADOW + 1));

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

static LRESULT CALLBACK shellProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
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

            GetWindowTextA(g_address, text, sizeof(text));
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

    g_instance = instance;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = shellProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AgaveShellWindow";

    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, "AgaveShellWindow", "BrowserShell - Agave",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1100, 800, NULL, NULL, instance, NULL);

    if (!g_hwnd)
        return 1;

    dataDirInit();
    loadBookmarks();

    g_back = makeButton("<", ID_BACK);
    g_forward = makeButton(">", ID_FORWARD);
    g_reload = makeButton("R", ID_RELOAD);
    g_stop = makeButton("X", ID_STOP);
    g_go = makeButton("Go", ID_GO);
    g_newtab = makeButton("+", ID_NEWTAB);
    g_menu = makeButton("=", ID_MENU);

    g_address = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)ID_ADDRESS, instance, NULL);

    g_findEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
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

    /* Initial tabs: command line URL wins, otherwise restore the session. */
    {
        const char* cmd = GetCommandLineA();
        const char* arg = NULL;

        if (cmd) {
            if (*cmd == '"') {
                cmd = strchr(cmd + 1, '"');
                if (cmd)
                    ++cmd;
            } else {
                cmd = strchr(cmd, ' ');
            }

            while (cmd && *cmd == ' ')
                ++cmd;

            if (cmd && *cmd)
                arg = cmd;
        }

        if (arg && arg[0] && arg[0] != '"') {
            strncpy(initial, arg, sizeof(initial) - 1);
            initial[sizeof(initial) - 1] = 0;
        } else {
            restored = loadSession(session, 8);
            strcpy(initial, restored > 0 ? session[0] : "http://www.163.com");
        }
    }

    if (restored > 1) {
        int i;

        for (i = 0; i < restored; ++i)
            createTab(session[i]);

        setActiveTab(g_activeTab);
    } else {
        createTab(initial);
    }

    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.hwnd == g_address && msg.wParam == VK_RETURN) {
                char text[MAX_PATH_CHARS];

                GetWindowTextA(g_address, text, sizeof(text));
                navigateActive(text, 0);
                continue;
            }

            if (msg.hwnd == g_findEdit && msg.wParam == VK_RETURN) {
                doFind(1);
                continue;
            }

            if (GetKeyState(VK_CONTROL) & 0x8000) {
                if (msg.wParam == 'D') { addBookmarkCurrent(); continue; }
                if (msg.wParam == 'F') { toggleFind(); continue; }
                if (msg.wParam == 'T') { navigateActive("about:blank", 1); continue; }
                if (msg.wParam == 'L') { SetFocus(g_address); continue; }
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
        }

        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}
