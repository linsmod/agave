/*
 * WebPImageDecoder.cpp - WebP decoding through the vendored libwebp.
 *
 * See WebPImageDecoder.h for why this is a whole-frame decode: the host buffers
 * the resource before the image is painted, and libwebp's simple API decodes a
 * complete file in one call. Pixels are written through PixelsBuffer::setRGBA(),
 * which also converts to the host's pixel format (BGRA/RGB565) and premultiplies
 * alpha, the same path the PNG/JPEG decoders use.
 */

#include "config.h"
#include "WebPImageDecoder.h"

#ifdef WEBP_SUPPORT

#include <webp/decode.h>

namespace WebCore {

WebPImageDecoder::WebPImageDecoder()
    : m_decoded(false)
{
}

WebPImageDecoder::~WebPImageDecoder()
{
}

void WebPImageDecoder::setData(const Vector<char>& data, bool allDataReceived)
{
    ImageDecoder::setData(data, allDataReceived);

    /* A new resource: the previous size/frame no longer describe it. */
    m_decoded = false;
    m_sizeAvailable = false;
    m_failed = false;
    setMoreWork(false);
    m_frameBufferCache.clear();
}

bool WebPImageDecoder::isSizeAvailable()
{
    int width = 0;
    int height = 0;

    if (m_sizeAvailable)
        return true;

    if (m_data.size() < 12)
        return false;

    if (!WebPGetInfo(reinterpret_cast<const uint8_t*>(m_data.data()),
                     (size_t)m_data.size(), &width, &height))
        return false;

    if (width <= 0 || height <= 0)
        return false;

    m_size = IntSize(width, height);
    m_sizeAvailable = true;
    return true;
}

PixelsBuffer* WebPImageDecoder::frameBufferAtIndex(size_t index)
{
    if (index)
        return 0;

    if (m_frameBufferCache.isEmpty())
        m_frameBufferCache.resize(1);

    {
        PixelsBuffer& frame = m_frameBufferCache[0];

        if (frame.status() != PixelsBuffer::FrameComplete)
            decode();
        return &frame;
    }
}

void WebPImageDecoder::decode()
{
    PixelsBuffer& buffer = m_frameBufferCache[0];
    int width = 0;
    int height = 0;
    uint8_t* rgba;

    if (m_decoded || m_data.isEmpty())
        return;

    m_decoded = true;

    rgba = WebPDecodeRGBA(reinterpret_cast<const uint8_t*>(m_data.data()),
                          (size_t)m_data.size(), &width, &height);
    if (!rgba || width <= 0 || height <= 0) {
        /* Corrupt or unsupported variant: keep the frame empty, tell ImageSource
         * this decoder failed so the resource is not retried forever. */
        if (rgba)
            WebPFree(rgba);
        m_failed = true;
        buffer.setStatus(PixelsBuffer::FrameComplete);
        setMoreWork(false);
        return;
    }

    m_size = IntSize(width, height);
    m_sizeAvailable = true;

    buffer.setFrameSize((unsigned)width, (unsigned)height);
    buffer.setRect(IntRect(0, 0, width, height));
    buffer.bytes().resize((size_t)width * (size_t)height);

    {
        bool sawAlpha = false;
        int x, y;

        for (y = 0; y < height; ++y) {
            const uint8_t* row = rgba + (size_t)y * (size_t)width * 4u;
            unsigned dst = (unsigned)y * (unsigned)width;

            for (x = 0; x < width; ++x) {
                unsigned red = row[0];
                unsigned green = row[1];
                unsigned blue = row[2];
                unsigned alpha = row[3];
                unsigned pixel;

                row += 4;
                PixelsBuffer::setRGBA(pixel, red, green, blue, alpha);
                buffer.bytes().setPixel(dst + (unsigned)x, pixel);
                if (!sawAlpha && alpha < 255)
                    sawAlpha = true;
            }
        }

        buffer.setHasAlpha(sawAlpha);
        buffer.ensureHeight((unsigned)height);
    }

    WebPFree(rgba);

    buffer.setStatus(PixelsBuffer::FrameComplete);
    setMoreWork(false);
}

}
#endif /* WEBP_SUPPORT */
