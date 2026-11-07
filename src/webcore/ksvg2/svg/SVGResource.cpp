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

#include "config.h"

#if ENABLE(SVG)
#include "SVGResource.h"

#include "RenderObject.h"
#include "SVGStyledElement.h"

namespace WebCore {

// Every live resource is tracked so that SVGStyledElement's destructor can find
// the resources it was a client of (the element is destroyed through its own
// untracked path, so there is no document registry to ask).
static HashSet<SVGResource*>& resourceRegistry()
{
    static HashSet<SVGResource*>* resources = new HashSet<SVGResource*>();
    return *resources;
}

SVGResource::SVGResource()
    : m_refCount(0)
{
    resourceRegistry().add(this);
}

SVGResource::~SVGResource()
{
    resourceRegistry().remove(this);
}

void SVGResource::addClient(SVGStyledElement* item)
{
    if (item)
        m_clients.add(item);
}

void SVGResource::repaintClients()
{
    repaintClients(m_clients);
}

void SVGResource::removeClient(SVGStyledElement* item)
{
    if (!item)
        return;

    HashSet<SVGResource*>& resources = resourceRegistry();
    HashSet<SVGResource*>::iterator end = resources.end();
    for (HashSet<SVGResource*>::iterator it = resources.begin(); it != end; ++it)
        (*it)->m_clients.remove(item);
}

void SVGResource::repaintClients(HashSet<SVGStyledElement*>& clients)
{
    HashSet<SVGStyledElement*>::iterator end = clients.end();
    for (HashSet<SVGStyledElement*>::iterator it = clients.begin(); it != end; ++it) {
        SVGStyledElement* element = *it;
        if (!element || !element->document())
            continue;
        RenderObject* renderer = element->renderer();
        if (renderer)
            renderer->repaint();
    }
}

}

#endif // ENABLE(SVG)

// vim:ts=4:noet
