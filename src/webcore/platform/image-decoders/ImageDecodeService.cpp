/*
 * ImageDecodeService.cpp: decode images off the engine thread.
 *
 * Copyright (C) 2026 agave project
 *
 * See ImageDecodeService.h for the contract and
 * docs/design-async-image-decode.md for why this exists. The pattern mirrors
 * MediaPipeline / WinHttpSession: a worker thread owns the heavy object, the
 * engine thread only queues work and polls results.
 */

#include "config.h"
#include "ImageDecodeService.h"

#include "Image.h"
#include "ImageDecoder.h"
#include <stdlib.h>

namespace WebCore {

/* Declared in ImageSourcePS.cpp: sniff the bytes and build the right decoder. */
ImageDecoder* createDecoder(const Vector<char>& data);

static ImageDecodeService* s_sharedService;

ImageDecodeService& ImageDecodeService::shared()
{
    /* Constructed on first use from the engine thread (same as FastMalloc's
     * globals); afterwards it lives for the process. */
    if (!s_sharedService)
        s_sharedService = new ImageDecodeService();
    return *s_sharedService;
}

ImageDecodeService::ImageDecodeService()
    : m_threadCount(0)
    , m_stopping(false)
    , m_enabled(-1)
#if defined(_WIN32) || defined(WIN32)
    , m_wake(0)
#endif
{
#if defined(_WIN32) || defined(WIN32)
    InitializeCriticalSection(&m_lock);
#else
    /* Only Win32 hosts can run the engine today; keep the object harmless. */
    m_enabled = 0;
#endif
}

ImageDecodeService::~ImageDecodeService()
{
    shutdown();
}

bool ImageDecodeService::enabled()
{
    if (m_enabled >= 0)
        return m_enabled != 0;

#if defined(_WIN32) || defined(WIN32)
    const char* value = getenv("AGB_IMG_ASYNC");

    if (value && value[0] == '0') {
        m_enabled = 0;
        return false;
    }
    startWorkers();
    m_enabled = m_threadCount > 0 ? 1 : 0;
#else
    m_enabled = 0;
#endif
    return m_enabled != 0;
}

void ImageDecodeService::startWorkers()
{
#if defined(_WIN32) || defined(WIN32)
    if (m_threadCount > 0 || m_stopping)
        return;

    if (!m_wake)
        m_wake = CreateEventA(0, FALSE, FALSE, 0); /* auto-reset */
    if (!m_wake)
        return;

    for (int i = 0; i < 2; ++i) {
        HANDLE thread = CreateThread(0, 0, &ImageDecodeService::workerThreadEntry, this, 0, 0);

        if (!thread)
            break;
        m_threads[m_threadCount++] = thread;
    }
#endif
}

bool ImageDecodeService::isLive(Image* image) const
{
    for (unsigned i = 0; i < m_live.size(); ++i)
        if (m_live[i] == image)
            return true;
    return false;
}

void ImageDecodeService::cancel(Image* image)
{
    for (unsigned i = 0; i < m_live.size(); ++i) {
        if (m_live[i] == image) {
            m_live[i] = 0; /* keep the slot: poll() drops the result */
            return;
        }
    }
}

#if defined(_WIN32) || defined(WIN32)
DWORD WINAPI ImageDecodeService::workerThreadEntry(LPVOID context)
{
    static_cast<ImageDecodeService*>(context)->workerLoop();
    return 0;
}
#endif

void ImageDecodeService::workerLoop()
{
#if defined(_WIN32) || defined(WIN32)
    for (;;) {
        WorkItem item;
        bool have = false;

        EnterCriticalSection(&m_lock);
        if (m_queue.size()) {
            item = m_queue[0];
            m_queue.remove(0);
            have = true;
        }
        bool stopping = m_stopping;
        LeaveCriticalSection(&m_lock);

        if (!have) {
            if (stopping)
                break; /* no more work and shutdown was requested */
            WaitForSingleObject(m_wake, 1000);
            continue;
        }

        /* Decode with a decoder of our own: the engine thread must never see
         * this one and the original ImageSource is not touched at all. */
        PixelsBuffer* frame = 0;

        {
            ImageDecoder* decoder = createDecoder(item.data);

            if (decoder) {
                decoder->setScaleHint(item.displaySize);
                decoder->setData(item.data, true);
                /* Off the engine thread there is nothing to be polite about. */
                decoder->setDecodeBudget(0, 0.0);

                PixelsBuffer* decoded = decoder->frameBufferAtIndex(0);

                if (decoded && decoded->status() != PixelsBuffer::FrameEmpty
                    && decoded->bytes().data()) {
                    frame = new PixelsBuffer(*decoded);
                }
                delete decoder;
            }
        }

        EnterCriticalSection(&m_lock);
        if (frame || m_stopping) {
            Finished done = { item.image, item.generation, frame };
            m_finished.append(done);
        }
        LeaveCriticalSection(&m_lock);
    }
#endif
}

bool ImageDecodeService::request(Image* image, const Vector<char>& data, const IntSize& displaySize, unsigned generation)
{
    if (!image || data.isEmpty() || !enabled())
        return false;

#if defined(_WIN32) || defined(WIN32)
    if (!isLive(image))
        m_live.append(image);

    WorkItem item;
    item.image = image;
    item.data = data;
    item.displaySize = displaySize;
    item.generation = generation;

    EnterCriticalSection(&m_lock);
    m_queue.append(item);
    LeaveCriticalSection(&m_lock);

    SetEvent(m_wake);
    return true;
#else
    (void)displaySize;
    (void)generation;
    return false;
#endif
}

unsigned ImageDecodeService::poll()
{
#if defined(_WIN32) || defined(WIN32)
    Vector<Finished> finished;

    EnterCriticalSection(&m_lock);
    if (m_finished.size()) {
        finished = m_finished;
        m_finished.clear();
    }
    LeaveCriticalSection(&m_lock);

    for (unsigned i = 0; i < finished.size(); ++i) {
        /* The image may have been destroyed (cancel() nulled its slot): then
         * the frame is just dropped. The worker never dereferenced the Image. */
        if (!isLive(finished[i].image)) {
            delete finished[i].frame;
            continue;
        }

        for (unsigned j = 0; j < m_live.size(); ++j) {
            if (m_live[j] == finished[i].image) {
                m_live.remove(j);
                break;
            }
        }
        finished[i].image->didDecodeFrame(finished[i].frame, finished[i].generation);
    }
    return finished.size();
#else
    return 0;
#endif
}

void ImageDecodeService::shutdown()
{
#if defined(_WIN32) || defined(WIN32)
    if (m_stopping)
        return;

    EnterCriticalSection(&m_lock);
    m_stopping = true;
    LeaveCriticalSection(&m_lock);

    if (m_threadCount > 0) {
        for (int i = 0; i < m_threadCount; ++i)
            SetEvent(m_wake); /* wake every worker so it sees m_stopping */
        for (int i = 0; i < m_threadCount; ++i) {
            WaitForSingleObject(m_threads[i], 2000);
            CloseHandle(m_threads[i]);
            m_threads[i] = 0;
        }
        m_threadCount = 0;
    }
    if (m_wake) {
        CloseHandle(m_wake);
        m_wake = 0;
    }
#endif
}

}
