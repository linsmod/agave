/*
 * WinHttpSession.cpp - Windows backend for the native network layer (task 3-1).
 *
 * Uses WinHTTP so that TLS (SChannel), the system certificate store, the system
 * proxy configuration and DNS all come from the OS instead of the ancient
 * bundled PolarSSL. One worker thread per request; results are queued and
 * delivered on the main thread from pump().
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "../NetworkSession.h"
#include "../NetworkTypes.h"

namespace Net {

namespace {

std::wstring widen(const std::string& s)
{
    std::wstring out;
    if (s.empty())
        return out;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (n <= 0)
        return out;
    out.resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string narrow(const wchar_t* s, int len)
{
    std::string out;
    if (!s || len <= 0)
        return out;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
    if (n <= 0)
        return out;
    out.resize((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, s, len, &out[0], n, NULL, NULL);
    return out;
}

std::string lastErrorText(DWORD code)
{
    char buf[256];
    buf[0] = '\0';
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, buf, sizeof(buf) - 1, NULL);
    std::string s(buf);
    while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r'))
        s.erase(s.size() - 1);
    return s;
}

/* Splits a raw "Name: value\r\n" block into the header list. */
void parseHeaders(const std::string& raw, HeaderList& out)
{
    size_t pos = 0;

    while (pos < raw.size()) {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos)
            eol = raw.size();

        std::string line = raw.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos && colon > 0) {
            std::string key = line.substr(0, colon);
            size_t vstart = colon + 1;
            while (vstart < line.size() && line[vstart] == ' ')
                ++vstart;
            out.push_back(std::make_pair(key, line.substr(vstart)));
        }
        pos = eol + 2;
    }
}

struct Event {
    enum Kind { KindResponse, KindData, KindComplete, KindError };

    Kind        kind;
    long long   id;
    Response    response;
    std::string data;
    Error       error;

    Event() : kind(KindComplete), id(0) {}
};

class WinHttpSession;

struct Job {
    long long        id;
    Request          request;
    Client*          client;
    HANDLE           thread;
    WinHttpSession*  owner;
    volatile LONG    cancelled;

    Job() : id(0), client(NULL), thread(NULL), owner(NULL), cancelled(0) {}
};

class WinHttpSession : public Session {
public:
    WinHttpSession();
    virtual ~WinHttpSession();

    virtual long long start(const Request& request, Client* client);
    virtual void cancel(long long id);
    virtual void pump();
    virtual void shutdown();
    virtual int pending() const;

    void post(Event* ev);
    void run(Job* job);
    void finish(Job* job);

private:
    static DWORD WINAPI threadEntry(LPVOID param);

    CRITICAL_SECTION      m_lock;
    std::vector<Job*>     m_jobs;
    std::vector<Event*>   m_events;
    long long             m_nextId;
    int                   m_pending;
};

WinHttpSession::WinHttpSession()
    : m_nextId(0), m_pending(0)
{
    InitializeCriticalSection(&m_lock);
}

WinHttpSession::~WinHttpSession()
{
    shutdown();
    DeleteCriticalSection(&m_lock);
}

long long WinHttpSession::start(const Request& request, Client* client)
{
    Job* job;

    if (request.url.empty() || !client)
        return 0;

    job = new Job();
    job->id      = ++m_nextId;
    job->request = request;
    job->client  = client;
    job->owner   = this;

    EnterCriticalSection(&m_lock);
    m_jobs.push_back(job);
    m_pending++;
    LeaveCriticalSection(&m_lock);

    job->thread = CreateThread(NULL, 0, &WinHttpSession::threadEntry, job, 0, NULL);
    if (!job->thread) {
        finish(job);
        return 0;
    }

    return job->id;
}

void WinHttpSession::cancel(long long id)
{
    EnterCriticalSection(&m_lock);
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i]->id == id)
            InterlockedExchange(&m_jobs[i]->cancelled, 1);
    }
    LeaveCriticalSection(&m_lock);
}

void WinHttpSession::post(Event* ev)
{
    EnterCriticalSection(&m_lock);
    m_events.push_back(ev);
    LeaveCriticalSection(&m_lock);
}

void WinHttpSession::pump()
{
    std::vector<Event*> events;

    EnterCriticalSection(&m_lock);
    events.swap(m_events);
    LeaveCriticalSection(&m_lock);

    for (size_t i = 0; i < events.size(); ++i) {
        Event*  ev = events[i];
        Client* client = NULL;
        Job*    job = NULL;

        EnterCriticalSection(&m_lock);
        for (size_t j = 0; j < m_jobs.size(); ++j) {
            if (m_jobs[j]->id == ev->id) {
                job    = m_jobs[j];
                client = job->client;
                break;
            }
        }
        LeaveCriticalSection(&m_lock);

        if (client) {
            switch (ev->kind) {
            case Event::KindResponse: client->onResponse(ev->response); break;
            case Event::KindData:     client->onData(ev->data.data(), (unsigned int)ev->data.size()); break;
            case Event::KindComplete: client->onComplete(); break;
            case Event::KindError:    client->onError(ev->error); break;
            }
        }

        if (job && (ev->kind == Event::KindComplete || ev->kind == Event::KindError))
            finish(job);

        delete ev;
    }
}

void WinHttpSession::finish(Job* job)
{
    if (!job)
        return;

    if (job->thread) {
        WaitForSingleObject(job->thread, INFINITE);
        CloseHandle(job->thread);
        job->thread = NULL;
    }

    EnterCriticalSection(&m_lock);
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i] == job) {
            m_jobs.erase(m_jobs.begin() + (long)i);
            break;
        }
    }
    if (m_pending > 0)
        m_pending--;
    LeaveCriticalSection(&m_lock);

    delete job;
}

