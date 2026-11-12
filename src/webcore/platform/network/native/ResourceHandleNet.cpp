/*
 * ResourceHandleNet.cpp - native backend implementation of the loader seam
 * (task 3-1). Mirrors the curl backend's responsibilities.
 */

#include "config.h"

#include "ResourceHandle.h"
#include "ResourceHandleClient.h"
#include "ResourceHandleInternal.h"
#include "ResourceHandleManagerNet.h"
#include "HttpCache.h"
#include "HTTPParsers.h"
#include "NetLog.h"

#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SharedBuffer.h"
#include "CString.h"
#include "KURL.h"
#include "PlatformString.h"
#include "FormData.h"

#if ENABLE(ANTJS)
#include "AntRuntime.h"
#endif

#include <stdlib.h>
#include <string>

#if PLATFORM(WIN32)
#include <windows.h>
#endif

namespace WebCore {

namespace {

/* set AGB_FONT_LOG=1 to trace the resource delivery callbacks */
bool handleLogOn(void)
{
    static bool on = getenv("AGB_FONT_LOG") != 0;
    return on;
}

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

/* The reason phrase for a status code, used when the transport did not provide
 * one. Browsers have always answered XMLHttpRequest.statusText from this table;
 * leaving it empty made every statusText "" where Chrome says "OK". */
const char* defaultStatusText(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a Teapot";
    case 422: return "Unprocessable Entity";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default: break;
    }

