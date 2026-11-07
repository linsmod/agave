/*
    Copyright (C) 2005, 2006 Nikolas Zimmermann <zimmermann@kde.org>
                  2005, 2006 Rob Buis <buis@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#include "config.h"

#if ENABLE(SVG)
#include "SVGPaintServerPattern.h"

#include "ImageBuffer.h"

namespace WebCore {

SVGPaintServerPattern::SVGPaintServerPattern(const SVGPatternElement* element)
    : m_patternElement(element)
{
}

SVGPaintServerPattern::~SVGPaintServerPattern()
{
}

void SVGPaintServerPattern::setPatternTransform(const AffineTransform& transform)
{
    m_patternTransform = transform;
}

void SVGPaintServerPattern::setPatternBoundaries(const FloatRect& boundaries)
{
    m_patternBoundaries = boundaries;
}

void SVGPaintServerPattern::setTile(std::auto_ptr<ImageBuffer> tile)
{
    m_tile = tile;
}

bool SVGPaintServerPattern::setup(GraphicsContext*&, RenderObject*, SVGPaintTargetType, bool) const
{
    // FIXME: pattern filling needs ImageBuffer-based tile rendering and picasso
    // ps_pattern_create_image; neither is wired yet (see the design doc).
    return false;
}

void SVGPaintServerPattern::draw(GraphicsContext*, RenderObject*, SVGPaintTargetType) const
{
}

}

#endif // ENABLE(SVG)

// vim:ts=4:noet
