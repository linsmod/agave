/*
 * ResourceHandleNet.cpp - native backend implementation of the loader seam
 * (task 3-1). Mirrors the curl backend's responsibilities.
 */

#include "config.h"

#include "ResourceHandle.h"
#include "ResourceHandleClient.h"
#include "ResourceHandleInternal.h"
#include "ResourceHandleManagerNet.h"

#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SharedBuffer.h"
#include "CString.h"
#include "KURL.h"
#include "PlatformString.h"

#include <stdlib.h>
#include <string>

#if PLATFORM(WIN32)
#include <windows.h>
#endif

namespace WebCore {

namespace {

std::string toUtf8(const String& s)
{
    return std::string(s.utf8().data());
}

std::string urlToUtf8(const KURL& url)
{
    DeprecatedString s = url.url();
    return std::string(s.ascii());
}

String mimeTypeOf(const Net::Response& response)
{
    std::string contentType = response.header("Content-Type");
    size_t semi = contentType.find(';');
    if (semi != std::string::npos)
        contentType = contentType.substr(0, semi);

    size_t first = contentType.find_first_not_of(" ");
    if (first == std::string::npos)
        return String();
    size_t last = contentType.find_last_not_of(" ");
    return String::fromUTF8(contentType.substr(first, last - first + 1).c_str());
}

long long contentLengthOf(const Net::Response& response)
{
    std::string len = response.header("Content-Length");
    if (len.empty())
        return -1;
    return atoll(len.c_str());
}

void fillResponse(ResourceResponse& out, const KURL& url, const Net::Response& in)
{
    out.setUrl(url);
    out.setHTTPStatusCode(in.status);
    out.setMimeType(mimeTypeOf(in));
    out.setExpectedContentLength(contentLengthOf(in));

    for (size_t i = 0; i < in.headers.size(); ++i) {
        out.setHTTPHeaderField(String::fromUTF8(in.headers[i].first.c_str()),
                               String::fromUTF8(in.headers[i].second.c_str()));
    }
}

void buildRequest(Net::Request& req, const ResourceRequest& request)
{
    req.url    = urlToUtf8(request.url());
    req.method = toUtf8(request.httpMethod());
    if (req.method.empty())
        req.method = "GET";
    req.timeoutMs = Net::netDefaultTimeoutMs();

    const HTTPHeaderMap& headers = request.httpHeaderFields();
    HTTPHeaderMap::const_iterator it = headers.begin();
    for (; it != headers.end(); ++it)
        req.headers.push_back(std::make_pair(toUtf8(it->first), toUtf8(it->second)));
}

/* Collects a synchronous load. */
class SyncCollector : public Net::Client {
public:
    SyncCollector(ResourceResponse& response, ResourceError& error, Vector<char>& data)
        : m_error(error), m_data(data)
        , m_done(false), m_failed(false), m_status(0)
    {
    }

    virtual void onResponse(const Net::Response& r)
    {
        m_status = r.status;
        m_netResponse = r;
    }

    virtual void onData(const char* data, unsigned int length)
    {
        m_data.append(data, (int)length);
    }

    virtual void onComplete()
    {
        m_done = true;
    }

    virtual void onError(const Net::Error& e)
    {
        m_done = true;
        m_failed = true;
        m_error = ResourceError(String("net"), e.code, String(),
                                String::fromUTF8(e.text.c_str()));
    }

    bool                 done() const { return m_done; }
    bool                 failed() const { return m_failed; }
    const Net::Response& netResponse() const { return m_netResponse; }

private:

    ResourceError&    m_error;
    Vector<char>&     m_data;
    Net::Response     m_netResponse;
    bool              m_done;
    bool              m_failed;
    int               m_status;
};

} /* namespace */

/* ------------------------------------------------------------------------- */
/* ResourceHandleInternal: Net::Client                                        */
/* ------------------------------------------------------------------------- */

void ResourceHandleInternal::onResponse(const Net::Response& netResponse)
{
    if (m_cancelled || !m_client)
        return;

    ResourceResponse response;
    fillResponse(response, m_request.url(), netResponse);

    m_hasResponse = true;
    m_response = response;
    m_client->didReceiveResponse(m_handle, response);
}

void ResourceHandleInternal::onData(const char* data, unsigned int length)
{
    if (m_cancelled || !m_client || !length)
        return;

    /* The last argument is the number of bytes received *so far*, not the size
     * of this chunk: WebCore sizes its buffers from it. */
    m_totalReceived += (int)length;
    m_client->didReceiveData(m_handle, data, (int)length, m_totalReceived);
}

void ResourceHandleInternal::onComplete()
{
    if (m_cancelled)
        return;

    m_finished = true;

    if (m_client)
        m_client->didFinishLoading(m_handle);
}

void ResourceHandleInternal::onError(const Net::Error& error)
{
    if (m_cancelled)
        return;

    m_finished = true;

    if (!m_client)
        return;

    ResourceError resourceError(String("net"), error.code,
                                String(m_request.url().url()),
                                String::fromUTF8(error.text.c_str()));
    m_client->didFail(m_handle, resourceError);
}

/* ------------------------------------------------------------------------- */
/* ResourceHandle                                                             */
/* ------------------------------------------------------------------------- */

ResourceHandle::~ResourceHandle()
{
    /* The loader may destroy a handle while its request is still in flight; the
     * session must stop calling back into it. */
    if (ResourceHandleManager* manager = ResourceHandleManager::sharedInstance())
        manager->cancel(this);
}

bool ResourceHandle::start()
{
    ResourceHandleManager::sharedInstance()->add(this);
    return true;
}

void ResourceHandle::cancel()
{
    ResourceHandleManager::sharedInstance()->cancel(this);
}

PassRefPtr<SharedBuffer> ResourceHandle::bufferedData()
{
    return 0;
}

bool ResourceHandle::supportsBufferedData()
{
    return false;
}

void ResourceHandle::setDefersLoading(bool defers)
{
    d->m_defersLoading = defers;
}

bool ResourceHandle::willLoadFromCache(ResourceRequest&)
{
    return false;
}

bool ResourceHandle::loadsBlocked()
{
    return false;
}

void ResourceHandle::loadResourceSynchronously(const ResourceRequest& request,
                                               ResourceError& error,
                                               ResourceResponse& response,
                                               Vector<char>& data)
{
    Net::Session* session = Net::Session::create();

    if (!session) {
        error = ResourceError(String("net"), -1, String(request.url().url()), String("no network session"));
        return;
    }

    {
        Net::Request req;
        buildRequest(req, request);

        SyncCollector collector(response, error, data);
        long long id = session->start(req, &collector);

        if (!id) {
            error = ResourceError(String("net"), -1, String(request.url().url()),
                                  String("could not start request"));
            session->shutdown();
            delete session;
            return;
        }

        while (!collector.done()) {
            session->pump();
            if (!collector.done())
                Sleep(5);
        }

        if (!collector.failed()) {
            fillResponse(response, request.url(), collector.netResponse());
        }
    }

    session->shutdown();
    delete session;
}

} /* namespace WebCore */
