/*
 * Copyright (C) 2006 Apple Computer, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
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

#include "ImageBuffer.h"

#include "GraphicsContext.h"

namespace WebCore {

static ps_color_format imageColorFormat()
{
    return COLOR_FORMAT_RGBA;
}

ImageBuffer::ImageBuffer(ps_canvas* canvas, ps_image* image, const IntSize& size)
    : m_canvas(canvas)
    , m_image(image)
    , m_size(size)
{
    // ps_context_create() takes its own reference on the canvas; GraphicsContext
    // takes a reference on the context, so drop ours afterwards.
    ps_context* context = ps_context_create(canvas, 0);
    m_context.set(new GraphicsContext(context));
    ps_context_unref(context);
}

std::auto_ptr<ImageBuffer> ImageBuffer::create(const IntSize& size, bool)
{
    int width = size.width() > 0 ? size.width() : 1;
    int height = size.height() > 0 ? size.height() : 1;

    ps_image* image = ps_image_create(imageColorFormat(), width, height);
    if (!image)
        return std::auto_ptr<ImageBuffer>();

    ps_canvas* canvas = ps_canvas_create_from_image(image, 0);
    if (!canvas) {
        ps_image_unref(image);
        return std::auto_ptr<ImageBuffer>();
    }

    std::auto_ptr<ImageBuffer> buffer(new ImageBuffer(canvas, image, IntSize(width, height)));
    return buffer;
}

ImageBuffer::~ImageBuffer()
{
    m_context.clear();
    if (m_canvas)
        ps_canvas_unref(m_canvas);
    if (m_image)
        ps_image_unref(m_image);
}

}

// vim:ts=4:noet
