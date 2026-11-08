/*
** webview.cpp: webview implements.
**
** Copyright(C) 2009 Zhang Ji Peng (onecoolx@gmail.com)
**
*/
#include "config.h"

#include <wtf/StringExtras.h>
#include <picasso/picasso.h>

#include "Page.h"
#include "Frame.h"
#include "PopupMenu.h"
#include "HTMLInputElement.h"
#include "HTMLTextAreaElement.h"
#include "HTMLNames.h"
#include "Document.h"
#include "Node.h"
#include "RenderObject.h"
#include <stdio.h>
#include "FrameTree.h"
#include "FrameView.h"
#include "FrameLoader.h"
#include "DocumentLoader.h"
#include "Editor.h"
#include "EventHandler.h"
#include "GraphicsContext.h"
#include "Settings.h"
#include "ContextMenu.h"
#include "SchemeExtension.h"
#include "SubstituteData.h"

#include "ChromeClientMC.h"
#include "ContextMenuClientMC.h"
#include "EditorClientMC.h"
#include "DragClientMC.h"
#include "InspectorClientMC.h"
#include "FrameLoaderClientMC.h"
#include "FocusController.h"
#include "GCController.h"

#include "WebView.h"
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include "WebEngine.h"
#include "WebEncoding.h"
#include "WebCallback.h"

using namespace WTF;
using namespace WebCore;

namespace WebCore {
extern ps_color_format pixelFormat(void);
extern ps_context* getGlobalContext(void);
}

namespace MaCross {

static void garbageRelease(void)
{
	WebCore::gcController().garbageCollectSoon();
}

class WebViewPrivate
{
public:
	WebViewPrivate(struct _mc_view* v, unsigned char* buf, int w, int h, int p)
		: m_view(v), m_buffer(buf), m_width(w), m_height(h), m_mode(0), m_txtanti(false)
		, m_canvas(0), m_context(0), m_drawRect(0, 0, 0, 0), m_minLayoutSize(0, 0), m_oldLayoutSize(0, 0)
		, m_activePopup(0), m_swallowNextText(false)
	{
		m_canvas = ps_canvas_create_with_data(buf, pixelFormat(), w, h, p);
		m_context = ps_context_create(m_canvas, 0);
		ps_set_filter(m_context, FILTER_NEAREST);
	}

	~WebViewPrivate()
	{
		ps_context_unref(m_context);
		ps_canvas_unref(m_canvas);
	}

	void resizeView(unsigned char* buf, int w, int h, int p)
	{
		ps_context_unref(m_context);
		ps_canvas_unref(m_canvas);
		
		m_buffer = buf;
		m_width = w;
		m_height = h;

		m_canvas = ps_canvas_create_with_data(buf, pixelFormat(), w, h, p);
		m_context = ps_context_create(m_canvas, 0);
		ps_set_filter(m_context, FILTER_NEAREST);
	}

