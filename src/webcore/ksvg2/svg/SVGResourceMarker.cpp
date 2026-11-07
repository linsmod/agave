/*
    Copyright (C) 2006 Nikolas Zimmermann <zimmermann@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#include "config.h"

#if ENABLE(SVG)
#include "SVGResourceMarker.h"

#include "Document.h"
#include "Element.h"
#include "SVGMarkerElement.h"
#include "SVGNames.h"

namespace WebCore {

SVGResourceMarker::SVGResourceMarker()
    : m_marker(0)
    , m_angle(0.0f)
    , m_refX(0.0f)
    , m_refY(0.0f)
    , m_useStrokeWidth(false)
    , m_autoAngle(false)
{
}

SVGResourceMarker::~SVGResourceMarker()
{
}

void SVGResourceMarker::setMarker(RenderSVGViewportContainer* marker)
{
    m_marker = marker;
}

void SVGResourceMarker::setAngle(float angle)
{
    m_autoAngle = false;
    m_angle = angle;
}

void SVGResourceMarker::setAutoAngle()
{
    m_autoAngle = true;
}

void SVGResourceMarker::setRef(float refX, float refY)
{
    m_refX = refX;
    m_refY = refY;
}

void SVGResourceMarker::setUseStrokeWidth(bool use)
{
    m_useStrokeWidth = use;
}

void SVGResourceMarker::draw(GraphicsContext*, const FloatRect&, float, float, float, float) const
{
    // FIXME: render the marker container at (x, y) with the computed angle.
    // Tracked in docs/plan-roadmap.md; drawing nothing is better than crashing.
}

SVGResourceMarker* getMarkerById(Document* document, const AtomicString& id)
{
    if (!document || id.isEmpty())
        return 0;

    Element* element = document->getElementById(id);
    if (!element || !element->isSVGElement() || !element->hasTagName(SVGNames::markerTag))
        return 0;

    return static_cast<SVGResourceMarker*>(static_cast<SVGMarkerElement*>(element)->canvasResource());
}

}

#endif // ENABLE(SVG)

// vim:ts=4:noet
