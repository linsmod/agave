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

#ifndef HTMLMediaElement_h
#define HTMLMediaElement_h

#include "CachedResourceClient.h"
#include "HTMLElement.h"
#include "MediaMetadata.h"
#include "Timer.h"

namespace WebCore {

class CachedImage;
class HTMLSourceElement;
class HTMLTrackElement;
class MediaPipeline;
class TextTrack;
class TextTrackList;

/* Media error codes (HTMLMediaElement.error.code). */
enum MediaErrorCode {
    MEDIA_ERR_NONE = 0,
    MEDIA_ERR_ABORTED = 1,
    MEDIA_ERR_NETWORK = 2,
    MEDIA_ERR_DECODE = 3,
    MEDIA_ERR_SRC_NOT_SUPPORTED = 4
};

/* <video>/<audio> share everything except "does it have a visual box".
 *
 * The element owns a *simulated* transport: play()/pause() drive a WebCore
 * timer that advances currentTime, and the readyState/networkState machine
 * follows the HTML spec closely enough for players to take their normal
 * branches. Nothing is decoded into audio/video output; the full scope and
 * its limits are in docs/arch-media.md.
 */
class HTMLMediaElement : public HTMLElement, public CachedResourceClient {
public:
    enum NetworkState {
        NETWORK_EMPTY = 0,
        NETWORK_IDLE = 1,
        NETWORK_LOADING = 2,
        NETWORK_NO_SOURCE = 3
    };

    enum ReadyState {
        HAVE_NOTHING = 0,
        HAVE_METADATA = 1,
        HAVE_CURRENT_DATA = 2,
        HAVE_FUTURE_DATA = 3,
        HAVE_ENOUGH_DATA = 4
    };

    HTMLMediaElement(const QualifiedName&, Document*);
    virtual ~HTMLMediaElement();

    virtual bool hasVideo() const { return false; }

    /* --- DOM ----------------------------------------------------------- */
    virtual bool mapToEntry(const QualifiedName&, MappedAttributeEntry& result) const;
    virtual void parseMappedAttribute(MappedAttribute*);
    virtual void insertedIntoDocument();
    virtual void removedFromDocument();

    /* --- source selection ---------------------------------------------- */
    String src() const;
    void setSrc(const String&);
    String currentSrc() const { return m_currentSrc; }
    String preload() const;
    void setPreload(const String&);

    /* A <source> child that can start a load calls this. */
    void sourceElementAdded(HTMLSourceElement*);
    /* --- network / ready state ----------------------------------------- */
    NetworkState networkState() const { return m_networkState; }
    ReadyState readyState() const { return m_readyState; }
    void load();

    /* --- playback state ------------------------------------------------ */
    double currentTime() const { return m_currentTime; }
    void setCurrentTime(double);
    double duration() const { return m_duration; }
    bool paused() const { return m_paused; }
    bool ended() const { return m_ended; }
    bool seeking() const { return m_seeking; }
    double playbackRate() const { return m_playbackRate; }
    void setPlaybackRate(double);
    double defaultPlaybackRate() const { return m_defaultPlaybackRate; }
    void setDefaultPlaybackRate(double);
    double volume() const { return m_volume; }
    void setVolume(double);
    bool muted() const { return m_muted; }
    void setMuted(bool);
    bool loop() const { return m_loop; }
    void setLoop(bool);
    bool autoplay() const { return m_autoplay; }
    void setAutoplay(bool);
    bool controls() const { return m_controls; }
    void setControls(bool);
    bool defaultMuted() const { return m_defaultMuted; }

    /* Simulated transport: play() starts the ticker, pause() stops it. */
    void play();
    void pause();

    /* True when a real decoder is driving this element (audio through waveOut
     * and/or decoded video frames). Falls back to the simulated ticker when
     * no decoder is available, so pages never see a dead element. */
    bool isRealPlayback() const { return m_pipeline != 0; }

    /* The renderer asks for the newest decoded frame; `out` receives BGRA of
     * the pipeline's video size. */
    bool copyVideoFrame(Vector<unsigned char>& out, int& width, int& height) const;

    /* --- capability / metadata ----------------------------------------- */
    String canPlayType(const String& type) const;

    int videoWidth() const;
    int videoHeight() const;

    bool hasError() const { return m_errorCode != MEDIA_ERR_NONE; }
    int errorCode() const { return m_errorCode; }

    /* poster and the (optional) first decoder frame. activeImage() prefers
     * the frame; both are null until a resource loads. */
    String poster() const;
    void setPoster(const String&);
    void setPosterImage(CachedImage*);
    void setFrameImage(CachedImage*);
    CachedImage* activeImage() const;

    /* CachedResourceClient */
    virtual void imageChanged(CachedImage*);

    /* --- tracks --------------------------------------------------------- */
    TextTrackList* textTracks();
    void addTextTrack(TextTrack*);
    void trackElementAdded(HTMLTrackElement*);
    const Vector<RefPtr<HTMLTrackElement> >& trackElements() const { return m_trackElements; }

    /* Called by MediaDecoder: the resource was accepted or rejected. Always
     * on the engine thread (an external decoder's result is polled there). */
    void metadataReady(const MediaInfo&);
    void loadFailed(MediaErrorCode);

private:
    void loadTimerFired(Timer<HTMLMediaElement>*);
    void probeTimerFired(Timer<HTMLMediaElement>*);
    void tickTimerFired(Timer<HTMLMediaElement>*);
    void frameTimerFired(Timer<HTMLMediaElement>*);
    bool hasSourceChildren() const;

    bool startPipeline();
    void stopPipeline();

    void startLoad();
    void tryNextSource();
    void stopTimers();

    void tick();
    void updateActiveCues();

    void dispatchMediaEvent(const AtomicString&);

    MediaInfo m_mediaInfo;

    String m_currentSrc;
    NetworkState m_networkState;
    ReadyState m_readyState;

    double m_currentTime;
    double m_duration;
    double m_lastTickTime;
    double m_playbackRate;
    double m_defaultPlaybackRate;
    double m_volume;
    bool m_paused;
    bool m_ended;
    bool m_seeking;
    bool m_muted;
    bool m_loop;
    bool m_autoplay;
    bool m_controls;
    bool m_defaultMuted;
    bool m_probePending;

    int m_errorCode;

    Timer<HTMLMediaElement> m_loadTimer;
    Timer<HTMLMediaElement> m_probeTimer;
    Timer<HTMLMediaElement> m_tickTimer;
    Timer<HTMLMediaElement> m_frameTimer;

    /* Real playback (audio/video decode). Null means the simulated ticker is
     * running instead. */
    MediaPipeline* m_pipeline;

    CachedImage* m_poster;
    CachedImage* m_frame;

    RefPtr<TextTrackList> m_textTracks;
    Vector<RefPtr<HTMLTrackElement> > m_trackElements;
};

} // namespace WebCore

#endif
