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

#ifndef MediaDocument_h
#define MediaDocument_h

#include "HTMLDocument.h"

namespace WebCore {

class DOMImplementation;

/* Top-level navigation to an audio/video resource: instead of downloading
 * the file, the browser shows it the way Chrome does - a document with a
 * single <video controls autoplay> (or <audio>) element pointing at the URL,
 * on a black background.
 *
 * The element loads the resource itself through the media layer, so this
 * document never parses the bytes: the tokenizer throws them away (the same
 * shape as ImageDocument / PluginDocument). */
class MediaDocument : public HTMLDocument {
public:
    MediaDocument(DOMImplementation*, Frame*);

    virtual bool isMediaDocument() const { return true; }
    virtual Tokenizer* createTokenizer();

    /* Builds <html><body><video|audio controls autoplay src=URL>. Called by
     * the tokenizer on the first byte (the bytes themselves are discarded). */
    void createDocumentStructure();

private:
    bool m_createdStructure;
};

} // namespace WebCore

#endif // MediaDocument_h
