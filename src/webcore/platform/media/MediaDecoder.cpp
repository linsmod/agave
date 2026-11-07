/*
 * Copyright (C) 2024 Zhang Ji Peng (onecoolx@gmail.com)
 *
 * Media probing / frame grabbing behind HTMLMediaElement. See MediaDecoder.h
 * and docs/arch-media.md.
 *
 * Threading contract: the engine thread calls probe/pollProbe/cancel; worker
 * threads only ever touch their own Job and the shared job list, which is
 * guarded by a critical section. No WebCore object is touched off-thread.
 */

#include "config.h"

#include "MediaDecoder.h"

#include "MediaDownloader.h"
#include "ResourceError.h"
#include "ResourceHandle.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>

#if PLATFORM(WIN32)
#include <windows.h>
#endif

namespace WebCore {

namespace MediaDecoder {

/* ------------------------------------------------------------------------- */
/* tool discovery                                                            */
/* ------------------------------------------------------------------------- */

#if PLATFORM(WIN32)
static bool fileIsExecutable(const std::string& path)
{
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
#else
static bool fileIsExecutable(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");

    if (!f)
        return false;
    fclose(f);
    return true;
}
#endif

static bool findTool(const char* envName, const char* exeName, std::string& out)
{
    const char* explicitPath = getenv(envName);

    if (explicitPath && *explicitPath) {
        if (fileIsExecutable(explicitPath)) {
            out = explicitPath;
            return true;
        }
    }

    /* Install locations this machine actually has (chocolatey ships ffmpeg
     * there); keep the list short - PATH lookup is the general answer. */
    static const char* const dirs[] = {
        "C:\\ProgramData\\chocolatey\\bin\\",
        "C:\\Program Files\\ffmpeg\\bin\\",
        "C:\\ffmpeg\\bin\\"
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        std::string path = std::string(dirs[i]) + exeName;

        if (fileIsExecutable(path)) {
            out = path;
            return true;
        }
    }

    /* Bare name: CreateProcess still resolves it through PATH, but only claim
     * availability when it is really there - callers use this to decide
     * whether to start a worker at all. */
    {
        std::string path = std::string(exeName);
        const char* pathEnv = getenv("PATH");

        if (!pathEnv)
            return false;

        std::string search(pathEnv);
        size_t start = 0;

        while (start <= search.size()) {
            size_t end = search.find(';', start);

            if (end == std::string::npos)
                end = search.size();

            if (end > start) {
                std::string dir = search.substr(start, end - start);
                std::string candidate = dir + "\\" + exeName;

                if (fileIsExecutable(candidate)) {
                    out = candidate;
                    return true;
                }
            }

            if (end == search.size())
                break;
            start = end + 1;
        }
    }

    return false;
}

bool ffprobeAvailable()
{
    std::string path;
    bool found = findTool("AGB_FFPROBE", "ffprobe.exe", path);

    if (getenv("AGB_MEDIA_DEBUG"))
        fprintf(stderr, "[media] ffprobe %s path=%s\n", found ? "found" : "missing",
                found ? path.c_str() : "-");
    return found;
}

bool ffmpegAvailable()
{
    std::string path;
    return findTool("AGB_FFMPEG", "ffmpeg.exe", path);
}

bool ffprobeExecutable(std::string& out)
{
    return findTool("AGB_FFPROBE", "ffprobe.exe", out);
}

bool ffmpegExecutable(std::string& out)
{
    return findTool("AGB_FFMPEG", "ffmpeg.exe", out);
}

std::string toolPathForURL(const std::string& url);

/* ------------------------------------------------------------------------- */
/* local reads                                                               */
/* ------------------------------------------------------------------------- */

/* "file:///E:/x/y.mp4" -> "E:/x/y.mp4" (percent decoded, query stripped).
 * Mirrors the network backend's helper so the media layer does not depend on
 * backend internals. */
static bool localPathForFileUrl(const char* url, std::string* out)
{
    if (!url || !out || strncasecmp(url, "file:", 5) != 0)
        return false;

    const char* rest = url + 5;

    if (strncmp(rest, "//", 2) != 0)
        return false;
    rest += 2;

    std::string path(rest);

    /* "file:///E:/x" keeps one slash after the authority ("file://" + "/E:/x");
     * Windows paths never start with a slash, so drop it when a drive letter
     * follows. Without this, fopen("/E:/x") fails and the URL looks like an
     * unsupported resource. */
    if (path.size() > 2 && path[0] == '/' && path[2] == ':'
            && ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z')))
        path.erase(0, 1);
    size_t cut = path.find_first_of("?#");

    if (cut != std::string::npos)
        path.resize(cut);

    {
        std::string decoded;
        decoded.reserve(path.size());

        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '%' && i + 2 < path.size()) {
                int hi = path[i + 1];
                int lo = path[i + 2];

                if (hi >= '0' && hi <= '9') hi -= '0';
                else if (hi >= 'a' && hi <= 'f') hi = hi - 'a' + 10;
                else if (hi >= 'A' && hi <= 'F') hi = hi - 'A' + 10;
                else { decoded += path[i]; continue; }

                if (lo >= '0' && lo <= '9') lo -= '0';
                else if (lo >= 'a' && lo <= 'f') lo = lo - 'a' + 10;
                else if (lo >= 'A' && lo <= 'F') lo = lo - 'A' + 10;
                else { decoded += path[i]; continue; }

                decoded += (char)((hi << 4) | lo);
                i += 2;
            } else {
                decoded += path[i];
            }
        }
        *out = decoded;
    }

    return true;
}

static bool readLocalFile(const std::string& path, size_t maxBytes, Vector<unsigned char>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    unsigned char buffer[65536];
    size_t total = 0;

    if (!f)
        return false;

    while (total < maxBytes) {
        size_t want = sizeof(buffer);
        size_t got;

        if (maxBytes - total < want)
            want = maxBytes - total;

        got = fread(buffer, 1, want, f);
        if (!got)
            break;

        for (size_t i = 0; i < got; ++i)
            out.append(buffer[i]);
        total += got;
    }

    fclose(f);
    return total > 0;
}

/* The header slice for the in-process parser. Local files only: an http(s)
 * body is fetched by the background downloader (MediaDownloader) and parsed
 * from the growing local copy - see probe() below. */
static bool readMediaHeader(const String& url, bool allowNetwork, Vector<unsigned char>& out, size_t maxBytes)
{
    (void)allowNetwork;

    std::string urlText = url.utf8().data();
    std::string path;

    out.clear();

    if (localPathForFileUrl(urlText.c_str(), &path))
        return readLocalFile(path, maxBytes, out);

    return false;
}

std::string toolPathForURL(const std::string& url)
{
    std::string path;

    if (localPathForFileUrl(url.c_str(), &path))
        return path;
    return url;
}

/* ------------------------------------------------------------------------- */
/* downloaded media cache                                                    */
/* ------------------------------------------------------------------------- */

/* ffmpeg/ffprobe are spawned inside the engine's own sandbox and cannot
 * complete a TLS handshake there ("Peer certificate failed verification"),
 * while the engine's network stack reaches the same URL fine. So an http(s)
 * resource is fetched once by the engine and decoded from a local copy: the
 * tools only ever see files, and a seek is instant. The copy is named by a
 * hash of the URL and lives under %LOCALAPPDATA%\Agave\cache\media.
 *
 * Consequence (documented in docs/arch-media.md section 7): a remote video is
 * downloaded in full before it plays - there is no progressive streaming. */
#if PLATFORM(WIN32)
/* A directory we can actually write to, creating it if needed. The engine may
 * run inside a sandbox whose write access is limited to a few roots, so each
 * candidate is verified with a real file instead of trusting CreateDirectory's
 * return value. */
static bool writableDirectory(const std::string& dir)
{
    {
        std::string probe = dir + "\\.write-test";
        FILE* f = fopen(probe.c_str(), "wb");

        if (!f)
            return false;

        fclose(f);
        DeleteFileA(probe.c_str());
    }

    return true;
}

static std::string cacheDirectory()
{
    static std::string dir;
    static bool resolved = false;

    if (resolved)
        return dir;

    resolved = true;

    /* Explicit override first: AGB_MEDIA_CACHE=<dir> (the probe harness uses
     * it when the process may only write inside its workspace). */
    {
        const char* override = getenv("AGB_MEDIA_CACHE");

        if (override && *override) {
            std::string candidate = override;

            CreateDirectoryA(candidate.c_str(), NULL);

            if (writableDirectory(candidate))
                dir = candidate;
        }
    }

    if (dir.empty()) {
        char base[MAX_PATH] = {0};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));

