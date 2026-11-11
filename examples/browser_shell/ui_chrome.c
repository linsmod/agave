/*
 * ui_chrome.c - toolbar, find bar, status bar and the UiChrome composition.
 */

#include "ui_chrome.h"

#include <stdio.h>
#include <string.h>

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

static int scale(const UiTheme* theme, int value)
{
    return uiThemeScale(theme, value);
}

static int startsWithNoCase(const char* text, const char* prefix)
{
    size_t n;

    if (!text || !prefix)
        return 0;

    n = strlen(prefix);

    return _strnicmp(text, prefix, n) == 0;
}

static void emitCommand(UiChrome* chrome, int command)
{
    if (chrome && chrome->cb.command)
        chrome->cb.command(chrome->cb.user, command);
}

static void emitFocus(UiChrome* chrome, int field)
{
    if (chrome && chrome->cb.focus_field)
        chrome->cb.focus_field(chrome->cb.user, field);
}

/* ------------------------------------------------------------------------- */
/* init / teardown                                                           */
/* ------------------------------------------------------------------------- */

void uiChromeInit(UiChrome* chrome, UiThemeKind kind, unsigned dpi,
                  int offscreen, const UiChromeCallbacks* callbacks)
{
    if (!chrome)
        return;

    memset(chrome, 0, sizeof(*chrome));
    uiThemeInit(&chrome->theme, kind, dpi);
    chrome->offscreen = offscreen;
    chrome->focus_field = 0;
    chrome->hover_part = UI_PART_NONE;
    chrome->model.active = -1;

    if (callbacks)
        chrome->cb = *callbacks;

    uiButtonInit(&chrome->toolbar.back, UI_CMD_BACK, UI_ICON_BACK);
    uiButtonInit(&chrome->toolbar.forward, UI_CMD_FORWARD, UI_ICON_FORWARD);
    uiButtonInit(&chrome->toolbar.reload, UI_CMD_RELOAD, UI_ICON_RELOAD);
    uiButtonInit(&chrome->toolbar.go, UI_CMD_GO, UI_ICON_ARROW_RIGHT);
    uiButtonInit(&chrome->toolbar.menu, UI_CMD_MENU, UI_ICON_MENU);

    uiButtonInit(&chrome->findbar.prev, UI_CMD_FIND_PREV, UI_ICON_CHEVRON_UP);
    uiButtonInit(&chrome->findbar.next, UI_CMD_FIND_NEXT, UI_ICON_CHEVRON_DOWN);
    uiButtonInit(&chrome->findbar.close, UI_CMD_FIND_CLOSE, UI_ICON_CLOSE);

    uiButtonInit(&chrome->win_min, UI_CMD_MINIMIZE, UI_ICON_MINUS);
    uiButtonInit(&chrome->win_max, UI_CMD_MAXIMIZE, UI_ICON_MAXIMIZE);
    uiButtonInit(&chrome->win_close, UI_CMD_CLOSE_WINDOW, UI_ICON_CLOSE);
    chrome->win_min.window_control = 1;
    chrome->win_max.window_control = 1;
    chrome->win_close.window_control = 1;
    chrome->win_close.danger = 1;
}

void uiChromeFree(UiChrome* chrome)
{
    if (!chrome)
        return;

    uiThemeFree(&chrome->theme);
}

void uiChromeSetDpi(UiChrome* chrome, unsigned dpi)
{
    if (!chrome)
        return;

    uiThemeSetDpi(&chrome->theme, dpi);
}

void uiChromeSetNative(UiChrome* chrome, void* parent, void* address, void* find)
{
    if (!chrome)
        return;

    chrome->parent_hwnd = parent;
    chrome->address_hwnd = address;
    chrome->find_hwnd = find;
}

void uiChromeSetFocusField(UiChrome* chrome, int field)
{
    if (!chrome)
        return;

    chrome->focus_field = field;
}

/* ------------------------------------------------------------------------- */
/* layout                                                                    */
/* ------------------------------------------------------------------------- */

