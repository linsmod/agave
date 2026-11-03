/*
 * Copyright (C) 2006, 2007 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CachedFont.h"

#include "Cache.h"
#include "CachedResourceClient.h"
#include "CachedResourceClientWalker.h"
#include "FontPlatformData.h"
#include "SharedBuffer.h"
#include "TextResourceDecoder.h"
#include "loader.h"
#include <wtf/Vector.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <picasso/picasso.h>
#endif

/* set AGB_FONT_LOG=1 to trace the @font-face load path */
static bool fontLogOn(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("AGB_FONT_LOG") ? 1 : 0;
    return on;
}

#if defined(_WIN32)
#define FONTLOG(...) do { if (fontLogOn()) { fprintf(stderr, "[font] " __VA_ARGS__); } } while (0)
#else
#define FONTLOG(...) do {} while (0)
#endif

namespace WebCore {

#if defined(_WIN32)

/* picasso's Windows font adapter is a thin GDI wrapper: ps_font_create does
 * CreateFontA with the face name. So a web font only has to be made visible
 * to GDI inside this process (AddFontMemResourceEx) and then can be created
 * by its *internal* family name - which usually differs from the CSS-declared
 * name, so parse it out of the sfnt 'name' table first. */

namespace {

inline unsigned short sfntU16(const unsigned char* p)
{
    return (unsigned short)((p[0] << 8) | p[1]);
}

inline unsigned long sfntU32(const unsigned char* p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

bool sfntExtractFamilyName(const unsigned char* data, size_t size, char* out, size_t outSize)
{
    out[0] = 0;
    if (size < 16)
        return false;

    const unsigned char* base = data;
    unsigned long version = sfntU32(base);

    /* font collection: address the first contained face */
    if (version == 0x74746366) { /* 'ttcf' */
        unsigned long firstOffset = sfntU32(data + 12);
        if (firstOffset + 12 > size)
            return false;
        base = data + firstOffset;
        version = sfntU32(base);
    }

    if (version != 0x00010000 && /* TrueType */
        version != 0x74727565 && /* 'true' */
        version != 0x4F54544F)   /* 'OTTO' (CFF) */
        return false;

    unsigned short numTables = sfntU16(base + 4);
    if (12 + (size_t)numTables * 16 > size)
        return false;

    const unsigned char* nameTable = 0;
    for (unsigned short i = 0; i < numTables; ++i) {
        const unsigned char* rec = base + 12 + i * 16;
        if (rec[0] == 'n' && rec[1] == 'a' && rec[2] == 'm' && rec[3] == 'e') {
            unsigned long offset = sfntU32(rec + 8);
            if (offset + 6 <= size)
                nameTable = data + offset;
            break;
        }
    }
    if (!nameTable)
        return false;
    size_t nameRemaining = size - (nameTable - data);

    unsigned short count = sfntU16(nameTable + 2);
    unsigned short stringOffset = sfntU16(nameTable + 4);
    if (stringOffset > nameRemaining || 6 + (size_t)count * 12 > nameRemaining)
        return false;
    const unsigned char* strings = nameTable + stringOffset;
    size_t stringsRemaining = nameRemaining - stringOffset;

    /* Windows family name (nameID 1): prefer the Unicode string
     * (platform 3, UTF-16BE), fall back to the Latin-1 Macintosh one. */
    const unsigned char* best16 = 0;
    unsigned short len16 = 0;
    const unsigned char* best8 = 0;
    unsigned short len8 = 0;

    for (unsigned short i = 0; i < count; ++i) {
        const unsigned char* rec = nameTable + 6 + i * 12;
        unsigned short platformID = sfntU16(rec);
        unsigned short nameID = sfntU16(rec + 6);
        unsigned short length = sfntU16(rec + 8);
        unsigned short offset = sfntU16(rec + 10);
        if (nameID != 1 || !length)
            continue;
        if ((size_t)offset + length > stringsRemaining)
            continue;
        if (platformID == 3 && !best16) {
            best16 = strings + offset;
            len16 = length;
        } else if (platformID == 1 && !best8) {
            best8 = strings + offset;
            len8 = length;
        }
    }

    if (best16) {
        /* length is in bytes; two bytes per UTF-16 code unit */
        int chars = len16 / 2;
        if (chars <= 0)
            return false;

        /* GDI's CreateFontA matches against the ANSI code page */
        wchar_t wide[64];
        if (chars > 63)
            chars = 63;
        for (int i = 0; i < chars; ++i)
            wide[i] = (wchar_t)((best16[i * 2] << 8) | best16[i * 2 + 1]);
        wide[chars] = 0;

        int converted = WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int)outSize, 0, 0);
        return converted > 1;
    }