        if (n > 0 && n < sizeof(base)) {
            std::string root = std::string(base) + "\\Agave";
            std::string candidate = root + "\\cache\\media";

            CreateDirectoryA((root + "\\cache").c_str(), NULL);
            CreateDirectoryA(candidate.c_str(), NULL);

            if (writableDirectory(candidate))
                dir = candidate;
        }
    }

    if (dir.empty()) {
        char tmp[MAX_PATH] = {0};

        if (GetTempPathA(sizeof(tmp), tmp)) {
            std::string candidate = std::string(tmp) + "agave-media-cache";

            CreateDirectoryA(candidate.c_str(), NULL);

            if (writableDirectory(candidate))
                dir = candidate;
        }
    }

    if (dir.empty()) {
        char base[MAX_PATH] = {0};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));

        if (n > 0 && n < sizeof(base)) {
            std::string candidate = std::string(base) + "\\Agave\\probe\\media-cache";

            CreateDirectoryA(candidate.c_str(), NULL);

            if (writableDirectory(candidate))
                dir = candidate;
        }
    }

    /* Last resort: next to the executable (a read-only install gets none, and
     * remote media then falls back to the URL, which the tools may reject). */
    if (dir.empty()) {
        char exe[MAX_PATH] = {0};

        if (GetModuleFileNameA(NULL, exe, sizeof(exe))) {
            std::string candidate(exe);
            size_t slash = candidate.find_last_of("\\/");

            if (slash != std::string::npos) {
                candidate.resize(slash);
                candidate += "\\media-cache";
                CreateDirectoryA(candidate.c_str(), NULL);

                if (writableDirectory(candidate))
                    dir = candidate;
            }
        }
    }

    if (getenv("AGB_MEDIA_DEBUG"))
        fprintf(stderr, "[media] media cache: %s\n", dir.empty() ? "(none)" : dir.c_str());

    return dir;
}

