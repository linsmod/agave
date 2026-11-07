/*
    Copyright (C) 2006 Nikolas Zimmermann <zimmermann@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#include "config.h"

#if ENABLE(SVG)
#include "SVGResourceClipper.h"

#include "Document.h"
#include "Element.h"
#include "GraphicsContext.h"
#include "SVGClipPathElement.h"
#include "SVGNames.h"

namespace WebCore {

SVGResourceClipper::SVGResourceClipper()
{
}

SVGResourceClipper::~SVGResourceClipper()
{
}

void SVGResourceClipper::resetClipData()
{
    m_clipData.clear();
}

void SVGResourceClipper::addClipData(const Path& path, WindRule rule, bool boundingBoxUnits)
{
    ClipData data;
    data.path = path;
    data.rule = rule;
    data.boundingBoxUnits = boundingBoxUnits;
    m_clipData.append(data);
}

void SVGResourceClipper::applyClip(GraphicsContext* context, const FloatRect& boundingBox) const
{
    if (!context || m_clipData.isEmpty())
        return;

    // FIXME: the SVG spec makes the clip path the union of its children; the
    // Path type in this tree has no addPath(), so apply the children as
    // successive intersections for now (exact for the common single-child
    // case).  Recorded in the design doc.
    for (unsigned i = 0; i < m_clipData.size(); ++i) {
        Path clip = m_clipData[i].path;
        if (m_clipData[i].boundingBoxUnits) {
            AffineTransform transform;
            transform.translate(boundingBox.x(), boundingBox.y());
            transform.scale(boundingBox.width(), boundingBox.height());
            clip.transform(transform);
        }
        if (!clip.isEmpty())
            context->clip(clip);
    }
}

SVGResourceClipper* getClipperById(Document* document, const AtomicString& id)
{
    if (!document || id.isEmpty())
        return 0;

    Element* element = document->getElementById(id);
    if (!element || !element->isSVGElement() || !element->hasTagName(SVGNames::clipPathTag))
        return 0;

    return static_cast<SVGResourceClipper*>(static_cast<SVGClipPathElement*>(element)->canvasResource());
}

}

#endif // ENABLE(SVG)

// vim:ts=4:noet
