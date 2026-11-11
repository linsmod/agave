/*
 * WebPImageDecoder.h - WebP decoding through the vendored libwebp.
 *
 * The image pipeline (createDecoder() in ImageSourcePS.cpp) sniffs the container
 * format; WebP was the only modern format missing, so every .webp resource
 * decoded to nothing (sizeAvail=0, no pixels). This decoder is deliberately
 * simple: libwebp is asked for the size with WebPGetInfo() and the whole frame is
 * decoded with WebPDecodeRGBA() the first time a frame buffer is requested. The
 * incremental (WebPIDecoder) path is not used - the host already buffers the
 * complete resource before painting (issues #19, wip/webp-decoder.md).
 */
#ifndef WEBP_IMAGE_DECODER_H_
#define WEBP_IMAGE_DECODER_H_

#include "config.h"

#ifdef WEBP_SUPPORT

#include "ImageDecoder.h"

namespace WebCore {

class WebPImageDecoder : public ImageDecoder
{
public:
    WebPImageDecoder();
    virtual ~WebPImageDecoder();

    virtual void setData(const Vector<char>& data, bool allDataReceived);
    virtual bool isSizeAvailable();
    virtual PixelsBuffer* frameBufferAtIndex(size_t index);

private:
    void decode();

    bool m_decoded;
};

}

#endif /* WEBP_SUPPORT */

#endif /* WEBP_IMAGE_DECODER_H_ */
