#ifndef _RESOURCEHANDLEINTERNAL_H_
#define _RESOURCEHANDLEINTERNAL_H_

/* The loader state type is provided by the selected network backend. */
#if defined(OPT_NET_BACKEND_NATIVE)
#include "ResourceHandleInternalNet.h"
#else
#include "ResourceHandleInternalCurl.h"
#endif

#endif
