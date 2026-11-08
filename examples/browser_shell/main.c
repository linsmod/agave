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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* libpng, from the vendored copy the engine already builds (dist/): the probe
 * writes PNG directly instead of leaving a BMP for the caller to convert. */
#include <png.h>

#include "agave_webview.h"
#include "../../src/webview/AgbLog.h"

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

/* $AGB_USERDATA (if set) is the single profile root: bookmarks/session here and
 * cookies/localStorage/module cache in the engine. The headless probe points it
 * at its own directory so test runs never touch the user's profile. */
static void createDirTree(char* path)
{
    char* p;
    char saved;

    if (!path || !path[0])
        return;

    for (p = path + 1; ; ++p) {
        saved = *p;

        if (saved == '\\' || saved == '/' || saved == 0) {
            *p = 0;
            CreateDirectoryA(path, NULL);
            *p = saved;

            if (!saved)
                break;
        }
    }
}

static void dataDirInit(void)
{
    const char* userdata = getenv("AGB_USERDATA");

    if (userdata && userdata[0]) {
        _snprintf(g_dataDir, sizeof(g_dataDir), "%s", userdata);
        g_dataDir[sizeof(g_dataDir) - 1] = 0;
        createDirTree(g_dataDir);
        return;
    }

    {
        const char* appdata = getenv("APPDATA");

        if (appdata && appdata[0]) {
            _snprintf(g_dataDir, sizeof(g_dataDir), "%s\\AgaveShell", appdata);
            g_dataDir[sizeof(g_dataDir) - 1] = 0;

            if (CreateDirectoryA(g_dataDir, NULL) ||
                GetFileAttributesA(g_dataDir) != INVALID_FILE_ATTRIBUTES)
                return;
        }
    }

    /* Fallback: %LOCALAPPDATA%\AgaveShellData. Never default next to the
     * executable: in a dev tree that is build-zig-native\, which a -Fresh
     * build deletes (history/bookmarks/session would vanish with it). */
    {
        const char* local = getenv("LOCALAPPDATA");

        if (local && local[0]) {
            _snprintf(g_dataDir, sizeof(g_dataDir), "%s\\AgaveShellData", local);
            g_dataDir[sizeof(g_dataDir) - 1] = 0;

            if (CreateDirectoryA(g_dataDir, NULL) ||
                GetFileAttributesA(g_dataDir) != INVALID_FILE_ATTRIBUTES)
                return;
        }
    }

    /* Last resort: next to the executable (portable USB-ish setups). */
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

/* Headless runs must not share the GUI profile: cookies, localStorage, the
 * module/HTTP caches and the shell state all live under $AGB_USERDATA, so the
 * probe gets its own root. AGB_USERDATA (env or --userdata) always wins, which
 * also lets parallel probes use disjoint directories. */
static void probeUserDataInit(void)
{
    char dir[MAX_PATH_CHARS];
    char env[MAX_PATH_CHARS + 32];
    const char* existing = getenv("AGB_USERDATA");

    dir[0] = 0;

    if (existing && existing[0]) {
        _snprintf(dir, sizeof(dir), "%s", existing);
    } else {
        const char* base = getenv("LOCALAPPDATA");

        if (!base || !base[0])
            base = getenv("TEMP");

        if (base && base[0])
            _snprintf(dir, sizeof(dir), "%s\\Agave\\probe", base);
    }

    if (dir[0]) {
        createDirTree(dir);
    }

    if (dir[0]) {
        _snprintf(env, sizeof(env), "AGB_USERDATA=%s", dir);
        putenv(env);
    }

    /* Shell state (bookmarks/session) follows the same root; nothing writes it
     * in probe mode, but a stray path must not land in the user profile. */
    if (dir[0]) {
        _snprintf(g_dataDir, sizeof(g_dataDir), "%s", dir);
        g_dataDir[sizeof(g_dataDir) - 1] = 0;
    }

    agbLog(AGB_LOG_PROBE, "userdata=%s", dir[0] ? dir : "(default: not isolated!)");
    fflush(stderr);
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
    agbLog(AGB_LOG_SHELL, "bookmarks loaded: %d entr%s from %s",
           g_bookmarkCount, g_bookmarkCount == 1 ? "y" : "ies", path);
}

/* Read the history back. It used to be append-only: recordHistory() wrote the
 * file but nothing ever loaded it, so the History menu only ever showed the
 * current session and every restart looked like "history lost". */
static void loadHistory(void)
{
    char path[MAX_PATH_CHARS];
    char line[1400];
    FILE* file;

    g_historyCount = 0;
    statePath(path, sizeof(path), "history.txt");
    file = fopen(path, "rb");

    if (!file)
        return;

    while (g_historyCount < AGW_MAX_HISTORY && fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (len == 0)
            continue;
        if (g_historyCount > 0 && !strcmp(g_history[g_historyCount - 1], line))
            continue;

        strncpy(g_history[g_historyCount], line, MAX_PATH_CHARS - 1);
        g_history[g_historyCount][MAX_PATH_CHARS - 1] = 0;
        ++g_historyCount;
    }

    fclose(file);
    agbLog(AGB_LOG_SHELL, "history loaded: %d entr%s from %s",
           g_historyCount, g_historyCount == 1 ? "y" : "ies", path);
}
/* Profile-backed shell state. Both the GUI and the probe need it: the probe
 * reads history/bookmarks too (a probe run could otherwise not verify that the
 * files are loaded), and it must not re-implement the profile layout. */
static void shellStateInit(void)
{
    dataDirInit();
    loadBookmarks();
    loadHistory();
}

static void saveBookmarks(void)
{
    char path[MAX_PATH_CHARS];
    FILE* file;
    int i;

    statePath(path, sizeof(path), "bookmarks.txt");
    file = fopen(path, "wb");

    if (!file) {
        agbLog(AGB_LOG_SHELL, "bookmarks: cannot write %s", path);
        return;
    }

    for (i = 0; i < g_bookmarkCount; ++i)
        fprintf(file, "%s\t%s\n", g_bookmarks[i].url,
                g_bookmarks[i].title[0] ? g_bookmarks[i].title : g_bookmarks[i].url);

    fclose(file);
}

/* 1 when the bookmark was added, 0 when nothing changed (empty URL, duplicate,
 * table full). Callers have to tell the difference: the shell used to print
 * "Bookmark added" even for a URL that was already saved. */
static int addBookmark(const char* url, const char* title)
{
    int i;

    if (!url || !url[0] || g_bookmarkCount >= AGW_MAX_BOOKMARKS)
        return 0;

    for (i = 0; i < g_bookmarkCount; ++i) {
        if (!strcmp(g_bookmarks[i].url, url))
            return 0;
    }

    strncpy(g_bookmarks[g_bookmarkCount].url, url, MAX_PATH_CHARS - 1);
    g_bookmarks[g_bookmarkCount].url[MAX_PATH_CHARS - 1] = 0;
    strncpy(g_bookmarks[g_bookmarkCount].title, title ? title : "", sizeof(g_bookmarks[0].title) - 1);
    g_bookmarks[g_bookmarkCount].title[sizeof(g_bookmarks[0].title) - 1] = 0;
    ++g_bookmarkCount;
    saveBookmarks();
    return 1;
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
    } else {
        /* Silent before: "history keeps disappearing" started here. The errno
         * number (and its text) is the point: the path alone cannot tell a
         * locked/read-only profile from a full disk or a vanished directory. */
        int savedErrno = errno;

        agbLog(AGB_LOG_SHELL, "history: cannot append to %s (errno=%d: %s)",
               path, savedErrno, strerror(savedErrno));
    }
}

