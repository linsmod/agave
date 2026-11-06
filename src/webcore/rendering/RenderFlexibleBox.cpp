/*
 * This file is part of the render object implementation for KHTML.
 *
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
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

#include "config.h"
#include "RenderFlexibleBox.h"

#include "CharacterNames.h"
#include "RenderLayer.h"
#include "RenderView.h"

using namespace std;

namespace WebCore {

/* Defined at the bottom; layout dumps its numbers when AGB_FLEX_DEBUG=1. */
static bool flexDebugEnabled();
static void flexDebugChild(const char* when, RenderObject* box, RenderObject* child);

class FlexBoxIterator {
public:
    FlexBoxIterator(RenderFlexibleBox* parent) {
        box = parent;
        if (box->style()->boxOrient() == HORIZONTAL && box->style()->direction() == RTL)
            forward = box->style()->boxDirection() != BNORMAL;
        else
            forward = box->style()->boxDirection() == BNORMAL;
        lastOrdinal = 1; 
        if (!forward) {
            // No choice, since we're going backwards, we have to find out the highest ordinal up front.
            RenderObject* child = box->firstChild();
            while (child) {
                if (child->style()->boxOrdinalGroup() > lastOrdinal)
                    lastOrdinal = child->style()->boxOrdinalGroup();
                child = child->nextSibling();
            }
        }
        
        reset();
    }

    void reset() {
        current = 0;
        currentOrdinal = forward ? 0 : lastOrdinal+1;
    }

    RenderObject* first() {
        reset();
        return next();
    }
    
    RenderObject* next() {

        do { 
            if (!current) {
                if (forward) {
                    currentOrdinal++; 
                    if (currentOrdinal > lastOrdinal)
                        return 0;
                    current = box->firstChild();
                } else {
                    currentOrdinal--;
                    if (currentOrdinal == 0)
                        return 0;
                    current = box->lastChild();
                }
            }
            else
                current = forward ? current->nextSibling() : current->previousSibling();
            if (current && current->style()->boxOrdinalGroup() > lastOrdinal)
                lastOrdinal = current->style()->boxOrdinalGroup();
        } while (!current || current->style()->boxOrdinalGroup() != currentOrdinal ||
                 current->style()->visibility() == COLLAPSE);
        return current;
    }

private:
    RenderFlexibleBox* box;
    RenderObject* current;
    bool forward;
    unsigned int currentOrdinal;
    unsigned int lastOrdinal;
};
    
RenderFlexibleBox::RenderFlexibleBox(Node* node)
:RenderBlock(node)
{
    setChildrenInline(false); // All of our children must be block-level
    m_flexingChildren = m_stretchingChildren = false;
}

RenderFlexibleBox::~RenderFlexibleBox()
{
}

void RenderFlexibleBox::calcHorizontalPrefWidths()
{
    RenderObject *child = firstChild();
    while (child) {
        // positioned children don't affect the minmaxwidth
        if (child->isPositioned() || child->style()->visibility() == COLLAPSE) {
            child = child->nextSibling();
            continue;
        }

        // A margin basically has three types: fixed, percentage, and auto (variable).
        // Auto and percentage margins simply become 0 when computing min/max width.
        // Fixed margins can be added in as is.
        Length ml = child->style()->marginLeft();
        Length mr = child->style()->marginRight();
        int margin = 0, marginLeft = 0, marginRight = 0;
        if (ml.isFixed())
            marginLeft += ml.value();
        if (mr.isFixed())
            marginRight += mr.value();
        margin = marginLeft + marginRight;

        m_minPrefWidth += child->minPrefWidth() + margin;
        m_maxPrefWidth += child->maxPrefWidth() + margin;

        child = child->nextSibling();
    }    
}

void RenderFlexibleBox::calcVerticalPrefWidths()
{
    RenderObject *child = firstChild();
    while(child != 0)
    {
        // Positioned children and collapsed children don't affect the min/max width
        if (child->isPositioned() || child->style()->visibility() == COLLAPSE) {
            child = child->nextSibling();
            continue;
        }

        // A margin basically has three types: fixed, percentage, and auto (variable).
        // Auto/percentage margins simply become 0 when computing min/max width.
        // Fixed margins can be added in as is.
        Length ml = child->style()->marginLeft();
        Length mr = child->style()->marginRight();
        int margin = 0;
        if (ml.isFixed())
            margin += ml.value();
        if (mr.isFixed())
            margin += mr.value();
        
        int w = child->minPrefWidth() + margin;
        m_minPrefWidth = max(w, m_minPrefWidth);
        
        w = child->maxPrefWidth() + margin;
        m_maxPrefWidth = max(w, m_maxPrefWidth);

        child = child->nextSibling();
    }    
}

void RenderFlexibleBox::calcPrefWidths()
{
    ASSERT(prefWidthsDirty());

    if (style()->width().isFixed() && style()->width().value() > 0)
        m_minPrefWidth = m_maxPrefWidth = calcContentBoxWidth(style()->width().value());
    else {
        m_minPrefWidth = m_maxPrefWidth = 0;

        if (hasMultipleLines() || isVertical())
            calcVerticalPrefWidths();
        else
            calcHorizontalPrefWidths();

        m_maxPrefWidth = max(m_minPrefWidth, m_maxPrefWidth);
    }

    if (style()->minWidth().isFixed() && style()->minWidth().value() > 0) {
        m_maxPrefWidth = max(m_maxPrefWidth, calcContentBoxWidth(style()->minWidth().value()));
        m_minPrefWidth = max(m_minPrefWidth, calcContentBoxWidth(style()->minWidth().value()));
    }
    
    if (style()->maxWidth().isFixed() && style()->maxWidth().value() != undefinedLength) {
        m_maxPrefWidth = min(m_maxPrefWidth, calcContentBoxWidth(style()->maxWidth().value()));
        m_minPrefWidth = min(m_minPrefWidth, calcContentBoxWidth(style()->maxWidth().value()));
    }

    int toAdd = borderLeft() + borderRight() + paddingLeft() + paddingRight();
    m_minPrefWidth += toAdd;
    m_maxPrefWidth += toAdd;

    setPrefWidthsDirty(false);
}

