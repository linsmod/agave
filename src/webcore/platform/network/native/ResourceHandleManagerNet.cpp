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

void ResourceHandleManager::add(ResourceHandle* job)
{
    ResourceHandleInternal* d = job->getInternal();

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
            job->ref();
            m_skipped.append(job);

            if (!m_downloadTimer.isActive())
                m_downloadTimer.startRepeating(0.01);
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
                CachedDelivery delivery;

                delivery.handle = job;
                delivery.entry = cached;
                m_cached.push_back(delivery);

                job->ref();

                if (!m_downloadTimer.isActive())
                    m_downloadTimer.startRepeating(0.01);
                return;
            }
        }

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

    if (!m_downloadTimer.isActive())
        m_downloadTimer.startRepeating(0.01);
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

    /* Nothing to prune: handles deregister themselves through cancel() (also
     * from their destructor), so the manager never touches a stale pointer. */
    if (!m_session || m_session->pending() == 0)
        m_downloadTimer.stop();
}

} /* namespace WebCore */
