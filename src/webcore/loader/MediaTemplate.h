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

#ifndef MediaTemplate_h
#define MediaTemplate_h

#include "PlatformString.h"

class KURL;

namespace WebCore {

class Frame;

/* A top-level navigation to a media resource is not a document load: the frame
 * gets a locally generated page - about:blank plus one <video|audio src=URL> -
 * and the element fetches the media itself, exactly once.
 *
 * This is the "replace" load (SubstituteData): the pending navigation is
 * dropped before its body is read, so the media is never downloaded twice. The
 * page itself is the same template Chrome synthesises for `:-webkit-full-page-media`.
 * See docs/arch-media.md section 7.2. */
namespace MediaTemplate {

/* The template as HTML text (title = file name, controls + autoplay). */
String html(const KURL&, bool isAudio);

/* Replaces `frame`'s current navigation with the template. */
void load(Frame*, const KURL&, bool isAudio);

/* True when the URL's last path component names an audio file. */
bool urlLooksLikeAudio(const KURL&);

} // namespace MediaTemplate

} // namespace WebCore

#endif // MediaTemplate_h
