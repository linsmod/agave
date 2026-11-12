/*
 * net_probe_curl.exe - one GET through the bundled libcurl (what
 * OPT_NET_BACKEND=curl uses), independent of the browser. Double-click it
 * (result in a message box) or run it from a console:
 *
 *   net_probe_curl.exe                        # https://example.com/
 *   net_probe_curl.exe https://host/path
 *
 * Pair of net_probe_winhttp.exe: same request, different stack. On failure the
 * libcurl trace is included, which is where proxy / TLS / HTTP2 differences
 * between the two stacks show up.
 *
 * Set NET_PROBE_CONSOLE=1 to force console output even when started from a GUI
 * (a GUI-subsystem child of pwsh has no console of its own).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <curl/curl.h>

static void say(const wchar_t *title, const wchar_t *text)
{
    if (GetConsoleWindow() || GetEnvironmentVariableW(L"NET_PROBE_CONSOLE", NULL, 0)) {
        wprintf(L"%ls\n", text);
        fflush(stdout);
    } else {
        MessageBoxW(NULL, text, title, MB_ICONINFORMATION);
    }
}

static size_t collect(char *data, size_t size, size_t count, void *user)
{
    (void)data;
    *(unsigned long long *)user += (unsigned long long)(size * count);
    return size * count;
}

typedef struct {
    char text[16384];
    size_t used;
} trace_t;

static int trace_cb(CURL *handle, curl_infotype type, char *data, size_t size, void *user)
{
    (void)handle;
    (void)type;
    trace_t *trace = (trace_t *)user;
    if (!trace || !data || !size)
        return 0;
    if (trace->used + size + 1 >= sizeof(trace->text))
        size = sizeof(trace->text) - trace->used - 1;
    memcpy(trace->text + trace->used, data, size);
    trace->used += size;
    trace->text[trace->used] = 0;
    return 0;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmdline, int show)
{
    (void)instance;
    (void)previous;
    (void)cmdline;
    (void)show;

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *wurl = (argv && argc > 1) ? argv[1] : L"https://example.com/";
    char url[2048];
    WideCharToMultiByte(CP_UTF8, 0, wurl, -1, url, sizeof(url), NULL, NULL);

    ULONGLONG start = GetTickCount64();
    CURLcode code = CURLE_OK;
    long status = 0;
    unsigned long long bytes = 0;
    trace_t trace;
    ZeroMemory(&trace, sizeof(trace));

    CURL *curl = curl_easy_init();
    if (!curl) {
        code = CURLE_FAILED_INIT;
    } else {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "agave-net-probe/1.0");
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, trace_cb);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &trace);
        code = curl_easy_perform(curl);
        if (code == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();

    ULONGLONG elapsed = GetTickCount64() - start;
    wchar_t text[12288];
    if (code != CURLE_OK) {
        char tail[4000];
        size_t take = trace.used > sizeof(tail) - 1 ? sizeof(tail) - 1 : trace.used;
        memcpy(tail, trace.text + (trace.used - take), take);
        tail[take] = 0;
        wchar_t wtrace[4096];
        MultiByteToWideChar(CP_UTF8, 0, tail, -1, wtrace, 4096);
        _snwprintf(text, 12288, L"curl failed (%d: %hs) after %llu ms\n%ls\n\n--- trace (tail) ---\n%ls",
                   (int)code, curl_easy_strerror(code), elapsed, wurl, wtrace);
    } else {
        _snwprintf(text, 12288, L"curl OK\nstatus=%ld bytes=%llu time=%llu ms\n%ls",
                   status, bytes, elapsed, wurl);
    }
    text[12287] = 0;
    say(L"net_probe_curl", text);
    return code == CURLE_OK ? 0 : 1;
}
