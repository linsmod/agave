/*
** PopupMenu.cpp: PopupMenu implements.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/

#include "config.h"
#include "PopupMenu.h"
#include "Document.h"
#include "FloatRect.h"
#include "FontData.h"
#include "FontSelector.h"
#include "Frame.h"
#include "FrameView.h"
#include "GraphicsContext.h"
#include "HTMLNames.h"
#include "Page.h"
#include "PlatformMouseEvent.h"
#include "PlatformScreen.h"
#include "PlatformScrollBar.h"
#include "RenderTheme.h"
#include "RenderView.h"
#include "TextStyle.h"
#include "Debug.h"

#include "WebCallback.h"
#include "ChromeClientMC.h"
#include "WebView.h"
#include "WebEngine.h"

namespace WebCore {

using namespace std;
using namespace HTMLNames;
using namespace MaCross;

// Maximum height of a popup window
static const int maxPopupHeight = 320;

static const int optionSpacingMiddle = 1;
static const int popupWindowBorderWidth = 1;
static const int endOfLinePadding = 2;
static const int separatorPadding = 4;
static const int separatorHeight = 1;


PopupMenu::PopupMenu(PopupMenuClient* client)
    : m_popupClient(client)
    , m_scrollBar(0)
    , m_popup(0)
    , m_ownedPopup(0)
    , m_webView(0)
	, m_view(0)
    , m_wasClicked(false)
    , m_itemHeight(0)
    , m_scrollOffset(0)
    , m_wheelDelta(0)
    , m_focusedIndex(0)
    , m_scrollbarCapturingMouse(false)
{
	m_view = new _mc_view;
	m_view->view = reinterpret_cast<WebView*>(this);
	m_view->data = 0;
}

PopupMenu::~PopupMenu()
{
    if (m_webView && m_webView->activePopup() == this)
        m_webView->setActivePopup(0);
	delete m_view;
    if (m_popup && cb_destroy_popup_menu && !m_ownedPopup)
      	cb_destroy_popup_menu(m_popup);
    delete m_ownedPopup;
}

void PopupMenu::updateRect(int x, int y, int width, int height)
{
	if (m_popup->dirty) {
    	_mc_rect r = {x, y, width, height}; 
    	m_popup->dirty(m_popup, &r);
	}
}

void PopupMenu::scrollRect(int sx, int sy, int x, int y, int width, int height)
{
    updateRect(x, y, width, height);
}

/* ------------------------------------------------------------------------- */
/* in-view popup: the engine owns the _mc_popup_menu, the view paints it       */
/* ------------------------------------------------------------------------- */

static WebView* webViewForFrameView(FrameView* v)
{
    if (!v || !v->frame() || !v->frame()->page())
        return 0;

    ChromeClientMC* client = static_cast<ChromeClientMC*>(v->frame()->page()->chrome()->client());
    return client ? client->webView() : 0;
}

void PopupMenu::invalidateAll()
{
    if (!m_webView)
        return;

    const IntRect& r = m_windowRect;
    m_webView->updateRect(r.x(), r.y(), r.width(), r.height());
}

static void ownedShow(_mc_popup_menu* m)
{
    PopupMenu* popup = static_cast<PopupMenu*>(m->handle);
    if (popup)
        popup->invalidateAll();
}

static void ownedHide(_mc_popup_menu* m)
{
    PopupMenu* popup = static_cast<PopupMenu*>(m->handle);
    if (!popup)
        return;

    if (popup->view() && popup->view()->activePopup() == popup)
        popup->view()->setActivePopup(0);
    popup->invalidateAll();
}

static void ownedMove(_mc_popup_menu* m, const MC_RECT* r)
{
    m->rect = *r;
}

static void ownedCapture(_mc_popup_menu* m)
{
}

static void ownedUpdate(_mc_popup_menu* m)
{
    PopupMenu* popup = static_cast<PopupMenu*>(m->handle);
    if (popup && popup->view())
        popup->view()->updateViewNow();
}

static void ownedDirty(_mc_popup_menu* m, const MC_RECT* r)
{
    PopupMenu* popup = static_cast<PopupMenu*>(m->handle);
    if (!popup || !popup->view())
        return;

    if (!r) {
        popup->invalidateAll();
        return;
    }

    const IntRect& pr = popup->windowRect();
    popup->view()->updateRect(pr.x() + r->x, pr.y() + r->y, r->w, r->h);
}