	struct _mc_view* m_view;
	unsigned char* m_buffer;
	int m_width;
	int m_height;
	int m_mode;
	bool m_txtanti;
	ps_canvas* m_canvas;
	ps_context* m_context;
	// webkit object
	IntRect m_drawRect;
	IntSize m_minLayoutSize;
	IntSize m_oldLayoutSize;
    RefPtr<Frame> m_frame;
    RefPtr<FrameView> m_frameView;
    /* Open <select> menu, painted over the page (see setActivePopup). */
    PopupMenu* m_activePopup;
    /* A keydown the page cancelled (preventDefault) must not turn into text:
     * Win32 delivers the character in a separate WM_CHAR, long after the page
     * said no. Set by keyDown(), consumed by textInput(). */
    bool m_swallowNextText;
};

static Frame *incrementFrame(Frame *curr, bool forward, bool wrapFlag)
{
    return forward ? curr->tree()->traverseNextWithWrap(wrapFlag)
                   : curr->tree()->traversePreviousWithWrap(wrapFlag);
}


// webview 
WebView::WebView(struct _mc_view* view, unsigned char* buffer, int width, int height, int pitch)
	:d(new WebViewPrivate(view, buffer, width, height, pitch))
{
    Settings::init(); 
	ps_set_text_antialias(d->m_context, isTextAntialias() ? True : False);
	d->m_txtanti = isTextAntialias();
	//FIXME: Chrome 's width and height is not view , is window
#if ENABLE(INSPECTOR)
    Page* page = new Page(new ChromeClientMC(view, width, height), new ContextMenuClientMC,
                          new EditorClientMC(view), new DragClientMC, new InspectorClientMC);
#else
    Page* page = new Page(new ChromeClientMC(view, width, height), new ContextMenuClientMC,
                          new EditorClientMC(view), new DragClientMC);
#endif
    FrameLoaderClientMC *client = new FrameLoaderClientMC;
    Frame* frame = new Frame(page, 0, client);
	client->setFrame(frame);
	d->m_frame = frame;

	FrameView* frameView = new FrameView(frame);
	frameView->resize(width, height);//set view size
	d->m_frameView = frameView;
    d->m_frame->setView(frameView);
    d->m_frameView->setContainingView(view);
    frameView->deref();

    d->m_frame->init();
    d->m_frame->page()->setGroupName(String("PageGroup"));
}

WebView::~WebView()
{
    stop();
    Page* page = d->m_frame->page();
    Frame *frame = page->mainFrame();
    if (frame)
        frame->loader()->detachFromParent();
    delete page;
	garbageRelease();
}

Frame * WebView::mainframe(void)
{
    return d->m_frame.get();
}

Page * WebView::page(void)
{
    Frame* frame = d->m_frame.get();

    return frame ? frame->page() : 0;
}

bool WebView::mainDocumentTitle(String& out)
{
    Frame* frame = d->m_frame.get();

    if (!frame || !frame->document())
        return false;

    out = frame->document()->title();
    return !out.isEmpty();
}

void WebView::resizeView(unsigned char* buffer, int width, int height, int pitch)
{
	if ((d->m_buffer != buffer) || (d->m_width != width) || (d->m_height != height)) {
		d->resizeView(buffer, width, height, pitch);
		ps_set_text_antialias(d->m_context, isTextAntialias() ? True : False);
		d->m_txtanti = isTextAntialias();
	}
    d->m_frameView->resize(width, height);
    /* The resize event is NOT sent here: this runs before the layout has picked
     * up the new size, so the page would see the old viewport. FrameView::layout
     * sends it after laying out, once per size change (and swallows a burst of
     * WM_SIZEs into that one layout). */
}

void WebView::scrollRect(int sx, int sy, int x, int y, int width, int height)
{
	updateRect(x, y, width, height);
}

void WebView::updateRect(int x, int y, int width, int height)
{
	/* The host surface is exactly the viewport, so anything outside it cannot be
	 * shown. Long documents do invalidate their whole height (css3test asked for
	 * 1096x21593 on every frame), and that rect drives both the paint walk and
	 * the tiled-background fill - i.e. one frame cost grew with the document
	 * instead of the window. */
	IntRect rect(x, y, width, height);

	rect.intersect(IntRect(0, 0, d->m_width, d->m_height));
	if (rect.isEmpty())
		return;

	d->m_drawRect.unite(rect);

	if (cb_invalidate_rect) {
		IntRect rc = d->m_drawRect;
		_mc_rect r = {rc.x(), rc.y(), rc.width(), rc.height()};
		cb_invalidate_rect(d->m_view, &r);
	}
}

void WebView::setDirtyRect(int x, int y, int width, int height)
{
	d->m_drawRect.unite(IntRect(x, y, width, height));
}

_mc_size WebView::getSize(void) const
{
	_mc_size size = {d->m_width, d->m_height};
	return size;
}

void WebView::setMinLayoutSize(int w, int h)
{
	d->m_minLayoutSize = IntSize(w, h);
	d->m_frameView->setNeedsLayout();
	d->m_frameView->resize(w, h);
	reflow();
}

void WebView::clearMinLayoutSize(void)
{
	if (!d->m_minLayoutSize.isEmpty()) {
		d->m_minLayoutSize = IntSize(0, 0);
		d->m_oldLayoutSize = IntSize(0, 0);
		d->m_frameView->setNeedsLayout();
		d->m_frameView->resize(d->m_width, d->m_height);
		reflow();
	}
}

void WebView::contentsPosChanged(int old_x, int old_y, int new_x, int new_y)
{
	if (cb_position_changed)
		cb_position_changed(d->m_view, old_x, old_y, new_x, new_y);
}

void WebView::updateViewNow(void)
{
	if (cb_update_view_now)
		cb_update_view_now(d->m_view);
}

void WebView::paintView(void)
{
	Settings* setting = Settings::globalSettings();
	if (d->m_txtanti != setting->textAntialias()) {
		ps_set_text_antialias(d->m_context, isTextAntialias() ? True : False);
		d->m_txtanti = isTextAntialias();
	}

	/* Split timing: layout and paint are the two places a single frame can
	 * stall for seconds on rule-heavy pages. Logged only when slow. */
	{
		LARGE_INTEGER freq, t0, t1, t2;
		double layoutMs = 0.0;
		double paintMs = 0.0;

		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);

		d->m_frameView->layoutIfNeededRecursive();

		QueryPerformanceCounter(&t1);
		/* AGB_PAINT_FULL=1 repaints the whole viewport instead of the
		 * accumulated dirty rect. Diagnostic for "stale text": if the page
		 * looks right with this on, the invalidation rects are wrong, not the
		 * layout. */
		if (getenv("AGB_PAINT_FULL"))
			d->m_drawRect = IntRect(0, 0, d->m_width, d->m_height);
		//last draw update rect
		IntRect painted = d->m_drawRect;
		if (!painted.isEmpty()) {
			GraphicsContext gc(d->m_context);
			gc.save();
			gc.clip(painted);
			d->m_frameView->paint(&gc, painted);
			gc.restore();
			d->m_drawRect = IntRect(0, 0, 0, 0); //clear the invalidate Rect 
		}
		QueryPerformanceCounter(&t2);

		layoutMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
		paintMs  = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)freq.QuadPart;

