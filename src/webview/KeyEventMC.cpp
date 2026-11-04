/*
** PlatformKeyEvent.cpp: Keyboard Event implements.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/

#include "config.h"
#include "macross.h"
#include "PlatformKeyboardEvent.h"
#include <wtf/ASCIICType.h>

namespace WebCore {

static String keyIdentifierForWindowsKeyCode(unsigned short keyCode)
{
    switch (keyCode) {
        case KEY_ALT:
            return "Alt";
		case KEY_TAB:
			return "Tab";
		case KEY_DOWN:
            return "Down";
        case KEY_END:
            return "End";
        case KEY_ENTER:
            return "Enter";
        case KEY_SPACE:
            return "Space";
		case KEY_SHIFT:
			return "Shift";
        case KEY_F1:
            return "F1";
        case KEY_F2:
            return "F2";
        case KEY_F3:
            return "F3";
        case KEY_F4:
            return "F4";
        case KEY_F5:
            return "F5";
        case KEY_F6:
            return "F6";
        case KEY_F7:
            return "F7";
        case KEY_F8:
            return "F8";
        case KEY_F9:
            return "F9";
        case KEY_F10:
            return "F11";
        case KEY_F12:
            return "F12";
        case KEY_HOME:
            return "Home";
        case KEY_INSERT:
            return "Insert";
        case KEY_LEFT:
            return "Left";
        case KEY_PAGEDOWN:
            return "PageDown";
        case KEY_PAGEUP:
            return "PageUp";
        case KEY_PAUSE:
            return "Pause";
        case KEY_RIGHT:
            return "Right";
        case KEY_UP:
            return "Up";
        case KEY_DELETE:
            return "Remove";
        default:
            break;
    }

    /* Numpad digits report VK_NUMPAD0..9 (0x60..0x69) on Windows. Those codes
     * are not characters - VK_NUMPAD1 is 0x61, i.e. 'a' - so they must be
     * mapped before the generic branch below turns them into "U+0061". */
    if (keyCode >= 0x60 && keyCode <= 0x69)
        return String::format("U+003%c", (char)('0' + (keyCode - 0x60)));

    return String::format("U+%04X", WTF::toASCIIUpper(keyCode));
}

PlatformKeyboardEvent::PlatformKeyboardEvent(bool isDown, unsigned code, unsigned flags)
    /* No text: a Win32 virtual key code is not a character. The character
     * arrives separately in WM_CHAR, which dispatches the keypress event and
     * inserts the text (see WebView::textInput / EventHandler::handleCharacterEvent).
     * Deriving it from the vk here used to insert the wrong character for
     * every key whose vk is not its character (numpad 1 -> VK_NUMPAD1 == 0x61
     * -> "a") and, together with WM_CHAR, inserted every character twice. */
    : m_text(String())
    , m_unmodifiedText(String())
    , m_keyIdentifier(keyIdentifierForWindowsKeyCode(code))
    , m_isKeyUp(!isDown)
    , m_autoRepeat((flags & 16) ? true : false)
    , m_WindowsKeyCode(code)
    , m_isKeypad(code >= 0x60 && code <= 0x69)
    , m_shiftKey(flags & 4) // shift
    , m_ctrlKey(flags & 2) // ctrl
    , m_altKey(flags & 1) // alt
    , m_metaKey(flags & 8) // meta
{

}

}