void uiChromeLayout(UiChrome* chrome, int width, int height)
{
    UiTheme* theme;
    UiChromeModel* model;
    int pad, gap, x, y;
    int toolbar_top, toolbar_h, strip_h, find_h, status_h;
    int content_top;
    int button_w, button_h;

    if (!chrome)
        return;

    theme = &chrome->theme;
    model = &chrome->model;

    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;

    chrome->width = width;
    chrome->height = height;

    strip_h = theme->tabstrip_h;
    toolbar_h = theme->toolbar_h;
    find_h = theme->find_h;
    status_h = theme->status_h;
    pad = scale(theme, 6);
    gap = scale(theme, 2);
    button_w = theme->button_w;
    button_h = theme->button_h;

    /* Chrome's caption controls are self-drawn at the right edge of the
     * title bar; the tab strip gets everything to their left. */
    {
        int control_w = theme->window_btn_w;

        chrome->win_min.rect = uiRectMake(width - control_w * 3, 0, control_w, strip_h);
        chrome->win_max.rect = uiRectMake(width - control_w * 2, 0, control_w, strip_h);
        chrome->win_close.rect = uiRectMake(width - control_w, 0, control_w, strip_h);

        chrome->tabstrip.right_reserved = control_w * 3;
    }

    uiTabStripLayout(&chrome->tabstrip, theme, model,
                     uiRectMake(0, 0, width, strip_h));

    toolbar_top = strip_h;
    chrome->toolbar.rect = uiRectMake(0, toolbar_top, width, toolbar_h);

    y = toolbar_top + (toolbar_h - button_h) / 2;
    x = pad;

    chrome->toolbar.back.rect = uiRectMake(x, y, button_w, button_h);
    x += button_w + gap;
    chrome->toolbar.forward.rect = uiRectMake(x, y, button_w, button_h);
    x += button_w + gap;
    chrome->toolbar.reload.rect = uiRectMake(x, y, button_w, button_h);
    x += button_w + gap;

    chrome->toolbar.separator = uiRectMake(x + gap, toolbar_top + scale(theme, 12),
                                           scale(theme, 1), toolbar_h - scale(theme, 24));
    x += scale(theme, 8);

    {
        int menu_x = width - pad - button_w;
        int address_x = x + scale(theme, 4);
        int address_right = menu_x - scale(theme, 8);
        int address_h = theme->address_h;
        int address_y = toolbar_top + (toolbar_h - address_h) / 2;
        int go_size = scale(theme, 28);
        int edit_x;
        int edit_w;

        chrome->toolbar.menu.rect = uiRectMake(menu_x, y, button_w, button_h);

        if (address_right < address_x + scale(theme, 120))
            address_right = address_x + scale(theme, 120);
        if (address_right > width - pad)
            address_right = width - pad;

        chrome->toolbar.address = uiRectMake(address_x, address_y,
                                             address_right - address_x, address_h);
        chrome->toolbar.address_icon = uiRectMake(address_x + scale(theme, 11),
                                                  address_y + (address_h - scale(theme, 16)) / 2,
                                                  scale(theme, 16), scale(theme, 16));

        chrome->toolbar.go.rect = uiRectMake(
            chrome->toolbar.address.x + chrome->toolbar.address.w - scale(theme, 4) - go_size,
            address_y + (address_h - go_size) / 2,
            go_size, go_size);
        chrome->toolbar.go.icon = UI_ICON_ARROW_RIGHT;

        edit_x = chrome->toolbar.address_icon.x + chrome->toolbar.address_icon.w + scale(theme, 8);
        edit_w = chrome->toolbar.go.rect.x - edit_x - scale(theme, 6);

        if (edit_w < scale(theme, 40))
            edit_w = scale(theme, 40);

        chrome->address_edit = uiRectMake(edit_x,
                                          address_y + (address_h - scale(theme, 20)) / 2,
                                          edit_w, scale(theme, 20));
    }

    content_top = strip_h + toolbar_h;

    if (model->find_visible) {
        int field_h = theme->find_field_h;
        int field_w = theme->find_field_w;
        int field_x;
        int field_y;
        int bs = scale(theme, 24);
        int bx;

        chrome->findbar.rect = uiRectMake(0, content_top, width, find_h);

        if (field_w > width - pad * 2)
            field_w = width - pad * 2;
        if (field_w < scale(theme, 160))
            field_w = scale(theme, 160);

        field_x = width - pad - field_w;

        if (field_x < pad)
            field_x = pad;

        field_y = content_top + (find_h - field_h) / 2;
        chrome->findbar.field = uiRectMake(field_x, field_y, field_w, field_h);

        bx = field_x + field_w - pad - bs;
        chrome->findbar.close.rect = uiRectMake(bx, field_y + (field_h - bs) / 2, bs, bs);
        bx -= bs + gap;
        chrome->findbar.next.rect = uiRectMake(bx, field_y + (field_h - bs) / 2, bs, bs);
        bx -= bs + gap;
        chrome->findbar.prev.rect = uiRectMake(bx, field_y + (field_h - bs) / 2, bs, bs);
        bx -= scale(theme, 4);

        chrome->findbar.edit = uiRectMake(
            field_x + scale(theme, 11) + scale(theme, 16) + scale(theme, 8),
            field_y + (field_h - scale(theme, 20)) / 2,
            bx - (field_x + scale(theme, 11) + scale(theme, 16) + scale(theme, 8)),
            scale(theme, 20));

        if (chrome->findbar.edit.w < scale(theme, 40))
            chrome->findbar.edit.w = scale(theme, 40);

        chrome->find_edit = chrome->findbar.edit;
        content_top += find_h;
    } else {
        memset(&chrome->findbar, 0, sizeof(chrome->findbar));
        chrome->find_edit = uiRectMake(0, 0, 0, 0);
    }

    chrome->content = uiRectMake(0, content_top, width,
                                 height - content_top - status_h);

    if (chrome->content.h < 0)
        chrome->content.h = 0;

    chrome->statusbar.rect = uiRectMake(0, height - status_h, width, status_h);
    chrome->statusbar.text = uiRectMake(scale(theme, 10), height - status_h,
                                        width - scale(theme, 20), status_h);
    chrome->statusbar.zoom = uiRectMake(0, 0, 0, 0);

    if (model->zoom != 100) {
        int zw = scale(theme, 46);
        int zh = status_h - scale(theme, 8);

        chrome->statusbar.zoom = uiRectMake(width - scale(theme, 10) - zw,
                                            height - status_h + scale(theme, 4),
                                            zw, zh);
        chrome->statusbar.text.w = chrome->statusbar.zoom.x - chrome->statusbar.text.x - scale(theme, 8);
    }

    if (chrome->statusbar.text.w < 0)
        chrome->statusbar.text.w = 0;

    /* Keep the tab strip's + button and the edit fields in sync after a
     * shrink; the shell reads these rects to MoveWindow its EDITs. */
    if (!model->find_visible)
        chrome->find_edit = uiRectMake(0, 0, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* painting                                                                  */
/* ------------------------------------------------------------------------- */

static void paintAddress(UiChrome* chrome, UiPainter* painter)
{
    UiTheme* theme = &chrome->theme;
    UiToolbar* bar = &chrome->toolbar;
    UiRect pill = bar->address;
    int focused = chrome->focus_field == 1;
    int hovered = chrome->hover_part == UI_PART_ADDRESS || bar->go.hover;
    unsigned fill = focused ? theme->surface : (hovered ? theme->field_hover : theme->field);
    int icon_size = scale(theme, 16);
    UiRect icon = bar->address_icon;
    int secure = startsWithNoCase(chrome->model.address, "https://");

    if (pill.w <= 0)
        return;

    uiFillRoundRect(painter, pill, theme->address_radius, fill);

    if (focused)
        uiStrokeRoundRect(painter, pill, theme->address_radius, theme->accent,
                          (float)scale(theme, 2));
    else if (hovered)
        uiStrokeRoundRect(painter, pill, theme->address_radius, theme->border_strong, 1.0f);
    else
        uiStrokeRoundRect(painter, pill, theme->address_radius, theme->border, 1.0f);

    uiIcon(painter, secure ? UI_ICON_LOCK : UI_ICON_GLOBE, icon,
           theme->text_muted, (1.5f * theme->scale));

    if (chrome->offscreen || !chrome->address_hwnd) {
        const char* text = chrome->model.address[0] ? chrome->model.address
                                                    : "Search or enter address";
        UiRect text_rect = uiRectMake(icon.x + icon.w + scale(theme, 8), pill.y,
                                      bar->go.rect.x - (icon.x + icon.w + scale(theme, 8)) - scale(theme, 6),
                                      pill.h);

        uiText(painter, text, text_rect, theme->font_address,
               chrome->model.address[0] ? theme->text : theme->text_faint, UI_ALIGN_LEFT);
    }

    (void)icon_size;
    uiButtonPaint(&bar->go, theme, painter);
}

static void paintToolbar(UiChrome* chrome, UiPainter* painter)
{
    UiTheme* theme = &chrome->theme;
    UiToolbar* bar = &chrome->toolbar;
    int percent;

    uiFillRect(painter, bar->rect, theme->toolbar);

    uiButtonPaint(&bar->back, theme, painter);
    uiButtonPaint(&bar->forward, theme, painter);
    bar->reload.icon = chrome->model.loading ? UI_ICON_STOP : UI_ICON_RELOAD;
    uiButtonPaint(&bar->reload, theme, painter);

    if (bar->separator.w > 0)
        uiFillRect(painter, bar->separator, theme->border);

    paintAddress(chrome, painter);
    uiButtonPaint(&bar->menu, theme, painter);

    /* A hairline separates the toolbar from the page; progress rides on it. */
    uiFillRect(painter, uiRectMake(0, bar->rect.y + bar->rect.h - scale(theme, 1),
                                   bar->rect.w, scale(theme, 1)), theme->border);

    if (chrome->model.loading) {
        percent = CLAMP(chrome->model.progress, 0, 100);

        if (percent <= 0)
            percent = 8;

        uiFillRect(painter, uiRectMake(0, bar->rect.y + bar->rect.h - scale(theme, 2),
                                       bar->rect.w * percent / 100, scale(theme, 2)),
                   theme->accent);
    }
}

static void paintFindBar(UiChrome* chrome, UiPainter* painter)
{
    UiTheme* theme = &chrome->theme;
    UiFindBar* bar = &chrome->findbar;
    int focused = chrome->focus_field == 2;
    int hovered = chrome->hover_part == UI_PART_FIND_FIELD;

    if (!chrome->model.find_visible || bar->rect.h <= 0)
        return;

    uiFillRect(painter, bar->rect, theme->toolbar);
    uiFillRect(painter, uiRectMake(0, bar->rect.y + bar->rect.h - scale(theme, 1),
                                   bar->rect.w, scale(theme, 1)), theme->border);

    uiFillRoundRect(painter, bar->field, theme->find_field_h / 2,
                    focused ? theme->surface : (hovered ? theme->field_hover : theme->field));

    if (focused)
        uiStrokeRoundRect(painter, bar->field, theme->find_field_h / 2, theme->accent,
                          (float)scale(theme, 2));
    else
        uiStrokeRoundRect(painter, bar->field, theme->find_field_h / 2, theme->border, 1.0f);

    uiIcon(painter, UI_ICON_SEARCH,
           uiRectMake(bar->field.x + scale(theme, 11),
                      bar->field.y + (bar->field.h - scale(theme, 16)) / 2,
                      scale(theme, 16), scale(theme, 16)),
           theme->text_muted, (1.5f * theme->scale));

    if (chrome->offscreen || !chrome->find_hwnd) {
        const char* text = chrome->model.find_text[0] ? chrome->model.find_text
                                                      : "Find in page";

        uiText(painter, text, bar->edit, theme->font_ui,
               chrome->model.find_text[0] ? theme->text : theme->text_faint, UI_ALIGN_LEFT);
    }

    uiButtonPaint(&bar->prev, theme, painter);
    uiButtonPaint(&bar->next, theme, painter);
    uiButtonPaint(&bar->close, theme, painter);
}

static void paintStatus(UiChrome* chrome, UiPainter* painter)
{
    UiTheme* theme = &chrome->theme;
    UiStatusBar* bar = &chrome->statusbar;
    const char* text = chrome->model.status[0] ? chrome->model.status : "Ready";

    uiFillRect(painter, bar->rect, theme->surface_alt);
    uiFillRect(painter, uiRectMake(0, bar->rect.y, bar->rect.w, scale(theme, 1)), theme->border);

    uiText(painter, text, bar->text, theme->font_small, theme->text_muted, UI_ALIGN_LEFT);

    if (chrome->model.zoom != 100 && bar->zoom.w > 0) {
        char label[32];

        _snprintf(label, sizeof(label), "%d%%", chrome->model.zoom);
        label[sizeof(label) - 1] = 0;

        uiFillRoundRect(painter, bar->zoom, scale(theme, 8), theme->accent_soft);
        uiText(painter, label, bar->zoom, theme->font_small, theme->accent, UI_ALIGN_CENTER);
    }
}

void uiChromePaint(UiChrome* chrome, HDC hdc)
{
    UiTheme* theme;
    UiPainter painter;

    if (!chrome || !hdc)
        return;

    theme = &chrome->theme;

    if (!uiPainterBegin(&painter, hdc, theme, chrome->width, chrome->height))
        return;

    uiFillRect(&painter, uiRectMake(0, 0, chrome->width, chrome->height), theme->window);

    uiTabStripPaint(&chrome->tabstrip, theme, &chrome->model, &painter);

    uiButtonPaint(&chrome->win_min, theme, &painter);
    uiButtonPaint(&chrome->win_max, theme, &painter);
    uiButtonPaint(&chrome->win_close, theme, &painter);

    paintToolbar(chrome, &painter);
    paintFindBar(chrome, &painter);
    paintStatus(chrome, &painter);

    uiPainterEnd(&painter);
}

/* ------------------------------------------------------------------------- */
/* hit testing / input                                                       */
/* ------------------------------------------------------------------------- */

static int buttonAt(UiButton* button, int x, int y)
{
    return button && button->visible && uiRectContains(button->rect, x, y);
}

UiPart uiChromeHitTest(UiChrome* chrome, int x, int y)
{
    UiPart part;
    int index;

    if (!chrome)
        return UI_PART_NONE;

    if (buttonAt(&chrome->win_min, x, y)) return UI_PART_WINDOW_MIN;
    if (buttonAt(&chrome->win_max, x, y)) return UI_PART_WINDOW_MAX;
    if (buttonAt(&chrome->win_close, x, y)) return UI_PART_WINDOW_CLOSE;

    part = uiTabStripHit(&chrome->tabstrip, x, y, &index);

    if (part != UI_PART_NONE)
        return part;

    if (buttonAt(&chrome->toolbar.back, x, y)) return UI_PART_BACK;
    if (buttonAt(&chrome->toolbar.forward, x, y)) return UI_PART_FORWARD;
    if (buttonAt(&chrome->toolbar.reload, x, y)) return UI_PART_RELOAD;
    if (buttonAt(&chrome->toolbar.go, x, y)) return UI_PART_GO;
    if (buttonAt(&chrome->toolbar.menu, x, y)) return UI_PART_MENU;

    if (chrome->model.find_visible) {
        if (buttonAt(&chrome->findbar.prev, x, y)) return UI_PART_FIND_PREV;
        if (buttonAt(&chrome->findbar.next, x, y)) return UI_PART_FIND_NEXT;
        if (buttonAt(&chrome->findbar.close, x, y)) return UI_PART_FIND_CLOSE;
        if (uiRectContains(chrome->findbar.field, x, y)) return UI_PART_FIND_FIELD;
    }

    if (uiRectContains(chrome->toolbar.address, x, y))
        return UI_PART_ADDRESS;

    if (uiRectContains(chrome->statusbar.rect, x, y))
        return UI_PART_STATUS;

    return UI_PART_NONE;
}

int uiChromeTabAt(UiChrome* chrome, int x, int y)
{
    int index = -1;

    if (!chrome)
        return -1;

    uiTabStripHit(&chrome->tabstrip, x, y, &index);

    return index;
}

static int feedButtons(UiChrome* chrome, const UiMouseEvent* event)
{
    int fire;
    int handled = 0;
    UiToolbar* bar = &chrome->toolbar;

    if (uiButtonMouse(&bar->back, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_BACK);
        handled = 1;
    }
    if (uiButtonMouse(&bar->forward, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_FORWARD);
        handled = 1;
    }
    if (uiButtonMouse(&bar->reload, event, &fire)) {
        if (fire) emitCommand(chrome, chrome->model.loading ? UI_CMD_STOP : UI_CMD_RELOAD);
        handled = 1;
    }
    if (uiButtonMouse(&bar->go, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_GO);
        handled = 1;
    }
    if (uiButtonMouse(&bar->menu, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_MENU);
        handled = 1;
    }

    if (uiButtonMouse(&chrome->win_min, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_MINIMIZE);
        handled = 1;
    }
    if (uiButtonMouse(&chrome->win_max, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_MAXIMIZE);
        handled = 1;
    }
    if (uiButtonMouse(&chrome->win_close, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_CLOSE_WINDOW);
        handled = 1;
    }

    return handled;
}

static int feedFindButtons(UiChrome* chrome, const UiMouseEvent* event)
{
    int fire;
    int handled = 0;
    UiFindBar* bar = &chrome->findbar;

    if (!chrome->model.find_visible)
        return 0;

    if (uiButtonMouse(&bar->prev, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_FIND_PREV);
        handled = 1;
    }
    if (uiButtonMouse(&bar->next, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_FIND_NEXT);
        handled = 1;
    }
    if (uiButtonMouse(&bar->close, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_FIND_CLOSE);
        handled = 1;
    }

    return handled;
}

static void updateToolbarModel(UiChrome* chrome)
{
    chrome->toolbar.back.enabled = chrome->model.can_back;
    chrome->toolbar.forward.enabled = chrome->model.can_forward;
    chrome->toolbar.menu.enabled = 1;
    chrome->toolbar.go.enabled = 1;
    chrome->toolbar.reload.enabled = 1;

    chrome->win_min.enabled = 1;
    chrome->win_max.enabled = 1;
    chrome->win_max.icon = chrome->model.maximized ? UI_ICON_RESTORE : UI_ICON_MAXIMIZE;
    chrome->win_close.enabled = 1;
}

int uiChromeMouse(UiChrome* chrome, const UiMouseEvent* event)
{
    UiPart part;

    if (!chrome || !event)
        return 0;

    part = uiChromeHitTest(chrome, event->x, event->y);

    switch (event->type) {
    case UI_MOUSE_MOVE:
        chrome->hover_part = part;
        chrome->mouse_inside = part != UI_PART_NONE;
        uiTabStripMouse(&chrome->tabstrip, &chrome->model, event, NULL);
        feedButtons(chrome, event);
        feedFindButtons(chrome, event);
        return chrome->mouse_inside;

    case UI_MOUSE_LEAVE:
        chrome->hover_part = UI_PART_NONE;
        chrome->mouse_inside = 0;
        uiTabStripLeave(&chrome->tabstrip);
        uiButtonReset(&chrome->toolbar.back);
        uiButtonReset(&chrome->toolbar.forward);
        uiButtonReset(&chrome->toolbar.reload);
        uiButtonReset(&chrome->toolbar.go);
        uiButtonReset(&chrome->toolbar.menu);
        uiButtonReset(&chrome->findbar.prev);
        uiButtonReset(&chrome->findbar.next);
        uiButtonReset(&chrome->findbar.close);
        return 0;

    case UI_MOUSE_DOWN:
        chrome->hover_part = part;

        if (part == UI_PART_TAB || part == UI_PART_TAB_CLOSE || part == UI_PART_TAB_NEW) {
            uiTabStripMouse(&chrome->tabstrip, &chrome->model, event, NULL);
            return 1;
        }

        if (feedButtons(chrome, event))
            return 1;

        if (feedFindButtons(chrome, event))
            return 1;

        if (part == UI_PART_ADDRESS) {
            emitFocus(chrome, 1);
            return 1;
        }

        if (part == UI_PART_FIND_FIELD) {
            emitFocus(chrome, 2);
            return 1;
        }

        if (part == UI_PART_STATUS)
            return 1;

        return 0;

    case UI_MOUSE_UP: {
        UiTabAction action;
        int handled = 0;

        memset(&action, 0, sizeof(action));
        action.activate = -1;
        action.close = -1;

        if (chrome->tabstrip.pressed_tab >= 0 ||
            chrome->tabstrip.pressed_close >= 0 ||
            chrome->tabstrip.pressed_new_tab) {
            uiTabStripMouse(&chrome->tabstrip, &chrome->model, event, &action);

            if (action.close >= 0 && chrome->cb.close_tab)
                chrome->cb.close_tab(chrome->cb.user, action.close);
            else if (action.activate >= 0 && chrome->cb.activate_tab)
                chrome->cb.activate_tab(chrome->cb.user, action.activate);
            else if (action.new_tab && chrome->cb.new_tab)
                chrome->cb.new_tab(chrome->cb.user);

            handled = 1;
        }

        if (feedButtons(chrome, event))
            handled = 1;
        if (feedFindButtons(chrome, event))
            handled = 1;

        if (part == UI_PART_ADDRESS || part == UI_PART_FIND_FIELD || part == UI_PART_STATUS)
            handled = 1;

        return handled;
    }

    default:
        return 0;
    }
}

/* The model's enabled state is derived from can_back/forward; keep the button
 * structs in sync whenever the shell refreshes the snapshot. */
void uiChromeSync(UiChrome* chrome)
{
    updateToolbarModel(chrome);
}
