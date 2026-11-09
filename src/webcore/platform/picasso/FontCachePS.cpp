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
/* The build copies `fonts/` next to the executable (fonts/NotoSansSC-*.ttf)
 * and the default family points at it. Register those files with GDI privately
 * so the browser does not depend on the face being installed system-wide;
 * FR_PRIVATE keeps them inside this process. Missing directory/files are not an
 * error: GDI falls back to its own default and createFontPlatformData() still
 * substitutes a CJK-capable face. Must run before the first CreateFontA. */
void registerBundledFonts()
{
    char exe[MAX_PATH];
    DWORD len = ::GetModuleFileNameA(0, exe, MAX_PATH);
    char* slash;
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA data;
    HANDLE find;

    if (!len || len >= MAX_PATH)
        return;

    slash = strrchr(exe, '\\');
    if (!slash)
        return;
    *(slash + 1) = 0;

    snprintf(pattern, sizeof(pattern), "%sfonts\\*.ttf", exe);

    find = ::FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char path[MAX_PATH];

            snprintf(path, sizeof(path), "%sfonts\\%s", exe, data.cFileName);
            ::AddFontResourceExA(path, FR_PRIVATE, 0);
        }
    } while (::FindNextFileA(find, &data));

    ::FindClose(find);
}

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

/* The default CJK family. Noto Sans SC is what Chrome resolves the default
 * (and `sans-serif`) to on this platform and it ships in fonts/; Microsoft
 * YaHei is the system fallback when the bundle is missing. */
const char* preferredCjkFamily()
{
    return faceDrawsCjk("Noto Sans SC") ? "Noto Sans SC" : "Microsoft YaHei";
}

} /* namespace */

void FontCache::platformInit()
{
#if defined(_WIN32)
    registerBundledFonts();
#endif
}

const FontData* FontCache::getFontDataForCharacters(const Font& font, const UChar* characters, int length)
{
    /* The default Latin families (Arial, Helvetica, sans-serif) carry no CJK
     * glyphs and picasso has no fontconfig style fallback on this platform, so
     * every non-Latin character used to be drawn as a missing-glyph box. Route
     * them to a family that does have the glyphs. */
    AtomicString cjkFamily(preferredCjkFamily());

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
    /* No page font and no usable family: this is the default page font. Noto
     * Sans SC (bundled in fonts/ and registered privately at startup) is the
     * face Chrome picks for a zh-CN default; Times New Roman used to make every
     * unstyled page look serif and shifted default line boxes. */
    static AtomicString fallbackStr("Noto Sans SC");
    return getCachedFontPlatformData(fontDescription, fallbackStr);
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

    bool substitute = !getenv("AGB_FONT_NO_SUBST");
    if (substitute) {
        for (int i = 0; latinOnly[i]; ++i) {
            if (requested && !strcasecmp(requested, latinOnly[i])) {
                effective = preferredCjkFamily();
                break;
            }
        }

        /* Anything else (a macOS/Android family, a typo, a face that exists but has
         * no CJK glyphs) is checked with GDI and replaced when it cannot draw CJK. */
        if (effective == requested && !faceDrawsCjk(effective))
            effective = preferredCjkFamily();
    }

    ps_font* font = ps_font_create ((char*)effective, CHARSET_UNICODE,
                                fontDescription.computedPixelSize(), blod, italic);

    return new FontPlatformData(font, fontDescription.computedPixelSize());
}

}
