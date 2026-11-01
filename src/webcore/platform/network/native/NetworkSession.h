/*
 * NetworkSession.h - the portable face of the native network backend (task 3-1).
 *
 * Threading contract:
 *   - start()/cancel()/pump()/shutdown() are called on the main (UI/engine) thread;
 *   - the implementation does the I/O on its own worker thread(s);
 *   - Client callbacks are ONLY invoked from pump(), i.e. on the main thread.
 *
 * That keeps the JS engine single threaded while still doing async I/O.
 */

#ifndef NETWORK_SESSION_H
#define NETWORK_SESSION_H

#include "NetworkTypes.h"

namespace Net {

class Client {
public:
    virtual ~Client() {}

    virtual void onResponse(const Response& response) = 0;
    virtual void onData(const char* data, unsigned int length) = 0;
    virtual void onComplete() = 0;
    virtual void onError(const Error& error) = 0;
};

class Session {
public:
    /* Platform factory: WinHTTP on Windows, sockets+OpenSSL on POSIX. */
    static Session* create();

    virtual ~Session() {}

    /* Returns a request id (> 0), or 0 when the request could not be started. */
    virtual long long start(const Request& request, Client* client) = 0;

    /* Cancels a request; the client receives no further callbacks for it. */
    virtual void cancel(long long id) = 0;

    /* Delivers queued events to the clients. Call from the main thread loop. */
    virtual void pump() = 0;

    /* Cancels everything and joins the worker threads. */
    virtual void shutdown() = 0;

    /* Number of requests not yet finished (for tests and the loader). */
    virtual int pending() const = 0;
};

} /* namespace Net */

#endif /* NETWORK_SESSION_H */
