/*
    Copyright (C) 2006 Apple Computer, Inc.
                  2006 Nikolas Zimmermann <zimmermann@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.
*/

#ifndef SVGResource_h
#define SVGResource_h

#if ENABLE(SVG)

#include <wtf/HashSet.h>

namespace WebCore {

class SVGStyledElement;

// Base class for everything an SVG element can reference by id and paint with:
// paint servers (solid/gradient/pattern), clip paths, masks, markers, filters.
//
// This tree has no wtf/RefCounted.h: RefPtr calls ref()/deref() on the object
// directly, so resources carry their own counter.
//
// The 2008 tree calls two functions that must stay static/global, because they
// are invoked without an instance (see SVGElement::insertedIntoDocument and
// SVGStyledElement's destructor):
//   - removeClient(SVGStyledElement*)      - drop the element from every resource
//   - repaintClients(HashSet<...>&)        - repaint a set of elements
class SVGResource {
public:
    SVGResource();
    virtual ~SVGResource();

    void ref() { ++m_refCount; }
    void deref()
    {
        if (--m_refCount <= 0)
            delete this;
    }

    virtual void invalidate() { }
    virtual bool isPaintServer() const { return false; }

    void addClient(SVGStyledElement*);
    void repaintClients();

    static void removeClient(SVGStyledElement*);
    static void repaintClients(HashSet<SVGStyledElement*>&);

protected:
    HashSet<SVGStyledElement*> m_clients;
    int m_refCount;
};

}

#endif // ENABLE(SVG)
#endif // SVGResource_h
