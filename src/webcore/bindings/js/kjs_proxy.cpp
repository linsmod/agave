/*
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2006, 2007 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "kjs_proxy.h"

#include "Chrome.h"
#include "Document.h"
#include "Frame.h"
#include "FrameLoader.h"
#include "GCController.h"
#include "JSDocument.h"
#include "JSDOMWindow.h"
#include "Page.h"
#include "Settings.h"
#include "kjs_events.h"
#include "kjs_window.h"

#if ENABLE(SVG)
#include "JSSVGLazyEventListener.h"
#endif

#include "NativeBinding.h"

#if ENABLE(ANTJS)
#include "AntRuntime.h"
#include <stdlib.h>
#include <string.h>
#endif

using namespace KJS;

namespace WebCore {

KJSProxy::KJSProxy(Frame* frame)
{
    m_frame = frame;
    m_handlerLineno = 0;
}

KJSProxy::~KJSProxy()
{
    // Check for <rdar://problem/4876466>. In theory, no JS should be executing
    // in our interpreter. 
    ASSERT(!m_script || !m_script->context());
    
    if (m_script) {
        m_script = 0;
    
        // It's likely that destroying the interpreter has created a lot of garbage.
        gcController().garbageCollectSoon();
    }
}

#if ENABLE(ANTJS)
/* WebKit's String::utf8() mangles some multi-byte sequences in very large
 * strings (a lead byte dropped, continuations masked with 0x7F), which corrupts
 * minified scripts. Encode straight from the UTF-16 buffer instead. */
