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

namespace WebCore {

RenderMedia::RenderMedia(HTMLMediaElement* element, const IntSize& intrinsicSize)
    : RenderReplaced(element, intrinsicSize)
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
        CachedImage* image = media ? media->activeImage() : 0;

        if (image && image->image() && !image->image()->isNull()) {
            /* The still (poster or decoded frame) fills the content box. The
             * aspect ratio is NOT preserved here: object-fit is recorded but
             * not implemented yet (docs/arch-media.md §8). */
            paintInfo.context->drawImage(image->image(), contentRect, CompositeSourceOver,
                                         document()->page()->inLowQualityImageInterpolationMode());
        } else {
            /* Chrome paints an empty <video> as a black box. */
            paintInfo.context->fillRect(contentRect, Color(0, 0, 0));
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
