/*
 * AntRuntime.cpp - C++ facade over the Ant bridge (antbridge.dll).
 */

#include "config.h"

#include "AntRuntime.h"

#include <stdlib.h>
#include <string.h>

#include "AntBridge.h"

#include "Document.h"
#include "Element.h"
#include "Node.h"
#include "PlatformString.h"
#include "Event.h"
#include "KURL.h"
#include "Text.h"
#include "HTMLInputElement.h"
#include "HTMLNames.h"
#include "HTMLTextAreaElement.h"

#include <stdio.h>
#include <string>
#include <vector>

namespace {

#define AGB_TAG_NODE 0x4E4F4431u   /* 'NOD1' */

AntbEngine* g_engine = NULL;
WebCore::Document* g_document = NULL;
bool g_documentBound = false;

/* DOM binding (defined further down) */
AntbClass* g_nodeClass = NULL;
AntbValue* g_locationObj = NULL;
AntbValue* g_docObj = NULL;   /* the JS document object (rooted) */
int nodeGetAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeAddEventListener(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docGetElementById(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeDispatchEvent(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int locationGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
AntbValue* wrapNode(AntbEngine*, WebCore::Node*);
int docCreateElement(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeFocus(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeBlur(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateTextNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeAppendChild(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeRemoveChild(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeInsertBefore(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeSetAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeRemoveAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);

/* document.title */
int titleGetter(AntbEngine* engine, AntbValue*, const char* name, AntbValue* out, void*)
{
    /* The exotic getter is called for every property of the document object, so
     * unknown names must fall through instead of returning the title. */
    if (!name || !g_document)
        return 0;

    if (!strcmp(name, "title")) {
        WebCore::String title = g_document->title();
        AntbValue* v = antb_make_string(engine, title.utf8().data());
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "body")) {
        if (!g_document->body())
            return 1;
        {
            AntbValue* v = wrapNode(engine, g_document->body());
            if (v) { antb_value_assign(out, v); antb_value_release(v); }
        }
        return 1;
    }

    if (!strcmp(name, "documentElement")) {
        if (!g_document->documentElement())
            return 1;
        {
            AntbValue* v = wrapNode(engine, g_document->documentElement());
            if (v) { antb_value_assign(out, v); antb_value_release(v); }
        }
        return 1;
    }

    if (!strcmp(name, "location")) {
        if (!g_locationObj) {
            AntbValue* obj = antb_make_object(engine);
            if (!obj)
                return 0;
            antb_define_property_rw(engine, obj, "href", locationGetter, NULL, NULL);
            g_locationObj = antb_retain(obj);   /* keep it alive across GC */
            antb_value_release(obj);
        }
        antb_value_assign(out, g_locationObj);
        return 1;
    }

    return 0;
}

/* document.location.<field> */
int locationGetter(AntbEngine* engine, AntbValue*, const char* name, AntbValue* out, void*)
{
    WebCore::KURL url;

    if (!g_document || !name)
        return 0;

    url = WebCore::KURL(g_document->URL());

    {
        WebCore::String value;
        WebCore::DeprecatedString deprecated = url.url();

        if (!strcmp(name, "href"))
            value = WebCore::String(deprecated);
        else if (!strcmp(name, "protocol"))
            value = url.protocol();
        else if (!strcmp(name, "host"))
            value = url.host();
        else if (!strcmp(name, "hostname"))
            value = url.host();
        else if (!strcmp(name, "pathname"))
            value = url.path();
        else if (!strcmp(name, "search"))
            value = url.query();
        else if (!strcmp(name, "hash"))
            value = url.ref();
        else
            return 0;

        {
            AntbValue* v = antb_make_string(engine, value.utf8().data());
            if (!v)
                return 0;
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

int titleSetter(AntbEngine* engine, AntbValue*, const char* name, const AntbValue* value, void*)
{
    (void)engine;

    if (!name || strcmp(name, "title") != 0)
        return 0;

    if (!g_document || !value)
        return 0;

    {
        char* text = antb_to_utf8(value);
        if (!text)
            return 0;
        g_document->setTitle(WebCore::String::fromUTF8(text));
        antb_free(text);
    }
    return 1;
}

} /* namespace */

namespace AntRuntime {

bool start()
{
    if (g_engine)
        return true;

    /* Ant needs the address of a stack variable from the calling thread. */
    volatile char stack_anchor = 0;
    g_engine = antb_engine_create((const void*)&stack_anchor);

    return g_engine != NULL;
}

void stop()
{
    if (g_engine) {
        antb_engine_destroy(g_engine);
        g_engine = NULL;
    }
}

bool isRunning()
{
    return g_engine != NULL;
}

int evaluate(const char* src, size_t len, char** out, char** err)
{
    AntbValue* value;

    if (out)
        *out = NULL;
    if (err)
        *err = NULL;

    if (!g_engine || !src)
        return 0;

    value = antb_eval(g_engine, src, (int)len, err);
    if (!value)
        return 0;

    if (out)
        *out = antb_to_utf8(value);

    antb_value_release(value);
    return 1;
}

void bindDocument(void* document)
{
    /* The Document object is replaced on every navigation, so always refresh
     * the pointer; only the JS shim objects are created once. */
    g_document = (WebCore::Document*)document;

    if (!g_engine || !g_document)
        return;

    if (g_documentBound) {
        if (g_docObj)
            antb_set_native(g_docObj, g_document, AGB_TAG_NODE);
        return;
    }

    {
        AntbValue* global   = antb_global(g_engine);
        AntbValue* window   = antb_make_object(g_engine);
        AntbValue* docObj   = antb_make_object(g_engine);

        if (!global || !window || !docObj) {
            antb_value_release(global);
            antb_value_release(window);
            antb_value_release(docObj);
            return;
        }

        antb_define_property_rw(g_engine, docObj, "title", titleGetter, titleSetter, NULL);

        if (!g_nodeClass) {
            g_nodeClass = antb_class_create(g_engine, "Node");
            if (g_nodeClass) {
                antb_class_add_method(g_engine, g_nodeClass, "getAttribute", nodeGetAttribute, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "addEventListener", nodeAddEventListener, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "dispatchEvent", nodeDispatchEvent, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "appendChild", nodeAppendChild, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "removeChild", nodeRemoveChild, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "insertBefore", nodeInsertBefore, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "setAttribute", nodeSetAttribute, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "focus", nodeFocus, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "blur", nodeBlur, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "removeAttribute", nodeRemoveAttribute, NULL);
            }
        }

        antb_define_method(g_engine, docObj, "getElementById", docGetElementById, NULL);
        antb_define_method(g_engine, docObj, "createElement", docCreateElement, NULL);
        antb_define_method(g_engine, docObj, "createTextNode", docCreateTextNode, NULL);
        antb_define_method(g_engine, docObj, "addEventListener", nodeAddEventListener, NULL);
        antb_define_method(g_engine, docObj, "dispatchEvent", nodeDispatchEvent, NULL);

        /* Document is a Node: let the node APIs work directly on it. */
        antb_set_native(docObj, g_document, AGB_TAG_NODE);

        antb_set_prop(g_engine, window, "document", docObj);
        antb_set_prop(g_engine, global, "window", window);
        antb_set_prop(g_engine, global, "document", docObj);

        g_docObj = antb_retain(docObj);

        antb_value_release(docObj);
        antb_value_release(window);
        antb_value_release(global);
    }

    g_documentBound = true;
}

} /* namespace AntRuntime */

/* ------------------------------------------------------------------------- */
/* DOM element binding (task 3)                                              */
/* ------------------------------------------------------------------------- */



namespace {

struct Listener {
    WebCore::Node* node;
    std::string    type;
    AntbValue*     fn;      /* rooted */
};

std::vector<Listener>* g_listeners = NULL;

WebCore::Node* nodeOf(AntbValue* self)
{
    return self ? (WebCore::Node*)antb_get_native(self, AGB_TAG_NODE) : 0;
}

int nodeGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    AntbValue* v = NULL;

    if (!n)
        return 0;

    if (!strcmp(name, "textContent")) {
        WebCore::String s = n->textContent();
        v = antb_make_string(engine, s.utf8().data());
    } else if (!strcmp(name, "nodeName")) {
        WebCore::String s = n->nodeName();
        v = antb_make_string(engine, s.utf8().data());
    } else if (!strcmp(name, "nodeType")) {
        v = antb_make_number(engine, (double)n->nodeType());
    } else if (!strcmp(name, "parentNode")) {
        if (n->parentNode()) {
            v = wrapNode(engine, n->parentNode());
        } else {
            return 1;   /* null */
        }
    } else if (!strcmp(name, "firstChild")) {
        if (n->firstChild()) {
            v = wrapNode(engine, n->firstChild());
        } else {
            return 1;
        }
    } else if (!strcmp(name, "lastChild")) {
        if (n->lastChild()) {
            v = wrapNode(engine, n->lastChild());
        } else {
            return 1;
        }
    } else if (!strcmp(name, "nextSibling")) {
        if (n->nextSibling()) {
            v = wrapNode(engine, n->nextSibling());
        } else {
            return 1;
        }
    } else if (!strcmp(name, "childNodes")) {
        {
            AntbValue* arr = antb_make_array(engine);
            WebCore::Node* c = n->firstChild();
            for (; c; c = c->nextSibling()) {
                AntbValue* w = wrapNode(engine, c);
                if (w) {
                    antb_array_push(engine, arr, w);
                    antb_value_release(w);
                }
            }
            antb_value_assign(out, arr);
            antb_value_release(arr);
        }
        return 1;
    } else if (!strcmp(name, "value")) {
        if (n->isElementNode()) {
            WebCore::Element* element = static_cast<WebCore::Element*>(n);
            if (element->hasTagName(WebCore::HTMLNames::inputTag)) {
                WebCore::String s = static_cast<WebCore::HTMLInputElement*>(element)->value();
                v = antb_make_string(engine, s.utf8().data());
            } else if (element->hasTagName(WebCore::HTMLNames::textareaTag)) {
                WebCore::String s = static_cast<WebCore::HTMLTextAreaElement*>(element)->value();
                v = antb_make_string(engine, s.utf8().data());
            } else {
                return 0;
            }
        } else {
            return 0;
        }
    } else if (!strcmp(name, "id") || !strcmp(name, "className") || !strcmp(name, "href")) {
        if (!n->isElementNode())
            return 0;
        {
            WebCore::String attrName = strcmp(name, "className") == 0
                ? WebCore::String("class") : WebCore::String(name);
            WebCore::String s = static_cast<WebCore::Element*>(n)->getAttribute(attrName);
            v = antb_make_string(engine, s.utf8().data());
        }
    } else {
        return 0;
    }

    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int nodeSetter(AntbEngine* engine, AntbValue* self, const char* name, const AntbValue* value, void*)
{
    WebCore::Node* n = nodeOf(self);

    (void)engine;

    if (!n || !value)
        return 0;

    if (!strcmp(name, "textContent")) {
        char* text = antb_to_utf8(value);
        if (!text)
            return 0;
        {
            WebCore::ExceptionCode ec = 0;
            n->setTextContent(WebCore::String::fromUTF8(text), ec);
        }
        antb_free(text);
        return 1;
    }

    if (!strcmp(name, "value") && n->isElementNode()) {
        char* text = antb_to_utf8(value);
        WebCore::Element* element = static_cast<WebCore::Element*>(n);
        if (!text)
            return 0;
        if (element->hasTagName(WebCore::HTMLNames::inputTag))
            static_cast<WebCore::HTMLInputElement*>(element)->setValue(WebCore::String::fromUTF8(text));
        else if (element->hasTagName(WebCore::HTMLNames::textareaTag))
            static_cast<WebCore::HTMLTextAreaElement*>(element)->setValue(WebCore::String::fromUTF8(text));
        antb_free(text);
        return 1;
    }

    if ((!strcmp(name, "id") || !strcmp(name, "className") || !strcmp(name, "href"))
            && n->isElementNode()) {
        char* text = antb_to_utf8(value);
        if (!text)
            return 0;
        {
            WebCore::ExceptionCode ec = 0;
            WebCore::String attrName = strcmp(name, "className") == 0
                ? WebCore::String("class") : WebCore::String(name);
            static_cast<WebCore::Element*>(n)->setAttribute(attrName,
                WebCore::String::fromUTF8(text), ec);
        }
        antb_free(text);
        return 1;
    }
    return 0;
}

int nodeGetAttribute(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* name = (argc > 0) ? antb_to_utf8(args[0]) : NULL;

    if (!n || !name) {
        antb_free(name);
        return 0;
    }

    {
        WebCore::String value;
        if (n->isElementNode())
            value = static_cast<WebCore::Element*>(n)->getAttribute(WebCore::String::fromUTF8(name));
        antb_free(name);

        {
            AntbValue* v = antb_make_string(engine, value.utf8().data());
            if (!v)
                return 0;
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

int nodeAddEventListener(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                         AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* type = (argc > 0) ? antb_to_utf8(args[0]) : NULL;

    (void)engine; (void)out;

    if (!n || !type || argc < 2) {
        antb_free(type);
        return 0;
    }

    if (!g_listeners)
        g_listeners = new std::vector<Listener>();

    {
        Listener l;
        l.node = n;
        l.type = type;
        l.fn   = antb_retain(args[1]);
        if (l.fn)
            g_listeners->push_back(l);
    }

    antb_free(type);
    return 1;
}

int nodeDispatchEvent(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* type = (argc > 0) ? antb_to_utf8(args[0]) : NULL;

    (void)engine; (void)out;

    if (!n || !type) {
        antb_free(type);
        return 0;
    }

    if (g_listeners) {
        AntbValue* evt = antb_make_object(engine);
        AntbValue* t   = antb_make_string(engine, type);

        if (evt && t)
            antb_set_prop(engine, evt, "type", t);

        for (size_t i = 0; i < g_listeners->size(); ++i) {
            Listener& l = (*g_listeners)[i];
            if (l.node == n && l.type == type) {
                AntbValue* argv[1];
                argv[0] = evt;
                antb_call(engine, l.fn, self, argv, evt ? 1 : 0, NULL);
            }
        }
        antb_value_release(t);
        antb_value_release(evt);
    }

    antb_free(type);
    return 1;
}

int docCreateElement(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char* tag;
    WebCore::ExceptionCode ec = 0;
    RefPtr<WebCore::Element> element;

    if (!g_document || argc < 1)
        return 1;

    tag = antb_to_utf8(args[0]);
    if (!tag)
        return 1;

    element = g_document->createElement(WebCore::String::fromUTF8(tag), ec);
    antb_free(tag);

    if (!element || ec)
        return 1;

    /* The wrapper outlives the RefPtr: keep the node alive for the JS handle.
     * The reference is never released (bounded by the document lifetime). */
    element->ref();

    {
        AntbValue* v = wrapNode(engine, element.get());
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

int docCreateTextNode(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    char* text;
    RefPtr<WebCore::Text> node;

    if (!g_document || argc < 1)
        return 1;

    text = antb_to_utf8(args[0]);
    if (!text)
        return 1;

    node = g_document->createTextNode(WebCore::String::fromUTF8(text));
    antb_free(text);

    if (!node)
        return 1;

    node->ref();

    {
        AntbValue* v = wrapNode(engine, node.get());
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

int nodeAppendChild(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                    AntbValue* out, void*)
{
    WebCore::Node* parent = nodeOf(self);
    WebCore::Node* child  = (argc > 0) ? nodeOf(args[0]) : 0;
    WebCore::ExceptionCode ec = 0;

    if (!parent || !child)
        return 1;

    parent->appendChild(child, ec);
    if (ec)
        return 1;

    {
        AntbValue* v = wrapNode(engine, child);
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

int nodeRemoveChild(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                    AntbValue* out, void*)
{
    WebCore::Node* parent = nodeOf(self);
    WebCore::Node* child  = (argc > 0) ? nodeOf(args[0]) : 0;
    WebCore::ExceptionCode ec = 0;

    (void)engine; (void)out;

    if (!parent || !child)
        return 1;

    child->ref();                 /* removal may drop the last reference */
    parent->removeChild(child, ec);
    child->deref();
    return 1;
}

int nodeInsertBefore(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    WebCore::Node* parent = nodeOf(self);
    WebCore::Node* child  = (argc > 0) ? nodeOf(args[0]) : 0;
    WebCore::Node* before = (argc > 1) ? nodeOf(args[1]) : 0;
    WebCore::ExceptionCode ec = 0;

    if (!parent || !child)
        return 1;

    parent->insertBefore(child, before, ec);

    {
        AntbValue* v = wrapNode(engine, child);
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

int nodeFocus(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
               AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);

    (void)engine; (void)args; (void)argc; (void)out;

    if (n && n->isElementNode())
        static_cast<WebCore::Element*>(n)->focus(true);
    return 1;
}

int nodeBlur(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
              AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);

    (void)engine; (void)args; (void)argc; (void)out;

    if (n && n->isElementNode())
        static_cast<WebCore::Element*>(n)->blur();
    return 1;
}

int nodeSetAttribute(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* name;
    char* value = 0;
    WebCore::ExceptionCode ec = 0;

    (void)engine; (void)out;

    if (!n || !n->isElementNode() || argc < 1)
        return 1;

    name = antb_to_utf8(args[0]);
    if (argc > 1)
        value = antb_to_utf8(args[1]);

    if (name) {
        static_cast<WebCore::Element*>(n)->setAttribute(
            WebCore::String::fromUTF8(name),
            WebCore::String::fromUTF8(value ? value : ""), ec);
    }

    antb_free(name);
    antb_free(value);
    return 1;
}

int nodeRemoveAttribute(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                        AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* name;
    WebCore::ExceptionCode ec = 0;

    (void)engine; (void)out;

    if (!n || !n->isElementNode() || argc < 1)
        return 1;

    name = antb_to_utf8(args[0]);
    if (name) {
        static_cast<WebCore::Element*>(n)->removeAttribute(WebCore::String::fromUTF8(name), ec);
        antb_free(name);
    }
    return 1;
}

AntbValue* wrapNode(AntbEngine* engine, WebCore::Node* node)
{
    AntbValue* v;

    if (!engine || !node || !g_nodeClass)
        return NULL;

    v = antb_wrap_native_as(engine, g_nodeClass, node, AGB_TAG_NODE);
    if (!v)
        return NULL;

    /* per-instance property table: textContent/nodeName/nodeType */
    antb_define_property_rw(engine, v, "textContent", nodeGetter, nodeSetter, NULL);
    return v;
}

int docGetElementById(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    char* id;
    WebCore::Element* element;

    if (!g_document || argc < 1)
        return 0;

    id = antb_to_utf8(args[0]);
    if (!id)
        return 0;

    element = g_document->getElementById(WebCore::AtomicString(id));
    antb_free(id);

    if (!element)
        return 1;   /* undefined */

    {
        AntbValue* v = wrapNode(engine, element);
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

} /* namespace */

namespace AntRuntime {

void dispatchEvent(void* nodePtr, void* eventPtr)
{
    WebCore::Node*  node  = (WebCore::Node*)nodePtr;
    WebCore::Event* event = (WebCore::Event*)eventPtr;

    if (!g_engine || !node || !event || !g_listeners)
        return;

    {
        std::string type = event->type().domString().utf8().data();
        AntbValue* self = wrapNode(g_engine, node);
        AntbValue* evt  = NULL;

        if (!self)
            return;

        /* the event object handed to JS */
        evt = antb_make_object(g_engine);
        if (evt) {
            AntbValue* t = antb_make_string(g_engine, type.c_str());
            antb_set_prop(g_engine, evt, "type", t);
            antb_value_release(t);
        }

        for (size_t i = 0; i < g_listeners->size(); ++i) {
            Listener& l = (*g_listeners)[i];
            if (l.node != node || l.type != type)
                continue;

            {
                AntbValue* args[1];
                args[0] = evt;
                antb_call(g_engine, l.fn, self, args, evt ? 1 : 0, NULL);
            }
        }

        antb_value_release(evt);
        antb_value_release(self);
    }
}

} /* namespace AntRuntime */