bool PopupMenu::createOwnPopup(FrameView* v)
{
    m_webView = webViewForFrameView(v);
    if (!m_webView)
        return false;

    m_ownedPopup = new _mc_popup_menu;
    memset(m_ownedPopup, 0, sizeof(_mc_popup_menu));
    m_ownedPopup->handle = this;
    m_ownedPopup->show = ownedShow;
    m_ownedPopup->hide = ownedHide;
    m_ownedPopup->move = ownedMove;
    m_ownedPopup->capture = ownedCapture;
    m_ownedPopup->update = ownedUpdate;
    m_ownedPopup->dirty = ownedDirty;
    m_popup = m_ownedPopup;
    return true;
}

void PopupMenu::show(const IntRect& r, FrameView* v, int index)
{
	calculatePositionAndSize(r, v);
	if (clientRect().isEmpty())
    	return;

    if (!m_popup) {
		_mc_rect rc = {r.x(), r.y(), r.width(), r.height()};

		if (cb_create_popup_menu) {
			m_popup = cb_create_popup_menu(&rc);
			if (!m_popup)
				return;
			m_popup->handle = this;
		} else if (!createOwnPopup(v)) {
			/* No host window and no view to draw into: tell the client the
			 * popup is not open, or the <select> stays "open" forever and the
			 * next click only toggles the flag (that was the dead dropdown). */
			if (m_popupClient)
				m_popupClient->hidePopup();
			return;
		}
	}

	if (!m_scrollBar)
		if (visibleItems() < client()->listSize()) {
		// We need a scroll bar
			m_scrollBar = new PlatformScrollbar(this, VerticalScrollbar, SmallScrollbar);
			m_scrollBar->setContainingView(m_view);
		}

	if (m_popup->move) {
		_mc_rect mrc = {m_windowRect.x(), m_windowRect.y(), 
										m_windowRect.width(), m_windowRect.height()};
		m_popup->move(m_popup, &mrc);
	}

	if (m_popup->show)
   		m_popup->show(m_popup);

	if (m_popup->capture)
   		m_popup->capture(m_popup);

	if (client()) {
		int index = client()->selectedIndex();
		if (index >= 0)
			setFocusedIndex(index);
	}

	if (m_ownedPopup) {
		resizeScrollbar(m_windowRect.width(), m_windowRect.height());
		m_webView->setActivePopup(this);
		invalidateAll();
	}
}

void PopupMenu::resizeScrollbar(int width, int height)
{
    if (!m_scrollBar)
        return;

    m_scrollBar->setRect(IntRect(width - m_scrollBar->width(), 0,
                                m_scrollBar->width(), height));
    int visibleItems = this->visibleItems();
    m_scrollBar->setEnabled(visibleItems < client()->listSize());
    m_scrollBar->setSteps(1, max(1, visibleItems - 1));
    m_scrollBar->setProportion(visibleItems, client()->listSize());
}

void PopupMenu::hide()
{
    if(m_popup && m_popup->hide)
      m_popup->hide(m_popup);
}

void PopupMenu::calculatePositionAndSize(const IntRect& r, FrameView* v)
{
    // rect is in absolute document coordinates, but we want to be in screen coordinates
    IntRect rScreenCoords(v->contentsToWindow(r.location()), r.size());

    // First, determine the popup's height
    int itemCount = client()->listSize();
    m_itemHeight = client()->clientStyle()->font().height() + optionSpacingMiddle;
    int naturalHeight = m_itemHeight * itemCount;
    int popupHeight = min(maxPopupHeight, naturalHeight);
    // The popup should show an integral number of items (i.e. no partial items should be visible)
    popupHeight -= popupHeight % m_itemHeight;
    
    // Next determine its width
    int popupWidth = 0;
    for (int i = 0; i < itemCount; ++i) {
        String text = client()->itemText(i);
        if (text.isEmpty())
            continue;

        popupWidth = max(popupWidth, client()->clientStyle()->font().width(TextRun(text.characters(), text.length())));
    }

    if (naturalHeight > maxPopupHeight)
        // We need room for a scrollbar
        popupWidth += PlatformScrollbar::verticalScrollbarWidth(SmallScrollbar);

    // Add padding to align the popup text with the <select> text
    // Note: We can't add paddingRight() because that value includes the width
    // of the dropdown button, so we must use our own endOfLinePadding constant.
    popupWidth += endOfLinePadding + client()->clientPaddingLeft();

    // Leave room for the border
    popupWidth += 2 * popupWindowBorderWidth;
    popupHeight += 2 * popupWindowBorderWidth;

    // The popup should be at least as wide as the control on the page
    popupWidth = max(rScreenCoords.width(), popupWidth);

    // Always left-align items in the popup.  This matches popup menus on the mac.
    int popupX = rScreenCoords.x();

    IntRect popupRect(popupX, rScreenCoords.bottom(), popupWidth, popupHeight);
    m_windowRect = popupRect;
    return;
}

