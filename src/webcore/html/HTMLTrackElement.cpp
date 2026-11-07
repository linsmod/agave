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
#include "HTMLTrackElement.h"

#include "Document.h"
#include "EventNames.h"
#include "HTMLMediaElement.h"
#include "HTMLNames.h"
#include "ResourceError.h"
#include "ResourceHandle.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "TextTrack.h"

namespace WebCore {

using namespace HTMLNames;

HTMLTrackElement::HTMLTrackElement(Document* doc)
    : HTMLElement(trackTag, doc)
    , m_readyState(NONE)
{
}

HTMLMediaElement* HTMLTrackElement::mediaElement() const
{
    Node* parent = this->parentNode();

    if (!parent || !parent->isElementNode())
        return 0;

    {
        Element* el = static_cast<Element*>(parent);

        if (el->hasTagName(videoTag) || el->hasTagName(audioTag))
            return static_cast<HTMLMediaElement*>(el);
    }

    return 0;
}

void HTMLTrackElement::setTrack(PassRefPtr<TextTrack> track)
{
    m_track = track;
}

void HTMLTrackElement::parseMappedAttribute(MappedAttribute* attr)
{
    const QualifiedName& attrName = attr->name();

    if (attrName == kindAttr || attrName == labelAttr || attrName == srclangAttr
            || attrName == defaultAttr || attrName == srcAttr) {
        /* Re-create the TextTrack so kind/label/language reflect the
         * attributes; textTracks is a live view over these elements. */
        if (attrName == kindAttr || attrName == labelAttr || attrName == srclangAttr || attrName == defaultAttr) {
            RefPtr<TextTrack> track = new TextTrack(kind(), label(), srclang());

            track->setId(id());
            setTrack(track);

            if (isDefault())
                track->setMode("hidden");

            if (HTMLMediaElement* media = mediaElement())
                media->trackElementAdded(this);
        }
    } else {
        HTMLElement::parseMappedAttribute(attr);
    }
}

void HTMLTrackElement::insertedIntoDocument()
{
    HTMLElement::insertedIntoDocument();

    if (!m_track) {
        RefPtr<TextTrack> track = new TextTrack(kind(), label(), srclang());

        track->setId(id());
        setTrack(track);

        if (isDefault())
            track->setMode("hidden");
    }

    if (HTMLMediaElement* media = mediaElement())
        media->trackElementAdded(this);
}

void HTMLTrackElement::removedFromDocument()
{
    HTMLElement::removedFromDocument();
}

String HTMLTrackElement::kind() const
{
    String value = getAttribute(kindAttr).domString().lower();

    if (value.isEmpty())
        return "subtitles";
    return value;
}

String HTMLTrackElement::src() const
{
    return document()->completeURL(getAttribute(srcAttr));
}

void HTMLTrackElement::setSrc(const String& value)
{
    setAttribute(srcAttr, value);
}

String HTMLTrackElement::srclang() const
{
    return getAttribute(srclangAttr).domString().lower();
}

String HTMLTrackElement::label() const
{
    return getAttribute(labelAttr);
}

bool HTMLTrackElement::isDefault() const
{
    return hasAttribute(defaultAttr);
}

} // namespace WebCore