static void saveSession(void)
{
    char path[MAX_PATH_CHARS];
    FILE* file;
    int i;
    int saved = 0;

    statePath(path, sizeof(path), "session.txt");
    file = fopen(path, "wb");

    if (!file) {
        agbLog(AGB_LOG_SHELL, "session: cannot write %s", path);
        return;
    }

    for (i = 0; i < MAX_TABS; ++i) {
        if (g_tabs[i].used && g_tabs[i].url[0]) {
            fprintf(file, "%s\n", g_tabs[i].url);
            ++saved;
        }
    }

    fclose(file);
    agbLog(AGB_LOG_SHELL, "session saved tabs=%d -> %s", saved, path);
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

/* Move a child only when it actually has to move. MoveWindow(..., TRUE) repaints
 * the control every time, and the toolbar is laid out on every WM_SIZE - a load
 * that makes the scrollbar appear/disappear fires those repeatedly, which is why
 * the "<" / ">" captions appeared to flicker during reloads. */
static void placeChild(HWND child, int x, int y, int w, int h)
{
    RECT r;
    POINT origin = { 0, 0 };

    if (!child)
        return;

    if (!GetWindowRect(child, &r))
        return;

    MapWindowPoints(NULL, g_hwnd, (POINT*)&r, 2);
    origin.x = r.left;
    origin.y = r.top;

    if (origin.x == x && origin.y == y &&
        (r.right - r.left) == w && (r.bottom - r.top) == h)
        return;

    MoveWindow(child, x, y, w, h, TRUE);
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

    placeChild(g_back, x, y, BUTTON_W, 26);             x += BUTTON_W + 2;
    placeChild(g_forward, x, y, BUTTON_W, 26);          x += BUTTON_W + 2;
    placeChild(g_reload, x, y, BUTTON_W, 26);           x += BUTTON_W + 2;
    placeChild(g_stop, x, y, BUTTON_W, 26);             x += BUTTON_W + 6;

    reserved = (BUTTON_W + 6) * 3 + 20;
    placeChild(g_address, x, y + 1, width - x - reserved, 24);
    x = width - reserved + 6;
    placeChild(g_go, x, y, 46, 26);                     x += 46 + 4;
    placeChild(g_newtab, x, y, BUTTON_W, 26);           x += BUTTON_W + 4;
    placeChild(g_menu, x, y, BUTTON_W, 26);

    if (g_findVisible) {
        int fy = TAB_HEIGHT + TOOLBAR_H + 2;

        placeChild(g_findEdit, 8, fy, 260, 24);
        placeChild(g_findPrev, 274, fy, 34, 24);
        placeChild(g_findNext, 312, fy, 34, 24);
        placeChild(g_findClose, 350, fy, 28, 24);
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

    if (index != g_activeTab || !g_tabs[index].hwnd)
        agbLog(AGB_LOG_SHELL, "tab activate index=%d (was %d) url=%s",
               index, g_activeTab, g_tabs[index].url);

    g_activeTab = index;

    for (i = 0; i < MAX_TABS; ++i) {
        if (g_tabs[i].used && g_tabs[i].hwnd) {
            ShowWindow(g_tabs[i].hwnd, i == index ? SW_SHOW : SW_HIDE);

            /* Page Visibility: background tabs keep loading, but their
             * document.hidden is true and pages pause timers/media on it. */
            if (g_tabs[i].view)
                agave_webview_set_visible(g_tabs[i].view, i == index);
        }
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
    int wasActive;
    char url[MAX_PATH_CHARS];

    if (index < 0 || index >= MAX_TABS || !g_tabs[index].used)
        return;

    wasActive = (index == g_activeTab);
    strncpy(url, g_tabs[index].url, sizeof(url) - 1);
    url[sizeof(url) - 1] = 0;

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

    agbLog(AGB_LOG_SHELL, "tab close index=%d url=%s active=%d closed-saved=%d",
           index, url, wasActive, g_closedCount);

    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* ctrl+shift+T: reopen the tab that was closed last. */
static void reopenClosedTab(void)
{
    ClosedTab closed;

    if (g_closedCount <= 0) {
        agbLog(AGB_LOG_SHELL, "tab reopen: nothing was closed");
        return;
    }

    closed = g_closed[--g_closedCount];
    agbLog(AGB_LOG_SHELL, "tab reopen url=%s left=%d", closed.url, g_closedCount);
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

    if (count < 2) {
        agbLog(AGB_LOG_SHELL, "tab switch: only %d tab(s) open", count);
        return;
    }

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
    static const char* const names[] = { "alert", "confirm", "prompt" };
    int answer = 1;

    (void)view;
    (void)user;

    /* A modal box on the engine thread looks exactly like a hang in the log, so
     * say which page called what before opening it. */
    agbLog(AGB_LOG_SHELL, "%s msg='%s'", type >= 0 && type <= 2 ? names[type] : "dialog",
           message ? message : "");

    if (type == 0) {
        MessageBoxA(g_hwnd, message ? message : "", "Page says", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    if (type == 1) {
        answer = MessageBoxA(g_hwnd, message ? message : "", "Confirm",
                             MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
        agbLog(AGB_LOG_SHELL, "confirm answered %s", answer ? "ok" : "cancel");
        return answer;
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
        agbLog(AGB_LOG_SHELL, "new window rejected (index=%d url=%s)", index,
               url ? url : "");
        agave_webview_destroy(popup);
        return 0;
    }

    agbLog(AGB_LOG_SHELL, "new window url=%s -> tab %d", url, index);

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

/* Downloads are engine jobs; the control mirrors their state into the UI
 * thread. The shell only renders it - the file is already on disk. */
static void onDownload(AgaveWebView* view, const MC_DOWNLOAD_INFO* info, void* user)
{
    Tab* tab = (Tab*)user;
    const char* name = info && info->utf8_path ? info->utf8_path : "";
    const char* slash = strrchr(name, '\\');
    char text[512];

    (void)view;

    if (!tab || !info)
        return;

    if (slash && slash[1])
        name = slash + 1;

    switch (info->state) {
    case MC_DL_STARTED:
    case MC_DL_PROGRESS: {
        char size[64];

        if (info->total)
            _snprintf(size, sizeof(size), "%d%% (%.1f/%.1f MB)",
                      (int)(info->received * 100 / info->total),
                      info->received / 1048576.0, info->total / 1048576.0);
        else
            _snprintf(size, sizeof(size), "%.1f MB", info->received / 1048576.0);
        size[sizeof(size) - 1] = 0;

        _snprintf(text, sizeof(text), "Downloading %s %s", name, size);
        break;
    }
    case MC_DL_FINISHED:
        _snprintf(text, sizeof(text), "Downloaded: %s", info->utf8_path ? info->utf8_path : "");
        break;
    case MC_DL_FAILED:
        _snprintf(text, sizeof(text), "Download failed (%d): %s", info->error,
                  info->utf8_url ? info->utf8_url : "");
        break;
    default:
        _snprintf(text, sizeof(text), "Download cancelled: %s", name);
        break;
    }

    text[sizeof(text) - 1] = 0;
    strncpy(tab->status, text, sizeof(tab->status) - 1);
    tab->status[sizeof(tab->status) - 1] = 0;

    /* A download navigation commits no document, so on_progress(finished) never
     * arrives and the tab would stay "loading" forever. */
    if (info->state != MC_DL_PROGRESS) {
        tab->loading = 0;
        tab->progress = 100;
    }

    if (tab == &g_tabs[g_activeTab])
        updateStatus();
}

/* The watchdog stepped in (see AgaveWebView.c): a page script was blocking the
 * page and got aborted, or the engine stopped draining while no script was
 * running (nothing was killed there - aborting cannot help). Runs on the UI
 * thread, so a notice is allowed to be a plain dialog; AGB_HANG_POPUP=0 keeps
 * unattended runs quiet. */
static void onUnresponsive(AgaveWebView* view, const char* reason,
                           unsigned int busy_ms, void* user)
{
    Tab* tab = (Tab*)user;
    const char* show = getenv("AGB_HANG_POPUP");
    char message[700];

    (void)view;

    if (tab) {
        strncpy(tab->status, reason ? reason : "", sizeof(tab->status) - 1);
        tab->status[sizeof(tab->status) - 1] = 0;

        if (tab == &g_tabs[g_activeTab])
            updateStatus();
    }

    agbLog(AGB_LOG_SHELL, "page unresponsive: %s (busy=%ums)", reason ? reason : "",
           busy_ms);

    if (show && show[0] == '0' && show[1] == 0)
        return;

    _snprintf(message, sizeof(message),
              "%s\n\n(%.1f s)\n\nAGB_HANG_POPUP=0 turns this notice off.",
              reason ? reason : "", busy_ms / 1000.0);
    message[sizeof(message) - 1] = 0;
    MessageBoxA(g_hwnd, message, "agave - page unresponsive",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

/* Right click: the control reports the hit-test; the shell owns the menu. */
static void navigateActive(const char* text, int newTab, const char* source);
static void addBookmarkCurrent(void);
static void reloadActive(const char* source);
static void backActive(const char* source);
static void forwardActive(const char* source);
static void stopActive(const char* source);

/* ------------------------------------------------------------------------- */
/* script injection (inspector.js / vConsole)                                */
/* ------------------------------------------------------------------------- */

/* Menu labels are UTF-8 (the shell speaks UTF-8 everywhere); the menu itself
 * has to be filled through the wide API or a Chinese label turns into
 * mojibake under the ANSI code page. */
static void appendMenuUtf8(HMENU menu, UINT flags, UINT_PTR id, const char* text)
{
    wchar_t wide[256];

    utf8ToWide(text, wide, 256);
    AppendMenuW(menu, flags, id, wide);
}

static char* readFileUtf8(const wchar_t* path, size_t* lengthOut)
{
    FILE* file;
    long length;
    char* buffer;

    if (lengthOut)
        *lengthOut = 0;

    file = _wfopen(path, L"rb");
    if (!file)
        return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    length = ftell(file);
    if (length < 0 || length > 16 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = (char*)malloc((size_t)length + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }

    if (length > 0 && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[length] = 0;
    fclose(file);

    if (lengthOut)
        *lengthOut = (size_t)length;

    return buffer;
}

/* Runs a JS file in the page. The file is looked up next to agave_shell.exe
 * first (the build copies inspector.js / vconsole.min.js there), then in the
 * current directory; $AGB_INSPECTOR overrides the inspector.js path. Keeping
 * the injection as a file on disk means it can be edited and re-injected
 * without relinking, the same deal as prelude.js/polyfills.js.
 *
 * `suffix` (optional) is appended after a newline and evaluated in the same
 * script: a UMD bundle needs its bootstrap to run only after the bundle has
 * registered itself, and one eval keeps that ordering independent of the
 * command queue. */
static int injectScriptAsset(AgaveWebView* view, const char* fileName,
                             const char* envName, const char* suffix)
{
    wchar_t path[MAX_PATH_CHARS];
    char* script = NULL;
    size_t length = 0;
    size_t fileLength = 0;
    Tab* tab = &g_tabs[g_activeTab];

    if (!view)
        return 0;

    path[0] = 0;

    if (envName) {
        const char* override = getenv(envName);

        if (override && override[0])
            utf8ToWide(override, path, MAX_PATH_CHARS);
    }

    if (!path[0]) {
        wchar_t module[MAX_PATH_CHARS];
        wchar_t name[MAX_PATH_CHARS];
        wchar_t* slash;

        if (!GetModuleFileNameW(NULL, module, MAX_PATH_CHARS))
            return 0;

        slash = wcsrchr(module, L'\\');
        if (slash)
            *(slash + 1) = 0;

        utf8ToWide(fileName, name, MAX_PATH_CHARS);

        wcsncpy(path, module, MAX_PATH_CHARS - 1);
        path[MAX_PATH_CHARS - 1] = 0;
        if (wcslen(path) + wcslen(name) < MAX_PATH_CHARS - 1)
            wcscat(path, name);
    }

    script = readFileUtf8(path, &length);

    if (!script) {
        /* Also accept a path relative to the working directory, which is how
         * the shell is usually launched while editing the repo. */
        utf8ToWide(fileName, path, MAX_PATH_CHARS);
        script = readFileUtf8(path, &length);
    }

    if (!script) {
        if (tab->used) {
            _snprintf(tab->status, sizeof(tab->status),
                      "Cannot read %s (put it next to agave_shell.exe)", fileName);
            tab->status[sizeof(tab->status) - 1] = 0;
            updateStatus();
        }
        agbLog(AGB_LOG_SHELL, "inject: cannot read %s", fileName);
        return 0;
    }

    fileLength = length;

    if (suffix && suffix[0]) {
        size_t extra = strlen(suffix);
        char* grown = (char*)realloc(script, length + extra + 3);

        if (grown) {
            script = grown;
            /* Start a fresh statement: the asset may end without a semicolon,
             * and a suffix beginning with "(" would otherwise chain onto it. */
            script[length] = '\n';
            script[length + 1] = ';';
            memcpy(script + length + 2, suffix, extra);
            script[length + 2 + extra] = 0;
            length += extra + 2;
        }
    }

    agave_webview_eval(view, script);
    agave_webview_set_focus(view);
    free(script);

    agbLog(AGB_LOG_SHELL, "inject: %s (%lu bytes, eval %lu)",
           fileName, (unsigned long)fileLength, (unsigned long)length);

    if (tab->used && tab->view == view) {
        _snprintf(tab->status, sizeof(tab->status),
                  "Injected %s (%lu bytes)", fileName, (unsigned long)fileLength);
        tab->status[sizeof(tab->status) - 1] = 0;
        updateStatus();
    }

    return 1;
}

static int injectScriptFile(AgaveWebView* view, const char* fileName, const char* envName)
{
    return injectScriptAsset(view, fileName, envName, NULL);
}

/* vConsole is the default payload for now; inspector.js is the built-in
 * hover inspector. Both are optional: the menu reports a missing file. */
static void injectVConsole(AgaveWebView* view)
{
    /* The page realm has an AMD `define` (prelude's loader), so the UMD bundle
     * registers itself as the module "VConsole" instead of setting
     * window.VConsole. The launcher is appended to the bundle and evaluated in
     * the same script, so registration has already happened when it runs;
     * instantiate once so clicking the menu twice does not stack panels. */
    injectScriptAsset(view, "vconsole.min.js", NULL,
        "(function () {"
        " var VC = window.VConsole ||"
        "   (typeof require === 'function' ? require('VConsole') : null);"
        " if (typeof VC !== 'function' || window.__agaveVConsole) return;"
        " try { window.__agaveVConsole = new VC(); }"
        " catch (e) {"
        "  if (window.console && console.error)"
        "   console.error('[vconsole] ' + (e && e.message));"
        " }"
        "})();");
}

static void injectInspector(AgaveWebView* view)
{
    injectScriptFile(view, "inspector.js", "AGB_INSPECTOR");
}

static void onContextMenu(AgaveWebView* view, int x, int y,
                          const char* linkUrl, const char* imageUrl,
                          const char* text, void* user)
{
    HMENU menu = CreatePopupMenu();
    POINT point;
    Tab* tab = (Tab*)user;
    HWND viewHwnd = view ? (HWND)agave_webview_hwnd(view) : NULL;
    int choice;

    (void)text;

    if (!tab) {
        agbLog(AGB_LOG_SHELL, "context menu without a tab, ignored");
        return;
    }

    /* x,y are client coordinates of the control, not of the shell window: with
     * g_hwnd the menu was offset by the toolbar height, and on a multi-monitor
     * setup the popup could open on the wrong monitor entirely. */
    point.x = x;
    point.y = y;
    ClientToScreen(viewHwnd ? viewHwnd : g_hwnd, &point);

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
    /* No view-source support yet: a document->string API does not exist, and the
     * old "View page source (eval)" evaluated outerHTML into the void (eval
     * results are discarded), so the item did nothing at all. Say so instead. */
    AppendMenuA(menu, MF_STRING | MF_GRAYED, 9, "View page source (not implemented)");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    appendMenuUtf8(menu, MF_STRING, 10, "注入 vConsole");
    appendMenuUtf8(menu, MF_STRING, 11, "注入 inspector.js");

    choice = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                 point.x, point.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);

    /* choice==0 means the menu was dismissed (or TrackPopupMenu failed): the
     * one line that tells "the menu never opened" apart from "the item did
     * nothing". */
    agbLog(AGB_LOG_SHELL, "context menu at %d,%d link=%s image=%s choice=%d",
           x, y,
           (linkUrl && linkUrl[0]) ? linkUrl : "(none)",
           (imageUrl && imageUrl[0]) ? imageUrl : "(none)",
           choice);

    switch (choice) {
    case 1: backActive("context menu"); break;
    case 2: forwardActive("context menu"); break;
    case 3: reloadActive("context menu"); break;
    case 4: if (linkUrl && linkUrl[0]) navigateActive(linkUrl, 0, "context menu"); break;
    case 5: if (linkUrl && linkUrl[0]) navigateActive(linkUrl, 1, "context menu"); break;
    case 6:
        if (linkUrl && linkUrl[0]) {
            wchar_t wide[2048];
            int copied = 0;
            int wideBytes;

            /* CF_UNICODETEXT, not CF_TEXT: a UTF-8 URL pasted into an ANSI
             * clipboard came out as mojibake. */
            utf8ToWide(linkUrl, wide, 2048);
            wideBytes = (lstrlenW(wide) + 1) * (int)sizeof(wchar_t);

            if (OpenClipboard(g_hwnd)) {
                HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wideBytes);

                if (memory) {
                    void* target = GlobalLock(memory);

                    if (target) {
                        memcpy(target, wide, (size_t)wideBytes);
                        GlobalUnlock(memory);
                        EmptyClipboard();
                        copied = SetClipboardData(CF_UNICODETEXT, memory) != NULL;
                        if (!copied)
                            GlobalFree(memory);
                    } else {
                        GlobalFree(memory);
                    }
                }
                CloseClipboard();
            }

            agbLog(AGB_LOG_SHELL, "clipboard copy link ok=%d url=%s", copied, linkUrl);
        }
        break;
    case 8: addBookmarkCurrent(); break;
    case 9: agbLog(AGB_LOG_SHELL, "view source: not implemented"); break;
    case 10: injectVConsole(view); break;
    case 11: injectInspector(view); break;
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

static int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes the %XX escapes of a URL path; invalid escapes stay literal. Needed
 * when a file: URL that is already encoded has to be rebuilt: treating it as a
 * raw filesystem path would encode its '%' again ("%20" -> "%2520") on every
 * pass, which is what made repeated Go clicks grow the address bar. */
static void decodeUrlPath(const char* text, char* out, size_t outSize)
{
    size_t o = 0;

    if (!outSize)
        return;

    if (!text) {
        out[0] = 0;
        return;
    }

    while (*text && o + 1 < outSize) {
        if (text[0] == '%' && text[1] && text[2]) {
            int hi = hexValue(text[1]);
            int lo = hexValue(text[2]);

            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                text += 3;
                continue;
            }
        }

        out[o++] = *text++;
    }

    out[o] = 0;
}

/* Splits "path?query#fragment" into the on-disk path and the URL suffix.
 * The suffix is appended to the file: URL verbatim - a query or fragment is
 * not part of the file name, and percent-encoding its '?' would make the path
 * point at a file called "x.html%3Ffocus=b" (which never exists). */
static void splitLocalSuffix(const char* text, char* pathOut, size_t pathSize,
                             const char** suffixOut)
{
    const char* mark = text ? strpbrk(text, "?#") : NULL;
    size_t len;

    if (!pathOut || !pathSize)
        return;

    if (!mark) {
        strncpy(pathOut, text ? text : "", pathSize - 1);
        pathOut[pathSize - 1] = 0;
        *suffixOut = "";
        return;
    }

    len = (size_t)(mark - text);
    if (len >= pathSize)
        len = pathSize - 1;
    memcpy(pathOut, text, len);
    pathOut[len] = 0;
    *suffixOut = mark;
}

/* Appends a query/fragment verbatim (no percent-encoding). */
static void appendLocalSuffix(char* out, size_t outSize, const char* suffix)
{
    size_t o = strlen(out);

    if (!suffix || !*suffix)
        return;

    while (*suffix && o + 1 < outSize)
        out[o++] = *suffix++;
    out[o] = 0;
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

    /* "C:/dir/x" -> file:///C:/dir/x. A drive path has no leading slash, so the
     * URL has to spell all three of them: with two ("file://C:/x") the third
     * rebuild below classified it as "file: + drive path" again and re-encoded
     * the path - the "%20" -> "%2520" growth on repeated Go clicks. */
    appendUrlEncoded(out, outSize, &written, "file:///");
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
            char localPath[MAX_PATH_CHARS];
            const char* suffix;

            splitLocalSuffix(rest, localPath, sizeof(localPath), &suffix);
            fileUrlFromLocalPath(localPath, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
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
         * rebuild the canonical file:/// form (the engine needs it). A
         * ?query / #fragment rides along verbatim. The path may already be
         * percent-encoded (it came from a URL, not from a typed path): decode
         * first, or every rebuild would encode the '%' again. */
        if (strchr(p, '\\') || (slashes < 3 && (isDrivePath(q) || isUncPath(q)))) {
            char localPath[MAX_PATH_CHARS];
            char decoded[MAX_PATH_CHARS];
            const char* suffix;
            const char* candidate = (strchr(p, '\\') && slashes == 0) ? p : q;

            splitLocalSuffix(candidate, localPath, sizeof(localPath), &suffix);
            if (strchr(localPath, '\\')) {
                /* A typed Windows path: its '%' is a literal file-name byte,
                 * not an escape. Only URL paths get decoded. */
                strncpy(decoded, localPath, sizeof(decoded) - 1);
                decoded[sizeof(decoded) - 1] = 0;
            } else {
                decodeUrlPath(localPath, decoded, sizeof(decoded));
            }
            fileUrlFromLocalPath(decoded, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
            return;
        }

        if (slashes < 2 && q[0]) {
            /* file:C:/x */
            char localPath[MAX_PATH_CHARS];
            char decoded[MAX_PATH_CHARS];
            const char* suffix;

            splitLocalSuffix(q, localPath, sizeof(localPath), &suffix);
            decodeUrlPath(localPath, decoded, sizeof(decoded));
            fileUrlFromLocalPath(decoded, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
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

    {
        /* A local path may carry a ?query / #fragment ("page.html?focus=b").
         * The suffix is not part of the file name: test and canonicalize the
         * path alone, then append the suffix verbatim. */
        char localPath[MAX_PATH_CHARS];
        const char* suffix;

        splitLocalSuffix(text, localPath, sizeof(localPath), &suffix);

        if (isDrivePath(localPath) || isUncPath(localPath)) {
            fileUrlFromLocalPath(localPath, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
            return;
        }

        /* Relative path that resolves to something on disk ("./x.html",
         * "debug/a.html"), optionally with a query. */
        if ((strchr(localPath, '\\') || strchr(localPath, '/')) && localPathExists(localPath)) {
            fileUrlFromLocalPath(localPath, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
            return;
        }
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

    if (index < 0) {
        agbLog(AGB_LOG_SHELL, "tab create failed: no free slot (max %d)", MAX_TABS);
        return;
    }

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
    callbacks.on_unresponsive = onUnresponsive;

    tab->used = 1;
    tab->view = agave_webview_create(g_hwnd, 0, TAB_HEIGHT + TOOLBAR_H,
                                     800, 400, &callbacks, tab);

    if (!tab->view) {
        agbLog(AGB_LOG_SHELL, "tab create failed: view=null url=%s", url ? url : "");
        tab->used = 0;
        return;
    }

    tab->hwnd = (HWND)agave_webview_hwnd(tab->view);
    strncpy(tab->url, url, MAX_PATH_CHARS - 1);
    ShowWindow(tab->hwnd, SW_HIDE);

    agbLog(AGB_LOG_SHELL, "tab create index=%d url=%s", index, tab->url);

    /* The GUI saves where a user expects to find downloads; the probe keeps the
     * engine default (<AGB_USERDATA>/downloads) so tests never touch this. */
    {
        const char* home = getenv("USERPROFILE");

        if (home && home[0]) {
            char downloads[MAX_PATH_CHARS];
            static int logged;

            _snprintf(downloads, sizeof(downloads), "%s\\Downloads", home);
            downloads[sizeof(downloads) - 1] = 0;
            agave_webview_set_download_dir(tab->view, downloads);

            /* Once per process: the directory is per view, the message is not. */
            if (!logged) {
                logged = 1;
                agbLog(AGB_LOG_SHELL, "download dir=%s", downloads);
            }
        }
    }

    agave_webview_set_zoom(tab->view, g_zoom);
    setActiveTab(index);
    agave_webview_navigate(tab->view, url);
}

static void navigateActive(const char* text, int newTab, const char* source)
{
    char url[MAX_PATH_CHARS];

    normalizeUrl(text, url, sizeof(url));

    /* The one line that tells which entry point started the navigation: the
     * engine only logs [nav] open url, which every path shares. Without this
     * an address-bar Enter, a Go click and a history-menu pick look identical. */
    agbLog(AGB_LOG_SHELL, "navigate (%s) url=%s newtab=%d",
           source ? source : "?", url, newTab);

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

/* Reload / back / forward / stop are each reachable from five places (toolbar,
 * F5 or ctrl+r, alt+arrow, main menu, context menu). The engine line they
 * produce ([nav] reload) only says *what* happened, never *what the user did* -
 * and "which refresh" is the first question in every white-page report. */
static void reloadActive(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view) {
        agbLog(AGB_LOG_SHELL, "reload (%s) ignored: no tab", source);
        return;
    }

    agbLog(AGB_LOG_SHELL, "reload (%s) url=%s", source, tab->url);
    agave_webview_reload(tab->view);
}

static void backActive(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view)
        return;

    agbLog(AGB_LOG_SHELL, "back (%s) can=%d url=%s", source,
           agave_webview_can_back(tab->view) ? 1 : 0, tab->url);
    agave_webview_back(tab->view);
}

static void forwardActive(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view)
        return;

    agbLog(AGB_LOG_SHELL, "forward (%s) can=%d url=%s", source,
           agave_webview_can_forward(tab->view) ? 1 : 0, tab->url);
    agave_webview_forward(tab->view);
}

static void stopActive(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view)
        return;

    agbLog(AGB_LOG_SHELL, "stop (%s) url=%s", source, tab->url);
    agave_webview_stop(tab->view);
}

static void addBookmarkCurrent(void)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used)
        return;

    /* The status line has to match what really happened: a duplicate URL is a
     * no-op, and claiming "Bookmark added" made the menu look broken. */
    if (addBookmark(tab->url, tab->title)) {
        agbLog(AGB_LOG_SHELL, "bookmark add url=%s total=%d", tab->url, g_bookmarkCount);
        strncpy(tab->status, "Bookmark added", sizeof(tab->status) - 1);
    } else {
        agbLog(AGB_LOG_SHELL, "bookmark duplicate url=%s", tab->url);
        strncpy(tab->status, "Already bookmarked", sizeof(tab->status) - 1);
    }

    updateStatus();
}

static void toggleFind(void)
{
    g_findVisible = !g_findVisible;

    agbLog(AGB_LOG_SHELL, "find bar %s", g_findVisible ? "on" : "off");

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

    if (!text[0]) {
        agbLog(AGB_LOG_SHELL, "find: empty query, ignored");
        return;
    }

    /* The engine gives no "hits" back, so this line is the only evidence that
     * the search reached the page at all. */
    agbLog(AGB_LOG_SHELL, "find text='%s' forward=%d", text, forward);
    agave_webview_search(tab->view, text, forward, 0);
}

/* One place that clamps, applies and logs the zoom: it used to be four copies of
 * the same three lines, invisible in the log even though zoom changes vw/vh and
 * every layout width bug report depends on it. */
static void setZoom(int percent, const char* source)
{
    int before = g_zoom;

    if (percent < 25)
        percent = 25;
    if (percent > 400)
        percent = 400;

    g_zoom = percent;
    applyZoomToActive();
    updateStatus();

    if (before != g_zoom)
        agbLog(AGB_LOG_SHELL, "zoom %d%% -> %d%% (%s)", before, g_zoom, source);
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
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    appendMenuUtf8(menu, MF_STRING, 8, "注入 vConsole");
    appendMenuUtf8(menu, MF_STRING, 9, "注入 inspector.js");

    GetCursorPos(&point);
    choice = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                 point.x, point.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);

    if (choice >= 200) {
        int index = g_historyCount - 1 - (choice - 200);

        if (index >= 0 && index < g_historyCount)
            navigateActive(g_history[index], 0, "history menu");
        return;
    }

    if (choice >= 100) {
        int index = choice - 100;

        if (index >= 0 && index < g_bookmarkCount)
            navigateActive(g_bookmarks[index].url, 0, "bookmark menu");
        return;
    }

    switch (choice) {
    case 1: addBookmarkCurrent(); break;
    case 2: setZoom(g_zoom + 25, "menu zoom in"); break;
    case 3: setZoom(g_zoom - 25, "menu zoom out"); break;
    case 4: setZoom(100, "menu reset zoom"); break;
    case 5: navigateActive("about:blank", 1, "menu"); break;
    case 6: toggleFind(); break;
    case 7: reloadActive("menu"); break;
    case 8: injectVConsole(g_tabs[g_activeTab].view); break;
    case 9: injectInspector(g_tabs[g_activeTab].view); break;
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

    agbLog(AGB_LOG_SHELL, "window.close() -> close tab");

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

        switch (id) {
        case ID_BACK:    backActive("button"); break;
        case ID_FORWARD: forwardActive("button"); break;
        case ID_RELOAD:  reloadActive("button"); break;
        case ID_STOP:    stopActive("button"); break;

        case ID_GO: {
            char text[MAX_PATH_CHARS];

            readControlText(g_address, text, sizeof(text));
            navigateActive(text, 0, "button");
            break;
        }

        case ID_NEWTAB: navigateActive("about:blank", 1, "button"); break;
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
/* Set when --probe was given: headless runs must not pop dialogs (their stderr
 * already carries the same information, and a modal box would hang CI). */
static int g_probeMode;
/* --interrupt-after <ms>: abort the page script once, mid-load (hang recovery). */
static int g_probeInterruptAfterMs;
/* --nav-after <ms>: navigate while the page is still busy (choke-point test). */
static int g_probeNavAfterMs;
static char g_probeNavUrl[MAX_PATH_CHARS];
/* --hide-after/--show-after <ms>: Page Visibility changes without a GUI tab
 * switch (the control's set_visible path). */
static int g_probeHideAfterMs;
static int g_probeShowAfterMs;
/* --reload-every <ms> --reload-count <n>: hammer Reload regardless of load state. */
static int g_probeReloadEveryMs;
static int g_probeReloadCount;
/* GUI runs only: tell the user where the crash dump landed. The file name is
 * relative to the process CWD, so show the absolute path - the shell may have
 * been started from anywhere. */
static void shellAnnounceCrashFile(const char* file)
{
    const char* opt = getenv("AGB_CRASH_POPUP");
    static volatile LONG announced;
    char full[MAX_PATH];
    char msg[640];
    DWORD n;

    if (g_probeMode) return;
    if (InterlockedExchange(&announced, 1) != 0) return;
    if (opt && opt[0] && strcmp(opt, "0") == 0) return;

    n = GetFullPathNameA(file, (DWORD)sizeof(full), full, NULL);
    if (n == 0 || n >= (DWORD)sizeof(full))
        lstrcpynA(full, file, (int)sizeof(full));

    snprintf(msg, sizeof(msg),
             "agave crashed and wrote the crash report here:\n\n%s\n\n"
             "Frames are module+offset; symbolize with:\n"
             "  pwsh -File tools\\cdb_stack.ps1 -Symbolize \"%s\"\n"
             "Set AGB_CRASH_POPUP=0 to disable this dialog.",
             full, full);

    MessageBoxA(NULL, msg, "agave crash", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}
static void shellPrintFrames(int skip, int depth)
{
    void* frames[64];
    unsigned short count = CaptureStackBackTrace((DWORD)skip, (DWORD)depth, frames, NULL);
    int i;

    for (i = 0; i < (int)count; ++i) {
        unsigned long long a = (unsigned long long)frames[i];
        const char* mod = "?";
        unsigned long long off = a;
        unsigned long long exe = (unsigned long long)GetModuleHandleA(NULL);
        unsigned long long av  = (unsigned long long)GetModuleHandleA("libagave.dll");
        unsigned long long ab  = (unsigned long long)GetModuleHandleA("antjsb.dll");

        if (av && a >= av && a < av + 0x2000000ULL) { mod = "libagave"; off = a - av; }
        else if (ab && a >= ab && a < ab + 0x2000000ULL) { mod = "antjsb"; off = a - ab; }
        else if (exe && a >= exe && a < exe + 0x100000ULL) { mod = "exe"; off = a - exe; }

        fprintf(stderr, "  #%02d %s+0x%llx\n", i, mod, off);
    }
}

/* The stderr stream may already be unusable when a crash happens on a worker
 * thread, so keep a file copy next to the working directory. */
static void shellWriteCrashFile(EXCEPTION_RECORD* rec)
{
    FILE* f = fopen("agb_crash.txt", "w");

    if (!f)
        return;

    fprintf(f, "thread=%lu code=0x%08lx addr=0x%llx",
            (unsigned long)GetCurrentThreadId(),
            (unsigned long)(rec ? rec->ExceptionCode : 0),
            (unsigned long long)(rec ? (unsigned long long)rec->ExceptionAddress : 0));

    if (rec && rec->NumberParameters >= 2)
        fprintf(f, " info0=%llu info1=0x%llx",
                (unsigned long long)rec->ExceptionInformation[0],
                (unsigned long long)rec->ExceptionInformation[1]);

    fprintf(f, "\n");

    {
        void* frames[64];
        unsigned short count = CaptureStackBackTrace(1, 48, frames, NULL);
        int i;

        for (i = 0; i < (int)count; ++i) {
            unsigned long long a = (unsigned long long)frames[i];
            unsigned long long exe = (unsigned long long)GetModuleHandleA(NULL);
            unsigned long long av  = (unsigned long long)GetModuleHandleA("libagave.dll");
            unsigned long long ab  = (unsigned long long)GetModuleHandleA("antjsb.dll");
            const char* mod = "?";
            unsigned long long off = a;

            if (av && a >= av && a < av + 0x2000000ULL) { mod = "libagave"; off = a - av; }
            else if (ab && a >= ab && a < ab + 0x2000000ULL) { mod = "antjsb"; off = a - ab; }
            else if (exe && a >= exe && a < exe + 0x100000ULL) { mod = "exe"; off = a - exe; }

            fprintf(f, "  #%02d %s+0x%llx\n", i, mod, off);
        }
    }

    fflush(f);
    fclose(f);
}

/* First-chance dump: the unhandled-exception filter can be bypassed - ant
 * installs its own handler and only chains to the previous filter once per
 * process, and its own StackWalk can fault - so an AV on a worker thread can
 * kill the process without any output. A vectored handler runs first, on every
 * thread, before SEH/filters, and writes the faulting stack to a file with raw
 * Win32 I/O (the CRT may be mid-crash). It never handles the exception. */
static LONG WINAPI shellVehHandler(EXCEPTION_POINTERS* ep)
{
    static volatile LONG dumped;
    EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : NULL;
    DWORD code = rec ? rec->ExceptionCode : 0;
    char buf[8192];
    int n = 0;
    int i;

    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedExchange(&dumped, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "[crash-firstchance] thread=%lu code=0x%08lx addr=0x%llx",
                  (unsigned long)GetCurrentThreadId(), (unsigned long)code,
                  (unsigned long long)(rec ? (unsigned long long)rec->ExceptionAddress : 0));

    if (rec && rec->NumberParameters >= 2)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " info0=%llu info1=0x%llx",
                      (unsigned long long)rec->ExceptionInformation[0],
                      (unsigned long long)rec->ExceptionInformation[1]);

    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\n");

    {
        void* frames[64];
        unsigned short count = CaptureStackBackTrace(1, 48, frames, NULL);
        unsigned long long exe = (unsigned long long)GetModuleHandleA(NULL);
        unsigned long long av  = (unsigned long long)GetModuleHandleA("libagave.dll");
        unsigned long long ab  = (unsigned long long)GetModuleHandleA("antjsb.dll");

        for (i = 0; i < (int)count && n < (int)sizeof(buf) - 64; ++i) {
            unsigned long long a = (unsigned long long)frames[i];
            const char* mod = "?";
            unsigned long long off = a;

            if (av && a >= av && a < av + 0x2000000ULL) { mod = "libagave"; off = a - av; }
            else if (ab && a >= ab && a < ab + 0x2000000ULL) { mod = "antjsb"; off = a - ab; }
            else if (exe && a >= exe && a < exe + 0x100000ULL) { mod = "exe"; off = a - exe; }

            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "  #%02d %s+0x%llx\n", i, mod, off);
        }
    }

    {
        HANDLE h = CreateFileA("agb_crash.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(h, buf, (DWORD)strlen(buf), &wrote, NULL);
            CloseHandle(h);
        }
    }

    shellAnnounceCrashFile("agb_crash.txt");

    /* Ask ant for the JS stack: without it the native frames only say
     * "JIT code". Resolved dynamically - the shell does not link antjsb. */
    {
        HMODULE ant = GetModuleHandleA("antjsb.dll");

        if (ant) {
            typedef void (*DumpJsStackFn)(void);
            DumpJsStackFn dump = (DumpJsStackFn)GetProcAddress(ant, "antb_print_last_js_stack");

            if (dump)
                dump();
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* Crash forensics: an AV that only happens outside the debugger (timing
 * sensitive UAFs, e.g. R67/R68) cannot be caught with cdb - attaching changes
 * the schedule and the bug disappears. This filter prints the faulting address
 * and the faulting thread's stack in the same "#NN module+0x..." shape as the
 * atexit trace, so tools/cdb_stack.ps1 -Symbolize can resolve it afterwards. */
static LONG WINAPI shellCrashFilter(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : NULL;

    fprintf(stderr, "[crash] thread=%lu code=0x%08lx addr=0x%llx",
            (unsigned long)GetCurrentThreadId(),
            (unsigned long)(rec ? rec->ExceptionCode : 0),
            (unsigned long long)(rec ? (unsigned long long)rec->ExceptionAddress : 0));

    if (rec && (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec->NumberParameters >= 2) {
        fprintf(stderr, " %s 0x%llx",
                rec->ExceptionInformation[0] == 0 ? "read" :
                rec->ExceptionInformation[0] == 1 ? "write" : "exec",
                (unsigned long long)rec->ExceptionInformation[1]);
    }

    fprintf(stderr, "\n");
    fflush(stderr);

    shellPrintFrames(1, 48);
    fflush(stderr);
    shellWriteCrashFile(rec);
    shellAnnounceCrashFile("agb_crash.txt");

    /* Keep the OS behaviour (exit code 0xC0000005 etc.): let the default
     * handler / WER terminate the process. */
    return EXCEPTION_CONTINUE_SEARCH;
}

static void shellExitTrace(void)
{
    unsigned long long exe = (unsigned long long)GetModuleHandleA(NULL);
    unsigned long long av  = (unsigned long long)GetModuleHandleA("libagave.dll");
    unsigned long long ab  = (unsigned long long)GetModuleHandleA("antjsb.dll");

    agbLog(AGB_LOG_SHELL, "atexit fired, bases exe=%llx agave=%llx ant=%llx", exe, av, ab);
    shellPrintFrames(1, 32);
    fflush(stderr);
}

/* ------------------------------------------------------------------------- */
/* probe mode: headless load + JS evaluation, for page debugging             */
/* ------------------------------------------------------------------------- */

#define PROBE_TIMER_EVAL    1
#define PROBE_TIMER_SETTLE  2
#define PROBE_TIMER_TIMEOUT 3
#define PROBE_TIMER_POLL    4   /* repeating: poll new document after reload */
#define PROBE_TIMER_INTERRUPT 5 /* one-shot: --interrupt-after (hang recovery) */
#define PROBE_TIMER_NAV     6   /* one-shot: --nav-after (navigation aborts a stuck page) */
#define PROBE_TIMER_RELOAD  7   /* repeating: --reload-every (hammer Reload) */
#define PROBE_TIMER_HIDE    8   /* one-shot: --hide-after (Page Visibility) */
#define PROBE_TIMER_SHOW    9   /* one-shot: --show-after (Page Visibility) */

/* Engine-thread callbacks post this to the probe window (main thread) to arm
 * the eval timer: window timers must be installed by the owning thread. */
#define AGW_PROBE_MSG (WM_APP + 77)
/* Engine thread -> main thread: the --eval script has finished. */
#define AGW_PROBE_EVALDONE (WM_APP + 78)

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
static int g_probeLoadArmed;   /* the load-finished signal already scheduled --eval */
static int g_probeEvalDone;
static int g_probeGotExit;
static int g_probeSawError;
static int g_probeSawDownload;   /* a download finished: that IS the outcome */
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
static char g_probeShot[512];     /* --shot: capture here, then exit        */
static int  g_probeShotRect[4];   /* --shot-rect x,y,w,h (client pixels)     */
static int  g_probeShotRectSet;   /* a rect was given (else the whole view) */
static int  g_probeShotSettled;   /* one frame elapsed since --eval          */
static char g_probeKeys[512];     /* --keys: typed into the page before eval */
static int  g_probeKeysDone;      /* the --keys list was typed out             */
static char g_probePreScript[2048]; /* --pre-eval: runs once before --keys     */
static char g_probeIme[512];        /* --ime: posted as WM_IME_CHAR           */
static int  g_probePreDone;       /* --pre-eval was executed                   */
static char g_probeInject[512];   /* --inject: script asset to run before eval */
static int  g_probeInjectDone;    /* --inject already ran for this document    */
static int  g_probeResizeW;       /* --resize WxH: control size before --eval   */
static int  g_probeResizeH;
static int  g_probeResizeDone;    /* the probe window was resized already      */
static int  g_probeZoom;          /* --zoom <percent>: page zoom before --eval  */
static int  g_probeZoomDone;

/* Offscreen capture. A hidden window gets no WM_PAINT, so PrintWindow has
 * nothing to render; the control keeps its own mirror buffer, and
 * agave_webview_paint_to() blits that into our DIB. Nothing is ever shown, so
 * a screenshot cannot disturb the user (iron law 7).
 *
 * The extension picks the encoding: .bmp is the raw 32-bit bottom-up DIB that
 * pixel_check.py has always read, .png goes straight out through the libpng
 * copy the engine already links (dist/) - no post-processing step needed.
 * --shot-rect crops the grab to a client-pixel rect, so one element can be
 * captured without cropping the image afterwards. */

#define PROBE_SHOT_BMP     0
#define PROBE_SHOT_PNG     1
#define PROBE_SHOT_UNKNOWN (-1)

static int probeShotFormat(const char* path)
{
    const char* dot = strrchr(path, '.');
    const char* slash = strrchr(path, '\\');
    const char* fwd = strrchr(path, '/');

    if (fwd && (!slash || fwd > slash))
        slash = fwd;

    /* A dot in a directory name is not an extension; no extension at all is
     * still an unambiguous "give me an image", so it means PNG. */
    if (!dot || (slash && dot < slash))
        return PROBE_SHOT_PNG;

    if (!_stricmp(dot, ".bmp"))
        return PROBE_SHOT_BMP;
    if (!_stricmp(dot, ".png"))
        return PROBE_SHOT_PNG;

    return PROBE_SHOT_UNKNOWN;
}

/* Writes the (rx,ry,rw,rh) crop of a bottom-up 32-bit BGRA frame as a
 * top-down RGB PNG. Alpha is dropped on purpose: the DIB section keeps none
 * (BI_RGB, alpha byte unused), and a screenshot with a transparent background
 * would be worse than one without an alpha channel. */
static int probeWritePng(const char* path, const unsigned char* bits,
                         int w, int h, int stride,
                         int rx, int ry, int rw, int rh)
{
    FILE* f;
    png_structp png;
    png_infop info = NULL;
    unsigned char* row;
    int y, ok = 0;

    f = fopen(path, "wb");
    if (!f) {
        agbLog(AGB_LOG_PROBE, "shot: cannot open %s (errno %d)", path, errno);
        return 0;
    }

    row = (unsigned char*)malloc((size_t)rw * 3);
    if (!row) {
        fclose(f);
        return 0;
    }

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        free(row);
        fclose(f);
        return 0;
    }
    info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        free(row);
        fclose(f);
        return 0;
    }

    /* libpng reports failures (write error, allocation) by longjmp'ing here.
     * f/png/info/row are set before this point and never reassigned, so their
     * values are still valid on the error path. */
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        free(row);
        fclose(f);
        remove(path);
        return 0;
    }

    png_init_io(png, f);
    png_set_IHDR(png, info, (png_uint_32)rw, (png_uint_32)rh, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    for (y = 0; y < rh; ++y) {
        /* PNG rows run top-down; the DIB is bottom-up, so client row
         * (ry + y) lives at DIB row (h - 1 - (ry + y)). */
        const unsigned char* src = bits
            + (size_t)(h - 1 - (ry + y)) * (size_t)stride + (size_t)rx * 4;
        int x;

        for (x = 0; x < rw; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        png_write_row(png, row);
    }

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    free(row);
    ok = ferror(f) ? 0 : 1;
    fclose(f);
    if (!ok)
        remove(path);
    return ok;
}

/* BMP stays exactly what it always was (bottom-up, 32-bit): pixel_check.py and
 * the pixel-assertion suites read it. The crop is written bottom-up too, so a
 * full-frame capture is byte-identical to the old output. */
static int probeWriteBmp(const char* path, const unsigned char* bits,
                         int w, int h, int stride,
                         int rx, int ry, int rw, int rh)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER bh;
    int rowBytes = rw * 4;
    int y, ok;
    FILE* f;

    f = fopen(path, "wb");
    if (!f) {
        agbLog(AGB_LOG_PROBE, "shot: cannot open %s (errno %d)", path, errno);
        return 0;
    }

    memset(&bh, 0, sizeof(bh));
    bh.biSize = sizeof(BITMAPINFOHEADER);
    bh.biWidth = rw;
    bh.biHeight = rh;               /* bottom-up: the BMP order */
    bh.biPlanes = 1;
    bh.biBitCount = 32;
    bh.biCompression = BI_RGB;

    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;             /* 'BM' */
    fh.bfOffBits = (DWORD)(sizeof(fh) + sizeof(BITMAPINFOHEADER));
    fh.bfSize = fh.bfOffBits + (DWORD)((size_t)rowBytes * (size_t)rh);

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bh, sizeof(BITMAPINFOHEADER), 1, f);
    for (y = 0; y < rh; ++y) {
        /* Bottom-up crop row y is client row (ry + rh - 1 - y); the DIB is
         * bottom-up as well, so its row index is (h - ry - rh + y). */
        const unsigned char* src = bits
            + (size_t)(h - ry - rh + y) * (size_t)stride + (size_t)rx * 4;
        fwrite(src, (size_t)rowBytes, 1, f);
    }

    ok = ferror(f) ? 0 : 1;
    fclose(f);
    if (!ok)
        remove(path);
    return ok;
}

/* CSS px -> client (device) px at the probe's zoom, rounding half away from
 * zero. The mirror buffer is device px, while getBoundingClientRect() - the
 * thing a --shot-rect value comes from - is CSS px: at --zoom 125 a 100px box
 * covers 125 mirror pixels. */
static int probeRectCssToClient(int value, int zoomPercent)
{
    long v = (long)value * zoomPercent;

    return (int)((v >= 0 ? v + 50 : v - 50) / 100);
}

static int probeCapture(const char* path)
{
    HWND target = (HWND)agave_webview_hwnd(g_probeView);
    RECT rc;
    HDC mdc;
    HBITMAP dib;
    BITMAPINFO bi;
    unsigned char* bits = NULL;
    int w, h, stride, fmt, ok = 0;
    int rx = 0, ry = 0, rw, rh;

    if (!target || !GetClientRect(target, &rc))
        return 0;

    w = (int)(rc.right - rc.left);
    h = (int)(rc.bottom - rc.top);
    if (w <= 0 || h <= 0)
        return 0;

    fmt = probeShotFormat(path);
    if (fmt == PROBE_SHOT_UNKNOWN) {
        agbLog(AGB_LOG_PROBE, "shot: unknown extension in %s (use .png or .bmp)", path);
        return 0;
    }

    /* --shot-rect is in CSS px, origin at the viewport's top-left: exactly what
     * getBoundingClientRect() reports for an element scrolled into view, at any
     * --zoom. Both edges are converted before subtracting so a fractional zoom
     * cannot shrink the crop by a pixel. Clamp instead of failing: a stale rect
     * still yields the part that is on screen, and a rect fully outside is
     * reported as a failure, not a blank file that looks like a rendering bug. */
    rw = w;
    rh = h;
    if (g_probeShotRectSet) {
        int zoom = g_probeZoom > 0 ? g_probeZoom : 100;
        int x0 = probeRectCssToClient(g_probeShotRect[0], zoom);
        int y0 = probeRectCssToClient(g_probeShotRect[1], zoom);
        int x1 = probeRectCssToClient(g_probeShotRect[0] + g_probeShotRect[2], zoom);
        int y1 = probeRectCssToClient(g_probeShotRect[1] + g_probeShotRect[3], zoom);

        rx = x0;
        ry = y0;
        rw = x1 - x0;
        rh = y1 - y0;

        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }

        if (rx >= w || ry >= h || rw <= 0 || rh <= 0) {
            agbLog(AGB_LOG_PROBE, "shot: rect %d,%d,%d,%d (css px) is outside the %dx%d view",
                   g_probeShotRect[0], g_probeShotRect[1],
                   g_probeShotRect[2], g_probeShotRect[3],
                   (w * 100 + zoom / 2) / zoom, (h * 100 + zoom / 2) / zoom);
            return 0;
        }
        if (rx + rw > w) rw = w - rx;
        if (ry + rh > h) rh = h - ry;
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;      /* bottom-up: the BMP order */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    mdc = CreateCompatibleDC(NULL);
    if (!mdc)
        return 0;
    dib = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mdc);
        return 0;
    }
    SelectObject(mdc, dib);

    agave_webview_paint_to(g_probeView, mdc, w, h);

    stride = w * 4;
    if (fmt == PROBE_SHOT_BMP)
        ok = probeWriteBmp(path, bits, w, h, stride, rx, ry, rw, rh);
    else
        ok = probeWritePng(path, bits, w, h, stride, rx, ry, rw, rh);

    if (ok)
        agbLog(AGB_LOG_PROBE, "shot %dx%d at %d,%d %s -> %s",
               rw, rh, rx, ry, fmt == PROBE_SHOT_BMP ? "bmp" : "png", path);

    DeleteObject(dib);
    DeleteDC(mdc);
    return ok;
}

/* Command line, parsed before any window exists: a --probe run must never
 * create the browser window (it used to be shown, then left on screen for the
 * whole capture because probeRun runs its own message loop). */
static int  g_showUsage;                        /* --help                     */
static int  g_argError;                         /* --probe without a URL       */
static char g_startupUrl[MAX_PATH_CHARS];       /* normalized, may be empty   */

static void probePrint(const char* tag, const char* text)
{
    printf("[%9.1fms] %s %s\n", agbLogElapsedMs(), tag, text ? text : "");
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
        const char* href = message + 14;

        if (!g_probeFinished) {
            char line[768];

            _snprintf(line, sizeof(line), "window load event url=%s", href);
            line[sizeof(line) - 1] = 0;
            probePrint("[probe]", line);
            PostMessageA(g_probeWnd, AGW_PROBE_MSG, g_probeWaitMs, 0);
        }
        return;
    }

    /* The --eval script has returned (the probe posts a follow-up eval that
     * sends this). Only now may --settle start counting: it used to be armed
     * when the eval was *posted*, so any script slower than --settle had the
     * view destroyed underneath it - the 0xC0000005 that looked like a JIT
     * crash (R61). Still a timer on the main thread: the message arrives from
     * the engine thread. */
    if (message && !strncmp(message, "__agb_evaldone__", 16)) {
        PostMessageA(g_probeWnd, AGW_PROBE_EVALDONE, 0, 0);
        return;
    }

    probePrint("[msg]", message);
}

static void probeOnError(AgaveWebView* view, int code, const char* url, void* user)
{
    (void)view;
    (void)user;

    g_probeSawError = 1;
    agbLog(AGB_LOG_PROBE, "load error code=%d url=%s", code, url ? url : "");

    /* A failed navigation commits no document, so on_progress(finished) never
     * arrives. Arm the eval here or the run would sit until --timeout; the
     * eval then reports the empty document and the run ends with the load
     * error code. */
    if (!g_probeFinished && !g_probeReloadMode && !g_probeLoadArmed) {
        g_probeLoadArmed = 1;
        PostMessageA(g_probeWnd, AGW_PROBE_MSG, g_probeWaitMs, 0);
    }
}

static int probeOnNewWindow(AgaveWebView* view, AgaveWebView* newView,
                            const char* url, void* user)
{
    (void)view;
    (void)user;

    /* Keep it: the probe reports the URL the new window ended up on, which is
     * what tells a working new-window path from a blank one. */
    g_probeNewView = newView;
    agbLog(AGB_LOG_PROBE, "new window requested url=%s", url ? url : "");
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
    agbLog(AGB_LOG_PROBE, "dialog type=%d msg=%s", type, message ? message : "");
    fflush(stderr);

    if (message && strstr(message, "LEAVE-GUARD")) {
        const char* allow = getenv("AGB_PROBE_ALLOW_UNLOAD");

        return (allow && *allow && *allow != '0') ? 1 : 0;
    }

    return 1;
}

/* The watchdog's notice, on the UI thread. The probe has no window to show it
 * in, so it prints one line: that is what makes "the engine aborted a page and
 * told the host" testable headlessly (AGB_UNRESPONSIVE_MS=2000 on a spinner). */
static void probeOnUnresponsive(AgaveWebView* view, const char* reason,
                                unsigned int busy_ms, void* user)
{
    (void)view;
    (void)user;

    probePrint("[probe] unresponsive", reason ? reason : "");
    (void)busy_ms;
}

/* Right click reaches the host on the UI thread (the control copies the payload
 * and posts it there; a menu cannot be built from the engine thread). The probe
 * only records it - no menu, so the run stays headless - which is what makes the
 * path testable without a human at the mouse. */
static void probeOnContextMenu(AgaveWebView* view, int x, int y,
                               const char* linkUrl, const char* imageUrl,
                               const char* text, void* user)
{
    (void)view;
    (void)user;
    (void)text;

    agbLog(AGB_LOG_PROBE, "context menu at %d,%d link=%s image=%s",
           x, y, linkUrl && linkUrl[0] ? linkUrl : "(none)",
           imageUrl && imageUrl[0] ? imageUrl : "(none)");
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

/* A download is a successful navigation outcome with no document behind it:
 * nothing commits, so on_progress(finished) never fires and there is nothing to
 * eval. Report the file and leave with the OK exit code. */
static void probeOnDownload(AgaveWebView* view, const MC_DOWNLOAD_INFO* info, void* user)
{
    (void)view;
    (void)user;

    if (!info)
        return;

    if (info->state == MC_DL_FINISHED) {
        /* Only a download that *is* the navigation counts as the outcome: if a
         * document already committed, this was a side effect of the page (an
         * iframe, a link) and the run must end the way it normally does. */
        if (!g_probeFinished) {
            g_probeSawDownload = 1;

            printf("[%9.1fms] [probe] download path=%s bytes=%llu total=%llu\n",
                   agbLogElapsedMs(), info->utf8_path ? info->utf8_path : "",
                   info->received, info->total);
            fflush(stdout);

            PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
        } else {
            agbLog(AGB_LOG_PROBE, "download finished path=%s bytes=%llu",
                   info->utf8_path ? info->utf8_path : "", info->received);
        }
    } else if (info->state == MC_DL_FAILED) {
        g_probeSawError = 1;

        printf("[%9.1fms] [probe] download failed code=%d url=%s\n",
               agbLogElapsedMs(), info->error, info->utf8_url ? info->utf8_url : "");
        fflush(stdout);

        if (!g_probeFinished)
            PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
    }
}

static void probeOnProgress(AgaveWebView* view, int percent, int finished, void* user)
{
    (void)view;
    (void)percent;
    (void)user;

    /* Engine-thread callback: never SetTimer here (the probe window is owned
     * by the main thread; cross-thread window timers are undefined). Post the
     * request to the main loop instead.
     *
     * "progress finished" is the engine's own "the document loaded" signal and
     * fires exactly once, for the requested document - a frame now gets its one
     * document from its first commit, so there is no initial empty document to
     * report early. --wait is therefore measured from the real load, and no JS
     * hook or readyState poll is needed. */
    if (finished && !g_probeFinished && !g_probeLoadArmed) {
        g_probeLoadArmed = 1;
        probePrint("[probe]", g_probeSawError ? "progress finished (with error)" : "progress finished");
        PostMessageA(g_probeWnd, AGW_PROBE_MSG, g_probeWaitMs, 0);
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

    if (message == AGW_PROBE_EVALDONE) {
        /* The eval finished. A probeExit()/sanity abort already closed the
         * window; otherwise the settle delay starts here. */
        if (!g_probeGotExit && !g_probeSanityFailed) {
            KillTimer(g_probeWnd, PROBE_TIMER_SETTLE);
            SetTimer(g_probeWnd, PROBE_TIMER_SETTLE, g_probeSettleMs, NULL);
        }
        return 0;
    }

    /* The message loop must end when the window goes away: GetMessage(NULL)
     * only returns 0 on WM_QUIT, never because the last window was destroyed. */
    if (message == WM_DESTROY)
        PostQuitMessage(0);

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

/* --keys: replay real typing through the control's own window procedure, so a
 * headless probe exercises the same path a keyboard does (WM_KEYDOWN, WM_CHAR,
 * WM_KEYUP) instead of a DOM-level shortcut that would skip the key handling
 * under test.
 *
 * {backspace} {enter} {tab} {esc} {delete} {left} {right} {up} {down} {home}
 * {end} name a non-printable key; everything else is typed as UTF-8.
 *
 * Keys go out one per timer tick: TranslateMessage appends the WM_CHAR to the
 * end of the message queue, so a burst of posted keydowns would deliver every
 * character after all of them - a real keyboard never does that, and the page
 * would see the keys in the wrong order. */
static int probeKeyIsExtended(unsigned vk)
{
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END: case VK_DELETE: case VK_INSERT:
    case VK_PRIOR: case VK_NEXT:
        return 1;
    default:
        return 0;
    }
}

typedef struct {
    unsigned vk;    /* virtual key for WM_KEYDOWN, 0 = text only */
    unsigned ch;    /* the character the key produces, 0 = none */
    unsigned ime;   /* 1 = post WM_IME_CHAR (what an IME delivers) */
} ProbeKey;

#define PROBE_MAX_KEYS 128
static ProbeKey g_probeKeyList[PROBE_MAX_KEYS];
static int g_probeKeyCount;
static int g_probeKeyIndex;

static void probeParseKeys(void)
{
    const unsigned char* p = (const unsigned char*)g_probeKeys;

    g_probeKeyCount = 0;

    while (*p && g_probeKeyCount < PROBE_MAX_KEYS) {
        unsigned vk = 0;
        unsigned ch = 0;
        unsigned cp;

        if (*p == '{') {
            const char* name = (const char*)p + 1;
            const char* close = strchr(name, '}');
            size_t len = close ? (size_t)(close - name) : 0;

            if (!close) {
                ++p;            /* stray '{': type it literally */
                ch = '{';
            } else {
                if (len == 9 && !strncmp(name, "backspace", 9))      { vk = VK_BACK;   ch = 8; }
                else if (len == 5 && !strncmp(name, "enter", 5))     { vk = VK_RETURN; ch = 13; }
                else if (len == 3 && !strncmp(name, "tab", 3))       { vk = VK_TAB;    ch = 9; }
                else if (len == 3 && !strncmp(name, "esc", 3))       { vk = VK_ESCAPE; }
                else if (len == 6 && !strncmp(name, "delete", 6))    { vk = VK_DELETE; }
                else if (len == 4 && !strncmp(name, "left", 4))      { vk = VK_LEFT; }
                else if (len == 5 && !strncmp(name, "right", 5))     { vk = VK_RIGHT; }
                else if (len == 2 && !strncmp(name, "up", 2))        { vk = VK_UP; }
                else if (len == 4 && !strncmp(name, "down", 4))      { vk = VK_DOWN; }
                else if (len == 4 && !strncmp(name, "home", 4))      { vk = VK_HOME; }
                else if (len == 3 && !strncmp(name, "end", 3))       { vk = VK_END; }
                p = (const unsigned char*)close + 1;
                if (!vk && !ch)
                    continue;   /* unknown token: skip it */
            }
        } else {
            /* Decode one UTF-8 code point (the control re-encodes it). */
            if (p[0] < 0x80) {
                cp = p[0];
                p += 1;
            } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
                cp = ((unsigned)(p[0] & 0x1F) << 6) | (unsigned)(p[1] & 0x3F);
                p += 2;
            } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                cp = ((unsigned)(p[0] & 0x0F) << 12) |
                     ((unsigned)(p[1] & 0x3F) << 6) | (unsigned)(p[2] & 0x3F);
                p += 3;
            } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
                       (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
                cp = ((unsigned)(p[0] & 0x07) << 18) | ((unsigned)(p[1] & 0x3F) << 12) |
                     ((unsigned)(p[2] & 0x3F) << 6) | (unsigned)(p[3] & 0x3F);
                p += 4;
            } else {
                ++p;            /* invalid byte: skip */
                continue;
            }

            if (cp >= 0x10000) {
                /* WM_CHAR carries one UTF-16 code unit: an astral character is
                 * a surrogate pair, i.e. two messages. */
                unsigned int pair = cp - 0x10000;
                unsigned int hi = 0xD800 + (pair >> 10);
                unsigned int lo = 0xDC00 + (pair & 0x3FF);

                if (g_probeKeyCount + 1 < PROBE_MAX_KEYS) {
                    g_probeKeyList[g_probeKeyCount].vk = 0;
                    g_probeKeyList[g_probeKeyCount].ch = hi;
                    g_probeKeyList[g_probeKeyCount].ime = 0;
                    g_probeKeyCount++;
                    g_probeKeyList[g_probeKeyCount].vk = 0;
                    g_probeKeyList[g_probeKeyCount].ch = lo;
                    g_probeKeyList[g_probeKeyCount].ime = 0;
                    g_probeKeyCount++;
                }
                continue;
            }

            ch = cp;
            /* The virtual key a real keyboard reports: letters arrive as
             * VK_A..VK_Z, digits as their ASCII code. Non-ASCII text has no vk. */
            if (cp >= 'a' && cp <= 'z')
                vk = cp - 'a' + 'A';
            else if (cp < 0x80)
                vk = cp;
        }

        g_probeKeyList[g_probeKeyCount].vk = vk;
        g_probeKeyList[g_probeKeyCount].ch = ch;
        g_probeKeyCount++;
    }
}

/* --ime: what a real IME delivers. Each character arrives as WM_IME_CHAR with
 * one UTF-16 code unit (a character outside the BMP arrives as two of them, the
 * surrogate pair), not as WM_KEYDOWN + WM_CHAR. The text is appended to the
 * key list so it shares the one-per-tick pacing. */
static void probeParseIme(void)
{
    WCHAR wide[PROBE_MAX_KEYS];
    int count;
    int i;

    if (!g_probeIme[0])
        return;

    count = MultiByteToWideChar(CP_UTF8, 0, g_probeIme, -1, wide, PROBE_MAX_KEYS - 1);
    if (count <= 0)
        return;

    for (i = 0; i < count && g_probeKeyCount < PROBE_MAX_KEYS; ++i) {
        if (!wide[i])
            break;
        g_probeKeyList[g_probeKeyCount].vk = 0;
        g_probeKeyList[g_probeKeyCount].ch = (unsigned)wide[i];
        g_probeKeyList[g_probeKeyCount].ime = 1;
        g_probeKeyCount++;
    }
}

/* Sends the next key; returns 0 when the list is exhausted. */
static int probeSendNextKey(void)
{
    HWND target = (HWND)agave_webview_hwnd(g_probeView);
    ProbeKey key;

    if (!target || g_probeKeyIndex >= g_probeKeyCount)
        return 0;

    key = g_probeKeyList[g_probeKeyIndex++];

    /* A posted WM_KEYDOWN is turned into WM_CHAR by the shell's own
     * TranslateMessage, exactly like a keyboard message; posting WM_CHAR as
     * well would deliver every character twice. Only text without a virtual
     * key (non-ASCII) has to be posted directly.
     *
     * The lParam has to look like a real keyboard message: TranslateMessage
     * derives WM_CHAR from it, and a zeroed lParam makes the WM_KEYUP look
     * like a key press too, so every character arrived twice. */
    if (key.ime) {
        agbLog(AGB_LOG_PROBE, "ime u+%04X", key.ch);
        PostMessageW(target, WM_IME_CHAR, (WPARAM)key.ch, 1);
    } else if (key.vk) {
        unsigned scan = MapVirtualKeyA(key.vk, MAPVK_VK_TO_VSC);
        LPARAM down = (LPARAM)(1 | (scan << 16) | (probeKeyIsExtended(key.vk) ? (1 << 24) : 0));
        LPARAM up = (LPARAM)(down | (1 << 30) | (1 << 31));

        agbLog(AGB_LOG_PROBE, "key vk=%u ch=%u", key.vk, key.ch);
        PostMessageA(target, WM_KEYDOWN, (WPARAM)key.vk, down);
        PostMessageA(target, WM_KEYUP, (WPARAM)key.vk, up);
    } else if (key.ch) {
        agbLog(AGB_LOG_PROBE, "char cp=%u", key.ch);
        PostMessageW(target, WM_CHAR, (WPARAM)key.ch, 1);
    }

    return 1;
}

static int probeRun(const char* url)
{
    /* Own profile root before any engine exists (cookies/localStorage/caches). */
    probeUserDataInit();
    shellStateInit();

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
        agbLog(AGB_LOG_PROBE, "CreateWindowEx failed: %lu", (unsigned long)GetLastError());
        return PROBE_EXIT_USAGE;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_message = probeOnMessage;
    callbacks.on_progress = probeOnProgress;
    callbacks.on_error = probeOnError;
    callbacks.on_download = probeOnDownload;
    callbacks.on_new_window = probeOnNewWindow;
    callbacks.on_close_window = probeOnCloseWindow;
    callbacks.on_dialog = probeOnDialog;
    callbacks.on_context_menu = probeOnContextMenu;
    callbacks.on_unresponsive = probeOnUnresponsive;

    g_probeView = agave_webview_create(g_probeWnd, 0, 0, 1100, 800, &callbacks, NULL);
    if (!g_probeView) {
        agbLog(AGB_LOG_PROBE, "agave_webview_create failed");
        return PROBE_EXIT_USAGE;
    }

    /* AGB_PROBE_SHOW=1 shows the window; debugging aid for load paths that
     * behave differently when the control is never visible. A hidden window
     * gets no WM_PAINT, so this is also what pixel checks need. */
    g_probeVisible = getenv("AGB_PROBE_SHOW") != NULL;
    if (g_probeVisible)
        ShowWindow(g_probeWnd, SW_SHOWNOACTIVATE);

    agbLog(AGB_LOG_PROBE, "headless %s (pid %lu)",
           g_probeVisible ? "window shown (AGB_PROBE_SHOW)" : "window hidden, AGB_PROBE_SHOW=1 to show",
           (unsigned long)GetCurrentProcessId());

    SetTimer(g_probeWnd, PROBE_TIMER_TIMEOUT, g_probeTimeoutMs, NULL);
    if (g_probeInterruptAfterMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_INTERRUPT, (UINT)g_probeInterruptAfterMs, NULL);
    if (g_probeNavAfterMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_NAV, (UINT)g_probeNavAfterMs, NULL);
    if (g_probeHideAfterMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_HIDE, (UINT)g_probeHideAfterMs, NULL);
    if (g_probeShowAfterMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_SHOW, (UINT)g_probeShowAfterMs, NULL);
    if (g_probeReloadEveryMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_RELOAD, (UINT)g_probeReloadEveryMs, NULL);

    /* Focus only makes sense for a visible window; a hidden control that grabs
     * focus activates a window nobody can see. */
    if (g_probeVisible)
        agave_webview_set_focus(g_probeView);

    agave_webview_navigate(g_probeView, url);

    /* Nothing is injected here any more: the engine's on_progress(finished)
     * callback is the single "the requested document loaded" signal, and --wait
     * is measured from it. */

    while (GetMessageW(&msg, NULL, 0, 0)) {
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

            if (msg.wParam == PROBE_TIMER_RELOAD) {
                if (g_probeReloadCount-- <= 0) {
                    KillTimer(g_probeWnd, PROBE_TIMER_RELOAD);
                } else if (g_probeView) {
                    probePrint("[probe]", "reload-every: posting reload");
                    agbLog(AGB_LOG_PROBE, "reload (hammer) %d left", g_probeReloadCount);
                    agave_webview_reload(g_probeView);
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_NAV) {
                KillTimer(g_probeWnd, PROBE_TIMER_NAV);
                if (g_probeView) {
                    probePrint("[probe]", "nav-after: navigating while the page is busy");
                    agave_webview_navigate(g_probeView,
                                           g_probeNavUrl[0] ? g_probeNavUrl : "about:blank");
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_HIDE) {
                KillTimer(g_probeWnd, PROBE_TIMER_HIDE);
                if (g_probeView) {
                    probePrint("[probe]", "hide-after: tab hidden");
                    agave_webview_set_visible(g_probeView, 0);
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_SHOW) {
                KillTimer(g_probeWnd, PROBE_TIMER_SHOW);
                if (g_probeView) {
                    probePrint("[probe]", "show-after: tab visible again");
                    agave_webview_set_visible(g_probeView, 1);
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_INTERRUPT) {
                KillTimer(g_probeWnd, PROBE_TIMER_INTERRUPT);
                if (g_probeView) {
                    probePrint("[probe]", "interrupt-after: aborting the page script");
                    agave_webview_interrupt(g_probeView);
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_EVAL) {
                KillTimer(g_probeWnd, PROBE_TIMER_EVAL);

                /* agave_webview_eval pumps the control's queue, which can
                 * re-enter this message loop while the EVAL timer message is
                 * still pending; without the flag the branch re-runs and the
                 * script gets evaluated once per nesting level. */
                if (g_probeEvalDone)
                    continue;

                /* --inject: run a script asset exactly the way the shell menus
                 * do, before --keys/--eval. The queue is FIFO, so waiting one
                 * tick is enough for the injected script to have run. */
                if (g_probeInject[0] && !g_probeInjectDone) {
                    g_probeInjectDone = 1;
                    probePrint("[probe]", "injecting script");
                    /* The names go through the same functions the menus call, so
                     * a probe covers the real injection path (including the
                     * vConsole instantiation step). */
                    if (!strcmp(g_probeInject, "vconsole"))
                        injectVConsole(g_probeView);
                    else if (!strcmp(g_probeInject, "inspector"))
                        injectInspector(g_probeView);
                    else
                        injectScriptFile(g_probeView, g_probeInject, NULL);
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 250, NULL);
                    continue;
                }

                /* --keys / --ime run before the eval: one key per tick so the
                 * character of a key is processed before the next key, then a
                 * short pause for the last one, then the eval. */
                if ((g_probeKeys[0] || g_probeIme[0]) && !g_probeKeysDone) {
                    if (!g_probeKeyCount) {
                        probeParseKeys();
                        probeParseIme();
                    }
                    /* --pre-eval: focus the field / dismiss a dialog before the
                     * first key. Without it a headless run can only type into
                     * whatever the page focused by itself. */
                    if (!g_probeKeyIndex && !g_probePreDone) {
                        g_probePreDone = 1;
                        agave_webview_set_focus(g_probeView);
                        if (g_probePreScript[0]) {
                            agave_webview_eval(g_probeView, g_probePreScript);
                            SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 200, NULL);
                            continue;
                        }
                    }
                    if (probeSendNextKey()) {
                        SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 60, NULL);
                        continue;
                    }
                    g_probeKeysDone = 1;
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 300, NULL);
                    continue;
                }

                /* --zoom <percent>: apply page zoom before --eval (order with
                 * --resize does not matter, both are settled before the eval
                 * reads the viewport). */
                if (g_probeZoom > 0 && !g_probeZoomDone) {
                    g_probeZoomDone = 1;
                    probePrint("[probe]", "applying zoom");
                    agbLog(AGB_LOG_PROBE, "zoom %d%%", g_probeZoom);
                    agave_webview_set_zoom(g_probeView, g_probeZoom);
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 400, NULL);
                    continue;
                }

                /* --resize WxH: resize the control once before --eval, so a run
                 * can measure what the page does when the window changes size
                 * (relayout, viewport units, media queries, resize event). The
                 * parent popup is resized too, so the control's own WM_SIZE is
                 * what drives the engine - the same path a dragged window takes,
                 * minus the visible window. */
                if (g_probeResizeW > 0 && g_probeResizeH > 0 && !g_probeResizeDone) {
                    g_probeResizeDone = 1;
                    probePrint("[probe]", "resizing control");
                    SetWindowPos(g_probeWnd, NULL, 0, 0,
                                 g_probeResizeW, g_probeResizeH,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    agave_webview_set_rect(g_probeView, 0, 0,
                                           g_probeResizeW, g_probeResizeH);
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 400, NULL);
                    continue;
                }

                g_probeEvalDone = 1;

                /* --shot without --eval: capture what the page painted and
                 * leave. The window is never shown, so this is the headless
                 * screenshot path. With an eval script the capture has to wait
                 * for the script and its --settle delay (see PROBE_TIMER_SETTLE
                 * below), otherwise the shot would be the pre-eval frame. */
                if (g_probeShot[0] && !g_probeScript[0]) {
                    /* Let one more frame run: an --eval that scrolled or mutated
                     * the DOM has not been painted yet, and the control's mirror
                     * still holds the pre-eval pixels. */
                    if (!g_probeShotSettled) {
                        g_probeShotSettled = 1;
                        g_probeEvalDone = 0;
                        SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 300, NULL);
                        continue;
                    }
                    if (probeCapture(g_probeShot))
                        g_probeGotExit = 1;
                    else
                        agbLog(AGB_LOG_PROBE, "shot failed: %s", g_probeShot);
                    break;
                }

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
                    /* A failed navigation can commit a document that never got
                     * parsed. Judge it by the document itself: it must carry a
                     * URL String, be a real document tree, and have something
                     * in it. Counting element children under <body> was wrong:
                     * a page whose body holds only text is a perfectly good
                     * document and was reported as "empty". */
                    "window.__agbDocOk = ((typeof document.URL === 'string') "
                    "&& (document.URL.length > 0) "
                    "&& (document.URL !== 'about:blank') "
                    "&& (document.documentElement !== null) "
                    "&& (document.documentElement.innerHTML.length > 0));"
                    "if (window.__agbDocOk) {"
                    " window.probeExit = function (result) {"
                    "  agavePostMessage('__agb_exit__' + (result === undefined ? '' : String(result)));"
                    " };"
                    "} else {"
                    " agavePostMessage('__agb_sanity__ document is empty (page never parsed?) url=' + document.URL);"
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
                /* Eval-completion signal. The engine drains commands in order,
                 * so this marker runs right after the script returns - and it
                 * still runs when the script threw or failed to compile, which
                 * is why it is a second eval instead of a wrapper around the
                 * first one (a syntax error in the user script would take the
                 * wrapper down with it). --settle starts counting only from
                 * here: it used to be armed when the eval was posted, so a
                 * script slower than --settle had its view destroyed while it
                 * was still running (R61). */
                agave_webview_eval(g_probeView,
                    "if (typeof agavePostMessage === 'function')"
                    " agavePostMessage('__agb_evaldone__');");
                continue;
            }

            if (msg.wParam == PROBE_TIMER_SETTLE) {
                KillTimer(g_probeWnd, PROBE_TIMER_SETTLE);

                /* --reload: start the next round; its load reports through
                 * on_progress(finished) like any other load. */
                if (g_probeReloadsLeft > 0) {
                    g_probeReloadsLeft--;
                    g_probeRound++;
                    g_probeFinished = 0;
                    g_probeLoadArmed = 0;
                    g_probeEvalDone = 0;
                    g_probePollCount = 0;
                    g_probeInjectDone = 0;
                    probePrint("[probe]", "reloading");
                    agbLog(AGB_LOG_PROBE, "reload (--reload) round=%d of %d",
                           g_probeRound, g_probeTotalRounds);

                    /* The reloaded document reports its own load through
                     * on_progress(finished), exactly like the first one: no
                     * marker or readyState poll is needed, and a stale
                     * document cannot pass for the new one because the
                     * signal comes from the engine per load. */
                    agave_webview_reload(g_probeView);
                    continue;
                }

                if (g_probeReloadMode)
                    g_probeRoundsDone = 1;

                /* --eval + --shot: the script has returned and --settle has
                 * elapsed. Give the engine one more frame before capturing:
                 * the control's mirror still holds the pre-eval pixels (R82). */
                if (g_probeShot[0]) {
                    if (!g_probeShotSettled) {
                        g_probeShotSettled = 1;
                        SetTimer(g_probeWnd, PROBE_TIMER_SETTLE, 300, NULL);
                        continue;
                    }
                    if (probeCapture(g_probeShot))
                        g_probeGotExit = 1;
                    else
                        agbLog(AGB_LOG_PROBE, "shot failed: %s", g_probeShot);
                }

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
        DispatchMessageW(&msg);
    }

    if (g_probeNewView) {
        const char* u = agave_webview_url(g_probeNewView);
        const char* t = agave_webview_title(g_probeNewView);

        printf("[%9.1fms] [probe] new window url=%s title=%s\n", agbLogElapsedMs(),
               u && u[0] ? u : "(none)", t && t[0] ? t : "(none)");
        fflush(stdout);
        agave_webview_destroy(g_probeNewView);
        g_probeNewView = NULL;
    }

    {
        const char* u = agave_webview_url(g_probeView);

        printf("[%9.1fms] [probe] main url=%s\n", agbLogElapsedMs(), u && u[0] ? u : "(none)");
        fflush(stdout);
    }

    agave_webview_destroy(g_probeView);

    {
        int code;

        if (g_probeSawError)
            code = PROBE_EXIT_LOADERROR;   /* nothing was committed: say why */
        else if (g_probeSawDownload)
            code = PROBE_EXIT_OK;          /* a download IS the outcome */
        else if (g_probeSanityFailed)
            code = PROBE_EXIT_SANITY;
        else if (g_probeGotExit)
            code = PROBE_EXIT_OK;
        else if (g_probeReloadMode && g_probeRoundsDone)
            code = PROBE_EXIT_OK;   /* every reload round ran to the end */
        else if (g_probeTimedOut && !g_probeFinished)
            code = PROBE_EXIT_TIMEOUT_LOAD;
        else if (g_probeTimedOut)
            code = PROBE_EXIT_TIMEOUT_NOEXIT;
        else
            code = PROBE_EXIT_NOEXIT;

        agbLog(AGB_LOG_PROBE, "exit code %d", code);
        fflush(stderr);
        return code;
    }
}

static void probeUsage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  agave_shell.exe [url]\n"
            "  agave_shell.exe --probe <url> [--eval <js> | --eval-file <path>]\n"
            "                  [--wait <ms>] [--settle <ms>] [--timeout <ms>]\n"
            "  --eval    script to run after the load finished (default dumps outerHTML\n"
            "            via agavePostMessage; page console goes to stderr)\n"
            "            the script may call probeExit(result) to leave immediately\n"
            "  --eval-file  same as --eval, but read the script from a file: the\n"
            "            command line parser treats \" and \\ specially, so any JS\n"
            "            containing string literals belongs in a file\n"
            "  --inject <name|path>  run a script asset before --eval, exactly like\n"
            "            the shell menus: \"inspector\" (inspector.js) or \"vconsole\"\n"
            "            (vconsole.min.js) resolve next to the exe, anything else is\n"
            "            a file path (next to the exe first, then the cwd)\n"
            "  --wait    settle time after 'load finished' before eval (default 3000)\n"
            "  --settle  time after the eval script returns before exit (default 1000;\n"
            "            a script slower than settle is still allowed to finish)\n"
            "  --timeout hard cap from start (default 60000)\n"
            "  --nav-after <ms> [--nav-url <url>]  navigate while the page is busy\n"
            "  --hide-after <ms> / --show-after <ms>  Page Visibility change\n"
            "            (document.hidden + visibilitychange) without a GUI tab switch\n"
            "  --shot <path>  capture what the page painted and exit; the extension\n"
            "            picks the format: .png (written directly, no conversion\n"
            "            step) or .bmp (raw 32-bit bottom-up, what pixel_check.py\n"
            "            reads); any other extension is refused. The window stays\n"
            "            hidden, so this is the headless screenshot path (no focus\n"
            "            stealing, nothing on screen). Combine it with\n"
            "            --eval/--eval-file to capture what the script changed\n"
            "            (the capture waits for the script and --settle)\n"
            "  --shot-rect <x,y,w,h>  crop the capture to a CSS-pixel rect\n"
            "            (origin at the viewport's top-left, i.e. the values\n"
            "            getBoundingClientRect() reports for an element scrolled\n"
            "            into view): --shot-rect 10,10,320,200 captures one\n"
            "            element. The file stays device pixels, so with --zoom 125\n"
            "            a 100px rect comes out 125px wide. Parts outside the\n"
            "            viewport are clamped to what is on screen\n"
            "  --keys <text>  type into the page before --eval, through the real\n"
            "            WM_KEYDOWN/WM_CHAR/WM_KEYUP path (the page must focus its\n"
            "            own control; {backspace} {enter} {tab} {left} {right}\n"
            "            {up} {down} {home} {end} {delete} {esc} name a key)\n"
            "  --pre-eval <js>  short snippet run once before --keys (focus a\n"
            "            field: \"document.querySelector('input').focus()\")\n"
            "  --ime <text>  deliver text the way an IME does: WM_IME_CHAR per\n"
            "            UTF-16 code unit (surrogate pairs included), no keydown\n"
            "  --resize <WxH>  resize the control (and its hidden parent) once\n"
            "            after the page settled, then wait 400ms so the engine can\n"
            "            relayout before --eval reads the new viewport\n"
            "  --zoom <percent>  page zoom before --eval (10..500, default 100)\n"
            "  --reload <n>  reload the page n times and run the eval after\n"
            "                every round (page lifecycle comparison)\n"
            "  --offline module fetches are served from the cache only\n"
            "            (no network; warm the cache with a normal run first)\n"
            "  --userdata <dir>  profile root for this run (cookies, localStorage,\n"
            "            module/HTTP caches, shell session). --probe defaults to\n"
            "            %%LOCALAPPDATA%%\\Agave\\probe so tests never touch the GUI profile\n");
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
        } else if (!strcmp(argv[i], "--eval-file") && i + 1 < argc) {
            /* Same as --eval, but the script comes from a file. parseCommandLine
             * treats " and \ as syntax, so an inline script holding JS string
             * literals (or a regexp) arrives mangled; a file always survives. */
            FILE* scriptFile = fopen(argv[++i], "rb");

            if (!scriptFile) {
                fprintf(stderr, "[shell] --eval-file: cannot open %s\n", argv[i]);
                g_argError = 1;
                return;
            }
            {
                size_t got = fread(g_probeScript, 1, sizeof(g_probeScript) - 1, scriptFile);

                g_probeScript[got] = 0;
            }
            fclose(scriptFile);
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            strncpy(g_probeShot, argv[++i], sizeof(g_probeShot) - 1);
            g_probeShot[sizeof(g_probeShot) - 1] = 0;
        } else if (!strcmp(argv[i], "--shot-rect") && i + 1 < argc) {
            int rx = 0, ry = 0, rw = 0, rh = 0;

            /* x,y,w,h in client pixels (origin at the viewport's top-left). */
            if (sscanf(argv[++i], "%d,%d,%d,%d", &rx, &ry, &rw, &rh) == 4
                && rw > 0 && rh > 0) {
                g_probeShotRect[0] = rx;
                g_probeShotRect[1] = ry;
                g_probeShotRect[2] = rw;
                g_probeShotRect[3] = rh;
                g_probeShotRectSet = 1;
            } else {
                fprintf(stderr, "[shell] --shot-rect wants x,y,w,h (e.g. 10,10,320,200), got %s\n",
                        argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            strncpy(g_probeKeys, argv[++i], sizeof(g_probeKeys) - 1);
            g_probeKeys[sizeof(g_probeKeys) - 1] = 0;
        } else if (!strcmp(argv[i], "--pre-eval") && i + 1 < argc) {
            strncpy(g_probePreScript, argv[++i], sizeof(g_probePreScript) - 1);
            g_probePreScript[sizeof(g_probePreScript) - 1] = 0;
        } else if (!strcmp(argv[i], "--ime") && i + 1 < argc) {
            strncpy(g_probeIme, argv[++i], sizeof(g_probeIme) - 1);
            g_probeIme[sizeof(g_probeIme) - 1] = 0;
        } else if (!strcmp(argv[i], "--inject") && i + 1 < argc) {
            strncpy(g_probeInject, argv[++i], sizeof(g_probeInject) - 1);
            g_probeInject[sizeof(g_probeInject) - 1] = 0;
        } else if (!strcmp(argv[i], "--resize") && i + 1 < argc) {
            int rw = 0, rh = 0;

            /* WxH, the same spelling the desktop uses for window sizes. */
            if (sscanf(argv[++i], "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
                g_probeResizeW = rw;
                g_probeResizeH = rh;
            } else {
                fprintf(stderr, "[shell] --resize wants WxH (e.g. 500x400), got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) {
            int percent = atoi(argv[++i]);

            if (percent >= 10 && percent <= 500)
                g_probeZoom = percent;
            else {
                fprintf(stderr, "[shell] --zoom wants 10..500 (percent), got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            g_probeWaitMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--settle") && i + 1 < argc) {
            g_probeSettleMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            g_probeTimeoutMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--reload-every") && i + 1 < argc) {
            /* Hammer Reload like a user does: post it again every N ms even if
             * the previous load is still in flight. */
            g_probeReloadEveryMs = atoi(argv[++i]);
            if (!g_probeReloadCount) g_probeReloadCount = 10;
        } else if (!strcmp(argv[i], "--reload-count") && i + 1 < argc) {
            g_probeReloadCount = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--nav-after") && i + 1 < argc) {
            /* Navigate N ms after the probe starts, while the page is still
             * busy: the control aborts the running script at agwPost(). */
            g_probeNavAfterMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--hide-after") && i + 1 < argc) {
            g_probeHideAfterMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--show-after") && i + 1 < argc) {
            g_probeShowAfterMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--nav-url") && i + 1 < argc) {
            strncpy(g_probeNavUrl, argv[++i], sizeof(g_probeNavUrl) - 1);
            g_probeNavUrl[sizeof(g_probeNavUrl) - 1] = 0;
        } else if (!strcmp(argv[i], "--interrupt-after") && i + 1 < argc) {
            /* Abort the page's script N ms after the probe starts. A hung page
             * (or a page whose JS never returns) otherwise only ends at
             * --timeout; this exercises the same path Reload uses. */
            g_probeInterruptAfterMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--reload") && i + 1 < argc) {
            g_probeReloadsLeft = atoi(argv[++i]);
            g_probeReloadMode = 1;
            g_probeTotalRounds = g_probeReloadsLeft + 1;
        } else if (!strcmp(argv[i], "--userdata") && i + 1 < argc) {
            static char userdataEnv[MAX_PATH_CHARS + 32];

            /* Picked up by the shell and by the engine (cookies/localStorage/
             * module + HTTP caches). putenv, not SetEnvironmentVariableA: MinGW's
             * getenv reads the CRT environment. */
            _snprintf(userdataEnv, sizeof(userdataEnv), "AGB_USERDATA=%s", argv[++i]);
            userdataEnv[sizeof(userdataEnv) - 1] = 0;
            putenv(userdataEnv);
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
        agbLog(AGB_LOG_PROBE, "missing URL");
        probeUsage();
        g_argError = 1;
    }
}

/* The engine answers `(prefers-color-scheme: dark)` from AGB_COLOR_SCHEME and
 * defaults to light. Following the Windows "app mode" here turned every page
 * with a `@media (prefers-color-scheme: dark)` block dark on a dark-mode
 * machine (css3test rendered its dark theme, which reads as "the viewport is
 * black"). The default is now light — the conventional canvas — and the OS
 * theme is only consulted when the host is asked for it explicitly with
 * `AGB_COLOR_SCHEME=auto`.
 *
 * Read once, before any view (and therefore any stylesheet) exists. Set it with
 * SetEnvironmentVariableA, not putenv: the shell and the engine DLL each link
 * their own static CRT, so a CRT-local environment would never reach the
 * engine. */
static void applySystemColorScheme(void)
{
    char setting[16];
    DWORD length = GetEnvironmentVariableA("AGB_COLOR_SCHEME", setting, sizeof(setting));
    DWORD light = 1;
    DWORD size = sizeof(light);

    /* An explicit dark/light wins; only `auto` follows the OS. */
    if (length == 0 || length >= sizeof(setting) || _stricmp(setting, "auto") != 0)
        return;

    /* NOTE: the path must be a double-backslash C literal; with single
     * backslashes the escapes are invalid and the key silently turns into
     * "SoftwareMicrosoftWindows...", which reads as ERROR_FILE_NOT_FOUND. */
    if (RegGetValueA(HKEY_CURRENT_USER,
                     "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     "AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size) != ERROR_SUCCESS)
        return;

    SetEnvironmentVariableA("AGB_COLOR_SCHEME", light ? "light" : "dark");
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

    /* Exception filter first: it only fires when no debugger is attached, which
     * is exactly the case where these crashes happen. */
    SetUnhandledExceptionFilter(shellCrashFilter);
    AddVectoredExceptionHandler(1, shellVehHandler);

    /* AGB_TEST_CRASH=1 proves the filter and the symbolize round-trip still
     * work (the handler only matters when no debugger is attached). */
    if (getenv("AGB_TEST_CRASH"))
        *(volatile int*)0 = 1;

    atexit(shellExitTrace);
    g_instance = instance;

    applySystemColorScheme();

    /* Headless first: no window, no toolbar, no session/bookmark files. */
    parseShellArgv();

    if (g_showUsage) {
        probeUsage();
        return 0;
    }

    if (g_argError)
        return PROBE_EXIT_USAGE;

    /* One line that answers "what was I started with": the options that succeed
     * used to leave no trace at all, so a probe run at --zoom 150 or a GUI run
     * with --userdata looked identical to a default start. */
    agbLog(AGB_LOG_SHELL, "argv url=%s probe=%d zoom=%d resize=%dx%d offline=%d eval=%d bytes keys=%d",
           g_startupUrl[0] ? g_startupUrl : "(none)",
           g_probeMode,
           g_probeZoom,
           g_probeResizeW, g_probeResizeH,
           getenv("AGB_OFFLINE") != NULL,
           (int)strlen(g_probeScript),
           g_probeKeys[0] ? 1 : 0);

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
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             1100, 800, NULL, NULL, instance, NULL);

    if (!g_hwnd) {
        agbLog(AGB_LOG_SHELL, "CreateWindowEx failed: %lu", (unsigned long)GetLastError());
        return 1;
    }

    agbLog(AGB_LOG_SHELL, "window ready hwnd=%p", (void*)g_hwnd);

    shellStateInit();

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
        agbLog(AGB_LOG_SHELL, "startup url from argv: %s", initial);
    } else {
        restored = loadSession(session, 8);
        strcpy(initial, restored > 0 ? session[0] : "http://www.163.com");
        agbLog(AGB_LOG_SHELL, "session restored tabs=%d first=%s", restored, initial);
    }

    if (restored > 1) {
        int i;

        for (i = 0; i < restored; ++i)
            createTab(session[i]);

        setActiveTab(g_activeTab);
    } else {
        createTab(initial);
    }

    agbLog(AGB_LOG_SHELL, "entering message loop");

    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.hwnd == g_address && msg.wParam == VK_RETURN) {
                char text[MAX_PATH_CHARS];

                readControlText(g_address, text, sizeof(text));
                navigateActive(text, 0, "enter");
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
                if (msg.wParam == 'J' && !shift) { navigateActive("agave://downloads", 1, "ctrl+j"); continue; }
                if (msg.wParam == 'F' && !shift) { toggleFind(); continue; }
                if (msg.wParam == 'T') {
                    /* ctrl+shift+T reopens the last closed tab, ctrl+T a new one. */
                    if (shift) reopenClosedTab();
                    else navigateActive("about:blank", 1, "ctrl+t");
                    continue;
                }
                if (msg.wParam == 'W' && !shift) { closeTab(g_activeTab); continue; }
                if (msg.wParam == 'R' && !shift) { reloadActive("ctrl+r"); continue; }
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
                    setZoom(g_zoom + 25, "ctrl+plus");
                    continue;
                }
                if (msg.wParam == VK_SUBTRACT || msg.wParam == 0xBD) {
                    setZoom(g_zoom - 25, "ctrl+minus");
                    continue;
                }
                if (msg.wParam == '0') {
                    setZoom(100, "ctrl+0");
                    continue;
                }
            }

            if (msg.wParam == VK_F5 && g_tabs[g_activeTab].used) {
                reloadActive("F5");
                continue;
            }

            /* alt+left / alt+right: back and forward. */
            if ((GetKeyState(VK_MENU) & 0x8000) && g_tabs[g_activeTab].used) {
                if (msg.wParam == VK_LEFT) { backActive("alt+left"); continue; }
                if (msg.wParam == VK_RIGHT) { forwardActive("alt+right"); continue; }
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    agbLog(AGB_LOG_SHELL, "message loop ended");

    return 0;
}
