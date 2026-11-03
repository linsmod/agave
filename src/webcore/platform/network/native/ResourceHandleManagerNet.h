/*
 * ResourceHandleManagerNet.h - drives the native network session from the
 * engine's main thread (task 3-1).
 */

#ifndef ResourceHandleManagerNet_h
#define ResourceHandleManagerNet_h

#include "Timer.h"
#include "Vector.h"
#include "NetworkSession.h"
#include "HttpCache.h"
#include <vector>

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

    /* Makes sure the download timer is queued in the timer heap. isActive()
     * is not enough: it is also true while the timer only sits in the
     * ready-to-fire set, and that state does not guarantee another tick. */
    void ensureDownloadTimerArmed();

    Net::Session*                m_session;
    Timer<ResourceHandleManager> m_downloadTimer;
    Vector<ResourceHandle*>      m_handles;
    Vector<ResourceHandle*>      m_skipped;

    /* Handles whose response came from the HTTP cache: delivered from the timer
     * so the loader still sees an asynchronous response. */
    struct CachedDelivery {
        ResourceHandle*        handle;
        Net::HttpCache::Entry  entry;
    };
    std::vector<CachedDelivery>  m_cached;
};

} /* namespace WebCore */

#endif /* ResourceHandleManagerNet_h */
