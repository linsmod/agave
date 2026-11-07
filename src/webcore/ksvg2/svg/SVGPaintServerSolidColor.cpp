/*
    Copyright (C) 2004, 2005, 2006 Nikolas Zimmermann <zimmermann@kde.org>
                  2004, 2005 Rob Buis <buis@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#include "config.h"

#if ENABLE(SVG)
#include "SVGPaintServerSolidColor.h"

#include "Color.h"
#include "GraphicsContext.h"
#include "RenderObject.h"
#include "RenderStyle.h"
#include "SVGPaint.h"
#include "SVGRenderStyle.h"

namespace WebCore {

SVGPaintServer* SVGPaintServerSolidColor::instance()
{
    // Deliberately leaked: the server lives for the whole process and has no state.
    static SVGPaintServerSolidColor* server = new SVGPaintServerSolidColor;
    return server;
}

SVGPaintServer* solidColorPaintServer()
{
    return SVGPaintServerSolidColor::instance();
}

static bool resolveColor(RenderStyle* style, SVGPaintTargetType target, Color& color, float& opacity)
{
    if (!style || !style->svgStyle())
        return false;

    SVGPaint* paint = target == ApplyToFillTargetType ? style->svgStyle()->fillPaint() : style->svgStyle()->strokePaint();
    if (!paint)
        return false;

    opacity = target == ApplyToFillTargetType ? style->svgStyle()->fillOpacity() : style->svgStyle()->strokeOpacity();

    switch (paint->paintType()) {
    case SVGPaint::SVG_PAINTTYPE_RGBCOLOR:
    case SVGPaint::SVG_PAINTTYPE_RGBCOLOR_ICCCOLOR:
    case SVGPaint::SVG_PAINTTYPE_URI_RGBCOLOR:
    case SVGPaint::SVG_PAINTTYPE_URI_RGBCOLOR_ICCCOLOR:
        color = paint->color();
        return true;
    case SVGPaint::SVG_PAINTTYPE_CURRENTCOLOR:
    case SVGPaint::SVG_PAINTTYPE_URI_CURRENTCOLOR:
        color = style->color();
        return true;
    case SVGPaint::SVG_PAINTTYPE_NONE:
    case SVGPaint::SVG_PAINTTYPE_URI:
    default:
        return false;
    }
}

bool SVGPaintServerSolidColor::setup(GraphicsContext*& context, RenderObject* object, SVGPaintTargetType target, bool) const
{
    if (!context || !object)
        return false;

    RenderStyle* style = object->style();
    Color color;
    float opacity = 1.0f;
    if (!resolveColor(style, target, color, opacity))
        return false;

    if (opacity < 1.0f)
        color = Color(color.red(), color.green(), color.blue(), static_cast<int>(color.alpha() * opacity));

    if (target == ApplyToFillTargetType) {
        context->setFillColor(color);
        return true;
    }

    context->setStrokeColor(color);

    // Stroke geometry (width) is part of the paint server in the 2008 design:
    // the graphics context only knows about colors.
    if (style && style->svgStyle()) {
        float width = SVGRenderStyle::cssPrimitiveToLength(object, style->svgStyle()->strokeWidth(), 1.0f);
        if (width <= 0.0f)
            width = 1.0f;
        ps_set_line_width(context->platformContext(), width);
    }
    return true;
}

void SVGPaintServerSolidColor::draw(GraphicsContext* context, RenderObject* object, SVGPaintTargetType target) const
{
    GraphicsContext* ctx = context;
    if (!setup(ctx, object, target, false))
        return;

    if (target == ApplyToFillTargetType)
        context->fillPath();
    else
        context->strokePath();
}

}

#endif // ENABLE(SVG)

// vim:ts=4:noet
