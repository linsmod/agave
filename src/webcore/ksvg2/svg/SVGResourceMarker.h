/*
    Copyright (C) 2006 Nikolas Zimmermann <zimmermann@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#ifndef SVGResourceMarker_h
#define SVGResourceMarker_h

#if ENABLE(SVG)

#include "FloatRect.h"
#include "SVGResource.h"

#include "AtomicString.h"

namespace WebCore {

class Document;
class GraphicsContext;
class RenderSVGViewportContainer;

// Marker painting is not implemented yet: draw() is a no-op so that the marker
// element can be resolved and the shape's paint path stays intact.  Recorded in
// docs/plan-roadmap.md ("SVG marker").
class SVGResourceMarker : public SVGResource {
public:
    SVGResourceMarker();
    virtual ~SVGResourceMarker();

    void setMarker(RenderSVGViewportContainer*);
    void setAngle(float angle);
    void setAutoAngle();
    void setRef(float refX, float refY);
    void setUseStrokeWidth(bool);

    void draw(GraphicsContext*, const FloatRect&, float x, float y, float strokeWidth, float angle) const;
    FloatRect cachedBounds() const { return m_cachedBounds; }

    virtual void invalidate() { m_cachedBounds = FloatRect(); }

private:
    RenderSVGViewportContainer* m_marker;
    float m_angle;
    float m_refX;
    float m_refY;
    bool m_useStrokeWidth;
    bool m_autoAngle;
    FloatRect m_cachedBounds;
};

SVGResourceMarker* getMarkerById(Document*, const AtomicString&);

}

#endif // ENABLE(SVG)
#endif // SVGResourceMarker_h
