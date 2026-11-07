/*
    Copyright (C) 2006 Nikolas Zimmermann <zimmermann@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
*/

#ifndef SVGResourceClipper_h
#define SVGResourceClipper_h

#if ENABLE(SVG)

#include "FloatRect.h"
#include "Path.h"
#include "SVGResource.h"

#include "AtomicString.h"

namespace WebCore {

class Document;
class GraphicsContext;

class SVGResourceClipper : public SVGResource {
public:
    SVGResourceClipper();
    virtual ~SVGResourceClipper();

    void resetClipData();
    void addClipData(const Path&, WindRule, bool boundingBoxUnits);

    void applyClip(GraphicsContext*, const FloatRect& boundingBox) const;

    virtual void invalidate() { }

private:
    struct ClipData {
        Path path;
        WindRule rule;
        bool boundingBoxUnits;
    };

    Vector<ClipData> m_clipData;
};

SVGResourceClipper* getClipperById(Document*, const AtomicString&);

}

#endif // ENABLE(SVG)
#endif // SVGResourceClipper_h
