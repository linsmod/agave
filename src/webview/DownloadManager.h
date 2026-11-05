/*
 * DownloadManager.h - download jobs that outlive the page that started them.
 *
 * A top-level navigation the engine cannot display (binary, attachment) is
 * turned into a download by FrameLoaderClientMC::dispatchDecidePolicyForMIMEType.
 * MainResourceLoader then hands the *live* ResourceHandle over
 * (ResourceLoader::detachHandle) and the job below owns the connection from
 * then on: the page can navigate away and the transfer keeps going.
 *
 * Design notes live in docs/download-manager.md.
 */

#ifndef DownloadManager_h
#define DownloadManager_h

#include "ResourceHandleClient.h"
#include "PlatformString.h"
#include "Shared.h"

#include <stdio.h>

#include <wtf/PassRefPtr.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>

namespace WebCore {

class ResourceError;
class ResourceHandle;
class ResourceRequest;
class ResourceResponse;

class DownloadJob : public Shared<DownloadJob>, public ResourceHandleClient {
public:
    DownloadJob(int id);
    virtual ~DownloadJob();

    bool start(PassRefPtr<ResourceHandle>, const ResourceRequest&, const ResourceResponse&);
    void cancel();

    int id() const { return m_id; }
    bool isActive() const { return m_state == StateActive; }

    /* ResourceHandleClient */
    virtual void didReceiveResponse(ResourceHandle*, const ResourceResponse&);
    virtual void didReceiveData(ResourceHandle*, const char*, int, int lengthReceived);
    virtual void didFinishLoading(ResourceHandle*);
    virtual void didFail(ResourceHandle*, const ResourceError&);

private:
    enum State { StateActive, StateFinished, StateFailed, StateCancelled };

    /* Returns false when the host asked to cancel. */
    bool emit(int state, int error);
    void closeFile(bool keep);
    void fail(int errorCode);

    int                  m_id;
    State                m_state;
    bool                 m_cancelling;
    RefPtr<ResourceHandle> m_handle;
    FILE*                m_file;
    String               m_url;
    String               m_mime;
    String               m_path;
    String               m_partPath;
    long long            m_received;
    long long            m_total;
    long long            m_lastEmitBytes;
    unsigned long        m_lastEmitTick;
};

class DownloadManager {
public:
    static DownloadManager& shared();

    /* Directory new files are written to. Defaults to
     * $AGB_USERDATA/downloads, or %LOCALAPPDATA%/Agave/downloads. */
    void setDirectory(const String& directory);
    const String& directory() const { return m_directory; }

    int start(PassRefPtr<ResourceHandle>, const ResourceRequest&, const ResourceResponse&);
    bool cancel(int id);
    int cancelAll();
    int activeCount() const;

private:
    DownloadManager();

    /* Drops finished/cancelled jobs. Never called from inside a job callback:
     * dropping the last reference there would delete the job under its own
     * stack frame. */
    void purgeFinished();

    String                        m_directory;
    int                           m_nextId;
    Vector<RefPtr<DownloadJob> >  m_jobs;
};

} /* namespace WebCore */

#endif /* DownloadManager_h */