void RenderFlexibleBox::layoutBlock(bool relayoutChildren)
{
    ASSERT(needsLayout());

    if (!relayoutChildren && layoutOnlyPositionedObjects())
        return;

    IntRect oldBounds;
    IntRect oldOutlineBox;
    bool checkForRepaint = checkForRepaintDuringLayout();
    if (checkForRepaint) {
        oldBounds = absoluteClippedOverflowRect();
        oldOutlineBox = absoluteOutlineBox();
    }

    view()->pushLayoutState(this, IntSize(m_x, m_y));

    int previousWidth = m_width;
    int previousHeight = m_height;
    
    calcWidth();
    calcHeight();
    m_overflowWidth = m_width;

    if (previousWidth != m_width || previousHeight != m_height ||
        (parent()->isFlexibleBox() && parent()->style()->boxOrient() == HORIZONTAL &&
         parent()->style()->boxAlign() == BSTRETCH))
        relayoutChildren = true;

    m_height = 0;
    m_overflowHeight = 0;
    m_flexingChildren = m_stretchingChildren = false;

    initMaxMarginValues();

    // For overflow:scroll blocks, ensure we have both scrollbars in place always.
    if (scrollsOverflow()) {
        if (style()->overflowX() == OSCROLL)
            m_layer->setHasHorizontalScrollbar(true);
        if (style()->overflowY() == OSCROLL)
            m_layer->setHasVerticalScrollbar(true);
    }

    if (isHorizontal())
        layoutHorizontalBox(relayoutChildren);
    else
        layoutVerticalBox(relayoutChildren);

    int oldHeight = m_height;
    calcHeight();
    if (oldHeight != m_height) {
        // If the block got expanded in size, then increase our overflowheight to match.
        if (m_overflowHeight > m_height)
            m_overflowHeight -= (borderBottom() + paddingBottom() + horizontalScrollbarHeight());
        if (m_overflowHeight < m_height)
            m_overflowHeight = m_height;
    }
    if (previousHeight != m_height)
        relayoutChildren = true;

    layoutPositionedObjects(relayoutChildren || isRoot());

    if (!isFloatingOrPositioned() && m_height == 0) {
        // We are a block with no border and padding and a computed height
        // of 0.  The CSS spec states that zero-height blocks collapse their margins
        // together.
        // When blocks are self-collapsing, we just use the top margin values and set the
        // bottom margin max values to 0.  This way we don't factor in the values
        // twice when we collapse with our previous vertically adjacent and
        // following vertically adjacent blocks.
        int pos = maxTopPosMargin();
        int neg = maxTopNegMargin();
        if (maxBottomPosMargin() > pos)
            pos = maxBottomPosMargin();
        if (maxBottomNegMargin() > neg)
            neg = maxBottomNegMargin();
        setMaxTopMargins(pos, neg);
        setMaxBottomMargins(0, 0);
    }

    // Always ensure our overflow width is at least as large as our width.
    if (m_overflowWidth < m_width)
        m_overflowWidth = m_width;

    if (!hasOverflowClip()) {
        if (ShadowData* boxShadow = style()->boxShadow()) {
            m_overflowLeft = min(m_overflowLeft, boxShadow->x - boxShadow->blur);
            m_overflowWidth = max(m_overflowWidth, m_width + boxShadow->x + boxShadow->blur);
            m_overflowTop = min(m_overflowTop, boxShadow->y - boxShadow->blur);
            m_overflowHeight = max(m_overflowHeight, m_height + boxShadow->y + boxShadow->blur);
        }
    }

    view()->popLayoutState();

    if (flexDebugEnabled()) {
        fprintf(stderr, "[flex] block-end box=%p w=%d h=%d\n", (void*)this, m_width, m_height);
        for (RenderObject* c = firstChild(); c; c = c->nextSibling())
            flexDebugChild("end", this, c);
    }

    // Update our scrollbars if we're overflow:auto/scroll/hidden now that we know if
    // we overflow or not.
    if (hasOverflowClip())
        m_layer->updateScrollInfoAfterLayout();

    // Repaint with our new bounds if they are different from our old bounds.
    if (checkForRepaint)
        repaintAfterLayoutIfNeeded(oldBounds, oldOutlineBox);
    
    setNeedsLayout(false);
}


/* Horizontal flex: the one line algorithm used by both `flex-wrap: nowrap`
 * (one line) and `wrap` (several lines).
 *
 * The 2008 single-line code laid every child out at its plain block width and
 * then grew/shrunk them by `box-flex`, which is two degrees off the spec: it
 * ignores `flex-basis` (so `flex: 2` on a text-heavy item loses space to a
 * narrower `flex: 1` sibling — css3test `#content` came out reversed) and it
 * weights shrinking by grow factor instead of `flex-shrink` x base size.
 * Start from the basis, clamp at min/max, distribute the free space. */
