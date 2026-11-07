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

#ifndef RenderMedia_h
#define RenderMedia_h

#include "RenderReplaced.h"
#include <wtf/Vector.h>

namespace WebCore {

class HTMLMediaElement;

/* The <video> box. Painting is:
 *
 *   poster / decoded frame  ->  when the element has one
 *   otherwise a black box   ->  what Chrome shows for an empty <video>
 *
 * Sizing is inherited from RenderReplaced, so the intrinsic 300x150 and every
 * width/height/CSS rule behave the same way they do for <img> (see
 * docs/arch-media.md §3).
 */
class RenderMedia : public RenderReplaced {
public:
    RenderMedia(HTMLMediaElement*, const IntSize& intrinsicSize);

    virtual const char* renderName() const { return "RenderMedia"; }

    virtual bool isMedia() const { return true; }
    virtual void paint(PaintInfo&, int tx, int ty);
    virtual void layout();

    /* The element's intrinsic size changed (metadata arrived, or a frame was
     * decoded): relayout if the box has no explicit size. */
    void mediaSizeChanged();

    virtual void imageChanged(CachedImage*);

private:
    HTMLMediaElement* mediaElement() const;
    IntSize mediaIntrinsicSize() const;

    /* Draws one decoded BGRA frame into the content box. */
    void paintDecodedFrame(GraphicsContext*, const IntRect& destRect,
                           const Vector<unsigned char>& frame,
                           int frameWidth, int frameHeight);

    /* Last decoded frame, kept between paints (the frame loop runs at 30 Hz
     * and a frame is a few hundred KB). */
    Vector<unsigned char> m_frame;
    bool m_loggedFrame;
};

} // namespace WebCore

#endif
