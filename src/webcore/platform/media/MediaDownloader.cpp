/*
 * Copyright (C) 2024 Zhang Ji Peng (onecoolx@gmail.com)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "MediaDownloader.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#if PLATFORM(WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace WebCore {
namespace MediaDownloader {

#if PLATFORM(WIN32)

namespace {

struct Entry {
    std::string url;
    std::string path;       /* the data file (grows while downloading) */
    std::string markerPath; /* <path>.complete - exists only when whole */
    HANDLE thread;
    FILE* file;
    volatile LONG stopRequested;
    volatile LONG finished;
    volatile LONG failed;
    unsigned long long bytes;
    unsigned long long total;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cond;
};

/* Only touched on the engine thread; the worker only reads its own Entry and
 * takes Entry::lock to publish counters. */
static std::vector<Entry*>* g_entries = 0;

static Entry* findEntry(const std::string& url)
{
    if (!g_entries)
        return 0;

    for (size_t i = 0; i < g_entries->size(); ++i) {
        if ((*g_entries)[i]->url == url)
            return (*g_entries)[i];
    }

    return 0;
}

/* "https://host:port/path?query" -> the pieces WinHTTP wants. */
struct UrlParts {    bool secure;
    std::wstring host;
    INTERNET_PORT port;
    std::wstring target;
};

static std::wstring widen(const char* s, size_t n)
{
    std::wstring out;
    int needed = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, 0, 0);

    if (needed <= 0)
        return out;

    out.resize((size_t)needed);
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, &out[0], needed);
    return out;
}

static bool parseUrl(const std::string& url, UrlParts& parts)
{
    size_t schemeEnd = url.find("://");

    if (schemeEnd == std::string::npos)
        return false;

    std::string scheme = url.substr(0, schemeEnd);

    parts.secure = (scheme == "https" || scheme == "wss");

    if (scheme != "http" && scheme != "https")
        return false;

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find_first_of("/?", hostStart);
    std::string authority = url.substr(hostStart, pathStart == std::string::npos
                                                     ? std::string::npos
                                                     : pathStart - hostStart);

    {
        std::string host = authority;
        std::string portText;
        size_t colon = authority.rfind(':');

        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            portText = authority.substr(colon + 1);
        }

        host.erase(0, host.find_first_not_of('@') == std::string::npos ? 0 : 0);

        parts.host = widen(host.c_str(), host.size());
        parts.port = portText.empty() ? (INTERNET_PORT)(parts.secure ? INTERNET_DEFAULT_HTTPS_PORT
                                                                     : INTERNET_DEFAULT_HTTP_PORT)
                                      : (INTERNET_PORT)atoi(portText.c_str());
    }

    parts.target = widen(pathStart == std::string::npos ? "/" : url.c_str() + pathStart,
                         pathStart == std::string::npos ? 1 : url.size() - pathStart);

    return !parts.host.empty();
}

static void publish(Entry* entry)
{
    EnterCriticalSection(&entry->lock);
    WakeAllConditionVariable(&entry->cond);
    LeaveCriticalSection(&entry->lock);
}

