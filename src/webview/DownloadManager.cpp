/*
 * DownloadManager.cpp - see DownloadManager.h and docs/arch-downloads.md.
 */

#include "config.h"
#include "DownloadManager.h"

#include "AgbLog.h"          /* before windows.h: it sets NOMINMAX */
#include "DownloadEvents.h"
#include "HTTPParsers.h"
#include "KURL.h"
#include "PlatformString.h"
#include "ResourceError.h"
#include "ResourceHandle.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "WebCallback.h"
#include "macross.h"

#include <shellapi.h>
#include <string.h>
#include <windows.h>

AgbDownloadEventSink g_agbDownloadEventSink = 0;

namespace WebCore {

using namespace MaCross;

/* ------------------------------------------------------------------ paths */

static bool toWide(const String& text, wchar_t* out, size_t cch)
{
    unsigned len = text.length();

    if (!cch || len + 1 > cch)
        return false;

    const UChar* chars = text.characters();

    for (unsigned i = 0; i < len; ++i)
        out[i] = (wchar_t)chars[i];

    out[len] = 0;
    return true;
}

static bool pathExists(const String& path)
{
    wchar_t wide[1024];

    if (!toWide(path, wide, 1024))
        return false;

    return GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

static void createDirTree(const String& directory)
{
    String path = directory;
    wchar_t wide[1024];

    if (!toWide(path, wide, 1024))
        return;

    /* CreateDirectoryW does not create parents: walk the separators. */
    for (unsigned i = 3; i < path.length(); ++i) {
        if (path[i] != '\\' && path[i] != '/')
            continue;

        String prefix = path.left(i);

        if (toWide(prefix, wide, 1024))
            CreateDirectoryW(wide, 0);
    }

    if (toWide(path, wide, 1024))
        CreateDirectoryW(wide, 0);
}

static FILE* openForWrite(const String& path)
{
    wchar_t wide[1024];

    if (!toWide(path, wide, 1024))
        return 0;

    return _wfopen(wide, L"wb");
}

static void removeFile(const String& path)
{
    wchar_t wide[1024];

    if (toWide(path, wide, 1024))
        _wremove(wide);
}

static bool renameFile(const String& from, const String& to)
{
    wchar_t wideFrom[1024];
    wchar_t wideTo[1024];

    if (!toWide(from, wideFrom, 1024) || !toWide(to, wideTo, 1024))
        return false;

    /* A leftover target (same name chosen twice in the same millisecond) must
     * not make rename fail. */
    _wremove(wideTo);
    return _wrename(wideFrom, wideTo) == 0;
}

/* -------------------------------------------------------------- filenames */

static bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

static String decodePercentEscapes(const char* data, size_t len)
{
    Vector<char> out;

    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '%' && i + 2 < len && isHexDigit(data[i + 1]) && isHexDigit(data[i + 2])) {
            out.append((char)((hexValue(data[i + 1]) << 4) | hexValue(data[i + 2])));
            i += 2;
        } else {
            out.append(data[i]);
        }
    }

    if (out.isEmpty())
        return String();

    return String::fromUTF8(out.data(), out.size());
}