void RenderFlexibleBox::layoutFlexLines(bool wrap, bool relayoutChildren)
{
    struct LineItem {
        RenderObject* child;
        int contentWidth;   /* target content-box width */
        int outerWidth;     /* margin box width */
        int outerHeight;    /* margin box height */
        int minContent;     /* content-box floor (min-width / min-content) */
        int maxContent;     /* content-box ceiling (max-width), INT_MAX if none */
        int extraWidth;     /* border + padding + horizontal margins */
        int breakOuterWidth; /* outer size with min/max applied, used for line breaking */
        float grow;
        float shrink;
        int marginLeft, marginRight, marginTop, marginBottom;
        int line;
        bool frozen;
        int violationSign;  /* +1 clamped up to min, -1 clamped down to max, 0 none */
    };

    const int contentLeft = borderLeft() + paddingLeft();
    const int innerWidth = max(0, contentWidth());
    const int columnGap = max(0, style()->flexColumnGap().calcValue(innerWidth));
    const int rowGap = max(0, style()->flexRowGap().calcValue(max(0, height() - borderTop() - borderBottom() - paddingTop() - paddingBottom())));

    Vector<LineItem> items;

    /* ---- pass 1: base size of every item (flex-basis), its floor/ceiling,
     * then line breaking. An explicit flex-basis wins; an auto basis uses the
     * specified width, or the max-content size when the width is auto (a text
     * run should not claim the whole row). */
    FlexBoxIterator iterator(this);
    for (RenderObject* child = iterator.first(); child; child = iterator.next()) {
        if (child->isPositioned()) {
            child->containingBlock()->insertPositionedObject(child);
            if (child->hasStaticX())
                child->setStaticX(contentLeft);
            if (child->hasStaticY())
                child->setStaticY(borderTop() + paddingTop());
            continue;
        }

        if (relayoutChildren)
            child->setChildNeedsLayout(true, false);

        child->setOverrideSize(-1);
        child->calcVerticalMargins();
        child->layoutIfNeeded();

        int bordersPlusPadding = child->borderLeft() + child->borderRight()
                               + child->paddingLeft() + child->paddingRight();
        Length basis = child->style()->flexBasis();
        int contentW;

        if (!basis.isAuto())
            contentW = max(0, basis.calcValue(innerWidth));
        else if (child->style()->width().isAuto())
            contentW = max(0, child->maxPrefWidth() - bordersPlusPadding);
        else
            contentW = max(0, child->width() - bordersPlusPadding);

        /* Floor and ceiling. `min-width: auto` (the initial value) keeps an
         * item at or above its min-content size; `min-width: 0` is the escape
         * hatch authors write to opt out of that clamp, so it must not. */
        int minContent = 0;
        Length minWidth = child->style()->minWidth();
        if (child->style()->minWidthIsAuto()) {
            minContent = max(0, child->minPrefWidth() - bordersPlusPadding);
            /* With a specified width, calcPrefWidths() short-circuits and
             * reports that width as the min-content size, so the clamp would
             * never let the item shrink. The automatic minimum really follows
             * the content, which is not tracked separately here; letting such
             * an item shrink matches Chrome for content that fits its width. */
            if (!child->style()->width().isAuto())
                minContent = 0;
        } else if (minWidth.isFixed())
            minContent = max(0, minWidth.value() - bordersPlusPadding);
        else if (!minWidth.isAuto())
            minContent = max(0, minWidth.calcValue(innerWidth) - bordersPlusPadding);

        int maxContent = INT_MAX;
        Length maxWidth = child->style()->maxWidth();
        if (maxWidth.isFixed() && !maxWidth.isUndefined())
            maxContent = max(0, maxWidth.value() - bordersPlusPadding);
        else if (maxWidth.isPercent() || maxWidth.isCalculated())
            maxContent = max(0, maxWidth.calcValue(innerWidth) - bordersPlusPadding);
        else if (maxWidth.type() == Intrinsic)
            maxContent = max(0, child->maxPrefWidth() - bordersPlusPadding);
        else if (maxWidth.type() == MinIntrinsic)
            maxContent = max(0, child->minPrefWidth() - bordersPlusPadding);

        if (minContent > maxContent)
            minContent = maxContent;

        LineItem item;
        item.child = child;
        item.contentWidth = contentW;
        item.breakOuterWidth = max(minContent, min(contentW, maxContent)) + bordersPlusPadding
                             + max(0, child->marginLeft()) + max(0, child->marginRight());
        item.minContent = minContent;
        item.maxContent = maxContent;
        item.marginLeft = child->marginLeft();
        item.marginRight = child->marginRight();
        item.marginTop = child->marginTop();
        item.marginBottom = child->marginBottom();
        item.outerWidth = contentW + bordersPlusPadding + item.marginLeft + item.marginRight;
        item.extraWidth = bordersPlusPadding + item.marginLeft + item.marginRight;
        item.outerHeight = child->height() + item.marginTop + item.marginBottom;
        item.grow = max(0.0f, child->style()->boxFlex());
        item.shrink = max(0.0f, child->style()->flexShrink());
        item.line = 0;
        item.frozen = false;
        item.violationSign = 0;

        items.append(item);
    }

    int line = 0;
    int lineWidth = 0;
    for (unsigned i = 0; i < items.size(); ++i) {
        int outer = items[i].breakOuterWidth;
        int gap = lineWidth > 0 ? columnGap : 0;

        if (wrap && lineWidth > 0 && lineWidth + gap + outer > innerWidth) {
            ++line;
            lineWidth = 0;
            gap = 0;
        }
        items[i].line = line;
        lineWidth += gap + outer;
    }
    const int lineCount = items.isEmpty() ? 0 : line + 1;

    /* ---- pass 2: distribute free space inside each line (flex-grow /
     * flex-shrink) and re-lay the items out at their final widths. */
    m_flexingChildren = true;

    for (int l = 0; l < lineCount; ++l) {
        int sumOuter = 0;
        int count = 0;

        for (unsigned i = 0; i < items.size(); ++i) {
            if (items[i].line != l)
                continue;
            sumOuter += items[i].outerWidth;
            ++count;
        }

        int freeSpace = innerWidth - sumOuter - columnGap * (count > 0 ? count - 1 : 0);

        /* Resolve flexible lengths (spec §9.7 in miniature). The base size is
         * the flex-basis; free space is shared by the flex factors, then each
         * item is clamped to its floor/ceiling and the items that got clamped
         * freeze — the rest is re-shared next pass. Clamping at the *end* (and
         * not before distribution) is what keeps `flex: 2` / `flex: 1` at 2:1
         * while still honouring a sibling's min-content floor. */
        if (freeSpace != 0 && count > 0) {
            bool expanding = freeSpace > 0;

            for (unsigned i = 0; i < items.size(); ++i) {
                if (items[i].line != l)
                    continue;
                if ((expanding ? items[i].grow : items[i].shrink) <= 0.0f)
                    items[i].frozen = true;
            }

            for (int pass = 0; pass < count + 1; ++pass) {
                int remaining = innerWidth - columnGap * (count > 0 ? count - 1 : 0);
                float factorSum = 0.0f;
                for (unsigned i = 0; i < items.size(); ++i) {
                    const LineItem& it = items[i];
                    if (it.line != l)
                        continue;
                    remaining -= it.outerWidth;
                    if (it.frozen)
                        continue;
                    factorSum += expanding ? it.grow : it.shrink * it.outerWidth;
                }
                if (factorSum <= 0.0f || !remaining)
                    break;

                /* One sequential pass: each item takes its cut of what is
                 * still unassigned, so rounding never drifts. */
                float factorLeft = factorSum;
                for (unsigned i = 0; i < items.size(); ++i) {
                    LineItem& it = items[i];
                    if (it.line != l || it.frozen)
                        continue;
                    float factor = expanding ? it.grow : it.shrink * it.outerWidth;
                    if (factor <= 0.0f)
                        continue;
                    int share = (int)(remaining * (factor / factorLeft));
                    factorLeft -= factor;
                    remaining -= share;
                    it.contentWidth = max(0, it.contentWidth + share);
                    it.outerWidth = it.contentWidth + it.extraWidth;
                }

                /* Clamp and remember who moved: min violations (positive) have
                 * to freeze in a growing line, max violations in a shrinking
                 * one; a pass with no violation is the answer. */
                int totalViolation = 0;
                for (unsigned i = 0; i < items.size(); ++i) {
                    LineItem& it = items[i];
                    if (it.line != l || it.frozen)
                        continue;
                    int clamped = max(it.minContent, min(it.contentWidth, it.maxContent));
                    int violation = clamped - it.contentWidth;
                    it.violationSign = 0;
                    if (violation) {
                        it.contentWidth = clamped;
                        it.outerWidth = it.contentWidth + it.extraWidth;
                        totalViolation += violation;
                        it.violationSign = violation > 0 ? 1 : -1;
                    }
                }
                if (!totalViolation)
                    break;

                /* A positive total means min violations dominated: freeze just
                 * those and let the others absorb the difference next pass. */
                int freezeSign = totalViolation > 0 ? 1 : -1;
                for (unsigned i = 0; i < items.size(); ++i) {
                    LineItem& it = items[i];
                    if (it.line != l || it.frozen)
                        continue;
                    if (it.violationSign == freezeSign)
                        it.frozen = true;
                }
            }
        }
    }

    /* Re-layout with the final widths. `m_flexingChildren` is what makes
     * RenderBox::calcWidth() take the override size at all, so it has to stay
     * set until every child layout below (including the stretch pass) is done. */
    m_flexingChildren = true;
    for (unsigned i = 0; i < items.size(); ++i) {
        RenderObject* child = items[i].child;
        int borderBox = items[i].contentWidth + child->borderLeft() + child->borderRight()
                      + child->paddingLeft() + child->paddingRight();

        child->setOverrideSize(borderBox);
        child->setChildNeedsLayout(true, false);
        child->layoutIfNeeded();
        items[i].outerWidth = child->width() + child->marginLeft() + child->marginRight();
        items[i].outerHeight = child->height() + child->marginTop() + child->marginBottom();
    }

    if (flexDebugEnabled()) {
        fprintf(stderr, "[flex] lines box=%p inner=%d wrap=%d lines=%d\n",
                (void*)this, innerWidth, (int)wrap, lineCount);
        for (unsigned i = 0; i < items.size(); ++i) {
            flexDebugChild("line", this, items[i].child);
            if (flexDebugEnabled())
                fprintf(stderr, "[flex]   item %p base=%d min=%d max=%d final=%d extra=%d\n",
                        (void*)items[i].child, items[i].contentWidth - 0, items[i].minContent,
                        items[i].maxContent, static_cast<RenderBox*>(items[i].child)->width(),
                        items[i].extraWidth);
        }
    }

    const int contentTop = borderTop() + paddingTop();

    /* align-items: stretch — an item without an explicit height fills the line.
     * The 2008 children read the parent's content height through
     * isStretchingChildren(), so the container height has to be settled first;
     * that is the order the old single-line code used. */
    if (style()->boxAlign() == BSTRETCH && !items.isEmpty()) {
        int tallest = 0;
        for (unsigned i = 0; i < items.size(); ++i)
            tallest = max(tallest, items[i].outerHeight);
        m_height = contentTop + tallest + borderBottom() + paddingBottom() + horizontalScrollbarHeight();
        calcHeight();
        m_stretchingChildren = true;
        for (unsigned i = 0; i < items.size(); ++i) {
            RenderObject* child = items[i].child;
            if (!child->style()->height().isAuto())
                continue;
            child->setChildNeedsLayout(true, false);
            child->layoutIfNeeded();
            items[i].outerHeight = child->height() + child->marginTop() + child->marginBottom();
        }
        m_stretchingChildren = false;
    }

    m_flexingChildren = false;

    /* ---- pass 3: vertical placement (line heights, align-items,
     * align-content) and horizontal placement (justify-content). */
    Vector<int> lineHeights;
    lineHeights.resize(lineCount);
    for (int l = 0; l < lineCount; ++l)
        lineHeights[l] = 0;
    for (unsigned i = 0; i < items.size(); ++i)
        lineHeights[items[i].line] = max(lineHeights[items[i].line], items[i].outerHeight);

    int totalHeight = 0;
    for (int l = 0; l < lineCount; ++l)
        totalHeight += lineHeights[l] + (l > 0 ? rowGap : 0);

    /* m_height still holds the previous layout's result here; a specified
     * height is the authority for align-content. */
    int containerHeight = style()->height().isFixed()
        ? style()->height().value()
        : height() - borderTop() - borderBottom() - paddingTop() - paddingBottom();
    int lineGap = rowGap;
    int alignOffset = 0;

    if (lineCount > 0 && containerHeight > totalHeight) {
        int freeV = containerHeight - totalHeight;

        switch (style()->alignContent()) {
        case BCENTER:
            alignOffset = freeV / 2;
            break;
        case BEND:
            alignOffset = freeV;
            break;
        case BJUSTIFY:
            if (lineCount > 1)
                lineGap += freeV / (lineCount - 1);
            break;
        case BSTRETCH:
            /* Default align-content: the single line grows to the container,
             * so align-items then centers inside the whole box. */
            for (int l = 0; l < lineCount; ++l)
                lineHeights[l] += freeV / lineCount;
            break;
        default:
            break;
        }
    }

    int yPos = contentTop + alignOffset;
    int overflowRight = contentLeft;

    for (int l = 0; l < lineCount; ++l) {
        int sumOuter = 0;
        int count = 0;
        for (unsigned i = 0; i < items.size(); ++i) {
            if (items[i].line != l)
                continue;
            sumOuter += items[i].outerWidth;
            ++count;
        }

        int baseGap = columnGap;
        int xPos = contentLeft;
        int free = innerWidth - sumOuter - columnGap * (count > 0 ? count - 1 : 0);

        switch (style()->boxPack()) {
        case BCENTER:
            if (free > 0)
                xPos += free / 2;
            break;
        case BEND:
            if (free > 0)
                xPos += free;
            break;
        case BJUSTIFY:
            if (count > 1 && free > 0)
                baseGap += free / (count - 1);
            break;
        default:
            break;
        }

        bool firstInLine = true;
        for (unsigned i = 0; i < items.size(); ++i) {
            if (items[i].line != l)
                continue;

            if (!firstInLine)
                xPos += baseGap;
            firstInLine = false;

            int childY = yPos;
            switch (style()->boxAlign()) {
            case BCENTER:
                childY += (lineHeights[l] - items[i].outerHeight) / 2;
                break;
            case BEND:
                childY += lineHeights[l] - items[i].outerHeight;
                break;
            default:
                break;
            }

            placeChild(items[i].child, xPos + items[i].marginLeft, childY + items[i].marginTop);
            if (items[i].child->isRenderBlock())
                static_cast<RenderBlock*>(items[i].child)->addVisualOverflow(static_cast<RenderBlock*>(items[i].child)->floatRect());

            xPos += items[i].outerWidth;
            overflowRight = max(overflowRight, xPos);
        }

        yPos += lineHeights[l] + lineGap;
    }

    int contentBottom = yPos - (lineCount > 0 ? lineGap : 0);

    m_height = contentBottom + borderBottom() + paddingBottom() + horizontalScrollbarHeight();
    m_overflowWidth = max(m_overflowWidth, overflowRight);
    m_overflowHeight = max(contentBottom, m_height);
    m_overflowTop = 0;

    calcHeight();
}