		/* The painted rect is part of the record: "paint=900ms" on a viewport
		 * is a different bug from "paint=900ms" on a 20000px document rect
		 * (one long page invalidation repaints everything). */
		if (getenv("AGB_PERF_LOG") && (layoutMs + paintMs) > 100.0)
			fprintf(stderr, "[perf] layout=%.0fms paint=%.0fms total=%.0fms rect=%dx%d@%d,%d\n",
			        layoutMs, paintMs, layoutMs + paintMs,
			        painted.width(), painted.height(), painted.x(), painted.y());
	}

	/* An open <select> menu is composited on top of the page, into this view's
	 * own surface: there is no host popup window (see PopupMenu::createOwnPopup).
	 * The menu's own dirty() call has already unioned its rect into m_drawRect,
	 * so a page repaint never leaves it half erased. */
	if (d->m_activePopup) {
		IntRect pr = d->m_activePopup->windowRect();
		GraphicsContext gc(d->m_context);

		gc.save();
		gc.clip(IntRect(0, 0, d->m_width, d->m_height));
		gc.fillRect(pr, Color(255, 255, 255));
		gc.translate(pr.x(), pr.y());
		d->m_activePopup->paint(d->m_activePopup->clientRect(), &gc);
		gc.restore();

		// 1px frame, so the menu reads as a popup and not as part of the page.
		gc.save();
		gc.setStrokeColor(Color(160, 160, 160));
		gc.strokeRect(FloatRect(pr.x() + 0.5f, pr.y() + 0.5f,
		                        pr.width() - 1.0f, pr.height() - 1.0f), 1.0f);
		gc.restore();
	}
}

void WebView::setActivePopup(PopupMenu* popup)
{
	d->m_activePopup = popup;
}

PopupMenu* WebView::activePopup(void) const
{
	return d->m_activePopup;
}

/* The open <select> menu takes the input: points arrive in view coordinates and
 * the menu hit-tests in its own client coordinates (see PopupMenu::handleMouseEvent,
 * which is also what the host-window API macross_menu_mouse_event calls). */
static void popupMouseEvent(PopupMenu* popup, MC_MOUSE_EVENT_TYPE type,
                            _mc_mouse_btn btn, unsigned modifier, int x, int y)
{
	IntRect pr = popup->windowRect();
	MC_MOUSE_EVENT e;
	e.type = type;
	e.button = btn;
	e.point.x = x - pr.x();
	e.point.y = y - pr.y();
	e.modifier = modifier;
	popup->handleMouseEvent(e);
}

bool WebView::mouseUp(_mc_mouse_btn btn, unsigned m, int x, int y)
{
	if (d->m_activePopup) {
		popupMouseEvent(d->m_activePopup, EVT_MOUSE_UP, btn, m, x, y);
		return true;
	}

	IntPoint p(x, y);
	PlatformMouseEvent mouseEvent(MouseEventReleased, _get_mouse_button(btn), p, m);
    return d->m_frame->eventHandler()->handleMouseReleaseEvent(mouseEvent);
}

bool WebView::mouseDown(_mc_mouse_btn btn, unsigned m, int x, int y)
{
	if (d->m_activePopup) {
		popupMouseEvent(d->m_activePopup, EVT_MOUSE_DOWN, btn, m, x, y);
		return true;
	}

	IntPoint p(x, y);
	PlatformMouseEvent mouseEvent(MouseEventPressed, _get_mouse_button(btn), p, m);
    return d->m_frame->eventHandler()->handleMousePressEvent(mouseEvent);
}

