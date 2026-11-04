/*
 * HttpCache.cpp - persistent HTTP cache for the native network backend.
 *
 * Storage: one file per URL under %LOCALAPPDATA%\Agave\http-cache. The file is a
 * text header followed by the raw body:
 *
 *   AGBHC1
 *   url <url>
 *   status <code>
 *   stored <ms> expiry <ms>
 *   etag <value>
 *   lastmodified <value>
 *   header <name>: <value>
 *   body <length>
 *   <length bytes>
 *
 * Keeping the URL (not just the hash) inside the file means a hash collision is
 * detected and ignored instead of serving the wrong file.
 */

#include "config.h"

#include "HttpCache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#if PLATFORM(WIN32)
#include <windows.h>
#endif

namespace Net {

namespace {

long long nowMs()
{
#if PLATFORM(WIN32)
    FILETIME ft;
    ULARGE_INTEGER v;

    GetSystemTimeAsFileTime(&ft);
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;

    /* 100ns since 1601 -> ms since 1970 */
    return (long long)(v.QuadPart / 10000ULL) - 11644473600000LL;
#else
    return (long long)time(0) * 1000;
#endif
}

std::string cacheDir()
{
#if PLATFORM(WIN32)
    /* $AGB_USERDATA redirects the whole profile root (cookies, localStorage,
     * module cache, HTTP cache). The headless probe sets it so a test run never
     * reads or writes the cache a GUI session uses. */
    const char* userdata = getenv("AGB_USERDATA");

    if (userdata && userdata[0]) {
        std::string dir = std::string(userdata) + "\\http-cache";

        CreateDirectoryA(userdata, NULL);
        CreateDirectoryA(dir.c_str(), NULL);
        return dir;
    }

    const char* base = getenv("LOCALAPPDATA");
    std::string dir = base ? base : ".";

    dir += "\\Agave\\http-cache";
    CreateDirectoryA((std::string(base ? base : ".") + "\\Agave").c_str(), NULL);
    CreateDirectoryA(dir.c_str(), NULL);
    return dir;
#else
    const char* home = getenv("HOME");
    std::string dir = (home ? home : ".");
    dir += "/.agave/http-cache";
    return dir;
#endif
}

unsigned long hashOf(const std::string& text)
{
    unsigned long hash = 2166136261u;

    for (size_t i = 0; i < text.size(); ++i) {
        hash ^= (unsigned char)text[i];
        hash *= 16777619u;
    }

    return hash;
}

/* Cache-Control: max-age=N -> N*1000ms. Returns 0 when absent/invalid. */
long long maxAgeMs(const std::string& cacheControl)
{
    size_t at = cacheControl.find("max-age=");

    if (at == std::string::npos)
        return 0;

    at += 8;

    {
        long seconds = 0;
        bool any = false;

        while (at < cacheControl.size() && cacheControl[at] >= '0' && cacheControl[at] <= '9') {
            seconds = seconds * 10 + (cacheControl[at] - '0');
            any = true;
            ++at;
        }

        return any ? (long long)seconds * 1000 : 0;
    }
}

std::string lower(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] >= 'A' && s[i] <= 'Z')
            s[i] = (char)(s[i] - 'A' + 'a');
    }
    return s;
}

} /* namespace */

HttpCache& HttpCache::instance()
{
    static HttpCache* cache = new HttpCache();
    return *cache;
}

HttpCache::HttpCache()
    : m_mode(1)
    , m_maxMemoryBytes(64u * 1024u * 1024u)
    , m_memoryBytes(0)
{
    const char* mode = getenv("AGB_HTTP_CACHE");
    const char* mb = getenv("AGB_HTTP_CACHE_MB");

    if (mode)
        m_mode = atoi(mode);

    if (mb) {
        int value = atoi(mb);

        if (value > 0)
            m_maxMemoryBytes = (size_t)value * 1024u * 1024u;
    }
}

bool HttpCache::enabled() const
{
    return m_mode > 0;
}

bool HttpCache::forced() const
{
    return m_mode >= 2;
}

std::string HttpCache::fileFor(const std::string& url) const
{
    char name[64];

    snprintf(name, sizeof(name), "%08lx.hc", hashOf(url));
    return cacheDir() + "\\" + name;
}

/* Cacheability is deliberately conservative: only GET + 200 is stored, and never
 * anything that says no-store or carries Set-Cookie (a cookie response may be
 * per-user and must not be replayed). */
bool HttpCache::cacheable(const Request& request, const Response& response) const
{
    std::string cacheControl;

    if (request.method != "GET" && !request.method.empty())
        return false;

    if (response.status != 200)
        return false;

    cacheControl = lower(response.header("cache-control"));

    if (cacheControl.find("no-store") != std::string::npos)
        return false;

    if (!response.header("set-cookie").empty())
        return false;

    return true;
}

long long HttpCache::freshnessMs(const Response& response) const
{
    std::string cacheControl = lower(response.header("cache-control"));
    long long age = maxAgeMs(cacheControl);

    if (age > 0)
        return age;

    /* No usable max-age: the entry is stored but immediately stale. The force
     * mode still serves it; the normal mode revalidates (stage 2). */
    return 0;
}

