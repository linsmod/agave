/*
 * ResourceHandleManagerNet.cpp - timer driven pump for the native backend.
 */

#include "config.h"

#include "ResourceHandleManagerNet.h"

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

    {
        Net::Request req;

        {
            DeprecatedString u = d->m_request.url().url();
            req.url = std::string(u.ascii());
        }
        req.method = std::string(d->m_request.httpMethod().utf8().data());
        if (req.method.empty())
            req.method = "GET";
        req.timeoutMs = 30000;

        const HTTPHeaderMap& headers = d->m_request.httpHeaderFields();
        HTTPHeaderMap::const_iterator it = headers.begin();
        for (; it != headers.end(); ++it)
            req.headers.push_back(std::make_pair(std::string(it->first.utf8().data()),
                                                 std::string(it->second.utf8().data())));

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

    m_handles.append(job);
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

    for (size_t i = 0; i < m_handles.size(); ++i) {
        if (m_handles[i] == job) {
            m_handles.remove(i);
            break;
        }
    }
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

    Vector<ResourceHandle*> jobs = manager->m_handles;
    manager->m_handles.clear();

    for (size_t i = 0; i < jobs.size(); ++i)
        jobs[i]->cancel();
}

void ResourceHandleManager::pump()
{
    if (m_session)
        m_session->pump();
}

void ResourceHandleManager::downloadTimerCallback(Timer<ResourceHandleManager>*)
{
    pump();

    /* drop finished handles */
    for (size_t i = 0; i < m_handles.size();) {
        ResourceHandleInternal* d = m_handles[i]->getInternal();
        if (!d || d->m_finished || d->m_cancelled)
            m_handles.remove(i);
        else
            ++i;
    }

    if (m_handles.isEmpty())
        m_downloadTimer.stop();
}

} /* namespace WebCore */
