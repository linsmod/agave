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

#ifndef MediaMetadata_h
#define MediaMetadata_h

#include "PlatformString.h"

namespace WebCore {

/* What a container header can tell us without decoding anything. */
struct MediaInfo {
    MediaInfo()
        : ok(false)
        , duration(0)
        , width(0)
        , height(0)
        , hasVideo(false)
        , hasAudio(false)
    {
    }

    bool ok;
    double duration;    /* seconds; 0 when unknown */
    int width;
    int height;
    bool hasVideo;
    bool hasAudio;
    String mimeType;
};

/* Parses the leading bytes of a media container and fills `out`.
 *
 * Used by HTMLMediaElement::load(): the file (or the first slice of an HTTP
 * response) is read once, and every format check works on that buffer. This
 * is deliberately small - it answers "can the page see a duration and a
 * resolution", not "can we play it" (see canPlayType()).
 *
 * Supported: MP4/MOV (`ftyp`), WebM/MKV (EBML), Ogg. Returns false otherwise.
 */
bool parseMediaHeader(const unsigned char* data, size_t size, MediaInfo& out);

/* Best-effort duration probe for the bytes that are *not* the header (Ogg and
 * some MP4s keep the duration at the end). `data` is the tail of the file. */
bool parseMediaTail(const unsigned char* data, size_t size, MediaInfo& out);

/* MIME guess from the URL, used for canPlayType() fallbacks and for the
 * metadata file open path. Empty when the extension is unknown. */
String mediaMimeTypeForURL(const String& url);

} // namespace WebCore

#endif
