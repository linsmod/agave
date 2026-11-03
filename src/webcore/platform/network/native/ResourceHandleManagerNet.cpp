/*
 * ResourceHandleManagerNet.cpp - timer driven pump for the native backend.
 */

#include "config.h"

#include "ResourceHandleManagerNet.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "SkipList.h"
#include "HttpCache.h"
#include <vector>

#include "ResourceHandle.h"
#include "ResourceHandleInternal.h"
#include "DeprecatedString.h"

#include <string>

namespace WebCore {

/* set AGB_FONT_LOG=1 to trace the network manager (cache hits, deliveries) */
static bool netLogOn(void)
{
    static bool on = getenv("AGB_FONT_LOG") != 0;
    return on;
}

/* --- file:// support ------------------------------------------------------ */

static void percentDecode(std::string* text)
{
    std::string out;
    out.reserve(text->size());

    for (size_t i = 0; i < text->size(); ++i) {
        if ((*text)[i] == '%' && i + 2 < text->size()) {
            char hex[3] = { (*text)[i + 1], (*text)[i + 2], 0 };
            char* end = 0;
            long value = strtol(hex, &end, 16);

            if (end == hex + 2) {
                out.push_back((char)value);
                i += 2;
                continue;
            }
        }

        out.push_back((*text)[i]);
    }

    text->swap(out);
}

static std::string mimeForLocalFile(const std::string& path)
{
    size_t dot = path.rfind('.');
    std::string ext = (dot == std::string::npos) ? std::string() : path.substr(dot + 1);

    /* lowercase */
    for (size_t i = 0; i < ext.size(); ++i)
        ext[i] = (char)tolower((unsigned char)ext[i]);

    if (ext == "html" || ext == "htm")   return "text/html";
    if (ext == "js" || ext == "mjs")     return "application/javascript";
    if (ext == "css")                    return "text/css";
    if (ext == "json")                   return "application/json";
    if (ext == "png")                    return "image/png";
    if (ext == "jpg" || ext == "jpeg")   return "image/jpeg";
    if (ext == "gif")                    return "image/gif";
    if (ext == "webp")                   return "image/webp";
    if (ext == "svg")                    return "image/svg+xml";
    if (ext == "ico")                    return "image/x-icon";
    if (ext == "bmp")                    return "image/bmp";
    if (ext == "txt")                    return "text/plain";
    if (ext == "xml")                    return "text/xml";
    if (ext == "pdf")                    return "application/pdf";
    if (ext == "wasm")                   return "application/wasm";
    if (ext == "ttf")                    return "font/ttf";
    if (ext == "otf")                    return "font/otf";
    if (ext == "woff")                   return "font/woff";
    if (ext == "woff2")                  return "font/woff2";
    if (ext == "mp3")                    return "audio/mpeg";
    if (ext == "wav")                    return "audio/wav";
    if (ext == "ogg")                    return "audio/ogg";
    if (ext == "mp4")                    return "video/mp4";
    if (ext == "webm")                   return "video/webm";

    return "application/octet-stream";
}

/* Converts a file: URL to a local path (percent decoded, query/fragment
 * stripped). "file:///E:/x/y.html" -> "E:/x/y.html",
 * "file://server/share/x" -> "//server/share/x" (UNC). */
static bool localPathForFileUrl(const char* url, std::string* out)
{
    if (!url || !out || strncasecmp(url, "file:", 5) != 0)
        return false;

    const char* rest = url + 5;

    if (!strncmp(rest, "//", 2))
        rest += 2;
    else
        return false;                    /* malformed: file:/x or file:x */

    std::string path(rest);

    size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos)
        path.resize(cut);

    percentDecode(&path);

    if (path.empty())
        return false;

    if (path[0] == '/' && path.size() > 2 && isalpha((unsigned char)path[1]) && path[2] == ':')
        path.erase(0, 1);                /* "/E:/..." -> "E:/..." */

    *out = path;
    return true;
}

