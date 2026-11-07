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

#ifndef HTMLTrackElement_h
#define HTMLTrackElement_h

#include "HTMLElement.h"

namespace WebCore {

class TextTrack;

/* <track kind src srclang label default>. The element itself only carries the
 * attributes; the cue list lives in a TextTrack owned by the parent media
 * element's textTracks list (see HTMLMediaElement::addTextTrack). */
class HTMLTrackElement : public HTMLElement {
public:
    HTMLTrackElement(Document*);

    enum ReadyState {
        NONE = 0,
        LOADING = 1,
        LOADED = 2,
        ERROR_STATE = 3
    };

    virtual HTMLTagStatus endTagRequirement() const { return TagStatusForbidden; }
    virtual int tagPriority() const { return 0; }

    virtual void parseMappedAttribute(MappedAttribute*);
    virtual void insertedIntoDocument();
    virtual void removedFromDocument();

    String kind() const;
    String src() const;
    void setSrc(const String&);
    String srclang() const;
    String label() const;
    bool isDefault() const;
    ReadyState readyState() const { return m_readyState; }

    void setReadyState(ReadyState state) { m_readyState = state; }

    /* The TextTrack this element feeds, created lazily. */
    TextTrack* track() const { return m_track.get(); }
    void setTrack(PassRefPtr<TextTrack>);

private:
    class HTMLMediaElement* mediaElement() const;

    RefPtr<TextTrack> m_track;
    ReadyState m_readyState;
};

} // namespace WebCore

#endif