static DWORD WINAPI downloadThread(void* param)
{
    Entry* entry = (Entry*)param;
    UrlParts parts;

    if (!parseUrl(entry->url, parts)) {
        entry->failed = 1;
        entry->finished = 1;
        publish(entry);
        return 0;
    }

    {
        HINTERNET session = WinHttpOpen(L"agave/1.0 (media)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

        if (!session) {
            entry->failed = 1;
            entry->finished = 1;
            publish(entry);
            return 0;
        }

        {
            HINTERNET connection = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);

            if (connection) {
                HINTERNET request = WinHttpOpenRequest(connection, L"GET", parts.target.c_str(),
                                                       NULL, WINHTTP_NO_REFERER,
                                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       parts.secure ? WINHTTP_FLAG_SECURE : 0);

                if (request) {
                    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;

                    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

                    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                            && WinHttpReceiveResponse(request, NULL)) {

                        {
                            DWORD status = 0;
                            DWORD size = sizeof(status);

                            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                                WINHTTP_NO_HEADER_INDEX);

                            if (status >= 400)
                                entry->failed = 1;
                        }

                        if (!entry->failed) {
                            DWORD length = 0;
                            DWORD lengthSize = sizeof(length);

                            if (WinHttpQueryHeaders(request,
                                                    WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                                    WINHTTP_HEADER_NAME_BY_INDEX, &length, &lengthSize,
                                                    WINHTTP_NO_HEADER_INDEX)) {
                                EnterCriticalSection(&entry->lock);
                                entry->total = length;
                                LeaveCriticalSection(&entry->lock);
                                publish(entry);
                            }
                        }

                        while (!entry->failed && !entry->stopRequested) {
                            DWORD available = 0;

                            if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
                                break;

                            {
                                std::vector<char> chunk(available);
                                DWORD read = 0;

                                if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0)
                                    break;

                                if (entry->file) {
                                    fwrite(&chunk[0], 1, read, entry->file);
                                    fflush(entry->file);
                                }

                                EnterCriticalSection(&entry->lock);
                                entry->bytes += read;
                                LeaveCriticalSection(&entry->lock);
                                publish(entry);
                            }
                        }
                    } else {
                        entry->failed = 1;
                    }

                    WinHttpCloseHandle(request);
                } else {
                    entry->failed = 1;
                }

                WinHttpCloseHandle(connection);
            } else {
                entry->failed = 1;
            }
        }

        WinHttpCloseHandle(session);
    }

    if (entry->file) {
        fclose(entry->file);
        entry->file = 0;
    }

    if (!entry->failed && !entry->stopRequested) {
        /* Complete. The data file cannot be renamed (ffmpeg keeps it open
         * while it decodes), so completion is a separate marker file: "the
         * marker exists" means "the body is all there". */
        {
            FILE* marker = fopen(entry->markerPath.c_str(), "wb");

            if (marker) {
                fclose(marker);
                entry->finished = 1;
            } else {
                entry->failed = 1;
            }
        }
    }

    entry->finished = 1;
    publish(entry);

    if (getenv("AGB_MEDIA_DEBUG"))
        fprintf(stderr, "[media] download done: bytes=%llu total=%llu failed=%d url=%s\n",
                (unsigned long long)entry->bytes, (unsigned long long)entry->total,
                (int)entry->failed, entry->url.c_str());
    return 0;
}

} // namespace

/* Cache budget. Downloads are kept for reuse (a reload of the same page costs
 * no traffic); the directory is capped and the oldest completed copies are
 * evicted when it grows past the limit. AGB_MEDIA_CACHE_MAX is in megabytes. */
static unsigned long long cacheBudgetBytes()
{
    const char* env = getenv("AGB_MEDIA_CACHE_MAX");
    unsigned long long megabytes = 512;

    if (env && *env) {
        long long value = atoll(env);

        if (value > 0)
            megabytes = (unsigned long long)value;
    }

    return megabytes * 1024ULL * 1024ULL;
}

static void pruneCache(const std::string& path)
{
    std::string dir = path;
    size_t slash = dir.find_last_of("\\/");
    struct Item {
        std::string data;
        FILETIME when;
        unsigned long long size;
    };
    std::vector<Item> items;
    unsigned long long total = 0;

    if (slash == std::string::npos)
        return;

    dir.resize(slash);

    {
        WIN32_FIND_DATAA found;
        std::string pattern = dir + "\\*.complete";
        HANDLE handle = FindFirstFileA(pattern.c_str(), &found);

        if (handle == INVALID_HANDLE_VALUE)
            return;

        do {
            std::string data = dir + "\\" + found.cFileName;
            WIN32_FILE_ATTRIBUTE_DATA info;

            /* "<name>.complete" -> "<name>" */
            data.resize(data.size() - 9);

            if (!GetFileAttributesExA(data.c_str(), GetFileExInfoStandard, &info))
                continue;

            {
                Item item;

                item.data = data;
                item.when = info.ftLastWriteTime;
                item.size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                total += item.size;
                items.push_back(item);
            }
        } while (FindNextFileA(handle, &found));

        FindClose(handle);
    }

    {
        unsigned long long budget = cacheBudgetBytes();
        unsigned long long target = budget / 2;

        if (total <= budget)
            return;

        /* Oldest first (tiny list, a selection sort is fine). */
        for (size_t i = 0; i < items.size(); ++i) {
            for (size_t j = i + 1; j < items.size(); ++j) {
                if (CompareFileTime(&items[j].when, &items[i].when) < 0) {
                    Item swap = items[i];
                    items[i] = items[j];
                    items[j] = swap;
                }
            }
        }

        for (size_t i = 0; i < items.size() && total > target; ++i) {
            /* Never evict a resource that is being downloaded right now. */
            bool active = false;

            for (size_t k = 0; g_entries && k < g_entries->size(); ++k) {
                if ((*g_entries)[k]->path == items[i].data)
                    active = true;
            }

            if (active)
                continue;

            DeleteFileA(items[i].data.c_str());
            DeleteFileA((items[i].data + ".complete").c_str());
            total -= items[i].size;

            if (getenv("AGB_MEDIA_DEBUG"))
                fprintf(stderr, "[media] cache evict: %s (%llu bytes)\n",
                        items[i].data.c_str(), items[i].size);
        }
    }
}

