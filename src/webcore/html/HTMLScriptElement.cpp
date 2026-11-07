/**
 * This file is part of the DOM implementation for KDE.
 *
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2003, 2004, 2005, 2006, 2007 Apple Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#include "config.h"
#include "HTMLScriptElement.h"

#include "CachedScript.h"
#include "DocLoader.h"
#include "Document.h"
#include "EventNames.h"
#include "Frame.h"
#include "HTMLNames.h"
#include "Text.h"

/* The one diagnostics sink (src/webview/AgbLog.h, header-only). */
#include "../../webview/AgbLog.h"

#if ENABLE(KJS)
#include "kjs_proxy.h"
#endif

#if ENABLE(ANTJS)
#include "../javascript/ant_backend/AntRuntime.h"
#endif

#if ENABLE(QJS)
#include "qjs_script.h"
#endif

namespace WebCore {

using namespace HTMLNames;
using namespace EventNames;

HTMLScriptElement::HTMLScriptElement(Document *doc)
    : HTMLElement(scriptTag, doc)
    , m_cachedScript(0)
    , m_createdByParser(false)
    , m_evaluated(false)
{
}

HTMLScriptElement::~HTMLScriptElement()
{
    if (m_cachedScript)
        m_cachedScript->deref(this);
}

bool HTMLScriptElement::isURLAttribute(Attribute *attr) const
{
    return attr->name() == srcAttr;
}

void HTMLScriptElement::childrenChanged()
{
    // If a node is inserted as a child of the script element
    // and the script element has been inserted in the document
    // we evaluate the script.
    if (!m_createdByParser && inDocument() && firstChild())
        evaluateScript(document()->URL(), text());
}

void HTMLScriptElement::parseMappedAttribute(MappedAttribute *attr)
{
    const QualifiedName& attrName = attr->name();
    if (attrName == srcAttr) {
        if (m_evaluated || m_cachedScript || m_createdByParser || !inDocument())
            return;

        // FIXME: Evaluate scripts in viewless documents.
        // See http://bugs.webkit.org/show_bug.cgi?id=5727
        if (!document()->frame())
            return;
    
        const AtomicString& url = attr->value();
        if (!url.isEmpty()) {
            /* Dynamically inserted entries keep the CachedScript path: the
             * unified module-fetcher route needs a waiting tokenizer, which
             * only the parser-driven flow provides. */
            m_cachedScript = document()->docLoader()->requestScript(url, getAttribute(charsetAttr));
            if (m_cachedScript)
                m_cachedScript->ref(this);
            else
                dispatchHTMLEvent(errorEvent, true, false);
        }
    } else if (attrName == onloadAttr)
        setHTMLEventListener(loadEvent, attr);
    else
        HTMLElement::parseMappedAttribute(attr);
}

void HTMLScriptElement::finishedParsing()
{
    // The parser just reached </script>. If we have no src and no text,
    // allow dynamic loading later.
    if (getAttribute(srcAttr).isEmpty() && text().isEmpty())
        setCreatedByParser(false);
    HTMLElement::finishedParsing();
}

void HTMLScriptElement::insertedIntoDocument()
{
    HTMLElement::insertedIntoDocument();

    ASSERT(!m_cachedScript);

    if (m_createdByParser)
        return;
    
    // FIXME: Eventually we'd like to evaluate scripts which are inserted into a 
    // viewless document but this'll do for now.
    // See http://bugs.webkit.org/show_bug.cgi?id=5727
    if (!document()->frame())
        return;
    
    const AtomicString& url = getAttribute(srcAttr);
    if (!url.isEmpty()) {
        /* Same as parseMappedAttribute: dynamic entries keep CachedScript. */
        m_cachedScript = document()->docLoader()->requestScript(url, getAttribute(charsetAttr));
        if (m_cachedScript)
            m_cachedScript->ref(this);
        else
            dispatchHTMLEvent(errorEvent, true, false);
        return;
    }

    // If there's an empty script node, we shouldn't evaluate the script
    // because if a script is inserted afterwards (by setting text or innerText)
    // it should be evaluated, and evaluateScript only evaluates a script once.
    String scriptString = text();    
    if (!scriptString.isEmpty())
        evaluateScript(document()->URL(), scriptString);
}

void HTMLScriptElement::removedFromDocument()
{
    HTMLElement::removedFromDocument();

    if (m_cachedScript) {
        m_cachedScript->deref(this);
        m_cachedScript = 0;
    }
}

void HTMLScriptElement::notifyFinished(CachedResource* o)
{
    CachedScript *cs = static_cast<CachedScript *>(o);

    /* For <script async> the tokenizer requests the body itself and hands the
     * resource straight to this element, so m_cachedScript stays null. */

    // Evaluating the script could lead to a garbage collection which
    // can delete the script element so we need to protect it.
    RefPtr<HTMLScriptElement> protect(this);
    
    if (cs->errorOccurred())
        dispatchHTMLEvent(errorEvent, true, false);
    else {
        evaluateScript(cs->url(), cs->script());
        dispatchHTMLEvent(loadEvent, false, false);
    }

    // script evaluation may have dereffed it already
    if (m_cachedScript) {
        m_cachedScript->deref(this);
        m_cachedScript = 0;
    }
}

bool HTMLScriptElement::isModuleScript() const
{
    const AtomicString& type = getAttribute(typeAttr);

    if (type.isEmpty())
        return false;

    return type.domString().stripWhiteSpace().lower() == "module";
}

bool HTMLScriptElement::shouldExecuteAsJavaScript()
{
    /* <script nomodule> is the legacy-browser fallback: an engine with module
     * support must not run it (v.qq.com's SystemJS bundle ran here and threw
     * "System is not defined" on every load). */
    if (hasAttribute(AtomicString("nomodule")))
        return false;

    /*
        Mozilla 1.8 and WinIE 7 both accept text/javascript and text/ecmascript.
        Mozilla 1.8 accepts application/javascript, application/ecmascript, and application/x-javascript, but WinIE 7 doesn't.
        WinIE 7 accepts text/javascript1.1 - text/javascript1.3, text/jscript, and text/livescript, but Mozilla 1.8 doesn't.
        Mozilla 1.8 allows leading and trailing whitespace, but WinIE 7 doesn't.
        Mozilla 1.8 and WinIE 7 both accept the empty string, but neither accept a whitespace-only string.
        We want to accept all the values that either of these browsers accept, but not other values.
     */
    static const AtomicString validTypes[] = {
        "module",           /* ES module: handled by evaluateScript below */
        "text/javascript",
        "text/ecmascript",
        "application/javascript",
        "application/ecmascript",
        "application/x-javascript",
        "text/javascript1.1",
        "text/javascript1.2",
        "text/javascript1.3",
        "text/jscript",
        "text/livescript",
    };
    static const unsigned validTypesCount = sizeof(validTypes) / sizeof(validTypes[0]);

    /*
         Mozilla 1.8 accepts javascript1.0 - javascript1.7, but WinIE 7 accepts only javascript1.1 - javascript1.3.
         Mozilla 1.8 and WinIE 7 both accept javascript and livescript.
         WinIE 7 accepts ecmascript and jscript, but Mozilla 1.8 doesn't.
         Neither Mozilla 1.8 nor WinIE 7 accept leading or trailing whitespace.
         We want to accept all the values that either of these browsers accept, but not other values.
     */
    static const AtomicString validLanguages[] = {
        "javascript",
        "javascript1.0",
        "javascript1.1",
        "javascript1.2",
        "javascript1.3",
        "javascript1.4",
        "javascript1.5",
        "javascript1.6",
        "javascript1.7",
        "livescript",
        "ecmascript",
        "jscript"
    };
    static const unsigned validLanguagesCount = sizeof(validLanguages) / sizeof(validLanguages[0]); 

    const AtomicString& type = getAttribute(typeAttr);
    if (!type.isEmpty()) {
        String lowerType = type.domString().stripWhiteSpace().lower();
        for (unsigned i = 0; i < validTypesCount; ++i)
            if (lowerType == validTypes[i])
                return true;

        return false;
    }
    
    const AtomicString& language = getAttribute(languageAttr);
    if (!language.isEmpty()) {
        String lowerLanguage = language.domString().lower();
        for (unsigned i = 0; i < validLanguagesCount; ++i)
            if (lowerLanguage == validLanguages[i])
                return true;

        return false;
    }

    // No type or language is specified, so we assume the script to be JavaScript
    return true;
}

void HTMLScriptElement::evaluateScript(const String& URL, const String& script)
{
    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[eval] enter %s evaluated=%d type='%s'\n",
                URL.utf8().data(), m_evaluated ? 1 : 0,
                getAttribute(typeAttr).domString().utf8().data());