bool PopupMenu::setFocusedIndex(int i, bool hotTracking)
{
    if (i < 0 || i >= client()->listSize() || i == focusedIndex())
        return false;

    if (!client()->itemIsEnabled(i))
        return false;

    if (!m_popup)
		return false;

    invalidateItem(focusedIndex());
    invalidateItem(i);

    m_focusedIndex = i;

    if (!hotTracking)
        client()->setTextFromItem(i);

    if (!scrollToRevealSelection() && m_popup->update)
        m_popup->update(m_popup);

    return true;
}

int PopupMenu::visibleItems() const
{
    return clientRect().height() / m_itemHeight;
}

int PopupMenu::listIndexAtPoint(const IntPoint& point) const
{
    return m_scrollOffset + point.y() / m_itemHeight;
}

int PopupMenu::focusedIndex() const
{
    return m_focusedIndex;
}

void PopupMenu::focusFirst()
{
    if (!client())
        return;

    int size = client()->listSize();

    for (int i = 0; i < size; ++i)
        if (client()->itemIsEnabled(i)) {
            setFocusedIndex(i);
            break;
        }
}

void PopupMenu::focusLast()
{
    if (!client())
        return;

    int size = client()->listSize();

    for (int i = size - 1; i > 0; --i)
        if (client()->itemIsEnabled(i)) {
            setFocusedIndex(i);
            break;
        }
}

bool PopupMenu::down(unsigned lines)
{
    if (!client())
        return false;

    int size = client()->listSize();

    int lastSelectableIndex, selectedListIndex;
    lastSelectableIndex = selectedListIndex = focusedIndex();
    for (int i = selectedListIndex + 1; i >= 0 && i < size; ++i)
        if (client()->itemIsEnabled(i)) {
            lastSelectableIndex = i;
            if (i >= selectedListIndex + (int)lines)
                break;
        }

    return setFocusedIndex(lastSelectableIndex);
}

bool PopupMenu::up(unsigned lines)
{
    if (!client())
        return false;

    int size = client()->listSize();

    int lastSelectableIndex, selectedListIndex;
    lastSelectableIndex = selectedListIndex = focusedIndex();
    for (int i = selectedListIndex - 1; i >= 0 && i < size; --i)
        if (client()->itemIsEnabled(i)) {
            lastSelectableIndex = i;
            if (i <= selectedListIndex - (int)lines)
                break;
        }

    return setFocusedIndex(lastSelectableIndex);
}

void PopupMenu::invalidateItem(int index)
{
    if (!m_popup)
        return;

    IntRect damageRect(clientRect());
    damageRect.setY(m_itemHeight * (index - m_scrollOffset));
    damageRect.setHeight(m_itemHeight);
    if (m_scrollBar)
        damageRect.setWidth(damageRect.width() - m_scrollBar->frameGeometry().width());

    if (m_popup->dirty) {
    	_mc_rect r = {damageRect.x(), damageRect.y(), damageRect.width(), damageRect.height()}; 
    	m_popup->dirty(m_popup, &r);
	}
}

IntRect PopupMenu::clientRect() const
{
    IntRect clientRect = m_windowRect;
    clientRect.inflate(-popupWindowBorderWidth);
    clientRect.setLocation(IntPoint(0, 0));
    return clientRect;
}

void PopupMenu::incrementWheelDelta(int delta)
{
    m_wheelDelta += delta;
}

void PopupMenu::reduceWheelDelta(int delta)
{
    if (m_wheelDelta > 0)
        m_wheelDelta -= delta;
    else if (m_wheelDelta < 0)
        m_wheelDelta += delta;
    else
        return;
}

bool PopupMenu::scrollToRevealSelection()
{
    if (!m_scrollBar)
        return false;

    int index = focusedIndex();

    if (index < m_scrollOffset) {
        m_scrollBar->setValue(index);
        return true;
    }

    if (index >= m_scrollOffset + visibleItems()) {
        m_scrollBar->setValue(index - visibleItems() + 1);
        return true;
    }

    return false;
}