static CString utf8ForScript(const String& text)
{
    const ::UChar* chars = text.characters();
    unsigned length = text.length();
    Vector<char> out;
    unsigned i;

    out.reserveCapacity(length + length / 2 + 8);

    for (i = 0; i < length; ++i) {
        unsigned c = chars[i];

        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < length) {
            unsigned low = chars[i + 1];

            if (low >= 0xDC00 && low <= 0xDFFF) {
                unsigned cp = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                out.append((char)(0xF0 | (cp >> 18)));
                out.append((char)(0x80 | ((cp >> 12) & 0x3F)));
                out.append((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.append((char)(0x80 | (cp & 0x3F)));
                ++i;
                continue;
            }
        }

        if (c < 0x80) {
            out.append((char)c);
        } else if (c < 0x800) {
            out.append((char)(0xC0 | (c >> 6)));
            out.append((char)(0x80 | (c & 0x3F)));
        } else {
            out.append((char)(0xE0 | (c >> 12)));
            out.append((char)(0x80 | ((c >> 6) & 0x3F)));
            out.append((char)(0x80 | (c & 0x3F)));
        }
    }

    out.append('\0');
    return CString(out.data(), (unsigned)(out.size() - 1));
}
#endif
JSValue* KJSProxy::evaluate(const String& filename, int baseLine, const String& str) 
{
    // evaluate code. Returns the JS return value or 0
    // if there was none, an error occured or the type couldn't be converted.

#if ENABLE(ANTJS)
    /* Task 2: page scripts run on the Ant engine. The DOM surface is provided by
     * the AntRuntime shim (document.title for now; elements come in task 3). */
    if (AntRuntime::isRunning() && m_frame) {
        CString code = utf8ForScript(str);
        char* out = 0;
        char* err = 0;

        /* Each view owns an Ant VM: make sure this frame's page is the active
         * one before evaluating, otherwise the script would see another view's
         * window/document. */
        if (m_frame->page())
            AntRuntime::activateContext(AntRuntime::contextForPage(m_frame->page()));

        m_frame->keepAlive();
        AntRuntime::bindDocument(m_frame->document());
        AntRuntime::setFilename(filename.isNull() ? "agave://inline" : filename.utf8().data());


        if (getenv("AGB_JS_DEBUG")) {
            fprintf(stderr, "[jsdbg] %s bytes=%u\n",
                    filename.isNull() ? "(inline)" : filename.utf8().data(),
                    (unsigned)code.length());
        }

        if (!AntRuntime::evaluate(code.data(), code.length(), &out, &err)) {
            CString where = filename.isNull() ? String("(inline)").utf8() : filename.utf8();
            fprintf(stderr, "[antjs] %s: %s\n", where.data(), err ? err : "(no message)");

            /* Point at the failing line of this script so the missing API is
             * visible without guessing. The engine reports "page.js:LINE:COL". */
            {
                int line = 0, column = 0;
                const char* pos = err ? strstr(err, "page.js:") : 0;
                if (pos && sscanf(pos + 8, "%d:%d", &line, &column) == 2 && line > 0) {
                    int current = 1;
                    unsigned start = 0, end = 0;
                    bool found = false;
                    const ::UChar* chars = str.characters();
                    unsigned length = str.length();

                    for (unsigned i = 0; i < length; ++i) {
                        if (chars[i] != '\n')
                            continue;
                        if (current == line) { end = i; found = true; break; }
                        ++current;
                        start = i + 1;
                    }
                    if (!found && current == line) {
                        end = length;
                        found = true;
                    }
                    if (!found)
                        start = end = 0;
                    if (end > start) {
                        String snippet(str.characters() + start, min(end - start, 400u));
                        CString text = snippet.utf8();
                        fprintf(stderr, "[antjs]   line %d col %d: %s\n", line, column, text.data());
                    }
                } else if (str.length()) {
                    /* No position in the stack: show the head of the script so it
                     * can still be matched against the page source. */
                    String head(str.characters(), min(str.length(), 300u));
                    CString text = head.utf8();
                    fprintf(stderr, "[antjs]   script head: %s\n", text.data());
                }
            }
        }

        free(out);
        free(err);
        return jsUndefined();
    }
#endif

    initScriptIfNeeded();
    // inlineCode is true for <a href="javascript:doSomething()">
    // and false for <script>doSomething()</script>. Check if it has the
    // expected value in all cases.
    // See smart window.open policy for where this is used.
    bool inlineCode = filename.isNull();

    m_script->setInlineCode(inlineCode);

    JSLock lock;

    // Evaluating the JavaScript could cause the frame to be deallocated
    // so we start the keep alive timer here.
    m_frame->keepAlive();
    
    JSValue* thisNode = Window::retrieve(m_frame);
  
    m_script->startTimeoutCheck();
    Completion comp = m_script->evaluate(filename, baseLine, reinterpret_cast<const KJS::UChar*>(str.characters()), str.length(), thisNode);
    m_script->stopTimeoutCheck();
  
    if (comp.complType() == Normal || comp.complType() == ReturnValue)
        return comp.value();

    if (comp.complType() == Throw) {
        UString errorMessage = comp.value()->toString(m_script->globalExec());
        int lineNumber = comp.value()->toObject(m_script->globalExec())->get(m_script->globalExec(), "line")->toInt32(m_script->globalExec());
        UString sourceURL = comp.value()->toObject(m_script->globalExec())->get(m_script->globalExec(), "sourceURL")->toString(m_script->globalExec());
        if (Page* page = m_frame->page())
            page->chrome()->addMessageToConsole(JSMessageSource, ErrorMessageLevel, errorMessage, lineNumber, sourceURL);
    }

    return 0;
}

void KJSProxy::clear() {
  // clear resources allocated by the interpreter, and make it ready to be used by another page
  // We have to keep it, so that the Window object for the frame remains the same.
  // (we used to delete and re-create it, previously)
    if (m_script) {
        Window *win = Window::retrieveWindow(m_frame);
        if (win) {
            win->clear();
#if ENABLE(JSNATIVEBINDING)
            (void)initNativeJSObjects(m_script); 
#endif            
        }
    }
}

EventListener* KJSProxy::createHTMLEventHandler(const String& functionName, const String& code, Node* node)
{
    initScriptIfNeeded();
    JSLock lock;
    return new JSLazyEventListener(functionName, code, Window::retrieveWindow(m_frame), node, m_handlerLineno);
}

#if ENABLE(SVG)
EventListener* KJSProxy::createSVGEventHandler(const String& functionName, const String& code, Node* node)
{
    initScriptIfNeeded();
    JSLock lock;
    return new JSSVGLazyEventListener(functionName, code, Window::retrieveWindow(m_frame), node, m_handlerLineno);
}
#endif

void KJSProxy::finishedWithEvent(Event* event)
{
  // This is called when the DOM implementation has finished with a particular event. This
  // is the case in sitations where an event has been created just for temporary usage,
  // e.g. an image load or mouse move. Once the event has been dispatched, it is forgotten
  // by the DOM implementation and so does not need to be cached still by the interpreter
  m_script->forgetDOMObject(event);
}

ScriptInterpreter* KJSProxy::interpreter()
{
  initScriptIfNeeded();
  ASSERT(m_script);
  return m_script.get();
}

void KJSProxy::initScriptIfNeeded()
{
    if (m_script)
        return;

    // Build the global object - which is a Window instance
    JSLock lock;
    JSObject* globalObject = new JSDOMWindow(m_frame->domWindow());

    // Create a KJS interpreter for this frame
    m_script = new ScriptInterpreter(globalObject, m_frame);

    //init dom object all Quickjs !!! <Debug>

    String userAgent = m_frame->loader()->userAgent(m_frame->document() ? m_frame->document()->URL() : KURL());
    if (userAgent.find("Microsoft") >= 0 || userAgent.find("MSIE") >= 0)
        m_script->setCompatMode(Interpreter::IECompat);
    else
        // If we find "Mozilla" but not "(compatible, ...)" we are a real Netscape
        if (userAgent.find("Mozilla") >= 0 && userAgent.find("compatible") == -1)
            m_script->setCompatMode(Interpreter::NetscapeCompat);

    m_frame->loader()->dispatchWindowObjectAvailable();

#if ENABLE(JSNATIVEBINDING)            
    (void)initNativeJSObjects(m_script);
#endif
}
    
void KJSProxy::clearDocumentWrapper() 
{
    if (!m_script)
        return;

    JSLock lock;
    m_script->globalObject()->removeDirect("document");
}

}