static bool startsWithIgnoreCase(const char* text, size_t len, const char* prefix)
{
    size_t plen = strlen(prefix);

    if (len < plen)
        return false;

    for (size_t i = 0; i < plen; ++i) {
        char a = text[i];
        char b = prefix[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }

    return true;
}

/* filename*=charset'lang'pct-encoded (RFC 5987) wins over filename="...". */
static String filenameFromContentDisposition(const String& header)
{
    if (header.isEmpty())
        return String();

    CString encoded = header.utf8();
    const char* s = encoded.data();
    size_t n = strlen(s);

    String plain;
    String extended;
    size_t i = 0;

    while (i < n) {
        while (i < n && (s[i] == ';' || s[i] == ' ' || s[i] == '\t'))
            ++i;

        size_t nameStart = i;

        while (i < n && s[i] != '=' && s[i] != ';')
            ++i;

        size_t nameLen = i - nameStart;

        if (i >= n || s[i] != '=') {
            while (i < n && s[i] != ';')
                ++i;
            continue;
        }

        ++i;

        while (i < n && (s[i] == ' ' || s[i] == '\t'))
            ++i;

        size_t valueStart;
        size_t valueLen;

        if (i < n && s[i] == '"') {
            ++i;
            valueStart = i;
            while (i < n && s[i] != '"')
                ++i;
            valueLen = i - valueStart;
            if (i < n)
                ++i;
        } else {
            valueStart = i;
            while (i < n && s[i] != ';')
                ++i;
            valueLen = i - valueStart;
            while (valueLen && (s[valueStart + valueLen - 1] == ' ' || s[valueStart + valueLen - 1] == '\t'))
                --valueLen;
        }

        if (nameLen == 9 && startsWithIgnoreCase(s + nameStart, nameLen, "filename*")) {
            const char* v = s + valueStart;
            size_t len = valueLen;
            int quotes = 0;

            /* Skip charset and language: UTF-8''%E6%8A%A5 */
            for (size_t k = 0; k < len; ++k) {
                if (v[k] != '\'')
                    continue;
                if (++quotes == 2) {
                    v += k + 1;
                    len -= k + 1;
                    break;
                }
            }

            if (quotes == 2)
                extended = decodePercentEscapes(v, len);
        } else if (nameLen == 8 && startsWithIgnoreCase(s + nameStart, nameLen, "filename")) {
            plain = decodePercentEscapes(s + valueStart, valueLen);
        }
    }

    if (!extended.isEmpty())
        return extended;

    return plain;
}

static bool isReservedDeviceName(const String& name)
{
    String base = name;
    int dot = base.reverseFind('.');

    if (dot > 0)
        base = base.left(dot);

    unsigned len = base.length();

    if (!len)
        return false;

    /* ASCII upper case, so the comparison is a plain code point compare. */
    char upper[8];

    if (len >= sizeof(upper))
        return false;

    for (unsigned i = 0; i < len; ++i) {
        UChar c = base[i];

        if (c >= 'a' && c <= 'z')
            c = (UChar)(c - 'a' + 'A');
        upper[i] = (char)c;
    }

    upper[len] = 0;

    if (!strcmp(upper, "CON") || !strcmp(upper, "PRN") || !strcmp(upper, "AUX") || !strcmp(upper, "NUL"))
        return true;

    if (len == 4 && (!strncmp(upper, "COM", 3) || !strncmp(upper, "LPT", 3)) && upper[3] >= '1' && upper[3] <= '9')
        return true;

    return false;
}

static String sanitizeFilename(const String& name)
{
    String out;

    for (unsigned i = 0; i < name.length(); ++i) {
        UChar c = name[i];

        if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out.append((UChar)'_');
        else
            out.append(c);
    }

    while (out.length() && (out[out.length() - 1] == ' ' || out[out.length() - 1] == '.'))
        out = out.left(out.length() - 1);

    unsigned lead = 0;

    while (lead < out.length() && out[lead] == ' ')
        ++lead;

    if (lead)
        out = out.substring(lead);

    if (out.isEmpty())
        out = "download";

    if (isReservedDeviceName(out))
        out.append("_");

    if (out.length() > 120) {
        int dot = out.reverseFind('.');
        String extension;

        if (dot > 0 && (unsigned)dot + 1 < out.length()) {
            extension = out.substring((unsigned)dot);

            if (extension.length() > 16)
                extension = String();
        }

        if (extension.isEmpty()) {
            out = out.left(120);
        } else {
            String trimmed = out.left(120 - extension.length());
            trimmed.append(extension);
            out = trimmed;
        }
    }

    return out;
}

static String filenameFor(const ResourceResponse& response, const KURL& url)
{
    String name = filenameFromContentDisposition(response.httpHeaderField("Content-Disposition"));

    if (name.isEmpty()) {
        DeprecatedString last = url.lastPathComponent();

        if (!last.isEmpty()) {
            /* ascii() is the 8-bit representation: percent escapes and raw
             * UTF-8 bytes both survive, and decodePercentEscapes() feeds the
             * result through fromUTF8(). */
            const char* bytes = last.ascii();

            name = decodePercentEscapes(bytes, strlen(bytes));
        }
    }

    name = sanitizeFilename(name);

    if (name.find('.') < 0)
        name.append(".bin");

    return name;
}

static String uniquePath(const String& directory, const String& name)
{
    String base = name;
    String extension;
    int dot = name.reverseFind('.');

    if (dot > 0) {
        base = name.left((unsigned)dot);
        extension = name.substring((unsigned)dot);
    }

    for (int n = 0; n < 1000; ++n) {
        String candidate = base;

        if (n) {
            candidate.append(" (");
            candidate.append(String::number(n));
            candidate.append(")");
        }

        candidate.append(extension);

        String full = directory;
        full.append("\\");
        full.append(candidate);

        if (!pathExists(full))
            return full;
    }

    return String();
}

/* ------------------------------------------------------------ DownloadJob */

DownloadJob::DownloadJob(int id)
    : m_id(id)
    , m_state(StateActive)
    , m_cancelling(false)
    , m_file(0)
    , m_received(0)
    , m_total(0)
    , m_lastEmitBytes(0)
    , m_lastEmitTick(0)
{
}

DownloadJob::~DownloadJob()
{
    closeFile(false);
}

bool DownloadJob::start(PassRefPtr<ResourceHandle> handle, const ResourceRequest& request, const ResourceResponse& response)
{
    m_handle = handle;
    m_url = String(response.url().url());
    m_mime = response.mimeType();
    m_total = response.expectedContentLength();

    if (m_url.isEmpty())
        m_url = String(request.url().url());

    String directory = DownloadManager::shared().directory();

    createDirTree(directory);

    m_path = uniquePath(directory, filenameFor(response, response.url()));

    if (m_path.isEmpty()) {
        agbLog(AGB_LOG_DL, "cannot pick a file name for url=%s", m_url.utf8().data());
        fail(ERR_NET_FILE_READ_ERROR);
        return false;
    }

    m_partPath = m_path;
    m_partPath.append(".part");
    m_file = openForWrite(m_partPath);

    if (!m_file) {
        agbLog(AGB_LOG_DL, "cannot create file path=%s", m_path.utf8().data());
        fail(ERR_NET_FILE_READ_ERROR);
        return false;
    }

    agbLog(AGB_LOG_DL, "start id=%d mime=%s total=%lld path=%s url=%s", m_id,
           m_mime.utf8().data(), m_total, m_path.utf8().data(), m_url.utf8().data());

    if (!emit(MC_DL_STARTED, 0)) {
        cancel();
        return false;
    }

    if (m_handle)
        m_handle->setClient(this);

    return true;
}

int DownloadJob::mcState() const
{
    switch (m_state) {
    case StateFinished:  return MC_DL_FINISHED;
    case StateFailed:    return MC_DL_FAILED;
    case StateCancelled: return MC_DL_CANCELLED;
    default:             break;
    }

    return m_received ? MC_DL_PROGRESS : MC_DL_STARTED;
}

void DownloadJob::closeFile(bool keep)
{
    if (m_file) {
        fclose(m_file);
        m_file = 0;
    }

    if (!keep)
        removeFile(m_partPath);
}

void DownloadJob::fail(int errorCode)
{
    if (m_state != StateActive)
        return;

    closeFile(false);
    m_state = StateFailed;
    m_handle = 0;

    agbLog(AGB_LOG_DL, "fail id=%d code=%d url=%s", m_id, errorCode, m_url.utf8().data());
    emit(MC_DL_FAILED, errorCode);
}

void DownloadJob::cancel()
{
    if (m_state != StateActive || m_cancelling)
        return;

    m_cancelling = true;

    RefPtr<DownloadJob> protect(this);

    if (m_handle) {
        m_handle->setClient(0);
        m_handle->cancel();
        m_handle = 0;
    }

    closeFile(false);
    m_state = StateCancelled;

    agbLog(AGB_LOG_DL, "cancel id=%d received=%lld url=%s", m_id, m_received, m_url.utf8().data());
    emit(MC_DL_CANCELLED, 0);

    m_cancelling = false;
}

bool DownloadJob::emit(int state, int error)
{
    bool keepGoing = true;

    if (cb_download) {
        CString url = m_url.utf8();
        CString path = m_path.utf8();
        CString mime = m_mime.utf8();

        MC_DOWNLOAD_INFO info;
        memset(&info, 0, sizeof(info));
        info.id = m_id;
        info.state = state;
        info.error = error;
        info.utf8_url = url.data();
        info.utf8_path = path.data();
        info.utf8_mime = mime.data();
        info.received = (unsigned long long)(m_received > 0 ? m_received : 0);
        info.total = (unsigned long long)(m_total > 0 ? m_total : 0);

        keepGoing = cb_download(m_id, &info) != 0;
    }

    /* The internal page redraws from this; it is independent of the host
     * callback (a probe sets no cb_download but still wants the event). */
    if (g_agbDownloadEventSink)
        g_agbDownloadEventSink(m_id, state);

    return keepGoing;
}

void DownloadJob::didReceiveResponse(ResourceHandle*, const ResourceResponse& response)
{
    /* The response that triggered the download was already reported; only a
     * redirect lands here, and the URL is what the host sees. */
    if (!response.url().isEmpty())
        m_url = String(response.url().url());
}

void DownloadJob::didReceiveData(ResourceHandle*, const char* data, int length, int)
{
    if (m_state != StateActive)
        return;

    if (m_file && length > 0 && fwrite(data, 1, (size_t)length, m_file) != (size_t)length) {
        fail(ERR_NET_FILE_READ_ERROR);
        return;
    }

    m_received += length;

    unsigned long now = GetTickCount();
    bool due = (m_received - m_lastEmitBytes >= 64 * 1024) || (now - m_lastEmitTick >= 200);

    if (!due)
        return;

    m_lastEmitBytes = m_received;
    m_lastEmitTick = now;

    if (!emit(MC_DL_PROGRESS, 0))
        cancel();
}

void DownloadJob::didFinishLoading(ResourceHandle*)
{
    if (m_state != StateActive)
        return;

    if (m_file) {
        fclose(m_file);
        m_file = 0;
    }

    if (!renameFile(m_partPath, m_path)) {
        removeFile(m_partPath);
        fail(ERR_NET_FILE_READ_ERROR);
        return;
    }

    m_state = StateFinished;
    m_handle = 0;

    agbLog(AGB_LOG_DL, "finish id=%d bytes=%lld path=%s", m_id, m_received, m_path.utf8().data());
    emit(MC_DL_FINISHED, 0);
}

void DownloadJob::didFail(ResourceHandle*, const ResourceError& error)
{
    if (m_state != StateActive || m_cancelling)
        return;

    agbLog(AGB_LOG_DL, "error id=%d code=%d url=%s", m_id, error.errorCode(), m_url.utf8().data());
    fail(error.errorCode());
}

/* -------------------------------------------------------- DownloadManager */

static String defaultDownloadDirectory()
{
    const char* userdata = getenv("AGB_USERDATA");
    String directory;

    if (userdata && userdata[0]) {
        directory = String::fromUTF8(userdata);
    } else {
        const char* local = getenv("LOCALAPPDATA");

        directory = String::fromUTF8(local && local[0] ? local : ".");
        directory.append("\\Agave");
    }

    directory.append("\\downloads");
    return directory;
}

static String normalizeDirectory(const String& input)
{
    String out;

    for (unsigned i = 0; i < input.length(); ++i) {
        UChar c = input[i];

        out.append(c == '/' ? (UChar)'\\' : c);
    }

    while (out.length() && out[out.length() - 1] == '\\')
        out = out.left(out.length() - 1);

    return out;
}

DownloadManager::DownloadManager()
    : m_directory(normalizeDirectory(defaultDownloadDirectory()))
    , m_nextId(0)
{
}

DownloadManager& DownloadManager::shared()
{
    static DownloadManager manager;

    return manager;
}

void DownloadManager::setDirectory(const String& directory)
{
    if (directory.isEmpty())
        return;

    m_directory = normalizeDirectory(directory);
    agbLog(AGB_LOG_DL, "directory %s", m_directory.utf8().data());
}

int DownloadManager::start(PassRefPtr<ResourceHandle> handle, const ResourceRequest& request, const ResourceResponse& response)
{
    purgeFinished();

    RefPtr<DownloadJob> job = new DownloadJob(++m_nextId);

    job->start(handle, request, response);

    /* Kept even when the start failed: the host already got MC_DL_FAILED and
     * needs the entry to stay visible. */
    m_jobs.append(job);

    return job->id();
}

bool DownloadManager::cancel(int id)
{
    for (unsigned i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i]->id() != id)
            continue;

        m_jobs[i]->cancel();
        return true;
    }

    return false;
}

