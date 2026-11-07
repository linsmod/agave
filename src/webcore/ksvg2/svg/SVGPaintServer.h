/*
    Copyright (C) 2004, 2005, 2006 Nikolas Zimmermann <zimmermann@kde.org>
                  2004, 2005 Rob Buis <buis@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.
*/

#ifndef SVGPaintServer_h
#define SVGPaintServer_h

#if ENABLE(SVG)

#include "SVGResource.h"

namespace WebCore {

class GraphicsContext;
class Path;
class RenderObject;
class RenderStyle;

enum SVGPaintServerType {
    SolidColorPaintServer = 0,
    LinearGradientPaintServer,
    RadialGradientPaintServer,
    PatternPaintServer
};

enum SVGPaintTargetType {
    ApplyToFillTargetType = 0,
    ApplyToStrokeTargetType
};

// A paint server resolves the SVG "fill"/"stroke" paint (a color, currentColor
// or url(#id)) into what the platform draws with.  RenderPath adds the path to
// the context first and then asks the server to draw it.
class SVGPaintServer : public SVGResource {
public:
    static SVGPaintServer* fillPaintServer(const RenderStyle*, const RenderObject*);
    static SVGPaintServer* strokePaintServer(const RenderStyle*, const RenderObject*);

    // Used by the render tree dumper, which has no object to resolve currentColor against.
    static SVGPaintServer* fillPaintServer(const RenderStyle*, const Path*);
    static SVGPaintServer* strokePaintServer(const RenderStyle*, const Path*);

    virtual bool setup(GraphicsContext*&, RenderObject*, SVGPaintTargetType, bool isPaintingText) const = 0;
    virtual void teardown(GraphicsContext*&, RenderObject*, SVGPaintTargetType, bool isPaintingText) const = 0;
    virtual void draw(GraphicsContext*, RenderObject*, SVGPaintTargetType) const = 0;

    virtual SVGPaintServerType type() const = 0;
    virtual bool isPaintServer() const { return true; }
};

}

#endif // ENABLE(SVG)
#endif // SVGPaintServer_h
