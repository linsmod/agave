/*
 * PageScheme.h - content providers for URL schemes the engine serves itself.
 *
 * The browser's own pages (agave://downloads and friends) have no network
 * backend: their bytes come from the engine. Before this existed, WebView::loadUrl
 * special-cased the scheme by building a synthetic document itself, which meant
 * only the paths that went through loadUrl worked - a reload re-requests the
 * current URL through the loader and fell into the network stack (R162).
 *
 * A provider puts the content where every load goes through: MainResourceLoader
 * asks the registry for bytes and, when it gets them, serves the request as
 * substitute data (the same mechanism internal pages already used).
 */

#ifndef PageScheme_h
#define PageScheme_h

#include "PlatformString.h"
#include <wtf/PassRefPtr.h>

namespace WebCore {

class KURL;
class SharedBuffer;
class SubstituteData;

/* Returns the page's bytes plus its MIME type, or null to fall through to the
 * network. Called on the engine thread, once per load. */
typedef PassRefPtr<SharedBuffer> (*PageSchemeFactory)(const KURL& url, String& mimeType);

/* Registers (or replaces) the provider for `scheme` (no "://" suffix). */
void registerPageScheme(const char* scheme, PageSchemeFactory factory);

/* True when a provider owns this scheme. */
bool hasPageScheme(const String& scheme);

/* Substitute data for a request the engine serves itself. Returns false when
 * no provider owns the scheme or the provider produced no content. */
bool pageSchemeSubstituteData(const KURL& url, SubstituteData& out);

} /* namespace WebCore */

#endif /* PageScheme_h */