void PopupMenu::updateFromElement()
{
    if (!m_popup)
        return;

    if (m_popup->dirty)
 		m_popup->dirty(m_popup, 0);

    if (!scrollToRevealSelection() && m_popup->update)
        m_popup->update(m_popup);
}

bool PopupMenu::itemWritingDirectionIsNatural()
{
    return true;
}

void PopupMenu::paint(const IntRect& damageRect, GraphicsContext* context)
{
    IntRect listRect = damageRect;
    listRect.move(IntSize(0, m_scrollOffset * m_itemHeight));

    for (int y = listRect.y(); y < listRect.bottom(); y += m_itemHeight) {
        int index = y / m_itemHeight;

        if (index >= client()->listSize())
            break;

        Color optionBackgroundColor, optionTextColor;
        RenderStyle* itemStyle = client()->itemStyle(index);
        if (index == focusedIndex()) {
            optionBackgroundColor = theme()->activeListBoxSelectionBackgroundColor();
            optionTextColor = theme()->activeListBoxSelectionForegroundColor();
        } else {
            optionBackgroundColor = client()->itemBackgroundColor(index);
            optionTextColor = itemStyle->color();
        }

        // itemRect is in client coordinates
        IntRect itemRect(0, (index - m_scrollOffset) * m_itemHeight, damageRect.width(), m_itemHeight);

        // Draw the background for this menu item
        if (itemStyle->visibility() != HIDDEN)
            context->fillRect(itemRect, optionBackgroundColor);

        if (client()->itemIsSeparator(index)) {
            IntRect separatorRect(itemRect.x() + separatorPadding, itemRect.y() + (itemRect.height() - separatorHeight) / 2, itemRect.width() - 2 * separatorPadding, separatorHeight);
            context->fillRect(separatorRect, optionTextColor);
            continue;
        }

        String itemText = client()->itemText(index);
            
        unsigned length = itemText.length();
        const UChar* string = itemText.characters();
        TextStyle textStyle(0, 0, 0, itemText.defaultWritingDirection() == WTF::Unicode::RightToLeft);
        TextRun textRun(string, length);

        context->setFillColor(optionTextColor);
        
        Font itemFont = client()->clientStyle()->font();
        if (client()->itemIsLabel(index)) {
            FontDescription d = itemFont.fontDescription();
            d.setBold(true);
            itemFont = Font(d, itemFont.letterSpacing(), itemFont.wordSpacing());
            itemFont.update(m_popupClient->fontSelector());
        }
        context->setFont(itemFont);
        
        // Draw the item text
        if (itemStyle->visibility() != HIDDEN) {
            int textX = client()->clientPaddingLeft();
            int textY = itemRect.y() + itemFont.ascent() + (itemRect.height() - itemFont.height()) / 2;
            context->drawBidiText(textRun, IntPoint(textX, textY), textStyle);
        }
    }

    if (m_scrollBar)
        m_scrollBar->paint(context, damageRect);
}

void PopupMenu::valueChanged(Scrollbar* scrollBar)
{
    if (!m_popup)
        return;

    int offset = scrollBar->value();

    if (m_scrollOffset == offset)
        return;

    m_scrollOffset = offset;

    IntRect listRect = clientRect();
    if (m_scrollBar)
        listRect.setWidth(listRect.width() - m_scrollBar->frameGeometry().width());

    if (m_scrollBar&&m_popup->dirty) {
        IntRect r = m_scrollBar->frameGeometry();
		r.unite(listRect);
		_mc_rect rt = {r.x(), r.y(), r.width(), r.height()};
        m_popup->dirty(m_popup, &rt);
    }
	if (m_popup->update)
    	m_popup->update(m_popup);
}

IntRect PopupMenu::windowClipRect() const
{
    return m_windowRect;
}

IntRect PopupMenu::windowClientRect() const
{
	if (m_popup)
		return IntRect(0, 0, m_popup->rect.w, m_popup->rect.h);	
	return IntRect();
}

/* Mouse handling shared by both modes. Points are in popup-local coordinates
 * (the host window's client area, or the view point minus windowRect() for the
 * in-view popup). Moved here from macross_menu_mouse_event so the overlay can
 * reuse it. */
