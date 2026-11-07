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
#include "MediaTemplate.h"

#include "CString.h"
#include "Frame.h"
#include "FrameLoader.h"
#include "KURL.h"
#include "ResourceRequest.h"
#include "SharedBuffer.h"
#include "SubstituteData.h"

#include <stdio.h>
#include <stdlib.h>

namespace WebCore {
namespace MediaTemplate {

bool urlLooksLikeAudio(const KURL& url)
{
    String path = url.path().lower();

    return path.endsWith(".mp3") || path.endsWith(".m4a") || path.endsWith(".aac")
        || path.endsWith(".wav") || path.endsWith(".flac") || path.endsWith(".opus")
        || path.endsWith(".oga") || path.endsWith(".ogg");
}

String html(const KURL& url, bool isAudio)
{
    String href = url.url();
    String name = url.path();
    int slash = name.reverseFind('/');

    if (slash >= 0)
        name = name.substring(slash + 1);

    /* Escape what would break the attribute / the text node. */
    href.replace('&', "&amp;");
    href.replace('"', "&quot;");
    href.replace('<', "&lt;");
    name.replace('&', "&amp;");
    name.replace('<', "&lt;");
    name.replace('>', "&gt;");

    {
        const char* tag = isAudio ? "audio" : "video";

        return String("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>") + name
             + "</title></head><body><" + tag + " controls autoplay src=\"" + href + "\">"
             + "</" + tag + "></body></html>";
    }
}

void load(Frame* frame, const KURL& url, bool isAudio)
{
    if (!frame || url.isEmpty())
        return;

    {
        String content = html(url, isAudio);
        CString bytes = content.utf8();
        RefPtr<SharedBuffer> buffer = new SharedBuffer(bytes.data(), bytes.length());
        ResourceRequest request(url);
        SubstituteData substitute(buffer, "text/html", "UTF-8", KURL(), url);

        if (getenv("AGB_MEDIA_DEBUG"))
            fprintf(stderr, "[nav] media template (blank + <%s src>) url=%s\n",
                    isAudio ? "audio" : "video", url.url().utf8().data());

        /* The "replace" part: a load from memory takes over from the pending
         * media navigation, so its body is never read - the <video src> below
         * is the only fetch of the resource. */
        frame->loader()->load(request, substitute);
    }
}

} // namespace MediaTemplate
} // namespace WebCore
