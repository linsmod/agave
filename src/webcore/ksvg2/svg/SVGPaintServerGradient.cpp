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
#include "SVGPaintServerGradient.h"

#include "Color.h"
#include "GraphicsContext.h"
#include "RenderObject.h"
#include "RenderStyle.h"
#include "SVGGradientElement.h"
#include "SVGRenderStyle.h"

namespace WebCore {

SVGGradientStop makeGradientStop(float offset, RGBA32 color)
{
    return SVGGradientStop(offset, color);
}

SVGPaintServerGradient::SVGPaintServerGradient(const SVGGradientElement* element)
    : m_gradientElement(element)
    , m_boundingBoxMode(true)
    , m_spreadMethod(SPREADMETHOD_PAD)
    , m_radius(0.0f)
{
}

SVGPaintServerGradient::~SVGPaintServerGradient()
{
}

void SVGPaintServerGradient::setGradientStops(const Vector<SVGGradientStop>& stops)
{
    m_stops = stops;
}

void SVGPaintServerGradient::setBoundingBoxMode(bool mode)
{
    m_boundingBoxMode = mode;
}

void SVGPaintServerGradient::setGradientSpreadMethod(SVGGradientSpreadMethod method)
{
    m_spreadMethod = method;
}

void SVGPaintServerGradient::setGradientTransform(const AffineTransform& transform)
{
    m_gradientTransform = transform;
}

void SVGPaintServerGradient::setGradientStart(const FloatPoint& point)
{
    m_start = point;
}

void SVGPaintServerGradient::setGradientEnd(const FloatPoint& point)
{
    m_end = point;
}

void SVGPaintServerGradient::setGradientCenter(const FloatPoint& point)
{
    m_center = point;
}

void SVGPaintServerGradient::setGradientFocal(const FloatPoint& point)
{
    m_focal = point;
}

void SVGPaintServerGradient::setGradientRadius(float radius)
{
    m_radius = radius;
}

bool SVGPaintServerGradient::setup(GraphicsContext*& context, RenderObject* object, SVGPaintTargetType target, bool) const
{
    if (!context || !object || !object->style() || !object->style()->svgStyle())
        return false;

    // FIXME: collapse the gradient to its first stop for now.  Real linear/radial
    // shading needs ps_gradient_create_linear/radial plus the objectBoundingBox
    // transform; tracked in docs/plan-roadmap.md ("SVG 渐变/图案").
    Vector<SVGGradientStop> stops;
    if (m_gradientElement)
        stops = m_gradientElement->buildStops();
    if (stops.isEmpty() && !m_stops.isEmpty())
        stops = m_stops;

    Color color(0, 0, 0);
    if (!stops.isEmpty()) {
        RGBA32 rgba = stops.first().color;
        color = Color(rgba);
    }

    float opacity = target == ApplyToFillTargetType ? object->style()->svgStyle()->fillOpacity() : object->style()->svgStyle()->strokeOpacity();
    if (opacity < 1.0f)
        color = Color(color.red(), color.green(), color.blue(), static_cast<int>(color.alpha() * opacity));

    if (target == ApplyToFillTargetType)
        context->setFillColor(color);
    else
        context->setStrokeColor(color);
    return true;
}

void SVGPaintServerGradient::draw(GraphicsContext* context, RenderObject* object, SVGPaintTargetType target) const
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
