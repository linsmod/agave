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
#include "HTMLVideoElement.h"

#include "HTMLNames.h"
#include "RenderMedia.h"

namespace WebCore {

using namespace HTMLNames;

/* Chrome's default replaced box for <video>: 300x150 (verified with
 * tools/ua_style_dump.py; see docs/arch-media.md §3). The intrinsic size is
 * passed to the renderer, not to the UA sheet, so width/height/CSS still win. */
static const int defaultVideoWidth = 300;
static const int defaultVideoHeight = 150;

HTMLVideoElement::HTMLVideoElement(Document* doc)
    : HTMLMediaElement(videoTag, doc)
{
}

HTMLVideoElement::HTMLVideoElement(const QualifiedName& tagName, Document* doc)
    : HTMLMediaElement(tagName, doc)
{
}

RenderObject* HTMLVideoElement::createRenderer(RenderArena* arena, RenderStyle* style)
{
    /* Like HTMLCanvasElement: only replaced when the box is actually used;
     * display:none still gets no renderer. */
    if (style->display() == NONE)
        return 0;

    return new (arena) RenderMedia(this, IntSize(defaultVideoWidth, defaultVideoHeight));
}

int HTMLVideoElement::width() const
{
    bool ok;
    int w = getAttribute(widthAttr).toInt(&ok);

    if (ok)
        return w;

    return videoWidth() > 0 ? videoWidth() : defaultVideoWidth;
}

void HTMLVideoElement::setWidth(int value)
{
    setAttribute(widthAttr, String::number(value));
}

int HTMLVideoElement::height() const
{
    bool ok;
    int h = getAttribute(heightAttr).toInt(&ok);

    if (ok)
        return h;

    return videoHeight() > 0 ? videoHeight() : defaultVideoHeight;
}

void HTMLVideoElement::setHeight(int value)
{
    setAttribute(heightAttr, String::number(value));
}

} // namespace WebCore
