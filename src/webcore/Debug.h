#ifndef _DEBUG_H_
#define _DEBUG_H_

#if _DEBUG
#include <stdio.h>

/* notImplemented() call sites are porting survey markers, but the stubs are
 * hit per frame (LocalizedStrings) and per load (loader callbacks), so the
 * trace is off by default: set AGB_NOTIMPLEMENTED=1 to see which callback
 * fired. */
#define notImplemented() { \
    static int agbNotImplementedOn = -1; \
    if (agbNotImplementedOn < 0) agbNotImplementedOn = getenv("AGB_NOTIMPLEMENTED") ? 1 : 0; \
    if (agbNotImplementedOn) fprintf(stderr, "FIXME: %s:%d:%s\n", __FILE__, __LINE__, __FUNCTION__); \
}
#else
#define notImplemented()
#endif

#endif /* _DEBUG_H_ */