void PopupMenu::handleMouseEvent(const MC_MOUSE_EVENT& event)
{
    if (!m_popup)
        return;

    switch (event.type) {
        case EVT_MOUSE_MOVE:
            {
                IntPoint mousePoint(event.point.x, event.point.y);
                if (m_scrollBar) {
                    IntRect scrollBarRect = m_scrollBar->frameGeometry();
                    if (m_scrollbarCapturingMouse || scrollBarRect.contains(mousePoint)) {
                        // Put the point into coordinates relative to the scroll bar
                        mousePoint.move(-scrollBarRect.x(), -scrollBarRect.y());
                        PlatformMouseEvent mevent(MouseEventMoved,
                                    _get_mouse_button(event.button), mousePoint, event.modifier);
                        m_scrollBar->handleMouseMoveEvent(mevent);
                        break;
                    }
                }

                IntRect bounds = windowClientRect();
                if (bounds.contains(mousePoint.x(), mousePoint.y()))
                    setFocusedIndex(listIndexAtPoint(mousePoint), true);
            }
            break;
        case EVT_MOUSE_UP:
            {
                IntPoint mousePoint(event.point.x, event.point.y);
                if (m_scrollBar) {
                    IntRect scrollBarRect = m_scrollBar->frameGeometry();
                    if (m_scrollbarCapturingMouse || scrollBarRect.contains(mousePoint)) {
                        setScrollbarCapturingMouse(false);
                        // Put the point into coordinates relative to the scroll bar
                        mousePoint.move(-scrollBarRect.x(), -scrollBarRect.y());
                        PlatformMouseEvent mevent(MouseEventReleased,
                                    _get_mouse_button(event.button), mousePoint, event.modifier);
                        m_scrollBar->handleMouseReleaseEvent(mevent);
                        // FIXME: This is a hack to work around PlatformScrollbar not
                        // invalidating correctly when it doesn't have a parent widget
                        IntRect r = scrollBarRect;
                        updateRect(r.x(), r.y(), r.width(), r.height());
                        break;
                    }
                }
                IntRect bounds = windowClientRect();
                if (client() && bounds.contains(mousePoint.x(), mousePoint.y())) {
                    client()->hidePopup();
                    int index = focusedIndex();
                    if (index >= 0)
                        client()->valueChanged(index);
                }
            }
            break;
        case EVT_MOUSE_DOWN:
            {
                IntPoint mousePoint(event.point.x, event.point.y);
                if (m_scrollBar) {
                    IntRect scrollBarRect = m_scrollBar->frameGeometry();
                    if (scrollBarRect.contains(mousePoint)) {
                        // Put the point into coordinates relative to the scroll bar
                        mousePoint.move(-scrollBarRect.x(), -scrollBarRect.y());
                        PlatformMouseEvent mevent(MouseEventPressed,
                                    _get_mouse_button(event.button), mousePoint, event.modifier);
                        m_scrollBar->handleMousePressEvent(mevent);
                        setScrollbarCapturingMouse(true);
                        break;
                    }
                }

                IntRect bounds = windowClientRect();
                if (client() && bounds.contains(mousePoint.x(), mousePoint.y()))
                    setFocusedIndex(listIndexAtPoint(mousePoint), true);
                else if (client())
                    client()->hidePopup();
            }
            break;
        default:
            break;
    }
}

void PopupMenu::handleKeyEvent(const MC_KEY_EVENT& event)
{
    if (!client() || event.type != EVT_KEY_DOWN)
        return;

    switch (event.key) {
        case KEY_UP:
            up();
            break;
        case KEY_DOWN:
            down();
            break;
        case KEY_HOME:
            focusFirst();
            break;
        case KEY_END:
            focusLast();
            break;
        case KEY_PAGEUP:
            {
                if (focusedIndex() != scrollOffset()) {
                    int firstVisibleItem = scrollOffset();
                    up(focusedIndex() - firstVisibleItem);
                } else
                    up(visibleItems());
            }
            break;
        case KEY_PAGEDOWN:
            {
                int lastVisibleItem = scrollOffset() + visibleItems() - 1;
                if (focusedIndex() != lastVisibleItem) {
                    down(lastVisibleItem - focusedIndex());
                } else
                    down(visibleItems());
            }
            break;
        case KEY_ENTER:
            {
                client()->hidePopup();
                int index = focusedIndex();
                client()->valueChanged(index);
            }
            break;
        case KEY_ESCAPE:
            client()->hidePopup();
            break;
        default:
            break;
    }
}

}

// vim: ts=4 sw=4 et