bool WebView::mouseMove(_mc_mouse_btn btn, unsigned m, int x, int y)
{
	if (d->m_activePopup) {
		popupMouseEvent(d->m_activePopup, EVT_MOUSE_MOVE, btn, m, x, y);
		return true;
	}

	IntPoint p(x, y);
	PlatformMouseEvent mouseEvent(MouseEventMoved, _get_mouse_button(btn), p, m);
    return d->m_frame->eventHandler()->handleMouseMoveEvent(mouseEvent);
}

bool WebView::mouseWheel(_mc_mouse_btn btn, unsigned m, int x, int y)
{
	/* Chrome closes the menu and lets the page scroll (the menu list itself is
	 * not wheel-scrollable here: macross_menu_mouse_event has no wheel case). */
	if (d->m_activePopup && d->m_activePopup->client())
		d->m_activePopup->client()->hidePopup();

    int delta = 0;
    /* WebKit's convention: a positive delta means "scrolling up" (the mac
     * port fed NSEvent.deltaY straight in). ScrollView::wheelEvent() scrolls
     * by -delta * LINE_STEP and EventHandler maps delta < 0 to ScrollDown,
     * so the wheel up has to be the positive one - with the signs the other
     * way round the wheel scrolled the wrong way. Each notch of a Windows
     * wheel arrives as one event, so 5 lines (LINE_STEP is 20px) puts a notch
     * at 100px, which is what Chrome scrolls on Windows. */
    if(btn == MOUSE_WHEEL_UP)
        delta = 5;
    else
        delta = -5;
    IntPoint p(x, y);
    PlatformWheelEvent wheelEvent(delta, p, m);
    return d->m_frame->eventHandler()->handleWheelEvent(wheelEvent);
}

bool WebView::keyUp(_mc_key_sym key, unsigned m)
{
	/* An open <select> menu owns the keyboard; nothing is left for the page. */
	if (d->m_activePopup)
		return true;

	PlatformKeyboardEvent keyEvent(false, (unsigned)key, m);
	Frame* frame = d->m_frame->page()->focusController()->focusedOrMainFrame();
	return frame->eventHandler()->keyEvent(keyEvent);
}

bool WebView::keyDown(_mc_key_sym key, unsigned m)
{
	if (d->m_activePopup) {
		MC_KEY_EVENT e;
		e.type = EVT_KEY_DOWN;
		e.key = (MC_VIRTUAL_KEY)(unsigned)key;
		e.modifier = m;
		d->m_activePopup->handleKeyEvent(e);
		return true;
	}

	PlatformKeyboardEvent keyEvent(true, (unsigned)key, m);
	Frame* frame = d->m_frame->page()->focusController()->focusedOrMainFrame();

	/* Only keys that really produce a character have a WM_CHAR on the way.
	 * Arming the flag for every key would make "preventDefault on ArrowLeft"
	 * swallow the next typed character. */
	unsigned vk = (unsigned)key;
	bool mayProduceText = (vk >= 0x20 && vk <= 0x7E) ||
	                      vk == VK_BACK || vk == VK_TAB || vk == VK_RETURN;

	/* keyEvent() answers true when the page called preventDefault(). The
	 * character of this key is still on its way, so remember the answer and
	 * drop that character: a keydown filter such as "only digits in a phone
	 * field" has to be able to refuse the text. */
	d->m_swallowNextText = mayProduceText && frame->eventHandler()->keyEvent(keyEvent);

	return !d->m_swallowNextText;
}

bool WebView::contextMenu(unsigned m, int x, int y)
{
    d->m_frame->page()->contextMenuController()->clearContextMenu();
	IntPoint p(x, y);
	PlatformMouseEvent mouseEvent(MouseEventPressed, _get_mouse_button(MOUSE_BTN_RIGHT), p, m);
    Frame* focusedFrame = d->m_frame->page()->focusController()->focusedOrMainFrame();
	if (cb_set_cursor)
		cb_set_cursor(POINTER_CURSOR);

    bool handledEvent = focusedFrame->eventHandler()->sendContextMenuEvent(mouseEvent);
	if (handledEvent) {
		return true;
	}
	return false;
}

