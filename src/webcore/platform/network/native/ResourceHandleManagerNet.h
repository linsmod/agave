/*
 * ResourceHandleManagerNet.h - drives the native network session from the
 * engine's main thread (task 3-1).
 */

#ifndef ResourceHandleManagerNet_h
#define ResourceHandleManagerNet_h

#include "Timer.h"
#include "Vector.h"
#include "NetworkSession.h"

namespace WebCore {

class ResourceHandle;

class ResourceHandleManager {
public:
    static ResourceHandleManager* sharedInstance();

    void add(ResourceHandle*);
    void cancel(ResourceHandle*);
    void startSyncJob(ResourceHandle*);
    void addCacheLoading(ResourceHandle*);
    static void releaseAllResourceJobs();

    /* Drains the session queue; called from the download timer. */
    void pump();

    /* Lets ResourceHandle::loadResourceSynchronously spin this thread. */
    void pumpOnce() { pump(); }

private:
    ResourceHandleManager();
    ~ResourceHandleManager();

    void downloadTimerCallback(Timer<ResourceHandleManager>*);

    Net::Session*                m_session;
    Timer<ResourceHandleManager> m_downloadTimer;
    Vector<ResourceHandle*>      m_handles;
};

} /* namespace WebCore */

#endif /* ResourceHandleManagerNet_h */
