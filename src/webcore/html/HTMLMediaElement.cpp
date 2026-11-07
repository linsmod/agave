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
#include "HTMLMediaElement.h"

#include "CachedImage.h"
#include "DocLoader.h"
#include "Document.h"
#include "EventNames.h"
#include "HTMLNames.h"
#include "HTMLSourceElement.h"
#include "HTMLTrackElement.h"
#include "BitmapImage.h"
#include "CachedImage.h"
#include "MediaDecoder.h"
#include "MediaPipeline.h"
#include "RenderMedia.h"
#include "SystemTime.h"
#include "TextTrack.h"

using namespace std;

namespace WebCore {

using namespace WebCore::EventNames;
using namespace HTMLNames;

/* Media event names are not in WebCore 2008's EventNames table (this port had
 * no media stack), so they are interned here. Every dispatch goes through
 * EventTargetNode::dispatchHTMLEvent(), which is what makes both
 * `addEventListener('playing')` and `el.onplaying = fn` work (R27). */
static const AtomicString& mediaEventName(const char* name)
{
    static HashMap<String, AtomicString>* names = 0;

    if (!names)
        names = new HashMap<String, AtomicString>();

    {
        String key(name);
        HashMap<String, AtomicString>::iterator it = names->find(key);

        if (it != names->end())
            return it->second;

        {
            AtomicString value(name);

            names->set(key, value);
            return names->find(key)->second;
        }
    }
}

#define DISPATCH_MEDIA_EVENT(name) dispatchMediaEvent(mediaEventName(name))

HTMLMediaElement::HTMLMediaElement(const QualifiedName& tagName, Document* doc)
    : HTMLElement(tagName, doc)
    , m_networkState(NETWORK_EMPTY)
    , m_readyState(HAVE_NOTHING)
    , m_currentTime(0)
    , m_duration(0)
    , m_lastTickTime(0)
    , m_playbackRate(1.0)
    , m_defaultPlaybackRate(1.0)
    , m_volume(1.0)
    , m_paused(true)
    , m_ended(false)
    , m_seeking(false)
    , m_muted(false)
    , m_loop(false)
    , m_autoplay(false)
    , m_controls(false)
    , m_defaultMuted(false)
    , m_probePending(false)
    , m_lastBufferedEnd(0)
    , m_stalled(false)
    , m_errorCode(MEDIA_ERR_NONE)
    , m_loadTimer(this, &HTMLMediaElement::loadTimerFired)
    , m_probeTimer(this, &HTMLMediaElement::probeTimerFired)
    , m_tickTimer(this, &HTMLMediaElement::tickTimerFired)
    , m_frameTimer(this, &HTMLMediaElement::frameTimerFired)
    , m_pipeline(0)
    , m_poster(0)
    , m_frame(0)
{
    if (Document* doc = document())
        doc->registerMediaElement(this);
}

HTMLMediaElement::~HTMLMediaElement()
{
    if (getenv("AGB_MEDIA_DEBUG"))
        fprintf(stderr, "[media] element destroyed src=%s\n", m_currentSrc.utf8().data());

    if (Document* doc = document())
        doc->unregisterMediaElement(this);

    stopTimers();
    stopPipeline();

    if (m_poster)
        m_poster->deref(this);
    if (m_frame)
        m_frame->deref(this);

    if (m_probePending)
        MediaDecoder::cancel(this);
}

bool HTMLMediaElement::mapToEntry(const QualifiedName& attrName, MappedAttributeEntry& result) const
{
    /* width/height on <video> are presentational (they become CSS lengths). */
    if (attrName == widthAttr || attrName == heightAttr) {
        result = eUniversal;
        return false;
    }

    return HTMLElement::mapToEntry(attrName, result);
}

void HTMLMediaElement::parseMappedAttribute(MappedAttribute* attr)
{
    const QualifiedName& attrName = attr->name();

    if (attrName == srcAttr) {
        /* Setting src (parser or JS) restarts resource selection. An empty
         * src is not a load. */
        if (!attr->isNull() && !attr->value().isEmpty())
            load();
    } else if (attrName == autoplayAttr) {
        m_autoplay = true;
    } else if (attrName == controlsAttr) {
        m_controls = true;
    } else if (attrName == loopAttr) {
        m_loop = true;
    } else if (attrName == mutedAttr) {
        m_muted = true;
        m_defaultMuted = true;
    } else if (attrName == posterAttr) {
        if (hasVideo() && !attr->isNull())
            setPosterImage(document()->docLoader()->requestImage(attr->value()));
    } else {
        HTMLElement::parseMappedAttribute(attr);
    }
}

void HTMLMediaElement::setPosterImage(CachedImage* image)
{
    if (m_poster)
        m_poster->deref(this);

    m_poster = image;

    if (m_poster) {
        m_poster->ref(this);

        if (renderer())
            renderer()->repaint();
    }
}

void HTMLMediaElement::setFrameImage(CachedImage* image)
{
    if (m_frame)
        m_frame->deref(this);

    m_frame = image;

    if (m_frame) {
        m_frame->ref(this);

        if (renderer() && renderer()->isMedia()) {
            static_cast<RenderMedia*>(renderer())->mediaSizeChanged();
            renderer()->repaint();
        }
    }
}

CachedImage* HTMLMediaElement::activeImage() const
{
    if (m_frame && m_frame->image() && !m_frame->errorOccurred())
        return m_frame;

    if (m_poster && m_poster->image() && !m_poster->errorOccurred())
        return m_poster;

    return 0;
}

void HTMLMediaElement::imageChanged(CachedImage* image)
{
    if (image != m_frame && image != m_poster)
        return;

    if (renderer() && renderer()->isMedia())
        static_cast<RenderMedia*>(renderer())->mediaSizeChanged();
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void HTMLMediaElement::insertedIntoDocument()
{
    HTMLElement::insertedIntoDocument();

    /* Defer the load by one turn: the parser appends the <source> children
     * right after the element, and starting on the first child would pick the
     * wrong source. Same pattern as Document's image-load event timer. */
    if (!getAttribute(srcAttr).isEmpty() || firstChild())
        m_loadTimer.startOneShot(0);
}

void HTMLMediaElement::removedFromDocument()
{
    stopTimers();
    HTMLElement::removedFromDocument();
}

void HTMLMediaElement::stopTimers()
{
    m_loadTimer.stop();
    m_probeTimer.stop();
    m_tickTimer.stop();
    m_frameTimer.stop();
}

/* ------------------------------------------------------------------------- */
/* source selection / load                                                   */
/* ------------------------------------------------------------------------- */

void HTMLMediaElement::load()
{
    stopTimers();
    stopPipeline();

    m_ended = false;
    m_currentTime = 0;
    m_errorCode = MEDIA_ERR_NONE;
    m_readyState = HAVE_NOTHING;
    m_duration = 0;
    m_mediaInfo = MediaInfo();

    if (m_probePending) {
        MediaDecoder::cancel(this);
        m_probePending = false;
    }

    DISPATCH_MEDIA_EVENT("emptied");

    {
        String url = getAttribute(srcAttr);

        if (!url.isEmpty()) {
            m_currentSrc = document()->completeURL(url);
            m_networkState = NETWORK_LOADING;
            startLoad();
            return;
        }
    }

    tryNextSource();
}

void HTMLMediaElement::tryNextSource()
{
    m_networkState = NETWORK_LOADING;

    for (Node* child = firstChild(); child; child = child->nextSibling()) {
        if (!child->isElementNode())
            continue;

        {
            Element* el = static_cast<Element*>(child);

            if (!el->hasTagName(sourceTag))
                continue;

            HTMLSourceElement* source = static_cast<HTMLSourceElement*>(el);
            String type = source->type();

            if (!type.isEmpty() && canPlayType(type).isEmpty())
                continue;

            {
                String url = source->src();

                if (url.isEmpty())
                    continue;

                m_currentSrc = document()->completeURL(url);
                startLoad();
                return;
            }
        }
    }

    /* Nothing usable: the spec dispatches `error` and settles on
     * NETWORK_NO_SOURCE, which is the branch players switch on. */
    m_currentSrc = String();
    m_networkState = NETWORK_NO_SOURCE;
    m_readyState = HAVE_NOTHING;
    m_errorCode = MEDIA_ERR_SRC_NOT_SUPPORTED;
    DISPATCH_MEDIA_EVENT("error");
}

bool HTMLMediaElement::hasSourceChildren() const
{
    for (Node* child = firstChild(); child; child = child->nextSibling()) {
        if (child->isElementNode()
                && static_cast<Element*>(child)->hasTagName(sourceTag))
            return true;
    }
    return false;
}

void HTMLMediaElement::sourceElementAdded(HTMLSourceElement* source)
{
    if (!source)
        return;

    if (m_networkState == NETWORK_LOADING || m_readyState >= HAVE_METADATA)
        return;

    {
        String type = source->type();

        if (!type.isEmpty() && canPlayType(type).isEmpty())
            return;
    }

    {
        String url = source->src();

        if (url.isEmpty())
            return;

        m_currentSrc = document()->completeURL(url);
        m_networkState = NETWORK_LOADING;
        startLoad();
    }
}

void HTMLMediaElement::startLoad()
{
    DISPATCH_MEDIA_EVENT("loadstart");

    /* The probe runs on the next turn so that a <source> inserted later in
     * the same parse chunk still gets its chance (and so that load() never
     * runs a process or a network request inside a DOM mutation). */
    m_loadTimer.startOneShot(0);
}

void HTMLMediaElement::loadTimerFired(Timer<HTMLMediaElement>*)
{
    /* No src attribute: this is the deferred resource selection for the
     * <source> children (insertedIntoDocument schedules it so the parser has
     * appended all of them). */
    if (m_currentSrc.isEmpty()) {
        if (hasSourceChildren())
            tryNextSource();
        return;
    }

    {
        MediaInfo info;
        MediaDecoder::ProbeStatus status = MediaDecoder::probe(this, m_currentSrc, info);

        if (status == MediaDecoder::ProbeReady) {
            metadataReady(info);
            return;
        }

        if (status == MediaDecoder::ProbeFailed) {
            loadFailed(MEDIA_ERR_SRC_NOT_SUPPORTED);
            return;
        }

        /* External decoder is running: poll it from the probe timer. */
        m_probePending = true;
        m_probeTimer.startRepeating(0.05);
    }
}

void HTMLMediaElement::probeTimerFired(Timer<HTMLMediaElement>*)
{
    MediaInfo info;
    MediaDecoder::ProbeStatus status = MediaDecoder::pollProbe(this, info);

    if (status == MediaDecoder::ProbePending)
        return;

    m_probeTimer.stop();
    m_probePending = false;

    if (status == MediaDecoder::ProbeReady)
        metadataReady(info);
    else
        loadFailed(MEDIA_ERR_SRC_NOT_SUPPORTED);
}

void HTMLMediaElement::metadataReady(const MediaInfo& info)
{
    m_probePending = false;
    m_mediaInfo = info;

    if (!info.ok) {
        loadFailed(MEDIA_ERR_SRC_NOT_SUPPORTED);
        return;
    }

    m_duration = info.duration;
    m_readyState = HAVE_METADATA;
    m_networkState = NETWORK_IDLE;

    DISPATCH_MEDIA_EVENT("loadedmetadata");

    m_readyState = HAVE_CURRENT_DATA;
    DISPATCH_MEDIA_EVENT("loadeddata");

    m_readyState = HAVE_FUTURE_DATA;
    DISPATCH_MEDIA_EVENT("canplay");

    if (renderer() && renderer()->isMedia())
        static_cast<RenderMedia*>(renderer())->mediaSizeChanged();

    /* Optional still frame: only when a decoder is configured, otherwise a
     * <video> paints poster (or black) and nothing shells out. The result is
     * picked up by m_frameTimer, so no WebCore object is touched by the
     * worker thread. */
    if (hasVideo() && getenv("AGB_MEDIA_FRAMES") && !m_currentSrc.isEmpty()) {
        MediaDecoder::requestFrame(this, m_currentSrc, 0);
        m_frameTimer.startRepeating(0.1);
    }

    if (m_autoplay)
        play();

    /* A play() that arrived before the metadata (common: pages call play() in
     * their own onload) can start the decoder now that the kind is known. */
    if (!m_paused && !m_pipeline)
        startPlayback();
}

void HTMLMediaElement::frameTimerFired(Timer<HTMLMediaElement>*)
{
    /* Live pipeline: the renderer reads the newest frame straight from the
     * pipeline (copyVideoFrame), so a tick only has to ask for a repaint when
     * a picture arrived since the last one. */
    if (m_pipeline && m_pipeline->hasVideo()) {
        if (m_pipeline->hasFrame() && renderer())
            renderer()->repaint();
        return;
    }

    /* Otherwise: the optional one-shot still (AGB_MEDIA_FRAMES). */
    {
        Vector<unsigned char> png;

        if (!MediaDecoder::pollFrame(this, png) || png.isEmpty())
            return;

        /* The bytes are a PNG; BitmapImage decodes them through the normal
         * image pipeline, so the still is painted by the same code as a
         * poster. BitmapImage is not refcounted in this engine: the
         * CachedImage takes ownership (its destructor deletes it). */
        {
            RefPtr<SharedBuffer> buffer = new SharedBuffer((const char*)png.data(), (int)png.size());
            BitmapImage* image = new BitmapImage();

            if (!buffer || !image) {
                delete image;
                return;
            }

            image->setData(buffer, true);

            /* setFrameImage() refs the CachedImage for the element, which
             * derefs it in its own destructor - exactly one owner (a second
             * deref here frees the image while m_frame still points at it). */
            setFrameImage(new CachedImage(image));
        }
    }
}

void HTMLMediaElement::loadFailed(MediaErrorCode code)
{
    m_probePending = false;
    m_networkState = NETWORK_NO_SOURCE;
    m_readyState = HAVE_NOTHING;
    m_errorCode = code;
    DISPATCH_MEDIA_EVENT("error");
}

/* ------------------------------------------------------------------------- */
/* transport                                                                 */
/* ------------------------------------------------------------------------- */

bool HTMLMediaElement::startPipeline()
{
    /* A decoder needs a real resource; without one the simulated ticker is
     * the only thing play() can do. */
    if (m_pipeline || m_currentSrc.isEmpty())
        return m_pipeline != 0;

    {
        MediaPipeline::Kind kind = MediaPipeline::AudioOnly;

        if (hasVideo() && m_mediaInfo.hasVideo && m_mediaInfo.width > 0 && m_mediaInfo.height > 0)
            kind = m_mediaInfo.hasAudio ? MediaPipeline::AudioVideo : MediaPipeline::VideoOnly;
        else if (hasVideo() && m_mediaInfo.width > 0)
            kind = m_mediaInfo.hasAudio ? MediaPipeline::AudioVideo : MediaPipeline::VideoOnly;

        m_pipeline = MediaPipeline::create(m_currentSrc, kind,
                                           m_mediaInfo.width, m_mediaInfo.height,
                                           m_muted, m_volume);
    }

    if (!m_pipeline) {
        return false;
    }

    /* The element may already carry a rate (pages set playbackRate before
     * play()); the pipeline bakes it into the decoder process, so it has to be
     * set before start(). */
    m_pipeline->setRate(m_playbackRate);

    if (!m_pipeline->start()) {
        delete m_pipeline;
        m_pipeline = 0;
        return false;
    }

    /* Resume at the position the page last asked for (play() after a seek, or
     * after `currentTime` was set while paused). */
    if (m_currentTime > 0.05)
        m_pipeline->seek(m_currentTime);

    m_pipeline->setPaused(false);

    /* Video needs the frame loop to notice new pictures. */
    if (m_pipeline->hasVideo())
        m_frameTimer.startRepeating(1.0 / 30.0);

    return true;
}

void HTMLMediaElement::stopPipeline()
{
    if (!m_pipeline)
        return;

    m_pipeline->stop();
    delete m_pipeline;
    m_pipeline = 0;
}

void HTMLMediaElement::startPlayback()
{
    /* Resume a paused pipeline in place (decoder and device stay warm). */
    if (m_pipeline) {
        m_pipeline->setPaused(false);

        if (m_pipeline->hasVideo())
            m_frameTimer.startRepeating(1.0 / 30.0);
        return;
    }

    /* The pipeline kind depends on the metadata (audio-only vs audio+video),
     * so play() before `loadedmetadata` must wait for it: starting here would
     * spawn an audio-only decoder for a video. metadataReady() calls back. */
    if (m_networkState == NETWORK_NO_SOURCE || m_readyState < HAVE_METADATA)
        return;

    startPipeline();
}

void HTMLMediaElement::play()
{
    if (!m_paused)
        return;

    m_paused = false;
    m_ended = false;

    if (m_readyState >= HAVE_METADATA && m_readyState < HAVE_FUTURE_DATA)
        m_readyState = HAVE_FUTURE_DATA;

    /* Real playback first: waveOut for audio, decoded frames for video. When
     * neither is available the ticker below simulates the clock, which is the
     * documented fallback (no ffmpeg installed). An element whose resource
     * failed to load must not start a decoder: sites rely on `error` to pick
     * another source. Before `loadedmetadata` there is nothing to start yet -
     * metadataReady() calls startPlayback() again. */
    startPlayback();

    DISPATCH_MEDIA_EVENT("play");
    DISPATCH_MEDIA_EVENT("playing");

    m_lastTickTime = WebCore::currentTime();

    /* timeupdate cadence: 50 ms, like Chrome. */
    m_tickTimer.startRepeating(0.05);
}

void HTMLMediaElement::pause()
{
    if (m_paused)
        return;

    m_paused = true;
    m_tickTimer.stop();
    m_frameTimer.stop();

    if (m_pipeline)
        m_pipeline->setPaused(true);

    DISPATCH_MEDIA_EVENT("pause");
}

void HTMLMediaElement::stopForDocumentTeardown()
{
    /* No events: the page is going away and listeners are being torn down too.
     * The decoder processes and the waveOut device must die here - the element
     * destructor can come much later (the node may still be referenced by JS
     * wrappers or a pending load). */
    m_paused = true;
    stopTimers();
    stopPipeline();

    m_readyState = HAVE_NOTHING;
}

void HTMLMediaElement::tickTimerFired(Timer<HTMLMediaElement>*)
{
    tick();
}

void HTMLMediaElement::tick()
{
    double now = WebCore::currentTime();

    if (m_paused)
        return;

    if (m_pipeline) {
        /* The decoder's own clock (device position for audio) is authoritative;
         * the ticker only reports it. */
        m_currentTime = m_pipeline->currentTime();

        /* Streaming: the decoder reached the end of the downloaded part and
         * more bytes have landed since. Re-spawn it at the frozen position. */
        if (m_pipeline->needsRestart()) {
            m_pipeline->clearRestart();
            m_pipeline->seek(m_currentTime);
        }

        /* Buffering: `waiting` while the decoder waits for bytes, `playing`
         * once it has them again, and readyState drops like Chrome's does. */
        {
            bool stalledNow = m_pipeline->isStalled();

            if (stalledNow != m_stalled) {
                m_stalled = stalledNow;

                if (stalledNow) {
                    if (m_readyState > HAVE_CURRENT_DATA)
                        m_readyState = HAVE_CURRENT_DATA;
                    DISPATCH_MEDIA_EVENT("waiting");
                } else {
                    if (m_readyState < HAVE_FUTURE_DATA)
                        m_readyState = HAVE_FUTURE_DATA;
                    DISPATCH_MEDIA_EVENT("playing");
                }
            }
        }

        /* `progress`: the downloaded frontier moved. Chrome paces this event,
         * a growth of 250 ms or half a percent is plenty for a progress bar. */
        {
            double bufferedEnd = m_pipeline->bufferedSeconds();

            if (bufferedEnd > m_lastBufferedEnd + 0.25
                    && bufferedEnd <= m_duration + 0.001) {
                m_lastBufferedEnd = bufferedEnd;
                DISPATCH_MEDIA_EVENT("progress");
            }
        }

        if (getenv("AGB_MEDIA_DEBUG"))
            m_pipeline->debugState("tick");
    } else {
        double delta = now - m_lastTickTime;

        m_lastTickTime = now;

        if (delta <= 0 || delta > 1.0)
            delta = 0.05;

        m_currentTime += delta * m_playbackRate;
    }

    if (m_duration > 0 && m_currentTime >= m_duration) {
        if (m_loop) {
            setCurrentTime(0);
        } else {
            m_currentTime = m_duration;
            m_paused = true;
            m_ended = true;
            m_tickTimer.stop();
            m_frameTimer.stop();
            stopPipeline();
            updateActiveCues();
            DISPATCH_MEDIA_EVENT("timeupdate");
            DISPATCH_MEDIA_EVENT("ended");
            return;
        }
    }

    updateActiveCues();
    DISPATCH_MEDIA_EVENT("timeupdate");
}

void HTMLMediaElement::setCurrentTime(double time)
{
    if (time < 0)
        time = 0;
    if (m_duration > 0 && time > m_duration)
        time = m_duration;

    m_ended = false;

    m_seeking = true;
    DISPATCH_MEDIA_EVENT("seeking");

    m_currentTime = time;
    m_lastTickTime = WebCore::currentTime();

    if (m_pipeline) {
        /* Restarts the decoder at the new position; the clock follows the
         * device again from there. */
        m_pipeline->seek(time);

        if (m_paused)
            m_pipeline->setPaused(true);
    }

    m_seeking = false;

    updateActiveCues();
    DISPATCH_MEDIA_EVENT("timeupdate");
    DISPATCH_MEDIA_EVENT("seeked");

    if (renderer())
        renderer()->repaint();
}

double HTMLMediaElement::bufferedEnd() const
{
    if (m_pipeline)
        return m_pipeline->bufferedSeconds();

    /* Simulated ticker (no decoder): the whole local resource is available as
     * soon as the metadata is. */
    if (m_readyState >= HAVE_METADATA)
        return m_duration;

    return 0;
}

bool HTMLMediaElement::stalled() const
{
    return m_pipeline && m_pipeline->isStalled();
}

bool HTMLMediaElement::copyVideoFrame(Vector<unsigned char>& out, int& width, int& height) const{
    if (!m_pipeline || !m_pipeline->hasVideo())
        return false;

    /* Any decoded frame the pipeline has is the current picture: the decoder
     * drops older ones itself, so there is nothing to filter here. */
    if (!m_pipeline->copyFrame(out, m_currentTime, 1000.0))
        return false;

    width = m_pipeline->videoWidth();
    height = m_pipeline->videoHeight();
    return width > 0 && height > 0;
}

void HTMLMediaElement::setPlaybackRate(double rate)
{
    if (rate <= 0)
        return;

    m_playbackRate = rate;
    m_lastTickTime = WebCore::currentTime();

    /* A real decoder needs the rate too: audio through ffmpeg's atempo, video
     * through the frame pacing (both inside the pipeline, and changing it
     * re-spawns the decoders at the current position). */
    if (m_pipeline)
        m_pipeline->setRate(rate);

    DISPATCH_MEDIA_EVENT("ratechange");
}

void HTMLMediaElement::setDefaultPlaybackRate(double rate)
{
    if (rate > 0)
        m_defaultPlaybackRate = rate;
}

void HTMLMediaElement::setVolume(double volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > 1)
        volume = 1;

    m_volume = volume;
    if (m_pipeline)
        m_pipeline->setVolume(m_muted ? 0.0 : volume);
    DISPATCH_MEDIA_EVENT("volumechange");
}

void HTMLMediaElement::setMuted(bool muted)
{
    if (m_muted == muted)
        return;

    m_muted = muted;
    if (m_pipeline)
        m_pipeline->setMuted(muted);
    DISPATCH_MEDIA_EVENT("volumechange");
}

void HTMLMediaElement::setLoop(bool loop)
{
    m_loop = loop;

    if (loop) {
        setAttribute(loopAttr, "");
    } else {
        ExceptionCode ec = 0;
        removeAttribute(loopAttr, ec);
    }
}

void HTMLMediaElement::setAutoplay(bool autoplay)
{
    m_autoplay = autoplay;

    if (autoplay) {
        setAttribute(autoplayAttr, "");
    } else {
        ExceptionCode ec = 0;
        removeAttribute(autoplayAttr, ec);
    }
}

void HTMLMediaElement::setControls(bool controls)
{
    m_controls = controls;

    if (controls) {
        setAttribute(controlsAttr, "");
    } else {
        ExceptionCode ec = 0;
        removeAttribute(controlsAttr, ec);
    }
}

/* ------------------------------------------------------------------------- */
/* attributes / metadata                                                     */
/* ------------------------------------------------------------------------- */

String HTMLMediaElement::src() const
{
    return document()->completeURL(getAttribute(srcAttr));
}

void HTMLMediaElement::setSrc(const String& value)
{
    setAttribute(srcAttr, value);
}

String HTMLMediaElement::preload() const
{
    String value = getAttribute(preloadAttr);

    if (value.isEmpty())
        return "metadata";
    if (value == "none" || value == "metadata" || value == "auto")
        return value;
    return "metadata";
}

void HTMLMediaElement::setPreload(const String& value)
{
    setAttribute(preloadAttr, value);
}

String HTMLMediaElement::poster() const
{
    return document()->completeURL(getAttribute(posterAttr));
}

void HTMLMediaElement::setPoster(const String& value)
{
    setAttribute(posterAttr, value);
}

int HTMLMediaElement::videoWidth() const
{
    return m_mediaInfo.width;
}

int HTMLMediaElement::videoHeight() const
{
    return m_mediaInfo.height;
}

String HTMLMediaElement::canPlayType(const String& type) const
{
    String mime = type.lower();
    int semi = mime.find(';');

    if (semi >= 0)
        mime = mime.substring(0, semi);

    mime = mime.stripWhiteSpace();

    if (mime.isEmpty())
        return String();

    if (mime == "video/mp4" || mime == "video/webm" || mime == "audio/mp4"
            || mime == "audio/mpeg" || mime == "audio/ogg" || mime == "audio/wav"
            || mime == "video/quicktime")
        return "probably";

    if (mime == "video/ogg" || mime == "video/x-matroska" || mime == "audio/flac"
            || mime == "application/x-mpegurl" || mime == "application/vnd.apple.mpegurl"
            || mime == "video/mp2t")
        return "maybe";

    return String();
}

void HTMLMediaElement::dispatchMediaEvent(const AtomicString& type)
{
    /* Media events fire on the element only (no bubbling, not cancelable). */
    dispatchHTMLEvent(type, false, false);
}

/* ------------------------------------------------------------------------- */
/* text tracks                                                               */
/* ------------------------------------------------------------------------- */

TextTrackList* HTMLMediaElement::textTracks()
{
    if (!m_textTracks)
        m_textTracks = new TextTrackList();

    return m_textTracks.get();
}

void HTMLMediaElement::addTextTrack(TextTrack* track)
{
    if (!track)
        return;

    textTracks()->append(track);
    track->setMediaElement(this);
}

void HTMLMediaElement::trackElementAdded(HTMLTrackElement* element)
{
    if (!element)
        return;

    for (unsigned i = 0; i < m_trackElements.size(); ++i) {
        if (m_trackElements[i].get() == element)
            return;
    }

    m_trackElements.append(element);

    if (element->track())
        addTextTrack(element->track());
}

void HTMLMediaElement::updateActiveCues()
{
    if (!m_textTracks)
        return;

    bool changed = false;

    for (unsigned i = 0; i < m_textTracks->length(); ++i) {
        TextTrack* track = m_textTracks->item(i);

        if (!track || track->mode() == "disabled")
            continue;

        {
            Vector<RefPtr<TextTrackCue> > active;
            const Vector<RefPtr<TextTrackCue> >& cues = track->cues();

            track->cuesAt(m_currentTime, active);

            for (unsigned k = 0; k < cues.size(); ++k) {
                TextTrackCue* cue = cues[k].get();
                bool nowActive = false;

                for (unsigned a = 0; a < active.size(); ++a) {
                    if (active[a].get() == cue) {
                        nowActive = true;
                        break;
                    }
                }

                if (nowActive != cue->isActive()) {
                    cue->setActive(nowActive);
                    changed = true;
                }
            }
        }
    }

    if (changed)
        DISPATCH_MEDIA_EVENT("cuechange");
}

} // namespace WebCore