int DownloadManager::cancelAll()
{
    int cancelled = 0;

    for (unsigned i = 0; i < m_jobs.size(); ++i) {
        if (!m_jobs[i]->isActive())
            continue;

        m_jobs[i]->cancel();
        ++cancelled;
    }

    return cancelled;
}

int DownloadManager::activeCount() const
{
    int active = 0;

    for (unsigned i = 0; i < m_jobs.size(); ++i)
        if (m_jobs[i]->isActive())
            ++active;

    return active;
}

String DownloadManager::pathFor(int id) const
{
    for (unsigned i = 0; i < m_jobs.size(); ++i)
        if (m_jobs[i]->id() == id)
            return m_jobs[i]->path();

    return String();
}

DownloadJob* DownloadManager::jobAt(int index) const
{
    if (index < 0 || (unsigned)index >= m_jobs.size())
        return 0;

    return m_jobs[index].get();
}

bool DownloadManager::openFile(int id)
{
    String path = pathFor(id);
    wchar_t wide[1024];

    if (path.isEmpty() || !toWide(path, wide, 1024))
        return false;

    /* Runs on the engine thread on purpose: ShellExecuteW is an out-of-process
     * shell call and does not need the UI thread's message pump. */
    HINSTANCE result = ShellExecuteW(0, L"open", wide, 0, 0, SW_SHOWNORMAL);

    agbLog(AGB_LOG_DL, "open id=%d path=%s ok=%d", id, path.utf8().data(),
           (INT_PTR)result > 32);

    return (INT_PTR)result > 32;
}

void DownloadManager::purgeFinished()
{
    Vector<RefPtr<DownloadJob> > keep;

    for (unsigned i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i]->isActive())
            keep.append(m_jobs[i]);
    }

    m_jobs = keep;
}

} /* namespace WebCore */