static bool readWholeFile(const std::string& path, std::string* body)
{
    FILE* file = fopen(path.c_str(), "rb");

    if (!file)
        return false;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < 0) {
        fclose(file);
        return false;
    }

    body->resize((size_t)size);

    size_t got = size ? fread(&(*body)[0], 1, (size_t)size, file) : 0;
    fclose(file);

    if (got != (size_t)size) {
        body->clear();
        return false;
    }

    return true;
}

ResourceHandleManager* ResourceHandleManager::sharedInstance()
{
    static ResourceHandleManager* manager = 0;
    if (!manager)
        manager = new ResourceHandleManager();
    return manager;
}

ResourceHandleManager::ResourceHandleManager()
    : m_session(Net::Session::create())
    , m_downloadTimer(this, &ResourceHandleManager::downloadTimerCallback)
{
}

ResourceHandleManager::~ResourceHandleManager()
{
    m_downloadTimer.stop();
    if (m_session) {
        m_session->shutdown();
        delete m_session;
        m_session = 0;
    }
}

void ResourceHandleManager::ensureDownloadTimerArmed()
{
    /* isActive() is true while the timer sits in the ready-to-fire set too
     * (collected but not fired yet); that does not mean it will fire again,
     * so check the heap membership instead. */
    if (!m_downloadTimer.isScheduled())
        m_downloadTimer.startRepeating(0.01);
}

void ResourceHandleManager::add(ResourceHandle* job)
{
    ResourceHandleInternal* d = job->getInternal();

    if (netLogOn())
        fprintf(stderr, "[netmgr] add url=%s\n", d ? d->m_request.url().url().ascii() : "(null d)");

    if (!m_session || !d) {
        job->cancel();
        return;
    }

    /* Blocklist: drop the request before it reaches the network. The failure is
     * reported from the timer so the caller sees it asynchronously, like a real
     * network error. */
    {
        DeprecatedString urlText = d->m_request.url().url();

        if (Net::isSkippedResource(std::string(urlText.ascii()))) {
            if (netLogOn())
                fprintf(stderr, "[netmgr] skipped by list\n");
            job->ref();
            m_skipped.append(job);

            ensureDownloadTimerArmed();
            return;
        }
    }

    /* file:// never reaches the network backend: serve it from disk through the
     * same asynchronous path the cache hits use (timer delivered, so the loader
     * keeps its async contract). */
    {
        DeprecatedString urlText = d->m_request.url().url();
        std::string path;

        if (localPathForFileUrl(urlText.ascii(), &path)) {
            if (netLogOn())
                fprintf(stderr, "[netmgr] local file path\n");
            Net::HttpCache::Entry entry;
            std::string body;
            bool ok = readWholeFile(path, &body);

            entry.response.status = ok ? 200 : 404;
            entry.response.statusText = ok ? "OK" : "Not Found";
            entry.response.headers.push_back(std::make_pair(std::string("Content-Type"),
                                                            mimeForLocalFile(path)));
            entry.body.swap(body);

            job->ref();

            CachedDelivery delivery;
            delivery.handle = job;
            delivery.entry = entry;
            m_cached.push_back(delivery);

            ensureDownloadTimerArmed();
            return;
        }
    }

    {
        Net::Request req;

        {
            DeprecatedString u = d->m_request.url().url();
            req.url = std::string(u.ascii());
        }
        req.method = std::string(d->m_request.httpMethod().utf8().data());
        if (req.method.empty())
            req.method = "GET";
        req.timeoutMs = Net::netDefaultTimeoutMs();

        const HTTPHeaderMap& headers = d->m_request.httpHeaderFields();
        HTTPHeaderMap::const_iterator it = headers.begin();
        for (; it != headers.end(); ++it)
            req.headers.push_back(std::make_pair(std::string(it->first.utf8().data()),
                                                 std::string(it->second.utf8().data())));

        /* Cache first: a repeated load of a big module graph would otherwise be
         * hundreds of round trips again. The hit is delivered from the timer so
         * the loader still sees an asynchronous response. */
        {
            Net::HttpCache::Entry cached;

            if (Net::HttpCache::instance().lookup(req, &cached)) {
                if (netLogOn())
                    fprintf(stderr, "[netmgr] cache hit status=%d body=%u\n", cached.response.status, (unsigned)cached.body.size());
                CachedDelivery delivery;

                delivery.handle = job;
                delivery.entry = cached;
                m_cached.push_back(delivery);

                job->ref();

                ensureDownloadTimerArmed();
                return;
            }
        }

        if (netLogOn())
            fprintf(stderr, "[netmgr] session start url=%s\n", req.url.c_str());

        d->m_requestId = m_session->start(req, d);
        if (d->m_requestId == 0) {
            /* report asynchronously to match the loader contract */
            Net::Error err;
            err.code = -1;
            err.text = "could not start request";
            d->onError(err);
            return;
        }
    }

    ensureDownloadTimerArmed();
}