    return "";
}

void fillResponse(ResourceResponse& out, const KURL& url, const Net::Response& in)
{
    out.setUrl(url);
    out.setHTTPStatusCode(in.status);
    out.setHTTPStatusText(String::fromUTF8(
        in.statusText.empty() ? defaultStatusText(in.status) : in.statusText.c_str()));
    out.setMimeType(mimeTypeOf(in));
    out.setExpectedContentLength(contentLengthOf(in));
    /* The charset in Content-Type is the transport-layer encoding, and it wins
     * over a <meta charset> inside the document (TextResourceDecoder enforces
     * that priority). The curl backend has always set this; the native backend
     * did not, so the document decoder fell back to Latin-1 and any UTF-8 page
     * whose <meta> was missing from the first bytes was mojibake
     * (www.163.com article pages). */
    out.setTextEncodingName(extractCharsetFromMediaType(
        String::fromUTF8(in.header("Content-Type").c_str())));

    for (size_t i = 0; i < in.headers.size(); ++i) {
        String name = String::fromUTF8(in.headers[i].first.c_str());
        String value = String::fromUTF8(in.headers[i].second.c_str());
        /* Set-Cookie is the one header that legitimately repeats: ResourceResponse
         * stores headers in a map, so a plain setHTTPHeaderField() kept only the
         * last cookie of a multi-cookie response (Baidu sends BAIDUID together
         * with several others from hector.baidu.com). Keep every value, newline
         * separated; the ant cookie jar parses one per line. */
        if (equalIgnoringCase(name, "Set-Cookie")) {
            String existing = out.httpHeaderField(name);
            if (!existing.isEmpty())
                value = existing + "\n" + value;
        }
        out.setHTTPHeaderField(name, value);
    }
#if ENABLE(ANTJS)
    /* The ant cookie jar is what document.cookie reads; feed it every response
     * (the old XHR-only path never saw cookies set by <script>/<img> responses). */
    {
        String setCookie = out.httpHeaderField("Set-Cookie");
        if (!setCookie.isEmpty())
            AntRuntime::addCookiesFromResponse(url.url().utf8().data(),
                                               setCookie.utf8().data());
    }
#endif
}

bool isRedirectStatus(int status)
{
    return status == 301 || status == 302 || status == 303
        || status == 307 || status == 308;
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
    if (handleLogOn())
        fprintf(stderr, "[handlecb] onResponse url=%s cancelled=%d client=%p\n",
                m_request.url().url().ascii(), (int)m_cancelled, (void*)m_client);
    if (m_cancelled || !m_client)
        return;

    /* 3xx: hand the redirect to WebCore instead of following it in WinHTTP.
     * MainResourceLoader::willSendRequest() updates the DocumentLoader (this is
     * where the address bar and the [nav] redirect line come from) and re-checks
     * the navigation policy; then the transfer restarts for the new URL. */
    if (isRedirectStatus(netResponse.status)) {
        std::string location = netResponse.header("Location");

        if (!location.empty()) {
            KURL target(m_request.url(), DeprecatedString::fromUtf8(location.c_str()));

            if (target.isValid()) {
                if (m_redirectCount >= 20) {
                    Net::Error error;

                    error.code = -3;
                    error.text = "too many redirects";
                    onError(error);
                    return;
                }

                m_redirectCount++;

                ResourceResponse redirectResponse;
                fillResponse(redirectResponse, m_request.url(), netResponse);

                ResourceRequest newRequest = m_request;
                newRequest.setURL(target);

                /* 303 always, and 301/302 after a POST, become GET without a
                 * body (307/308 keep the method). */
                if (netResponse.status == 303
                    || ((netResponse.status == 301 || netResponse.status == 302)
                        && newRequest.httpMethod() == "POST")) {
                    newRequest.setHTTPMethod("GET");
                    newRequest.setHTTPBody(PassRefPtr<FormData>());
                }

                if (Net::logEnabled())
                    Net::logLine("net.redirect %d %s -> %s", netResponse.status,
                                 m_request.url().url().ascii(), target.url().ascii());

                m_client->willSendRequest(m_handle, newRequest, redirectResponse);

                /* The policy check may have cancelled the load. */
                if (m_cancelled || !m_client)
                    return;

                m_request = newRequest;
                m_hasResponse = false;
                m_response = ResourceResponse();
                m_netResponse = Net::Response();
                m_body.clear();
                m_totalReceived = 0;

                ResourceHandleManager::sharedInstance()->redirect(m_handle);
                return;
            }
        }
    }

    ResourceResponse response;
    fillResponse(response, m_request.url(), netResponse);

    m_hasResponse = true;
    m_response = response;
    m_netResponse = netResponse;
    m_body.clear();
    m_client->didReceiveResponse(m_handle, response);
}

void ResourceHandleInternal::onData(const char* data, unsigned int length)
{
    if (handleLogOn())
        fprintf(stderr, "[handlecb] onData len=%u cancelled=%d client=%p\n", length, (int)m_cancelled, (void*)m_client);
    if (m_cancelled || !m_client || !length)
        return;

    /* The last argument is the number of bytes received *so far*, not the size
     * of this chunk: WebCore sizes its buffers from it. */
    m_totalReceived += (int)length;

    /* Bodies are kept for the HTTP cache; 16 MB is the same cap the cache uses. */
    if (m_body.size() + length <= 16u * 1024u * 1024u)
        m_body.append(data, length);

    m_client->didReceiveData(m_handle, data, (int)length, m_totalReceived);
}

void ResourceHandleInternal::onComplete()
{
    if (handleLogOn())
        fprintf(stderr, "[handlecb] onComplete cancelled=%d client=%p\n", (int)m_cancelled, (void*)m_client);
    if (m_cancelled)
        return;

    m_finished = true;

    /* Store cacheable responses (GET + 200 without no-store/set-cookie) for the
     * next load; the body was accumulated in onData(). */
    if (m_hasResponse) {
        Net::Request cacheRequest;

        cacheRequest.url = std::string(m_request.url().url().ascii());
        cacheRequest.method = std::string(m_request.httpMethod().utf8().data());

        if (cacheRequest.method.empty())
            cacheRequest.method = "GET";

        Net::HttpCache::instance().store(cacheRequest, m_netResponse, m_body);
    }

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
    if (handleLogOn())
        fprintf(stderr, "[handle] start\n");
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