static std::string cacheFileNameForURL(const std::string& url)
{
    std::string out = cacheDirectory();
    unsigned long long hash = 1469598103934665603ULL;

    if (out.empty())
        return out;

    out += "\\";

    {
        size_t i;

        for (i = 0; i < url.size(); ++i) {
            hash ^= (unsigned char)url[i];
            hash *= 1099511628211ULL;
        }
    }

    {
        char name[32];

        snprintf(name, sizeof(name), "%016llx", hash);
        out += name;
    }

    /* Keep the extension: ffmpeg guesses the container from it, and the
     * header parser does not have to sniff every byte. */
    {
        size_t dot = url.rfind('.');
        size_t slash = url.find_last_of("/?");
        bool usable = dot != std::string::npos && dot + 1 < url.size()
                   && url.size() - dot <= 6
                   && (slash == std::string::npos || dot > slash);

        if (usable) {
            size_t i;

            for (i = dot + 1; i < url.size(); ++i) {
                if (!isalnum((unsigned char)url[i])) {
                    usable = false;
                    break;
                }
            }
        }

        out += usable ? url.substr(dot) : ".bin";
    }

    return out;
}
/* Reads the first `maxBytes` of a growing file (the downloader's copy)
 * without disturbing the writer. */
static bool readFilePrefix(const std::string& path, size_t maxBytes, Vector<unsigned char>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    unsigned char buffer[65536];
    size_t total = 0;

    out.clear();

    if (!f)
        return false;

    while (total < maxBytes) {
        size_t want = sizeof(buffer);
        size_t got;

        if (want > maxBytes - total)
            want = maxBytes - total;

        got = fread(buffer, 1, want, f);

        if (!got)
            break;

        {
            size_t i;

            for (i = 0; i < got; ++i)
                out.append(buffer[i]);
        }

        total += got;
    }

    fclose(f);
    return total > 0;
}