void ResourceHandleManager::cancel(ResourceHandle* job)
{
    ResourceHandleInternal* d = job->getInternal();

    if (!d)
        return;

    d->m_cancelled = true;

    if (m_session && d->m_requestId)
        m_session->cancel(d->m_requestId);

}

void ResourceHandleManager::startSyncJob(ResourceHandle* job)
{
    add(job);
}

void ResourceHandleManager::addCacheLoading(ResourceHandle* job)
{
    /* no disk cache integration yet: fall through to a normal load */
    add(job);
}

void ResourceHandleManager::releaseAllResourceJobs()
{
    ResourceHandleManager* manager = sharedInstance();
    manager->m_downloadTimer.stop();

    /* every live handle cancels itself; force the session to drop what is left */
    if (manager->m_session)
        manager->m_session->shutdown();
}

void ResourceHandleManager::pump()
{
    if (m_session)
        m_session->pump();
}

void ResourceHandleManager::downloadTimerCallback(Timer<ResourceHandleManager>*)
{
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    if (netLogOn())
        fprintf(stderr, "[netmgr] tick cached=%u skipped=%u pending=%d\n",
                (unsigned)m_cached.size(), (unsigned)m_skipped.size(), m_session ? m_session->pending() : -1);
    /* Cached responses are delivered first, like completed network jobs. */
    if (!m_cached.empty()) {
        std::vector<CachedDelivery> hits;

        hits.swap(m_cached);

        for (size_t i = 0; i < hits.size(); ++i) {
            ResourceHandleInternal* d = hits[i].handle->getInternal();

            if (d && !d->m_cancelled) {
                d->onResponse(hits[i].entry.response);

                if (!hits[i].entry.body.empty())
                    d->onData(hits[i].entry.body.data(), (unsigned int)hits[i].entry.body.size());

                d->onComplete();
            }

            hits[i].handle->deref();
        }
    }

    /* Fail blocklisted requests first, then pump the real session. */
    if (!m_skipped.isEmpty()) {
        Vector<ResourceHandle*> failed;

        failed.swap(m_skipped);

        for (unsigned i = 0; i < failed.size(); ++i) {
            ResourceHandleInternal* d = failed[i]->getInternal();

            if (d) {
                Net::Error error;

                error.code = -2;
                error.text = "skipped by skipres.txt";
                d->onError(error);
            }
            failed[i]->deref();
        }
    }

    pump();
    QueryPerformanceCounter(&t1);
    if (getenv("AGB_PERF_LOG")) {
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (ms > 100.0)
            fprintf(stderr, "[perf] net.pump %.0fms pending=%d\n", ms, m_session ? m_session->pending() : -1);
    }

    /* Stop the repeating timer only when there is really nothing left to do.
     * New work can arrive *while this callback runs*: delivering a document
     * finishes the load, which runs layout, which requests fonts and other
     * sub-resources. Those land in m_cached/m_skipped after the swap above,
     * so stopping the timer here would strand them with no pump left to
     * deliver them and the resource would stay pending forever (a font stuck
     * in "loading" forever is what that looked like). */
    if ((!m_session || m_session->pending() == 0) && m_cached.empty() && m_skipped.isEmpty())
        m_downloadTimer.stop();
}

} /* namespace WebCore */
