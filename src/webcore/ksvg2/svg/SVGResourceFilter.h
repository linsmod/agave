/*
    Copyright (C) 2006 Nikolas Zimmermann <zimmermann@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#ifndef SVGResourceFilter_h
#define SVGResourceFilter_h

#if ENABLE(SVG)

#include "AtomicString.h"

namespace WebCore {

class Document;

// Filters are part of ENABLE_SVG_EXPERIMENTAL_FEATURES in this era of WebKit.
// The forward declaration is unconditional because RenderPath/SVGRenderSupport
// declare an SVGResourceFilter* parameter even when the feature is off.
class SVGResourceFilter;

#if ENABLE(SVG_EXPERIMENTAL_FEATURES)
SVGResourceFilter* getFilterById(Document*, const AtomicString&);
#endif

}

#endif // ENABLE(SVG)
#endif // SVGResourceFilter_h
