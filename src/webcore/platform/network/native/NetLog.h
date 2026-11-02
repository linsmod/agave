/*
 * NetLog.h - timestamped network / page lifecycle log.
 *
 * Enabled by either AGB_NET_LOG=1 (or a file path) or AGB_NET_DEBUG=1.
 * Every line is prefixed with the time since the first log call, so a run can
 * be read as a timeline:
 *
 *   [netlog]   +0.0ms  net.begin  GET https://example.com/
 *   [netlog]  +81.4ms  net.response status=200 https://example.com/
 *   [netlog] +123.9ms  net.done   dur=123.9ms bytes=1256 https://example.com/
 */

#ifndef NetLog_h
#define NetLog_h

namespace Net {

/* True when logging is enabled (reads the environment once). */
bool logEnabled();

/* Milliseconds since the first log call. */
double logElapsedMs();

/* Writes one timestamped line: "[netlog] +123.4ms  <text>" */
void logLine(const char* format, ...);

/* Convenience for page lifecycle events: "[netlog] +12.3ms  page.<event> ..." */
void logEvent(const char* category, const char* format, ...);

/* Marks t0 = now (called once by logLine). */
void logReset();

} /* namespace Net */

#endif /* NetLog_h */
