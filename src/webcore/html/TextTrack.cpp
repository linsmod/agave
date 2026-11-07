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

#include "TextTrack.h"

namespace WebCore {

/* --- TextTrackCue ------------------------------------------------------- */

/* --- TextTrack ---------------------------------------------------------- */

TextTrack::TextTrack(const String& kind, const String& label, const String& language)
    : m_kind(kind.isEmpty() ? String("subtitles") : kind)
    , m_label(label)
    , m_language(language)
    , m_mode("disabled")
    , m_mediaElement(0)
{
}

TextTrack::~TextTrack()
{
}

void TextTrack::addCue(PassRefPtr<TextTrackCue> cue)
{
    if (!cue)
        return;

    m_cues.append(cue);
}

void TextTrack::removeCue(TextTrackCue* cue)
{
    for (unsigned i = 0; i < m_cues.size(); ++i) {
        if (m_cues[i].get() == cue) {
            m_cues.remove(i);
            return;
        }
    }
}

void TextTrack::setCues(const Vector<RefPtr<TextTrackCue> >& cues)
{
    m_cues = cues;
}

void TextTrack::cuesAt(double time, Vector<RefPtr<TextTrackCue> >& out) const
{
    for (unsigned i = 0; i < m_cues.size(); ++i) {
        if (time >= m_cues[i]->startTime() && time < m_cues[i]->endTime())
            out.append(m_cues[i]);
    }
}

/* WebVTT subset: "WEBVTT" header, cues with optional id line, a
 * `hh:mm:ss.mmm --> hh:mm:ss.mmm` timing line and text until a blank line.
 * NOTE/STYLE/REGION blocks and cue settings are ignored (they do not affect
 * cue text, which is all the API exposes). */
static bool parseVTTTime(const String& text, double& seconds)
{
    /* Accept "mm:ss.mmm", "hh:mm:ss.mmm" and "ss.mmm". */
    unsigned parts[3];
    int count = 0;
    unsigned pos = 0;
    unsigned i = 0;

    seconds = 0;

    while (i < text.length()) {
        UChar c = text[i];

        if (c >= '0' && c <= '9') {
            pos = pos * 10 + (c - '0');
        } else if (c == ':') {
            if (count >= 2)
                return false;
            parts[count++] = pos;
            pos = 0;
        } else if (c == '.' || c == ',') {
            break;
        } else {
            return false;
        }
        ++i;
    }

    parts[count++] = pos;

    if (i < text.length() && (text[i] == '.' || text[i] == ',')) {
        unsigned millis = 0;
        unsigned digits = 0;

        ++i;
        while (i < text.length() && digits < 3) {
            UChar c = text[i];

            if (c < '0' || c > '9')
                return false;
            millis = millis * 10 + (c - '0');
            ++digits;
            ++i;
        }
        while (digits < 3) {
            millis *= 10;
            ++digits;
        }
        seconds = (double)millis / 1000.0;
    }

    if (count == 3)
        seconds += parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
    else if (count == 2)
        seconds += parts[0] * 60.0 + parts[1];
    else
        seconds += parts[0];

    return true;
}

bool TextTrack::parseWebVTT(const String& text)
{
    /* A leading BOM and CRLF are common; normalize both. */
    String body = text;
    Vector<RefPtr<TextTrackCue> > cues;

    if (body.startsWith(String("\xEF\xBB\xBF")))
        body = body.substring(3);

    body.replace("\r\n", "\n");
    body.replace('\r', '\n');

    {
        int newline = body.find('\n');
        String header = newline < 0 ? body : body.substring(0, newline);

        if (!header.startsWith("WEBVTT"))
            return false;
    }

    int lineStart = body.find('\n');
    unsigned pos = lineStart < 0 ? body.length() : (unsigned)(lineStart + 1);

    while (pos < body.length()) {
        int next = body.find('\n', pos);
        String line = next < 0 ? body.substring(pos) : body.substring(pos, (unsigned)next - pos);
        unsigned lineEnd = next < 0 ? body.length() : (unsigned)(next + 1);

        /* Blank lines separate blocks. */
        if (line.stripWhiteSpace().isEmpty()) {
            pos = lineEnd;
            continue;
        }

        /* Skip comment / style / region blocks entirely. */
        if (line.startsWith("NOTE") || line.startsWith("STYLE") || line.startsWith("REGION")) {
            while (pos < body.length()) {
                int end = body.find('\n', pos);
                String inner = end < 0 ? body.substring(pos) : body.substring(pos, (unsigned)end - pos);

                pos = end < 0 ? body.length() : (unsigned)(end + 1);
                if (inner.stripWhiteSpace().isEmpty())
                    break;
            }
            continue;
        }

        {
            unsigned cueStart = pos;
            int arrow = line.find("-->");
            String id;

            if (arrow < 0) {
                /* No timing on this line: it is the cue id. */
                id = line.stripWhiteSpace();

                if (pos >= body.length())
                    break;

                {
                    int nextLine = body.find('\n', pos);
                    line = nextLine < 0 ? body.substring(pos) : body.substring(pos, (unsigned)nextLine - pos);
                    lineEnd = nextLine < 0 ? body.length() : (unsigned)(nextLine + 1);
                    arrow = line.find("-->");
                }

                if (arrow < 0) {
                    /* Not a cue: skip the line and keep scanning. */
                    pos = lineEnd;
                    continue;
                }
            }

            {
                String startText = line.substring(0, (unsigned)arrow).stripWhiteSpace();
                String rest = line.substring((unsigned)arrow + 3).stripWhiteSpace();
                String endText;
                double startSeconds = 0;
                double endSeconds = 0;

                {
                    int space = rest.find(' ');

                    if (space >= 0)
                        endText = rest.substring(0, (unsigned)space);
                    else
                        endText = rest;
                }

                if (!parseVTTTime(startText, startSeconds)
                        || !parseVTTTime(endText, endSeconds)) {
                    pos = lineEnd;
                    continue;
                }

                /* Text lines until a blank line. */
                {
                    String cueText;
                    unsigned textPos = lineEnd;

                    while (textPos < body.length()) {
                        int end = body.find('\n', textPos);
                        String inner = end < 0 ? body.substring(textPos)
                                               : body.substring(textPos, (unsigned)end - textPos);

                        if (inner.stripWhiteSpace().isEmpty()) {
                            textPos = end < 0 ? body.length() : (unsigned)(end + 1);
                            break;
                        }

                        if (!cueText.isEmpty())
                            cueText += "\n";
                        cueText += inner;

                        textPos = end < 0 ? body.length() : (unsigned)(end + 1);
                    }

                    {
                        RefPtr<TextTrackCue> cue = new TextTrackCue(startSeconds, endSeconds, cueText);

                        cue->setId(id);
                        cues.append(cue);
                    }

                    pos = textPos;
                }
            }

            (void)cueStart;
        }
    }

    m_cues = cues;
    return true;
}

/* --- TextTrackList ------------------------------------------------------ */

TextTrackList::TextTrackList()
{
}

TextTrack* TextTrackList::item(unsigned index) const
{
    if (index >= m_tracks.size())
        return 0;
    return m_tracks[index].get();
}

TextTrack* TextTrackList::getTrackById(const String& id) const
{
    for (unsigned i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i]->label() == id)
            return m_tracks[i].get();
    }
    return 0;
}

void TextTrackList::append(PassRefPtr<TextTrack> track)
{
    if (track)
        m_tracks.append(track);
}

void TextTrackList::remove(TextTrack* track)
{
    for (unsigned i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].get() == track) {
            m_tracks.remove(i);
            return;
        }
    }
}

} // namespace WebCore
