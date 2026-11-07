/*
    Copyright (C) 2004, 2005, 2006 Nikolas Zimmermann <zimmermann@kde.org>
                  2004, 2005 Rob Buis <buis@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#ifndef SVGPaintServerSolidColor_h
#define SVGPaintServerSolidColor_h

#if ENABLE(SVG)

#include "SVGPaintServer.h"

namespace WebCore {

// Stateless: the color is resolved from the RenderObject's style on every draw,
// so one shared instance serves every element (the 2008 tree had a per-style
// instance, which leaked and could not see inherited style updates).
class SVGPaintServerSolidColor : public SVGPaintServer {
public:
    virtual bool setup(GraphicsContext*&, RenderObject*, SVGPaintTargetType, bool isPaintingText) const;
    virtual void teardown(GraphicsContext*&, RenderObject*, SVGPaintTargetType, bool) const { }
    virtual void draw(GraphicsContext*, RenderObject*, SVGPaintTargetType) const;

    virtual SVGPaintServerType type() const { return SolidColorPaintServer; }

    static SVGPaintServer* instance();
};

SVGPaintServer* solidColorPaintServer();

}

#endif // ENABLE(SVG)
#endif // SVGPaintServerSolidColor_h
