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

#include "config.h"

#if ENABLE(SVG)
#include "SVGPaintServer.h"

#include "Document.h"
#include "Element.h"
#include "RenderObject.h"
#include "RenderStyle.h"
#include "SVGGradientElement.h"
#include "SVGNames.h"
#include "SVGPaint.h"
#include "SVGPaintServerSolidColor.h"
#include "SVGPatternElement.h"
#include "SVGRenderStyle.h"
#include "SVGStyledElement.h"
#include "SVGURIReference.h"

namespace WebCore {

// Resolves url(#id) to the resource element's canvasResource().  Only paint
// servers can come back here; clip-path/mask/filter/marker references are
// resolved by their own ById helpers.
static SVGPaintServer* paintServerByReference(Document* document, SVGPaint* paint)
{
    if (!document)
        return 0;

    String id = SVGURIReference::getTarget(paint->uri());
    if (id.isEmpty())
        return 0;

    Element* element = document->getElementById(AtomicString(id));
    if (!element || !element->isSVGElement())
        return 0;

    if (!element->hasTagName(SVGNames::linearGradientTag) && !element->hasTagName(SVGNames::radialGradientTag)
        && !element->hasTagName(SVGNames::patternTag))
        return 0;

    SVGStyledElement* styled = static_cast<SVGStyledElement*>(element);
    SVGResource* resource = styled->canvasResource();
    if (!resource)
        return 0;

    return static_cast<SVGPaintServer*>(resource);
}

static SVGPaintServer* paintServerFor(const RenderStyle* style, SVGPaintTargetType target, const RenderObject* object, const Path*)
{
    if (!style || !style->svgStyle())
        return 0;

    SVGPaint* paint = target == ApplyToFillTargetType ? style->svgStyle()->fillPaint() : style->svgStyle()->strokePaint();
    if (!paint)
        return 0;

    switch (paint->paintType()) {
    case SVGPaint::SVG_PAINTTYPE_NONE:
        return 0;
    case SVGPaint::SVG_PAINTTYPE_URI:
    case SVGPaint::SVG_PAINTTYPE_URI_NONE:
    case SVGPaint::SVG_PAINTTYPE_URI_CURRENTCOLOR:
    case SVGPaint::SVG_PAINTTYPE_URI_RGBCOLOR:
    case SVGPaint::SVG_PAINTTYPE_URI_RGBCOLOR_ICCCOLOR: {
        // No object (render tree dump) or a dangling reference: only the
        // RGB fallback of the paint value can still be drawn.
        if (!object || !object->document())
            return paint->paintType() == SVGPaint::SVG_PAINTTYPE_URI_RGBCOLOR ? solidColorPaintServer() : 0;

        SVGPaintServer* server = paintServerByReference(object->document(), paint);
        if (server)
            return server;

        if (paint->paintType() == SVGPaint::SVG_PAINTTYPE_URI_RGBCOLOR)
            return solidColorPaintServer();
        return 0;
    }
    default:
        return solidColorPaintServer();
    }
}

SVGPaintServer* SVGPaintServer::fillPaintServer(const RenderStyle* style, const RenderObject* object)
{
    return paintServerFor(style, ApplyToFillTargetType, object, 0);
}

SVGPaintServer* SVGPaintServer::strokePaintServer(const RenderStyle* style, const RenderObject* object)
{
    return paintServerFor(style, ApplyToStrokeTargetType, object, 0);
}

SVGPaintServer* SVGPaintServer::fillPaintServer(const RenderStyle* style, const Path* path)
{
    return paintServerFor(style, ApplyToFillTargetType, 0, path);
}

SVGPaintServer* SVGPaintServer::strokePaintServer(const RenderStyle* style, const Path* path)
{
    return paintServerFor(style, ApplyToStrokeTargetType, 0, path);
}

}

#endif // ENABLE(SVG)

// vim:ts=4:noet
