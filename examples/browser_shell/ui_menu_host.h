/*
 * ui_menu_host.h - modal Win32 host for ui_menu.
 */

#ifndef AGAVE_UI_MENU_HOST_H
#define AGAVE_UI_MENU_HOST_H

#include "ui_menu.h"

/*
 * Shows `root` as a layered top-level popup anchored at screen (x, y) and
 * runs it modally (its own message loop, like TrackPopupMenu did). Returns
 * the chosen item id, or 0 when the menu was dismissed.
 *
 * `align` picks which panel edge sits at the anchor: LEFT for context menus
 * at the pointer, RIGHT for the app menu under the toolbar button.
 */
int uiMenuHostRun(const UiTheme* theme, const UiMenu* root, int x, int y,
                  UiMenuAlign align, HWND owner);

#endif /* AGAVE_UI_MENU_HOST_H */