bool localPathForURL(const std::string& url, std::string& out)
{
    {
        std::string path;

        if (localPathForFileUrl(url.c_str(), &path)) {
            out = path;
            return true;
        }
    }

    if (strncasecmp(url.c_str(), "http:", 5) != 0
            && strncasecmp(url.c_str(), "https:", 5) != 0)
        return false;

    {
        std::string path = cacheFileNameForURL(url);

        /* Present means readable: the downloader writes this file in place and
         * the decoders may start on a partial copy (streaming). */
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            out = path;
            return true;
        }

        return false;
    }
}
#else
bool localPathForURL(const std::string& url, std::string& out)
{
    std::string path;

    if (localPathForFileUrl(url.c_str(), &path)) {
        out = path;
        return true;
    }
    return false;
}
#endif

bool readHeaderBytes(const String& url, size_t maxBytes, Vector<unsigned char>& out)
{
    return readMediaHeader(url, false, out, maxBytes);
}

std::string cachePathForURL(const std::string& url)
{
#if PLATFORM(WIN32)
    return cacheFileNameForURL(url);
#else
    (void)url;
    return std::string();
#endif
}

bool localCopyComplete(const std::string& url)
{
#if PLATFORM(WIN32)
    std::string path;
    std::string local;
    std::string path_;

    if (localPathForFileUrl(url.c_str(), &path))
        return true;

    if (strncasecmp(url.c_str(), "http:", 5) != 0
            && strncasecmp(url.c_str(), "https:", 5) != 0)
        return true;

    path_ = cacheFileNameForURL(url);

    /* No copy at all: nothing is "complete", but there is also nothing to wait
     * for - the caller treats it as not streaming. */
    if (path_.empty() || GetFileAttributesA(path_.c_str()) == INVALID_FILE_ATTRIBUTES)
        return true;

    if (MediaDownloader::finished(url))
        return true;

    return GetFileAttributesA((path_ + ".complete").c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    (void)url;
    return true;
#endif
}

/* ------------------------------------------------------------------------- */
/* worker jobs                                                               */
/* ------------------------------------------------------------------------- */

struct Job {
    bool probe;                 /* true: ffprobe metadata, false: ffmpeg frame */
    const void* element;
    std::string url;
    double time;
    volatile LONG done;
    bool ok;
    MediaInfo info;
    std::vector<unsigned char> bytes;
#if PLATFORM(WIN32)
    HANDLE thread;
#endif
};

static std::vector<Job*>* g_jobs = 0;

/* Finished jobs waiting to be picked up by their owner. Both tables are only
 * touched on the engine thread (pump / probe / poll / cancel); worker threads
 * see `Job::done` and never these vectors. */
static std::vector<Job*> g_results;

#if PLATFORM(WIN32)
static CRITICAL_SECTION* g_jobsLock = 0;

static void lockJobs()
{
    if (!g_jobsLock) {
        g_jobsLock = (CRITICAL_SECTION*)malloc(sizeof(CRITICAL_SECTION));
        InitializeCriticalSection(g_jobsLock);
    }
    EnterCriticalSection(g_jobsLock);
}

static void unlockJobs()
{
    LeaveCriticalSection(g_jobsLock);
}
#else
static void lockJobs() { }
static void unlockJobs() { }
#endif

/* Runs `exe` with argv (no shell involved: every argument is quoted here),
 * collecting stdout bytes. */
static bool runTool(const std::string& exe, const std::vector<std::string>& args,
                    std::vector<unsigned char>& out)
{
#if PLATFORM(WIN32)
    SECURITY_ATTRIBUTES sa;
    HANDLE readPipe = NULL;
    HANDLE writePipe = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    std::string commandLine;
    bool ok = false;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return false;

    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    commandLine = "\"" + exe + "\"";
    for (size_t i = 0; i < args.size(); ++i)
        commandLine += " \"" + args[i] + "\"";

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;       /* ffprobe writes diagnostics to stderr */
    si.hStdInput = NULL;

    memset(&pi, 0, sizeof(pi));

    {
        std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back('\0');

        if (CreateProcessA(NULL, &mutableCommand[0], NULL, NULL, TRUE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            unsigned char buffer[8192];
            DWORD avail = 0;
            DWORD got = 0;

            CloseHandle(writePipe);
            writePipe = NULL;

            for (;;) {
                if (PeekNamedPipe(readPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                    if (ReadFile(readPipe, buffer, sizeof(buffer), &got, NULL) && got) {
                        out.insert(out.end(), buffer, buffer + got);
                        continue;
                    }
                }

                if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
                    break;

                Sleep(2);
            }

            while (PeekNamedPipe(readPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                if (ReadFile(readPipe, buffer, sizeof(buffer), &got, NULL) && got)
                    out.insert(out.end(), buffer, buffer + got);
            }

            WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            ok = !out.empty();
        }
    }

    if (writePipe)
        CloseHandle(writePipe);
    CloseHandle(readPipe);
    return ok;
#else
    (void)exe;
    (void)args;
    (void)out;
    return false;
#endif
}

/* ffprobe -of json output is machine generated; scrape the two fields we
 * need. The key match is anchored on the quoted name so a nested "duration"
 * in another block cannot be picked up by accident. */
static bool jsonNumberAfter(const std::string& text, const char* key, double* out)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = text.find(needle);
    char* end = 0;
    double value;

    if (pos == std::string::npos)
        return false;

    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;

    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'))
        ++pos;

    if (pos >= text.size())
        return false;

    if (text[pos] == '"')
        ++pos;

    value = strtod(text.c_str() + pos, &end);
    if (end == text.c_str() + pos)
        return false;

    *out = value;
    return true;
}

/* Tools take a path, not a URL: ffprobe/ffmpeg on Windows do not understand
 * "file:" at all ("No such file or directory"). */
static std::string toolArgumentForURL(const std::string& url)
{
    std::string path;

    if (localPathForFileUrl(url.c_str(), &path))
        return path;
    return url;
}

static void runProbe(Job* job)
{
    std::string exe;

    if (!findTool("AGB_FFPROBE", "ffprobe.exe", exe))
        return;

    {
        std::vector<std::string> args;
        std::vector<unsigned char> output;

        args.push_back("-v");
        args.push_back("error");
        args.push_back("-show_entries");
        args.push_back("format=duration:stream=width,height,codec_type");
        args.push_back("-of");
        args.push_back("json");
        args.push_back(toolArgumentForURL(job->url));

        if (getenv("AGB_MEDIA_DEBUG"))
            fprintf(stderr, "[media] ffprobe %s [%s]\n", exe.c_str(), args.back().c_str());

        if (!runTool(exe, args, output)) {
            if (getenv("AGB_MEDIA_DEBUG"))
                fprintf(stderr, "[media] ffprobe failed, bytes=%d\n", (int)output.size());
            if (getenv("AGB_MEDIA_DEBUG"))
                fprintf(stderr, "[media] ffprobe produced no output for %s\n", job->url.c_str());
            return;
        }

        {
            std::string text(output.begin(), output.end());
            MediaInfo info;
            double value = 0;

            if (jsonNumberAfter(text, "duration", &value))
                info.duration = value;
            if (jsonNumberAfter(text, "width", &value))
                info.width = (int)value;
            if (jsonNumberAfter(text, "height", &value))
                info.height = (int)value;

            info.hasVideo = text.find("\"codec_type\": \"video\"") != std::string::npos
                         || text.find("\"codec_type\":\"video\"") != std::string::npos;
            info.hasAudio = text.find("\"codec_type\": \"audio\"") != std::string::npos
                         || text.find("\"codec_type\":\"audio\"") != std::string::npos;

            info.ok = info.duration > 0 || info.width > 0 || info.hasVideo || info.hasAudio;
            info.mimeType = mediaMimeTypeForURL(String::fromUTF8(job->url.c_str()));

            if (!info.ok && getenv("AGB_MEDIA_DEBUG"))
                fprintf(stderr, "[media] ffprobe unusable output (%d bytes): %.300s\n",
                        (int)text.size(), text.c_str());

            job->info = info;
            job->ok = info.ok;
        }
    }
}

static void runFrame(Job* job)
{
    std::string exe;

    if (!findTool("AGB_FFMPEG", "ffmpeg.exe", exe))
        return;

    {
        std::vector<std::string> args;
        std::vector<unsigned char> png;
        char timeText[64];

        snprintf(timeText, sizeof(timeText), "%.3f", job->time);

        args.push_back("-v");
        args.push_back("error");
        args.push_back("-ss");
        args.push_back(timeText);
        args.push_back("-i");
        args.push_back(toolArgumentForURL(job->url));
        args.push_back("-frames:v");
        args.push_back("1");
        args.push_back("-f");
        args.push_back("image2pipe");
        args.push_back("-vcodec");
        args.push_back("png");
        args.push_back("-");

        if (runTool(exe, args, png)) {
            job->bytes = png;
            job->ok = true;
        }
    }
}

#if PLATFORM(WIN32)
static DWORD WINAPI jobThread(void* param)
{
    Job* job = (Job*)param;

    if (job->probe)
        runProbe(job);
    else
        runFrame(job);

    InterlockedExchange(&job->done, 1);
    return 0;
}
#endif

static void startJob(Job* job)
{
    if (!g_jobs)
        g_jobs = new std::vector<Job*>();

    lockJobs();
    g_jobs->push_back(job);
    unlockJobs();

#if PLATFORM(WIN32)
    job->thread = CreateThread(NULL, 0, jobThread, job, 0, NULL);

    if (!job->thread) {
        job->ok = false;
        InterlockedExchange(&job->done, 1);
    }
#endif
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

void pump()
{
    if (!g_jobs)
        return;

    std::vector<Job*> finished;

    lockJobs();

    for (size_t i = 0; i < g_jobs->size(); ) {
        Job* job = (*g_jobs)[i];

        if (job->done) {
            finished.push_back(job);
            (*g_jobs)[i] = (*g_jobs)[g_jobs->size() - 1];
            g_jobs->pop_back();
        } else {
            ++i;
        }
    }

    unlockJobs();

    /* Finished jobs move to the result table; the owning element picks them
     * up from pollProbe()/pollFrame() on its own timer. */
    for (size_t i = 0; i < finished.size(); ++i) {
        Job* job = finished[i];
        bool replaced = false;

        /* A newer job for the same element and kind supersedes an older
         * unread result (a page that seeks repeatedly must not build a
         * backlog of frames). */
        for (size_t k = 0; k < g_results.size(); ++k) {
            if (g_results[k]->element == job->element && g_results[k]->probe == job->probe) {
#if PLATFORM(WIN32)
                if (g_results[k]->thread)
                    CloseHandle(g_results[k]->thread);
#endif
                delete g_results[k];
                g_results[k] = job;
                replaced = true;
                break;
            }
        }

        if (!replaced)
            g_results.push_back(job);
    }
}

/* ------------------------------------------------------------------------- */
/* streaming probes                                                          */
/* ------------------------------------------------------------------------- */

/* An http(s) resource is downloaded by MediaDownloader into the media cache.
 * The metadata probe then parses the *growing* local file, so `loadedmetadata`
 * (and playback) start long before the body is complete. */
struct PendingProbe {
    const void* element;
    std::string url;
    std::string path;
};

static std::vector<PendingProbe>* g_pendingProbes = 0;

static bool isNetworkMediaURL(const std::string& url)
{
    return !strncasecmp(url.c_str(), "http:", 5) || !strncasecmp(url.c_str(), "https:", 5);
}

/* Tries to get `out` from the local copy of a network resource: the file
 * exists from the first chunk, and the `.complete` marker tracks the end. */
static bool probeNetworkCopy(const std::string& url, const std::string& path, MediaInfo& out)
{
    const size_t kHeaderBytes = 1024 * 1024;
    const unsigned long long kEnoughBytes = 64 * 1024;
    Vector<unsigned char> data;

    {
        unsigned long long have = MediaDownloader::bytes(url);
        bool done = MediaDownloader::finished(url);

        /* Too little to hold a container header yet. */
        if (!done && have < kEnoughBytes)
            return false;

        if (!readFilePrefix(path, kHeaderBytes, data) || data.isEmpty())
            return false;

        {
            bool parsed = parseMediaHeader(data.data(), data.size(), out);

            /* The container parser reports "recognized" as soon as it sees the
             * magic, even when the header is still truncated - keep polling
             * until the fields a player needs are actually there. */
            bool complete = parsed && (out.duration > 0 || (out.width > 0 && out.height > 0));

            if (getenv("AGB_MEDIA_DEBUG"))
                fprintf(stderr, "[media] probe copy: bytes=%llu done=%d read=%d parsed=%d complete=%d dur=%.3f %dx%d\n",
                        have, (int)done, (int)data.size(), (int)parsed, (int)complete,
                        out.duration, out.width, out.height);

            return complete;
        }
    }
}

ProbeStatus probe(const void* element, const String& url, MediaInfo& out)
{
    const std::string urlText = url.utf8().data();
    bool forceExternal = getenv("AGB_MEDIA_PROBE")
        && !strcmp(getenv("AGB_MEDIA_PROBE"), "ffprobe");

    /* Local files: in-process header parse, no thread. */
    if (!forceExternal && !isNetworkMediaURL(urlText)) {
        Vector<unsigned char> data;

        if (readMediaHeader(url, false, data, 1024 * 1024)
                && data.size() > 0
                && parseMediaHeader(data.data(), data.size(), out))
            return ProbeReady;
    }

    if (!forceExternal && isNetworkMediaURL(urlText)) {
        std::string path = cachePathForURL(urlText);

        if (!path.empty()) {
            /* The downloader writes `path` directly and drops a `.complete`
             * marker when the body is whole (see MediaDownloader.h). */
            std::string local = path;

            MediaDownloader::ensure(urlText, path);

            if (probeNetworkCopy(urlText, local, out))
                return ProbeReady;

            if (!MediaDownloader::failed(urlText)) {
                if (!g_pendingProbes)
                    g_pendingProbes = new std::vector<PendingProbe>();

                {
                    PendingProbe pending;

                    pending.element = element;
                    pending.url = urlText;
                    pending.path = local;
                    g_pendingProbes->push_back(pending);
                }

                return ProbePending;
            }
        }
    }

    /* Everything else: ffprobe on a worker (a local copy when there is one,
     * the URL otherwise). */
    if (!ffprobeAvailable())
        return ProbeFailed;

    {
        Job* job = new Job();
        std::string local;

        job->probe = true;
        job->element = element;
        job->url = localPathForURL(urlText, local) ? local : urlText;
        job->time = 0;
        job->done = 0;
        job->ok = false;
#if PLATFORM(WIN32)
        job->thread = NULL;
#endif

        startJob(job);
        return ProbePending;
    }
}

ProbeStatus pollProbe(const void* element, MediaInfo& out)
{
    /* A download that is still feeding the header parse. */
    for (size_t i = 0; g_pendingProbes && i < g_pendingProbes->size(); ++i) {
        PendingProbe& pending = (*g_pendingProbes)[i];

        if (pending.element != element)
            continue;

        if (probeNetworkCopy(pending.url, pending.path, out)) {
            g_pendingProbes->erase(g_pendingProbes->begin() + i);
            return ProbeReady;
        }

        if (MediaDownloader::failed(pending.url)) {
            g_pendingProbes->erase(g_pendingProbes->begin() + i);
            return ProbeFailed;
        }

        /* The body is complete but the built-in parser did not like it: hand
         * the finished local file to ffprobe. */
        if (MediaDownloader::finished(pending.url)) {
            std::string local = pending.path;

            g_pendingProbes->erase(g_pendingProbes->begin() + i);

            if (ffprobeAvailable()) {
                Job* job = new Job();

                job->probe = true;
                job->element = element;
                job->url = local;
                job->time = 0;
                job->done = 0;
                job->ok = false;
#if PLATFORM(WIN32)
                job->thread = NULL;
#endif
                startJob(job);
                return ProbePending;
            }

            return ProbeFailed;
        }

        return ProbePending;
    }

    for (size_t i = 0; i < g_results.size(); ++i) {
        Job* job = g_results[i];
        ProbeStatus status;

        if (!job->probe || job->element != element)
            continue;

        g_results.erase(g_results.begin() + i);

        status = job->ok ? ProbeReady : ProbeFailed;

        if (job->ok)
            out = job->info;

#if PLATFORM(WIN32)
        if (job->thread)
            CloseHandle(job->thread);
#endif
        delete job;
        return status;
    }

    return ProbePending;
}

void cancel(const void* element)
{
    if (g_pendingProbes) {
        for (size_t i = 0; i < g_pendingProbes->size(); ) {
            if ((*g_pendingProbes)[i].element == element)
                g_pendingProbes->erase(g_pendingProbes->begin() + i);
            else
                ++i;
        }
    }

    /* Running jobs are not killed: `element` is cleared so their result is
     * ignored when it arrives (the Job itself must outlive the thread). */
    if (g_jobs) {
        lockJobs();
        for (size_t i = 0; i < g_jobs->size(); ++i) {
            if ((*g_jobs)[i]->element == element)
                (*g_jobs)[i]->element = 0;
        }
        unlockJobs();
    }

    for (size_t i = 0; i < g_results.size(); ) {
        Job* job = g_results[i];

        if (job->element == element) {
            g_results.erase(g_results.begin() + i);
#if PLATFORM(WIN32)
            if (job->thread)
                CloseHandle(job->thread);
#endif
            delete job;
        } else {
            ++i;
        }
    }
}

void requestFrame(const void* element, const String& url, double time)
{
    Job* job;

    if (!ffmpegAvailable())
        return;

    job = new Job();
    job->probe = false;
    job->element = element;
    job->url = url.utf8().data();
    job->time = time;
    job->done = 0;
    job->ok = false;
#if PLATFORM(WIN32)
    job->thread = NULL;
#endif

    startJob(job);
}

bool pollFrame(const void* element, Vector<unsigned char>& out)
{
    for (size_t i = 0; i < g_results.size(); ++i) {
        Job* job = g_results[i];
        bool ok;

        if (job->probe || job->element != element)
            continue;

        g_results.erase(g_results.begin() + i);
        ok = job->ok && !job->bytes.empty();

        if (ok) {
            for (size_t k = 0; k < job->bytes.size(); ++k)
                out.append(job->bytes[k]);
        }

#if PLATFORM(WIN32)
        if (job->thread)
            CloseHandle(job->thread);
#endif
        delete job;
        return ok;
    }

    return false;
}

} // namespace MediaDecoder

} // namespace WebCore
