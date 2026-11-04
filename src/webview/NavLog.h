/*
 * NavLog.h - navigation actions log ([nav]).
 *
 * Every navigation the browser decides on prints one line: host loads
 * (address bar, tab, probe), link clicks, location changes, form submits,
 * history back/forward/reload/stop, redirects, popups, downloads and load
 * failures. It is on by default so a run explains where the page went;
 * AGB_NAV_LOG=0 silences the channel.
 *
 * Header-only and C-compatible on purpose: libagave (C++) and the webview
 * control (C, separate DLL) both call it without an export hop.
 */

#ifndef NavLog_h
#define NavLog_h

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static inline int agbNavLogEnabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char* value = getenv("AGB_NAV_LOG");

        enabled = (value && value[0] == '0' && value[1] == 0) ? 0 : 1;
    }

    return enabled != 0;
}

/* Callers pass the rest of the line; the prefix and the newline are added
 * here so every [nav] line has the same shape. */
static inline void agbNavLog(const char* format, ...)
{
    va_list args;

    if (!agbNavLogEnabled())
        return;

    fputs("[nav] ", stderr);

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fputc('\n', stderr);
}

#endif /* NavLog_h */