bool ensure(const std::string& url, const std::string& path)
{
    Entry* entry = findEntry(url);

    if (entry)
        return true;

    {
        Entry* created = new Entry();

        pruneCache(path);

        created->url = url;
        created->path = path;
        created->markerPath = path + ".complete";
        created->thread = NULL;
        created->file = 0;
        created->stopRequested = 0;
        created->finished = 0;
        created->failed = 0;
        created->bytes = 0;
        created->total = 0;

        InitializeCriticalSection(&created->lock);
        InitializeConditionVariable(&created->cond);

        /* Completed by an earlier run: the marker says the body is whole. */
        if (GetFileAttributesA(created->markerPath.c_str()) != INVALID_FILE_ATTRIBUTES
                && GetFileAttributesA(created->path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            created->finished = 1;
        } else {
            /* A previous run may have left a partial file; start over. */
            DeleteFileA(created->markerPath.c_str());
            created->file = fopen(created->path.c_str(), "wb");

            if (!created->file) {
                created->failed = 1;
                created->finished = 1;
            } else {
                created->thread = CreateThread(NULL, 0, downloadThread, created, 0, NULL);
            }
        }

        if (!g_entries)
            g_entries = new std::vector<Entry*>();
        g_entries->push_back(created);
    }

    return true;
}

unsigned long long bytes(const std::string& url)
{
    Entry* entry = findEntry(url);

    if (!entry)
        return 0;

    EnterCriticalSection(&entry->lock);
    unsigned long long value = entry->bytes;
    LeaveCriticalSection(&entry->lock);
    return value;
}

unsigned long long total(const std::string& url)
{
    Entry* entry = findEntry(url);

    return entry ? entry->total : 0;
}

bool finished(const std::string& url)
{
    Entry* entry = findEntry(url);

    return entry ? entry->finished != 0 : true;
}

bool failed(const std::string& url)
{
    Entry* entry = findEntry(url);

    return entry ? entry->failed != 0 : false;
}

bool waitForBytes(const std::string& url, unsigned long long wanted, int timeoutMs)
{
    Entry* entry = findEntry(url);

    if (!entry)
        return false;

    EnterCriticalSection(&entry->lock);

    {
        DWORD deadline = GetTickCount() + (DWORD)(timeoutMs > 0 ? timeoutMs : 0);
        bool reached = false;

        while (entry->bytes < wanted && !entry->finished) {
            DWORD now = GetTickCount();

            if ((int)(deadline - now) <= 0)
                break;

            SleepConditionVariableCS(&entry->cond, &entry->lock, (DWORD)(deadline - now));
        }

        reached = entry->bytes >= wanted;
        LeaveCriticalSection(&entry->lock);
        return reached;
    }
}

void stop(const std::string& url)
{
    Entry* entry = findEntry(url);

    if (entry)
        entry->stopRequested = 1;
}

#else /* !PLATFORM(WIN32) */

bool ensure(const std::string&, const std::string&) { return false; }
unsigned long long bytes(const std::string&) { return 0; }
unsigned long long total(const std::string&) { return 0; }
bool finished(const std::string&) { return true; }
bool failed(const std::string&) { return false; }
bool waitForBytes(const std::string&, unsigned long long, int) { return false; }
void stop(const std::string&) { }

#endif

} // namespace MediaDownloader
} // namespace WebCore
