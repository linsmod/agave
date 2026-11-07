/*
** FontDataPS.cpp: FontData implements by picasso.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/


#include "config.h"
#include "Font.h"
#include "FontCache.h"
#include "FloatRect.h"
#include "FontData.h"
#include "FontDescription.h"
#include <wtf/MathExtras.h>

#include <picasso/picasso.h>

namespace WebCore
{
extern ps_context* getGlobalContext(void);

void FontData::platformInit()
{
	ps_font_info info;
	ps_font* f = ps_set_font(getGlobalContext(), m_font.hfont());
	ps_get_font_info(getGlobalContext(), &info);
    m_ascent = info.ascent;
    m_descent = info.descent;

    /* x-height from the ink box of lowercase "x". ps_get_text_extent().h is
     * NOT text-dependent (the API reports the font's box height), which made
     * Font::xHeight() return the full box (20px on a 14px font) and pushed
     * every vertical-align:middle icon too far up (Baidu hot-search rows). */
    ps_glyph g;
    ps_size size;
    if (ps_get_glyph(getGlobalContext(), 'x', &g)) {
        ps_rect ink = {0, 0, 0, 0};
        if (ps_glyph_get_ink_bounds(&g, &ink) && ink.h > 0)
            m_xHeight = ink.h;
        else
            m_xHeight = info.ascent * 0.5f;   /* sane fallback */
    } else
        m_xHeight = info.ascent * 0.5f;

	ps_get_text_extent(getGlobalContext(), " ", 1, &size);
    m_spaceWidth = size.w;

    m_lineGap = info.leading;
    m_unitsPerEm = info.unitsEM;
    /* "normal" line height is the font box (ascender+descender), the way
     * direct TTF readers (Chrome) compute it: adding the line gap inflated
     * every font-normal inline box (an icon web font with hhea asc+desc
     * = 1.0em measured 15px at font-size 14 against the file's own 14). The
     * gap stays in m_lineGap for anything that needs the full spacing. */
    m_lineSpacing = m_ascent + m_descent;
	ps_set_font(getGlobalContext(), f);
}

void FontData::platformDestroy()
{
    delete m_smallCapsFontData;
}

FontData* FontData::smallCapsFontData(const FontDescription& fontDescription) const
{
    if (!m_smallCapsFontData) {
        int smallCapsHeight = lroundf(0.70f * fontDescription.computedSize());
        ps_font* hfont = ps_font_create_copy(m_font.hfont());
        ps_font_set_size(hfont, smallCapsHeight);
        m_smallCapsFontData = new FontData(FontPlatformData(hfont, smallCapsHeight));
    }
    return m_smallCapsFontData;
}

bool FontData::containsCharacters(const UChar* characters, int length) const
{
    return true;
}

void FontData::determinePitch()
{
    m_treatAsFixedPitch = false;
}

float FontData::platformWidthForGlyph(Glyph glyph) const
{
	/*FIXME: font css style size is zero.*/
	if (!m_font.size())
		return 0;

	ps_glyph g;
	ps_font* f = ps_set_font(getGlobalContext(), m_font.hfont());
	if (!ps_get_glyph(getGlobalContext(), (int)glyph, &g)){
		ps_set_font(getGlobalContext(), f);
		return 0;
	}
    ps_size size = {0, 0};
	ps_glyph_get_extent(&g, &size);
	ps_set_font(getGlobalContext(), f);
    return size.w;
}

}