void WinHttpSession::shutdown()
{
    std::vector<Job*> jobs;

    EnterCriticalSection(&m_lock);
    jobs = m_jobs;
    for (size_t i = 0; i < jobs.size(); ++i)
        InterlockedExchange(&jobs[i]->cancelled, 1);
    LeaveCriticalSection(&m_lock);

    for (size_t i = 0; i < jobs.size(); ++i)
        finish(jobs[i]);

    EnterCriticalSection(&m_lock);
    for (size_t i = 0; i < m_events.size(); ++i)
        delete m_events[i];
    m_events.clear();
    m_jobs.clear();
    m_pending = 0;
    LeaveCriticalSection(&m_lock);
}

int WinHttpSession::pending() const
{
    return m_pending;
}

DWORD WINAPI WinHttpSession::threadEntry(LPVOID param)
{
    Job* job = (Job*)param;
    job->owner->run(job);
    return 0;
}

void WinHttpSession::run(Job* job)
{
    std::wstring wurl = widen(job->request.url);
    URL_COMPONENTS uc;
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    int timeout = job->request.timeoutMs > 0 ? job->request.timeoutMs : 30000;
    std::wstring host;
    std::wstring path;
    bool secure = false;
    bool ok = false;
    DWORD err = 0;

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        Event* ev = new Event();
        ev->kind = Event::KindError;
        ev->id = job->id;
        ev->error.code = (int)GetLastError();
        ev->error.text = "invalid URL: " + job->request.url;
        post(ev);
        return;
    }

    host.assign(uc.lpszHostName, uc.dwHostNameLength);
    path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty())
        path = L"/";
    secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    session = WinHttpOpen(L"agave/1.0 (native)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { err = GetLastError(); goto cleanup; }

    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    connect = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    if (!connect) { err = GetLastError(); goto cleanup; }

    {
        std::wstring verb = widen(job->request.method.empty() ? "GET" : job->request.method);
        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;

        request = WinHttpOpenRequest(connect, verb.c_str(), path.c_str(), NULL,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) { err = GetLastError(); goto cleanup; }

        /* Servers (baidu CDNs among them) answer with gzip; let WinHTTP
         * decompress transparently, otherwise the script arrives as binary. */
        {
            DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION,
                             &decompression, sizeof(decompression));
        }
    }

    {
        std::wstring headers;
        for (size_t i = 0; i < job->request.headers.size(); ++i) {
            headers += widen(job->request.headers[i].first);
            headers += L": ";
            headers += widen(job->request.headers[i].second);
            headers += L"\r\n";
        }

        const void* bodyPtr = job->request.body.empty() ? NULL : job->request.body.data();
        DWORD bodyLen = (DWORD)job->request.body.size();
        LPCWSTR hdr = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
        DWORD hdrLen = headers.empty() ? 0 : (DWORD)headers.size();

        if (!WinHttpSendRequest(request, hdr, hdrLen, (LPVOID)bodyPtr, bodyLen, bodyLen, 0)) {
            err = GetLastError();
            goto cleanup;
        }
    }

    if (!WinHttpReceiveResponse(request, NULL)) {
        err = GetLastError();
        goto cleanup;
    }

    {
        Event* ev = new Event();
        DWORD  statusCode = 0;
        DWORD  size = sizeof(statusCode);

        ev->kind = Event::KindResponse;
        ev->id = job->id;

        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size,
                                WINHTTP_NO_HEADER_INDEX)) {
            ev->response.status = (int)statusCode;
        }

        {
            DWORD rawLen = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX, NULL, &rawLen,
                                WINHTTP_NO_HEADER_INDEX);
            if (rawLen > 0) {
                std::vector<wchar_t> raw(rawLen / sizeof(wchar_t) + 1, 0);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &rawLen,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    parseHeaders(narrow(&raw[0], (int)(rawLen / sizeof(wchar_t))),
                                 ev->response.headers);
                }
            }
        }

        if (getenv("AGB_NET_DEBUG")) {
            fprintf(stderr, "[net] %s status=%d headers:", job->request.url.c_str(), (int)statusCode);
            for (size_t hi = 0; hi < ev->response.headers.size(); ++hi)
                fprintf(stderr, " %s=%s;", ev->response.headers[hi].first.c_str(),
                        ev->response.headers[hi].second.c_str());
            fprintf(stderr, "\n");
        }

        post(ev);
    }

    for (;;) {
        char buf[16384];
        DWORD read = 0;

        if (InterlockedCompareExchange(&job->cancelled, 0, 0) != 0) {
            err = ERROR_CANCELLED;
            goto cleanup;
        }

        if (!WinHttpReadData(request, buf, sizeof(buf), &read)) {
            err = GetLastError();
            goto cleanup;
        }
        if (read == 0)
            break;

        {
            Event* ev = new Event();
            ev->kind = Event::KindData;
            ev->id = job->id;
            ev->data.assign(buf, read);
            post(ev);
        }
    }

    ok = true;

cleanup:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);

    if (ok) {
        Event* ev = new Event();
        ev->kind = Event::KindComplete;
        ev->id = job->id;
        post(ev);
    } else if (InterlockedCompareExchange(&job->cancelled, 0, 0) == 0) {
        Event* ev = new Event();
        ev->kind = Event::KindError;
        ev->id = job->id;
        ev->error.code = (int)err;
        ev->error.text = lastErrorText(err);
        post(ev);
    }
}

} /* namespace */

Session* Session::create()
{
    return new WinHttpSession();
}

} /* namespace Net */
