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

#ifndef MediaDecoder_h
#define MediaDecoder_h

#include "MediaMetadata.h"
#include <wtf/Vector.h>

namespace WebCore {

/* The seam between the DOM side (HTMLMediaElement) and the actual byte
 * inspection. Nothing here may run JS or block the engine thread:
 *
 *   - local files / data: parsed in-process on the caller's first call;
 *   - anything else: handed to a worker thread that runs `ffprobe`, and the
 *     media element polls pollProbe() from its own timer.
 *
 * See docs/arch-media.md §6.
 */
namespace MediaDecoder {

enum ProbeStatus {
    ProbePending = 0,
    ProbeReady = 1,
    ProbeFailed = 2
};

/* Starts a probe for `element`. Returns:
 *   ProbeReady  - `out` is filled (in-process header parse succeeded);
 *   ProbeFailed - nothing can read this URL (`out` untouched);
 *   ProbePending- a worker is running, poll pollProbe().
 * Starting a second probe for the same element replaces the first. */
ProbeStatus probe(const void* element, const String& url, MediaInfo& out);

/* Polls the pending probe of `element`. `out` is only valid on ProbeReady. */
ProbeStatus pollProbe(const void* element, MediaInfo& out);

/* Drops anything pending for `element` (element is going away). */
void cancel(const void* element);

/* Asks for one decoded frame at `time` (seconds). A no-op when no ffmpeg is
 * configured. The element polls pollFrame(). */
void requestFrame(const void* element, const String& url, double time);

/* Returns true and hands over the encoded image bytes when a frame arrived.
 * The buffer is copied into `out`. Multiple frames for one element coalesce:
 * only the newest is kept. */
bool pollFrame(const void* element, Vector<unsigned char>& out);

/* Called once per frame from the engine's pump: lets finished jobs be seen. */
void pump();

/* Tool discovery (AGB_FFMPEG / AGB_FFPROBE override, then the usual install
 * locations, then PATH). Returning false means "no external decoder", which
 * is a supported configuration. */
bool ffprobeAvailable();
bool ffmpegAvailable();

} // namespace MediaDecoder

} // namespace WebCore

#endif
