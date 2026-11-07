/*
 * Copyright (C) 2024 Zhang Ji Peng (onecoolx@gmail.com)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "RenderMedia.h"

#include "CachedImage.h"
#include "Document.h"
#include "GraphicsContext.h"
#include "HTMLMediaElement.h"
#include "HTMLNames.h"
#include "Page.h"
#include "RenderView.h"
#include <picasso/picasso.h>
#include <stdio.h>
#include <stdlib.h>

namespace WebCore {

/* Declared by the engine's platform layer (WebEngine.cpp); the renderer needs
 * it to wrap decoder output in a canvas of the surface's own format. */
extern ps_color_format pixelFormat(void);

RenderMedia::RenderMedia(HTMLMediaElement* element, const IntSize& intrinsicSize)
    : RenderReplaced(element, intrinsicSize)
    , m_loggedFrame(false)
{
}

HTMLMediaElement* RenderMedia::mediaElement() const
{
    Node* n = node();

    if (!n || !n->isElementNode())
        return 0;

    {
        Element* el = static_cast<Element*>(n);

        if (!el->hasTagName(HTMLNames::videoTag))
            return 0;
        return static_cast<HTMLMediaElement*>(el);
    }
}

void RenderMedia::paint(PaintInfo& paintInfo, int tx, int ty)
{
    if (!shouldPaint(paintInfo, tx, ty))
        return;

    tx += m_x;
    ty += m_y;

    if (hasBoxDecorations() && paintInfo.phase != PaintPhaseOutline && paintInfo.phase != PaintPhaseSelfOutline)
        paintBoxDecorations(paintInfo, tx, ty);

    if ((paintInfo.phase == PaintPhaseOutline || paintInfo.phase == PaintPhaseSelfOutline)
            && style()->outlineWidth() && style()->visibility() == VISIBLE)
        paintOutline(paintInfo.context, tx, ty, width(), height(), style());

    if (paintInfo.phase != PaintPhaseForeground && paintInfo.phase != PaintPhaseSelection)
        return;

    if (!shouldPaintWithinRoot(paintInfo))
        return;

    {
        int cWidth = contentWidth();
        int cHeight = contentHeight();
        int leftBorder = borderLeft();
        int topBorder = borderTop();
        int leftPad = paddingLeft();
        int topPad = paddingTop();
        IntRect contentRect(IntPoint(tx + leftBorder + leftPad, ty + topBorder + topPad),
                            IntSize(cWidth, cHeight));

        if (cWidth <= 0 || cHeight <= 0)
            return;

        HTMLMediaElement* media = mediaElement();

        /* Live decoded frame first: a playing <video> must show the current
         * picture, not a poster. The buffer is reused across paints: a frame
         * is a few hundred KB and the frame loop runs at 30 Hz. */
        if (media) {
            int frameWidth = 0;
            int frameHeight = 0;

            if (media->copyVideoFrame(m_frame, frameWidth, frameHeight)
                    && frameWidth > 0 && frameHeight > 0
                    && m_frame.size() >= (size_t)frameWidth * frameHeight * 4) {
                if (getenv("AGB_MEDIA_DEBUG") && !m_loggedFrame) {
                    m_loggedFrame = true;
                    fprintf(stderr, "[media] first frame %dx%d -> box %dx%d\n",
                            frameWidth, frameHeight, contentRect.width(), contentRect.height());
                }
                paintDecodedFrame(paintInfo.context, contentRect, m_frame, frameWidth, frameHeight);
                return;
            }
        }

        {
            CachedImage* image = media ? media->activeImage() : 0;

            if (image && image->image() && !image->image()->isNull()) {
                /* The still (poster or decoded frame) fills the content box.
                 * object-fit is recorded but not implemented yet (see
                 * docs/arch-media.md section 8). */
                paintInfo.context->drawImage(image->image(), contentRect, CompositeSourceOver,
                                             document()->page()->inLowQualityImageInterpolationMode());
                return;
            }
        }

        /* Chrome paints an empty <video> as a black box. */
        paintInfo.context->fillRect(contentRect, Color(0, 0, 0));
    }
}

/* BGRA pixels straight from the decoder: the frame is wrapped as a ps_image
 * (no copy; the decoder buffer lives for the duration of the paint) and filled
 * into the content box with picasso's image source. The source pixels are
 * scaled here first, exactly like ImagePS does for <img>: picasso's sampler
 * renders a scaled source as a ghost, a 1:1 source is pixel exact. Alpha is
 * ignored on purpose - decoder output is opaque, and CSS opacity is applied by
 * the graphics context, not by the frame. */
