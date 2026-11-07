/*
 * Copyright (C) 2024 Zhang Ji Peng (onecoolx@gmail.com)
 *
 * Real playback for HTMLMediaElement: audio through waveOut, video frames from
 * an ffmpeg decode process. See docs/arch-media.md.
 *
 * Threading contract (important):
 *   - the engine thread owns every WebCore object;
 *   - one decoder process per element writes PCM / raw frames;
 *   - a shared ring buffer plus one pumping thread moves audio to waveOut;
 *   - the renderer only reads the newest frame under a critical section.
 * Nothing here calls back into WebCore.
 */

#ifndef MediaPipeline_h
#define MediaPipeline_h

#include "PlatformString.h"
#include <string>
#include <wtf/Vector.h>

namespace WebCore {

class MediaPipeline;

/* One pipeline per playing media element. Created on play(), destroyed on
 * pause()/end/destruction, so at most a handful exist at a time. */
class MediaPipeline {
public:
    enum Kind {
        AudioOnly,
        VideoOnly,
        AudioVideo
    };

    static MediaPipeline* create(const String& url, Kind kind,
                                 int videoWidth, int videoHeight, bool muted, double volume);

    ~MediaPipeline();

    /* Starts the decoder process and (for audio) the waveOut device.
     * Returns false when no decoder is available - the caller then falls back
     * to the simulated transport instead of failing. */
    bool start();

    void stop();
    void setPaused(bool paused);
    void setVolume(double volume);
    void setMuted(bool muted);

    /* --- clock ---------------------------------------------------------- */
    /* Position in seconds. For audio this is derived from the waveOut playback
     * cursor (what the user actually hears); for video-only it is wall clock
     * from start minus paused time. */
    double currentTime() const;

    double duration() const { return m_duration; }
    int videoWidth() const { return m_width; }
    int videoHeight() const { return m_height; }
    bool hasAudio() const { return m_kind != VideoOnly; }
    bool hasVideo() const { return m_kind != AudioOnly; }
    bool failed() const { return m_failed; }
    bool ended() const;

    void seek(double seconds);

    /* --- streaming ------------------------------------------------------ */
    /* A decoder that runs out of bytes while the download is still feeding the
     * local file sets a restart request and stops publishing; the engine asks
     * `needsRestart()` from its timer and re-spawns the decoders at the current
     * position with `seek()`. While that happens the clock is frozen, so the
     * element stalls instead of running past the data. */
    bool needsRestart() const;
    void clearRestart();

    /* --- buffering ------------------------------------------------------ */
    /* Seconds of media already on disk (the whole file for a complete copy).
     * This is what `buffered` reports while a resource streams in. */
    double bufferedSeconds() const;

    /* True while the decoder has no bytes and the download has not finished:
     * the element turns that into `waiting` + readyState HAVE_CURRENT_DATA. */
    bool isStalled() const;

    /* Playback speed. Changing it re-spawns the decoders (the audio side needs
     * ffmpeg's atempo filter), so the caller keeps the clock position. */
    void setRate(double rate);
    double rate() const { return m_rate; }

    /* --- video ---------------------------------------------------------- */
    /* Copies the newest decoded frame as BGRA into `out` (resized to the
     * pipeline's video size). Returns false when no frame has arrived yet or
     * the frame is older than `maxAgeSeconds` past the requested position. */
    bool copyFrame(Vector<unsigned char>& out, double position, double maxAgeSeconds);

    /* True when at least one frame is ready (the renderer uses this to decide
     * whether it can paint a real picture). */
    bool hasFrame() const;

    /* Diagnostic dump (AGB_MEDIA_DEBUG only). */
    void debugState(const char* where) const;

private:
    MediaPipeline();

    /* Called from a decoder thread that ran out of bytes: freezes the clock at
     * the frontier, waits for the download to move on and requests a restart.
     * Returns false when the resource is finished (a real EOF). */
    bool waitForMoreData();

    struct Impl;
    Impl* m_impl;

    /* The decoder threads are plain C functions; they reach the private Impl
     * through these free functions rather than through friendship games. */
    friend void mediaPipelineThreadAudio(MediaPipeline*);
    friend void mediaPipelineThreadVideo(MediaPipeline*);

    String m_url;

    /* A local file the decoders read (the URL itself for file:, the engine's
     * downloaded copy for http(s)); empty means "hand the URL to the tool".
     * For a resource that is still downloading this is the .part file. */
    std::string m_localPath;
    bool m_streaming;
    double m_rate;
    Kind m_kind;
    int m_width;
    int m_height;
    double m_duration;
    bool m_muted;
    double m_volume;
    bool m_failed;
};

} // namespace WebCore

#endif
