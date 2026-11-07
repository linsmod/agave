/*
 * Copyright (C) 2024 Zhang Ji Peng (onecoolx@gmail.com)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "HTMLSourceElement.h"

#include "HTMLAudioElement.h"
#include "Document.h"
#include "HTMLMediaElement.h"
#include "HTMLNames.h"
#include "HTMLVideoElement.h"

namespace WebCore {

using namespace HTMLNames;

HTMLSourceElement::HTMLSourceElement(Document* doc)
    : HTMLElement(sourceTag, doc)
{
}

HTMLMediaElement* HTMLSourceElement::mediaElement() const
{
    Node* parent = this->parentNode();

    if (!parent || !parent->isElementNode())
        return 0;

    {
        Element* el = static_cast<Element*>(parent);

        if (el->hasTagName(videoTag))
            return static_cast<HTMLMediaElement*>(static_cast<HTMLVideoElement*>(el));
        if (el->hasTagName(audioTag))
            return static_cast<HTMLMediaElement*>(static_cast<HTMLAudioElement*>(el));
    }

    return 0;
}

void HTMLSourceElement::parseMappedAttribute(MappedAttribute* attr)
{
    const QualifiedName& attrName = attr->name();

    if (attrName == srcAttr || attrName == typeAttr) {
        /* A <source> that gains its src after insertion has to be able to
         * start the load; the media element decides whether to use it. */
        if (HTMLMediaElement* media = mediaElement())
            media->sourceElementAdded(this);
    } else {
        HTMLElement::parseMappedAttribute(attr);
    }
}

void HTMLSourceElement::insertedIntoDocument()
{
    HTMLElement::insertedIntoDocument();

    if (HTMLMediaElement* media = mediaElement())
        media->sourceElementAdded(this);
}

void HTMLSourceElement::removedFromDocument()
{
    HTMLElement::removedFromDocument();
}

String HTMLSourceElement::src() const
{
    return document()->completeURL(getAttribute(srcAttr));
}

void HTMLSourceElement::setSrc(const String& value)
{
    setAttribute(srcAttr, value);
}

String HTMLSourceElement::type() const
{
    return getAttribute(typeAttr);
}

void HTMLSourceElement::setType(const String& value)
{
    setAttribute(typeAttr, value);
}

String HTMLSourceElement::media() const
{
    return getAttribute(mediaAttr);
}

} // namespace WebCore