/* `AGB_FLEX_DEBUG=1` dumps the numbers the flex algorithms work with: the
 * single-line (2008) path grows children from their *preferred* width, so a
 * content-heavy item can end up wider than a `flex: 2` sibling (css3test
 * `#content`: `#tests{flex:2}` came out narrower than `aside{flex:1}`).
 * Off unless the variable is set, so it can stay in the tree. */
static bool flexDebugEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("AGB_FLEX_DEBUG") ? 1 : 0;
    return enabled != 0;
}

static void flexDebugChild(const char* when, RenderObject* box, RenderObject* child)
{
    if (!flexDebugEnabled())
        return;

    RenderStyle* s = child->style();
    Length basis = s->flexBasis();
    int w = static_cast<RenderBox*>(child)->width();
    fprintf(stderr, "[flex] %s box=%p child=%p %s grow=%g shrink=%g basis=%s(%d) w=%d over=%d minPref=%d maxPref=%d minW=%d maxW=%d\n",
            when, (void*)box, (void*)child, child->renderName(),
            (double)s->boxFlex(), (double)s->flexShrink(),
            basis.isAuto() ? "auto" : "fixed", basis.isAuto() ? -1 : basis.calcValue(0),
            w, child->overrideWidth(), child->minPrefWidth(), child->maxPrefWidth(),
            s->minWidth().isFixed() ? s->minWidth().value() : -1,
            s->maxWidth().isUndefined() ? -1 : 0);
}

