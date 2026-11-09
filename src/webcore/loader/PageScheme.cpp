#include "config.h"

#include "PageScheme.h"

#include "KURL.h"
#include "SharedBuffer.h"
#include "SubstituteData.h"
#include <wtf/Vector.h>

namespace WebCore {

namespace {

struct PageSchemeEntry {
    String scheme;
    PageSchemeFactory factory;
};

/* Eight is plenty: internal pages are a handful of schemes, and a fixed table
 * keeps the lookup allocation-free on the load path. */
const int kMaxPageSchemes = 8;

Vector<PageSchemeEntry>& pageSchemes()
{
    static Vector<PageSchemeEntry>* schemes = new Vector<PageSchemeEntry>;

    return *schemes;
}

} /* namespace */

void registerPageScheme(const char* scheme, PageSchemeFactory factory)
{
    Vector<PageSchemeEntry>& schemes = pageSchemes();
    size_t i;

    if (!scheme || !scheme[0] || !factory)
        return;

    for (i = 0; i < schemes.size(); ++i) {
        if (equalIgnoringCase(schemes[i].scheme, String(scheme))) {
            schemes[i].factory = factory;
            return;
        }
    }

    if (schemes.size() >= kMaxPageSchemes)
        return;

    PageSchemeEntry entry;

    entry.scheme = String(scheme);
    entry.factory = factory;
    schemes.append(entry);
}

bool hasPageScheme(const String& scheme)
{
    Vector<PageSchemeEntry>& schemes = pageSchemes();
    size_t i;

    for (i = 0; i < schemes.size(); ++i) {
        if (equalIgnoringCase(schemes[i].scheme, scheme))
            return true;
    }

    return false;
}

bool pageSchemeSubstituteData(const KURL& url, SubstituteData& out)
{
    Vector<PageSchemeEntry>& schemes = pageSchemes();
    String protocol = url.protocol();
    size_t i;

    for (i = 0; i < schemes.size(); ++i) {
        if (!equalIgnoringCase(schemes[i].scheme, protocol))
            continue;

        {
            String mimeType;
            RefPtr<SharedBuffer> data = schemes[i].factory(url, mimeType);

            if (!data || !data->size())
                return false;

            out = SubstituteData(data.release(), mimeType, String("utf-8"),
                                 KURL(), url);
            return true;
        }
    }

    return false;
}

} /* namespace WebCore */
