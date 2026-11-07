/*
    Copyright (C) 2006 Nikolas Zimmermann <zimmermann@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#include "config.h"

#if ENABLE(SVG)
#include "SVGResourceMasker.h"

#include "Document.h"
#include "Element.h"
#include "SVGMaskElement.h"
#include "SVGNames.h"

namespace WebCore {

SVGResourceMasker::SVGResourceMasker(SVGMaskElement* element)
    : m_maskElement(element)
{
}

SVGResourceMasker::~SVGResourceMasker()
{
}

SVGResourceMasker* getMaskerById(Document* document, const AtomicString& id)
{
    if (!document || id.isEmpty())
        return 0;

    Element* element = document->getElementById(id);
    if (!element || !element->isSVGElement() || !element->hasTagName(SVGNames::maskTag))
        return 0;

    return static_cast<SVGResourceMasker*>(static_cast<SVGMaskElement*>(element)->canvasResource());
}

}

#endif // ENABLE(SVG)

// vim:ts=4:noet
