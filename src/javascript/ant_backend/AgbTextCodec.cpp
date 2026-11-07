/*
 * AgbTextCodec.cpp - host-supplied legacy text codecs for ant's TextDecoder.
 *
 * ant has no gbk/big5/shift_jis tables and should not grow any: this browser
 * already ships them (src/webcore/platform/unicode/gbk_map.cpp, shiftjis_map.cpp,
 * big5_map.cpp, enabled by ENABLE_GBK_SUPPORT / ENABLE_SHIFTJIS_SUPPORT ... ON
 * in CMakeLists.txt). ant exposes a provider hook (antb_set_text_codec_provider)
 * and asks it for any label its own table does not know, so the tables exist
 * exactly once in the process.
 *
 * Without this, `new TextDecoder('gbk')` throws RangeError in ant and every GBK
 * endpoint decodes as replacement characters - common on older Chinese APIs.
 */

#include "config.h"

#include "AgbTextCodec.h"

#include <string.h>

#include "AntBridge.h"
#include "CString.h"
#include "TextEncoding.h"
#include "TextEncodingRegistry.h"
#include "PlatformString.h"

namespace {

/* Labels ant must never answer from here: its own table is authoritative for
 * these and a host codec would change established behaviour. Kept explicit so a
 * future registry alias cannot silently take them over. */
bool isAntBuiltinLabel(const char* label, size_t len)
{
    static const char* const builtins[] = {
        "utf-8", "utf8", "unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8",
        "x-unicode20utf8", "utf-16", "utf-16le", "utf-16be", "unicode",
        "unicodefeff", "unicodefffe", "ucs-2", "csunicode", "iso-10646-ucs-2",
        "windows-1252", "iso-8859-1", "latin1", "l1", "ascii", "us-ascii",
        "iso-8859-2",
        NULL
    };

    for (int i = 0; builtins[i]; ++i) {
        const char* b = builtins[i];
        size_t bl = strlen(b);

        if (bl == len && strncasecmp(label, b, len) == 0)
            return true;
    }

    return false;
}

/* Labels WebCore's registry does not know but whose bytes it can decode.
 * GB18030 is a superset of GBK for the two-byte range that legacy pages
 * actually use (the four-byte plane is rare and still unsupported); answering
 * from the GBK table beats rejecting the label, which is what made
 * `new TextDecoder('gb18030')` throw. */
struct AliasEntry {
    const char* alias;
    const char* encoding;
};

const AliasEntry kAliases[] = {
    { "gb18030",   "GBK" },
    { "gb-18030",  "GBK" },
    { "cp936",     "GBK" },
    { "ms936",     "GBK" },
    { "windows-936", "GBK" },
    { "cp932",     "SHIFT-JIS" },
    { "ms932",     "SHIFT-JIS" },
    { "windows-31j", "SHIFT-JIS" },
    { "ms_kanji",  "SHIFT-JIS" },
    { "x-sjis",    "SHIFT-JIS" },
    { "euc-jp",    "EUC-JP" },
    { "cp949",     "EUC-KR" },
    { "ms949",     "EUC-KR" },
    { "uhc",       "EUC-KR" },
    { "windows-949", "EUC-KR" },
    { "cn-big5",   "BIG5" },
    { "x-x-big5",  "BIG5" },
};

/* Looks the label up in the alias table above. Returns the WebCore encoding name
 * or NULL. */
const char* aliasEncoding(const char* alias)
{
    for (size_t i = 0; i < sizeof(kAliases) / sizeof(kAliases[0]); ++i) {
        if (!strcasecmp(alias, kAliases[i].alias))
            return kAliases[i].encoding;
    }

    return NULL;
}

/* Copies WebCore's canonical name (e.g. "GBK") downcased into the caller's
 * buffer. Returns false when the label is unknown or not worth handing over. */
bool canonicalLabel(const char* label, size_t len, char* out, size_t cap)
{
    if (!label || !len || !out || cap < 2)
        return false;

    if (isAntBuiltinLabel(label, len))
        return false;

    /* The registry wants a NUL-terminated alias. */
    char alias[64];

    if (len >= sizeof(alias))
        return false;

    memcpy(alias, label, len);
    alias[len] = 0;

    const char* canonical = aliasEncoding(alias);

    if (!canonical)
        canonical = WebCore::atomicCanonicalTextEncodingName(alias);

    if (!canonical || !canonical[0])
        return false;

    /* WebCore reports UTF-8/latin1 for a handful of aliases; those stay with
     * ant so both sides agree on one implementation. */
    if (!strcasecmp(canonical, "UTF-8") || !strcasecmp(canonical, "ISO-8859-1")
        || !strcasecmp(canonical, "windows-1252") || !strcasecmp(canonical, "US-ASCII"))
        return false;

    size_t n = strlen(canonical);

    if (n >= cap)
        n = cap - 1;

    for (size_t i = 0; i < n; ++i) {
        char c = canonical[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }

    out[n] = 0;
    return true;
}

int resolveCallback(void* user, const char* label, size_t len, char* out, size_t cap)
{
    (void)user;
    return canonicalLabel(label, len, out, cap) ? 1 : 0;
}

long decodeCallback(void* user, const char* label,
                    const unsigned char* in, size_t in_len,
                    char* out, size_t out_cap, int fatal)
{
    (void)user;

    if (!label || !label[0] || (!in && in_len))
        return -1;

    if (in_len == 0) {
        if (out_cap) out[0] = 0;
        return 0;
    }

    /* label arrives lowercased (that is what resolve() returned, and what
     * TextDecoder.prototype.encoding reports). The registry is case-insensitive,
     * but the alias table above is not, so map back first. */
    const char* encodingName = aliasEncoding(label);

    if (!encodingName) {
        const char* canonical = WebCore::atomicCanonicalTextEncodingName(label);
        encodingName = canonical && canonical[0] ? canonical : label;
    }

    WebCore::TextEncoding encoding(encodingName);

    if (!encoding.isValid())
        return -1;

    /* fatal is honoured by the codec itself: WebCore substitutes U+FFFD for
     * invalid sequences, which is what a non-fatal TextDecoder must do too, so
     * there is nothing to special-case here. */
    (void)fatal;

    WebCore::String decoded = encoding.decode(reinterpret_cast<const char*>(in), in_len);
    WebCore::CString utf8 = decoded.utf8();

    if (!utf8.data())
        return -1;

    size_t n = utf8.length();

    if (n + 1 > out_cap)
        n = out_cap > 0 ? out_cap - 1 : 0;

    if (n)
        memcpy(out, utf8.data(), n);

    if (out_cap)
        out[n] = 0;

    return (long)n;
}

AntbTextCodecProvider g_provider = {
    resolveCallback,
    decodeCallback,
    NULL
};

} // namespace

namespace Agb {

void installTextCodecProvider()
{
    antb_set_text_codec_provider(&g_provider);
}

} // namespace Agb
