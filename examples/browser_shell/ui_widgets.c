/*
 * ui_widgets.c - UiButton and UiTabStrip.
 */

#include "ui_widgets.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static int tabVisible(const UiChromeModel* model, int index)
{
    return index >= 0 && index < UI_MAX_TABS && model->tabs[index].used;
}

static wchar_t firstCharacter(const char* utf8)
{
    wchar_t wide[8];
    int n;
    int i;

    if (!utf8 || !utf8[0])
        return L'?';

    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 8);

    if (n <= 0)
        return L'?';

    for (i = 0; i < n - 1; ++i) {
        if (wide[i] != L' ' && wide[i] != L'\t')
            return wide[i];
    }

    return L'?';
}

/* Stable, pleasant color per site: same URL -> same chip color. */
static unsigned faviconColor(const char* url)
{
    static const unsigned palette[] = {
        0xFF1A73E8, 0xFFD93025, 0xFF188038, 0xFFE37400,
        0xFF9334E6, 0xFF00897B, 0xFF3949AB, 0xFFC5221F,
        0xFF6D4C41, 0xFF00838F
    };
    unsigned hash = 2166136261u;
    const char* p;

    for (p = url ? url : ""; *p; ++p) {
        hash ^= (unsigned char)*p;
        hash *= 16777619u;
    }

    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

static void fillTabShape(UiPainter* painter, UiRect rect, int radius, unsigned color)
{
    UiRect bottom;

    if (rect.w <= 0 || rect.h <= 0)
        return;

    uiFillRoundRect(painter, rect, radius, color);

    /* Square off the bottom: an active tab merges into the toolbar. */
    bottom = uiRectMake(rect.x, rect.y + rect.h / 2, rect.w, rect.h - rect.h / 2);
    uiFillRect(painter, bottom, color);
}

/* Chrome's *inactive* tab hover/press is a rounded pill that floats inside the
 * tab slot and leaves a hairline of strip at the bottom; only the active tab
 * merges into the toolbar (fillTabShape squares that one off). Reusing
 * fillTabShape here made the hover read as a box welded to the toolbar. */
static void fillTabPill(UiPainter* painter, const UiTheme* theme, UiRect rect,
                        int radius, unsigned color)
{
    UiRect pill;
    int inset;

    if (rect.w <= 0 || rect.h <= 0)
        return;

    inset = uiThemeScale(theme, 1);

    if (rect.h - inset * 3 < 2) {
        uiFillRoundRect(painter, rect, radius, color);
        return;
    }

    pill = uiRectMake(rect.x, rect.y + inset, rect.w, rect.h - inset * 3);
    uiFillRoundRect(painter, pill, radius, color);
}

static void paintFavicon(UiPainter* painter, const UiTheme* theme,
                         UiRect rect, const char* url, const char* title)
{
    unsigned color = faviconColor(url);
    wchar_t letter[2];
    UiRect text;

    uiFillRoundRect(painter, rect, uiThemeScale(theme, 4), color);

    letter[0] = firstCharacter(title && title[0] ? title : url);
    letter[1] = 0;

    text = uiRectMake(rect.x, rect.y, rect.w, rect.h);
    uiTextW(painter, letter, text, theme->font_small, 0xFFFFFFFF, UI_ALIGN_CENTER);
}

static void paintTabClose(UiPainter* painter, const UiTheme* theme,
                          UiRect hit, int hovered, int pressed)
{
    /* 20 px chip with the full 16 px glyph box inside it. The old code used a
     * 16 px chip *and* inset the icon by another 4 px on each side, so the
     * glyph box was 8 px and the X itself 4x4 device pixels - the "× 太小"
     * report. */
    int chip = uiThemeScale(theme, 10);
    int glyph = uiThemeScale(theme, 16);
    int cx = hit.x + hit.w / 2;
    int cy = hit.y + hit.h / 2;
    unsigned color = (hovered || pressed) ? theme->text : theme->text_muted;
    UiRect icon;

    if (glyph > hit.w)
        glyph = hit.w;

    icon = uiRectMake(cx - glyph / 2, cy - glyph / 2, glyph, glyph);

    if (hovered || pressed) {
        /* Chrome's tab close is a neutral chip; only the title-bar X turns
         * red. */
        uiFillCircle(painter, cx, cy, chip,
                     pressed ? theme->tab_close_pressed : theme->tab_close_hover);
    }

    uiIcon(painter, UI_ICON_CLOSE, icon, color,
           ((hovered || pressed) ? 1.5f : 1.4f) * theme->scale);
}

/* ------------------------------------------------------------------------- */
/* UiButton                                                                  */
/* ------------------------------------------------------------------------- */

void uiButtonInit(UiButton* button, int id, UiIcon icon)
{
    if (!button)
        return;

    memset(button, 0, sizeof(*button));
    button->id = id;
    button->icon = icon;
    button->enabled = 1;
    button->visible = 1;
}

void uiButtonReset(UiButton* button)
{
    if (!button)
        return;

    button->hover = 0;
    button->pressed = 0;
}

void uiButtonPaint(UiButton* button, const UiTheme* theme, UiPainter* painter)
{
    UiRect icon;
    unsigned background = 0;
    unsigned color;
    int hovered;

    if (!button || !button->visible || button->rect.w <= 0 || button->rect.h <= 0)
        return;

    hovered = button->hover && button->enabled;

    if (button->window_control) {
        /* Title-bar buttons run the full tab-strip height, like Chrome's. */
        if (button->danger && (button->pressed || hovered))
            background = theme->window_close_hover;
        else if (button->pressed && button->enabled)
            background = theme->button_pressed;
        else if (hovered)
            background = theme->button_hover;

        if (background)
            uiFillRect(painter, button->rect, background);

        if (button->danger && background == theme->window_close_hover)
            color = 0xFFFFFFFFu;
        else if (!button->enabled)
            color = theme->text_faint;
        else
            color = theme->icon;

        {
            int size = uiThemeScale(theme, 16);
            int cx = button->rect.x + button->rect.w / 2;
            int cy = button->rect.y + button->rect.h / 2;

            icon = uiRectMake(cx - size / 2, cy - size / 2, size, size);
        }

        uiIcon(painter, button->icon, icon, color, (1.4f * theme->scale));
        return;
    }

    /* Chrome's toolbar icon buttons get a circular tint, not a rounded
     * square; the radius is the full button so the shape reads as a circle
     * even at 125%/150%. */
    if (button->pressed && button->enabled)
        background = theme->button_pressed;
    else if (button->toggled)
        background = theme->button_pressed;
    else if (hovered)
        background = theme->button_hover;

    if (background) {
        int cx = button->rect.x + button->rect.w / 2;
        int cy = button->rect.y + button->rect.h / 2;
        int radius = (button->rect.w < button->rect.h ? button->rect.w
                                                      : button->rect.h) / 2;

        uiFillCircle(painter, cx, cy, radius, background);
    }

    if (!button->enabled)
        color = theme->text_faint;
    else if (button->toggled)
        color = theme->text;
    else
        color = theme->icon;

    /* A fixed 8 px inset was only right for the 32 px toolbar buttons; the
     * 24 px find-bar buttons were left with an 8 px glyph box (chevrons ~3 px
     * wide). Size the box off the button instead, capped at Chrome's 16 px. */
    {
        int side = button->rect.w < button->rect.h ? button->rect.w : button->rect.h;
        int box = side - uiThemeScale(theme, 8);
        int cx = button->rect.x + button->rect.w / 2;
        int cy = button->rect.y + button->rect.h / 2;

        if (box > uiThemeScale(theme, 16))
            box = uiThemeScale(theme, 16);
        if (box < uiThemeScale(theme, 10))
            box = uiThemeScale(theme, 10);

        icon = uiRectMake(cx - box / 2, cy - box / 2, box, box);
    }

    uiIcon(painter, button->icon, icon, color, (1.7f * theme->scale));
}

int uiButtonHit(const UiButton* button, int x, int y)
{
    return button && button->visible && button->enabled &&
           uiRectContains(button->rect, x, y);
}

int uiButtonMouse(UiButton* button, const UiMouseEvent* event, int* fire)
{
    int inside;

    if (fire)
        *fire = 0;

    if (!button || !button->visible || !event)
        return 0;

    inside = button->enabled && uiRectContains(button->rect, event->x, event->y);

    switch (event->type) {
    case UI_MOUSE_MOVE:
        button->hover = inside;
        return inside;

    case UI_MOUSE_DOWN:
        if (!inside)
            return 0;
        button->pressed = 1;
        button->hover = 1;
        return 1;

    case UI_MOUSE_UP: {
        int wasPressed = button->pressed;

        button->pressed = 0;
        button->hover = inside;

        if (wasPressed && inside) {
            if (fire)
                *fire = 1;
            return 1;
        }

        return inside;
    }

    case UI_MOUSE_LEAVE:
        button->hover = 0;
        button->pressed = 0;
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* UiTabStrip                                                               */
/* ------------------------------------------------------------------------- */

void uiTabStripLayout(UiTabStrip* strip, const UiTheme* theme,
                      const UiChromeModel* model, UiRect area)
{
    int i;
    int slot_w;
    int x;
    int tab_y;
    int available;
    int gap;
    int plus;
    int pad;

    if (!strip || !theme || !model)
        return;

    strip->rect = area;
    strip->count = 0;
    strip->hover_tab = -1;
    strip->hover_close = -1;
    strip->pressed_tab = -1;
    strip->pressed_close = -1;
    strip->hover_new_tab = 0;
    strip->pressed_new_tab = 0;

    memset(strip->tab, 0, sizeof(strip->tab));
    memset(strip->close, 0, sizeof(strip->close));
    memset(strip->slot_of, 0, sizeof(strip->slot_of));

    for (i = 0; i < UI_MAX_TABS; ++i) {
        if (tabVisible(model, i))
            strip->slot_of[strip->count++] = i;
    }

    pad = theme->strip_pad;
    gap = theme->tab_gap;
    plus = theme->button_w;
    available = area.w - pad * 2 - plus - gap - strip->right_reserved;

    slot_w = theme->tab_max_w;

    if (strip->count > 0) {
        if (available < strip->count)
            available = strip->count;

        /* Shrink to fit: never let the last tab run under the window buttons.
         * At the minimum window width (680 px) even 16 tabs still fit; only
         * the title ellipsizes and the close button drops out. */
        slot_w = available / strip->count;

        if (slot_w > theme->tab_max_w)
            slot_w = theme->tab_max_w;
    }

    if (slot_w < 1)
        slot_w = 1;

    x = area.x + pad;
    tab_y = area.y + area.h - theme->tab_h;

    for (i = 0; i < strip->count; ++i) {
        int index = strip->slot_of[i];
        int w = slot_w - gap;
        int close_w = uiThemeScale(theme, 24);

        if (w < gap + 2)
            w = gap + 2;

        strip->tab[index] = uiRectMake(x, tab_y, w, theme->tab_h);

        /* Every tab carries its close chip, hovered or not (Chrome's default).
         * It only drops out once the strip is squeezed to less than half the
         * natural tab width: at that point the title needs the room more than
         * the chip does. The rect is the single source of truth - a zero rect
         * means "not painted, not clickable". */
        if (w * 2 >= theme->tab_max_w) {
            strip->close[index] = uiRectMake(x + w - close_w - uiThemeScale(theme, 2),
                                             tab_y + (theme->tab_h - close_w) / 2,
                                             close_w, close_w);
        } else {
            strip->close[index] = uiRectMake(0, 0, 0, 0);
        }

        x += slot_w;
    }

    strip->new_tab = uiRectMake(x + gap,
                                area.y + area.h - theme->button_h,
                                theme->button_w, theme->button_h);

    if (strip->new_tab.x + strip->new_tab.w >
        area.x + area.w - pad - strip->right_reserved)
        strip->new_tab.x = area.x + area.w - pad - strip->right_reserved - strip->new_tab.w;

    if (strip->new_tab.x < area.x + pad)
        strip->new_tab.x = area.x + pad;
}

UiPart uiTabStripHit(UiTabStrip* strip, int x, int y, int* model_index)
{
    int i;

    if (model_index)
        *model_index = -1;

    if (!strip || !uiRectContains(strip->rect, x, y))
        return UI_PART_NONE;

    for (i = 0; i < strip->count; ++i) {
        int index = strip->slot_of[i];

        if (uiRectContains(strip->close[index], x, y)) {
            if (model_index)
                *model_index = index;
            return UI_PART_TAB_CLOSE;
        }

        if (uiRectContains(strip->tab[index], x, y)) {
            if (model_index)
                *model_index = index;
            return UI_PART_TAB;
        }
    }

    if (uiRectContains(strip->new_tab, x, y))
        return UI_PART_TAB_NEW;

    return UI_PART_NONE;
}

static void paintTab(UiPainter* painter, const UiTheme* theme,
                     const UiChromeModel* model, UiTabStrip* strip,
                     int index, int active, int hovered, int close_hovered,
                     int pressed, int close_pressed)
{
    UiRect rect = strip->tab[index];
    UiRect favicon;
    UiRect title;
    int show_close;
    int show_favicon;

    if (rect.w <= 0)
        return;

    /* Layout already decided whether this tab has a chip (it drops out below
     * half the natural width); painting follows the rect so what looks
     * clickable and what is clickable cannot drift apart. */
    show_close = strip->close[index].w > 0;
    /* The icon never goes away: shrinking only takes room from the title. */
    show_favicon = 1;

    if (active)
        fillTabShape(painter, rect, theme->tab_radius, theme->surface);
    else if (pressed)
        fillTabPill(painter, theme, rect, theme->tab_radius, theme->tab_pressed);
    else if (hovered)
        fillTabPill(painter, theme, rect, theme->tab_radius, theme->tab_hover);

    if (show_favicon) {
        favicon = uiRectMake(rect.x + uiThemeScale(theme, 10),
                             rect.y + (rect.h - uiThemeScale(theme, 16)) / 2,
                             uiThemeScale(theme, 16), uiThemeScale(theme, 16));
        paintFavicon(painter, theme, favicon,
                     model->tabs[index].url, model->tabs[index].title);

        if (model->tabs[index].hung)
            uiFillCircle(painter, favicon.x + favicon.w - 1, favicon.y + 1,
                         uiThemeScale(theme, 3), theme->danger);

        title.x = favicon.x + favicon.w + uiThemeScale(theme, 8);
    } else {
        title.x = rect.x + uiThemeScale(theme, 8);
    }

    title.y = rect.y;
    title.h = rect.h;

    if (show_close)
        title.w = strip->close[index].x - title.x - uiThemeScale(theme, 2);
    else
        title.w = rect.x + rect.w - title.x - uiThemeScale(theme, 8);

    if (title.w > 0) {
        const char* label = model->tabs[index].title[0] ? model->tabs[index].title : "New tab";
        unsigned color = active ? theme->text
                                : (hovered ? theme->text : theme->text_muted);

        uiText(painter, label, title, theme->font_ui, color, UI_ALIGN_LEFT);
    }

    if (show_close)
        paintTabClose(painter, theme, strip->close[index], close_hovered,
                      close_pressed);
}

/* A tab that merges into the strip visually: the active one (its shape is
 * squared off into the toolbar) and the one under the pointer (its pill already
 * separates itself). Separators never touch those. */
static int tabIsFlush(const UiChromeModel* model, const UiTabStrip* strip, int index)
{
    return index == model->active ||
           index == strip->hover_tab ||
           index == strip->pressed_tab;
}

/* Chrome's strip keeps a hairline between neighbours. It lives inside the
 * layout gap, so it always has its own room and never eats a tab; it is skipped
 * where either neighbour is flush (see above) - which is exactly "no line on
 * both sides of the active/hovered tab". */
static void paintTabSeparators(UiPainter* painter, const UiTheme* theme,
                               const UiChromeModel* model, UiTabStrip* strip)
{
    int width = uiThemeScale(theme, 1);
    int height = uiThemeScale(theme, 16);
    int i;

    if (height > theme->tab_h - uiThemeScale(theme, 10))
        height = theme->tab_h - uiThemeScale(theme, 10);
    if (height < 2)
        height = 2;

    for (i = 1; i < strip->count; ++i) {
        int left = strip->slot_of[i - 1];
        int right = strip->slot_of[i];
        const UiRect* tab = &strip->tab[right];

        if (tab->w <= 0)
            continue;

        if (tabIsFlush(model, strip, left) || tabIsFlush(model, strip, right))
            continue;

        uiFillRect(painter,
                   uiRectMake(tab->x - (theme->tab_gap + width) / 2,
                              tab->y + (tab->h - height) / 2, width, height),
                   theme->tab_sep);
    }
}

void uiTabStripPaint(UiTabStrip* strip, const UiTheme* theme,
                     const UiChromeModel* model, UiPainter* painter)
{
    int i;
    int active = model->active;

    if (!strip || !theme || !model || !painter || !painter->g)
        return;

    uiFillRect(painter, strip->rect, theme->chrome);

    GdipSetClipRectI(painter->g, strip->rect.x, strip->rect.y,
                     strip->rect.w, strip->rect.h, CombineModeReplace);

    paintTabSeparators(painter, theme, model, strip);

    for (i = 0; i < strip->count; ++i) {
        int index = strip->slot_of[i];

        if (index == active)
            continue;

        paintTab(painter, theme, model, strip, index, 0,
                 strip->hover_tab == index, strip->hover_close == index,
                 strip->pressed_tab == index, strip->pressed_close == index);
    }

    if (tabVisible(model, active)) {
        paintTab(painter, theme, model, strip, active, 1,
                 strip->hover_tab == active, strip->hover_close == active,
                 strip->pressed_tab == active, strip->pressed_close == active);
    }

    /* "+" new tab */
    if (strip->hover_new_tab || strip->pressed_new_tab) {
        int radius = uiThemeScale(theme, 12);

        uiFillCircle(painter, strip->new_tab.x + strip->new_tab.w / 2,
                     strip->new_tab.y + strip->new_tab.h / 2,
                     radius, strip->pressed_new_tab ? theme->button_pressed
                                                    : theme->button_hover);
    }

    uiIcon(painter, UI_ICON_PLUS,
           uiRectInset(strip->new_tab, uiThemeScale(theme, 9), uiThemeScale(theme, 9)),
           theme->icon, (1.7f * theme->scale));

    GdipResetClip(painter->g);
}

void uiTabStripMouse(UiTabStrip* strip, const UiChromeModel* model,
                     const UiMouseEvent* event, UiTabAction* action)
{
    UiPart part;
    int index = -1;

    if (action) {
        action->activate = -1;
        action->close = -1;
        action->new_tab = 0;
    }

    if (!strip || !model || !event)
        return;

    switch (event->type) {
    case UI_MOUSE_LEAVE:
        strip->hover_tab = -1;
        strip->hover_close = -1;
        strip->hover_new_tab = 0;
        strip->pressed_tab = -1;
        strip->pressed_close = -1;
        strip->pressed_new_tab = 0;
        return;

    case UI_MOUSE_MOVE:
        part = uiTabStripHit(strip, event->x, event->y, &index);
        strip->hover_tab = (part == UI_PART_TAB || part == UI_PART_TAB_CLOSE) ? index : -1;
        strip->hover_close = (part == UI_PART_TAB_CLOSE) ? index : -1;
        strip->hover_new_tab = (part == UI_PART_TAB_NEW);
        return;

    case UI_MOUSE_DOWN:
        part = uiTabStripHit(strip, event->x, event->y, &index);
        strip->pressed_tab = (part == UI_PART_TAB || part == UI_PART_TAB_CLOSE) ? index : -1;
        strip->pressed_close = (part == UI_PART_TAB_CLOSE) ? index : -1;
        strip->pressed_new_tab = (part == UI_PART_TAB_NEW);
        return;

    case UI_MOUSE_UP: {
        int pressed_tab = strip->pressed_tab;
        int pressed_close = strip->pressed_close;
        int pressed_new = strip->pressed_new_tab;

        strip->pressed_tab = -1;
        strip->pressed_close = -1;
        strip->pressed_new_tab = 0;

        part = uiTabStripHit(strip, event->x, event->y, &index);

        if (pressed_close >= 0 && part == UI_PART_TAB_CLOSE && index == pressed_close) {
            if (action)
                action->close = index;
            return;
        }

        if (pressed_tab >= 0 && part == UI_PART_TAB && index == pressed_tab) {
            if (action)
                action->activate = index;
            return;
        }

        if (pressed_new && part == UI_PART_TAB_NEW) {
            if (action)
                action->new_tab = 1;
            return;
        }

        return;
    }

    default:
        return;
    }
}

void uiTabStripLeave(UiTabStrip* strip)
{
    if (!strip)
        return;

    strip->hover_tab = -1;
    strip->hover_close = -1;
    strip->hover_new_tab = 0;
    strip->pressed_tab = -1;
    strip->pressed_close = -1;
    strip->pressed_new_tab = 0;
}
