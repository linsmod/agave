/*
 * This file is part of the render object implementation for KHTML.
 *
 * Copyright (C) 2003 Apple Computer, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef RenderFlexibleBox_h
#define RenderFlexibleBox_h

#include "RenderBlock.h"

namespace WebCore {

class RenderFlexibleBox : public RenderBlock {
public:
    RenderFlexibleBox(Node*);
    virtual ~RenderFlexibleBox();

    virtual const char* renderName() const;

    virtual void calcPrefWidths();
    void calcHorizontalPrefWidths();
    void calcHorizontalMinPrefWidths();
    void calcVerticalPrefWidths();

    virtual void layoutBlock(bool relayoutChildren);
    void layoutHorizontalBox(bool relayoutChildren);
    void layoutVerticalBox(bool relayoutChildren);
    void layoutFlexLines(bool wrap, bool relayoutChildren);

    virtual bool avoidsFloats() const { return true; }

    virtual bool isFlexibleBox() const { return true; }
    virtual bool isFlexingChildren() const { return m_flexingChildren; }
    virtual bool isStretchingChildren() const { return m_stretchingChildren; }

    void placeChild(RenderObject* child, int x, int y);

protected:
    int allowedChildFlex(RenderObject* child, bool expanding, unsigned group);

    bool hasMultipleLines() const { return style()->boxLines() == MULTIPLE; }
    bool isVertical() const { return style()->boxOrient() == VERTICAL; }
    bool isHorizontal() const { return style()->boxOrient() == HORIZONTAL; }

    bool m_flexingChildren : 1;
    bool m_stretchingChildren : 1;
    /* The height calcHeight() resolved for this box before the flex layout
     * zeroes m_height (spec percentage/specified heights). The cross-axis
     * placement needs it: line stretch and align-items centering work against
     * the container's used height, which is computed after the lines are
     * placed. 0 when nothing was resolved. */
    int m_preFlexHeight;
};

} // namespace WebCore

#endif // RenderFlexibleBox_h
