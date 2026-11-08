/*
** ImagePS.cpp: Image implements by picasso.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/
#include "config.h"
#include "Image.h"
#include "BitmapImage.h"
#include "ImageObserver.h"
#include "ImageDecoder.h"
#include "GraphicsContext.h"
#include "AffineTransform.h"
#include "wtf/MathExtras.h"

#include <picasso/picasso.h>
#include "Debug.h"

#include <stdlib.h>

PassRefPtr<WebCore::SharedBuffer> loadResourceIntoBuffer(const char*);

namespace WebCore {

void FrameData::clear()
{
    if (m_frame) {
        delete m_frame;
        m_frame = 0;
        m_duration = 0.;
        m_hasAlpha = true;
    }
}

void BitmapImage::initPlatformData()
{
}

void BitmapImage::invalidatePlatformData()
{
}

Image* Image::loadPlatformResource(const char *name)
{
    RefPtr<SharedBuffer> buffer = loadResourceIntoBuffer(name);
    BitmapImage* img = new BitmapImage;
    img->setData(buffer.release(), true);
    return img;
}

static ps_composite get_com_op(CompositeOperator op)
{
    switch (op) {
        case CompositeClear:
            return COMPOSITE_CLEAR;
        case CompositeCopy:
            return COMPOSITE_SRC;
        case CompositeSourceOver:
            return COMPOSITE_SRC_OVER;
        case CompositeSourceIn:
            return COMPOSITE_SRC_IN;
        case CompositeSourceOut:
            return COMPOSITE_SRC_OUT;
        case CompositeSourceAtop:
            return COMPOSITE_SRC_ATOP;
        case CompositeDestinationOver:
            return COMPOSITE_DST_OVER;
        case CompositeDestinationIn:
            return COMPOSITE_DST_IN;
        case CompositeDestinationOut:
            return COMPOSITE_DST_OUT;
        case CompositeDestinationAtop:
            return COMPOSITE_DST_ATOP;
        case CompositeXOR:
            return COMPOSITE_XOR;
        case CompositePlusDarker:
            return COMPOSITE_DARKEN;
        case CompositeHighlight:
            return COMPOSITE_PLUS;
        case CompositePlusLighter:
            return COMPOSITE_LIGHTEN;
        default:
            return COMPOSITE_SRC_OVER;
    }
}

/* Draws an image at a size other than its intrinsic one.
 *
 * picasso's image-as-fill sampler (ps_set_source_image + ps_fill) renders a
 * scaled source as an almost invisible ghost: on a white page a QR code comes
 * out as thin outlines because only the module edges survive, while the same
 * image at its intrinsic size - which takes the 1:1 ps_canvas_bitblt path - is
 * pixel exact. CSS-scaled <img> is everywhere (and WeChat's login QR is one of
 * them), so instead of trusting that sampler the pixels are scaled here with a
 * box filter and then handed to the bitblt path that is known to be correct.
 *
 * Only opaque images in the canvas pixel format take this route; anything else
 * falls back to the old path. */
/* Source rect (natural image pixels - what CSS/layout talk about) -> pixel
 * buffer pixels. They differ when the decoder produced a scaled frame (L3:
 * JPEG DCT scaling), in which case the buffer is smaller than the image. */
static void bufferRectForSource(NativeImagePtr imgref, const FloatRect& src,
                                int& sx, int& sy, int& sw, int& sh)
{
    int natW = imgref->naturalWidth > 0 ? imgref->naturalWidth : imgref->width;
    int natH = imgref->naturalHeight > 0 ? imgref->naturalHeight : imgref->height;
    float kx = natW > 0 ? (float)imgref->width / (float)natW : 1.0f;
    float ky = natH > 0 ? (float)imgref->height / (float)natH : 1.0f;

    sx = (int)(src.x() * kx + 0.5f);
    sy = (int)(src.y() * ky + 0.5f);
    sw = (int)(src.width() * kx + 0.5f);
    sh = (int)(src.height() * ky + 0.5f);
}

