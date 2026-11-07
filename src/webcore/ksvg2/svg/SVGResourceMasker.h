/*
    Copyright (C) 2006 Nikolas Zimmermann <zimmermann@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#ifndef SVGResourceMasker_h
#define SVGResourceMasker_h

#if ENABLE(SVG)

#include "FloatRect.h"
#include "SVGResource.h"

#include "AtomicString.h"

namespace WebCore {

class Document;
class GraphicsContext;
class SVGMaskElement;

class SVGResourceMasker : public SVGResource {
public:
    explicit SVGResourceMasker(SVGMaskElement*);
    virtual ~SVGResourceMasker();

    // FIXME: masking needs an offscreen buffer (ImageBuffer, not implemented in
    // this tree yet) - the mask is currently ignored, see the design doc.
    void applyMask(GraphicsContext*, const FloatRect&) const { }

    virtual void invalidate() { }

private:
    SVGMaskElement* m_maskElement;
};

SVGResourceMasker* getMaskerById(Document*, const AtomicString&);

}

#endif // ENABLE(SVG)
#endif // SVGResourceMasker_h
