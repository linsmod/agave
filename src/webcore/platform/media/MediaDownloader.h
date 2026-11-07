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

#ifndef MediaDownloader_h
#define MediaDownloader_h

#include <string>

namespace WebCore {

/* Background download of an http(s) media resource into a local file the
 * decoders can read while it is still growing.
 *
 * Why not the engine's ResourceHandle: that is an engine-thread state machine,
 * and the media element wants to start decoding the first bytes instead of
 * waiting for the whole body. WinHTTP on its own thread keeps the engine
 * responsive and, unlike the ffmpeg/ffprobe child processes, can complete a
 * TLS handshake.
 *
 * Contract: `path` is written to `<path>.part` and renamed to `<path>` only
 * when the body is complete, so "the final file exists" means "it is all
 * there". Readers may open the .part file at any time; bytes are appended and
 * flushed per chunk.
 *
 * See docs/arch-media.md section 7.3. */
namespace MediaDownloader {

/* Starts a download for `url` writing `path` (or joins the one in flight).
 * Returns false when the URL cannot be parsed at all. */
bool ensure(const std::string& url, const std::string& path);

/* Bytes already flushed to the .part file for `url` (0 when unknown). */
unsigned long long bytes(const std::string& url);

/* Content-Length when the server sent one, 0 otherwise. */
unsigned long long total(const std::string& url);

/* True once the body is complete (file renamed) / the transfer failed. */
bool finished(const std::string& url);
bool failed(const std::string& url);

/* Waits until at least `wanted` bytes are available, the transfer ends, or
 * `timeoutMs` elapses. Returns true when the byte count is reached. */
bool waitForBytes(const std::string& url, unsigned long long wanted, int timeoutMs);

/* Stops the transfer (the .part file is left as is). */
void stop(const std::string& url);

} // namespace MediaDownloader

} // namespace WebCore

#endif // MediaDownloader_h