void WebView::setFocus(void)
{
    ChromeClientMC * client = static_cast<ChromeClientMC *>(d->m_frame->page()->chrome()->client());
	if (!d->m_frame->document())
		return;
	if (!client->isFocused()) {
    	Frame* frame = d->m_frame->page()->focusController()->focusedOrMainFrame();
    	d->m_frame->page()->focusController()->setFocusedFrame(frame);
    	client->setFocused(true);
	}
}

void WebView::killFocus(void)
{
    ChromeClientMC * client = static_cast<ChromeClientMC *>(d->m_frame->page()->chrome()->client());

	/* Leaving the view closes an open menu (the host window used to get
	 * WM_KILLFOCUS and do this). */
	if (d->m_activePopup && d->m_activePopup->client())
		d->m_activePopup->client()->hidePopup();

	if (!d->m_frame->document())
		return;
	if (client->isFocused()) {
    	d->m_frame->page()->focusController()->setFocusedFrame(0);
    	client->setFocused(false);
	}
}

int WebView::renderMode(void)
{
	return d->m_mode;
}

void WebView::setRenderMode(int mode, int limit)
{
	d->m_mode = mode;
	switch (mode)
	{
	case 0:
		{
			d->m_frameView->setNeedsLayout();
			d->m_frame->page()->setSmallView(false);
		}
		break;
	case 1:
		{
			d->m_frameView->setNeedsLayout();
			d->m_frame->page()->setSmallView(true);
            d->m_frame->page()->setSmallViewWidth((unsigned)limit);
		}
		break;
	}
	reflow();
}

void WebView::setViewSource(bool b)
{
    d->m_frame->setInViewSourceMode(b);
}

void WebView::setContentsPosition(int x, int y)
{
	int dx, dy, fx, fy, max_x, max_y;

	Frame* focus = d->m_frame->page()->focusController()->focusedOrMainFrame();
	dx = x - focus->view()->contentsX();
	dy = y - focus->view()->contentsY();

	for (Frame *frame = focus; frame; frame = frame->tree()->parent()) {
		fx = dx + frame->view()->contentsX();
		fy = dy + frame->view()->contentsY();

		max_x = frame->view()->contentsWidth();
		max_y = frame->view()->contentsHeight();

		if (fx > max_x) fx = max_x;
		if (fy > max_y) fy = max_y;
		if (fx < 0) fx = 0;
		if (fy < 0) fy = 0;

		if (frame->view()->setContentsPos(fx, fy))
			break;
	}
}

void WebView::contentsPosition(int *x, int *y)
{
    if (!x || !y) {
        return;
    }

	Frame* frame = d->m_frame->page()->focusController()->focusedOrMainFrame();
	*x = frame->view()->contentsX();
	*y = frame->view()->contentsY();
}

_mc_size WebView::contentsSize(void)
{
	_mc_size size;
	Frame* frame = d->m_frame->page()->focusController()->focusedOrMainFrame();
	size.w = frame->view()->contentsWidth();
    size.h = frame->view()->contentsHeight();
	return size;
}

void WebView::backward(void)
{
    d->m_frame->page()->goBack();
	garbageRelease();
}

void WebView::forward(void)
{
    d->m_frame->page()->goForward();
	garbageRelease();
}

void WebView::stop(void)
{
    d->m_frame->loader()->stopForUserCancel();
	garbageRelease();
}

void WebView::reload(void)
{
    /* reload() alone only replaces the DocumentLoader: stopAllLoaders() runs
     * inside continueLoadAfterNavigationPolicy with sendUnload=false, so the old
     * document is never unloaded and keeps its listeners/scripts alive into the
     * new load. With repeated reloads (or a reload while a script is running)
     * that leaves the frame with a half-torn document - the page ends up blank.
     * Tear it down explicitly first. */
    d->m_frame->loader()->stopLoading(true);
    d->m_frame->loader()->reload();
	garbageRelease();
}

void WebView::repaint(void)
{
	IntRect rc = IntRect(d->m_frame->view()->visibleContentRect());
	d->m_frame->view()->updateContents(rc, true);
    for (Frame* child = d->m_frame->tree()->firstChild(); child; child = child->tree()->nextSibling()){
		IntRect crc = IntRect(child->view()->visibleContentRect());
		child->view()->updateContents(crc, true);
	}
}

void WebView::reflow(void)
{
    if (d->m_frame->document())
        d->m_frame->document()->recalcStyle(Node::Force);
    for (Frame* child = d->m_frame->tree()->firstChild(); child; child = child->tree()->nextSibling()){
        if (child->document())
            child->document()->recalcStyle(Node::Force);
	}

    d->m_frameView->layoutIfNeededRecursive();
}

