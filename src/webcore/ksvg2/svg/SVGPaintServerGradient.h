/*
    Copyright (C) 2005, 2006 Nikolas Zimmermann <zimmermann@kde.org>
                  2005, 2006 Rob Buis <buis@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#ifndef SVGPaintServerGradient_h
#define SVGPaintServerGradient_h

#if ENABLE(SVG)

#include "SVGPaintServer.h"

#include "AffineTransform.h"
#include "Color.h"
#include "FloatPoint.h"
#include "FloatRect.h"
#include <wtf/Vector.h>

namespace WebCore {

class SVGGradientElement;

struct SVGGradientStop {
    SVGGradientStop(float o = 0.0f, RGBA32 c = 0)
        : offset(o)
        , color(c)
    {
    }

    float offset;
    RGBA32 color;
};

SVGGradientStop makeGradientStop(float offset, RGBA32 color);

// The unqualified SPREADMETHOD_* spelling is what GradientAttributes.h uses for
// the global SVGGradientSpreadMethod enum (SVGGradientElement has its own nested
// SVG_SPREADMETHOD_* enum with the same values).
enum SVGGradientSpreadMethod {
    SPREADMETHOD_UNKNOWN = 0,
    SPREADMETHOD_PAD     = 1,
    SPREADMETHOD_REFLECT = 2,
    SPREADMETHOD_REPEAT  = 3
};

// The gradient paint servers keep the 2008 element API (setGradientStops &
// friends) so that SVGGradientElement builds them unchanged.  Rendering is a
// first cut: stop colors are collapsed to the first stop (see draw()).  The
// picasso gradient backend (ps_gradient_create_linear/radial) is the next step.
class SVGPaintServerGradient : public SVGPaintServer {
public:
    explicit SVGPaintServerGradient(const SVGGradientElement* = 0);
    virtual ~SVGPaintServerGradient();

    void setGradientStops(const Vector<SVGGradientStop>&);
    void setBoundingBoxMode(bool);
    void setGradientSpreadMethod(SVGGradientSpreadMethod);
    void setGradientTransform(const AffineTransform&);
    void setGradientStart(const FloatPoint&);
    void setGradientEnd(const FloatPoint&);
    void setGradientCenter(const FloatPoint&);
    void setGradientFocal(const FloatPoint&);
    void setGradientRadius(float);

    virtual bool setup(GraphicsContext*&, RenderObject*, SVGPaintTargetType, bool isPaintingText) const;
    virtual void teardown(GraphicsContext*&, RenderObject*, SVGPaintTargetType, bool) const { }
    virtual void draw(GraphicsContext*, RenderObject*, SVGPaintTargetType) const;

protected:
    const SVGGradientElement* m_gradientElement;
    Vector<SVGGradientStop> m_stops;
    bool m_boundingBoxMode;
    SVGGradientSpreadMethod m_spreadMethod;
    AffineTransform m_gradientTransform;
    FloatPoint m_start;
    FloatPoint m_end;
    FloatPoint m_center;
    FloatPoint m_focal;
    float m_radius;
};

class SVGPaintServerLinearGradient : public SVGPaintServerGradient {
public:
    explicit SVGPaintServerLinearGradient(const SVGGradientElement* element = 0)
        : SVGPaintServerGradient(element)
    {
    }

    virtual SVGPaintServerType type() const { return LinearGradientPaintServer; }
};

class SVGPaintServerRadialGradient : public SVGPaintServerGradient {
public:
    explicit SVGPaintServerRadialGradient(const SVGGradientElement* element = 0)
        : SVGPaintServerGradient(element)
    {
    }

    virtual SVGPaintServerType type() const { return RadialGradientPaintServer; }
};

}

#endif // ENABLE(SVG)
#endif // SVGPaintServerGradient_h