bool HttpCache::readFromDisk(const std::string& url, Entry* entry)
{
    std::string path = fileFor(url);
    FILE* file = fopen(path.c_str(), "rb");
    char line[4096];
    long long bodyLength = -1;

    if (!file)
        return false;

    if (!fgets(line, sizeof(line), file) || strncmp(line, "AGBHC1", 6) != 0) {
        fclose(file);
        return false;
    }

    entry->response = Response();

    while (fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);

        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;

        if (!strncmp(line, "url ", 4)) {
            if (url != (line + 4)) {
                fclose(file);
                return false;
            }
        } else if (!strncmp(line, "status ", 7)) {
            entry->response.status = atoi(line + 7);
        } else if (!strncmp(line, "stored ", 7)) {
            entry->storedAt = atoll(line + 7);
        } else if (!strncmp(line, "expiry ", 7)) {
            entry->expiresAt = atoll(line + 7);
        } else if (!strncmp(line, "etag ", 5)) {
            entry->etag = line + 5;
        } else if (!strncmp(line, "lastmodified ", 13)) {
            entry->lastModified = line + 13;
        } else if (!strncmp(line, "header ", 7)) {
            const char* value = strstr(line + 7, ": ");

            if (value)
                entry->response.headers.push_back(
                    std::make_pair(std::string(line + 7, value - (line + 7)), std::string(value + 2)));
        } else if (!strncmp(line, "body ", 5)) {
            bodyLength = atoll(line + 5);
            break;
        }
    }

    if (bodyLength < 0) {
        fclose(file);
        return false;
    }

    entry->body.resize((size_t)bodyLength);

    if (bodyLength > 0 && fread(&entry->body[0], 1, (size_t)bodyLength, file) != (size_t)bodyLength) {
        fclose(file);
        return false;
    }

    fclose(file);
    return true;
}

void HttpCache::writeToDisk(const std::string& url, const Entry& entry)
{
    std::string path = fileFor(url);
    std::string temp = path + ".tmp";
    FILE* file = fopen(temp.c_str(), "wb");

    if (!file)
        return;

    fprintf(file, "AGBHC1\n");
    fprintf(file, "url %s\n", url.c_str());
    fprintf(file, "status %d\n", entry.response.status);
    fprintf(file, "stored %lld\n", entry.storedAt);
    fprintf(file, "expiry %lld\n", entry.expiresAt);
    fprintf(file, "etag %s\n", entry.etag.c_str());
    fprintf(file, "lastmodified %s\n", entry.lastModified.c_str());

    for (size_t i = 0; i < entry.response.headers.size(); ++i)
        fprintf(file, "header %s: %s\n", entry.response.headers[i].first.c_str(),
                entry.response.headers[i].second.c_str());

    fprintf(file, "body %u\n", (unsigned)entry.body.size());

    if (!entry.body.empty())
        fwrite(entry.body.data(), 1, entry.body.size(), file);

    fclose(file);

    /* atomic enough: replace the old entry only once the new one is on disk */
    remove(path.c_str());
    rename(temp.c_str(), path.c_str());
}

bool HttpCache::lookup(const Request& request, Entry* entry)
{
    if (!enabled() || !entry || request.method != "GET")
        return false;

    {
        std::map<std::string, Entry>::iterator it = m_memory.find(request.url);

        if (it != m_memory.end()) {
            if (forced() || it->second.expiresAt > nowMs()) {
                *entry = it->second;
                return true;
            }
            return false;
        }
    }

    {
        Entry loaded;

        if (!readFromDisk(request.url, &loaded))
            return false;

        if (!forced() && loaded.expiresAt <= nowMs())
            return false;

        if (m_memoryBytes + loaded.body.size() <= m_maxMemoryBytes) {
            m_memory[request.url] = loaded;
            m_memoryBytes += loaded.body.size();
        }

        *entry = loaded;
        return true;
    }
}

void HttpCache::store(const Request& request, const Response& response, const std::string& body)
{
    Entry entry;

    if (!enabled() || !cacheable(request, response) || body.size() > (16u * 1024u * 1024u))
        return;

    entry.response = response;
    entry.body = body;
    entry.storedAt = nowMs();
    entry.expiresAt = entry.storedAt + freshnessMs(response);
    entry.etag = response.header("etag");
    entry.lastModified = response.header("last-modified");

    {
        std::map<std::string, Entry>::iterator it = m_memory.find(request.url);

        if (it != m_memory.end()) {
            m_memoryBytes -= it->second.body.size();
            m_memory.erase(it);
        }
    }

    if (m_memoryBytes + entry.body.size() <= m_maxMemoryBytes) {
        m_memory[request.url] = entry;
        m_memoryBytes += entry.body.size();
    }

    writeToDisk(request.url, entry);
}

void HttpCache::clear()
{
    m_memory.clear();
    m_memoryBytes = 0;
}

} /* namespace Net */