void RenderFlexibleBox::layoutHorizontalBox(bool relayoutChildren)
{
    /* LTR rows all go through the line algorithm in layoutFlexLines(); the
     * 2008 code below grows children from their plain block width and ignores
     * flex-basis, which reverses `flex: 2` / `flex: 1` rows with uneven content
     * (css3test `#content`). RTL keeps the 2008 placement for now: the line
     * placer only walks left-to-right. */
    if (style()->direction() == LTR) {
        layoutFlexLines(style()->boxLines() == MULTIPLE, relayoutChildren);
        return;
    }

    int toAdd = borderBottom() + paddingBottom() + horizontalScrollbarHeight();
    int yPos = borderTop() + paddingTop();
    int xPos = borderLeft() + paddingLeft();
    bool heightSpecified = false;
    int oldHeight = 0;
    
    unsigned int highestFlexGroup = 0;
    unsigned int lowestFlexGroup = 0;
    bool haveFlex = false;
    int remainingSpace = 0;
    m_overflowHeight = m_height;

    // The first walk over our kids is to find out if we have any flexible children.
    FlexBoxIterator iterator(this);
    RenderObject* child = iterator.next();
    while (child) {
        flexDebugChild("scan", this, child);
        // Check to see if this child flexes.
        if (!child->isPositioned() && child->style()->boxFlex() > 0.0f) {
            // We always have to lay out flexible objects again, since the flex distribution
            // may have changed, and we need to reallocate space.
            child->setOverrideSize(-1);
            if (!relayoutChildren)
                child->setChildNeedsLayout(true, false);
            haveFlex = true;
            unsigned int flexGroup = child->style()->boxFlexGroup();
            if (lowestFlexGroup == 0)
                lowestFlexGroup = flexGroup;
            if (flexGroup < lowestFlexGroup)
                lowestFlexGroup = flexGroup;
            if (flexGroup > highestFlexGroup)
                highestFlexGroup = flexGroup;
        }
        child = iterator.next();
    }
    
    /* A wrapping row without flexible children uses the line layout below; the
     * single-line code has no concept of lines. */
    if (!haveFlex && style()->boxLines() == MULTIPLE) {
        layoutFlexLines(true, relayoutChildren);
        return;
    }

    // We do 2 passes.  The first pass is simply to lay everyone out at
    // their preferred widths.  The second pass handles flexing the children.
    do {
        // Reset our height.
        m_height = yPos;
        m_overflowHeight = m_height;
        xPos = borderLeft() + paddingLeft();
                
        // Our first pass is done without flexing.  We simply lay the children
        // out within the box.  We have to do a layout first in order to determine
        // our box's intrinsic height.
        int maxAscent = 0, maxDescent = 0;
        child = iterator.first();
        while (child) {
            // make sure we relayout children if we need it.
            if (relayoutChildren || (child->isReplaced() && (child->style()->width().isPercent() || child->style()->height().isPercent())))
                child->setChildNeedsLayout(true, false);
            
            if (child->isPositioned()) {
                child = iterator.next();
                continue;
            }
    
            // Compute the child's vertical margins.
            child->calcVerticalMargins();
    
            // Now do the layout.
            child->layoutIfNeeded();
    
            // Update our height and overflow height.
            if (style()->boxAlign() == BBASELINE) {
                int ascent = child->marginTop() + child->getBaselineOfFirstLineBox();
                if (ascent == -1)
                    ascent = child->marginTop() + child->height() + child->marginBottom();
                int descent = (child->marginTop() + child->height() + child->marginBottom()) - ascent;
                
                // Update our maximum ascent.
                maxAscent = max(maxAscent, ascent);
                
                // Update our maximum descent.
                maxDescent = max(maxDescent, descent);
                
                // Now update our height.
                m_height = max(yPos + maxAscent + maxDescent, m_height);
            }
            else
                m_height = max(m_height, yPos + child->marginTop() + child->height() + child->marginBottom());

            child = iterator.next();
        }
        
        if (!iterator.first() && hasLineIfEmpty())
            m_height += lineHeight(true, true);
        
        m_height += toAdd;

        // Always make sure our overflowheight is at least our height.
        if (m_overflowHeight < m_height)
            m_overflowHeight = m_height;
        
        oldHeight = m_height;
        calcHeight();

        relayoutChildren = false;
        if (oldHeight != m_height)
            heightSpecified = true;
        
        // Now that our height is actually known, we can place our boxes.
        m_stretchingChildren = (style()->boxAlign() == BSTRETCH);
        bool firstInFlowChild = true;
        child = iterator.first();
        while (child) {
            if (child->isPositioned()) {
                child->containingBlock()->insertPositionedObject(child);
                if (child->hasStaticX()) {
                    if (style()->direction() == LTR)
                        child->setStaticX(xPos);
                    else child->setStaticX(width() - xPos);
                }
                if (child->hasStaticY())
                    child->setStaticY(yPos);
                child = iterator.next();
                continue;
            }
    
            // We need to see if this child's height has changed, since we make block elements
            // fill the height of a containing box by default.
            // Now do a layout.
            int oldChildHeight = child->height();
            static_cast<RenderBox*>(child)->calcHeight();
            if (oldChildHeight != child->height())
                child->setChildNeedsLayout(true, false);
            child->layoutIfNeeded();
    
            // We can place the child now, using our value of box-align.
            /* flex gap: space between in-flow items (not before the first one) */
            if (!firstInFlowChild)
                xPos += style()->flexColumnGap().calcValue(contentWidth());
            firstInFlowChild = false;

            xPos += child->marginLeft();
            int childY = yPos;
            switch (style()->boxAlign()) {
                case BCENTER:
                    childY += child->marginTop() + max(0, (contentHeight() - (child->height() + child->marginTop() + child->marginBottom()))/2);
                    break;
                case BBASELINE: {
                    int ascent = child->marginTop() + child->getBaselineOfFirstLineBox();
                    if (ascent == -1)
                        ascent = child->marginTop() + child->height() + child->marginBottom();
                    childY += child->marginTop() + (maxAscent - ascent);
                    break;
                }
                case BEND:
                    childY += contentHeight() - child->marginBottom() - child->height();
                    break;
                default: // BSTART
                    childY += child->marginTop();
                    break;
            }

            placeChild(child, xPos, childY);

            if (child->isRenderBlock())
                static_cast<RenderBlock*>(child)->addVisualOverflow(static_cast<RenderBlock*>(child)->floatRect());

            m_overflowHeight = max(m_overflowHeight, childY + child->overflowHeight(false));
            m_overflowTop = min(m_overflowTop, child->yPos() + child->overflowTop(false));
            
            xPos += child->width() + child->marginRight();
    
            child = iterator.next();
        }

        remainingSpace = borderLeft() + paddingLeft() + contentWidth() - xPos;
        
        m_stretchingChildren = false;
        if (m_flexingChildren)
            haveFlex = false; // We're done.
        else if (haveFlex) {
            // We have some flexible objects.  See if we need to grow/shrink them at all.
            if (!remainingSpace)
                break;

            // Allocate the remaining space among the flexible objects.  If we are trying to
            // grow, then we go from the lowest flex group to the highest flex group.  For shrinking,
            // we go from the highest flex group to the lowest group.
            bool expanding = remainingSpace > 0;
            unsigned int start = expanding ? lowestFlexGroup : highestFlexGroup;
            unsigned int end = expanding? highestFlexGroup : lowestFlexGroup;
            for (unsigned int i = start; i <= end && remainingSpace; i++) {
                // Always start off by assuming the group can get all the remaining space.
                int groupRemainingSpace = remainingSpace;
                do {
                    // Flexing consists of multiple passes, since we have to change ratios every time an object hits its max/min-width
                    // For a given pass, we always start off by computing the totalFlex of all objects that can grow/shrink at all, and
                    // computing the allowed growth before an object hits its min/max width (and thus
                    // forces a totalFlex recomputation).
                    int groupRemainingSpaceAtBeginning = groupRemainingSpace;
                    float totalFlex = 0.0f;
                    child = iterator.first();
                    while (child) {
                        if (allowedChildFlex(child, expanding, i))
                            totalFlex += child->style()->boxFlex();
                        child = iterator.next();
                    }
                    child = iterator.first();
                    int spaceAvailableThisPass = groupRemainingSpace;
                    while (child) {
                        int allowedFlex = allowedChildFlex(child, expanding, i);
                        if (allowedFlex) {
                            int projectedFlex = (allowedFlex == INT_MAX) ? allowedFlex : (int)(allowedFlex * (totalFlex / child->style()->boxFlex()));
                            spaceAvailableThisPass = expanding ? min(spaceAvailableThisPass, projectedFlex) : max(spaceAvailableThisPass, projectedFlex);
                        }
                        child = iterator.next();
                    }

                    // The flex groups may not have any flexible objects this time around. 
                    if (!spaceAvailableThisPass || totalFlex == 0.0f) {
                        // If we just couldn't grow/shrink any more, then it's time to transition to the next flex group.
                        groupRemainingSpace = 0;
                        continue;
                    }

                    // Now distribute the space to objects.
                    child = iterator.first();
                    while (child && spaceAvailableThisPass && totalFlex) {
                        if (allowedChildFlex(child, expanding, i)) {
                            int spaceAdd = (int)(spaceAvailableThisPass * (child->style()->boxFlex()/totalFlex));
                            if (spaceAdd) {
                                child->setOverrideSize(child->overrideWidth() + spaceAdd);
                                m_flexingChildren = true;
                                relayoutChildren = true;
                            }

                            spaceAvailableThisPass -= spaceAdd;
                            remainingSpace -= spaceAdd;
                            groupRemainingSpace -= spaceAdd;
                            
                            totalFlex -= child->style()->boxFlex();
                        }
                        child = iterator.next();
                    }
                    if (groupRemainingSpace == groupRemainingSpaceAtBeginning) {
                        // this is not advancing, avoid getting stuck by distributing the remaining pixels
                        child = iterator.first();
                        int spaceAdd = groupRemainingSpace > 0 ? 1 : -1;
                        while (child && groupRemainingSpace) {
                            if (allowedChildFlex(child, expanding, i)) {
                                child->setOverrideSize(child->overrideWidth() + spaceAdd);
                                m_flexingChildren = true;
                                relayoutChildren = true;
                                remainingSpace -= spaceAdd;
                                groupRemainingSpace -= spaceAdd;
                            }
                            child = iterator.next();
                        }
                    }
                } while (groupRemainingSpace);
            }

            // We didn't find any children that could grow.
            if (haveFlex && !m_flexingChildren)
                haveFlex = false;
        }
    } while (haveFlex);

    m_flexingChildren = false;

    if (flexDebugEnabled()) {
        fprintf(stderr, "[flex] final box=%p content=%d x=%d h=%d\n",
                (void*)this, contentWidth(), borderLeft() + paddingLeft(), m_height);
        RenderObject* c = iterator.first();
        while (c) {
            flexDebugChild("final", this, c);
            c = iterator.next();
        }
    }

    if (remainingSpace > 0 && ((style()->direction() == LTR && style()->boxPack() != BSTART) ||
                               (style()->direction() == RTL && style()->boxPack() != BEND))) {
        // Children must be repositioned.
        int offset = 0;
        if (style()->boxPack() == BJUSTIFY) {
            // Determine the total number of children.
            int totalChildren = 0;
            child = iterator.first();
            while (child) {
                if (child->isPositioned()) {
                    child = iterator.next();
                    continue;
                }
                totalChildren++;
                child = iterator.next();
            }

            // Iterate over the children and space them out according to the
            // justification level.
            if (totalChildren > 1) {
                totalChildren--;
                bool firstChild = true;
                child = iterator.first();
                while (child) {
                    if (child->isPositioned()) {
                        child = iterator.next();
                        continue;
                    }

                    if (firstChild) {
                        firstChild = false;
                        child = iterator.next();
                        continue;
                    }

                    offset += remainingSpace/totalChildren;
                    remainingSpace -= (remainingSpace/totalChildren);
                    totalChildren--;

                    placeChild(child, child->xPos()+offset, child->yPos());
                    child = iterator.next();
                }
            }
        } else {
            if (style()->boxPack() == BCENTER)
                offset += remainingSpace/2;
            else // END for LTR, START for RTL
                offset += remainingSpace;
            child = iterator.first();
            while (child) {
                if (child->isPositioned()) {
                    child = iterator.next();
                    continue;
                }
                placeChild(child, child->xPos()+offset, child->yPos());
                child = iterator.next();
            }
        }
    }
    
    child = iterator.first();
    while (child && child->isPositioned()) {
        child = iterator.next();
    }
    
    if (child) {
        m_overflowLeft = min(child->xPos() + child->overflowLeft(false), m_overflowLeft);

        RenderObject* lastChild = child;
        while ((child = iterator.next())) {
            if (!child->isPositioned())
                lastChild = child;
        }
        m_overflowWidth = max(lastChild->xPos() + lastChild->overflowWidth(false), m_overflowWidth);
    }
    
    // So that the calcHeight in layoutBlock() knows to relayout positioned objects because of
    // a height change, we revert our height back to the intrinsic height before returning.
    if (heightSpecified)
        m_height = oldHeight;
}

