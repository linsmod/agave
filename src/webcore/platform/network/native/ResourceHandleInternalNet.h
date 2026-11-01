/*
 * ResourceHandleInternalNet.h - loader-side state for the native network
 * backend (task 3-1). Mirrors ResourceHandleInternalCurl.h but talks to
 * Net::Session instead of libcurl.
 */

#ifndef ResourceHandleInternalNet_h
#define ResourceHandleInternalNet_h

#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "AuthenticationChallenge.h"
#include "DownloadResourceClient.h"
#include "CString.h"
#include "NetworkSession.h"
#include "NetworkTypes.h"

namespace WebCore {

class ResourceHandle;
class ResourceHandleClient;

class ResourceHandleInternal : public Net::Client, public Noncopyable {
public:
    ResourceHandleInternal(ResourceHandle* loader, const ResourceRequest& request,
                           ResourceHandleClient* c, bool defersLoading,
                           bool shouldContentSniff, bool mightDownloadFromHandle,
                           bool asyncDownload)
        : m_client(c)
        , m_request(request)
        , m_handle(loader)
        , m_requestId(0)
        , m_asyncDownload(asyncDownload)
        , m_defersLoading(defersLoading)
        , m_shouldContentSniff(shouldContentSniff)
        , m_mightDownloadFromHandle(mightDownloadFromHandle)
        , m_cancelled(false)
        , m_finished(false)
        , m_hasResponse(false)
        , m_hasDownloadClient(false)
        , m_forSavingResource(false)
        , m_totalReceived(0)
    {
    }

    virtual ~ResourceHandleInternal() {}

    /* Net::Client */
    virtual void onResponse(const Net::Response& response);
    virtual void onData(const char* data, unsigned int length);
    virtual void onComplete();
    virtual void onError(const Net::Error& error);

    ResourceHandleClient* m_client;
    ResourceRequest       m_request;
    ResourceHandle*       m_handle;
    long long             m_requestId;
    bool                  m_asyncDownload;
    bool                  m_defersLoading;
    bool                  m_shouldContentSniff;
    bool                  m_mightDownloadFromHandle;
    bool                  m_cancelled;
    bool                  m_finished;
    bool                  m_hasResponse;
    bool                  m_hasDownloadClient;
    ResourceResponse      m_response;   /* last received response (download client) */
    AuthenticationChallenge m_currentWebChallenge;
    bool                  m_forSavingResource;
    int                   m_totalReceived;   /* bytes received so far (didReceiveData) */

    bool needsDownloadClient() { return m_mightDownloadFromHandle && !m_hasDownloadClient; }
    bool needsResourceHandleForDownloading() { return m_mightDownloadFromHandle && !m_forSavingResource; }
};

} /* namespace WebCore */

#endif /* ResourceHandleInternalNet_h */