static bool draw_scaled_image(GraphicsContext* context, NativeImagePtr imgref,
                              const FloatRect& dst, const FloatRect& src, CompositeOperator op)
{
    if (!context || !imgref || !imgref->source || !imgref->source->initialized())
        return false;
    /* bitblt copies pixels, it does not blend, so alpha sources must not use
     * it. Same reason the 1:1 fast path below checks imgref->transparent. */
    if (imgref->transparent || op != CompositeSourceOver)
        return false;

    if (pixelFormat() != COLOR_FORMAT_BGRA || pixelPerBytes() != 4)
        return false;

    {
        PixelsBuffer* buffer = imgref->source->frameBufferAtIndex(imgref->index);

        if (!buffer || !buffer->bytes().data() || buffer->status() == PixelsBuffer::FrameEmpty)
            return false;

        int imgW = imgref->width;
        int imgH = imgref->height;

        /* Source rectangle in image pixels, clamped to the image. */
        int sx;
        int sy;
        int sw;
        int sh;

        bufferRectForSource(imgref, src, sx, sy, sw, sh);

        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx + sw > imgW) sw = imgW - sx;
        if (sy + sh > imgH) sh = imgH - sy;
        if (sw <= 0 || sh <= 0 || imgW <= 0 || imgH <= 0)
            return false;

        /* Destination size in device pixels, so zoomed pages still fill the
         * box the layout asked for. */
        ps_context* gc = context->platformContext();
        ps_point p0 = { dst.x(), dst.y() };
        ps_point p1 = { dst.x() + dst.width(), dst.y() + dst.height() };

        ps_world_to_viewport(gc, &p0);
        ps_world_to_viewport(gc, &p1);

        int dw = (int)(p1.x - p0.x + 0.5f);
        int dh = (int)(p1.y - p0.y + 0.5f);

        if (dw <= 0 || dh <= 0 || (dw == sw && dh == sh))
            return false;

        unsigned* pixels = (unsigned*)malloc(sizeof(unsigned) * (size_t)dw * (size_t)dh);

        if (!pixels)
            return false;

        {
            const unsigned* s = (const unsigned*)buffer->bytes().data();
            /* Only the rows that are decoded so far are sampled: a partial
             * frame (per-paint decode budget) has the rest zeroed, and reading
             * them would cost the same as a full pass while painting nothing. */
            const int decodedRows = (int)buffer->height();
            /* Box-filter samples per destination pixel. The exact box is
             * (sw/dw)x(sh/dh) taps - 100 for a 4000px JPEG shown in 400px - and
             * costs ~85ms on a 12MP source. Partial frames use a very cheap
             * box (2 taps/axis: they are visibly filling in anyway); the final
             * frame uses a denser one, so what stays on screen looks like the
             * exact filter at a fraction of the cost. AGB_IMG_SAMPLE_MAX sets
             * the final cap (0 = exact, for pixel-reference screenshots). */
            static int maxTaps = -1;
            int stepMax;

            if (maxTaps < 0) {
                const char* value = getenv("AGB_IMG_SAMPLE_MAX");

                maxTaps = value ? atoi(value) : 4;
                if (maxTaps < 0)
                    maxTaps = 0;
            }
            if (buffer->status() == PixelsBuffer::FrameComplete)
                stepMax = maxTaps;
            else
                stepMax = (maxTaps == 0 || maxTaps > 2) ? 2 : maxTaps;
            int y;

            for (y = 0; y < dh; ++y) {
                int y0 = sy + (y * sh) / dh;
                int y1 = sy + ((y + 1) * sh) / dh;
                int x;

                if (y1 <= y0)
                    y1 = y0 + 1;
                if (y1 > sy + sh)
                    y1 = sy + sh;
                if (y0 >= decodedRows)
                    continue; /* not decoded yet */
                if (y1 > decodedRows)
                    y1 = decodedRows;

                /* Subsample the source band instead of averaging all of it. */
                int stepY = (stepMax > 0 && y1 - y0 > stepMax) ? (y1 - y0) / stepMax : 1;

                for (x = 0; x < dw; ++x) {
                    int x0 = sx + (x * sw) / dw;
                    int x1 = sx + ((x + 1) * sw) / dw;
                    unsigned acc0 = 0, acc1 = 0, acc2 = 0, acc3 = 0, n = 0;
                    int j;

                    if (x1 <= x0)
                        x1 = x0 + 1;
                    if (x1 > sx + sw)
                        x1 = sx + sw;

                    /* Subsample the box instead of averaging every pixel it
                     * covers (see stepY above). Hoisted out of the row loop:
                     * an integer division per destination pixel per row is
                     * measurable on a 400x300 draw. */
                    int stepX = (stepMax > 0 && x1 - x0 > stepMax) ? (x1 - x0) / stepMax : 1;

                    /* Box filter: average the source pixels this destination
                     * pixel covers. The buffer is premultiplied BGRA, so the
                     * four byte lanes can be averaged independently. */
                    for (j = y0; j < y1; j += stepY) {
                        const unsigned* row = s + (size_t)j * imgW;
                        int i;

                        for (i = x0; i < x1; i += stepX) {
                            unsigned p = row[i];

                            acc0 += p & 0xff;
                            acc1 += (p >> 8) & 0xff;
                            acc2 += (p >> 16) & 0xff;
                            acc3 += (p >> 24) & 0xff;
                            ++n;
                        }
                    }

                    if (!n)
                        n = 1;

                    /* One division per pixel instead of four: scale is 16.16
                     * fixed point of 1/n, applied with a shift. */
                    unsigned scale = 0x10000u / n;
                    unsigned a0 = (acc0 * scale) >> 16;
                    unsigned a1 = (acc1 * scale) >> 16;
                    unsigned a2 = (acc2 * scale) >> 16;
                    unsigned a3 = (acc3 * scale) >> 16;

                    pixels[(size_t)y * dw + x] = (a3 << 24) | (a2 << 16) | (a1 << 8) | a0;
                }
            }
        }

        {
            ps_image* scaled = ps_image_create_with_data((ps_byte*)pixels, pixelFormat(),
                                                         dw, dh, dw * 4);
            bool ok = false;

            if (scaled) {
                ps_canvas* srcCanvas = ps_canvas_create_from_image(scaled, 0);
                ps_canvas* dstCanvas = ps_context_get_canvas(gc);

                if (srcCanvas && dstCanvas) {
                    ps_rect whole = { 0, 0, dw, dh };

                    ps_canvas_bitblt(srcCanvas, &whole, dstCanvas, &p0);
                    ok = true;
                }
                if (srcCanvas)
                    ps_canvas_unref(srcCanvas);
                ps_image_unref(scaled);
            }

            free(pixels);
            return ok;
        }
    }
}

