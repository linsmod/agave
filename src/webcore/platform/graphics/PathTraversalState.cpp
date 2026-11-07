/*
    Copyright (C) 2004, 2005, 2007 Nikolas Zimmermann <zimmermann@kde.org>
                  2004, 2005 Rob Buis <buis@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#include "config.h"

#if ENABLE(SVG)
#include "PathTraversalState.h"

#include <math.h>

namespace WebCore {

double PathTraversalState::distance(const FloatPoint& from, const FloatPoint& to) const
{
    double dx = to.x() - from.x();
    double dy = to.y() - from.y();
    return sqrt(dx * dx + dy * dy);
}

double PathTraversalState::moveTo(const FloatPoint& point)
{
    m_current = point;
    m_start = point;
    m_hasCurrentPoint = true;
    return 0.0;
}

double PathTraversalState::lineTo(const FloatPoint& point)
{
    if (!m_hasCurrentPoint)
        return moveTo(point);

    double length = distance(m_current, point);
    m_current = point;
    return length;
}

double PathTraversalState::quadraticBezierTo(const FloatPoint& cp, const FloatPoint& end)
{
    if (!m_hasCurrentPoint)
        return moveTo(end);

    // Flatten the curve into segments and sum the chord lengths.
    const int segments = 32;
    double length = 0.0;
    FloatPoint previous = m_current;
    for (int i = 1; i <= segments; ++i) {
        double t = static_cast<double>(i) / segments;
        double mt = 1.0 - t;
        double x = mt * mt * m_current.x() + 2.0 * mt * t * cp.x() + t * t * end.x();
        double y = mt * mt * m_current.y() + 2.0 * mt * t * cp.y() + t * t * end.y();
        FloatPoint current(x, y);
        length += distance(previous, current);
        previous = current;
    }
    m_current = end;
    return length;
}

double PathTraversalState::cubicBezierTo(const FloatPoint& cp1, const FloatPoint& cp2, const FloatPoint& end)
{
    if (!m_hasCurrentPoint)
        return moveTo(end);

    const int segments = 48;
    double length = 0.0;
    FloatPoint previous = m_current;
    for (int i = 1; i <= segments; ++i) {
        double t = static_cast<double>(i) / segments;
        double mt = 1.0 - t;
        double x = mt * mt * mt * m_current.x() + 3.0 * mt * mt * t * cp1.x() + 3.0 * mt * t * t * cp2.x() + t * t * t * end.x();
        double y = mt * mt * mt * m_current.y() + 3.0 * mt * mt * t * cp1.y() + 3.0 * mt * t * t * cp2.y() + t * t * t * end.y();
        FloatPoint current(x, y);
        length += distance(previous, current);
        previous = current;
    }
    m_current = end;
    return length;
}

double PathTraversalState::closeSubpath()
{
    if (!m_hasCurrentPoint)
        return 0.0;

    double length = distance(m_current, m_start);
    m_current = m_start;
    return length;
}

}

#endif // ENABLE(SVG)

// vim:ts=4:noet
