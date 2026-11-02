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

#if defined(_WIN32)
#include <windows.h>
#endif

namespace WebCore
{

/* Pages name families that do not exist on every Windows install (macOS or
 * Android families, or CJK-less Latin faces). GDI silently substitutes its
 * default font for a missing face, which has no CJK glyphs, so Chinese text
 * renders as missing-glyph boxes. Ask GDI whether the face can draw a CJK
 * glyph and fall back to one that can. Cached: this runs per family. */
namespace {

#if defined(_WIN32)
struct FaceCacheEntry {
    char name[LF_FACESIZE];
    bool usable;
};

int CALLBACK countFaceProc(const LOGFONTA*, const TEXTMETRICA*, DWORD, LPARAM param)
{
    *(bool*)param = true;
    return 0;
}

/* GDI quietly substitutes a default font when the requested face does not
 * exist, so "the font renders CJK" is not enough: ask whether the face is
 * installed at all, then whether it actually carries a CJK glyph. */
bool faceDrawsCjk(const char* family)
{
    static FaceCacheEntry cache[64];
    static int cacheCount = 0;
    LOGFONTA lf;
    HDC dc;
    HFONT font;
    HFONT previous;
    WORD index = 0;
    DWORD result;
    bool installed = false;
    int i;

    if (!family || !family[0])
        return false;

    for (i = 0; i < cacheCount; ++i) {
        if (!strcasecmp(cache[i].name, family))
            return cache[i].usable;
    }

    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    strncpy(lf.lfFaceName, family, LF_FACESIZE - 1);

    dc = ::GetDC(0);

    ::EnumFontFamiliesExA(dc, &lf, (FONTENUMPROCA)countFaceProc, (LPARAM)&installed, 0);

    font = ::CreateFontIndirectA(&lf);
    previous = (HFONT)::SelectObject(dc, font);

    /* U+4E2D (CJK). A missing glyph comes back as 0xFFFF. */
    result = ::GetGlyphIndicesW(dc, L"\x4E2D", 1, &index, 0);

    ::SelectObject(dc, previous);
    ::DeleteObject(font);
    ::ReleaseDC(0, dc);

    {
        bool usable = installed && result != GDI_ERROR && index != 0xFFFF;

        if (cacheCount < 64) {
            strncpy(cache[cacheCount].name, family, LF_FACESIZE - 1);
            cache[cacheCount].name[LF_FACESIZE - 1] = 0;
            cache[cacheCount].usable = usable;
            ++cacheCount;
        }

        return usable;
    }
}
#else
bool faceDrawsCjk(const char*)
{
    return true;
}
#endif

} /* namespace */

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

    /* Anything else (a macOS/Android family, a typo, a face that exists but has
     * no CJK glyphs) is checked with GDI and replaced when it cannot draw CJK. */
    if (effective == requested && !faceDrawsCjk(effective))
        effective = "Microsoft YaHei";

    ps_font* font = ps_font_create ((char*)effective, CHARSET_UNICODE,
                                fontDescription.computedPixelSize(), blod, italic);

    return new FontPlatformData(font, fontDescription.computedPixelSize());
}

}