void RenderMedia::paintDecodedFrame(GraphicsContext* context, const IntRect& destRect,
                                    const Vector<unsigned char>& frame,
                                    int frameWidth, int frameHeight)
{
    ps_context* gc = context ? context->platformContext() : 0;

    if (!gc || frameWidth <= 0 || frameHeight <= 0)
        return;

    {
        /* Destination in device pixels, so a zoomed page still fills the box. */
        ps_point p0 = { destRect.x(), destRect.y() };
        ps_point p1 = { destRect.x() + destRect.width(), destRect.y() + destRect.height() };

        ps_world_to_viewport(gc, &p0);
        ps_world_to_viewport(gc, &p1);

        {
            int dw = (int)(p1.x - p0.x + 0.5f);
            int dh = (int)(p1.y - p0.y + 0.5f);

            if (dw <= 0 || dh <= 0)
                return;

            {
                const unsigned* pixels = (const unsigned*)frame.data();
                unsigned* filtered = 0;

                if (dw != frameWidth || dh != frameHeight) {
                    filtered = (unsigned*)malloc(sizeof(unsigned) * (size_t)dw * (size_t)dh);

                    if (!filtered)
                        return;

                    {
                        int y;

                        for (y = 0; y < dh; ++y) {
                            int y0 = (y * frameHeight) / dh;
                            int y1 = ((y + 1) * frameHeight) / dh;
                            int x;

                            if (y1 <= y0)
                                y1 = y0 + 1;

                            for (x = 0; x < dw; ++x) {
                                int x0 = (x * frameWidth) / dw;
                                int x1 = ((x + 1) * frameWidth) / dw;
                                unsigned a0 = 0, a1 = 0, a2 = 0, a3 = 0, n = 0;
                                int j;

                                if (x1 <= x0)
                                    x1 = x0 + 1;

                                for (j = y0; j < y1; ++j) {
                                    const unsigned* row = pixels + (size_t)j * frameWidth;
                                    int i;

                                    for (i = x0; i < x1; ++i) {
                                        unsigned px = row[i];

                                        a0 += px & 0xff;
                                        a1 += (px >> 8) & 0xff;
                                        a2 += (px >> 16) & 0xff;
                                        a3 += (px >> 24) & 0xff;
                                        ++n;
                                    }
                                }

                                if (!n)
                                    n = 1;

                                filtered[(size_t)y * dw + x] = ((a3 / n) << 24) | ((a2 / n) << 16)
                                                             | ((a1 / n) << 8) | (a0 / n);
                            }
                        }
                    }

                    pixels = filtered;
                }

                {
                    ps_image* image = ps_image_create_with_data((ps_byte*)pixels, pixelFormat(),
                                                                dw, dh, dw * 4);

                    if (image) {
                        ps_composite old = ps_set_composite_operator(gc, COMPOSITE_SRC_OVER);
                        ps_rect drect = { destRect.x(), destRect.y(),
                                          destRect.width(), destRect.height() };

                        ps_set_source_image(gc, image);
                        ps_rectangle(gc, &drect);
                        ps_fill(gc);
                        ps_set_composite_operator(gc, old);

                        ps_image_unref(image);
                    }
                }

                if (filtered)
                    free(filtered);
            }
        }
    }
}

void RenderMedia::layout()
{
    ASSERT(needsLayout());

    {
        IntRect oldBounds;
        IntRect oldOutlineBox;
        bool checkForRepaint = checkForRepaintDuringLayout();

        if (checkForRepaint) {
            oldBounds = absoluteClippedOverflowRect();
            oldOutlineBox = absoluteOutlineBox();
        }

        calcWidth();
        calcHeight();
        adjustOverflowForBoxShadow();

        if (checkForRepaint)
            repaintAfterLayoutIfNeeded(oldBounds, oldOutlineBox);
    }

    setNeedsLayout(false);
}

void RenderMedia::mediaSizeChanged()
{
    IntSize size = mediaIntrinsicSize();

    if (size.isEmpty())
        return;

    if (size == intrinsicSize())
        return;

    setIntrinsicSize(size);

    if (!parent())
        return;

    if (!prefWidthsDirty())
        setPrefWidthsDirty(true);

    {
        IntSize oldSize = IntSize(m_width, m_height);

        calcWidth();
        calcHeight();

        if (oldSize == IntSize(m_width, m_height))
            return;
    }

    if (!selfNeedsLayout())
        setNeedsLayout(true);
}

IntSize RenderMedia::mediaIntrinsicSize() const
{
    HTMLMediaElement* media = mediaElement();

    if (!media)
        return intrinsicSize();

    /* Metadata first, then the poster/frame's own size. Keeping the fallback
     * (300x150) is what makes an element with no resource still lay out. */
    {
        int w = media->videoWidth();
        int h = media->videoHeight();

        if (w > 0 && h > 0)
            return IntSize(w, h);
    }

    {
        CachedImage* image = media->activeImage();

        if (image && image->image() && !image->image()->isNull()) {
            IntSize imageSize = image->imageSize(style()->effectiveZoom());

            if (!imageSize.isEmpty())
                return imageSize;
        }
    }

    return intrinsicSize();
}

void RenderMedia::imageChanged(CachedImage* image)
{
    RenderReplaced::imageChanged(image);
    mediaSizeChanged();
}

} // namespace WebCore
