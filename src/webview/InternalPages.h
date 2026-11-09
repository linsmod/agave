/*
 * InternalPages.h - the browser's own pages, served as a URL scheme provider.
 *
 * "agave://downloads" resolves to downloads.html next to the executable. The
 * provider is registered with the loader (WebCore::registerPageScheme), so
 * navigate, reload, iframe and window.open all get the page from the engine -
 * there is no network backend behind the scheme and no per-caller special case
 * (R162).
 */

#ifndef InternalPages_h
#define InternalPages_h

#include <wtf/PassRefPtr.h>

namespace WebCore {
class KURL;
class SharedBuffer;
class String;
}

/* Registers every internal scheme. Called once from macross_initialize, so both
 * the shell and a renderer process serve their own pages. */
void agaveRegisterInternalPages(void);

/* The provider itself (exposed for tests/diagnostics). */
PassRefPtr<WebCore::SharedBuffer> agaveInternalPage(const WebCore::KURL& url,
                                                    WebCore::String& mimeType);

#endif /* InternalPages_h */