    if (m_evaluated)
        return;
    
    if (!shouldExecuteAsJavaScript())
        return;

    /* <script defer src> runs after parsing like a module, but unlike a module
     * it must not hold the tokenizer: blocking the parse made it run before
     * <body> existed (deepseek's main.js read document.body.classList and
     * died). The element path is only reached for non-blocking scripts
     * (async / defer / dynamic), so queue it for the post-parse flush. */
    if (defer() && !hasAttribute(AtomicString("async"))) {
        agbLog(AGB_LOG_PARSE, "script (defer) queued for parse end url=%s len=%u",
               URL.utf8().data(), (unsigned)script.length());
        AntRuntime::queueDeferredScript(document(), URL.utf8().data(), script.utf8().data(), this);
        m_evaluated = true;
        return;
    }

    Frame* frame = document()->frame();
    if (frame) {
#if ENABLE(KJS)
        KJSProxy* proxy = frame->scriptProxy();
        if (proxy) {
            m_evaluated = true;

            if (getenv("AGB_MODULE_DEBUG"))
                fprintf(stderr, "[module] %s type='%s' isModule=%d\n",
                        URL.utf8().data(),
                        getAttribute(typeAttr).domString().utf8().data(),
                        isModuleScript() ? 1 : 0);

            if (isModuleScript()) {
                /* Modules run after parsing, like defer; evaluating here would
                 * run them against a half built document. They also never set
                 * document.currentScript (HTML spec). */
                agbLog(AGB_LOG_PARSE, "script (module, src) queued for parse end url=%s len=%u",
                       URL.utf8().data(), (unsigned)script.length());
                AntRuntime::queueModule(URL.utf8().data(), script.utf8().data());
            } else {
                agbLog(AGB_LOG_PARSE, "script (async/dynamic) running url=%s len=%u",
                       URL.utf8().data(), (unsigned)script.length());
                CurrentScriptScope currentScript(document(), this);
                proxy->evaluate(URL, 0, script);
            }

            Document::updateDocumentsRendering();
        }
#endif

#if ENABLE(QJS)
        ScriptController* controller = frame->script();
        if (controller) {
            m_evaluated = true;
            controller->evaluate(URL, 0, script);
            Document::updateDocumentsRendering();
        }
#endif
    }
}

