/*
 * Copyright (C) 2024 Zhang Ji Peng (onecoolx@gmail.com)
 *
 * Real playback: an ffmpeg process per element produces PCM (audio) and raw
 * BGRA frames (video). Audio goes to waveOut, and the *device* position is
 * the clock; video frames are kept newest-only and selected by that clock.
 *
 * See MediaPipeline.h for the threading contract and docs/arch-media.md for
 * the feature's scope.
 */

#include "config.h"

#include "MediaPipeline.h"
#include "MediaDecoder.h"
#include "MediaDownloader.h"
#include "MediaMetadata.h"

#include "CString.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#if PLATFORM(WIN32)
#include <windows.h>
#include <mmsystem.h>
#endif

namespace WebCore {

namespace {

const int kSampleRate = 44100;
const int kChannels   = 2;
const int kBytesPerSample = 2;
const int kBufferCount = 6;
const int kBufferMillis = 100;

/* Video is decoded at a fixed rate so a frame index maps to a timestamp
 * without parsing the container's own timing. 25 fps is what the sample
 * clips use and is plenty for a browser that paints on the frame loop. */
const int kVideoFps = 25;



} // namespace

struct MediaPipeline::Impl {
    Impl()
        : audioProcess(NULL)
        , audioThread(NULL)
        , audioPipe(NULL)
        , videoProcess(NULL)
        , videoThread(NULL)
        , videoPipe(NULL)
        , stopRequested(false)
        , paused(false)
        , waveOut(NULL)
        , sampleRate(kSampleRate)
        , channels(kChannels)
        , seekBase(0)
        , pausedAccum(0)
        , videoBytesPerFrame(0)
        , frameIndex(0)
        , frameStamp(0)
        , frameValid(false)
        , needsRestart(0)
        , stalled(0)
        , stalledAt(0)
    {
        memset(buffers, 0, sizeof(buffers));
        memset(&freq, 0, sizeof(freq));
        memset(&startedAt, 0, sizeof(startedAt));
        memset(&pausedSince, 0, sizeof(pausedSince));
    }

    /* --- decoders (one process + one reader thread each) --- */
    HANDLE audioProcess;
    HANDLE audioThread;
    HANDLE audioPipe;       /* read end of the PCM pipe */
    HANDLE videoProcess;
    HANDLE videoThread;
    HANDLE videoPipe;       /* read end of the raw-frame pipe */
    volatile LONG stopRequested;
    volatile LONG paused;

    /* --- audio --- */
    HWAVEOUT waveOut;
    WAVEHDR buffers[kBufferCount];
    int sampleRate;
    int channels;
    double seekBase;
    LARGE_INTEGER freq;
    LARGE_INTEGER startedAt;
    double pausedAccum;
    LARGE_INTEGER pausedSince;

    /* --- video --- */
    int videoBytesPerFrame;
    Vector<unsigned char> frame;
    Vector<unsigned char> frameScratch;
    unsigned long frameIndex;
    double frameStamp;
    bool frameValid;
    CRITICAL_SECTION frameLock;

    /* --- streaming --- */
    volatile LONG needsRestart;   /* a decoder hit EOF, more bytes are coming */
    volatile LONG stalled;
    double stalledAt;             /* clock value frozen while stalled */
};

/* ------------------------------------------------------------------------- */
/* process plumbing                                                          */
/* ------------------------------------------------------------------------- */

#if PLATFORM(WIN32)

/* Spawns exe + args with stdout on a pipe and stderr to NUL (ffmpeg progress
 * is noise unless something goes wrong). */
static bool spawnDecoder(const std::string& exe, const std::vector<std::string>& args,
                         HANDLE* outRead, HANDLE* outProcess)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE readPipe = NULL;
    HANDLE writePipe = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    std::string commandLine;
    std::vector<char> mutableCommand;
    HANDLE nul;
    bool ok = false;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return false;

    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    commandLine = "\"" + exe + "\"";
    for (size_t i = 0; i < args.size(); ++i)
        commandLine += " \"" + args[i] + "\"";

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = (nul != INVALID_HANDLE_VALUE) ? nul : NULL;
    si.hStdInput = NULL;

    memset(&pi, 0, sizeof(pi));

