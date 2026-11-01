/*
** FontCachePS.cpp: FontCache implements by picasso.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/

#include "config.h"
#include "DeprecatedString.h"
#include "FontCache.h"
#include "FontData.h"
#include "Font.h"
#include "Debug.h"
#include <stdio.h>
#include <string.h>

namespace WebCore
{

void FontCache::platformInit()
{
    notImplemented();
}

const FontData* FontCache::getFontDataForCharacters(const Font& font, const UChar* characters, int length)
{
    /* The default Latin families (Arial, Helvetica, sans-serif) carry no CJK
     * glyphs and picasso has no fontconfig style fallback on this platform, so
     * every non-Latin character used to be drawn as a missing-glyph box. Route
     * them to a family that does have the glyphs. */
    static AtomicString cjkFamily("Microsoft YaHei");

    if (!characters || length <= 0)
        return 0;

    for (int i = 0; i < length; ++i) {
        UChar c = characters[i];
        if (c < 0x0100)
            continue;

        {
            FontPlatformData* platform = getCachedFontPlatformData(font.fontDescription(), cjkFamily, true);
            if (!platform)
                return 0;
            return getCachedFontData(platform);
        }
    }

    return 0;
}

FontPlatformData* FontCache::getSimilarFontPlatformData(const Font& font)
{
    notImplemented();
    return 0;
}

bool FontCache::fontExists(const FontDescription&, const AtomicString& family)
{
    notImplemented();
    return true;
}

FontPlatformData* FontCache::getLastResortFallbackFont(const FontDescription& fontDescription)
{
    // FIXME: Would be even better to somehow get the user's default font here.  For now we'll pick
    // the default that the user would get without changing any prefs.
    static AtomicString timesStr("Times New Roman");
    return getCachedFontPlatformData(fontDescription, timesStr);
}

FontPlatformData* FontCache::createFontPlatformData(const FontDescription& fontDescription, const AtomicString& family)
{
    ps_bool italic;
	ps_font_weight blod;

    if (fontDescription.italic())
        italic = True;
    else
        italic = False;

    if (fontDescription.bold())
        blod = FONT_WEIGHT_BOLD;
    else
        blod = FONT_WEIGHT_MEDIUM;

    /* picasso has no per-character fallback on this platform and the stock
     * Latin families carry no CJK glyphs, so Chinese text used to render as
     * missing-glyph boxes. Substitute a family that covers both scripts. */
    static const char* latinOnly[] = {
        "arial", "helvetica", "verdana", "tahoma", "segoe ui", "calibri",
        "times new roman", "georgia", "garamond", "courier new", "consolas",
        "sans-serif", "serif", "monospace", "cursive", "fantasy", 0
    };

    const char* requested = (const char*)family.deprecatedString().ascii();
    const char* effective = requested;

    for (int i = 0; latinOnly[i]; ++i) {
        if (requested && !strcasecmp(requested, latinOnly[i])) {
            effective = "Microsoft YaHei";
            break;
        }
    }

    ps_font* font = ps_font_create ((char*)effective, CHARSET_UNICODE,
                                fontDescription.computedPixelSize(), blod, italic);

    return new FontPlatformData(font, fontDescription.computedPixelSize());
}

}