void WebView::loadHtml(const char* str, const char* rurl)
{
    stop();
	String urlstr = String::fromUTF8(rurl);
	KURL baseurl(urlstr.deprecatedString());
    ResourceRequest request(baseurl);
    RefPtr<WebCore::SharedBuffer> data = new SharedBuffer(str, strlen(str));
    SubstituteData substituteData(data, String("text/html"), String("utf-8"), KURL(), baseurl);
    d->m_frame->loader()->load(request, substituteData);
}

/* Reads one of the browser's own pages: agave://downloads -> downloads.html
 * next to the executable. The name is restricted to [a-z0-9_-] so a crafted URL
 * cannot walk out of the directory. */
static bool loadInternalPage(const char* url, String& out)
{
    const char* name = url + 8;   /* after "agave://" */
    char page[64];
    char path[1024];
    char* buffer = 0;
    FILE* file = 0;
    long size = 0;
    size_t n = 0;

    while (name[n] && name[n] != '/' && name[n] != '?' && name[n] != '#') {
        char c = name[n];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;

        if (n + 1 >= sizeof(page))
            return false;

        page[n] = c;
        ++n;
    }

    page[n] = 0;

    if (!n)
        return false;

    {
        DWORD len = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));

        if (!len || len >= sizeof(path))
            return false;

        {
            char* slash = strrchr(path, '\\');

            if (!slash)
                return false;

            slash[1] = 0;
        }
    }

    strncat(path, page, sizeof(path) - strlen(path) - 1);
    strncat(path, ".html", sizeof(path) - strlen(path) - 1);

    file = fopen(path, "rb");

    if (!file) {
        fprintf(stderr, "[agave] internal page %s not found (looked for %s)\n", url, path);
        return false;
    }

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size > 0) {
        buffer = (char*)malloc((size_t)size + 1);

        if (buffer) {
            size_t read = fread(buffer, 1, (size_t)size, file);

            buffer[read] = 0;
            out = String::fromUTF8(buffer, read);
            free(buffer);
        }
    }

    fclose(file);
    return !out.isEmpty();
}

void WebView::loadUrl(const char* url, bool lc)
{	
    stop();

    /* Internal pages are served by the engine itself: a synthetic document
     * whose URL is the agave:// one, so the address bar, document.URL and the
     * realm's binding profile all agree, and no network layer is involved. */
    if (url && 0 == strncasecmp(url, "agave://", 8)) {
        String html;

        if (loadInternalPage(url, html)) {
            CString bytes = html.utf8();

            loadHtml(bytes.data(), url);
            return;
        }
    }
    if (0 == strncasecmp(url, "about:blank", 11)) {
        /* A tab navigated to about:blank must own a document: since R44 the
         * main frame has none until its first load, which left
         * document.documentElement null and page scripts without a body.
         * Commit a skeleton through the substitute-data path (the one internal
         * pages use). The commit lands on the next timer tick - nothing runs in
         * between, and document.URL stays about:blank. The location and title
         * callbacks still fire from the load pipeline (dispatchDidSetLocation
         * synchronously, <title> after the parse), so no manual cb_set_* here.
         *
         * The early return matters: falling through would run the URL through
         * the KURL branch and start a second about:blank load, whose
         * stopAllLoaders() cancels the skeleton that is still waiting for its
         * timer tick - ending with an empty document and a duplicate request. */
        loadHtml("<html><head><title>about:blank</title></head><body></body></html>", url);
        return;
    }

	KURL Url;
	if (0 == strncasecmp(url, "file://", 7)) {
		/* A file: URL may carry a ?query / #fragment ("page.html?focus=b").
		 * encode_string() escapes everything that is not a safe path byte,
		 * '?' included, which turned the query into part of the file name
		 * ("...html%3Ffocus=b") and the load came back as an empty document.
		 * Only the path is escaped; the suffix is structural and goes through
		 * verbatim. */
		DeprecatedString raw = DeprecatedString::fromUtf8(url);
		int qmark = raw.find('?');
		int hash = raw.find('#');
		int suffixAt = (qmark < 0) ? hash : ((hash < 0) ? qmark : (qmark < hash ? qmark : hash));

		DeprecatedString UrlStr = suffixAt >= 0
			? KURL::encode_string(raw.left((unsigned)suffixAt)) + raw.mid((unsigned)suffixAt)
			: KURL::encode_string(raw);
		Url = KURL(UrlStr);
	} else {
		Url = KURL(url);
	}

    if (!Url.isValid()){ 
        //try to guess it is a http url.
        DeprecatedString urlstr("http://");
        urlstr.append(url);
        Url = KURL(urlstr);
    }

    if (!Url.isValid())// it is still a invalid url! 
        return;

#if ENABLE(SCHEMEEXTENSION)
    if (scheduleScheme(Url))
        return;
#endif

    ResourceRequest request(Url);
    d->m_frame->page()->backForwardList()->setEnabled(!lc);
    d->m_frame->loader()->load(request);
}