    if (best8) {
        if ((size_t)len8 >= outSize)
            len8 = (unsigned short)(outSize - 1);
        memcpy(out, best8, len8);
        out[len8] = 0;
        return len8 > 0;
    }

    return false;
}

} /* namespace */

#endif /* _WIN32 */

CachedFont::CachedFont(DocLoader* dl, const String &url)
    : CachedResource(url, FontResource)
    , m_customFontDataCreated(false)
    , m_fontMemHandle(0)
{
    memset(m_familyName, 0, sizeof(m_familyName));
    // Don't load the file yet.  Wait for an access before triggering the load.
    m_loading = true;
    m_loadInitiated = false;
}

CachedFont::~CachedFont()
{
    /* The private font registration is intentionally kept for the process
     * lifetime: glyph pages and cached FontDatas may outlive this resource
     * and GDI has no cheap way to know when the last use is gone.
     * Deferred: LRU the registrations if pages with many @font-face rules
     * turn out to exhaust GDI's private font quota. */
}

void CachedFont::ref(CachedResourceClient *c)
{
    CachedResource::ref(c);

    if (!m_loading)
        c->fontLoaded(this);
}

void CachedFont::data(PassRefPtr<SharedBuffer> data, bool allDataReceived)
{
    if (!allDataReceived)
        return;

    m_data = data;
    setEncodedSize(m_data.get() ? m_data->size() : 0);
    FONTLOG("data url=%s size=%u\n", url().utf8().data(), m_data.get() ? (unsigned)m_data->size() : 0);
    m_loading = false;
    checkNotify();
}

void CachedFont::beginLoadIfNeeded(DocLoader* dl)
{
    if (!m_loadInitiated) {
        m_loadInitiated = true;
        FONTLOG("beginLoad url=%s dl=%p\n", url().utf8().data(), (void*)dl);
        cache()->loader()->load(dl, this, false);
    }
}

bool CachedFont::ensureCustomFontData()
{
#if defined(_WIN32)
    if (m_customFontDataCreated)
        return m_familyName[0] != 0;

    m_customFontDataCreated = true;

    SharedBuffer* buf = CachedResource::data();
    if (!buf || !buf->size()) {
        FONTLOG("ensure: no data\n");
        return false;
    }

    if (!sfntExtractFamilyName((const unsigned char*)buf->data(), buf->size(), m_familyName, sizeof(m_familyName))) {
        FONTLOG("ensure: sfnt parse failed (woff? corrupt?) size=%u\n", (unsigned)buf->size());
        return false;
    }

    DWORD numFonts = 0;
    m_fontMemHandle = ::AddFontMemResourceEx((PVOID)buf->data(), (DWORD)buf->size(), 0, &numFonts);
    if (!m_fontMemHandle || !numFonts) {
        FONTLOG("ensure: AddFontMemResourceEx failed gle=%lu\n", GetLastError());
        m_familyName[0] = 0;
        return false;
    }
    FONTLOG("ensure: registered '%s' numFonts=%lu\n", m_familyName, numFonts);
    return true;
#else
    return false;
#endif
}

FontPlatformData CachedFont::platformDataFromCustomData(int size, bool bold, bool italic)
{
#if defined(_WIN32)
    if (m_familyName[0]) {
        ps_font* font = ps_font_create(m_familyName, CHARSET_UNICODE, (float)size,
                                       bold ? FONT_WEIGHT_BOLD : FONT_WEIGHT_REGULAR,
                                       italic ? True : False);
        FONTLOG("platformData size=%d bold=%d font=%p\n", size, bold, (void*)font);
        if (font)
            return FontPlatformData(font, size);
    }
#endif
    return FontPlatformData();
}

void CachedFont::allReferencesRemoved()
{
    /* keep the private font registered; see the destructor comment */
}

void CachedFont::checkNotify()
{
    if (m_loading)
        return;

    CachedResourceClientWalker w(m_clients);
    while (CachedResourceClient *c = w.next())
         c->fontLoaded(this);
}


void CachedFont::error()
{
    m_loading = false;
    m_errorOccurred = true;
    checkNotify();
}

}