static ps_image* build_image(NativeImagePtr imgref){
	if (!imgref->source->initialized())
		return 0;

    PixelsBuffer* buffer = imgref->source->frameBufferAtIndex(imgref->index);
    if (!buffer || !buffer->bytes().data())
        return 0;

	ps_image* img = ps_image_create_with_data(buffer->bytes().data(), pixelFormat(),
												imgref->width, imgref->height, imgref->pitch);

	if (img) {
		if (imgref->transparent) {
			if (imgref->colorkey) {
				ps_color c = {0,0,0,0};
				ps_image_set_transparent_color(img, &c);
			}
			ps_image_set_allow_transparent(img, True);
		}
	}
	return img;
}

void Image::drawPattern(GraphicsContext* context, const FloatRect& tileRect, const AffineTransform& patternTransform,
                        const FloatPoint& p, CompositeOperator op, const FloatRect& dst)
{
    NativeImagePtr imgref = nativeImageForCurrentFrame();
    if (!imgref)
        return;

    ps_image * img = build_image(imgref);
	if (img) {
		ps_context * gc = context->platformContext(); 
		ps_composite com_op = ps_set_composite_operator(gc, get_com_op(op));

        /* tileRect is in natural image pixels (background geometry); the image
         * object is in buffer pixels, which differ for a scaled decode (L3). */
        int bsx;
        int bsy;
        int bsw;
        int bsh;

        bufferRectForSource(imgref, tileRect, bsx, bsy, bsw, bsh);

		ps_rect rect = {bsx, bsy, bsw, bsh};
		ps_image* pimg = ps_image_create_from_image(img, &rect);

		if (tileRect.size() == dst.size() && !p.x() && !p.y()) { //For optimization with single image
			ps_set_source_image(gc, pimg);
			//draw image
			ps_rect drect = {dst.x(), dst.y(), dst.width(), dst.height()};
			ps_rectangle(gc, &drect);
			ps_fill(gc);
		} else {
			//create tile image
			ps_matrix* mtx = ps_matrix_create_copy(patternTransform.platformTransform());
			ps_matrix_translate (mtx, p.x()-dst.x(), p.y()-dst.y());

			ps_pattern* pattern = ps_pattern_create_image(pimg, WRAP_TYPE_REPEAT, WRAP_TYPE_REPEAT, mtx);
			ps_set_source_pattern(gc, pattern);

			//draw pattern
			ps_rect drect = {dst.x(), dst.y(), dst.width(), dst.height()};
			ps_rectangle(gc, &drect);
			ps_fill(gc);

			//destory pattern objects
			ps_pattern_unref(pattern);
			ps_matrix_unref(mtx);
		}
		ps_image_unref(pimg);
		(void)ps_set_composite_operator(gc, com_op);

		ps_image_unref(img);
	}

	if (imageObserver())
		imageObserver()->didDraw(this);
}