/* The virtual key a typed character is reported with. keypress readers
 * (event.keyCode / which) and the editing command table both key off it;
 * Win32 gives no vk for text that came from the IME, so fall back to the
 * character itself for printable ASCII (digits and punctuation have their
 * ASCII value as vk). */
static unsigned virtualKeyForCharacter(UChar c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';   /* VK_A .. VK_Z */
    if (c < 0x80)
        return c;
    return 0;
}

bool WebView::textInput(const char* text)
{
	String str = String::fromUTF8(text);
	if (str.isEmpty() || str.isNull())
		return false;

    if (d->m_activePopup) {
        /* An open <select> menu swallows the text: there is no type-ahead. */
        d->m_swallowNextText = false;
        return false;
    }

    if (d->m_swallowNextText) {
        /* The page cancelled the keydown of this character. */
        d->m_swallowNextText = false;
        return false;
    }

    Frame* frame = d->m_frame->page()->focusController()->focusedOrMainFrame();
    if (!frame)
        return false;

    /* WM_CHAR also carries the control characters TranslateMessage produces
     * for Backspace (0x08), Enter (0x0D), Tab (0x09) and Ctrl+letter
     * (0x01..0x1A). Those are editing commands, handled from the keydown, and
     * must never be inserted as text: the old code appended them to the
     * field's value, so pressing Backspace left a literal 0x08 behind. */
    String typed;
    for (unsigned i = 0; i < str.length(); ++i) {
        UChar c = str[i];
        if (c >= 0x20 && c != 0x7F)
            typed.append(c);
    }
    if (typed.isEmpty())
        return false;

    /* A character is a keypress. The page sees the event first - charCode and
     * keyCode now carry the real character - and its default action inserts
     * the text through the normal editing path, so the caret, the selection
     * and "insert at the caret" behave like in any other browser. The old code
     * appended to input->value() directly, which ignored both. */
    PlatformKeyboardEvent pressEvent(true, virtualKeyForCharacter(typed[0]), 0);
    pressEvent.setText(typed);

    if (frame->eventHandler()->handleCharacterEvent(pressEvent))
        return true;

    /* Nothing editable took the text (a field the engine does not consider
     * editable): fall back to the value path the port used before. */
    if (Node* focused = frame->document()->focusedNode()) {
        if (focused->isElementNode() && focused->hasTagName(HTMLNames::inputTag)) {
            HTMLInputElement* input = static_cast<HTMLInputElement*>(focused);
            input->setValue(input->value() + typed);
            /* setValue() refreshes m_value; the *painted* text of a focused
             * control comes from its inner editor, so refresh that as well. */
            if (input->renderer()) {
                input->renderer()->updateFromElement();
                input->renderer()->repaint(true);
            }
            return true;
        }
        if (focused->isElementNode() && focused->hasTagName(HTMLNames::textareaTag)) {
            HTMLTextAreaElement* area = static_cast<HTMLTextAreaElement*>(focused);
            area->setValue(area->value() + typed);
            if (area->renderer())
                area->renderer()->repaint(true);
            return true;
        }
    }

    return false;
}

void WebView::setEncodingName(_mc_encoding name)
{
    if (name == CHARSET_AUTO_DETECT)
        d->m_frame->loader()->documentLoader()->setOverrideEncoding(""); //auto detect
    else
        d->m_frame->loader()->documentLoader()->setOverrideEncoding(String(Encodings[name]));

    reload();
}

_mc_encoding WebView::encodingName(void)
{
    String encoding = d->m_frame->loader()->documentLoader()->overrideEncoding();
    if (!encoding.isEmpty()) {
        for (int i = 0; i < EncodingNum; i++) {
            if (equalIgnoringCase(encoding, Encodings[i]))
                return static_cast<MC_ENCODING_NAME>(i);
        }
    }
    return CHARSET_AUTO_DETECT;
}

int WebView::textScale(void)
{
    return d->m_frame->zoomFactor();
}

