/*
    Copyright (C) 2005, 2006 Nikolas Zimmermann <zimmermann@kde.org>
                  2005, 2006 Rob Buis <buis@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#ifndef SVGPaintServerPattern_h
#define SVGPaintServerPattern_h

#if ENABLE(SVG)

#include "SVGPaintServer.h"

#include "AffineTransform.h"
#include "FloatRect.h"
#include <memory>

namespace WebCore {

class ImageBuffer;
class SVGPatternElement;

// Pattern tiling needs ImageBuffer, which this tree does not implement yet.
// The class exists so that SVGPatternElement can build it; setup() reports
// "cannot paint" and the shape stays transparent until patterns are wired.
class SVGPaintServerPattern : public SVGPaintServer {
public:
    explicit SVGPaintServerPattern(const SVGPatternElement* = 0);
    virtual ~SVGPaintServerPattern();

    void setPatternTransform(const AffineTransform&);
    void setPatternBoundaries(const FloatRect&);
    void setTile(std::auto_ptr<ImageBuffer>);

    virtual bool setup(GraphicsContext*&, RenderObject*, SVGPaintTargetType, bool isPaintingText) const;
    virtual void teardown(GraphicsContext*&, RenderObject*, SVGPaintTargetType, bool) const { }
    virtual void draw(GraphicsContext*, RenderObject*, SVGPaintTargetType) const;

    virtual SVGPaintServerType type() const { return PatternPaintServer; }

private:
    const SVGPatternElement* m_patternElement;
    AffineTransform m_patternTransform;
    FloatRect m_patternBoundaries;
    std::auto_ptr<ImageBuffer> m_tile;
};

}

#endif // ENABLE(SVG)
#endif // SVGPaintServerPattern_h