void BitmapImage::draw(GraphicsContext* context, const FloatRect& dst, const FloatRect& src, CompositeOperator op)
{  
	startAnimation();

    /* L3: tell the decoder how big this image is actually drawn, so a 12MP JPEG
     * shown in a 400x300 box decodes at 1/8 scale instead of full size. If the
     * frame we have is now too small for the display (zoom in, resize), drop it
     * and decode again - the hint is applied to the fresh decoder. */
    int displayWidth = 0;
    int displayHeight = 0;

    if (context && dst.width() > 0 && dst.height() > 0) {
        ps_point p0 = { dst.x(), dst.y() };
        ps_point p1 = { dst.x() + dst.width(), dst.y() + dst.height() };
        ps_context* gc = context->platformContext();

        ps_world_to_viewport(gc, &p0);
        ps_world_to_viewport(gc, &p1);

        displayWidth = (int)(p1.x - p0.x + 0.5f);
        displayHeight = (int)(p1.y - p0.y + 0.5f);
    }

    m_pendingDisplaySize = IntSize(displayWidth, displayHeight);

    bool painting = imageDecodeStatsInFrame();

    /* L2: when the frame is not here yet, ask a worker for it and draw nothing
     * this frame; ImageDecodeService delivers the pixels and the observer
     * repaints. Only inside a paint traversal: canvas drawImage()/printing have
     * no later repaint, so they take the inline path below and decode fully.
     * The inline path also stays the fallback when async is disabled or the
     * format animates. */
    if (asyncDecodingAllowed() && painting) {
        if (m_source.hasDecodedFrame() && displayWidth > 0
            && m_source.setScaleHint(IntSize(displayWidth, displayHeight))) {
            /* Zoomed in past the decoded buffer: drop it and decode again. */
            dropAdoptedFrame();
        }
        if (!m_source.hasDecodedFrame()) {
            requestAsyncDecode(IntSize(displayWidth, displayHeight));
            return;
        }
    } else if (context && !getenv("AGB_IMG_NO_SCALE") && !m_useNaturalDecode) {
        /* L3 on the inline path: tell the decoder how big the image is drawn.
         * Outside a paint traversal (canvas) the caller needs the whole frame,
         * so the budget is bypassed. */
        if (displayWidth > 0 && m_source.setScaleHint(IntSize(displayWidth, displayHeight)))
            destroyDecodedData();
    }

    /* Outside a paint traversal the caller needs pixels now (canvas drawImage,
     * printing): decode the whole frame instead of a budgeted slice. */
    m_source.setDecodeBudgetIgnored(!painting);

    NativeImagePtr imgref = nativeImageForCurrentFrame();

    m_source.setDecodeBudgetIgnored(false);

	if (!imgref) {
        /* Nothing to draw yet, but the decoder may still owe rows (the per-paint
         * budget stopped it): keep the progression alive. */
        if (m_source.frameHasMoreWork())
            requestFrameProgress();
		return;
    }

    /* A scaled draw must not go through picasso's image-fill sampler (see
     * draw_scaled_image); scale the pixels here and blit. */
    if ((dst.size() != src.size()) && draw_scaled_image(context, imgref, dst, src, op)) {
        if (imageObserver())
            imageObserver()->didDraw(this);
        if (m_source.frameHasMoreWork())
            requestFrameProgress();
        return;
    }

	ps_image * img = build_image(imgref);
    if (img) {
        ps_context * gc = context->platformContext(); 
        /* Source rect arrives in natural image pixels; ps_image works in buffer
         * pixels (they differ for a scaled decode). */
        int sx;
        int sy;
        int sw;
        int sh;

        bufferRectForSource(imgref, src, sx, sy, sw, sh);

		ps_rect rect = {sx, sy, sw, sh};
		ps_rect drect = {dst.x(), dst.y(), dst.width(), dst.height()};
		if ((dst.size() == src.size()) && (op == CompositeSourceOver) 
			&& (!imgref->transparent)) {
			//For optimization with single image
			ps_point point = {drect.x, drect.y};
			ps_world_to_viewport(gc, &point);
			ps_canvas* src_canvas = ps_canvas_create_from_image(img, 0);
			ps_canvas* dst_canvas = ps_context_get_canvas(gc); 
			ps_canvas_bitblt(src_canvas, &rect, dst_canvas, &point);
			ps_canvas_unref(src_canvas);
		} else {
			ps_composite com_op = ps_set_composite_operator(gc, get_com_op(op));
			ps_image* pimg = ps_image_create_from_image(img, &rect);
			ps_set_source_image(gc, pimg);
			ps_rectangle(gc, &drect);
			ps_fill(gc);
			(void)ps_set_composite_operator(gc, com_op);
			ps_image_unref(pimg);
		}
		ps_image_unref(img);
    }

	if (imageObserver())
		imageObserver()->didDraw(this);

    /* The frame may still be missing rows (budget): ask for another paint so the
     * next slice happens on the next frame, not inside this one. */
    if (m_source.frameHasMoreWork())
            requestFrameProgress();
}

void BitmapImage::drawPattern(GraphicsContext* context, const FloatRect& tileRect, const AffineTransform& patternTransform,
                              const FloatPoint& p, CompositeOperator op, const FloatRect& dst)
{
    /* A repeating background repeats the tile at its *image* pixel size, so the
     * tile has to be decoded at the natural size: a scaled decode (L3) would
     * repeat a smaller tile and change the pattern geometry. Switch this image
     * to natural-size decoding for good (draw() stops hinting it down). */
    m_useNaturalDecode = true;

    IntSize decoded = m_source.decodedFrameSize(m_currentFrame);

    if (decoded.width() > 0 && decoded != size())
        destroyDecodedData(); /* re-decode at the natural size */

    Image::drawPattern(context, tileRect, patternTransform, p, op, dst);
}

void BitmapImage::checkForSolidColor()
{
    m_isSolidColor = false;
}

}