    mutableCommand.assign(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');

    if (CreateProcessA(NULL, &mutableCommand[0], NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        *outRead = readPipe;
        *outProcess = pi.hProcess;
        CloseHandle(pi.hThread);
        ok = true;
    }

    CloseHandle(writePipe);

    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);

    if (!ok)
        CloseHandle(readPipe);

    return ok;
}

static void killProcess(HANDLE process)
{
    if (!process)
        return;

    TerminateProcess(process, 0);
    WaitForSingleObject(process, 1000);
    CloseHandle(process);
}

#endif /* PLATFORM_WIN32 */

/* ------------------------------------------------------------------------- */
/* MediaPipeline                                                             */
/* ------------------------------------------------------------------------- */

MediaPipeline::MediaPipeline()
    : m_impl(new Impl())
    , m_kind(AudioOnly)
    , m_width(0)
    , m_height(0)
    , m_duration(0)
    , m_muted(false)
    , m_volume(1.0)
    , m_failed(false)
{
#if PLATFORM(WIN32)
    InitializeCriticalSection(&m_impl->frameLock);
    QueryPerformanceFrequency(&m_impl->freq);
    QueryPerformanceCounter(&m_impl->startedAt);
#endif
}

MediaPipeline::~MediaPipeline()
{
    stop();

    if (m_impl) {
#if PLATFORM(WIN32)
        DeleteCriticalSection(&m_impl->frameLock);
#endif
        delete m_impl;
    }
}

/* The decoder threads are started by start(); they only ever touch the Impl. */

void mediaPipelineThreadAudio(MediaPipeline*);
void mediaPipelineThreadVideo(MediaPipeline*);

#if PLATFORM(WIN32)
static DWORD WINAPI pipelineAudioEntry(void* param)
{
    mediaPipelineThreadAudio((MediaPipeline*)param);
    return 0;
}

static DWORD WINAPI pipelineVideoEntry(void* param)
{
    mediaPipelineThreadVideo((MediaPipeline*)param);
    return 0;
}
#endif

MediaPipeline* MediaPipeline::create(const String& url, Kind kind,
                                     int videoWidth, int videoHeight,
                                     bool muted, double volume)
{
    MediaPipeline* pipeline = new MediaPipeline();

    pipeline->m_url = url;
    pipeline->m_kind = kind;
    pipeline->m_streaming = false;
    pipeline->m_rate = 1.0;
    pipeline->m_width = videoWidth;
    pipeline->m_height = videoHeight;
    pipeline->m_muted = muted;
    pipeline->m_volume = volume;

    {
        /* The decoders only ever read files: a file: URL becomes its path,
         * an http(s) URL the engine's downloaded copy (see MediaDecoder).
         * While that copy is incomplete the decoders will hit EOF and ask for
         * a restart until the download finishes. */
        std::string local;

        if (MediaDecoder::localPathForURL(url.utf8().data(), local))
            pipeline->m_localPath = local;

        pipeline->m_streaming = !MediaDecoder::localCopyComplete(url.utf8().data());
    }

    {
        /* Duration comes from the same header parser the metadata path uses,
         * so the clock and `duration` never disagree. The file is read through
         * its path (a file: URL has already been turned into one). */
        MediaInfo info;
        Vector<unsigned char> data;

        if (!pipeline->m_localPath.empty()
                && MediaDecoder::readHeaderBytesFromFile(pipeline->m_localPath, 1024 * 1024, data)
                && parseMediaHeader(data.data(), data.size(), info))
            pipeline->m_duration = info.duration;
    }

    return pipeline;
}

bool MediaPipeline::start()
{
#if !PLATFORM(WIN32)
    m_failed = true;
    return false;
#else
    std::string ffmpeg;

    if (!MediaDecoder::ffmpegExecutable(ffmpeg)) {
        if (getenv("AGB_MEDIA_DEBUG"))
            fprintf(stderr, "[media] pipeline: no ffmpeg\n");
        m_failed = true;
        return false;
    }

    if (getenv("AGB_MEDIA_DEBUG"))
        fprintf(stderr, "[media] pipeline start kind=%d %dx%d url=%s local=%s\n",
                (int)m_kind, m_width, m_height, m_url.utf8().data(),
                m_localPath.empty() ? "(none)" : m_localPath.c_str());

    {
        std::string target = m_localPath.empty()
            ? MediaDecoder::toolPathForURL(m_url.utf8().data())
            : m_localPath;

        m_impl->stopRequested = 0;
        m_impl->paused = 0;

        /* The clock starts now. seekBase is the position the decoders start
         * from: play() leaves it at 0, seek() sets it before restarting. */
        QueryPerformanceCounter(&m_impl->startedAt);
        m_impl->pausedAccum = 0;
        m_impl->pausedSince.QuadPart = 0;

        {
            char seekArg[32];
            bool seeking = m_impl->seekBase > 0.0005;

            if (seeking)
                snprintf(seekArg, sizeof(seekArg), "%.3f", m_impl->seekBase);

            /* --- audio decoder: PCM on stdout --- */
            if (hasAudio()) {
                std::vector<std::string> args;
                char rate[32], channels[32];

                snprintf(rate, sizeof(rate), "%d", kSampleRate);
                snprintf(channels, sizeof(channels), "%d", kChannels);

                args.push_back("-v"); args.push_back("error");
                if (seeking) { args.push_back("-ss"); args.push_back(seekArg); }
                args.push_back("-i"); args.push_back(target);
                args.push_back("-vn");

                /* playbackRate: tempo is done inside ffmpeg so the device and
                 * the video pacing stay in sync (0.5x..2x covers the spec's
                 * common range; atempo itself is happy further out). */
                if (m_rate != 1.0) {
                    char tempo[32];

                    snprintf(tempo, sizeof(tempo), "atempo=%.3f", m_rate);
                    args.push_back("-af"); args.push_back(tempo);
                }
                args.push_back("-f"); args.push_back("s16le");
                args.push_back("-acodec"); args.push_back("pcm_s16le");
                args.push_back("-ar"); args.push_back(rate);
                args.push_back("-ac"); args.push_back(channels);
                args.push_back("-");

                if (!spawnDecoder(ffmpeg, args, &m_impl->audioPipe, &m_impl->audioProcess)) {
                    m_failed = true;
                    return false;
                }

            {
                WAVEFORMATEX format;
                MMRESULT result;

                memset(&format, 0, sizeof(format));
                format.wFormatTag = WAVE_FORMAT_PCM;
                format.nChannels = (WORD)kChannels;
                format.nSamplesPerSec = kSampleRate;
                format.wBitsPerSample = 16;
                format.nBlockAlign = (WORD)(format.nChannels * format.wBitsPerSample / 8);
                format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

                result = waveOutOpen(&m_impl->waveOut, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);

                if (result != MMSYSERR_NOERROR) {
                    /* No audio device (or the WAVEFORMATEX is refused): the
                     * video side still plays, the clock falls back to wall
                     * time. This is a supported configuration. */
                    m_impl->waveOut = NULL;
                }
            }

            m_impl->audioThread = CreateThread(NULL, 0,
                (LPTHREAD_START_ROUTINE)pipelineAudioEntry, this, 0, NULL);
        }

        /* --- video decoder: raw BGRA frames on stdout --- */
        if (hasVideo() && m_width > 0 && m_height > 0) {
            std::vector<std::string> args;
            char size[64], rate[32];

            snprintf(size, sizeof(size), "%dx%d", m_width, m_height);
            snprintf(rate, sizeof(rate), "%d", kVideoFps);

                if (seeking) { args.push_back("-ss"); args.push_back(seekArg); }
            args.push_back("-i"); args.push_back(target);
            args.push_back("-an");
            args.push_back("-vf"); args.push_back(std::string("fps=") + rate + ",scale=" + size);
            args.push_back("-f"); args.push_back("rawvideo");
            args.push_back("-pix_fmt"); args.push_back("bgra");
            args.push_back("-");

            if (spawnDecoder(ffmpeg, args, &m_impl->videoPipe, &m_impl->videoProcess)) {
                m_impl->videoBytesPerFrame = m_width * m_height * 4;
                m_impl->frame.resize(m_impl->videoBytesPerFrame);
                m_impl->frameScratch.resize(m_impl->videoBytesPerFrame);

                m_impl->videoThread = CreateThread(NULL, 0,
                    (LPTHREAD_START_ROUTINE)pipelineVideoEntry, this, 0, NULL);
            }
        }

        return true;
        }
    }
#endif
}

void MediaPipeline::stop()
{
#if PLATFORM(WIN32)
    if (!m_impl)
        return;

    InterlockedExchange(&m_impl->stopRequested, 1);

    if (m_impl->waveOut) {
        waveOutReset(m_impl->waveOut);
    }

    /* Kill the decoders first: they are what the reader threads block on, so
     * closing their pipes makes ReadFile return instead of the waits below
     * sitting out their timeout (which used to make every seek take seconds). */
    killProcess(m_impl->audioProcess);
    m_impl->audioProcess = NULL;

    killProcess(m_impl->videoProcess);
    m_impl->videoProcess = NULL;

    if (m_impl->audioThread) {
        WaitForSingleObject(m_impl->audioThread, 2000);
        CloseHandle(m_impl->audioThread);
        m_impl->audioThread = NULL;
    }

    if (m_impl->videoThread) {
        WaitForSingleObject(m_impl->videoThread, 2000);
        CloseHandle(m_impl->videoThread);
        m_impl->videoThread = NULL;
    }

    if (m_impl->audioPipe) {
        CloseHandle(m_impl->audioPipe);
        m_impl->audioPipe = NULL;
    }

    if (m_impl->videoPipe) {
        CloseHandle(m_impl->videoPipe);
        m_impl->videoPipe = NULL;
    }

    if (m_impl->waveOut) {
        for (int i = 0; i < kBufferCount; ++i) {
            if (m_impl->buffers[i].lpData)
                free(m_impl->buffers[i].lpData);
            m_impl->buffers[i].lpData = NULL;
        }
        waveOutClose(m_impl->waveOut);
        m_impl->waveOut = NULL;
    }

#endif
}

void MediaPipeline::setPaused(bool paused)
{
#if PLATFORM(WIN32)
    if (!m_impl)
        return;

    if (paused == (m_impl->paused != 0))
        return;

    m_impl->paused = paused ? 1 : 0;

    if (m_impl->waveOut) {
        if (paused)
            waveOutPause(m_impl->waveOut);
        else
            waveOutRestart(m_impl->waveOut);
    }

    /* Video-only pipelines keep their own wall clock; freeze/unfreeze it so
     * a paused element does not advance. */
    {
        if (paused) {
            QueryPerformanceCounter(&m_impl->pausedSince);
        } else {
            LARGE_INTEGER now;

            QueryPerformanceCounter(&now);
            m_impl->pausedAccum += (double)(now.QuadPart - m_impl->pausedSince.QuadPart)
                                 / (double)m_impl->freq.QuadPart;
        }
    }
#endif
}

void MediaPipeline::setVolume(double volume)
{
    m_volume = volume;
#if PLATFORM(WIN32)
    if (m_impl && m_impl->waveOut) {
        DWORD value = (DWORD)(volume * 0xFFFF);

        waveOutSetVolume(m_impl->waveOut, (value << 16) | value);
    }
#endif
}

void MediaPipeline::setMuted(bool muted)
{
    m_muted = muted;
    setVolume(muted ? 0.0 : m_volume);
}

double MediaPipeline::currentTime() const
{
#if PLATFORM(WIN32)
    LARGE_INTEGER now;
    double elapsed;

    if (!m_impl)
        return 0;

    /* While the decoder is waiting for the download to reach the next second
     * the clock must not run away - the element is buffering, not playing. */
    if (m_impl->stalled)
        return m_impl->stalledAt;

    /* Wall clock minus paused time. Without audio this *is* the clock, and it
       runs faster or slower with playbackRate (video-only pipelines have no
       device to scale for them). */
    QueryPerformanceCounter(&now);
    elapsed = (double)(now.QuadPart - m_impl->startedAt.QuadPart) / (double)m_impl->freq.QuadPart;
    elapsed -= m_impl->pausedAccum;

    if (m_impl->paused && m_impl->pausedSince.QuadPart)
        elapsed -= (double)(now.QuadPart - m_impl->pausedSince.QuadPart) / (double)m_impl->freq.QuadPart;

    if (elapsed < 0)
        elapsed = 0;

    if (elapsed > 0 && !hasAudio())
        elapsed *= m_rate;

    elapsed += m_impl->seekBase;

    /* When audio is actually reaching the device, its own position is what the
       user hears: it already includes atempo, so it is the authority (and the
       only clock that stays correct for a rate other than 1x). Before the
       first buffer drains it reads 0, which is when the wall clock stands in. */
    if (m_impl->waveOut) {
        MMTIME time;
        double device = 0;

        memset(&time, 0, sizeof(time));
        time.wType = TIME_SAMPLES;

        if (waveOutGetPosition(m_impl->waveOut, &time, sizeof(time)) == MMSYSERR_NOERROR) {
            if (time.wType == TIME_SAMPLES)
                device = (double)time.u.sample / (double)kSampleRate;
            else if (time.wType == TIME_BYTES)
                device = (double)time.u.cb / (double)(kSampleRate * kChannels * kBytesPerSample);
            else
                device = (double)time.u.ms / 1000.0;

            /* The device position counts *output* PCM; atempo makes one second
             * of output carry `rate` seconds of media, so scale it back. */
            if (device > 0.02)
                elapsed = m_impl->seekBase + device * m_rate;
        }
    }

    return elapsed;
#else
    return 0;
#endif
}

void MediaPipeline::debugState(const char* where) const
{
#if PLATFORM(WIN32)
    if (!m_impl)
        return;

    {
        LARGE_INTEGER now;
        double wall;
        double device = -1;
        double ahead = -1;

        QueryPerformanceCounter(&now);
        wall = (double)(now.QuadPart - m_impl->startedAt.QuadPart) / (double)m_impl->freq.QuadPart;

        /* Device position is what proves audio is really being consumed:
         * waveOutGetPosition advances only while buffers drain to the card. */
        if (m_impl->waveOut) {
            MMTIME time;

            memset(&time, 0, sizeof(time));
            time.wType = TIME_SAMPLES;

            if (waveOutGetPosition(m_impl->waveOut, &time, sizeof(time)) == MMSYSERR_NOERROR) {
                if (time.wType == TIME_SAMPLES)
                    device = (double)time.u.sample / (double)kSampleRate;
                else if (time.wType == TIME_BYTES)
                    device = (double)time.u.cb / (double)(kSampleRate * kChannels * kBytesPerSample);
                else
                    device = (double)time.u.ms / 1000.0;

                ahead = device - wall;
            }
        }

        fprintf(stderr, "[media] %s: clock=%.3f wall=%.3f device=%.3f ahead=%.3f seekBase=%.3f paused=%d waveOut=%d frameValid=%d\n",
                where, currentTime(), wall, device, ahead, m_impl->seekBase,
                (int)m_impl->paused, m_impl->waveOut ? 1 : 0, (int)m_impl->frameValid);
    }
#else
    (void)where;
#endif
}

bool MediaPipeline::ended() const
{
    if (m_duration > 0 && currentTime() >= m_duration - 0.05)
        return true;
    return false;
}

void MediaPipeline::seek(double seconds)
{
    /* A seek restarts the decoders with `-ss`: exact enough for a browser
     * scrub, and it avoids keeping a demuxer state machine here. */
    if (seconds < 0)
        seconds = 0;

    {
        bool wasPaused = m_impl->paused != 0;

        stop();

        /* start() spawns both decoders from seekBase, so set it first. */
        m_impl->seekBase = seconds;
        m_impl->frameValid = false;
        m_impl->frameIndex = 0;
        m_impl->needsRestart = 0;
        m_impl->stalled = 0;
        m_impl->stalledAt = 0;

        if (!start())
            return;

        if (wasPaused)
            setPaused(true);
    }
}

bool MediaPipeline::needsRestart() const
{
    return m_impl && m_impl->needsRestart != 0;
}

void MediaPipeline::clearRestart()
{
    if (m_impl)
        m_impl->needsRestart = 0;
}

bool MediaPipeline::hasFrame() const
{
    return m_impl && m_impl->frameValid;
}

bool MediaPipeline::copyFrame(Vector<unsigned char>& out, double position, double maxAgeSeconds)
{
    bool copied = false;

    if (!m_impl || !m_impl->frameValid || m_impl->videoBytesPerFrame <= 0)
        return false;

#if PLATFORM(WIN32)
    EnterCriticalSection(&m_impl->frameLock);
#endif

    {
        double stamp = m_impl->frameStamp;

        /* The decoded frame trails the clock by up to one frame, and further
         * behind after a slow decode or a seek. Accept it as long as it is not
         * stale by more than the caller's tolerance. */
        if (position - stamp <= maxAgeSeconds) {
            out = m_impl->frame;
            copied = true;
        }
    }

#if PLATFORM(WIN32)
    LeaveCriticalSection(&m_impl->frameLock);
#endif

    return copied;
}

/* ------------------------------------------------------------------------- */
/* buffering / rate                                                          */
/* ------------------------------------------------------------------------- */

double MediaPipeline::bufferedSeconds() const
{
    if (!m_streaming)
        return m_duration;

    {
        std::string url = m_url.utf8().data();
        unsigned long long have = MediaDownloader::bytes(url);
        unsigned long long total = MediaDownloader::total(url);

        /* The byte frontier maps to media time through the file's overall
         * bitrate (size / duration), which is accurate enough for a progress
         * bar and needs no per-packet index. */
        if (m_duration > 0 && total > 0)
            return m_duration * (double)have / (double)total;
    }

    return currentTime();
}

bool MediaPipeline::isStalled() const
{
    return m_impl && m_impl->stalled != 0;
}

void MediaPipeline::setRate(double rate)
{
    if (rate <= 0)
        rate = 1.0;

    if (rate < 0.25)
        rate = 0.25;
    if (rate > 4.0)
        rate = 4.0;

    if (rate == m_rate)
        return;

    m_rate = rate;

    /* ffmpeg's atempo is fixed per process, so a rate change is a re-spawn at
     * the current position (the audio device is restarted too). */
    if (m_impl && !m_impl->stopRequested && (m_impl->audioThread || m_impl->videoThread))
        seek(currentTime());
}

/* ------------------------------------------------------------------------- */
/* decoder threads                                                           */
/* ------------------------------------------------------------------------- */

#if PLATFORM(WIN32)

/* A decoder ran out of bytes. If the resource is still arriving, freeze the
 * clock at the byte frontier and ask the engine to re-spawn the decoders at
 * that position once enough new data has landed; the decoders do not retry by
 * themselves (ffmpeg exits at EOF on a growing file).
 *
 * Returns true when nothing more is coming (the caller should treat it as the
 * real end); false means "restart requested, stop this thread". */
bool MediaPipeline::waitForMoreData()
{
    if (!m_streaming || MediaDownloader::finished(m_url.utf8().data()))
        return true;

    if (!m_impl->stalled) {
        m_impl->stalledAt = currentTime();
        m_impl->stalled = 1;
    }

    {
        std::string url = m_url.utf8().data();

        /* One restart per chunk of downloaded data. The chunk must be worth
         * the (re)spawn cost but small enough that the player does not sit
         * idle while the rest of a file arrives: 512 KB is ~20 s of a
         * typical web video. */
        const unsigned long long kRestartChunk = 512 * 1024;
        unsigned long long seen = MediaDownloader::bytes(url);

        while (!m_impl->stopRequested) {
            unsigned long long now = MediaDownloader::bytes(url);

            if (MediaDownloader::finished(url)) {
                if (now > seen) {
                    m_impl->needsRestart = 1;
                    return false;
                }
                return true;
            }

            if (now >= seen + kRestartChunk) {
                m_impl->needsRestart = 1;
                return false;
            }

            Sleep(50);
        }
    }

    return true;
}

/* Audio: ffmpeg's PCM on stdout -> waveOut buffers. The ring is small (6 x
 * 100 ms) so pause/seek respond immediately and memory stays bounded. */
void mediaPipelineThreadAudio(MediaPipeline* pipeline)
{
    MediaPipeline::Impl* impl = pipeline->m_impl;
    int bufferBytes = impl->sampleRate * impl->channels * kBytesPerSample * kBufferMillis / 1000;
    int index = 0;
    bool primed = false;

    for (int i = 0; i < kBufferCount; ++i) {
        impl->buffers[i].lpData = (LPSTR)malloc(bufferBytes);
        impl->buffers[i].dwBufferLength = bufferBytes;
        impl->buffers[i].dwFlags = WHDR_DONE;
    }

    while (!impl->stopRequested) {
        WAVEHDR* header = &impl->buffers[index];

        /* Recycle a finished buffer; when none is free, sleep a little. This
         * is the natural rate limiter: the decoder is pulled as the device
         * drains, never buffered into memory. */
        if (!(header->dwFlags & WHDR_DONE) || header->dwFlags & WHDR_INQUEUE) {
            Sleep(5);
            continue;
        }

        {
            DWORD filled = 0;

            while (filled < (DWORD)bufferBytes && !impl->stopRequested) {
                DWORD chunk = 0;
                DWORD want = (DWORD)bufferBytes - filled;

                if (!ReadFile(impl->audioPipe, header->lpData + filled, want, &chunk, NULL) || chunk == 0)
                    break;
                filled += chunk;
            }

            if (filled == 0) {
                /* Out of PCM: either the decoder finished or the download
                 * frontier moved on (streaming). */
                if (!pipeline->waitForMoreData())
                    return;
                break;
            }

            header->dwBufferLength = filled;
            header->dwFlags = 0;
            header->dwLoops = 0;

            if (impl->waveOut) {
                if (!primed) {
                    /* The first buffer starts the device; priming avoids the
                     * initial underrun click. */
                    waveOutPrepareHeader(impl->waveOut, header, sizeof(WAVEHDR));
                    waveOutWrite(impl->waveOut, header, sizeof(WAVEHDR));
                    primed = true;
                } else {
                    waveOutPrepareHeader(impl->waveOut, header, sizeof(WAVEHDR));
                    waveOutWrite(impl->waveOut, header, sizeof(WAVEHDR));
                }
            }

            index = (index + 1) % kBufferCount;
        }
    }

    /* No device: drain-and-discard so the decoder does not block on a full
     * pipe. The wall clock still drives currentTime. */
    if (!impl->waveOut) {
        char drain[8192];

        while (!impl->stopRequested) {
            DWORD got = 0;

            if (!ReadFile(impl->audioPipe, drain, sizeof(drain), &got, NULL) || got == 0)
                break;
        }
    }
}

/* Video: raw BGRA frames on stdout -> newest-only frame buffer. ffmpeg decodes
 * as fast as the pipe allows, so a frame is *held* until its own timestamp
 * comes due on the playback clock; without that wait the whole file would be
 * consumed in a second and the element would show its last frame (typically
 * black) from the start. Frames that arrive late are shown at once, so a slow
 * decode drops frames instead of slowing the clock. */
void mediaPipelineThreadVideo(MediaPipeline* pipeline)
{
    MediaPipeline::Impl* impl = pipeline->m_impl;
    int frameBytes = impl->videoBytesPerFrame;
    double frameInterval = 1.0 / (double)kVideoFps;

    if (frameBytes <= 0)
        return;

    while (!impl->stopRequested) {
        DWORD got = 0;
        DWORD want = (DWORD)frameBytes;
        DWORD filled = 0;

        while (filled < want && !impl->stopRequested) {
            if (!ReadFile(impl->videoPipe, impl->frameScratch.data() + filled, want - filled, &got, NULL) || got == 0)
                break;
            filled += got;
        }

        if (filled < want) {
            /* Out of frames: either the decoder finished or the download
             * frontier moved on (streaming). */
            if (!pipeline->waitForMoreData())
                return;
            break;
        }

        {
            double due = impl->seekBase + (double)impl->frameIndex * frameInterval;

            /* The clock is authoritative: for audio that is the device
             * position (what the user hears), for video-only the wall clock.
             * Pause freezes it, so the wait resumes where it stopped. */
            while (!impl->stopRequested && pipeline->currentTime() + 0.0005 < due)
                Sleep(2);
        }

        if (impl->stopRequested)
            break;

        EnterCriticalSection(&impl->frameLock);
        memcpy(impl->frame.data(), impl->frameScratch.data(), frameBytes);
        impl->frameStamp = impl->seekBase + (double)impl->frameIndex * frameInterval;
        impl->frameValid = true;
        ++impl->frameIndex;
        LeaveCriticalSection(&impl->frameLock);
    }
}

#else

void mediaPipelineThreadAudio(MediaPipeline* pipeline) { (void)pipeline; }
void mediaPipelineThreadVideo(MediaPipeline* pipeline) { (void)pipeline; }

#endif

} // namespace WebCore
