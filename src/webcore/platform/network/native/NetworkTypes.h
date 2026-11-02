/*
 * NetworkTypes.h - platform independent request/response types for the
 * native network backend (task 3-1).
 *
 * Kept free of WebCore and of any platform header so that it can be unit
 * tested standalone (see host/netdemo.cpp).
 */

#ifndef NETWORK_TYPES_H
#define NETWORK_TYPES_H

#include <string>
#include <vector>
#include <utility>

namespace Net {

typedef std::vector<std::pair<std::string, std::string> > HeaderList;

/* Default network timeout. Overridable via AGB_NET_TIMEOUT_MS; tests use a small
 * value so a dead endpoint fails fast instead of stalling the run. */
inline int netDefaultTimeoutMs()
{
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("AGB_NET_TIMEOUT_MS");
        cached = env ? atoi(env) : 8000;
        if (cached <= 0)
            cached = 8000;
    }
    return cached;
}


struct Request {
    std::string url;
    std::string method;          /* "GET" when empty */
    HeaderList  headers;
    std::string body;
    int         timeoutMs;       /* 0 => netDefaultTimeoutMs() */
    bool        followRedirects;

    Request() : method("GET"), timeoutMs(0), followRedirects(true) {}
};

struct Response {
    int         status;
    std::string statusText;
    HeaderList  headers;

    Response() : status(0) {}

    /* Returns "" when the header is absent (case insensitive). */
    std::string header(const std::string& name) const;
};

struct Error {
    int         code;            /* 0 == success */
    std::string text;

    Error() : code(0) {}
};

} /* namespace Net */

#endif /* NETWORK_TYPES_H */