void RenderFlexibleBox::layoutVerticalBox(bool relayoutChildren)
{
    int xPos = borderLeft() + paddingLeft();
    int yPos = borderTop() + paddingTop();
    if( style()->direction() == RTL )
        xPos = m_width - paddingRight() - borderRight();
    int toAdd = borderBottom() + paddingBottom() + horizontalScrollbarHeight();
    bool heightSpecified = false;
    int oldHeight = 0;
    
    unsigned int highestFlexGroup = 0;
    unsigned int lowestFlexGroup = 0;
    bool haveFlex = false;
    int remainingSpace = 0;
    
    // The first walk over our kids is to find out if we have any flexible children.
    FlexBoxIterator iterator(this);
    RenderObject *child = iterator.next();
    while (child) {
        // Check to see if this child flexes.
        if (!child->isPositioned() && child->style()->boxFlex() > 0.0f) {
            // We always have to lay out flexible objects again, since the flex distribution
            // may have changed, and we need to reallocate space.
            child->setOverrideSize(-1);
            if (!relayoutChildren)
                child->setChildNeedsLayout(true, false);
            haveFlex = true;
            unsigned int flexGroup = child->style()->boxFlexGroup();
            if (lowestFlexGroup == 0)
                lowestFlexGroup = flexGroup;
            if (flexGroup < lowestFlexGroup)
                lowestFlexGroup = flexGroup;
            if (flexGroup > highestFlexGroup)
                highestFlexGroup = flexGroup;
        }
        child = iterator.next();
    }

    // We confine the line clamp ugliness to vertical flexible boxes (thus keeping it out of
    // mainstream block layout); this is not really part of the XUL box model.
    bool haveLineClamp = style()->lineClamp() >= 0 && style()->lineClamp() <= 100;
    if (haveLineClamp) {
        int maxLineCount = 0;
        child = iterator.first();
        while (child) {
            if (!child->isPositioned()) {
                if (relayoutChildren || (child->isReplaced() && (child->style()->width().isPercent() || child->style()->height().isPercent())) ||
                    (child->style()->height().isAuto() && child->isBlockFlow() && !child->needsLayout())) {
                    child->setChildNeedsLayout(true, false);
                    
                    // Dirty all the positioned objects.
                    if (child->isRenderBlock()) {
                        static_cast<RenderBlock*>(child)->markPositionedObjectsForLayout();
                        static_cast<RenderBlock*>(child)->clearTruncation();
                    }
                }
                child->layoutIfNeeded();
                if (child->style()->height().isAuto() && child->isBlockFlow())
                    maxLineCount = max(maxLineCount, static_cast<RenderBlock*>(child)->lineCount());
            }
            child = iterator.next();
        }
        
        // Get the # of lines and then alter all block flow children with auto height to use the
        // specified height. We always try to leave room for at least one line.
        int numVisibleLines = max(1, static_cast<int>((maxLineCount + 1) * style()->lineClamp() / 100.0));
        if (numVisibleLines < maxLineCount) {
            for (child = iterator.first(); child; child = iterator.next()) {
                if (child->isPositioned() || !child->style()->height().isAuto() || !child->isBlockFlow())
                    continue;
                
                RenderBlock* blockChild = static_cast<RenderBlock*>(child);
                int lineCount = blockChild->lineCount();
                if (lineCount <= numVisibleLines)
                    continue;
                
                int newHeight = blockChild->heightForLineCount(numVisibleLines);
                if (newHeight == child->height())
                    continue;
                
                child->setChildNeedsLayout(true, false);
                child->setOverrideSize(newHeight);
                m_flexingChildren = true;
                child->layoutIfNeeded();
                m_flexingChildren = false;
                child->setOverrideSize(-1);
                
                // FIXME: For now don't support RTL.
                if (style()->direction() != LTR)
                    continue;
                
                // Get the last line
                RootInlineBox* lastLine = blockChild->lineAtIndex(lineCount-1);
                if (!lastLine)
                    continue;
                
                // See if the last item is an anchor
                InlineBox* anchorBox = lastLine->lastChild();
                if (!anchorBox)
                    continue;
                if (!anchorBox->object()->element())
                    continue;
                if (!anchorBox->object()->element()->isLink())
                    continue;
                
                RootInlineBox* lastVisibleLine = blockChild->lineAtIndex(numVisibleLines-1);
                if (!lastVisibleLine)
                    continue;

                const UChar ellipsisAndSpace[2] = { horizontalEllipsis, ' ' };
                static AtomicString ellipsisAndSpaceStr(ellipsisAndSpace, 2);

                const Font& font = style(numVisibleLines == 1)->font();
                int ellipsisAndSpaceWidth = font.width(TextRun(ellipsisAndSpace, 2));

                // Get ellipsis width + " " + anchor width
                int totalWidth = ellipsisAndSpaceWidth + anchorBox->width();
                
                // See if this width can be accommodated on the last visible line
                RenderBlock* destBlock = static_cast<RenderBlock*>(lastVisibleLine->object());
                RenderBlock* srcBlock = static_cast<RenderBlock*>(lastLine->object());
                
                // FIXME: Directions of src/destBlock could be different from our direction and from one another.
                if (srcBlock->style()->direction() != LTR)
                    continue;
                if (destBlock->style()->direction() != LTR)
                    continue;

                int blockEdge = destBlock->rightOffset(lastVisibleLine->yPos());
                if (!lastVisibleLine->canAccommodateEllipsis(true, blockEdge, 
                                                             lastVisibleLine->xPos() + lastVisibleLine->width(),
                                                             totalWidth))
                    continue;

                // Let the truncation code kick in.
                lastVisibleLine->placeEllipsis(ellipsisAndSpaceStr, true, blockEdge, totalWidth, anchorBox);
                destBlock->setHasMarkupTruncation(true);
            }
        }
    }

    // We do 2 passes.  The first pass is simply to lay everyone out at
    // their preferred widths.  The second pass handles flexing the children.
    // Our first pass is done without flexing.  We simply lay the children
    // out within the box.
    do {
        m_height = borderTop() + paddingTop();
        int minHeight = m_height + toAdd;
        m_overflowHeight = m_height;

        bool firstInFlowChild = true;
        child = iterator.first();
        while (child) {
            // make sure we relayout children if we need it.
            if (!haveLineClamp && (relayoutChildren || (child->isReplaced() && (child->style()->width().isPercent() || child->style()->height().isPercent()))))
                child->setChildNeedsLayout(true, false);
    
            if (child->isPositioned())
            {
                child->containingBlock()->insertPositionedObject(child);
                if (child->hasStaticX()) {
                    if (style()->direction() == LTR)
                        child->setStaticX(borderLeft()+paddingLeft());
                    else
                        child->setStaticX(borderRight()+paddingRight());

                    /* An absolutely positioned child of a flex container
                     * takes its static position from the container's alignment
                     * (CSS Flexbox 1 §4.1). In a vertical box the cross axis is
                     * X, so align-items controls the horizontal placement:
                     * `align-items:center` must centre the child instead of
                     * leaving it at the left edge (DeepSeek's ICP/footer block
                     * is position:absolute;bottom:0 inside a column flex). */
                    int cross = child->width();
                    /* A positioned child is laid out later (layoutPositionedObjects),
                     * so its final width may not be known yet. A fixed CSS width
                     * is available immediately. */
                    if (cross <= 0 && child->style()->width().isFixed())
                        cross = child->style()->width().value();
                    if (cross > 0) {
                        int avail = contentWidth();
                        int aligned = child->staticX();

                        if (style()->boxAlign() == BCENTER)
                            aligned = borderLeft() + paddingLeft() + (avail - cross) / 2;
                        else if (style()->boxAlign() == BEND)
                            aligned = borderLeft() + paddingLeft() + avail - cross;

                        if (aligned >= 0)
                            child->setStaticX(aligned);
                    }
                }
                if (child->hasStaticY())
                    child->setStaticY(m_height);
                child = iterator.next();
                continue;
            }
    
            // Compute the child's vertical margins.
            child->calcVerticalMargins();

            /* flex gap: space between in-flow items (not before the first one) */
            if (!firstInFlowChild)
                m_height += style()->flexRowGap().calcValue(contentHeight());
            firstInFlowChild = false;
    
            // Add in the child's marginTop to our height.
            m_height += child->marginTop();
    
            // Now do a layout.
            child->layoutIfNeeded();
    
            // We can place the child now, using our value of box-align.
            int childX = borderLeft() + paddingLeft();
            switch (style()->boxAlign()) {
                case BCENTER:
                case BBASELINE: // Baseline just maps to center for vertical boxes
                    childX += child->marginLeft() + max(0, (contentWidth() - (child->width() + child->marginLeft() + child->marginRight()))/2);
                    break;
                case BEND:
                    if (style()->direction() == RTL)
                        childX += child->marginLeft();
                    else
                        childX += contentWidth() - child->marginRight() - child->width();
                    break;
                default: // BSTART/BSTRETCH
                    if (style()->direction() == LTR)
                        childX += child->marginLeft();
                    else
                        childX += contentWidth() - child->marginRight() - child->width();
                    break;
            }
    
            // Place the child.
            placeChild(child, childX, m_height);
            m_height += child->height() + child->marginBottom();
    
            if (child->isRenderBlock())
                static_cast<RenderBlock*>(child)->addVisualOverflow(static_cast<RenderBlock*>(child)->floatRect());

            // See if this child has made our overflow need to grow.
            m_overflowWidth = max(child->xPos() + child->overflowWidth(false), m_overflowWidth);
            m_overflowLeft = min(child->xPos() + child->overflowLeft(false), m_overflowLeft);
            
            child = iterator.next();
        }

        yPos = m_height;
        
        if (!iterator.first() && hasLineIfEmpty())
            m_height += lineHeight(true, true);
    
        m_height += toAdd;

        // Negative margins can cause our height to shrink below our minimal height (border/padding).
        // If this happens, ensure that the computed height is increased to the minimal height.
        if (m_height < minHeight)
            m_height = minHeight;

        // Always make sure our overflowheight is at least our height.
        if (m_overflowHeight < m_height)
            m_overflowHeight = m_height;

        // Now we have to calc our height, so we know how much space we have remaining.
        oldHeight = m_height;
        calcHeight();
        if (oldHeight != m_height)
            heightSpecified = true;

        remainingSpace = borderTop() + paddingTop() + contentHeight() - yPos;
        
        if (m_flexingChildren)
            haveFlex = false; // We're done.
        else if (haveFlex) {
            // We have some flexible objects.  See if we need to grow/shrink them at all.
            if (!remainingSpace)
                break;

            // Allocate the remaining space among the flexible objects.  If we are trying to
            // grow, then we go from the lowest flex group to the highest flex group.  For shrinking,
            // we go from the highest flex group to the lowest group.
            bool expanding = remainingSpace > 0;
            unsigned int start = expanding ? lowestFlexGroup : highestFlexGroup;
            unsigned int end = expanding? highestFlexGroup : lowestFlexGroup;
            for (unsigned int i = start; i <= end && remainingSpace; i++) {
                // Always start off by assuming the group can get all the remaining space.
                int groupRemainingSpace = remainingSpace;
                do {
                    // Flexing consists of multiple passes, since we have to change ratios every time an object hits its max/min-width
                    // For a given pass, we always start off by computing the totalFlex of all objects that can grow/shrink at all, and
                    // computing the allowed growth before an object hits its min/max width (and thus
                    // forces a totalFlex recomputation).
                    int groupRemainingSpaceAtBeginning = groupRemainingSpace;
                    float totalFlex = 0.0f;
                    child = iterator.first();
                    while (child) {
                        if (allowedChildFlex(child, expanding, i))
                            totalFlex += child->style()->boxFlex();
                        child = iterator.next();
                    }
                    child = iterator.first();
                    int spaceAvailableThisPass = groupRemainingSpace;
                    while (child) {
                        int allowedFlex = allowedChildFlex(child, expanding, i);
                        if (allowedFlex) {
                            int projectedFlex = (allowedFlex == INT_MAX) ? allowedFlex : (int)(allowedFlex * (totalFlex / child->style()->boxFlex()));
                            spaceAvailableThisPass = expanding ? min(spaceAvailableThisPass, projectedFlex) : max(spaceAvailableThisPass, projectedFlex);
                        }
                        child = iterator.next();
                    }

                    // The flex groups may not have any flexible objects this time around. 
                    if (!spaceAvailableThisPass || totalFlex == 0.0f) {
                        // If we just couldn't grow/shrink any more, then it's time to transition to the next flex group.
                        groupRemainingSpace = 0;
                        continue;
                    }
            
                    // Now distribute the space to objects.
                    child = iterator.first();
                    while (child && spaceAvailableThisPass && totalFlex) {
                        if (allowedChildFlex(child, expanding, i)) {
                            int spaceAdd = (int)(spaceAvailableThisPass * (child->style()->boxFlex()/totalFlex));
                            if (spaceAdd) {
                                child->setOverrideSize(child->overrideHeight() + spaceAdd);
                                m_flexingChildren = true;
                                relayoutChildren = true;
                            }

                            spaceAvailableThisPass -= spaceAdd;
                            remainingSpace -= spaceAdd;
                            groupRemainingSpace -= spaceAdd;
                            
                            totalFlex -= child->style()->boxFlex();
                        }
                        child = iterator.next();
                    }
                    if (groupRemainingSpace == groupRemainingSpaceAtBeginning) {
                        // this is not advancing, avoid getting stuck by distributing the remaining pixels
                        child = iterator.first();
                        int spaceAdd = groupRemainingSpace > 0 ? 1 : -1;
                        while (child && groupRemainingSpace) {
                            if (allowedChildFlex(child, expanding, i)) {
                                child->setOverrideSize(child->overrideHeight() + spaceAdd);
                                m_flexingChildren = true;
                                relayoutChildren = true;
                                remainingSpace -= spaceAdd;
                                groupRemainingSpace -= spaceAdd;
                            }
                            child = iterator.next();
                        }
                    }
                } while (groupRemainingSpace);
            }

            // We didn't find any children that could grow.
            if (haveFlex && !m_flexingChildren)
                haveFlex = false;
        }        
    } while (haveFlex);

    if (style()->boxPack() != BSTART && remainingSpace > 0) {
        // Children must be repositioned.
        int offset = 0;
        if (style()->boxPack() == BJUSTIFY) {
            // Determine the total number of children.
            int totalChildren = 0;
            child = iterator.first();
            while (child) {
                if (child->isPositioned()) {
                    child = iterator.next();
                    continue;
                }
                totalChildren++;
                child = iterator.next();
            }
            
            // Iterate over the children and space them out according to the
            // justification level.
            if (totalChildren > 1) {
                totalChildren--;
                bool firstChild = true;
                child = iterator.first();
                while (child) {
                    if (child->isPositioned()) {
                        child = iterator.next();
                        continue;
                    }
                    
                    if (firstChild) {
                        firstChild = false;
                        child = iterator.next();
                        continue;
                    }

                    offset += remainingSpace/totalChildren;
                    remainingSpace -= (remainingSpace/totalChildren);
                    totalChildren--;
                    placeChild(child, child->xPos(), child->yPos()+offset);
                    child = iterator.next();
                }
            }
        } else {
            if (style()->boxPack() == BCENTER)
                offset += remainingSpace/2;
            else // END
                offset += remainingSpace;
            child = iterator.first();
            while (child) {
                if (child->isPositioned()) {
                    child = iterator.next();
                    continue;
                }
                placeChild(child, child->xPos(), child->yPos()+offset);
                child = iterator.next();
            }
        }
    }
    
    child = iterator.first();
    while (child && child->isPositioned()) {
        child = iterator.next();
    }
    
    if (child) {
        m_overflowTop = min(child->yPos() + child->overflowTop(false), m_overflowTop);

        RenderObject* lastChild = child;
        while ((child = iterator.next())) {
            if (!child->isPositioned())
                lastChild = child;
        }
        m_overflowHeight = max(lastChild->yPos() + lastChild->overflowHeight(false), m_overflowHeight);
    }

    // So that the calcHeight in layoutBlock() knows to relayout positioned objects because of
    // a height change, we revert our height back to the intrinsic height before returning.
    if (heightSpecified)
        m_height = oldHeight; 
}

