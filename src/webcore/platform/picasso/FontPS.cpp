/*
** FontPS.cpp: Font implements by picasso.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/
#include "config.h"
#include "Font.h"
#include <wtf/MathExtras.h>

#include "CString.h"
#include "FontData.h"
#include "FontFallbackList.h"
#include "GlyphBuffer.h"
#include "GraphicsContext.h"
#include "AffineTransform.h"
#include "Debug.h"

namespace WebCore {

static inline ps_color make_color(const Color& c)
{
	ps_color color;
	color.r = ((double)c.red())/255;
	color.g = ((double)c.green())/255;
	color.b = ((double)c.blue())/255;
	color.a = ((double)c.alpha())/255;
	return color;
}

void Font::drawGlyphs(GraphicsContext* graphicsContext, const FontData* font, const GlyphBuffer& glyphBuffer,  int from, int numGlyphs, const FloatPoint& point) const
{
	/*FIXME: font css style size is zero.*/
	if (!font->m_font.size())
		return;

    ps_context* gc = graphicsContext->platformContext();
    Color pcol = graphicsContext->fillColor();
	ps_color fc = make_color(pcol);

	ps_set_text_color(gc, &fc);
	ps_set_font(gc, font->m_font.hfont());

    const GlyphBufferGlyph* glyphs = (GlyphBufferGlyph*)glyphBuffer.glyphs(from);

    /* Glyphs do not follow the world matrix on their own: picasso only applies
     * it to outline glyphs. With an identity text matrix the win32/freetype
     * font adapters hand back hinted mono bitmaps (GGO_BITMAP / mono
     * rasterization), and painter::render_glyph can only translate a mono
     * bitmap. So a CSS rotate()/scale() moved the box but left the text
     * unrotated somewhere else - often off the clip, i.e. invisible.
     *
     * Fix: for a rotated/scaled context, move the linear part into picasso's
     * text matrix so the glyph outlines (and advance vectors) come out
     * pre-transformed, draw with an identity world matrix so the painter does
     * not apply it a second time, and pass the pen already mapped to device
     * space. Ascent is added by picasso in device space, so it is subtracted
     * here after the mapping. */
    const AffineTransform& m = graphicsContext->getMatrix();
    if (m.a() == 1 && m.b() == 0 && m.c() == 0 && m.d() == 1) {
        ps_wide_text_out_length(gc, point.x(), point.y()-font->ascent(), glyphs, numGlyphs);
        return;
    }

    ps_matrix* textMtx = ps_matrix_create_init((float)m.a(), (float)m.b(), (float)m.c(), (float)m.d(), 0, 0);
    ps_matrix* identity = ps_matrix_create_init(1, 0, 0, 1, 0, 0);
    if (!textMtx || !identity) {
        if (textMtx) ps_matrix_unref(textMtx);
        if (identity) ps_matrix_unref(identity);
        return;
    }

    float x = (float)(m.a() * point.x() + m.c() * point.y() + m.e());
    float y = (float)(m.b() * point.x() + m.d() * point.y() + m.f());

    ps_save(gc);
    ps_identity(gc);
    ps_set_text_matrix(gc, textMtx);
    ps_wide_text_out_length(gc, x, y-font->ascent(), glyphs, numGlyphs);
    ps_restore(gc);
    ps_set_text_matrix(gc, identity);

    ps_matrix_unref(identity);
    ps_matrix_unref(textMtx);
}

void Font::drawComplexText(GraphicsContext* graphicsContext, const TextRun& run, const TextStyle& style,
                                                          const FloatPoint& point, int from, int to) const
{
    drawSimpleText (graphicsContext, run, style, point, from, to);
}

float Font::floatWidthForComplexText(const TextRun& run, const TextStyle& style) const
{
    return floatWidthForSimpleText (run, style, 0);
}

FloatRect Font::selectionRectForComplexText(const TextRun& run, const TextStyle& style, const IntPoint& point, int h,
                                            int from, int to) const
{
    return selectionRectForSimpleText(run, style, point, h, from, to);
}

int Font::offsetForPositionForComplexText(const TextRun& run, const TextStyle& style, int x, bool includePartialGlyphs) const
{
    return offsetForPositionForSimpleText(run, style, x, includePartialGlyphs);
}

}
