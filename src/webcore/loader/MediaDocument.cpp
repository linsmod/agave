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
#include "MediaDocument.h"

#include "DocumentLoader.h"
#include "Element.h"
#include "Frame.h"
#include "FrameLoader.h"
#include "HTMLNames.h"
#include "SegmentedString.h"
#include "Text.h"
#include "XMLTokenizer.h"

namespace WebCore {

using namespace HTMLNames;

namespace {

/* Nothing here needs styling: the player geometry and the black page come from
 * Chrome's UA rule for `:-webkit-full-page-media` (html4.css), so a bare media
 * document looks the same here and in Chrome. */
const char* const kDocumentStyle = "";

} // namespace

class MediaTokenizer : public Tokenizer {
public:
    MediaTokenizer(MediaDocument* doc)
        : m_doc(doc)
    {
    }

    virtual bool write(const SegmentedString&, bool appendData);
    virtual void finish();
    virtual bool isWaitingForScripts() const { return false; }

    virtual bool wantsRawData() const { return true; }
    virtual bool writeRawData(const char* data, int len);

private:
    MediaDocument* m_doc;
};

bool MediaTokenizer::write(const SegmentedString&, bool)
{
    /* Raw data only: the media element loads the resource, the bytes are
     * never parsed. */
    return false;
}

bool MediaTokenizer::writeRawData(const char* data, int len)
{
    (void)data;
    (void)len;

    m_doc->createDocumentStructure();

    return false;
}

void MediaTokenizer::finish()
{
    m_doc->createDocumentStructure();

    if (!m_parserStopped)
        m_doc->finishedParsing();
}

void MediaDocument::createDocumentStructure()
{
    ExceptionCode ec = 0;
    Frame* frame = this->frame();

    if (m_createdStructure || !frame)
        return;

    m_createdStructure = true;

    String mimeType = frame->loader()->responseMIMEType();
    bool isAudio = mimeType.startsWith("audio/", false);

    /* Servers that send application/octet-stream (or nothing) for a media
     * file: the extension decides, and the element's canPlayType() answers
     * from the same place. */
    if (!mimeType.startsWith("video/", false) && !mimeType.startsWith("audio/", false)) {
        String path = frame->loader()->url().path().lower();

        isAudio = path.endsWith(".mp3") || path.endsWith(".m4a") || path.endsWith(".aac")
               || path.endsWith(".wav") || path.endsWith(".flac") || path.endsWith(".opus")
               || path.endsWith(".oga") || path.endsWith(".ogg");
    }

    RefPtr<Element> rootElement = createElementNS(xhtmlNamespaceURI, "html", ec);
    appendChild(rootElement, ec);

    {
        RefPtr<Element> head = createElementNS(xhtmlNamespaceURI, "head", ec);

        /* Chrome titles a media document after the file, which is what a
         * user looks at in the tab. */
        {
            String path = frame->loader()->url().path();
            int slash = path.reverseFind('/');
            String name = slash >= 0 ? path.substring(slash + 1) : path;

            if (!name.isEmpty()) {
                RefPtr<Element> title = createElementNS(xhtmlNamespaceURI, "title", ec);
                title->appendChild(createTextNode(name), ec);
                head->appendChild(title, ec);
            }
        }

        {
            RefPtr<Element> style = createElementNS(xhtmlNamespaceURI, "style", ec);
            style->appendChild(createTextNode(kDocumentStyle), ec);
            head->appendChild(style, ec);
        }
        rootElement->appendChild(head, ec);
    }

    {
        RefPtr<Element> body = createElementNS(xhtmlNamespaceURI, "body", ec);
        RefPtr<Element> media = createElementNS(xhtmlNamespaceURI,
                                                isAudio ? "audio" : "video", ec);

        media->setAttribute(controlsAttr, "");
        media->setAttribute(autoplayAttr, "");
        media->setAttribute(srcAttr, String(frame->loader()->url().url()));

        body->appendChild(media, ec);
        rootElement->appendChild(body, ec);
    }
}

MediaDocument::MediaDocument(DOMImplementation* implementation, Frame* frame)
    : HTMLDocument(implementation, frame)
    , m_createdStructure(false)
{
    setParseMode(Compat);
}

Tokenizer* MediaDocument::createTokenizer()
{
    return new MediaTokenizer(this);
}

} // namespace WebCore