void WebView::setTextScale(int factor)
{
    d->m_frame->setZoomFactor(factor);
}

int WebView::scaleFactor(void)
{
	return (int)(d->m_frame->pageZoomFactor()*100);
}

void WebView::setScaleFactor(int scale)
{
	if (scale < 10) scale = 10;
	if (scale > 500) scale = 500;

	if (d->m_frame->setPageZoomFactor((float)scale/100.0f))
		firstLayout();
}

bool WebView::search(const char *str, bool forward, bool caseSens, bool wrap)
{
    if (!d->m_frame->page() || !d->m_frame->page()->mainFrame())
        return false;

    if (!str || !strlen(str))
        return false;

    String search(str, strlen(str));
    bool found = false;

    Frame* frame = d->m_frame->page()->focusController()->focusedOrMainFrame();
    Frame* startFrame = frame;
    do {
        found = frame->findString(search, forward, caseSens, false, true);
        if (found) {
            if (frame != startFrame)
                startFrame->selectionController()->clear();
            d->m_frame->page()->focusController()->setFocusedFrame(frame);
            return found;
        }
        frame = incrementFrame(frame, forward, wrap);
    } while (frame && frame != startFrame);

    // Search contents of startFrame, on the other side of the selection that we did earlier.
    // We cheat a bit and just research with wrap on
    if (wrap && !startFrame->selectionController()->isNone()) {
        found = startFrame->findString(search, forward, caseSens, true, true);
        d->m_frame->page()->focusController()->setFocusedFrame(frame);
    }

    return found;
}

int WebView::markAll(const char *str, bool caseSens, bool highlight, unsigned int limit)
{
    if (!d->m_frame->page() || !d->m_frame->page()->mainFrame())
        return -1;

    if (!str || !strlen(str))
        return -1;

    String search(str, strlen(str));
    unsigned int matches = 0;

    Frame* frame = d->m_frame->page()->mainFrame();
    do {
        frame->setMarkedTextMatchesAreHighlighted(highlight);
        matches += frame->markAllMatchesForText(search, caseSens, (limit == 0)? 0 : (limit - matches));
        frame = incrementFrame(frame, true, false);
    } while (frame);

    return matches;
}

void WebView::executeScript(const char* script)
{
	d->m_frame->loader()->executeScript(String::fromUTF8(script), true);
}

void WebView::unmarkAll(void)
{
    if (!d->m_frame->page() || !d->m_frame->page()->mainFrame())
        return;

    Frame* frame = d->m_frame->page()->mainFrame();
    do {
        if (Document* document = frame->document())
            document->removeMarkers(DocumentMarker::TextMatch);
        frame = incrementFrame(frame, true, false);
    } while (frame);
}

void WebView::firstLayout(void)
{
	if (!d->m_minLayoutSize.isEmpty()) { //use min layout size.
		d->m_frameView->resize(d->m_minLayoutSize.width(), d->m_minLayoutSize.height());
		d->m_oldLayoutSize = IntSize(0, 0);
	}
}

void WebView::resizeContents(int w, int h)
{
	if (!d->m_minLayoutSize.isEmpty()) { //use min layout size.
		if (w && h) {
			int sw = min(max(d->m_minLayoutSize.width(), w), d->m_width);
			int sh = min(max(d->m_minLayoutSize.height(), h), d->m_height);

			if (((d->m_oldLayoutSize.width() < sw) || (d->m_oldLayoutSize.height() < sh))) {

				int nw = d->m_oldLayoutSize.width();
				int nh = d->m_oldLayoutSize.height();

				// FIXME: default margin 8 px
				nw = (8 > (sw-nw) && nw > 0) ? nw : sw;
				nh = (8 > (sh-nh) && nh > 0) ? nh : sh;

				d->m_frameView->resize(nw, nh);
				d->m_oldLayoutSize = IntSize(nw, nh);
			}
		}
	}
}

bool WebView::isTextAntialias(void)
{
	Settings* setting = Settings::globalSettings();
	return setting->textAntialias();
}

void WebView::setTextAntialias(bool b)
{
	Settings* setting = Settings::globalSettings();
	setting->setTextAntialias(b);
}

MouseButton _get_mouse_button(_mc_mouse_btn button)
{
	switch (button) {
		case MOUSE_BTN_LEFT:
			return LeftButton;
		case MOUSE_BTN_MIDDLE:
			return MiddleButton;
		case MOUSE_BTN_RIGHT:
			return RightButton;
		default:
			return NoButton;
	}
}
}