String HTMLScriptElement::text() const
{
    String val = "";
    
    for (Node *n = firstChild(); n; n = n->nextSibling()) {
        if (n->isTextNode())
            val += static_cast<Text *>(n)->data();
    }
    
    return val;
}

void HTMLScriptElement::setText(const String &value)
{
    ExceptionCode ec = 0;
    int numChildren = childNodeCount();
    
    if (numChildren == 1 && firstChild()->isTextNode()) {
        static_cast<Text *>(firstChild())->setData(value, ec);
        return;
    }
    
    if (numChildren > 0) {
        removeChildren();
    }
    
    appendChild(document()->createTextNode(value.impl()), ec);
}

String HTMLScriptElement::htmlFor() const
{
    // DOM Level 1 says: reserved for future use.
    return String();
}

void HTMLScriptElement::setHtmlFor(const String &/*value*/)
{
    // DOM Level 1 says: reserved for future use.
}

String HTMLScriptElement::event() const
{
    // DOM Level 1 says: reserved for future use.
    return String();
}

void HTMLScriptElement::setEvent(const String &/*value*/)
{
    // DOM Level 1 says: reserved for future use.
}

String HTMLScriptElement::charset() const
{
    return getAttribute(charsetAttr);
}

void HTMLScriptElement::setCharset(const String &value)
{
    setAttribute(charsetAttr, value);
}

bool HTMLScriptElement::defer() const
{
    return !getAttribute(deferAttr).isNull();
}

void HTMLScriptElement::setDefer(bool defer)
{
    setAttribute(deferAttr, defer ? "" : 0);
}

String HTMLScriptElement::src() const
{
    return document()->completeURL(getAttribute(srcAttr));
}

void HTMLScriptElement::setSrc(const String &value)
{
    setAttribute(srcAttr, value);
}

String HTMLScriptElement::type() const
{
    return getAttribute(typeAttr);
}

void HTMLScriptElement::setType(const String &value)
{
    setAttribute(typeAttr, value);
}

}
