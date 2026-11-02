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
#include "HTMLElement.h"
#include "NodeList.h"
#include "DocumentFragment.h"
#include "ResourceHandle.h"
#include "ResourceHandleClient.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
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
WebCore::Document* documentOf(AntbValue* self);
int docGetElementById(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeDispatchEvent(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int locationGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
AntbValue* wrapNode(AntbEngine*, WebCore::Node*);
int nodeGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int attrsGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int nodeGetElementsByTagName(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeCloneNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateDocumentFragment(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeReplaceChild(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeHasAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeContains(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeCompareDocumentPosition(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeRemove(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int nodeClassList(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int classListAdd(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int classListRemove(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int classListContains(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int classListToggle(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetElementsByClassName(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeQuerySelectorAll(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeQuerySelector(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int imageConstructor(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
/* ------------------------------------------------------------------------- */
/* Asynchronous requests (XHR with async = true)                              */
/* ------------------------------------------------------------------------- */

/* One in-flight request. The handle is ref'd by this object, so it stays alive
 * until the job is collected; the JS callback is retained and released when the
 * response has been delivered. */
struct XhrJob : public WebCore::ResourceHandleClient {
    XhrJob(AntbEngine* engine, AntbValue* callback)
        : m_engine(engine)
        , m_callback(antb_retain(callback))
        , m_status(0)
        , m_done(false)
        , m_failed(false)
    {
    }

    virtual ~XhrJob()
    {
        if (m_callback)
            antb_value_release(m_callback);
    }

    void start(const char* url)
    {
        WebCore::KURL kurl((const char*)url);
        WebCore::ResourceRequest request(kurl);

        m_handle = WebCore::ResourceHandle::create(request, this, false, true, false);
    }

    virtual void didReceiveResponse(WebCore::ResourceHandle*, const WebCore::ResourceResponse& response)
    {
        m_status = response.httpStatusCode();
    }

    virtual void didReceiveData(WebCore::ResourceHandle*, const char* data, int length, int)
    {
        if (length > 0)
            m_body.append(data, length);
    }

    virtual void didFinishLoading(WebCore::ResourceHandle*)
    {
        m_done = true;
    }

    virtual void didFail(WebCore::ResourceHandle*, const WebCore::ResourceError&)
    {
        m_done = true;
        m_failed = true;
    }

    void deliver()
    {
        if (!m_callback || !m_engine)
            return;

        {
            WebCore::String text(m_body.data(), m_body.size());
            AntbValue* textValue = antb_make_string(m_engine, text.utf8().data());
            AntbValue* statusValue = antb_make_number(m_engine, m_failed ? 0.0 : (double)m_status);
            AntbValue* argArray[2];
            AntbValue* result;

            if (!textValue || !statusValue) {
                if (textValue) antb_value_release(textValue);
                if (statusValue) antb_value_release(statusValue);
                return;
            }

            argArray[0] = statusValue;
            argArray[1] = textValue;
            result = antb_call(m_engine, m_callback, NULL, argArray, 2, NULL);
            if (result)
                antb_value_release(result);

            antb_value_release(textValue);
            antb_value_release(statusValue);
        }

        antb_value_release(m_callback);
        m_callback = NULL;
    }

    bool done() const { return m_done; }

    bool deliverAndMaybeFinish()
    {
        if (!m_done)
            return false;

        deliver();
        return true;
    }

    AntbEngine*                     m_engine;
    AntbValue*                      m_callback;
    RefPtr<WebCore::ResourceHandle> m_handle;
    Vector<char>                    m_body;
    int                             m_status;
    bool                            m_done;
    bool                            m_failed;
};

static Vector<XhrJob*>& xhrJobs()
{
    static Vector<XhrJob*>* jobs = new Vector<XhrJob*>();
    return *jobs;
}

/* Delivers finished responses and frees jobs. Called from the start of every
 * evaluation: the network pump runs outside JS, so delivery has to be flushed
 * somewhere that is known to be safe. */
static void collectXhrJobs()
{
    Vector<XhrJob*>& jobs = xhrJobs();

    for (unsigned i = 0; i < jobs.size(); ) {
        if (jobs[i]->deliverAndMaybeFinish()) {
            delete jobs[i];
            jobs.remove(i);
        } else {
            ++i;
        }
    }
}

int globalHttpRequest(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    char* urlText;
    XhrJob* job;

    (void)out;

    if (argc < 2)
        return 1;

    urlText = antb_to_utf8(args[0]);
    if (!urlText)
        return 1;

    job = new XhrJob(engine, args[1]);
    xhrJobs().append(job);
    job->start(urlText);

    antb_free(urlText);
    return 1;
}

int globalHttpGet(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpRequest(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeSetter(AntbEngine*, AntbValue*, const char*, const AntbValue*, void*);
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
int titleGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
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

    if (!strcmp(name, "classList")) {
        return nodeClassList(engine, self, NULL, 0, out, NULL);
    }

    if (!strcmp(name, "head")) {
        RefPtr<WebCore::NodeList> heads = g_document->getElementsByTagName("head");
        WebCore::Node* headNode = (heads && heads->length()) ? heads->item(0) : 0;
        if (!headNode)
            return 1;
        {
            AntbValue* v = wrapNode(engine, headNode);
            if (v) { antb_value_assign(out, v); antb_value_release(v); }
        }
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

    if (!strcmp(name, "forms") || !strcmp(name, "images") || !strcmp(name, "links")
            || !strcmp(name, "anchors") || !strcmp(name, "scripts") || !strcmp(name, "embeds")
            || !strcmp(name, "plugins") || !strcmp(name, "applets")) {
        /* Collections are exposed as empty arrays: scripts only read .length or
         * iterate, and the real NodeLists are not wrapped yet. */
        AntbValue* empty = antb_make_array(engine);
        if (!empty)
            return 0;
        antb_value_assign(out, empty);
        antb_value_release(empty);
        return 1;
    }

    if (!strcmp(name, "cookie")) {
        AntbValue* v = antb_make_string(engine, "");
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "readyState")) {
        AntbValue* v = antb_make_string(engine, "complete");
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "URL") || !strcmp(name, "documentURI")) {
        WebCore::String url = g_document->URL();
        AntbValue* v = antb_make_string(engine, url.utf8().data());
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "referrer") || !strcmp(name, "domain") || !strcmp(name, "characterSet")
            || !strcmp(name, "charset") || !strcmp(name, "compatMode")) {
        AntbValue* v = antb_make_string(engine, strcmp(name, "compatMode") == 0 ? "CSS1Compat" : "");
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
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

    /* Everything else falls through to the node accessors: the document is a
     * Node, and the single exotic getter installed on this object would
     * otherwise swallow nodeType/nodeName/parentNode/... (jQuery's Sizzle
     * bails out when doc.nodeType is undefined). */
    return nodeGetter(engine, self, name, out, NULL);
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

int titleSetter(AntbEngine* engine, AntbValue* self, const char* name, const AntbValue* value, void*)
{
    if (!name)
        return 0;

    if (!strcmp(name, "title")) {
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

    return nodeSetter(engine, self, name, value, NULL);
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

    collectXhrJobs();

    /* Page scripts are wrapped in try/catch so engine-thrown errors come with a
     * stack: the engine's own TypeErrors carry no position information. `var`
     * inside a try block is still global, so top-level declarations keep their
     * meaning; only top-level let/const become block scoped, which pages in
     * practice do not rely on. */
    {
        size_t wrappedLen = len + 64;
        char* wrapped = (char*)malloc(wrappedLen);
        if (!wrapped)
            return 0;

        {
            int written = snprintf(wrapped, wrappedLen,
                                   "try{\n%.*s\n}catch(__agbE){__agbLastError=__agbDescribe(__agbE);}\n",
                                   (int)len, src);
            if (written <= 0 || (size_t)written >= wrappedLen) {
                free(wrapped);
                value = antb_eval(g_engine, src, (int)len, err);
                if (!value)
                    return 0;
                if (out)
                    *out = antb_to_utf8(value);
                antb_value_release(value);
                return 1;
            }

            value = antb_eval(g_engine, wrapped, written, err);
        }
        free(wrapped);
    }

    if (!value) {
        /* A syntax error still surfaces through the eval result. */
        return 0;
    }

    antb_value_release(value);

    /* Did the wrapped script throw? */
    {
        AntbValue* global = antb_global(g_engine);
        AntbValue* reported = global ? antb_get_prop(g_engine, global, "__agbLastError") : NULL;
        bool threw = false;

        if (reported) {
            char* text = antb_to_utf8(reported);
            if (text && text[0]) {
                threw = true;
                if (err) {
                    *err = (char*)malloc(strlen(text) + 1);
                    if (*err)
                        strcpy(*err, text);
                }
            }
            antb_free(text);
            antb_value_release(reported);
        }
        antb_value_release(global);

        /* Reset for the next evaluation. */
        if (threw) {
            AntbValue* global2 = antb_global(g_engine);
            if (global2) {
                antb_delete_prop(g_engine, global2, "__agbLastError");
                antb_value_release(global2);
            }
            return 0;
        }
    }

    if (out)
        *out = NULL;
    return 1;
}

/* Small JS-side shims, evaluated once per document: constructors and module
 * helpers are much easier to express in JS than through the C bridge. */
static const char kPrelude[] =
"(function(g){"
"  if (typeof g.Image === 'undefined' && g.document) {"
"    g.Image = function Image() { return g.document.createElement('img'); };"
"  }"
"  g.__agbDescribe = function (e) {"
"    try {"
"      if (e && e.stack) return String(e.stack);"
"      if (e && e.message) return String(e.name) + ': ' + String(e.message);"
"      return String(e);"
"    } catch (_) { return 'unknown error'; }"
"  };"
"  if (g.navigator) {"
"    if (!g.navigator.platform) g.navigator.platform = 'Win32';"
"    if (!g.navigator.language) g.navigator.language = 'zh-CN';"
"    if (!g.navigator.appVersion) g.navigator.appVersion = g.navigator.userAgent || '';"
"    if (!g.navigator.vendor) g.navigator.vendor = '';"
"    g.navigator.onLine = true;"
"    g.navigator.cookieEnabled = true;"
"    if (!g.navigator.appCodeName) g.navigator.appCodeName = 'Mozilla';"
"    if (!g.navigator.appName) g.navigator.appName = 'Netscape';"
"    if (!g.navigator.mimeTypes) g.navigator.mimeTypes = [];"
"    if (!g.navigator.plugins) g.navigator.plugins = [];"
"    if (!g.navigator.languages) g.navigator.languages = ['zh-CN', 'zh'];"
"    if (!g.navigator.hardwareConcurrency) g.navigator.hardwareConcurrency = 4;"
"    if (!g.navigator.maxTouchPoints) g.navigator.maxTouchPoints = 0;"
"  }"
"  {"
"    var _lsData = {};"
"    g.localStorage = {"
"      getItem: function (k) { return Object.prototype.hasOwnProperty.call(_lsData, k) ? _lsData[k] : null; },"
"      setItem: function (k, v) { _lsData[k] = String(v); },"
"      removeItem: function (k) { delete _lsData[k]; },"
"      clear: function () { _lsData = {}; },"
"      key: function (i) { var ks = Object.keys(_lsData); return i < ks.length ? ks[i] : null; }"
"    };"
"    g.sessionStorage = g.localStorage;"
"  }"
"  if (!g.requestIdleCallback) { g.requestIdleCallback = function (cb) { return 1; }; g.cancelIdleCallback = function () {}; }"
"  if (!g.createImageBitmap) g.createImageBitmap = function () { return null; };"
"  if (!g.getComputedStyle) g.getComputedStyle = function (el) { return (el && el.style) ? el.style : {}; };"
"  {"
"    var _ctors = ['Document','HTMLDocument','Element','HTMLElement','HTMLDivElement','Node','NodeList','HTMLCollection','Event','CustomEvent','MouseEvent','KeyboardEvent','DocumentFragment','Text','Comment','Attr','DOMParser','XMLSerializer','MutationObserver','Storage','Location','Navigator','Screen','History','Window','Performance','CSSStyleDeclaration'];"
"    for (var _ci = 0; _ci < _ctors.length; _ci++) {"
"      if (typeof g[_ctors[_ci]] === 'undefined') g[_ctors[_ci]] = function () { return null; };"
"    }"
"  }"
"  if (typeof g.XMLHttpRequest === 'undefined') {"
"    var XHR = function XMLHttpRequest() {"
"      this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = '';"
"      this.onreadystatechange = null; this.onload = null; this.onerror = null; this.onloadend = null; this.onabort = null;"
"      this._url = null; this._method = 'GET'; this._async = true; this._headers = {};"
"    };"
"    XHR.prototype.open = function (method, url, async) {"
"      this._method = method || 'GET'; this._url = url; this._async = (async !== false);"
"      this.readyState = 1; this._fire();"
"    };"
"    XHR.prototype.setRequestHeader = function (k, v) { this._headers[k] = v; };"
"    XHR.prototype.getAllResponseHeaders = function () { return ''; };"
"    XHR.prototype.getResponseHeader = function () { return null; };"
"    XHR.prototype.abort = function () { this.readyState = 0; if (this.onabort) this.onabort.call(this, this); };"
"    XHR.prototype.overrideMimeType = function () {};"
"    XHR.prototype.addEventListener = function (type, fn) { this['_ev_' + type] = fn; };"
"    XHR.prototype.removeEventListener = function (type) { this['_ev_' + type] = null; };"
"    XHR.prototype._fire = function (type) {"
"      if (this.onreadystatechange) this.onreadystatechange.call(this, this);"
"      if (type) {"
"        var h = this['on' + type] || this['_ev_' + type];"
"        if (h) h.call(this, this);"
"      }"
"    };"
"    XHR.prototype._finish = function (status, text) {"
"      this.status = status;"
"      this.responseText = (text === undefined || text === null) ? '' : String(text);"
"      this.response = this.responseText;"
"      this.readyState = 4;"
"      this._fire();"
"      if (!status) this._fire('error');"
"      else this._fire('load');"
"      this._fire('loadend');"
"    };"
"    XHR.prototype.send = function () {"
"      var self = this;"
"      if (!this._async) {"
"        var text = null;"
"        try { text = __agbHttpGet(this._url); } catch (e) { text = null; }"
"        this._finish((text === undefined || text === null) ? 0 : 200, text);"
"        return;"
"      }"
"      /* Async: the C side owns the request and delivers (status, body) on a"
"       * later frame, so the UI keeps running. */"
"      __agbHttpRequest(this._url, function (status, text) { self._finish(status, text); });"
"    };"
"    g.XMLHttpRequest = XHR;"
"  }"
"  if (typeof g.define === 'undefined') {"
"    var mods = {};"
"    var def = function define(name, deps, factory) {"
"      if (typeof name !== 'string') { factory = deps; deps = name; name = null; }"
"      if (!(deps && typeof deps.length === 'number')) { factory = deps; deps = []; }"
"      var args = [], i;"
"      for (i = 0; i < deps.length; i++) args.push(mods[deps[i]] || {});"
"      var exports = {};"
"      var r = (typeof factory === 'function') ? factory.apply(exports, args) : exports;"
"      var mod = (r === undefined) ? exports : r;"
"      if (name) mods[name] = mod;"
"      return mod;"
"    };"
"    def.amd = {};"
"    g.define = def;"
"    if (typeof g.require === 'undefined') {"
"      g.require = function require(deps, cb) {"
"        if (typeof deps === 'string') return mods[deps] || {};"
"        var args = [], i;"
"        if (deps) for (i = 0; i < deps.length; i++) args.push(mods[deps[i]] || {});"
"        if (cb) cb.apply(null, args);"
"      };"
"    }"
"  }"
"})(window);";

void pumpAsyncRequests(void)
{
    collectXhrJobs();
}

void setFilename(const char* name)
{
    if (g_engine && name)
        antb_set_filename(g_engine, name);
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
        AntbValue* window   = global;   /* window IS the global object */
        AntbValue* docObj   = antb_make_object(g_engine);

        if (!global || !docObj) {
            antb_value_release(global);
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
                antb_class_add_method(g_engine, g_nodeClass, "getElementsByTagName", nodeGetElementsByTagName, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "cloneNode", nodeCloneNode, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "replaceChild", nodeReplaceChild, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "hasAttribute", nodeHasAttribute, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "contains", nodeContains, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "compareDocumentPosition", nodeCompareDocumentPosition, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "getElementsByClassName", nodeGetElementsByClassName, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "querySelectorAll", nodeQuerySelectorAll, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "querySelector", nodeQuerySelector, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "focus", nodeFocus, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "blur", nodeBlur, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "removeAttribute", nodeRemoveAttribute, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "remove", nodeRemove, NULL);
            }
        }

        antb_define_method(g_engine, docObj, "getElementById", docGetElementById, NULL);
        antb_define_method(g_engine, docObj, "createElement", docCreateElement, NULL);
        antb_define_method(g_engine, docObj, "createTextNode", docCreateTextNode, NULL);
        antb_define_method(g_engine, docObj, "createDocumentFragment", docCreateDocumentFragment, NULL);
        antb_define_method(g_engine, docObj, "addEventListener", nodeAddEventListener, NULL);
        antb_define_method(g_engine, docObj, "dispatchEvent", nodeDispatchEvent, NULL);
        /* The document is a plain object (not a Node wrapper), so the node
         * methods have to be installed on it explicitly. */
        antb_define_method(g_engine, docObj, "getElementsByTagName", nodeGetElementsByTagName, NULL);
        antb_define_method(g_engine, docObj, "getElementsByClassName", nodeGetElementsByClassName, NULL);
        antb_define_method(g_engine, docObj, "querySelector", nodeQuerySelector, NULL);
        antb_define_method(g_engine, docObj, "querySelectorAll", nodeQuerySelectorAll, NULL);
        antb_define_method(g_engine, docObj, "appendChild", nodeAppendChild, NULL);
        antb_define_method(g_engine, docObj, "removeChild", nodeRemoveChild, NULL);
        antb_define_method(g_engine, docObj, "insertBefore", nodeInsertBefore, NULL);
        antb_define_method(g_engine, docObj, "replaceChild", nodeReplaceChild, NULL);
        antb_define_method(g_engine, docObj, "getAttribute", nodeGetAttribute, NULL);
        antb_define_method(g_engine, docObj, "setAttribute", nodeSetAttribute, NULL);
        antb_define_method(g_engine, docObj, "hasAttribute", nodeHasAttribute, NULL);
        antb_define_method(g_engine, docObj, "cloneNode", nodeCloneNode, NULL);
        antb_define_method(g_engine, docObj, "contains", nodeContains, NULL);

        /* Document is a Node: let the node APIs work directly on it. */
        antb_set_native(docObj, g_document, AGB_TAG_NODE);

        /* window.addEventListener is what jQuery's ready setup calls first;
         * listeners live on the document node so both targets share one list. */
        antb_set_native(window, g_document, AGB_TAG_NODE);
        antb_define_method(g_engine, window, "addEventListener", nodeAddEventListener, NULL);
        antb_define_method(g_engine, window, "dispatchEvent", nodeDispatchEvent, NULL);

        antb_set_prop(g_engine, window, "document", docObj);
        antb_define_method(g_engine, global, "__agbHttpGet", globalHttpGet, NULL);
        antb_define_method(g_engine, global, "__agbHttpRequest", globalHttpRequest, NULL);

        /* Web pages read bare globals like `location` and `navigator`. */
        {
            AntbValue* loc = antb_make_object(g_engine);
            if (loc) {
                antb_define_property_rw(g_engine, loc, "href", locationGetter, NULL, NULL);
                antb_set_prop(g_engine, global, "location", loc);
                antb_set_prop(g_engine, docObj, "defaultView", global);
                antb_value_release(loc);
            }
            {
                AntbValue* nav = antb_make_object(g_engine);
                if (nav) {
                    AntbValue* ua = antb_make_string(g_engine, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) agave/1.0");
                    if (ua) {
                        antb_set_prop(g_engine, nav, "userAgent", ua);
                        antb_set_prop(g_engine, nav, "appName", ua);
                        antb_value_release(ua);
                    }
                    antb_set_prop(g_engine, global, "navigator", nav);
                    antb_value_release(nav);
                }
            }
            {
                AntbValue* scr = antb_make_object(g_engine);
                if (scr) {
                    AntbValue* v = antb_make_number(g_engine, 1920);
                    if (v) { antb_set_prop(g_engine, scr, "width", v); antb_value_release(v); }
                    v = antb_make_number(g_engine, 1080);
                    if (v) { antb_set_prop(g_engine, scr, "height", v); antb_value_release(v); }
                    antb_set_prop(g_engine, global, "screen", scr);
                    antb_value_release(scr);
                }
            }
        }
        antb_set_prop(g_engine, global, "window", window);
        antb_set_prop(g_engine, global, "document", docObj);

        g_docObj = antb_retain(docObj);

        antb_value_release(docObj);
        antb_value_release(global);
    }

    if (g_engine && antb_eval(g_engine, kPrelude, (int)strlen(kPrelude), NULL))
        ; /* bootstrap shims are best effort */

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

/* Any attribute name yields an object: jQuery reads .expando off
 * div.attributes[name] and would otherwise throw on undefined. */
int attrsGetter(AntbEngine* engine, AntbValue*, const char* name, AntbValue* out, void*)
{
    AntbValue* stub;

    (void)name;

    stub = antb_make_object(engine);
    if (!stub)
        return 0;

    antb_value_assign(out, stub);
    antb_value_release(stub);
    return 1;
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
    } else if (!strcmp(name, "previousSibling")) {
        if (n->previousSibling()) {
            v = wrapNode(engine, n->previousSibling());
        } else {
            return 1;
        }
    } else if (!strcmp(name, "ownerDocument")) {
        /* Not exposed: handing out a Document wrapper (shared or re-wrapped)
         * crashes the engine on baidu's jQuery path, and callers tolerate
         * undefined here (they guard with `context.ownerDocument && ...`). */
        return 1;
    } else if (!strcmp(name, "nodeValue")) {
        if (n->isTextNode() || n->isCommentNode())
            v = antb_make_string(engine, n->nodeValue().utf8().data());
        else
            return 1;
    } else if (!strcmp(name, "hasChildNodes")) {
        v = antb_make_number(engine, n->hasChildNodes() ? 1 : 0);
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
    } else if (!strcmp(name, "children")) {
        /* Element-only children, as an array (some libraries only read length). */
        AntbValue* arr = antb_make_array(engine);
        if (!arr)
            return 0;
        for (WebCore::Node* c = n->firstChild(); c; c = c->nextSibling()) {
            if (!c->isElementNode())
                continue;
            {
                AntbValue* w = wrapNode(engine, c);
                if (w) {
                    antb_array_push(engine, arr, w);
                    antb_value_release(w);
                }
            }
        }
        antb_value_assign(out, arr);
        antb_value_release(arr);
        return 1;
    } else if (!strcmp(name, "childElementCount")) {
        unsigned count = 0;
        for (WebCore::Node* c = n->firstChild(); c; c = c->nextSibling())
            if (c->isElementNode())
                ++count;
        {
            AntbValue* v = antb_make_number(engine, (double)count);
            if (!v)
                return 0;
            antb_value_assign(out, v);
            antb_value_release(v);
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
    } else if (!strcmp(name, "innerHTML")) {
        if (!n->isElementNode())
            return 0;
        {
            WebCore::Element* element = static_cast<WebCore::Element*>(n);
            if (!element->isHTMLElement())
                return 0;
            WebCore::String html = static_cast<WebCore::HTMLElement*>(element)->innerHTML();
            v = antb_make_string(engine, html.utf8().data());
        }
    } else if (!strcmp(name, "outerHTML")) {
        if (!n->isElementNode())
            return 0;
        {
            WebCore::Element* element = static_cast<WebCore::Element*>(n);
            if (!element->isHTMLElement())
                return 0;
            WebCore::String html = static_cast<WebCore::HTMLElement*>(element)->outerHTML();
            v = antb_make_string(engine, html.utf8().data());
        }
    } else if (!strcmp(name, "attributes")) {
        /* Attribute nodes are not exposed yet; an empty bag keeps code that only
         * probes it (jQuery's support tests) from throwing. */
        AntbValue* existing = antb_get_prop(engine, self, "__agbAttrs");
        if (existing && !antb_is_number(existing)) {
            antb_value_assign(out, existing);
            antb_value_release(existing);
            return 1;
        }
        antb_value_release(existing);
        {
            AntbValue* attrs = antb_make_object(engine);
            if (!attrs)
                return 0;
            antb_define_property_rw(engine, attrs, "x", attrsGetter, NULL, NULL);
            antb_set_prop(engine, self, "__agbAttrs", attrs);
            antb_value_assign(out, attrs);
            antb_value_release(attrs);
        }
        return 1;
    } else if (!strcmp(name, "style")) {
        /* Real CSSStyleDeclaration is not exposed; a plain object keeps scripts
         * that only poke at style properties working. */
        AntbValue* existing = antb_get_prop(engine, self, "__agbStyle");
        if (existing && !antb_is_number(existing)) {
            antb_value_assign(out, existing);
            antb_value_release(existing);
            return 1;
        }
        antb_value_release(existing);
        {
            AntbValue* style = antb_make_object(engine);
            if (!style)
                return 0;
            antb_set_prop(engine, self, "__agbStyle", style);
            antb_value_assign(out, style);
            antb_value_release(style);
        }
        return 1;
    } else if (!strcmp(name, "classList")) {
        return nodeClassList(engine, self, NULL, 0, out, NULL);
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

    if (!strcmp(name, "outerHTML") && n->isElementNode()) {
        WebCore::Element* element = static_cast<WebCore::Element*>(n);
        char* text = antb_to_utf8(value);
        if (!element->isHTMLElement() || !text) {
            antb_free(text);
            return 0;
        }
        {
            WebCore::ExceptionCode ec = 0;
            static_cast<WebCore::HTMLElement*>(element)->setOuterHTML(WebCore::String::fromUTF8(text), ec);
        }
        antb_free(text);
        return 1;
    }

    if (!strcmp(name, "innerHTML") && n->isElementNode()) {
        WebCore::Element* element = static_cast<WebCore::Element*>(n);
        char* text = antb_to_utf8(value);
        if (!element->isHTMLElement() || !text) {
            antb_free(text);
            return 0;
        }
        {
            WebCore::ExceptionCode ec = 0;
            static_cast<WebCore::HTMLElement*>(element)->setInnerHTML(WebCore::String::fromUTF8(text), ec);
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

int docCreateElement(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char* tag;
    WebCore::ExceptionCode ec = 0;
    RefPtr<WebCore::Element> element;

    if (!documentOf(self) || argc < 1)
        return 1;

    tag = antb_to_utf8(args[0]);
    if (!tag)
        return 1;

    element = documentOf(self)->createElement(WebCore::String::fromUTF8(tag), ec);
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

int docCreateTextNode(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    char* text;
    RefPtr<WebCore::Text> node;

    if (!documentOf(self) || argc < 1)
        return 1;

    text = antb_to_utf8(args[0]);
    if (!text)
        return 1;

    node = documentOf(self)->createTextNode(WebCore::String::fromUTF8(text));
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

int docCreateDocumentFragment(AntbEngine* engine, AntbValue* self, AntbValue**, int,
                              AntbValue* out, void*)
{
    RefPtr<WebCore::DocumentFragment> fragment;

    if (!documentOf(self))
        return 1;

    fragment = documentOf(self)->createDocumentFragment();
    if (!fragment)
        return 1;

    fragment->ref();
    {
        AntbValue* wrapped = wrapNode(engine, fragment.get());
        if (wrapped) {
            antb_value_assign(out, wrapped);
            antb_value_release(wrapped);
        }
    }
    return 1;
}

int nodeReplaceChild(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    WebCore::Node* parent = nodeOf(self);
    WebCore::Node* child  = (argc > 0) ? nodeOf(args[0]) : 0;
    WebCore::Node* old    = (argc > 1) ? nodeOf(args[1]) : 0;
    WebCore::ExceptionCode ec = 0;

    (void)engine; (void)out;

    if (!parent || !child || !old)
        return 1;

    child->ref();
    parent->replaceChild(child, old, ec);
    child->deref();
    return 1;
}

int nodeHasAttribute(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* name;

    (void)engine;

    if (!n || !n->isElementNode() || argc < 1)
        return 1;

    name = antb_to_utf8(args[0]);
    if (name) {
        bool has = static_cast<WebCore::Element*>(n)->hasAttribute(WebCore::String::fromUTF8(name));
        AntbValue* v = antb_make_number(engine, has ? 1 : 0);
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        antb_free(name);
    }
    return 1;
}

int nodeContains(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                 AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    WebCore::Node* other = (argc > 0) ? nodeOf(args[0]) : 0;

    (void)engine;

    if (!n)
        return 1;

    {
        AntbValue* v = antb_make_number(engine, (other && (other == n || other->isDescendantOf(n))) ? 1 : 0);
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

static void pushWrappedNode(AntbEngine* engine, AntbValue* array, WebCore::Node* node)
{
    AntbValue* wrapped = wrapNode(engine, node);
    if (wrapped) {
        antb_array_push(engine, array, wrapped);
        antb_value_release(wrapped);
    }
}

/* Elements whose class attribute contains `name` (simple token match). */
int nodeGetElementsByClassName(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                               AntbValue* out, void*)
{
    WebCore::Node* root = nodeOf(self);
    char* name;
    AntbValue* arr;

    if (!root || argc < 1)
        return 1;

    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    arr = antb_make_array(engine);
    if (!arr) {
        antb_free(name);
        return 1;
    }

    {
        WebCore::String wanted = WebCore::String::fromUTF8(name);
        for (WebCore::Node* n = root->firstChild(); n; n = n->traverseNextNode(root)) {
            if (!n->isElementNode())
                continue;
            {
                WebCore::String cls = static_cast<WebCore::Element*>(n)->getAttribute(WebCore::String("class"));
                if (cls.contains(wanted, false))
                    pushWrappedNode(engine, arr, n);
            }
        }
    }

    antb_free(name);
    antb_value_assign(out, arr);
    antb_value_release(arr);
    return 1;
}

/* Selector support: descendant selectors made of #id, .class and tag parts
 * ("#s-top-bar .s-top-tabs", "div.foo", "a"). Anything fancier yields an
 * empty list, which is what most callers already handle. */
static void matchSimpleSelector(WebCore::Node* root, const WebCore::String& part,
                                Vector<WebCore::Node*>& matches)
{
    if (part.isEmpty())
        return;

    if (part[0] == '#') {
        WebCore::String id = part.substring(1);
        WebCore::Element* element = root->document() ? root->document()->getElementById(WebCore::AtomicString(id)) : 0;
        if (element && (element == root || element->isDescendantOf(root)))
            matches.append(element);
        return;
    }

    if (part[0] == '.') {
        WebCore::String wanted = part.substring(1);
        for (WebCore::Node* n = root->firstChild(); n; n = n->traverseNextNode(root)) {
            if (!n->isElementNode())
                continue;
            {
                WebCore::String cls = static_cast<WebCore::Element*>(n)->getAttribute(WebCore::String("class"));
                if (cls.contains(wanted, false))
                    matches.append(n);
            }
        }
        return;
    }

    {
        RefPtr<WebCore::NodeList> list = root->getElementsByTagName(part);
        if (list) {
            unsigned count = list->length();
            for (unsigned i = 0; i < count; ++i)
                if (WebCore::Node* child = list->item(i))
                    matches.append(child);
        }
    }
}

int nodeQuerySelectorAll(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                         AntbValue* out, void*)
{
    WebCore::Node* root = nodeOf(self);
    char* selector;
    AntbValue* arr;

    if (!root || argc < 1)
        return 1;

    selector = antb_to_utf8(args[0]);
    if (!selector)
        return 1;

    arr = antb_make_array(engine);
    if (!arr) {
        antb_free(selector);
        return 1;
    }

    {
        WebCore::String text = WebCore::String::fromUTF8(selector);
        Vector<WebCore::String> parts;
        unsigned partStart = 0;

        for (unsigned i = 0; i <= text.length(); ++i) {
            if (i == text.length() || text[i] == ' ') {
                if (i > partStart)
                    parts.append(text.substring(partStart, i - partStart));
                partStart = i + 1;
            }
        }

        if (parts.size() == 1) {
            Vector<WebCore::Node*> matches;
            matchSimpleSelector(root, parts[0], matches);
            for (unsigned i = 0; i < matches.size(); ++i)
                pushWrappedNode(engine, arr, matches[i]);
        } else if (parts.size() > 1) {
            Vector<WebCore::Node*> current;
            matchSimpleSelector(root, parts[0], current);

            for (unsigned p = 1; p < parts.size() && !current.isEmpty(); ++p) {
                Vector<WebCore::Node*> next;
                for (unsigned i = 0; i < current.size(); ++i)
                    matchSimpleSelector(current[i], parts[p], next);
                current = next;
            }

            for (unsigned i = 0; i < current.size(); ++i)
                pushWrappedNode(engine, arr, current[i]);
        }
    }

    antb_free(selector);
    antb_value_assign(out, arr);
    antb_value_release(arr);
    return 1;
}
/* Returns the first match: callers expect an element, not a list. */
int nodeQuerySelector(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    AntbValue* holder;

    if (argc < 1)
        return 1;

    holder = antb_make_array(engine);
    if (!holder)
        return 0;

    if (nodeQuerySelectorAll(engine, self, args, argc, holder, NULL)) {
        AntbValue* first = antb_array_get(holder, 0);
        if (first) {
            antb_value_assign(out, first);
            antb_value_release(first);
        }
    }

    antb_value_release(holder);
    return 1;
}

/* new Image() → an <img> element */
int imageConstructor(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    WebCore::ExceptionCode ec = 0;
    RefPtr<WebCore::Element> img;

    if (!g_document)
        return 1;

    img = g_document->createElement(WebCore::String("img"), ec);
    if (!img || ec)
        return 1;

    img->ref();
    {
        AntbValue* wrapped = wrapNode(engine, img.get());
        if (wrapped) {
            antb_value_assign(out, wrapped);
            antb_value_release(wrapped);
        }
    }
    (void)args; (void)argc;
    return 1;
}

/* Synchronous GET used by the XMLHttpRequest shim in the prelude. */
int globalHttpGet(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                  AntbValue* out, void*)
{
    char* urlText;

    if (argc < 1)
        return 1;

    urlText = antb_to_utf8(args[0]);
    if (!urlText)
        return 1;

    {
        WebCore::KURL url((const char*)urlText);
        WebCore::ResourceRequest request(url);
        WebCore::ResourceError error;
        WebCore::ResourceResponse response;
        Vector<char> data;

        antb_free(urlText);

        WebCore::ResourceHandle::loadResourceSynchronously(request, error, response, data);
        if (!error.isNull())
            return 1;   /* undefined → the shim reports status 0 */

        {
            WebCore::String body(data.data(), data.size());
            AntbValue* v = antb_make_string(engine, body.utf8().data());
            if (!v)
                return 1;
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

int nodeCloneNode(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                  AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    bool deep = (argc > 0) && antb_to_boolean(args[0]);
    RefPtr<WebCore::Node> copy;

    if (!n)
        return 1;

    copy = n->cloneNode(deep);
    if (!copy)
        return 1;

    copy->ref();   /* the wrapper outlives this RefPtr */
    {
        AntbValue* wrapped = wrapNode(engine, copy.get());
        if (wrapped) {
            antb_value_assign(out, wrapped);
            antb_value_release(wrapped);
        }
    }
    return 1;
}

/* Libraries feature-test this; only "disconnected" is ever needed. */
int nodeCompareDocumentPosition(AntbEngine* engine, AntbValue*, AntbValue**, int,
                                AntbValue* out, void*)
{
    AntbValue* v = antb_make_number(engine, 1);
    if (!v)
        return 0;
    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

/* element.remove(): detach from the parent, like the DOM spec method. */
int nodeRemove(AntbEngine*, AntbValue* self, AntbValue**, int, AntbValue*, void*)
{
    WebCore::Node* n = nodeOf(self);

    if (n && n->parentNode()) {
        WebCore::ExceptionCode ec = 0;
        WebCore::Node* parent = n->parentNode();

        n->ref();                 /* removal may drop the last reference */
        parent->removeChild(n, ec);
        n->deref();
    }

    return 1;
}

/* classList: backed directly by the class attribute. The Element pointer is
 * handed to the callbacks as userdata and kept alive by a ref. */
static WebCore::String readClassTokens(WebCore::Element* el)
{
    return el->getAttribute(WebCore::String("class"));
}

static void writeClassTokens(WebCore::Element* el, const WebCore::String& tokens)
{
    WebCore::ExceptionCode ec = 0;
    el->setAttribute(WebCore::String("class"), tokens, ec);
}

static bool tokenListContains(const WebCore::String& tokens, const WebCore::String& token)
{
    unsigned start = 0;

    for (unsigned i = 0; i <= tokens.length(); ++i) {
        if (i == tokens.length() || tokens[i] == ' ') {
            if (i > start && tokens.substring(start, i - start) == token)
                return true;
            start = i + 1;
        }
    }
    return false;
}

static int classListAdd(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue*, void* user)
{
    WebCore::Element* el = static_cast<WebCore::Element*>(user);
    char* token;

    if (!el || argc < 1)
        return 1;

    token = antb_to_utf8(args[0]);
    if (!token)
        return 1;

    {
        WebCore::String name = WebCore::String::fromUTF8(token);
        WebCore::String tokens = readClassTokens(el);
        if (!name.isEmpty() && !tokenListContains(tokens, name))
            writeClassTokens(el, tokens.isEmpty() ? name : tokens + " " + name);
    }

    antb_free(token);
    return 1;
}

static int classListRemove(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue*, void* user)
{
    WebCore::Element* el = static_cast<WebCore::Element*>(user);
    char* token;

    if (!el || argc < 1)
        return 1;

    token = antb_to_utf8(args[0]);
    if (!token)
        return 1;

    {
        WebCore::String name = WebCore::String::fromUTF8(token);
        WebCore::String tokens = readClassTokens(el);
        WebCore::String rebuilt;
        unsigned start = 0;

        for (unsigned i = 0; i <= tokens.length(); ++i) {
            if (i == tokens.length() || tokens[i] == ' ') {
                if (i > start) {
                    WebCore::String part = tokens.substring(start, i - start);
                    if (part != name)
                        rebuilt = rebuilt.isEmpty() ? part : rebuilt + " " + part;
                }
                start = i + 1;
            }
        }

        writeClassTokens(el, rebuilt);
    }

    antb_free(token);
    return 1;
}

static int classListContains(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                             AntbValue* out, void* user)
{
    WebCore::Element* el = static_cast<WebCore::Element*>(user);
    char* token;
    bool found = false;

    if (!el || argc < 1)
        return 1;

    token = antb_to_utf8(args[0]);
    if (!token)
        return 1;

    found = tokenListContains(readClassTokens(el), WebCore::String::fromUTF8(token));
    antb_free(token);

    {
        AntbValue* v = antb_make_number(engine, found ? 1.0 : 0.0);
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
    }
    return 1;
}

static int classListToggle(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void* user)
{
    WebCore::Element* el = static_cast<WebCore::Element*>(user);
    char* token;
    bool contains = false;

    if (!el || argc < 1)
        return 1;

    token = antb_to_utf8(args[0]);
    if (!token)
        return 1;

    contains = tokenListContains(readClassTokens(el), WebCore::String::fromUTF8(token));
    antb_free(token);

    if (contains)
        classListRemove(engine, self, args, argc, out, user);
    else
        classListAdd(engine, self, args, argc, out, user);

    {
        AntbValue* v = antb_make_number(engine, contains ? 0.0 : 1.0);
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
    }
    return 1;
}

static int nodeClassList(AntbEngine* engine, AntbValue* self, AntbValue**, int,
                         AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    AntbValue* obj;

    if (!n || !n->isElementNode())
        return 1;

    {
        WebCore::Element* el = static_cast<WebCore::Element*>(n);

        el->ref();   /* lives as long as the classList object */
        obj = antb_make_object(engine);
        if (!obj) {
            el->deref();
            return 0;
        }

        antb_define_method(engine, obj, "add", classListAdd, el);
        antb_define_method(engine, obj, "remove", classListRemove, el);
        antb_define_method(engine, obj, "contains", classListContains, el);
        antb_define_method(engine, obj, "toggle", classListToggle, el);
        antb_value_assign(out, obj);
        antb_value_release(obj);
    }
    return 1;
}


int nodeGetElementsByTagName(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                             AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* name;
    RefPtr<WebCore::NodeList> list;
    AntbValue* arr;

    if (!n || argc < 1)
        return 1;

    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    list = n->getElementsByTagName(WebCore::String::fromUTF8(name));
    antb_free(name);

    arr = antb_make_array(engine);
    if (!arr)
        return 1;

    if (list) {
        unsigned count = list->length();
        for (unsigned i = 0; i < count; ++i) {
            WebCore::Node* child = list->item(i);
            AntbValue* wrapped;
            if (!child)
                continue;
            wrapped = wrapNode(engine, child);
            if (wrapped) {
                antb_array_push(engine, arr, wrapped);
                antb_value_release(wrapped);
            }
        }
    }

    antb_value_assign(out, arr);
    antb_value_release(arr);
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

/* The document-level helpers work on whatever Document the receiver wraps,
 * so they can also be installed on plain document wrappers. */
WebCore::Document* documentOf(AntbValue* self)
{
    WebCore::Node* n = nodeOf(self);
    if (n && n->nodeType() == 9)
        return static_cast<WebCore::Document*>(n);
    return g_document;
}


int docGetElementById(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    char* id;
    WebCore::Element* element;

    if (!documentOf(self) || argc < 1)
        return 0;

    id = antb_to_utf8(args[0]);
    if (!id)
        return 0;

    element = documentOf(self)->getElementById(WebCore::AtomicString(id));
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

void dispatchWindowEvent(const char* type)
{
    AntbValue* self;

    if (!g_engine || !g_document || !type || !g_listeners)
        return;

    self = wrapNode(g_engine, g_document);
    if (!self)
        return;

    {
        AntbValue* evt = antb_make_object(g_engine);
        if (evt) {
            AntbValue* t = antb_make_string(g_engine, type);
            if (t) {
                antb_set_prop(g_engine, evt, "type", t);
                antb_value_release(t);
            }
        }

        for (size_t i = 0; i < g_listeners->size(); ++i) {
            Listener& l = (*g_listeners)[i];
            if (l.node != g_document || l.type != type)
                continue;
            {
                AntbValue* args[1];
                args[0] = evt;
                antb_call(g_engine, l.fn, self, args, evt ? 1 : 0, NULL);
            }
        }

        antb_value_release(evt);
    }

    antb_value_release(self);
}

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
