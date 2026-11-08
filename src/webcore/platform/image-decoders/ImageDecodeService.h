/*
 * ImageDecodeService.h: decode images off the engine thread.
 *
 * Copyright (C) 2026 agave project
 *
 * L1 (docs/design-async-image-decode.md) bounds the decoding done inline by a
 * paint; L2 takes it off the engine thread entirely: paint asks for a frame, a
 * worker decodes it (whole frame, at the display scale from L3) and the engine
 * adopts the finished pixels on a later frame. The engine thread never touches
 * a decoder that a worker owns and vice versa:
 *
 *   engine: request(image, bytes, displaySize, generation)
 *   worker: decodes from a copy of the bytes with its own decoder, hands back
 *           an owned PixelsBuffer
 *   engine: poll() -> Image::didDecodeFrame() -> repaint
 *
 * The image is kept alive by a reference the engine thread takes on request()
 * and releases after the result has been delivered, so a worker never
 * dereferences a dead image (and never touches Image at all beyond the pointer
 * it uses for matching).
 */

#ifndef ImageDecodeService_h
#define ImageDecodeService_h

#include "IntSize.h"
#include <wtf/Vector.h>

#if defined(_WIN32) || defined(WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace WebCore {

class Image;
class PixelsBuffer;

class ImageDecodeService {
public:
    static ImageDecodeService& shared();

    /* Engine thread. Queues one whole-frame decode; the caller keeps a
     * "pending" flag per image so it does not queue twice. Returns false when
     * the service is disabled (AGB_IMG_ASYNC=0) or no worker could be started,
     * in which case the caller falls back to the inline decoder. */
    bool request(Image* image, const Vector<char>& data, const IntSize& displaySize, unsigned generation);

    /* Engine thread: deliver finished frames (calls Image::didDecodeFrame for
     * images that are still alive). Call once per frame, before painting, so a
     * fresh frame is painted in the same frame. */
    unsigned poll();

    /* Engine thread: the Image is going away (its ImageSource owned it). Frames
     * still in flight for it are dropped when they arrive; the worker never
     * touched the Image, only the pointer for matching. */
    void cancel(Image* image);

    bool enabled();

    void shutdown();

private:
    ImageDecodeService();
    ~ImageDecodeService();
    ImageDecodeService(const ImageDecodeService&);
    ImageDecodeService& operator=(const ImageDecodeService&);

    struct WorkItem {
        Image* image;
        Vector<char> data;
        IntSize displaySize;
        unsigned generation;
    };

    struct Finished {
        Image* image;
        unsigned generation;
        PixelsBuffer* frame;
    };

    void startWorkers();
    void workerLoop();
    bool isLive(Image* image) const;

#if defined(_WIN32) || defined(WIN32)
    static DWORD WINAPI workerThreadEntry(LPVOID context);
#endif

    Vector<WorkItem> m_queue;      // guarded by m_lock
    Vector<Finished> m_finished;   // guarded by m_lock
    Vector<Image*> m_live;         // engine thread only: images with jobs in flight

#if defined(_WIN32) || defined(WIN32)
    CRITICAL_SECTION m_lock;
    HANDLE m_wake;
    HANDLE m_threads[2];
#endif

    int m_threadCount;
    bool m_stopping;
    int m_enabled; // -1 unknown, 0 disabled, 1 enabled
};

}

#endif // ImageDecodeService_h
