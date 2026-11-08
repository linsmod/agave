/*
** ImageSourcePS.cpp: ImageSource implements by picasso.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/

#include "stdlib.h"
#include "config.h"

#include "ImageDecoder.h"
#include "ImageDecodeStats.h"
#include "ImageSource.h"
#include "SharedBuffer.h"
#include "Debug.h"

#ifdef GIF_SUPPORT
#include "GIFImageDecoder.h"
#endif /* GIF_SUPPORT */

#ifdef JPEG_SUPPORT
#include "JPEGImageDecoder.h"
#endif /* JPEG_SUPPORT */

/* The one diagnostics sink (src/webview/AgbLog.h, header-only). */
#include "../../../webview/AgbLog.h"

#ifdef PNG_SUPPORT
#include "PNGImageDecoder.h"
#endif /* PNG_SUPPORT */

#ifdef BMP_SUPPORT
#include "BMPImageDecoder.h"
#endif /* BMP_SUPPORT */

#ifdef ICO_SUPPORT
#include "ICOImageDecoder.h"
#endif /* ICO_SUPPORT */

#ifdef XBM_SUPPORT
#include "XBMImageDecoder.h"
#endif /* XBM_SUPPORT */

namespace WebCore {

extern ps_color_format pixelFormat(void);
extern int pixelPerBytes(void);

/* Every decoder entry point below is lazy: the first call for a frame decodes
 * it, and a large JPEG/PNG used to decode all of its rows inside that one call.
 * Time them so [perf] can say "paint=467ms, decode=460ms" instead of blaming
 * the paint traversal. See ImageDecodeStats.h. */
static PixelsBuffer* timedFrameBufferAtIndex(ImageDecoder* decoder, size_t index)
{
    double began = imageDecodeStatsNow();
    PixelsBuffer* buffer = decoder->frameBufferAtIndex(index);

    imageDecodeStatsAddMs((imageDecodeStatsNow() - began) * 1000.0);
    return buffer;
}

/* How much decoding one paint may do, across all images.
 *
 * AGB_IMG_DECODE_MS   0 = unlimited (old behaviour), default 12ms/frame: the
 *                     rest of the rows are decoded on later frames while the
 *                     page keeps running JS/input (R136).
 * AGB_IMG_DECODE_ROWS row cap for one decoder call (default 256). */
static double imageDecodeBudgetMs()
{
    static double ms = -1.0;

    if (ms < 0.0) {
        const char* value = getenv("AGB_IMG_DECODE_MS");

        ms = value ? atof(value) : 12.0;
        if (ms < 0.0)
            ms = 0.0;
    }
    return ms;
}

static int imageDecodeBudgetRows()
{
    static int rows = -1;

    if (rows < 0) {
        const char* value = getenv("AGB_IMG_DECODE_ROWS");

        rows = value ? atoi(value) : 256;
        if (rows < 0)
            rows = 0;
    }
    return rows;
}

/* Applies this frame's remaining decode budget to the decoder. Every public
 * ImageSource entry that can decode goes through here, so the budget covers
 * scaled draws and pattern fills too. */
static void applyDecodeBudget(ImageDecoder* decoder, bool ignoreBudget)
{
    /* Canvas drawImage / printing: no later frame to continue in, so decode the
     * whole frame now. */
    if (ignoreBudget) {
        decoder->setDecodeBudget(0, 0.0);
        return;
    }

    double budgetMs = imageDecodeBudgetMs();
    double deadline = 0.0;

    if (budgetMs <= 0.0) {
        /* AGB_IMG_DECODE_MS=0 means "no budget at all" (the pre-R136 behaviour,
         * used for reference screenshots and A/B runs): no time limit and no
         * row cap, so one call decodes the whole frame. */
        decoder->setDecodeBudget(0, 0.0);
        return;
    }

    double left = budgetMs;

    /* Inside paint every image shares the frame's budget; outside it (JS
     * decode(), an animation timer) the call gets a fresh one. */
    if (imageDecodeStatsInFrame()) {
        left -= imageDecodeStatsPeekMs();
        if (left < 0.0)
            left = 0.0;
    }
    deadline = imageDecodeStatsNow() + left / 1000.0;
    decoder->setDecodeBudget(imageDecodeBudgetRows(), deadline);
}

ImageDecoder* createDecoder(const Vector<char>& data)
{
    // We need at least 4 bytes to figure out what kind of image we're dealing with.
    int length = data.size();
    if (length < 4)
        return 0;
    
    const unsigned char* uContents = (const unsigned char*) data.data();
    const char* contents = data.data();

#ifdef GIF_SUPPORT
    // GIFs begin with GIF8(7 or 9).
    if (strncmp(contents, "GIF8", 4) == 0)
    {
        return new GIFImageDecoder();
    }
#endif /* GIF_SUPPORT */

#ifdef PNG_SUPPORT
    // Test for PNG.
    if (uContents[0] == 0x89 &&
        uContents[1] == 0x50 &&
        uContents[2] == 0x4E &&
        uContents[3] == 0x47)
    {
        return new PNGImageDecoder();
    }
#endif /* PNG_SUPPORT */

#ifdef JPEG_SUPPORT
    // JPEG
    if (uContents[0] == 0xFF &&
        uContents[1] == 0xD8 &&
        uContents[2] == 0xFF)
    {
        return new JPEGImageDecoder();
    }
#endif /* JPEG_SUPPORT */

#ifdef BMP_SUPPORT
    // BMP
    if (strncmp(contents, "BM", 2) == 0)
    {
        return new BMPImageDecoder();
    }
#endif /* BMP_SUPPORT */
    
#ifdef ICO_SUPPORT
    // ICOs always begin with a 2-byte 0 followed by a 2-byte 1.
    // CURs begin with 2-byte 0 followed by 2-byte 2.
    if (!memcmp(contents, "\000\000\001\000", 4) ||
        !memcmp(contents, "\000\000\002\000", 4))
    {
        return new ICOImageDecoder();
    }
#endif /* ICO_SUPPORT */

#ifdef XBM_SUPPORT
    // XBMs require 8 bytes of info.
    if (length >= 8 && strncmp(contents, "#define ", 8) == 0)
    {
        return new XBMImageDecoder();
    }
#endif /* XBM_SUPPORT */
    // Give up. We don't know what the heck this is.
    
    return 0;
}

ImageSource::ImageSource()
  : m_decoder(0)
  , m_adoptedFrame(0)
  , m_ignoreDecodeBudget(false)
{
}

ImageSource::~ImageSource()
{
    clear();
}

void ImageSource::clear()
{
    clearDecodedFrame();
    if (m_decoder) {
        delete m_decoder;
        m_decoder = 0;
    }
}

/* L2: a worker decoded the frame; from now on the frame accessors serve it and
 * the engine-side decoder is only kept for the natural size / animation
 * metadata. Takes ownership of `frame`. */
void ImageSource::adoptDecodedFrame(PixelsBuffer* frame)
{
    clearDecodedFrame();
    m_adoptedFrame = frame;
    m_adoptedNaturalSize = size();
}

void ImageSource::clearDecodedFrame()
{
    if (m_adoptedFrame) {
        delete m_adoptedFrame;
        m_adoptedFrame = 0;
    }
}

bool ImageSource::initialized() const
{
    return (m_decoder || m_adoptedFrame) ? true : false;
}

void ImageSource::setData(SharedBuffer* data, bool allDataReceived)
{
    /* New bytes invalidate whatever a worker produced for the old data. */
    clearDecodedFrame();

    // Make the decoder by sniffing the bytes.
    // This method will examine the data and instantiate an instance of the appropriate decoder plugin.
    // If insufficient bytes are available to determine the image type, no decoder plugin will be
    // made.    
    if (m_decoder)
        delete m_decoder;

    m_decoder = createDecoder(data->buffer());

    if (!m_decoder)
        return;
    m_decoder->setData(data->buffer(), allDataReceived);
    applyScaleHint();
}

/* L3: keep the decoder's scale hint in sync with the display size. Called
 * whenever a decoder (re)appears, and from setScaleHint(). */
void ImageSource::applyScaleHint()
{
    if (m_decoder)
        m_decoder->setScaleHint(m_scaleHint);
}

bool ImageSource::setScaleHint(const IntSize& displaySize)
{
    if (displaySize.width() <= 0 || displaySize.height() <= 0)
        return false;

    m_scaleHint = displaySize;
    if (m_adoptedFrame) {
        /* L2: the adopted frame is the only pixels we have. Ask for a new
         * decode when it is smaller than the display (and smaller than the
         * image itself, otherwise re-decoding cannot help). */
        IntSize natural = m_adoptedNaturalSize;
        unsigned width = m_adoptedFrame->width();

        if (width == 0)
            return false;
        if (natural.width() > 0 && width >= (unsigned)natural.width())
            return false;
        return width < (unsigned)displaySize.width();
    }
    if (!m_decoder)
        return false;

    applyScaleHint();

    /* Re-decode only when the frame we already have is smaller than what is
     * being displayed now (zoom-in/resize): decoding a bigger buffer than the
     * screen can show wastes work, decoding a smaller one shows upscaled mush.
     * Never ask for more than the natural size, though: an image displayed
     * larger than its own pixels would otherwise re-decode on every frame. */
    const PixelsBuffer* buffer = m_decoder->cachedFrameBuffer(0);
    IntSize natural = size();

    if (!buffer || buffer->width() == 0)
        return false; /* nothing decoded yet: the hint is already stored */
    if (natural.width() > 0 && buffer->width() >= (unsigned)natural.width())
        return false; /* already full resolution */
    return buffer->width() < (unsigned)displaySize.width();
}

bool ImageSource::isSizeAvailable()
{
    if (m_adoptedFrame)
        return true;
    if (!m_decoder)
        return false;

    return m_decoder->isSizeAvailable();
}

IntSize ImageSource::size() const
{
    if (m_adoptedFrame)
        return m_adoptedNaturalSize;
    if (!m_decoder)
        return IntSize();

    return m_decoder->size();
}

int ImageSource::repetitionCount()
{
    if (!m_decoder)
        return cAnimationNone;

    return m_decoder->repetitionCount();
}

size_t ImageSource::frameCount() const
{
    if (m_adoptedFrame)
        return 1;
    return m_decoder ? m_decoder->frameCount() : 0;
}

PixelsBuffer* ImageSource::frameBufferAtIndex(size_t index)
{
    if (m_adoptedFrame)
        return index == 0 ? m_adoptedFrame : 0;
    if (!m_decoder)
        return 0;

    applyDecodeBudget(m_decoder, m_ignoreDecodeBudget);
    return timedFrameBufferAtIndex(m_decoder, index);
}

bool ImageSource::frameHasMoreWork() const
{
    if (m_adoptedFrame)
        return false;
    return m_decoder && m_decoder->hasMoreWork();
}

IntSize ImageSource::decodedFrameSize(size_t index) const
{
    if (m_adoptedFrame)
        return index == 0
            ? IntSize((int)m_adoptedFrame->width(), (int)m_adoptedFrame->fullHeight())
            : IntSize();

    const PixelsBuffer* buffer = m_decoder ? m_decoder->cachedFrameBuffer(index) : 0;

    if (!buffer || buffer->width() == 0)
        return IntSize();
    return IntSize((int)buffer->width(), (int)buffer->fullHeight());
}

NativeImagePtr ImageSource::createFrameAtIndex(size_t index)
{
    if (!m_decoder || m_decoder->failed())
        return 0;

    /* Defensive: a corrupt/absurd image header (ad beacons, truncated PNG)
     * used to reach the decoder with dimensions that make it allocate an
     * enormous frame buffer - the process died with an access violation inside
     * PNGImageDecoder::frameBufferAtIndex while painting v.qq.com. Refuse
     * anything beyond what a browser viewport can plausibly show. */
    {
        IntSize frameSize = size();
        double pixels = (double)frameSize.width() * (double)frameSize.height();

        /* size() is 0x0 before the header is decoded: that must go through to
         * the decoder, otherwise the image never appears (and a later paint
         * can crash on the missing frame). Only reject *known* absurd sizes. */
        if (frameSize.width() > 16384 || frameSize.height() > 16384
            || pixels > 40000000.0) {
            agbLog(AGB_LOG_CACHE, "frame rejected size=%dx%d", frameSize.width(), frameSize.height());
            return 0;
        }
    }

    PixelsBuffer* buffer = frameBufferAtIndex(index);

    /* [cache]: which frame a paint is about to use. FramePartial with fewer rows
     * than the image means the paint is drawing a half-decoded frame;
     * FrameComplete here means the frame is fine and the bug is a repaint that
     * never reached the window. */
    agbLog(AGB_LOG_CACHE, "frame idx=%u status=%d rows=%u/%d",
           (unsigned)index,
           buffer ? (int)buffer->status() : -1,
           buffer ? buffer->height() : 0,
           size().height());

#ifdef IMAGE_DISPLAY_ONCE
    if (!buffer || !buffer->bytes().data() || (buffer->status() != PixelsBuffer::FrameComplete))
        return 0;
#else
    if (!buffer || !buffer->bytes().data() || (buffer->status() == PixelsBuffer::FrameEmpty))
        return 0;
#endif

    ImageRef* img = new ImageRef;
	img->source = this;
	img->index = index;
	/* Buffer geometry: smaller than natural when the decoder used a scale (L3). */
	img->width = buffer->width() ? (int)buffer->width() : size().width();
	img->height = buffer->fullHeight() ? (int)buffer->fullHeight() : size().height();
	img->naturalWidth = size().width();
	img->naturalHeight = size().height();
	img->pitch = img->width * pixelPerBytes();

	if (buffer->hasAlpha()) {
		if (pixelFormat() != COLOR_FORMAT_BGRA) {
			img->colorkey = true;
		}
		img->transparent = true;
	}
    return img;
}

float ImageSource::frameDurationAtIndex(size_t index)
{
    if (m_adoptedFrame)
        return 0;
    if (!m_decoder)
        return 0;

    PixelsBuffer* buffer = frameBufferAtIndex(index);
    if (!buffer || buffer->status() == PixelsBuffer::FrameEmpty)
        return 0;

    return buffer->duration() / 1000.0f;
}

bool ImageSource::frameHasAlphaAtIndex(size_t index)
{
    if (m_adoptedFrame)
        return m_adoptedFrame->hasAlpha();
    if (!m_decoder || !m_decoder->supportsAlpha())
        return false;

    PixelsBuffer* buffer = frameBufferAtIndex(index);
    if (!buffer || buffer->status() == PixelsBuffer::FrameEmpty)
        return false;

    return buffer->hasAlpha();
}

bool ImageSource::frameIsCompleteAtIndex(size_t index)
{
    if (m_adoptedFrame)
        return index == 0;
    if (!m_decoder)
        return false;

    PixelsBuffer* buffer = frameBufferAtIndex(index);
    return buffer && buffer->status() == PixelsBuffer::FrameComplete;
}

}