void RenderFlexibleBox::placeChild(RenderObject* child, int x, int y)
{
    IntRect oldRect(child->xPos(), child->yPos() , child->width(), child->height());

    // Place the child.
    child->setPos(x, y);

    // If the child moved, we have to repaint it as well as any floating/positioned
    // descendants.  An exception is if we need a layout.  In this case, we know we're going to
    // repaint ourselves (and the child) anyway.
    if (!selfNeedsLayout() && child->checkForRepaintDuringLayout())
        child->repaintDuringLayoutIfMoved(oldRect);
}

int RenderFlexibleBox::allowedChildFlex(RenderObject* child, bool expanding, unsigned int group)
{
    if (child->isPositioned() || child->style()->boxFlex() == 0.0f || child->style()->boxFlexGroup() != group)
        return 0;
                        
    if (expanding) {
        if (isHorizontal()) {
            // FIXME: For now just handle fixed values.
            int maxW = INT_MAX;
            int w = child->overrideWidth() - (child->borderLeft() + child->borderRight() + child->paddingLeft() + child->paddingRight());
            if (!child->style()->maxWidth().isUndefined() &&
                child->style()->maxWidth().isFixed())
                maxW = child->style()->maxWidth().value();
            else if (child->style()->maxWidth().type() == Intrinsic)
                maxW = child->maxPrefWidth();
            else if (child->style()->maxWidth().type() == MinIntrinsic)
                maxW = child->minPrefWidth();
            if (maxW == INT_MAX)
                return maxW;
            return max(0, maxW - w);
        } else {
            // FIXME: For now just handle fixed values.
            int maxH = INT_MAX;
            int h = child->overrideHeight() - (child->borderTop() + child->borderBottom() + child->paddingTop() + child->paddingBottom());
            if (!child->style()->maxHeight().isUndefined() &&
                child->style()->maxHeight().isFixed())
                maxH = child->style()->maxHeight().value();
            if (maxH == INT_MAX)
                return maxH;
            return max(0, maxH - h);
        }
    }

    // FIXME: For now just handle fixed values.
    if (isHorizontal()) {
        int minW = child->minPrefWidth();
        int w = child->overrideWidth() - (child->borderLeft() + child->borderRight() + child->paddingLeft() + child->paddingRight());
        if (child->style()->minWidth().isFixed())
            minW = child->style()->minWidth().value();
        else if (child->style()->minWidth().type() == Intrinsic)
            minW = child->maxPrefWidth();
        else if (child->style()->minWidth().type() == MinIntrinsic)
            minW = child->minPrefWidth();
            
        int allowedShrinkage = min(0, minW - w);
        return allowedShrinkage;
    } else {
        if (child->style()->minHeight().isFixed()) {
            int minH = child->style()->minHeight().value();
            int h = child->overrideHeight() - (child->borderLeft() + child->borderRight() + child->paddingLeft() + child->paddingRight());
            int allowedShrinkage = min(0, minH - h);
            return allowedShrinkage;
        }
    }
    
    return 0;
}

const char *RenderFlexibleBox::renderName() const
{
    if (isFloating())
        return "RenderFlexibleBox (floating)";
    if (isPositioned())
        return "RenderFlexibleBox (positioned)";
    if (isRelPositioned())
        return "RenderFlexibleBox (relative positioned)";
    return "RenderFlexibleBox";
}

} // namespace WebCore
