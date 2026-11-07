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

#ifndef TextTrack_h
#define TextTrack_h

#include "PlatformString.h"
#include <wtf/PassRefPtr.h>
#include "Shared.h"
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>

namespace WebCore {

class HTMLMediaElement;

/* WebVTT cue. Times are in seconds. No renderer draws cues (see
 * docs/arch-media.md §8); the JS side exposes the data and the media element
 * fires `cuechange` when the active set changes. */
class TextTrackCue : public Shared<TextTrackCue> {
public:
    TextTrackCue(double start, double end, const String& text)
        : m_start(start)
        , m_end(end)
        , m_text(text)
        , m_active(false)
        , m_paused(false)
    {
    }

    double startTime() const { return m_start; }
    double endTime() const { return m_end; }
    void setStartTime(double v) { m_start = v; }
    void setEndTime(double v) { m_end = v; }

    const String& id() const { return m_id; }
    void setId(const String& id) { m_id = id; }

    const String& text() const { return m_text; }
    void setText(const String& text) { m_text = text; }

    bool isActive() const { return m_active; }
    void setActive(bool active) { m_active = active; }

    bool isPaused() const { return m_paused; }
    void setPaused(bool paused) { m_paused = paused; }

private:
    double m_start;
    double m_end;
    String m_id;
    String m_text;
    bool m_active;
    bool m_paused;
};

/* A TextTrack is a cue list plus a mode. Only "showing" / "hidden" tracks
 * contribute to cuechange; "disabled" is ignored, like the spec. */
class TextTrack : public Shared<TextTrack> {
public:
    TextTrack(const String& kind, const String& label, const String& language);
    ~TextTrack();

    const String& kind() const { return m_kind; }
    const String& label() const { return m_label; }
    const String& language() const { return m_language; }
    const String& id() const { return m_id; }
    void setId(const String& id) { m_id = id; }

    String mode() const { return m_mode; }
    void setMode(const String& mode) { m_mode = mode; }

    const Vector<RefPtr<TextTrackCue> >& cues() const { return m_cues; }
    void addCue(PassRefPtr<TextTrackCue>);
    void removeCue(TextTrackCue*);

    /* Replaces the whole cue list (a <track> finished loading its VTT). */
    void setCues(const Vector<RefPtr<TextTrackCue> >&);

    /* Parses a WebVTT document. False when the WEBVTT header is missing. */
    bool parseWebVTT(const String& text);

    /* Collects the cues covering `time`. */
    void cuesAt(double time, Vector<RefPtr<TextTrackCue> >& out) const;

    HTMLMediaElement* mediaElement() const { return m_mediaElement; }
    void setMediaElement(HTMLMediaElement* element) { m_mediaElement = element; }

private:
    String m_kind;
    String m_label;
    String m_language;
    String m_id;
    String m_mode;
    HTMLMediaElement* m_mediaElement;
    Vector<RefPtr<TextTrackCue> > m_cues;
};

/* The textTracks collection; the JS wrapper adds `length`/`item`/index access. */
class TextTrackList : public Shared<TextTrackList> {
public:
    TextTrackList();

    unsigned length() const { return m_tracks.size(); }
    TextTrack* item(unsigned index) const;
    TextTrack* getTrackById(const String& id) const;

    void append(PassRefPtr<TextTrack>);
    void remove(TextTrack*);

private:
    Vector<RefPtr<TextTrack> > m_tracks;
};

} // namespace WebCore

#endif
