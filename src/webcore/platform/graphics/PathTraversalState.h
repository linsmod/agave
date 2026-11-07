/*
    Copyright (C) 2004, 2005, 2007 Nikolas Zimmermann <zimmermann@kde.org>
                  2004, 2005 Rob Buis <buis@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#ifndef PathTraversalState_h
#define PathTraversalState_h

#if ENABLE(SVG)

#include "FloatPoint.h"

namespace WebCore {

// Walks a path segment by segment and reports the length of each segment.
// SVGPathSegList uses it to find the segment containing a given length
// (path.getPointAtLength() style APIs).  Segment lengths are approximations
// for curves (flattened sampling).
class PathTraversalState {
public:
    enum TraversalAction {
        TraversalSegmentAtLength = 0,
        TraversalPointAtLength,
    };

    PathTraversalState(TraversalAction action, float desiredLength = 0.0f)
        : m_action(action)
        , m_desiredLength(desiredLength)
        , m_totalLength(0.0f)
        , m_segmentIndex(0)
        , m_hasCurrentPoint(false)
    {
    }

    double moveTo(const FloatPoint&);
    double lineTo(const FloatPoint&);
    double quadraticBezierTo(const FloatPoint& cp, const FloatPoint& end);
    double cubicBezierTo(const FloatPoint& cp1, const FloatPoint& cp2, const FloatPoint& end);
    double closeSubpath();

    TraversalAction m_action;
    float m_desiredLength;
    float m_totalLength;
    int m_segmentIndex;

private:
    double distance(const FloatPoint&, const FloatPoint&) const;

    FloatPoint m_current;
    FloatPoint m_start;
    bool m_hasCurrentPoint;
};

}

#endif // ENABLE(SVG)
#endif // PathTraversalState_h
