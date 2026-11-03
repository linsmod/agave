/*
 * HttpCache.h - small persistent HTTP cache for the native network backend.
 *
 * Why it exists: the module graph of a modern site is a few hundred small files.
 * Through a proxy every one of them costs a full round trip (~150ms here), so
 * load time is 90% network and the engine never gets measured. This cache keeps
 * response bodies on disk, so a repeat load can be served without the network.
 *
 * AGB_HTTP_CACHE:
 *   0  off
 *   1  normal (default): serve only while the entry is fresh by its own
 *      Cache-Control max-age; otherwise fall through to the network.
 *   2  force: serve any stored entry regardless of age and store every 200.
 *      This is the "measure the engine, not the network" mode.
 *
 * AGB_HTTP_CACHE_MB: memory budget for deserialised bodies (default 64).
 */

#ifndef NETWORK_HTTP_CACHE_H
#define NETWORK_HTTP_CACHE_H

#include "NetworkTypes.h"

#include <map>
#include <string>

namespace Net {

class HttpCache {
public:
    struct Entry {
        Response    response;
        std::string body;
        long long   storedAt;      /* ms since epoch */
        long long   expiresAt;     /* ms since epoch; <= storedAt means stale */
        std::string etag;
        std::string lastModified;

        Entry() : storedAt(0), expiresAt(0) {}
    };

    static HttpCache& instance();

    bool enabled() const;
    bool forced() const;

    /* Fresh (or, in force mode, any) stored response for this request. */
    bool lookup(const Request& request, Entry* entry);

    /* Stores a completed response when it is cacheable. */
    void store(const Request& request, const Response& response, const std::string& body);

    void clear();
    size_t entries() const { return m_memory.size(); }
    size_t memoryBytes() const { return m_memoryBytes; }

private:
    HttpCache();

    std::string fileFor(const std::string& url) const;
    bool readFromDisk(const std::string& url, Entry* entry);
    void writeToDisk(const std::string& url, const Entry& entry);
    bool cacheable(const Request& request, const Response& response) const;
    long long freshnessMs(const Response& response) const;

    int    m_mode;
    size_t m_maxMemoryBytes;
    size_t m_memoryBytes;
    std::map<std::string, Entry> m_memory;
};

} /* namespace Net */

#endif /* NETWORK_HTTP_CACHE_H */
