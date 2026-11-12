/*
 * net_probe_winhttp.exe - one GET through WinHTTP (what OPT_NET_BACKEND=native
 * uses), independent of the browser. Double-click it (result in a message box)
 * or run it from a console:
 *
 *   net_probe_winhttp.exe                     # https://example.com/
 *   net_probe_winhttp.exe https://host/path
 *
 * This is the pair of net_probe_curl.exe: same request, different stack, so a
 * machine-specific network difference (proxy, TLS, HTTP/2, firewall) shows up
 * without rebuilding the shell.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

static void say(const wchar_t *title, const wchar_t *text)
{
    if (GetConsoleWindow() || GetEnvironmentVariableW(L"NET_PROBE_CONSOLE", NULL, 0)) {
        wprintf(L"%ls\n", text);
        fflush(stdout);
    } else {
        MessageBoxW(NULL, text, title, MB_ICONINFORMATION);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmdline, int show)
{
    (void)instance;
    (void)previous;
    (void)cmdline;
    (void)show;

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *url = (argv && argc > 1) ? argv[1] : L"https://example.com/";

    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    wchar_t extra[1024] = {0};
    URL_COMPONENTS parts;
    ZeroMemory(&parts, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 1024;

    if (!WinHttpCrackUrl(url, 0, 0, &parts)) {
        wchar_t text[512];
        _snwprintf(text, 512, L"WinHttpCrackUrl failed (%lu) for:\n%ls", GetLastError(), url);
        text[511] = 0;
        say(L"net_probe_winhttp", text);
        return 1;
    }

    ULONGLONG start = GetTickCount64();
    DWORD error = 0;
    DWORD status = 0;
    unsigned long long bytes = 0;

    HINTERNET session = WinHttpOpen(L"agave-net-probe/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = GetLastError();
    } else {
        HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
        if (!connection) {
            error = GetLastError();
        } else {
            DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
            HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, NULL,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
            if (!request) {
                error = GetLastError();
            } else {
                if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                    !WinHttpReceiveResponse(request, NULL)) {
                    error = GetLastError();
                } else {
                    DWORD size = sizeof(status);
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                        WINHTTP_NO_HEADER_INDEX);
                    unsigned char buffer[16384];
                    DWORD read = 0;
                    while (WinHttpReadData(request, buffer, sizeof(buffer), &read) && read > 0)
                        bytes += read;
                }
                WinHttpCloseHandle(request);
            }
            WinHttpCloseHandle(connection);
        }
        WinHttpCloseHandle(session);
    }

    ULONGLONG elapsed = GetTickCount64() - start;
    wchar_t text[1024];
    if (error)
        _snwprintf(text, 1024, L"WinHTTP failed (error %lu) after %llu ms\n%ls", error, elapsed, url);
    else
        _snwprintf(text, 1024, L"WinHTTP OK\nstatus=%lu bytes=%llu time=%llu ms\n%ls",
                   status, bytes, elapsed, url);
    text[1023] = 0;
    say(L"net_probe_winhttp", text);
    return error ? 1 : 0;
}

