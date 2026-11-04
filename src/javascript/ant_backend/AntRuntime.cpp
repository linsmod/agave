/*
 * AntRuntime.cpp - C++ facade over the Ant bridge (antbridge.dll).
 */

#include "config.h"

#include "AntRuntime.h"

#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define AGB_PATH_CHARS 260
#include <string.h>
#include <map>

#include "AntBridge.h"
#include "../../webcore/platform/network/native/NetworkSession.h"
#include "../platform/network/native/NetLog.h"

#include "Document.h"
#include "DOMImplementation.h"
#include "HTMLDocument.h"
#include "Element.h"
#include "RenderObject.h"
#include "Node.h"
#include "CSSComputedStyleDeclaration.h"
#include "CSSMutableStyleDeclaration.h"
#include "StyledElement.h"
#include "PlatformString.h"
#include "Event.h"
#include "MouseEvent.h"
#include "KeyboardEvent.h"
#include "Frame.h"
#include "FrameLoader.h"
#include "Page.h"
#include "KURL.h"
#include "Text.h"
#include "Comment.h"
#include "HTMLInputElement.h"
#include "HTMLCanvasElement.h"
#include "CanvasRenderingContext2D.h"
#include "CanvasStyle.h"
#include "CanvasGradient.h"
#include "CanvasPattern.h"
#include "HTMLNames.h"
#include "HTMLElement.h"
#include "NodeList.h"
#include "DocumentFragment.h"
#include "ResourceHandle.h"
#include "FormData.h"
#include "TextEncoding.h"
#include "ResourceHandleClient.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "HTMLTextAreaElement.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <set>

namespace {
static std::string cookieHeaderForCurrentDocument();
static void setCookieForCurrentDocument(const std::string& text);


#define AGB_TAG_NODE 0x4E4F4431u   /* 'NOD1' */

/* ------------------------------------------------------------------------- */
/* Per-view runtime state                                                     */
/* ------------------------------------------------------------------------- */
/* Everything that used to be a process-wide global is now owned by one
 * RuntimeState. Today there is a single state; the goal is one state (one Ant
 * VM) per view, switched by contextActivate() at the bridge dispatch funnel.
 *
 * `g_*` below stay as macros over g_currentState so the rest of this file -
 * 250+ references - keeps working unchanged. */

#define AGB_MAX_DOCUMENT_SLOTS 16

struct RuntimeState {
    AntbEngine*        engine;
    WebCore::Document* document;
    AntbClass*         nodeClass;
    AntbClass*         canvasCtxClass;  /* CanvasRenderingContext2D (2d context) */
    AntbValue*         locationObj;
    AntbValue*         docObj;          /* the JS document object (rooted) */
    bool               documentBound;
    void*              listeners;       /* std::vector<Listener>* (see below) */
    WebCore::Document* docSlots[AGB_MAX_DOCUMENT_SLOTS];
    unsigned long      docSlotGeneration[AGB_MAX_DOCUMENT_SLOTS];
    int                docSlotCount;
    int                currentSlot;
    unsigned long      generation;      /* id of the realm (see nextGeneration) */
    unsigned long      polyfillsGeneration; /* realm whose VM already got polyfills.js */
};

/* Every realm (one document) gets a fresh id. Resources that can outlive
 * their document - module downloads, XHR requests, pinned DOM nodes - carry
 * the id of the realm they were created in, so the unload path can cancel
 * exactly the ones that belong to the document that goes away.
 *
 * The counter is process wide: the module loader and the request queue are
 * shared between views, and a per-view counter would let one view cancel
 * another view's requests. */
static unsigned long nextGeneration(void)
{
    static unsigned long counter = 0;

    return ++counter;
}

static std::vector<RuntimeState*>* g_states = NULL;
/* Never NULL: the mirrors below are dereferenced by code that can run before a
 * real state exists (isRunning(), teardown paths). */
static RuntimeState  g_emptyState;
static RuntimeState* g_currentState = &g_emptyState;

/* The generation of the realm that is currently running (0 when there is
 * none). */
static unsigned long currentGeneration(void)
{
    if (!g_currentState || g_currentState == &g_emptyState)
        return 0;

    return g_currentState->generation;
}

/* DOM nodes are pinned (ref'd) so a freed node's address is never reused for
 * a new node while a JS wrapper still points at it. The pins are grouped by
 * realm: when a realm is gone, its nodes can be released instead of being
 * pinned forever (a reload used to leak a whole document per navigation). */
static std::map<unsigned long, std::set<WebCore::Node*>*>& pinnedNodes(void)
{
    static std::map<unsigned long, std::set<WebCore::Node*>*>* pins =
        new std::map<unsigned long, std::set<WebCore::Node*>*>();

    return *pins;
}

/* Releases the pins of a realm. The nodes are still owned by WebCore when the
 * realm is torn down (the document is replaced, not deleted yet), so the
 * release is deferred by one generation: unref'ing a node the outgoing
 * document still uses would free it too early. */
static void releasePinnedNodes(unsigned long generation)
{
    std::map<unsigned long, std::set<WebCore::Node*>*>& pins = pinnedNodes();
    std::map<unsigned long, std::set<WebCore::Node*>*>::iterator it = pins.find(generation);

    if (it == pins.end())
        return;

    std::set<WebCore::Node*>* nodes = it->second;

    if (nodes) {
        for (std::set<WebCore::Node*>::iterator n = nodes->begin(); n != nodes->end(); ++n)
            (*n)->deref();

        delete nodes;
    }

    pins.erase(it);
}

static std::vector<RuntimeState*>& runtimeStates()
{
    if (!g_states)
        g_states = new std::vector<RuntimeState*>();

    return *g_states;
}

/* Page* -> RuntimeState*, so KJSProxy can evaluate in the right engine. */
static std::vector<std::pair<void*, void*> >* g_pageContexts = NULL;

static std::vector<std::pair<void*, void*> >& pageContexts()
{
    if (!g_pageContexts)
        g_pageContexts = new std::vector<std::pair<void*, void*> >();

    return *g_pageContexts;
}

static RuntimeState* createRuntimeState(AntbEngine* engine)
{
    RuntimeState* state = new RuntimeState();

    memset(state, 0, sizeof(RuntimeState));
    state->engine = engine;
    state->currentSlot = -1;

    /* The first realm of this view gets an id too: its resources are
     * cancelled by it when the first document is replaced. */
    state->generation = nextGeneration();

    runtimeStates().push_back(state);

    if (!g_currentState)
        g_currentState = state;

    return state;
}

static RuntimeState* stateForEngine(AntbEngine* engine)
{
    std::vector<RuntimeState*>& states = runtimeStates();

    for (size_t i = 0; i < states.size(); ++i) {
        if (states[i]->engine == engine)
            return states[i];
    }

    return NULL;
}

static void activateState(RuntimeState* state)
{
    if (state)
        g_currentState = state;
}

/* The mirrors. Keep these names: 250+ references across this file rely on them. */
#define g_engine        (g_currentState->engine)
#define g_document      (g_currentState->document)
#define g_nodeClass     (g_currentState->nodeClass)
#define g_canvasCtxClass (g_currentState->canvasCtxClass)
#define g_locationObj   (g_currentState->locationObj)
#define g_docObj        (g_currentState->docObj)
#define g_documentBound (g_currentState->documentBound)
#define g_docSlots      (g_currentState->docSlots)
#define g_docSlotGeneration (g_currentState->docSlotGeneration)
#define g_docSlotCount  (g_currentState->docSlotCount)
#define g_currentSlot   (g_currentState->currentSlot)

static int slotForDocument(WebCore::Document* document)
{
    int i;

    if (!document)
        return -1;

    for (i = 0; i < g_docSlotCount; ++i) {
        if (g_docSlots[i] == document)
            return i;
    }

    /* A slot of an older realm points at a document that is gone: reuse it
     * instead of running out of slots (and instead of handing out a pointer
     * to a freed document). */
    for (i = 0; i < g_docSlotCount; ++i) {
        if (g_docSlotGeneration[i] != currentGeneration()) {
            g_docSlots[i] = document;
            g_docSlotGeneration[i] = currentGeneration();
            return i;
        }
    }

    if (g_docSlotCount >= AGB_MAX_DOCUMENT_SLOTS)
        return -1;

    g_docSlots[g_docSlotCount] = document;
    g_docSlotGeneration[g_docSlotCount] = currentGeneration();
    return g_docSlotCount++;
}

/* DOM binding (defined further down) - all mirrored from g_currentState. */

static void activateSlot(int slot)
{
    if (slot < 0 || slot >= g_docSlotCount || !g_docSlots[slot])
        return;

    g_currentSlot = slot;
    g_document = g_docSlots[slot];

    if (g_docObj)
        antb_set_native(g_docObj, g_document, AGB_TAG_NODE);
}
int nodeGetAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeAddEventListener(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeRemoveEventListener(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
WebCore::Document* documentOf(AntbValue* self);
int docGetElementById(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeDispatchEvent(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int locationGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int locationSetter(AntbEngine*, AntbValue*, const char*, const AntbValue*, void*);
int locationAssign(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int locationReplace(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int locationReload(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int locationToString(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docWrite(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateHTMLDocument(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docHasFeature(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docWriteln(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int historyBack(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int historyForward(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int historyGo(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int historyPushState(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
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
int nodeGetBoundingClientRect(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetContext(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeToDataURL(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);

/* CanvasRenderingContext2D methods, dispatched through one shared callback:
 * `user` carries the operation. */
enum CanvasCtxOp {
    CV_SAVE, CV_RESTORE, CV_SCALE, CV_ROTATE, CV_TRANSLATE,
    CV_CLEAR_RECT, CV_FILL_RECT, CV_STROKE_RECT,
    CV_BEGIN_PATH, CV_CLOSE_PATH, CV_MOVE_TO, CV_LINE_TO,
    CV_QUADRATIC, CV_BEZIER, CV_ARC, CV_ARC_TO, CV_RECT,
    CV_FILL, CV_STROKE, CV_CLIP, CV_DRAW_IMAGE,
    CV_CREATE_LINEAR, CV_CREATE_RADIAL, CV_CREATE_PATTERN,
    CV_MEASURE_TEXT, CV_SET_TRANSFORM, CV_SET_LINE_DASH, CV_GET_LINE_DASH,
    CV_GET_IMAGE_DATA, CV_PUT_IMAGE_DATA, CV_IS_POINT_IN_PATH,
    CV_FILL_TEXT, CV_STROKE_TEXT
};
static int canvasCtxMethod(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int canvasCtxGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
static int canvasCtxSetter(AntbEngine*, AntbValue*, const char*, const AntbValue*, void*);
AntbValue* wrapCanvasCtx(AntbEngine*, WebCore::CanvasRenderingContext2D*);
static AntbValue* expandoFor(AntbEngine*, WebCore::Node*, bool);
static void clearExpandoObjects();
static int nodeClassList(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int classListAdd(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int classListRemove(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int classListContains(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
static int classListToggle(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetElementsByClassName(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeQuerySelectorAll(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeQuerySelector(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalFrameSlot(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalOpenWindow(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalPostMessage(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalDialog(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalCloseWindow(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalStorageGet(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalStorageHas(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalStorageSet(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalStorageDel(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalStorageClear(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalStorageKey(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalStorageCount(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalComputedStyle(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalActivateSlot(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int imageConstructor(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
/* ------------------------------------------------------------------------- */
/* Asynchronous requests (XHR with async = true)                              */
/* ------------------------------------------------------------------------- */

/* One in-flight request. The handle is ref'd by this object, so it stays alive
 * until the job is collected; the JS callback is retained and released when the
 * response has been delivered. */
/* Binary responses cannot travel as WebCore::String (NUL and non-UTF-8 bytes
 * are destroyed on the way through), so they are handed to JS as base64 and
 * decoded there. This is the transport behind XHR responseType=arraybuffer and
 * fetch().arrayBuffer(); without it wasm/zip/image bytes arrive empty. */
static std::string base64Encode(const char* data, size_t len)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;

    out.reserve(((len + 2) / 3) * 4);

    {
        size_t i = 0;

        while (i + 2 < len) {
            unsigned v = ((unsigned char)data[i] << 16)
                       | ((unsigned char)data[i + 1] << 8)
                       |  (unsigned char)data[i + 2];

            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += table[(v >> 6) & 63];
            out += table[v & 63];
            i += 3;
        }

        if (i < len) {
            unsigned v = (unsigned char)data[i] << 16;
            bool two = (i + 1 < len);

            if (two)
                v |= (unsigned char)data[i + 1] << 8;

            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += two ? table[(v >> 6) & 63] : '=';
            out += '=';
        }
    }

    return out;
}

struct XhrJob : public WebCore::ResourceHandleClient {
    XhrJob(AntbEngine* engine, AntbValue* callback, bool binary = false,
           const char* method = NULL, const char* contentType = NULL,
           const char* body = NULL)
        : m_engine(engine)
        , m_callback(antb_retain(callback))
        , m_status(0)
        , m_done(false)
        , m_failed(false)
        , m_binary(binary)
        , m_method(method ? method : "GET")
        , m_contentType(contentType ? contentType : "")
        , m_postBody(body ? body : "")
        , m_startMs(0)
        , m_doneMs(0)
        , m_slot(g_currentSlot)
        , m_generation(currentGeneration())
    {
    }

    virtual ~XhrJob()
    {
        if (m_callback)
            antb_value_release(m_callback);
    }

    void start(const char* url)
    {
        m_startMs = Net::logElapsedMs();
        if (Net::logEnabled())
            Net::logLine("xhr.begin   %s", url);

        WebCore::KURL kurl((const char*)url);
        WebCore::ResourceRequest request(kurl);
        std::string cookies = cookieHeaderForCurrentDocument();

        /* Page code posts JSON everywhere (device fingerprinting, auth, RPC):
         * sending GET without a body made those endpoints fail and the SDKs
         * retried in a loop, which pinned a core. */
        if (!m_method.empty() && m_method != "GET")
            request.setHTTPMethod(WebCore::String(m_method.c_str()));

        if (!m_postBody.empty() && m_method != "GET" && m_method != "HEAD") {
            WebCore::TextEncoding encoding("UTF-8");

            if (!encoding.isValid())
                encoding = WebCore::UTF8Encoding();

            WebCore::String bodyString = WebCore::String::fromUTF8(m_postBody.c_str());

            request.setHTTPBody(PassRefPtr<WebCore::FormData>(
                new WebCore::FormData(encoding.encode(bodyString.characters(), bodyString.length()))));
        }

        if (!m_contentType.empty())
            request.addHTTPHeaderField(WebCore::String("Content-Type"),
                                       WebCore::String::fromUTF8(m_contentType.c_str()));

        if (!cookies.empty())
            request.addHTTPHeaderField(WebCore::String("Cookie"),
                                       WebCore::String::fromUTF8(cookies.c_str()));


        m_handle = WebCore::ResourceHandle::create(request, this, false, true, false);
    }

    /* Stops the request and drops the JS callback: the response must not
     * reach a realm that is going away. */
    void cancel()
    {
        if (m_handle)
            m_handle->cancel();

        if (m_callback) {
            antb_value_release(m_callback);
            m_callback = 0;
        }

        m_done = true;
        m_failed = true;
    }

    virtual void didReceiveResponse(WebCore::ResourceHandle*, const WebCore::ResourceResponse& response)
    {
        m_status = response.httpStatusCode();

        {
            WebCore::String setCookie = response.httpHeaderField("Set-Cookie");

            if (!setCookie.isEmpty())
                setCookieForCurrentDocument(std::string(setCookie.utf8().data()));
        }
    }

    virtual void didReceiveData(WebCore::ResourceHandle*, const char* data, int length, int)
    {
        if (length > 0)
            m_body.append(data, length);
    }

    virtual void didFinishLoading(WebCore::ResourceHandle*)
    {
        m_doneMs = Net::logElapsedMs();
        m_done = true;
    }

    virtual void didFail(WebCore::ResourceHandle*, const WebCore::ResourceError&)
    {
        m_doneMs = Net::logElapsedMs();
        m_done = true;
        m_failed = true;
    }

    void deliver()
    {
        int previousSlot = g_currentSlot;

        if (m_slot >= 0)
            activateSlot(m_slot);

        if (Net::logEnabled())
            Net::logLine("xhr.deliver lag=%.1fms total=%.1fms status=%d bytes=%u",
                         m_doneMs > 0 ? Net::logElapsedMs() - m_doneMs : -1.0,
                         m_startMs > 0 ? Net::logElapsedMs() - m_startMs : -1.0,
                         m_status, (unsigned)m_body.size());
        if (!m_callback || !m_engine)
            return;

        {
            std::string payloadText;

            if (m_binary)
                payloadText = base64Encode(m_body.data(), m_body.size());
            else
                payloadText = WebCore::String(m_body.data(), m_body.size()).utf8().data();

            AntbValue* textValue = antb_make_string(m_engine, payloadText.c_str());
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

        m_callback = NULL;
        if (previousSlot >= 0)
            activateSlot(previousSlot);
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
    int                             m_slot;
    double                          m_startMs;
    double                          m_doneMs;
    bool                            m_done;
    bool                            m_failed;
    bool                            m_binary;      /* deliver base64 instead of text */
    std::string                     m_method;      /* GET/POST/... */
    std::string                     m_contentType;
    std::string                     m_postBody;    /* request body (text) */
    unsigned long                   m_generation;  /* the realm that started it */

    unsigned long generation() const { return m_generation; }
};

static Vector<XhrJob*>& xhrJobs()
{
    static Vector<XhrJob*>* jobs = new Vector<XhrJob*>();
    return *jobs;
}

/* Cancels the requests that belong to a realm that is going away: their
 * response must not be delivered into the realm that replaces it. */
static void cancelXhrJobsForGeneration(unsigned long generation)
{
    Vector<XhrJob*>& jobs = xhrJobs();

    for (unsigned i = 0; i < jobs.size(); ) {
        XhrJob* job = jobs[i];

        if (job && job->generation() <= generation) {
            job->cancel();
            delete job;
            jobs.remove(i);
        } else {
            ++i;
        }
    }
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

/* Same request, but the callback receives (status, base64-bytes). */
int globalHttpRequestBin(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
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

    job = new XhrJob(engine, args[1], true);
    xhrJobs().append(job);
    job->start(urlText);

    antb_free(urlText);
    return 1;
}

/* Full request: (url, method, contentType, body, callback), always binary.
 * POST bodies are JSON/form text; the body is copied into the job, so the
 * temporaries can go right after start(). */
int globalHttpRequestEx(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                        AntbValue* out, void*)
{
    char* urlText;
    char* methodText = NULL;
    char* typeText = NULL;
    char* bodyText = NULL;
    XhrJob* job;

    (void)out;

    if (argc < 5)
        return 1;

    urlText = antb_to_utf8(args[0]);
    if (!urlText)
        return 1;

    if (args[1])
        methodText = antb_to_utf8(args[1]);

    if (args[2])
        typeText = antb_to_utf8(args[2]);

    if (args[3])
        bodyText = antb_to_utf8(args[3]);

    job = new XhrJob(engine, args[4], true, methodText, typeText, bodyText);
    xhrJobs().append(job);
    job->start(urlText);

    antb_free(urlText);
    antb_free(methodText);
    antb_free(typeText);
    antb_free(bodyText);
    return 1;
}

int globalHttpGet(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpGetBin(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalClassOp(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpRequest(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpRequestBin(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpRequestEx(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeSetter(AntbEngine*, AntbValue*, const char*, const AntbValue*, void*);
int docCreateElement(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateElementNS(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeFocus(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeBlur(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeClick(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateTextNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateComment(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeAppendChild(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeRemoveChild(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeInsertBefore(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeSetAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeRemoveAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);

/* ------------------------------------------------------------------------- */
/* Cookie jar                                                                 */
/* ------------------------------------------------------------------------- */
struct CookieEntry {
    std::string name;
    std::string value;
    std::string domain;
    std::string path;
    bool        secure;
};
static std::vector<CookieEntry>* g_cookies = NULL;
static std::string        g_cookieFile;
static std::string        g_storageFile;
static std::vector<std::pair<std::string, std::string> >* g_storage = NULL;

/* Creates every missing component of an absolute path ("a\b\c"). Returns true
 * when the leaf exists afterwards. */
static bool createDirTree(char* path)
{
    char* p;
    char saved;

    if (!path || !path[0])
        return false;

    for (p = path + 1; ; ++p) {
        saved = *p;

        if (saved == '\\' || saved == '/' || saved == 0) {
            *p = 0;

            CreateDirectoryA(path, NULL);

            *p = saved;

            if (!saved)
                break;
        }
    }

    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}


/* Persisted cookies live in <data dir>/cookies.txt, one record per line:
 *   domain <TAB> path <TAB> secure <TAB> name <TAB> value
 * The data dir is $AGB_USERDATA when set (the headless probe points it at its
 * own directory so tests never touch the profile a GUI session uses), else
 * %APPDATA%\Agave when writable, else the exe folder. */
static const char* dataDir(void)
{
    static char directory[AGB_PATH_CHARS];
    static int  ready = 0;

    if (ready)
        return directory;

    ready = 1;
    directory[0] = 0;

    {
        const char* userdata = getenv("AGB_USERDATA");

        if (userdata && userdata[0]) {
            _snprintf(directory, sizeof(directory), "%s", userdata);
            directory[sizeof(directory) - 1] = 0;

            if (createDirTree(directory))
                return directory;

            directory[0] = 0;
        }
    }

    {
        const char* appdata = getenv("APPDATA");

        if (appdata && appdata[0]) {
            _snprintf(directory, sizeof(directory), "%s\\Agave", appdata);
            directory[sizeof(directory) - 1] = 0;

            if (CreateDirectoryA(directory, NULL) ||
                GetFileAttributesA(directory) != INVALID_FILE_ATTRIBUTES)
                return directory;
        }
    }

    {
        char module[AGB_PATH_CHARS];

        if (GetModuleFileNameA(NULL, module, sizeof(module))) {
            char* slash = strrchr(module, '\\');

            if (slash)
                *(slash + 1) = 0;

            _snprintf(directory, sizeof(directory), "%sAgaveData", module);
            directory[sizeof(directory) - 1] = 0;
            CreateDirectoryA(directory, NULL);
        }
    }

    return directory;
}
static std::vector<CookieEntry>& cookieJar();

/* ------------------------------------------------------------------------- */
/* localStorage                                                              */
/* ------------------------------------------------------------------------- */

/* Keys are "origin|name"; values may contain tabs/newlines, so they are
 * percent-escaped on the way to disk. The whole file is rewritten on change. */
static std::vector<std::pair<std::string, std::string> >& storage()
{
    if (!g_storage)
        g_storage = new std::vector<std::pair<std::string, std::string> >();

    return *g_storage;
}

static std::string escapeStorage(const std::string& text)
{
    std::string out;

    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = (unsigned char)text[i];

        if (c == '%' || c == '\t' || c == '\r' || c == '\n') {
            char buffer[8];

            sprintf(buffer, "%%%02X", (unsigned)c);
            out += buffer;
        } else {
            out += (char)c;
        }
    }

    return out;
}

static std::string unescapeStorage(const std::string& text)
{
    std::string out;

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            char hex[3];

            hex[0] = text[i + 1];
            hex[1] = text[i + 2];
            hex[2] = 0;
            out += (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out += text[i];
        }
    }

    return out;
}

/* localStorage is written lazily: setItem() used to rewrite the whole file
 * every call, which showed up as ~11% of a page load in the sampler
 * (ntdll!NtWriteFile). Mark dirty here, flush from the frame pump. */
static int           g_storageDirty = 0;
static unsigned long g_storageSavedAt = 0;

static void saveStorageNow()
{
    FILE* file;
    std::vector<std::pair<std::string, std::string> >& entries = storage();

    g_storageDirty = 0;
    g_storageSavedAt = GetTickCount();

    if (g_storageFile.empty())
        return;

    file = fopen(g_storageFile.c_str(), "wb");

    if (!file)
        return;

    for (size_t i = 0; i < entries.size(); ++i) {
        fprintf(file, "%s\t%s\n", escapeStorage(entries[i].first).c_str(), escapeStorage(entries[i].second).c_str());
    }

    fclose(file);
}

static void saveStorage()
{
    g_storageDirty = 1;
}

void flushStorageIfDirty(void)
{
    if (!g_storageDirty)
        return;

    /* throttle: at most one write per 100ms */
    if (g_storageSavedAt && GetTickCount() - g_storageSavedAt < 100)
        return;

    saveStorageNow();
}

static void loadStorage()
{
    FILE* file;
    char line[16384];

    if (g_storageFile.empty())
        return;

    file = fopen(g_storageFile.c_str(), "rb");

    if (!file)
        return;

    while (fgets(line, sizeof(line), file)) {
        std::string text(line);
        size_t tab = text.find('\t');

        if (tab == std::string::npos)
            continue;

        while (!text.empty() && (text[text.size() - 1] == '\n' || text[text.size() - 1] == '\r'))
            text.erase(text.size() - 1);

        storage().push_back(std::make_pair(unescapeStorage(text.substr(0, tab)),
                                           unescapeStorage(text.substr(tab + 1))));
    }

    fclose(file);
}

static const char* storageGet(const std::string& key, std::string& out)
{
    std::vector<std::pair<std::string, std::string> >& entries = storage();

    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].first == key) {
            out = entries[i].second;
            return out.c_str();
        }
    }

    return NULL;
}

static void storageSet(const std::string& key, const std::string& value)
{
    std::vector<std::pair<std::string, std::string> >& entries = storage();

    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].first == key) {
            entries[i].second = value;
            saveStorage();
            return;
        }
    }

    entries.push_back(std::make_pair(key, value));
    saveStorage();
}

static void storageDel(const std::string& key)
{
    std::vector<std::pair<std::string, std::string> >& entries = storage();

    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].first == key) {
            entries.erase(entries.begin() + i);
            saveStorage();
            return;
        }
    }
}

static void saveCookies()
{
    FILE* file;
    std::vector<CookieEntry>& jar = cookieJar();

    if (g_cookieFile.empty())
        return;

    file = fopen(g_cookieFile.c_str(), "wb");

    if (!file)
        return;

    for (size_t i = 0; i < jar.size(); ++i) {
        const CookieEntry& entry = jar[i];

        if (entry.name.find_first_of("\t\r\n") != std::string::npos ||
            entry.value.find_first_of("\t\r\n") != std::string::npos)
            continue;

        fprintf(file, "%s\t%s\t%d\t%s\t%s\n",
                entry.domain.c_str(), entry.path.c_str(),
                entry.secure ? 1 : 0,
                entry.name.c_str(), entry.value.c_str());
    }

    fclose(file);
}

static void loadCookies()
{
    FILE* file;
    char line[4096];

    if (g_cookieFile.empty())
        return;

    file = fopen(g_cookieFile.c_str(), "rb");

    if (!file)
        return;

    while (fgets(line, sizeof(line), file)) {
        std::vector<std::string> fields;
        std::string text(line);
        size_t start = 0;

        while (start <= text.size() && fields.size() < 5) {
            size_t tab = text.find('\t', start);

            fields.push_back(text.substr(start, tab == std::string::npos ? std::string::npos : tab - start));

            if (tab == std::string::npos)
                break;

            start = tab + 1;
        }

        if (fields.size() < 5)
            continue;

        while (!fields[4].empty() &&
               (fields[4][fields[4].size() - 1] == '\n' || fields[4][fields[4].size() - 1] == '\r'))
            fields[4].erase(fields[4].size() - 1);

        if (fields[3].empty())
            continue;

        CookieEntry entry;
        entry.domain = fields[0];
        entry.path = fields[1];
        entry.secure = fields[2] == "1";
        entry.name = fields[3];
        entry.value = fields[4];
        cookieJar().push_back(entry);
    }

    fclose(file);
}

static std::vector<CookieEntry>& cookieJar()
{
    if (!g_cookies)
        g_cookies = new std::vector<CookieEntry>();
    return *g_cookies;
}
static std::string hostOfCurrentDocument()
{
    if (!g_document)
        return std::string();
    {
        WebCore::KURL url(g_document->URL());
        WebCore::String host = url.host();
        return std::string(host.utf8().data());
    }
}
static bool domainMatches(const std::string& cookieDomain, const std::string& host)
{
    if (cookieDomain.empty() || host.empty())
        return true;
    if (cookieDomain == host)
        return true;
    /* ".example.com" also matches "www.example.com" */
    if (cookieDomain[0] == '.')
        return host.size() > cookieDomain.size()
            && host.compare(host.size() - cookieDomain.size(), cookieDomain.size(), cookieDomain) == 0;
    return host.size() > cookieDomain.size()
        && host[host.size() - cookieDomain.size() - 1] == '.'
        && host.compare(host.size() - cookieDomain.size(), cookieDomain.size(), cookieDomain) == 0;
}
static std::string cookieHeaderForCurrentDocument()
{
    std::string out;
    std::string host = hostOfCurrentDocument();
    std::vector<CookieEntry>& jar = cookieJar();
    for (size_t i = 0; i < jar.size(); ++i) {
        if (!domainMatches(jar[i].domain, host))
            continue;
        if (jar[i].secure && g_document && !WebCore::KURL(g_document->URL()).protocol().startsWith("https"))
            continue;
        if (!out.empty())
            out += "; ";
        out += jar[i].name + "=" + jar[i].value;
    }
    return out;
}
static void setCookieForCurrentDocument(const std::string& text)
{
    std::string host = hostOfCurrentDocument();
    CookieEntry entry;
    std::string expires;
    bool expired = false;
    size_t pos = 0;
    bool first = true;
    entry.secure = false;
    entry.domain = host;
    while (pos <= text.size()) {
        size_t end = text.find(';', pos);
        std::string part = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t firstNonSpace = part.find_first_not_of(" ");
        if (firstNonSpace == std::string::npos)
            part.clear();
        else
            part = part.substr(firstNonSpace);
        if (first) {
            size_t eq = part.find('=');
            if (eq != std::string::npos) {
                entry.name = part.substr(0, eq);
                entry.value = part.substr(eq + 1);
            }
            first = false;
        } else {
            std::string lower = part;
            for (size_t i = 0; i < lower.size(); ++i)
                lower[i] = (char)tolower((unsigned char)lower[i]);
            if (lower.compare(0, 5, "path=") == 0) {
                entry.path = part.substr(5);
            } else if (lower.compare(0, 7, "domain=") == 0) {
                entry.domain = part.substr(7);
            } else if (lower.compare(0, 6, "secure") == 0) {
                entry.secure = true;
            } else if (lower.compare(0, 9, "max-age=0") == 0) {
                expired = true;
            } else if (lower.compare(0, 8, "expires=") == 0) {
                expires = lower;
                if (expires.find("1970") != std::string::npos || expires.find("jan 1970") != std::string::npos)
                    expired = true;
            }
        }
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    if (entry.name.empty())
        return;
    std::vector<CookieEntry>& jar = cookieJar();
    for (size_t i = 0; i < jar.size(); ++i) {
        if (jar[i].name == entry.name && jar[i].domain == entry.domain) {
            jar.erase(jar.begin() + i);
            break;
        }
    }
    if (!expired)
        jar.push_back(entry);
    saveCookies();
}
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
        std::string jar = cookieHeaderForCurrentDocument();
        AntbValue* v = antb_make_string(engine, jar.c_str());
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
            antb_define_method(engine, obj, "toString", locationToString, NULL);
            antb_define_method(engine, obj, "valueOf", locationToString, NULL);
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
/* Resolves `text` against the current document and navigates the frame. */
static void navigateTo(const char* text)
{
    WebCore::Frame* frame;
    if (!g_document || !text)
        return;
    frame = g_document->frame();
    if (!frame)
        return;
    {
        WebCore::KURL base(g_document->URL());
        WebCore::KURL target(base, WebCore::DeprecatedString(text));
        if (!target.isValid())
            return;
        frame->loader()->load(target, 0);
    }
}
int locationSetter(AntbEngine*, AntbValue*, const char* name, const AntbValue* value, void*)
{
    char* text;
    if (!name || strcmp(name, "href") != 0 || !value)
        return 0;
    text = antb_to_utf8(value);
    if (!text)
        return 0;
    navigateTo(text);
    antb_free(text);
    return 1;
}
int locationAssign(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue*, void*)
{
    char* text;
    if (argc < 1)
        return 1;
    text = antb_to_utf8(args[0]);
    if (!text)
        return 1;
    navigateTo(text);
    antb_free(text);
    return 1;
}
int locationReplace(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                    AntbValue* out, void* user)
{
    return locationAssign(engine, self, args, argc, out, user);
}
int locationReload(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*)
{
    if (g_document && g_document->frame())
        g_document->frame()->loader()->reload();
    return 1;
}
/* window.close(): the engine asks the host to close the view (ChromeClientMC
 * -> cb_close_window), which the shell maps to closing the tab. */
int globalCloseWindow(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*)
{
    if (g_document && g_document->frame() && g_document->frame()->page())
        g_document->frame()->page()->chrome()->closeWindowSoon();
    return 1;
}
/* document.implementation.hasFeature(): always true, like modern browsers. */
int docHasFeature(AntbEngine* engine, AntbValue*, AntbValue**, int, AntbValue* out, void*)
{
    AntbValue* v = antb_make_number(engine, 1.0);
    if (!v)
        return 0;
    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

/* document.createHTMLDocument(title): jQuery feature-tests this. */
int docCreateHTMLDocument(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                          AntbValue* out, void*)
{
    WebCore::Document* doc = documentOf(NULL);
    WebCore::ExceptionCode ec = 0;
    WTF::PassRefPtr<WebCore::HTMLDocument> created;

    if (!doc)
        return 1;

    {
        WebCore::String title;
        if (argc > 0) {
            char* text = antb_to_utf8(args[0]);
            if (text) {
                title = WebCore::String::fromUTF8(text);
                antb_free(text);
            }
        }
        created = doc->implementation()->createHTMLDocument(title);
    }

    if (!created)
        return 1;

    created->ref();   /* the wrapper keeps it alive */

    {
        AntbValue* v = wrapNode(engine, created.get());
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}
/* document.write / writeln feed the parser, as the spec does. */
int docWrite(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue*, void*)
{
    char* text;
    if (!g_document || argc < 1)
        return 1;
    text = antb_to_utf8(args[0]);
    if (!text)
        return 1;
    g_document->write(WebCore::String::fromUTF8(text));
    antb_free(text);
    return 1;
}
int docWriteln(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue*, void*)
{
    char* text;
    if (!g_document || argc < 1)
        return 1;
    text = antb_to_utf8(args[0]);
    if (!text)
        return 1;
    g_document->writeln(WebCore::String::fromUTF8(text));
    antb_free(text);
    return 1;
}
int historyBack(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*)
{
    if (g_document && g_document->frame() && g_document->frame()->page())
        g_document->frame()->page()->goBack();
    return 1;
}
int historyForward(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*)
{
    if (g_document && g_document->frame() && g_document->frame()->page())
        g_document->frame()->page()->goForward();
    return 1;
}
int historyGo(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
              AntbValue* out, void* user)
{
    double delta = (argc > 0) ? antb_to_number(args[0]) : -1;
    if (delta < 0)
        return historyBack(engine, self, args, argc, out, user);
    if (delta > 0)
        return historyForward(engine, self, args, argc, out, user);
    return 1;
}
/* pushState only rewrites the document URL: enough for SPA routers that poll
 * location and re-render, without a real history entry. */
int historyPushState(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue*, void*)
{
    if (argc > 2 && g_document) {
        char* urlText = antb_to_utf8(args[2]);
        if (urlText) {
            WebCore::KURL base(g_document->URL());
            WebCore::KURL target(base, WebCore::DeprecatedString(urlText));
            if (target.isValid())
                g_document->setURL(target.url());
            antb_free(urlText);
        }
    }
    return 1;
}
int locationGetter(AntbEngine* engine, AntbValue*, const char* name, AntbValue* out, void*)
{
    WebCore::KURL url;

    if (!g_document || !name)
        return 0;

    url = WebCore::KURL(g_document->URL());

    {
        WebCore::String value;
        WebCore::DeprecatedString deprecated = url.url();

        /* CSSOM semantics: protocol keeps its colon, host keeps the port and
         * hostname does not. Pages compare these strings literally (163.com
         * redirects forever when `protocol` comes back without the colon). */
        if (!strcmp(name, "href"))
            value = WebCore::String(deprecated);
        else if (!strcmp(name, "protocol")) {
            value = url.protocol();
            if (!value.endsWith(WebCore::String(":")))
                value = value + WebCore::String(":");
        } else if (!strcmp(name, "host")) {
            value = url.host();
            if (url.port()) {
                value = value + WebCore::String(":") + WebCore::String::number(url.port());
            }
        } else if (!strcmp(name, "hostname"))
            value = url.host();
        else if (!strcmp(name, "port")) {
            value = url.port() ? WebCore::String::number(url.port()) : WebCore::String();
        } else if (!strcmp(name, "origin")) {
            WebCore::String protocol = url.protocol();
            if (!protocol.endsWith(WebCore::String(":")))
                protocol = protocol + WebCore::String(":");
            value = protocol + WebCore::String("//") + url.host();
            if (url.port())
                value = value + WebCore::String(":") + WebCore::String::number(url.port());
        } else if (!strcmp(name, "pathname"))
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

/* location.toString() / valueOf(): `String(location)` and
 * `location.toString()` return the href. carbon.js builds its request headers
 * from window.location.toString() and dies with "undefined is not a function"
 * without them. */
int locationToString(AntbEngine* engine, AntbValue*, AntbValue**, int, AntbValue* out, void*)
{
    WebCore::String href;

    if (!g_document)
        return 0;

    href = WebCore::String(WebCore::KURL(g_document->URL()).url());

    {
        AntbValue* v = antb_make_string(engine, href.utf8().data());
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
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
    if (!strcmp(name, "cookie")) {
        char* text;

        if (!value)
            return 0;

        text = antb_to_utf8(value);
        if (!text)
            return 0;

        setCookieForCurrentDocument(std::string(text));
        antb_free(text);
        return 1;
    }


    return nodeSetter(engine, self, name, value, NULL);
}

void dropListenersForOldDocument();
} /* namespace */

/* -------------------------------------------------------------------------
 * Crash forensics
 * -------------------------------------------------------------------------
 * Heap corruption (0xC0000374) gives no JS stack and no useful message. Keep
 * the last few bridge operations in a fixed-size ring - no allocation, so it
 * survives a damaged heap - and dump it from an unhandled-exception filter.
 * Only compiled in when AGB_TRACE is defined at build time? No: it costs a
 * snprintf per DOM access, so it is behind the AGB_TRACE_CRASH env var.
 */
#define AGB_TRACE_SLOTS 256
static char g_trace[AGB_TRACE_SLOTS][112];
static volatile LONG g_tracePos = 0;
static int  g_traceOn = -1;

static int traceEnabled(void)
{
    if (g_traceOn < 0)
        g_traceOn = getenv("AGB_TRACE_CRASH") ? 1 : 0;

    return g_traceOn;
}

#if defined(_MSC_VER)
#define AGB_HAVE_TRACE 1
#else
#define AGB_HAVE_TRACE 1
#endif

static void dumpTrace(const char* why)
{
    void* frames[24];
    unsigned short count;
    LONG pos = g_tracePos;
    int i;

    count = RtlCaptureStackBackTrace(0, 24, frames, NULL);

    fprintf(stderr, "\n=== TRACE STOP: %s ===\n", why);

    for (i = 0; i < (int)count; ++i)
        fprintf(stderr, "  #%02d %p\n", i, frames[i]);

    fprintf(stderr, "--- last %d bridge operations ---\n", AGB_TRACE_SLOTS / 2);

    for (i = AGB_TRACE_SLOTS / 2; i > 0; --i) {
        LONG at = pos - i;

        if (at < 0)
            continue;

        fprintf(stderr, "  [op] %s\n", g_trace[at % AGB_TRACE_SLOTS]);
    }

    fflush(stderr);
}

static void traceOp(const char* fmt, ...)
{
    char buf[112];
    LONG slot;
    va_list ap;

    if (!traceEnabled())
        return;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;

    slot = InterlockedIncrement(&g_tracePos) - 1;
    memcpy(g_trace[slot % AGB_TRACE_SLOTS], buf, sizeof(buf));

    /* 0xC0000374 is a fail-fast and never reaches the exception filter, so
     * validate the heap ourselves every few operations and stop right there,
     * with the ring buffer still holding what led to it. */
    if ((slot & 31) == 0 && !HeapValidate(GetProcessHeap(), 0, NULL)) {
        dumpTrace("HeapValidate failed - heap already corrupted");
        ExitProcess(3);
    }
}

static LONG WINAPI agbCrashFilter(EXCEPTION_POINTERS* info)
{
    char why[128];

    snprintf(why, sizeof(why), "unhandled exception 0x%08lX at %p",
             (unsigned long)(info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0),
             (void*)(info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : 0));
    dumpTrace(why);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void installCrashFilter(void)
{
    static int installed = 0;

    if (installed || !traceEnabled())
        return;

    installed = 1;
    SetUnhandledExceptionFilter(agbCrashFilter);
    fprintf(stderr, "[trace] crash filter installed\n");
}

/* ------------------------------------------------------------------------- */
/* Binding profiler (AGB_PROFILE=1)                                          */
/* ------------------------------------------------------------------------- */
/* Counts and times the bridge entry points the engine spends its time in.   */
/* Cheap enough to leave compiled in: disabled it is one getenv check.       */
enum {
    PROF_NODE_GET = 0,
    PROF_NODE_SET,
    PROF_STYLE_GET,
    PROF_STYLE_SET,
    PROF_COMPUTED,
    PROF_CREATE_ELEMENT,
    PROF_APPEND,
    PROF_SLOTS
};

struct ProfSlot {
    const char*        name;
    unsigned long long calls;
    unsigned long long ticks;
};

static ProfSlot g_prof[PROF_SLOTS] = {
    { "node.get", 0, 0 },
    { "node.set", 0, 0 },
    { "style.get", 0, 0 },
    { "style.set", 0, 0 },
    { "computedStyle", 0, 0 },
    { "createElement", 0, 0 },
    { "appendChild", 0, 0 }
};

static int g_profEnabled = -1;
static double g_profFreq = 0.0;

static void dumpProfile(void)
{
    int i;

    if (!g_profFreq || g_profFreq <= 0.0)
        g_profFreq = 1000000.0;

    fprintf(stderr, "\n[prof] binding calls ms\n");

    for (i = 0; i < PROF_SLOTS; ++i)
        fprintf(stderr, "[prof] %-14s %9llu %9.1f\n", g_prof[i].name, g_prof[i].calls,
                (double)g_prof[i].ticks * 1000.0 / g_profFreq);

    fflush(stderr);
}

static int profEnabled(void)
{
    if (g_profEnabled < 0) {
        if (getenv("AGB_PROFILE")) {
            LARGE_INTEGER freq;

            QueryPerformanceFrequency(&freq);
            g_profFreq = (double)freq.QuadPart;
            g_profEnabled = 1;
            atexit(dumpProfile);
        } else {
            g_profEnabled = 0;
        }
    }

    return g_profEnabled;
}

struct ProfScope {
    int                slot;
    unsigned long long started;
    bool               on;

    explicit ProfScope(int s) : slot(s), started(0), on(profEnabled() != 0)
    {
        if (on) {
            LARGE_INTEGER now;

            QueryPerformanceCounter(&now);
            started = (unsigned long long)now.QuadPart;
        }
    }

    ~ProfScope()
    {
        if (!on)
            return;

        {
            LARGE_INTEGER now;

            QueryPerformanceCounter(&now);
            g_prof[slot].calls++;
            g_prof[slot].ticks += (unsigned long long)now.QuadPart - started;
        }
    }
};

/* ------------------------------------------------------------------------- */
/* Sampling profiler (AGB_PROFILE=1)                                         */
/* ------------------------------------------------------------------------- */
/* A watchdog thread samples the JS thread's instruction pointer a few times a
 * second; the histogram points at the function that actually burns the time
 * when the JS engine itself is the bottleneck (the bridge counters only add
 * up to a couple of seconds on browserscore, so the cost is inside the VM). */
#define PROF_SAMPLES 65536

static unsigned long long g_profSamples[PROF_SAMPLES];
static volatile LONG      g_profSampleCount = 0;
static volatile LONG      g_profStop = 0;
static DWORD              g_profThreadId = 0;
static HANDLE             g_profWatcher = NULL;
static int                g_profSamplerOn = -1;

static DWORD WINAPI profilerWatcher(LPVOID)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_profThreadId);

    if (!thread)
        return 0;

    while (!g_profStop) {
        CONTEXT context;

        Sleep(1);

        if (SuspendThread(thread) == (DWORD)-1)
            continue;

        context.ContextFlags = CONTEXT_CONTROL;

        if (GetThreadContext(thread, &context)) {
            LONG at = InterlockedIncrement(&g_profSampleCount) - 1;

            if (at < PROF_SAMPLES)
                g_profSamples[at] = (unsigned long long)context.Rip;
        }

        ResumeThread(thread);
    }

    CloseHandle(thread);
    return 0;
}

static const char* moduleNameFor(unsigned long long address, unsigned long long* offset)
{
    static const char* names[] = { "libagave.dll", "antbridge.dll", NULL };
    static unsigned long long bases[3] = { 0, 0, 0 };
    int i;

    if (!bases[0]) {
        bases[0] = (unsigned long long)GetModuleHandleA("libagave.dll");
        bases[1] = (unsigned long long)GetModuleHandleA("antbridge.dll");
        bases[2] = (unsigned long long)GetModuleHandleA(NULL);
    }

    for (i = 0; i < 3; ++i) {
        if (!bases[i])
            continue;

        if (address >= bases[i] && address < bases[i] + 0x8000000ULL) {
            *offset = address - bases[i];
            return names[i] ? names[i] : "host.exe";
        }
    }

    *offset = address;
    return "other";
}

static void dumpSampleProfile(void)
{
    int i;
    LONG count = g_profSampleCount;

    if (count > PROF_SAMPLES)
        count = PROF_SAMPLES;

    fprintf(stderr, "\n[profjs] samples=%ld\n", (long)count);

    for (i = 0; i < count; ++i) {
        unsigned long long offset = 0;
        const char* module = moduleNameFor(g_profSamples[i], &offset);

        fprintf(stderr, "[profjs] %s+0x%llx\n", module, offset);
    }

    fflush(stderr);
}

static void startSampler(void)
{
    if (g_profSamplerOn >= 0)
        return;

    g_profSamplerOn = getenv("AGB_PROFILE") ? 1 : 0;

    if (!g_profSamplerOn)
        return;

    g_profThreadId = GetCurrentThreadId();
    g_profWatcher = CreateThread(NULL, 0, profilerWatcher, NULL, 0, NULL);
    atexit(dumpSampleProfile);
}

namespace AntRuntime {

static void initCookieStorage()
{
    if (!g_cookieFile.empty())
        return;

    {
        const char* directory = dataDir();

        if (directory && directory[0]) {
            g_cookieFile = std::string(directory) + "\\cookies.txt";
            g_storageFile = std::string(directory) + "\\localstorage.txt";
            loadCookies();
            loadStorage();
            return;
        }
    }

    g_cookieFile = "cookies.txt";
    g_storageFile = "localstorage.txt";
    loadCookies();
    loadStorage();
}

/* Runs before every bridged native callback (see antb_set_context_activate).
 * With a single engine there is nothing to switch; once views get their own
 * engines this becomes: g_current = stateFor(engine) and refresh the g_* mirror. */
static void contextActivate(AntbEngine* engine)
{
    RuntimeState* state;

    if (!engine)
        return;

    if (g_currentState && g_currentState->engine == engine)
        return;

    state = stateForEngine(engine);

    if (state)
        activateState(state);
}

/* URL of the script currently being evaluated; used as the base for the
 * relative imports of module scripts. */
static std::string g_currentFilename;

/* Module bodies are cached by URL and fetched ahead of time: as soon as a module
 * arrives, its static imports are queued on the same session, so the network
 * works in parallel while the engine parses. A 154 module graph is otherwise
 * 154 serial round trips (~15s on a remote host). */
struct ModuleEntry {
    std::string body;
    bool        done;
    bool        failed;
    long long   id;
    unsigned long generation;   /* the realm that started the download */
    unsigned long serial;       /* identity: a client may only write to the entry it was started for */

    ModuleEntry() : done(false), failed(false), id(0), generation(0), serial(0) {}
};

static std::map<std::string, ModuleEntry*>* g_moduleCache = NULL;
static Net::Session* g_moduleSession = NULL;
static std::vector<std::pair<std::string, unsigned long> >* g_modulePending = NULL;
static int g_moduleInFlight = 0;
static unsigned long g_moduleSerial = 0;

/* AGB_OFFLINE=1 (shell --offline): module fetches are served from the cache
 * only; cache misses fail fast instead of touching the network. Makes module
 * evaluation deterministic (replay from a warmed cache) for reproducing
 * module-graph failures without network timing in the picture. */
static int g_moduleOffline = -1;

static bool moduleOffline(void)
{
    if (g_moduleOffline < 0) {
        g_moduleOffline = getenv("AGB_OFFLINE") ? 1 : 0;

        if (g_moduleOffline)
            fprintf(stderr, "[module] offline: fetches served from cache only\n");
    }

    return g_moduleOffline != 0;
}

/* External <script type="module" src> entries routed through the module
 * fetcher: URL -> the generation that asked for it. When the body arrives it
 * is handed to the WebCore notify hook (the waiting tokenizer), so the module
 * runs in the parse flow - never from inside a network pump. */
static std::map<std::string, unsigned long>* g_moduleEntryScripts = NULL;
static ModuleEntryNotifyHook g_moduleEntryNotifyHook = NULL;

void setModuleEntryNotifyHook(ModuleEntryNotifyHook hook)
{
    g_moduleEntryNotifyHook = hook;
}

static void notifyModuleEntry(const std::string& url, const std::string* source, bool failed)
{
    if (!g_moduleEntryNotifyHook)
        return;

    g_moduleEntryNotifyHook(url.c_str(),
                            (source && !source->empty() && !failed) ? source->c_str() : NULL,
                            failed ? 1 : 0);
}
static int moduleFetchLimit(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char* env = getenv("AGB_MODULE_CONCURRENCY");
        int requested = env ? atoi(env) : 12;

        cached = (requested > 0 && requested <= 128) ? requested : 12;
    }

    return cached;
}

static void moduleStart(const std::string& url, bool force);
static void modulePrefetchImports(const std::string& url, const std::string& body);

/* Imports discovered while the fetch slots are full wait here; a finished
 * request immediately starts the next one, so the graph loads ~6 wide instead
 * of one file at a time. */
static void moduleQueuePending(const std::string& url, unsigned long generation)
{
    if (!g_modulePending)
        g_modulePending = new std::vector<std::pair<std::string, unsigned long> >();

    for (size_t i = 0; i < g_modulePending->size(); ++i) {
        if ((*g_modulePending)[i].first == url)
            return;
    }

    g_modulePending->push_back(std::make_pair(url, generation));
}

static void moduleDrainPending(void)
{
    while (g_modulePending && !g_modulePending->empty()
            && g_moduleInFlight < moduleFetchLimit()) {
        std::string url = g_modulePending->front().first;

        g_modulePending->erase(g_modulePending->begin());
        moduleStart(url, true);
    }
}

/* Drops the request that owns the entry and frees its fetch slot. Used when
 * the body turns out to be complete without it (a disk cache hit): the
 * download is pure waste and, worse, its onData would append a second copy of
 * the file to a body that is already complete - the doubled source then fails
 * to compile ("Identifier 'x' has already been declared"). */
static void moduleCancelRequest(ModuleEntry* entry)
{
    if (!entry || !entry->id)
        return;

    if (g_moduleSession)
        g_moduleSession->cancel(entry->id);

    entry->id = 0;

    if (g_moduleInFlight > 0)
        --g_moduleInFlight;

    moduleDrainPending();
}

/* Cancels the module downloads that belong to a realm that is going away: the
 * bodies they would deliver are of no use to the realm that replaces it, and
 * the requests would keep the shared connection pool busy. */
static void cancelModuleDownloadsForGeneration(unsigned long generation)
{
    if (g_moduleCache) {
        std::map<std::string, ModuleEntry*>::iterator it = g_moduleCache->begin();

        for (; it != g_moduleCache->end(); ++it) {
            ModuleEntry* entry = it->second;

            if (!entry || entry->generation > generation)
                continue;

            if (entry->id) {
                if (g_moduleSession)
                    g_moduleSession->cancel(entry->id);

                entry->id = 0;

                if (g_moduleInFlight > 0)
                    --g_moduleInFlight;
            }

            /* A download that is still running is useless now. One that
             * finished stays cached: its body is on disk anyway. */
            if (!entry->done) {
                entry->done = true;
                entry->failed = true;
            }
        }
    }

    /* Imports that were queued for download but never started. */
    if (g_modulePending) {
        std::vector<std::pair<std::string, unsigned long> >::iterator it = g_modulePending->begin();

        while (it != g_modulePending->end()) {
            if (it->second <= generation)
                it = g_modulePending->erase(it);
            else
                ++it;
        }
    }
}

static ModuleEntry* moduleEntryFor(const std::string& url)
{
    if (!g_moduleCache)
        g_moduleCache = new std::map<std::string, ModuleEntry*>();

    std::map<std::string, ModuleEntry*>::iterator it = g_moduleCache->find(url);

    if (it != g_moduleCache->end())
        return it->second;

    ModuleEntry* entry = new ModuleEntry();

    /* The serial is the entry's identity: requests started for an earlier
     * entry of the same URL (deleted and recreated by moduleFetch's retry)
     * must not be able to append their data to this one. */
    entry->serial = ++g_moduleSerial;

    (*g_moduleCache)[url] = entry;
    return entry;
}

static void moduleCacheStore(const std::string& url, const std::string& body);
static bool moduleCacheLoad(const std::string& url, std::string* body);
static void modulePrefetchImports(const std::string& url, const std::string& body);

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The engine only joins module specifiers against http(s) URL bases
 * (esm_is_url), so file:// module filenames are masqueraded as
 * "http://file.ant/<path>" when handed to it. The engine resolves imports in
 * that space and fetches through the host fetcher (moduleFetch), which maps
 * back to the real file:// URL and reads from disk. import.meta.url shows
 * the masqueraded form. */
static const char kEngineFileUrlPrefix[] = "http://file.ant/";
static const size_t kEngineFileUrlPrefixLen = 16;

static bool isFileUrl(const std::string& u)
{
    return u.compare(0, 7, "file://") == 0;
}

static bool isEngineFileUrl(const std::string& u)
{
    return u.compare(0, kEngineFileUrlPrefixLen, kEngineFileUrlPrefix) == 0;
}

static std::string engineUrlForFile(const std::string& fileUrl)
{
    const char* rest = fileUrl.c_str();
    size_t skip = 0;

    if (fileUrl.compare(0, 8, "file:///") == 0)
        skip = 8;
    else if (fileUrl.compare(0, 7, "file://") == 0)
        skip = 7;
    else
        return fileUrl;

    /* file://localhost/... */
    if (fileUrl.compare(skip, 10, "localhost/") == 0)
        skip += 10;

    return std::string(kEngineFileUrlPrefix) + (rest + skip);
}

static std::string fileUrlForEngine(const std::string& engineUrl)
{
    return "file:///" + engineUrl.substr(kEngineFileUrlPrefixLen);
}

/* file:// module URLs are read straight from disk - the same direct-read
 * treatment the document loader gives them. No network session, no fetch
 * slots, works offline. */
static bool moduleReadLocalFile(const std::string& url, std::string* body)
{
    std::string path;
    FILE* file;
    long size;

    if (url.compare(0, 8, "file:///") == 0)
        path = url.substr(8);
    else if (url.compare(0, 7, "file://") == 0)
        path = url.substr(7);
    else
        return false;

    if (path.compare(0, 11, "localhost/") == 0)
        path.erase(0, 10);

    /* "/e:/dir/file" form (the slash before the drive letter). */
    if (path.size() > 2 && path[0] == '/' && path[2] == ':')
        path.erase(0, 1);

    /* Percent escapes (%20 and friends). */
    {
        std::string out;
        size_t i = 0;

        while (i < path.size()) {
            int hi, lo;

            if (path[i] == '%' && i + 2 < path.size()
                    && (hi = hexVal(path[i + 1])) >= 0 && (lo = hexVal(path[i + 2])) >= 0) {
                out += (char)((hi << 4) | lo);
                i += 3;
            } else
                out += path[i++];
        }

        path.swap(out);
    }

    file = fopen(path.c_str(), "rb");

    if (!file)
        return false;

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size <= 0) {
        fclose(file);
        return false;
    }

    body->resize((size_t)size);
    fread(&(*body)[0], 1, (size_t)size, file);
    fclose(file);
    return true;
}

/* Bodies of routed entry scripts that arrived inside a network pump callback.
 * JS must not run there (see pumpAsyncRequests): the body is deposited here
 * and handed to the WebCore notify hook after the pump, so the waiting
 * tokenizer resumes and runs the module in the parse flow. */
struct ArrivedEntryBody {
    std::string url;
    std::string source;
    bool failed;
};

static std::vector<ArrivedEntryBody>* g_arrivedEntryBodies = NULL;

/* A routed entry script finished (or failed): deposit the result for the
 * frame loop, or report the failure. Dropped when another realm owns the
 * page now. */
static void moduleEntryScriptArrived(const std::string& url, const ModuleEntry* entry)
{
    std::map<std::string, unsigned long>::iterator it;
    unsigned long generation;

    if (!g_moduleEntryScripts)
        return;

    it = g_moduleEntryScripts->find(url);

    if (it == g_moduleEntryScripts->end())
        return;

    generation = it->second;
    g_moduleEntryScripts->erase(it);

    if (currentGeneration() != generation) {
        if (getenv("AGB_MODULE_DEBUG"))
            fprintf(stderr, "[modentry] stale %s (gen %lu)\n", url.c_str(), generation);
        return;
    }

    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[modentry] arrived %s gen=%lu failed=%d\n",
                url.c_str(), generation, (entry && entry->failed) ? 1 : 0);

    if (entry && !entry->failed) {
        ArrivedEntryBody body;

        body.url = url;
        body.source = entry->body;
        body.failed = false;

        if (!g_arrivedEntryBodies)
            g_arrivedEntryBodies = new std::vector<ArrivedEntryBody>();

        g_arrivedEntryBodies->push_back(body);
    } else {
        ArrivedEntryBody body;

        body.url = url;
        body.failed = true;

        if (!g_arrivedEntryBodies)
            g_arrivedEntryBodies = new std::vector<ArrivedEntryBody>();

        g_arrivedEntryBodies->push_back(body);
    }
}

struct ModuleFetchClient : public Net::Client {
    std::string url;
    unsigned long serial;

    ModuleFetchClient(const std::string& u, unsigned long s) : url(u), serial(s) {}

    ModuleEntry* target() const
    {
        ModuleEntry* entry;

        if (!g_moduleCache)
            return NULL;

        {
            std::map<std::string, ModuleEntry*>::iterator it = g_moduleCache->find(url);

            if (it == g_moduleCache->end())
                return NULL;

            entry = it->second;
        }

        /* The entry was replaced by a newer request (retry / refetch): this
         * client's bytes belong to an entry that is gone. */
        if (!entry || entry->serial != serial)
            return NULL;

        return entry;
    }

    virtual void onResponse(const Net::Response&) {}

    virtual void onData(const char* data, unsigned int length)
    {
        ModuleEntry* entry = target();

        /* A complete body stays complete: once the entry is done (disk cache
         * hit while the request was in flight), further bytes are a duplicate
         * of the file, not a continuation of it. */
        if (entry && !entry->done && data && length)
            entry->body.append(data, length);
    }

    virtual void onComplete()
    {
        ModuleEntry* entry = target();

        if (entry && !entry->done) {
            entry->done = true;
            entry->id = 0;

            if (g_moduleInFlight > 0)
                --g_moduleInFlight;

            moduleCacheStore(url, entry->body);

            /* Fan out: scan a *prefetched* body for its imports too, otherwise the
             * graph is only ever one import list deep and the ESM loader ends up
             * fetching module after module with ~150ms of RTT each. */
            modulePrefetchImports(url, entry->body);

            moduleDrainPending();

            moduleEntryScriptArrived(url, entry);
        }
    }

    virtual void onError(const Net::Error&)
    {
        ModuleEntry* entry = target();

        if (entry && !entry->done) {
            entry->failed = true;
            entry->done = true;
            entry->id = 0;

            if (g_moduleInFlight > 0)
                --g_moduleInFlight;

            moduleDrainPending();

            moduleEntryScriptArrived(url, entry);
        }
    }
};

/* Resolves a relative specifier against the URL of the module holding it. */
static std::string moduleUrlJoin(const std::string& baseUrl, const std::string& specifier)
{
    std::string out;
    std::string host;
    size_t schemeEnd;
    size_t hostSlash;
    size_t dirSlash;
    size_t i;

    if (specifier.empty())
        return specifier;

    if (specifier.compare(0, 7, "http://") == 0 || specifier.compare(0, 8, "https://") == 0)
        return specifier;

    schemeEnd = baseUrl.find("://");

    if (schemeEnd == std::string::npos)
        return specifier;

    hostSlash = baseUrl.find('/', schemeEnd + 3);

    if (hostSlash == std::string::npos)
        return specifier;

    host = baseUrl.substr(0, hostSlash);

    if (specifier[0] == '/')
        return host + specifier;

    /* Relative to the *directory* of the base module: `./x.js` imported from
     * /src/app.js is /src/x.js, not /x.js. Using find() here resolved every
     * relative import against the site root, so browserscore.dev asked for
     * 217 files that do not exist (404s) next to the 225 real ones. */
    dirSlash = baseUrl.rfind('/');
    out = (dirSlash == std::string::npos || dirSlash < hostSlash) ? host + "/" : baseUrl.substr(0, dirSlash + 1);

    for (i = 0; i < specifier.size(); ++i) {
        if (specifier.compare(i, 3, "../") == 0) {
            size_t cut = out.size();

            if (cut > host.size() + 1) {
                cut -= 1;
                while (cut > host.size() + 1 && out[cut - 1] != '/')
                    --cut;
                out.erase(cut);
            }

            i += 2;
        } else if (specifier.compare(i, 2, "./") == 0) {
            i += 1;
        } else {
            out += specifier.substr(i);
            break;
        }
    }

    return out;
}

/* -------------------------------------------------------------------------
 * Module disk cache
 * -------------------------------------------------------------------------
 * The module graph is 200+ small files; with ~150ms of proxy RTT that is most
 * of the load time. Keep fetched bodies on disk so a second run measures the
 * engine instead of the network. AGB_MODULE_CACHE=0 disables it,
 * AGB_MODULE_CACHE_TTL=<seconds> expires entries (default 3600).
 */
static bool moduleCacheEnabled(void)
{
    static int on = -1;

    if (on < 0) {
        const char* env = getenv("AGB_MODULE_CACHE");
        on = (!env || atoi(env) != 0) ? 1 : 0;
    }

    return on != 0;
}

/* Returns a writable cache directory called 
ame. The data dir under
 * %APPDATA% is preferred, but it is not always creatable (the earlier
 * CreateDirectoryA silently failed and every store got ERROR_PATH_NOT_FOUND),
 * so fall back to <exe>\cache and then to .\cache. */
static std::string ensureCacheDir(const char* name)
{
    static std::string resolved;

    if (!resolved.empty())
        return resolved;

    {
        std::string dir = std::string(dataDir()) + "\\" + name;

        if (CreateDirectoryA(dir.c_str(), NULL) || GetFileAttributesA(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            resolved = dir;
            return resolved;
        }
    }

    {
        char module[AGB_PATH_CHARS];

        if (GetModuleFileNameA(NULL, module, sizeof(module))) {
            char* slash = strrchr(module, '\\');

            if (slash)
                *(slash + 1) = 0;

            {
                std::string base = std::string(module) + "cache";
                std::string dir = base + "\\" + name;

                CreateDirectoryA(base.c_str(), NULL);

                if (CreateDirectoryA(dir.c_str(), NULL) || GetFileAttributesA(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    resolved = dir;
                    return resolved;
                }
            }
        }
    }

    resolved = ".\\cache";

    if (CreateDirectoryA(resolved.c_str(), NULL) || GetFileAttributesA(resolved.c_str()) != INVALID_FILE_ATTRIBUTES)
        return resolved;

    return resolved;
}

static std::string moduleCachePath(const std::string& url)
{
    std::string dir = ensureCacheDir("module-cache");
    unsigned long hash = 2166136261u;
    char name[64];

    for (size_t i = 0; i < url.size(); ++i) {
        hash ^= (unsigned char)url[i];
        hash *= 16777619u;
    }

    snprintf(name, sizeof(name), "\\%08lx.mjs", hash);
    return dir + name;
}

static bool moduleCacheLoad(const std::string& url, std::string* body)
{
    std::string path;
    FILE* file;
    char buffer[4096];
    size_t got;
    long size;
    const char* ttlText;
    int ttl = 3600;

    if (!moduleCacheEnabled() || !body)
        return false;

    path = moduleCachePath(url);

    ttlText = getenv("AGB_MODULE_CACHE_TTL");

    if (ttlText)
        ttl = atoi(ttlText);

    if (ttl > 0 && !moduleOffline()) {
        /* Offline replay ignores TTL: the point is a deterministic run from
         * whatever the cache holds, however stale. */
        WIN32_FILE_ATTRIBUTE_DATA info;

        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info))
            return false;

        {
            ULARGE_INTEGER written;
            unsigned long long writtenSec;
            unsigned long long now = (unsigned long long)time(NULL);

            written.LowPart = info.ftLastWriteTime.dwLowDateTime;
            written.HighPart = info.ftLastWriteTime.dwHighDateTime;
            writtenSec = written.QuadPart / 10000000ULL;

            if (writtenSec > 11644473600ULL)
                writtenSec -= 11644473600ULL;
            else
                writtenSec = 0;

            if (now > writtenSec && (now - writtenSec) > (unsigned long long)ttl)
                return false;
        }
    }

    file = fopen(path.c_str(), "rb");

    if (!file)
        return false;

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size <= 0) {
        fclose(file);
        return false;
    }

    /* first line is the URL, so a hash collision is detected instead of served */
    got = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[got] = 0;

    {
        char* newline = strchr(buffer, '\n');

        if (!newline) {
            fclose(file);
            return false;
        }

        /* compare only the URL line: the buffer also holds the body after it */
        *newline = 0;

        if (url != buffer) {
            fclose(file);
            return false;
        }

        *newline = '\n';
        body->assign(newline + 1, got - (size_t)(newline + 1 - buffer));
    }

    for (;;) {
        char chunk[4096];
        size_t n = fread(chunk, 1, sizeof(chunk), file);

        if (n == 0)
            break;

        body->append(chunk, n);
    }

    fclose(file);
    return true;
}

static void moduleCacheStore(const std::string& url, const std::string& body)
{
    std::string path;
    FILE* file;

    if (!moduleCacheEnabled() || body.empty())
        return;

    path = moduleCachePath(url);
    file = fopen(path.c_str(), "wb");

    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[modcache] store %s -> %s (%s, err=%lu)\n", url.c_str(), path.c_str(),
                file ? "ok" : "FAILED", file ? 0UL : (unsigned long)GetLastError());

    if (!file)
        return;

    fprintf(file, "%s\n", url.c_str());
    fwrite(body.data(), 1, body.size(), file);
    fclose(file);
}

static void moduleStart(const std::string& url, bool force)
{
    ModuleEntry* entry = moduleEntryFor(url);

    if (entry->done || entry->id)
        return;

    /* file:// modules read straight from disk and complete synchronously;
     * their imports fan out here too. */
    if (url.compare(0, 7, "file://") == 0) {
        entry->done = true;
        entry->failed = !moduleReadLocalFile(url, &entry->body);

        if (!entry->failed)
            modulePrefetchImports(url, entry->body);

        return;
    }

    /* Disk cache first, for prefetches too. Serving the body from disk here
     * means the graph is built without network, no in-flight request is left
     * behind for a body that is already complete, and a reload of a warm graph
     * is as fast as the engine. The cache is content addressed by URL, which
     * is realm independent (only the in-flight part belongs to a realm).
     * Prefetch runs before evaluation, so a cache miss still has to fail the
     * entry in offline mode (moduleFetch only consults the cache for entries
     * that are not done yet). */
    if (moduleCacheLoad(url, &entry->body)) {
        entry->done = true;
        entry->failed = false;

        if (getenv("AGB_MODULE_DEBUG"))
            fprintf(stderr, "[modcache] hit(prefetch) %s\n", url.c_str());

        modulePrefetchImports(url, entry->body);
        return;
    }

    if (moduleOffline()) {
        entry->done = true;
        entry->failed = true;
        return;
    }

    /* The download belongs to the realm that asks for it: when that realm is
     * gone, the download is cancelled with it. */
    entry->generation = currentGeneration();

    if (g_moduleInFlight >= moduleFetchLimit()) {
        if (!force)
            moduleQueuePending(url, entry->generation);
        if (!force)
            return;
    }

    if (!g_moduleSession)
        g_moduleSession = Net::Session::create();

    if (!g_moduleSession)
        return;

    {
        Net::Request request;

        request.url = url;
        request.method = "GET";
        request.timeoutMs = 5000;

        entry->id = g_moduleSession->start(request, new ModuleFetchClient(url, entry->serial));

        if (!entry->id) {
            entry->done = true;
            entry->failed = true;
            return;
        }

        ++g_moduleInFlight;

        if (getenv("AGB_MODULE_DEBUG"))
            fprintf(stderr, "[modstart] %s in-flight=%d\n", url.c_str(), g_moduleInFlight);
    }
}

/* Entry point for external <script type="module" src>: load the body through
 * the module fetcher (the tokenizer and HTMLScriptElement call this instead
 * of CachedScript, so the entry script and its imports share one cache and
 * one cancel owner). Cache hits notify synchronously - the caller guards
 * against reentrancy the same way the CachedScript ref() path does. */
void fetchAndQueueModuleScript(const char* url)
{
    std::string u(url ? url : "");
    ModuleEntry* entry;

    if (u.empty())
        return;

    entry = moduleEntryFor(u);

    /* A failed entry may be the leftover of a realm teardown (the download was
     * cancelled with the document that asked for it), not a property of the
     * URL. Reloading the page must not inherit that failure: retry instead of
     * reporting the entry as broken forever. */
    if (entry->done && entry->failed) {
        entry->done = false;
        entry->failed = false;
        entry->body.clear();
    }

    if (entry->done) {
        notifyModuleEntry(u, &entry->body, entry->failed);
        return;
    }

    if (!g_moduleEntryScripts)
        g_moduleEntryScripts = new std::map<std::string, unsigned long>();

    (*g_moduleEntryScripts)[u] = currentGeneration();
    moduleStart(u, false);

    /* file:// entries (and offline-gated ones) complete synchronously inside
     * moduleStart: notify right away. The caller guards reentrancy, and this
     * still runs in the parse flow. */
    entry = moduleEntryFor(u);

    if (entry->done)
        notifyModuleEntry(u, &entry->body, entry->failed);
}

/* Queues the static imports of a freshly received module. */
static void modulePrefetchImports(const std::string& url, const std::string& body)
{
    size_t pos = 0;
    int found = 0;

    while ((pos = body.find("from", pos)) != std::string::npos) {
        size_t at = pos + 4;

        while (at < body.size() && (body[at] == ' ' || body[at] == '\t' || body[at] == '\r' || body[at] == '\n'))
            ++at;

        if (at < body.size() && (body[at] == '\'' || body[at] == '"')) {
            char quote = body[at];
            size_t close = body.find(quote, at + 1);

            if (close != std::string::npos && close > at + 1) {
                std::string specifier = body.substr(at + 1, close - at - 1);

                if (!specifier.empty() && specifier[0] == '.') {
                    ++found;
                    moduleStart(moduleUrlJoin(url, specifier), false);
                }

                pos = close;
                continue;
            }
        }

        pos = at;
    }

    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[modprefetch] %s found=%d in-flight=%d\n",
                url.c_str(), found, g_moduleInFlight);
}

static char* moduleFetch(const char* urlText, unsigned long* outSize)
{
    std::string url(urlText ? urlText : "");
    ModuleEntry* entry;
    unsigned long began;
    int attempt;

    /* Engine-space masqueraded file URLs map back to real file:// URLs
     * (local read; keys of the module caches stay real). */
    if (isEngineFileUrl(url))
        url = fileUrlForEngine(url);

    if (outSize)
        *outSize = 0;

    if (url.empty())
        return NULL;

    entry = moduleEntryFor(url);

    if (!entry->done && moduleCacheLoad(url, &entry->body)) {
        entry->done = true;
        entry->failed = false;

        if (getenv("AGB_MODULE_DEBUG"))
            fprintf(stderr, "[modcache] hit %s\n", url.c_str());

        /* A prefetch may still be running for this URL: it would append a
         * second copy of the file to the body just loaded from disk. */
        moduleCancelRequest(entry);
    }

    if (!entry->done && moduleOffline()) {
        if (getenv("AGB_MODULE_DEBUG"))
            fprintf(stderr, "[modfetch] OFFLINE miss %s\n", url.c_str());

        return NULL;
    }

    for (attempt = 0; attempt < 2; ++attempt) {
        if (!entry->done)
            moduleStart(url, true);

        began = GetTickCount();

        while (!entry->done && GetTickCount() - began < 5000) {
            if (g_moduleSession)
                g_moduleSession->pump();

            if (!entry->done)
                Sleep(0);
        }

        if (entry->done && !entry->failed)
            break;

        /* A prefetched or pooled request can fail; retry once from scratch. */
        if (attempt == 0) {
            if (g_moduleCache)
                g_moduleCache->erase(url);

            delete entry;
            entry = moduleEntryFor(url);

            if (getenv("AGB_MODULE_DEBUG"))
                fprintf(stderr, "[modfetch] retry %s\n", url.c_str());
        }
    }

    if (!entry->done || entry->failed) {
        if (getenv("AGB_MODULE_DEBUG"))
            fprintf(stderr, "[modfetch] FAILED %s\n", url.c_str());

        return NULL;
    }

    modulePrefetchImports(url, entry->body);

    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[modfetch] t=%lu %s %u bytes | in-flight=%d\n",
                (unsigned long)GetTickCount(), url.c_str(), (unsigned)entry->body.size(), g_moduleInFlight);

    {
        char* buffer = (char*)malloc(entry->body.size() + 1);

        if (!buffer)
            return NULL;

        memcpy(buffer, entry->body.data(), entry->body.size());
        buffer[entry->body.size()] = 0;

        if (outSize)
            *outSize = (unsigned long)entry->body.size();

        return buffer;
    }
}

bool start()
{
    startSampler();
    AntbEngine* engine;

    if (g_engine)
        return true;

    initCookieStorage();
    /* Ant needs the address of a stack variable from the calling thread. */
    volatile char stack_anchor = 0;
    engine = antb_engine_create((const void*)&stack_anchor);

    if (!engine)
        return false;

    activateState(createRuntimeState(engine));
    antb_set_context_activate(contextActivate);
    antb_set_module_fetcher(moduleFetch);

    /* Unhandled promise rejections must be logged, not fatal: one bad page
     * promise used to call process_exit_with(EXIT_FAILURE) and close the browser. */
    antb_set_uncaught_nonfatal(g_engine, 1);

    return true;
}

void* createContext()
{
    volatile char stack_anchor = 0;
    AntbEngine* engine = antb_engine_create((const void*)&stack_anchor);

    if (!engine)
        return NULL;

    if (!g_currentState || g_currentState == &g_emptyState) {
        /* First context in the process: the prelude and the bridge hooks are
         * installed lazily by bindDocument(), but the dispatch funnel must be
         * registered before any script runs. */
        antb_set_context_activate(contextActivate);
        activateState(createRuntimeState(engine));
        return g_currentState;
    }

    {
        RuntimeState* state = createRuntimeState(engine);

        return state;
    }
}

void destroyContext(void* context)
{
    RuntimeState* state = (RuntimeState*)context;

    if (!state || state == &g_emptyState)
        return;

    {
        std::vector<RuntimeState*>& states = runtimeStates();

        for (size_t i = 0; i < states.size(); ++i) {
            if (states[i] == state) {
                states.erase(states.begin() + i);
                break;
            }
        }
    }

    if (state->engine)
        antb_engine_destroy(state->engine);

    if (g_currentState == state)
        g_currentState = &g_emptyState;

    delete state;
}

void activateContext(void* context)
{
    if (context && context != &g_emptyState)
        activateState((RuntimeState*)context);
}

void registerPageContext(void* page, void* context)
{
    if (!page)
        return;

    {
        std::vector<std::pair<void*, void*> >& pages = pageContexts();

        for (size_t i = 0; i < pages.size(); ++i) {
            if (pages[i].first == page) {
                pages[i].second = context;
                return;
            }
        }

        pages.push_back(std::make_pair(page, context));
    }
}

void* contextForPage(void* page)
{
    std::vector<std::pair<void*, void*> >& pages = pageContexts();

    for (size_t i = 0; i < pages.size(); ++i) {
        if (pages[i].first == page)
            return pages[i].second;
    }

    return NULL;
}

void stop()
{
    std::vector<RuntimeState*>& states = runtimeStates();

    for (size_t i = 0; i < states.size(); ++i) {
        RuntimeState* state = states[i];

        if (state->engine)
            antb_engine_destroy(state->engine);

        delete state;
    }

    states.clear();
    g_currentState = &g_emptyState;
}

bool isRunning()
{
    return g_engine != NULL;
}

/* Scripts reach the engine through the tokenizer path, which does not know the
 * <script> carried type="module", so module syntax is detected here instead: a
 * script starting with import/export is evaluated as an ES module, which also
 * makes its relative imports resolve against the script URL. */
static bool looksLikeModule(const char* src, size_t len)
{
    size_t i = 0;

    for (; i < len; ++i) {
        char c = src[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            ++i;
            continue;
        }
        break;
    }

    if (i >= len)
        return false;

    /* <script type="module"> whose first statement is not an import still has to
     * be treated as a module. Imports are line-based in practice, so look for a
     * line that starts with import/xport. */
    {
        for (size_t at = i; at < len; ) {
            while (at < len && (src[at] == ' ' || src[at] == '\t')) ++at;

            if (at + 6 <= len && (!strncmp(src + at, "import", 6) || !strncmp(src + at, "export", 6))) {
                char after = (at + 6 < len) ? src[at + 6] : ' ';

                if (after == ' ' || after == '\t' || after == '{' || after == '*')
                    return true;
            }

            while (at < len && src[at] != '\n') ++at;
            if (at < len) ++at;
        }
    }

    if (len - i >= 6 && !strncmp(src + i, "import", 6)) {
        size_t j = i + 6;

        for (; j < len; ++j) {
            char c = src[j];

            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '(' || c == '.' || c == '{' || c == '*')
                return true;
            break;
        }
        return false;
    }

    if (len - i >= 6 && !strncmp(src + i, "export", 6)) {
        size_t j = i + 6;

        for (; j < len; ++j) {
            char c = src[j];

            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '*')
                return true;
            break;
        }
        return false;
    }

    return false;
}

/* Set once the document has been parsed: ES modules run only after that. */
static bool g_documentParsed = false;

/* Nesting depth of script evaluation: if it is > 1 while the load event is
 * dispatched, the VM is re-entering and that is worth knowing. */
static int g_evalDepth = 0;

int evaluate(const char* src, size_t len, char** out, char** err)
{
    AntbValue* value;
    struct DepthGuard {
        int* depth;
        explicit DepthGuard(int* d) : depth(d) { ++*d; }
        ~DepthGuard() {
            if (getenv("AGB_EVT_DEBUG"))
                fprintf(stderr, "[evt] evaluate leave depth=%d\n", *depth);
            --*depth;
        }
    } guard(&g_evalDepth);

    if (getenv("AGB_EVT_DEBUG"))
        fprintf(stderr, "[evt] evaluate enter depth=%d len=%d\n", g_evalDepth, (int)len);

    if (out)
        *out = NULL;
    if (err)
        *err = NULL;

    if (!g_engine || !src)
        return 0;

    /* ES module syntax cannot be evaluated as a classic script, and modules are
     * deferred by spec: running one while the parser is still building the
     * document makes it look for nodes that do not exist yet. */
    if (looksLikeModule(src, len)) {
        if (!g_documentParsed) {
            std::string body(src, len);

            queueModule(g_currentFilename.c_str(), body.c_str());
            return 1;
        }

        return evaluateModule(src, len, g_currentFilename.c_str(), out, err);
    }

    collectXhrJobs();
    flushStorageIfDirty();

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

void pumpAsyncRequests(void)
{
    /* Deliver finished requests first (JS callbacks), then let the engine run
     * its own timers/microtasks - both happen on the frame loop, never inside
     * the network pump. */
    collectXhrJobs();

    /* The module session has no driver of its own: prefetches and routed
     * entry scripts only complete when someone pumps. The frame loop is the
     * natural place (moduleFetch's wait loop covers the evaluation path). */
    if (g_moduleSession) {
        g_moduleSession->pump();

        moduleDrainPending();
    }

    /* Entry bodies that arrived during the pump above: outside the network
     * pump now. Hand them to the waiting tokenizer - the module itself runs
     * later, in the parse flow (see HTMLTokenizer::notifyModuleEntry). */
    if (g_arrivedEntryBodies && !g_arrivedEntryBodies->empty()) {
        std::vector<ArrivedEntryBody> arrived;

        arrived.swap(*g_arrivedEntryBodies);

        for (size_t i = 0; i < arrived.size(); ++i)
            notifyModuleEntry(arrived[i].url, arrived[i].failed ? NULL : &arrived[i].source,
                              arrived[i].failed);
    }

    if (g_engine) {
        antb_pump(g_engine);

        /* Timer callbacks are outside evaluate(), so report and clear anything
         * they threw instead of leaving it pending in the engine. */
        if (char* pending = antb_take_pending_error(g_engine)) {
            fprintf(stderr, "[antjs] (timer) %s\n", pending);
            free(pending);
        }
    }
}

void setFilename(const char* name)
{
    if (name)
        g_currentFilename = name;

    if (g_engine && name)
        antb_set_filename(g_engine, name);
}

/* --- ES modules ---------------------------------------------------------- */

/* ES modules are deferred by spec: they run after the document has been parsed,
 * not inline where the <script> sits. Running them during parsing made pages
 * look up DOM nodes that did not exist yet (css3test's whole test harness hung
 * on that). */
struct DeferredModule {
    std::string url;
    std::string source;
    unsigned long generation;   /* the realm that queued it */
    void* document;             /* the document it belongs to (bound before it
                                 * runs: the bridge may still point at the
                                 * frame's initial about:blank document) */
    bool isClassic;             /* classic <script defer> runs via evaluate(),
                                 * module scripts via evaluateModule() */
    DeferredModule() : generation(0), document(NULL), isClassic(false) {}
};

static std::vector<DeferredModule>* g_deferredModules = NULL;

/* Set after the post-parse flush ran for a document. A defer script that
 * arrives after that point (slow network, dynamically inserted) must still run,
 * so it executes immediately instead of sitting in a queue that is never
 * flushed. Keyed by document, not by generation: the frame's initial empty
 * document shares the realm generation with the page that replaces it. */
static void* g_lastFlushDocument = NULL;

int evaluateModule(const char* src, size_t len, const char* filename, char** out, char** err);

/* Starts downloading the import graph of a module script as soon as its body is
 * available, without waiting for the deferred execution. */
void prefetchModuleGraph(const char* url, const char* source)
{
    if (!url || !source)
        return;

    /* Only module sources: scanning a classic script for `from './x'` matches
     * arbitrary string literals and flooded the connection pool with bogus
     * requests, starving the scripts the parser was waiting for. */
    if (!looksLikeModule(source, strlen(source)))
        return;

    modulePrefetchImports(std::string(url), std::string(source));
}

void queueModule(const char* url, const char* source)
{
    if (!g_deferredModules)
        g_deferredModules = new std::vector<DeferredModule>();

    DeferredModule module;

    module.url = url ? url : "";
    module.source = source ? source : "";
    module.generation = currentGeneration();
    module.document = g_document;
    module.isClassic = false;
    g_deferredModules->push_back(module);

    /* Start downloading the import graph now, while the rest of the page is still
     * loading, instead of waiting for the deferred flush; Chrome starts as soon
     * as it sees the script tag. */
    modulePrefetchImports(module.url, module.source);
}

void flushDeferredModules(void* document)
{
    std::vector<DeferredModule> pending;

    /* The frame's initial empty document reaches DOMContentLoaded as well; it
     * has no body and no deferred work, so it must not seal the queue for the
     * page that replaces it (both share one realm generation). */
    if (!document || !((WebCore::Document*)document)->body())
        return;

    /* Mark the flush first: an empty queue returns early, but a defer script
     * that arrives after this point must not wait for a flush that already
     * happened (it would sit in the queue forever). */
    g_lastFlushDocument = document;

    if (!g_deferredModules || g_deferredModules->empty())
        return;

    /* Take the list first: running a script may queue more (dynamic import). */
    pending = *g_deferredModules;
    g_deferredModules->clear();

    for (size_t i = 0; i < pending.size(); ++i) {
        char* err = NULL;

        /* A script that another realm queued belongs to that realm: running
         * it here would execute a document's code in the wrong realm. */
        if (pending[i].generation != currentGeneration())
            continue;

        /* The bridge is bound to whatever document ran last, which is not
         * necessarily this script's document (the frame's initial about:blank
         * document is bound first, and a deferred script can be evaluated
         * straight from the resource callback). Bind it before running. */
        if (pending[i].document && pending[i].document != g_document)
            bindDocument(pending[i].document);

        if (pending[i].isClassic) {
            /* Classic <script defer>: plain global evaluation, in document
             * order. The document is fully parsed here, so document.body and
             * the rest of the DOM exist. */
            char* out = NULL;
            setFilename(pending[i].url.c_str());
            evaluate(pending[i].source.c_str(), pending[i].source.size(), &out, &err);
            if (out)
                free(out);
        } else {
            evaluateModule(pending[i].source.c_str(), pending[i].source.size(),
                           pending[i].url.c_str(), NULL, &err);
        }

        if (err) {
            fprintf(stderr, "[antjs] %s: %s\n", pending[i].url.c_str(), err);
            free(err);
        }
    }
}

void queueDeferredScript(void* document, const char* url, const char* source)
{
    /* The post-parse flush already ran for this document: nothing will consume
     * the queue anymore, so run the script right away (slow network, dynamic
     * insertion). */
    if (g_lastFlushDocument && g_lastFlushDocument == document) {
        char* out = NULL;
        char* err = NULL;

        if (document && document != g_document)
            bindDocument(document);

        setFilename(url ? url : "");
        evaluate(source ? source : "", source ? strlen(source) : 0, &out, &err);
        if (out)
            free(out);
        if (err) {
            fprintf(stderr, "[antjs] %s: %s\n", url ? url : "", err);
            free(err);
        }
        return;
    }

    if (!g_deferredModules)
        g_deferredModules = new std::vector<DeferredModule>();

    DeferredModule script;

    script.url = url ? url : "";
    script.source = source ? source : "";
    script.generation = currentGeneration();
    script.document = document;
    script.isClassic = true;
    g_deferredModules->push_back(script);
}

/* Drops the queued modules of a realm that is going away. */
static void cancelDeferredModulesForGeneration(unsigned long generation)
{
    if (!g_deferredModules)
        return;

    std::vector<DeferredModule>::iterator it = g_deferredModules->begin();

    while (it != g_deferredModules->end()) {
        if (it->generation <= generation)
            it = g_deferredModules->erase(it);
        else
            ++it;
    }
}

int evaluateModule(const char* src, size_t len, const char* filename, char** out, char** err)
{
    AntbValue* value;

    if (out)
        *out = NULL;
    if (err)
        *err = NULL;

    if (!g_engine || !src)
        return 0;

    collectXhrJobs();

    {
        unsigned long began = GetTickCount();

        /* file:// filenames masquerade as http://file.ant/... for the engine
         * (see the masquerade note by kEngineFileUrlPrefix). */
        std::string masqueraded;
        const char* engineFilename = filename;

        if (filename && isFileUrl(filename)) {
            masqueraded = engineUrlForFile(filename);
            engineFilename = masqueraded.c_str();
        }

        value = antb_eval_module(g_engine, src, (int)len, engineFilename, err);

        if (getenv("AGB_MODULE_DEBUG"))
            fprintf(stderr, "[modeval] %s %u bytes %s in %u ms\n",
                    filename ? filename : "(null)", (unsigned)len,
                    value ? "ok" : "FAILED", (unsigned)(GetTickCount() - began));
    }

    if (!value)
        return 0;

    if (out)
        *out = antb_to_utf8(value);

    antb_value_release(value);
    return 1;
}

/* Evaluates a bootstrap script that lives next to the binary (or at $envName).
 * prelude.js and polyfills.js both ship that way so they can be edited without
 * relinking the engine. Returns false when the file is missing or fails. */
static bool evalScriptFile(const char* envName, const char* fileName)
{
    char path[AGB_PATH_CHARS];
    char* buffer = NULL;
    FILE* file = NULL;
    long size = 0;
    bool ok = false;
    const char* env = envName ? getenv(envName) : NULL;

    if (env && env[0])
        file = fopen(env, "rb");

    if (!file) {
        DWORD len = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));

        if (len > 0 && len < sizeof(path)) {
            char* slash = strrchr(path, '\\');

            if (slash) {
                strcpy(slash + 1, fileName);
                file = fopen(path, "rb");
            }
        }
    }

    if (!file) {
        fprintf(stderr, "[agave] %s not found (set %s to load one)\n", fileName, envName);
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

            /* antb_eval returns the completion value, NULL only on an error. */
            AntbValue* result = g_engine ? antb_eval(g_engine, buffer, (int)read, NULL) : NULL;

            if (result) {
                ok = true;
                antb_value_release(result);
            } else
                fprintf(stderr, "[agave] %s failed to evaluate\n", fileName);

            free(buffer);
        }
    }

    fclose(file);
    return ok;
}

/* Boots the window shims for the document being bound. Read from prelude.js so
 * it can be edited with the same turnaround as polyfills.js. */
static void loadPrelude(void)
{
    if (evalScriptFile("AGB_PRELUDE", "prelude.js"))
        return;

    fprintf(stderr, "[agave] running without the window prelude: pages will most likely fail\n");
}

/* Loads polyfills.js (next to the binary, or $AGB_POLYFILLS) and evaluates it.
 * Kept out of the built-in prelude so the file can be edited without relinking
 * the engine. */
static void loadPolyfills(void)
{
    char path[AGB_PATH_CHARS];
    char* buffer = NULL;
    FILE* file = NULL;
    long size = 0;
    const char* env = getenv("AGB_POLYFILLS");

    /* Polyfills patch the VM, not the process: they have to run once per realm.
     * They used to run once per process, so the realm created by a reload
     * started without them - the page's `class X extends URLSearchParams` then
     * lost its own methods (the native class is branded and the wrapper that
     * makes subclassing work lives in polyfills.js). */
    if (!g_currentState || g_currentState == &g_emptyState || !g_engine)
        return;

    if (g_currentState->polyfillsGeneration == g_currentState->generation)
        return;

    g_currentState->polyfillsGeneration = g_currentState->generation;

    if (env && env[0])
        file = fopen(env, "rb");

    if (!file) {
        DWORD len = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));

        if (len > 0 && len < sizeof(path)) {
            char* slash = strrchr(path, '\\');

            if (slash) {
                strcpy(slash + 1, "polyfills.js");
                file = fopen(path, "rb");
            }
        }
    }

    if (!file) {
        fprintf(stderr, "[agave] polyfills.js not found (set AGB_POLYFILLS to load one)\n");
        return;
    }

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size > 0) {
        buffer = (char*)malloc((size_t)size + 1);

        if (buffer) {
            size_t read = fread(buffer, 1, (size_t)size, file);

            buffer[read] = 0;

            if (g_engine && !antb_eval(g_engine, buffer, (int)read, NULL))
                fprintf(stderr, "[agave] polyfills.js failed to evaluate\n");

            free(buffer);
        }
    }

    fclose(file);
}

/* ------------------------------------------------------------------------- */
/* Realm isolation: one realm per document                                     */
/* ------------------------------------------------------------------------- */
/* The Ant VM used to be one per VIEW, so a reload left the previous page's
 * state in place: its globals (window.foo), its listeners and its module
 * registry were all still there when the next document started running.
 *
 * Isolation instead gives every document its own realm: the old realm is
 * stopped and unloaded (AntRuntime::documentWillUnload) and a fresh one is
 * started for the next document (AntRuntime::beginNewRealm).
 *
 * The host keeps the context handle (the RuntimeState), so only the engine
 * inside the state is replaced - the Page -> context mapping stays valid. */
/* The generation whose pins are waiting to be released. The nodes of the
 * outgoing document are still used by WebCore while the realm is torn down,
 * so they are released one generation later. */
static unsigned long g_pinsPendingRelease = 0;

/* Cancels every resource that belongs to the given realm (inclusive) and is
 * still in flight: XHR requests, module downloads and queued imports. Called
 * before the realm that owns them goes away, so nothing that arrives later
 * can reach the realm that replaces it. */
static void cancelInFlightResources(unsigned long generation)
{
    if (!generation)
        return;

    cancelXhrJobsForGeneration(generation);
    cancelModuleDownloadsForGeneration(generation);
    cancelDeferredModulesForGeneration(generation);

    /* Routed entry scripts of the dying realm: their arrival would queue code
     * into a page that is going away. Notify failed so a tokenizer that is
     * blocked waiting for one of them can resume (no JS runs in this notify). */
    if (g_moduleEntryScripts) {
        std::map<std::string, unsigned long>::iterator it = g_moduleEntryScripts->begin();

        while (it != g_moduleEntryScripts->end()) {
            if (it->second == generation) {
                notifyModuleEntry(it->first, NULL, true);
                g_moduleEntryScripts->erase(it++);
            } else
                ++it;
        }
    }
}

/* Stops the current realm: the page's JS world goes away with its document.
 * The VM is destroyed, so its globals, its module registry and its timers
 * cannot be seen by the next document. */
static void stopRealm(void)
{
    RuntimeState* state = g_currentState;

    if (!state || state == &g_emptyState)
        return;

    /* Stop the realm's resources first: a response that arrives later must
     * not be delivered into the realm that replaces this one. */
    cancelInFlightResources(state->generation);

    /* The nodes of the previous realm can be released now (their document
     * was replaced a generation ago). */
    if (g_pinsPendingRelease)
        releasePinnedNodes(g_pinsPendingRelease);

    g_pinsPendingRelease = state->generation;

    /* Values owned by the old engine have to go before the engine does. */
    if (g_docObj) {
        antb_value_release(g_docObj);
        g_docObj = 0;
    }
    if (g_locationObj) {
        antb_value_release(g_locationObj);
        g_locationObj = 0;
    }

    /* The old page's listeners close over its DOM. */
    dropListenersForOldDocument();

    /* ...and the JS wrappers of its nodes must not be handed out to the next
     * document (the bridge keeps them in a process wide cache). */
    if (state->engine && g_document)
        antb_release_owner(state->engine, g_document);

    /* The node class belongs to the old engine. */
    g_nodeClass = 0;
    g_canvasCtxClass = 0;
    clearExpandoObjects();
    g_documentBound = false;

    /* The slots point at documents that are going away: keeping them would
     * hand out pointers to freed documents. */
    for (int i = 0; i < g_docSlotCount; ++i) {
        g_docSlots[i] = 0;
        g_docSlotGeneration[i] = 0;
    }

    g_docSlotCount = 0;
    g_currentSlot = -1;

    if (state->engine) {
        antb_engine_destroy(state->engine);
        state->engine = 0;
    }

    if (getenv("AGB_EVT_DEBUG"))
        fprintf(stderr, "[evt] realm stopped (generation=%lu)\n", state->generation);
}

/* Starts a fresh realm (a new VM) for the next document. Called before the new
 * document is parsed, so that its module graph is registered in the new realm
 * and not in the one that is going away. */
void beginNewRealm(void)
{
    RuntimeState* state = g_currentState;

    if (!state || state == &g_emptyState)
        return;

    if (state->engine)
        return;   /* a realm is already running */

    /* Every realm gets a fresh id: the resources of the old one are
     * cancelled by it, the new ones belong to the new id. */
    state->generation = nextGeneration();

    {
        /* Ant bounds the JS stack with the address of a local in the calling
         * thread - the same thread that creates the view. */
        volatile char stack_anchor = 0;
        state->engine = antb_engine_create((const void*)&stack_anchor);
    }

    if (state->engine)
        antb_set_uncaught_nonfatal(state->engine, 1);

    if (getenv("AGB_EVT_DEBUG"))
        fprintf(stderr, "[evt] realm started (generation=%lu, engine=%p)\n",
                state->generation, (void*)state->engine);
}

void bindDocument(void* document)
{
    WebCore::Document* previous = g_document;

    /* The Document object is replaced on every navigation, so always refresh
     * the pointer; only the JS shim objects are created once. */
    g_document = (WebCore::Document*)document;

    /* Binding the document that is already installed is a no-op. A page with
     * async ad scripts gets here 3-6 times for the same document, and the
     * redundant passes make the VM run a closure body once per pass (css3test
     * ran its whole suite several times over). */
    if (g_documentBound && previous == (WebCore::Document*)document && g_docObj)
        return;

    g_currentSlot = slotForDocument(g_document);

    if (getenv("AGB_EVT_DEBUG"))
        fprintf(stderr, "[evt] bindDocument %p (bound=%d) url=%s\n", document, (int)g_documentBound,
                g_document ? g_document->URL().utf8().data() : "?");

    if (!g_engine || !g_document)
        return;

    if (g_documentBound) {
        AntbValue* global;

        if (g_docObj)
            antb_set_native(g_docObj, g_document, AGB_TAG_NODE);

        /* window IS the global object and nodeAddEventListener() reads the node
         * from it. Refreshing only g_docObj left the window pointing at the
         * document that was replaced by the navigation, so a reloaded page
         * registered its handlers against the old document, the load event
         * matched no listener and css3test's tests never started again. */
        global = antb_global(g_engine);

        if (global) {
            antb_set_native(global, g_document, AGB_TAG_NODE);
            antb_value_release(global);
        }
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
        antb_define_property_rw(g_engine, docObj, "cookie", titleGetter, titleSetter, NULL);

        if (!g_nodeClass) {
            g_nodeClass = antb_class_create(g_engine, "Node");
            if (g_nodeClass) {
                antb_class_add_method(g_engine, g_nodeClass, "getAttribute", nodeGetAttribute, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "addEventListener", nodeAddEventListener, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "removeEventListener", nodeRemoveEventListener, NULL);
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
                antb_class_add_method(g_engine, g_nodeClass, "click", nodeClick, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "removeAttribute", nodeRemoveAttribute, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "remove", nodeRemove, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "getBoundingClientRect", nodeGetBoundingClientRect, NULL);
                /* Canvas: no per-tag classes exist, so the two element-side
                 * canvas APIs live here and check the tag themselves. */
                antb_class_add_method(g_engine, g_nodeClass, "getContext", nodeGetContext, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "toDataURL", nodeToDataURL, NULL);

                /* Catch-all property: the class dispatching getter only serves
                 * names present in this table and returns undefined for
                 * everything else, so without this entry every node property
                 * (tagName/innerHTML/nodeType/...) read as undefined - Vue's
                 * mount() got an undefined container.innerHTML and gave up
                 * with "Component is missing template or render function".
                 * nodeGetter dispatches on the name itself. */
                antb_class_set_property(g_engine, g_nodeClass, "", nodeGetter, NULL);
            }
        }

        if (!g_canvasCtxClass) {
            g_canvasCtxClass = antb_class_create(g_engine, "CanvasRenderingContext2D");
            if (g_canvasCtxClass) {
                antb_class_add_method(g_engine, g_canvasCtxClass, "save", canvasCtxMethod, (void*)(intptr_t)CV_SAVE);
                antb_class_add_method(g_engine, g_canvasCtxClass, "restore", canvasCtxMethod, (void*)(intptr_t)CV_RESTORE);
                antb_class_add_method(g_engine, g_canvasCtxClass, "scale", canvasCtxMethod, (void*)(intptr_t)CV_SCALE);
                antb_class_add_method(g_engine, g_canvasCtxClass, "rotate", canvasCtxMethod, (void*)(intptr_t)CV_ROTATE);
                antb_class_add_method(g_engine, g_canvasCtxClass, "translate", canvasCtxMethod, (void*)(intptr_t)CV_TRANSLATE);
                antb_class_add_method(g_engine, g_canvasCtxClass, "clearRect", canvasCtxMethod, (void*)(intptr_t)CV_CLEAR_RECT);
                antb_class_add_method(g_engine, g_canvasCtxClass, "fillRect", canvasCtxMethod, (void*)(intptr_t)CV_FILL_RECT);
                antb_class_add_method(g_engine, g_canvasCtxClass, "strokeRect", canvasCtxMethod, (void*)(intptr_t)CV_STROKE_RECT);
                antb_class_add_method(g_engine, g_canvasCtxClass, "beginPath", canvasCtxMethod, (void*)(intptr_t)CV_BEGIN_PATH);
                antb_class_add_method(g_engine, g_canvasCtxClass, "closePath", canvasCtxMethod, (void*)(intptr_t)CV_CLOSE_PATH);
                antb_class_add_method(g_engine, g_canvasCtxClass, "moveTo", canvasCtxMethod, (void*)(intptr_t)CV_MOVE_TO);
                antb_class_add_method(g_engine, g_canvasCtxClass, "lineTo", canvasCtxMethod, (void*)(intptr_t)CV_LINE_TO);
                antb_class_add_method(g_engine, g_canvasCtxClass, "quadraticCurveTo", canvasCtxMethod, (void*)(intptr_t)CV_QUADRATIC);
                antb_class_add_method(g_engine, g_canvasCtxClass, "bezierCurveTo", canvasCtxMethod, (void*)(intptr_t)CV_BEZIER);
                antb_class_add_method(g_engine, g_canvasCtxClass, "arc", canvasCtxMethod, (void*)(intptr_t)CV_ARC);
                antb_class_add_method(g_engine, g_canvasCtxClass, "arcTo", canvasCtxMethod, (void*)(intptr_t)CV_ARC_TO);
                antb_class_add_method(g_engine, g_canvasCtxClass, "rect", canvasCtxMethod, (void*)(intptr_t)CV_RECT);
                antb_class_add_method(g_engine, g_canvasCtxClass, "fill", canvasCtxMethod, (void*)(intptr_t)CV_FILL);
                antb_class_add_method(g_engine, g_canvasCtxClass, "stroke", canvasCtxMethod, (void*)(intptr_t)CV_STROKE);
                antb_class_add_method(g_engine, g_canvasCtxClass, "clip", canvasCtxMethod, (void*)(intptr_t)CV_CLIP);
                antb_class_add_method(g_engine, g_canvasCtxClass, "drawImage", canvasCtxMethod, (void*)(intptr_t)CV_DRAW_IMAGE);
                antb_class_add_method(g_engine, g_canvasCtxClass, "createLinearGradient", canvasCtxMethod, (void*)(intptr_t)CV_CREATE_LINEAR);
                antb_class_add_method(g_engine, g_canvasCtxClass, "createRadialGradient", canvasCtxMethod, (void*)(intptr_t)CV_CREATE_RADIAL);
                antb_class_add_method(g_engine, g_canvasCtxClass, "createPattern", canvasCtxMethod, (void*)(intptr_t)CV_CREATE_PATTERN);
                antb_class_add_method(g_engine, g_canvasCtxClass, "measureText", canvasCtxMethod, (void*)(intptr_t)CV_MEASURE_TEXT);
                antb_class_add_method(g_engine, g_canvasCtxClass, "fillText", canvasCtxMethod, (void*)(intptr_t)CV_FILL_TEXT);
                antb_class_add_method(g_engine, g_canvasCtxClass, "strokeText", canvasCtxMethod, (void*)(intptr_t)CV_STROKE_TEXT);
                antb_class_add_method(g_engine, g_canvasCtxClass, "setTransform", canvasCtxMethod, (void*)(intptr_t)CV_SET_TRANSFORM);
                antb_class_add_method(g_engine, g_canvasCtxClass, "transform", canvasCtxMethod, (void*)(intptr_t)CV_SET_TRANSFORM);
                antb_class_add_method(g_engine, g_canvasCtxClass, "setLineDash", canvasCtxMethod, (void*)(intptr_t)CV_SET_LINE_DASH);
                antb_class_add_method(g_engine, g_canvasCtxClass, "getLineDash", canvasCtxMethod, (void*)(intptr_t)CV_GET_LINE_DASH);
                antb_class_add_method(g_engine, g_canvasCtxClass, "getImageData", canvasCtxMethod, (void*)(intptr_t)CV_GET_IMAGE_DATA);
                antb_class_add_method(g_engine, g_canvasCtxClass, "putImageData", canvasCtxMethod, (void*)(intptr_t)CV_PUT_IMAGE_DATA);
                antb_class_add_method(g_engine, g_canvasCtxClass, "isPointInPath", canvasCtxMethod, (void*)(intptr_t)CV_IS_POINT_IN_PATH);
            }
        }

        antb_define_method(g_engine, docObj, "getElementById", docGetElementById, NULL);
        antb_define_method(g_engine, docObj, "createElement", docCreateElement, NULL);
        antb_define_method(g_engine, docObj, "createElementNS", docCreateElementNS, NULL);
        antb_define_method(g_engine, docObj, "createTextNode", docCreateTextNode, NULL);
        antb_define_method(g_engine, docObj, "createComment", docCreateComment, NULL);
        antb_define_method(g_engine, docObj, "createDocumentFragment", docCreateDocumentFragment, NULL);
        /* document.implementation: jQuery and others feature-test
         * document.implementation.createHTMLDocument(). */
        {
            AntbValue* implObj = antb_make_object(g_engine);
            if (implObj) {
                antb_define_method(g_engine, implObj, "createHTMLDocument", docCreateHTMLDocument, NULL);
                antb_define_method(g_engine, implObj, "hasFeature", docHasFeature, NULL);
                antb_set_prop(g_engine, docObj, "implementation", implObj);
                antb_value_release(implObj);
            }
        }
        antb_define_method(g_engine, docObj, "createHTMLDocument", docCreateHTMLDocument, NULL);
        antb_define_method(g_engine, docObj, "write", docWrite, NULL);
        antb_define_method(g_engine, docObj, "writeln", docWriteln, NULL);
        antb_define_method(g_engine, docObj, "addEventListener", nodeAddEventListener, NULL);
        antb_define_method(g_engine, docObj, "removeEventListener", nodeRemoveEventListener, NULL);
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
        antb_define_method(g_engine, window, "removeEventListener", nodeRemoveEventListener, NULL);
        antb_define_method(g_engine, window, "dispatchEvent", nodeDispatchEvent, NULL);

        antb_set_prop(g_engine, window, "document", docObj);
        antb_define_method(g_engine, global, "__agbHttpGet", globalHttpGet, NULL);
        antb_define_method(g_engine, global, "__agbHttpRequest", globalHttpRequest, NULL);
        /* Binary variants: base64 payloads for XHR responseType=arraybuffer and
         * fetch().arrayBuffer() (wasm, zip, images). */
        antb_define_method(g_engine, global, "__agbHttpGetBin", globalHttpGetBin, NULL);
        antb_define_method(g_engine, global, "__agbHttpRequestBin", globalHttpRequestBin, NULL);
        /* (url, method, contentType, body, cb) - POST/PUT with a text body. */
        antb_define_method(g_engine, global, "__agbHttpRequestEx", globalHttpRequestEx, NULL);
        antb_define_method(g_engine, global, "__agbClassOp", globalClassOp, NULL);
        antb_define_method(g_engine, global, "__agbFrameSlot", globalFrameSlot, NULL);
        antb_define_method(g_engine, global, "__agbActivateSlot", globalActivateSlot, NULL);
        antb_define_method(g_engine, global, "__agbOpenWindow", globalOpenWindow, NULL);
        antb_define_method(g_engine, global, "__agbPostMessage", globalPostMessage, NULL);
        antb_define_method(g_engine, global, "__agbDialog", globalDialog, NULL);
        antb_define_method(g_engine, global, "__agbCloseWindow", globalCloseWindow, NULL);
        antb_define_method(g_engine, global, "__agbStorageGet", globalStorageGet, NULL);
        antb_define_method(g_engine, global, "__agbStorageHas", globalStorageHas, NULL);
        antb_define_method(g_engine, global, "__agbStorageSet", globalStorageSet, NULL);
        antb_define_method(g_engine, global, "__agbStorageDel", globalStorageDel, NULL);
        antb_define_method(g_engine, global, "__agbStorageClear", globalStorageClear, NULL);
        antb_define_method(g_engine, global, "__agbStorageKey", globalStorageKey, NULL);
        antb_define_method(g_engine, global, "__agbStorageCount", globalStorageCount, NULL);
        antb_define_method(g_engine, global, "__agbComputedStyle", globalComputedStyle, NULL);

        /* history: SPA routers and "go back" links use it. */
        {
            AntbValue* hist = antb_make_object(g_engine);
            if (hist) {
                antb_define_method(g_engine, hist, "back", historyBack, NULL);
                antb_define_method(g_engine, hist, "forward", historyForward, NULL);
                antb_define_method(g_engine, hist, "go", historyGo, NULL);
                antb_define_method(g_engine, hist, "pushState", historyPushState, NULL);
                antb_define_method(g_engine, hist, "replaceState", historyPushState, NULL);
                antb_set_prop(g_engine, global, "history", hist);
                antb_value_release(hist);
            }
        }
        /* Web pages read bare globals like `location` and `navigator`. */
        {
            AntbValue* loc = antb_make_object(g_engine);
            if (loc) {
                antb_define_property_rw(g_engine, loc, "href", locationGetter, locationSetter, NULL);
                antb_define_method(g_engine, loc, "assign", locationAssign, NULL);
                antb_define_method(g_engine, loc, "replace", locationReplace, NULL);
                antb_define_method(g_engine, loc, "reload", locationReload, NULL);
                antb_define_method(g_engine, loc, "toString", locationToString, NULL);
                antb_define_method(g_engine, loc, "valueOf", locationToString, NULL);
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

    /* Bootstrap shims for this document, from prelude.js next to the binary
     * (or at AGB_PRELUDE). */
    loadPrelude();

    /* Polyfills live in polyfills.js next to the binary (or at AGB_POLYFILLS)
     * so they can be edited without rebuilding the engine. */
    loadPolyfills();

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
    bool           once;    /* remove after the first dispatch */
};

/* Per-state listener list (see RuntimeState::listeners). */
static std::vector<Listener>*& listeners()
{
    return *(std::vector<Listener>**)&g_currentState->listeners;
}

#define g_listeners listeners()


/* Navigation dropped the previous document: its listeners reference nodes and
 * closures from that page, so drop them (closing over freed DOM would fire on
 * the wrong elements). Nodes stay pinned, so no stale pointer can be handed
 * out for a recycled address. */
void dropListenersForOldDocument()
{
    if (!g_listeners)
        return;

    for (size_t i = 0; i < g_listeners->size(); ++i) {
        if ((*g_listeners)[i].fn)
            antb_value_release((*g_listeners)[i].fn);
    }
    g_listeners->clear();
}

WebCore::Node* nodeOf(AntbValue* self)
{
    return self ? (WebCore::Node*)antb_get_native(self, AGB_TAG_NODE) : 0;
}

/* ------------------------------------------------------------------------- */
/* Inline style object                                                        */
/*                                                                            */
/* The bridge installs ONE getter/setter pair per object that dispatches on the */
/* property name (the same scheme nodeGetter/nodeSetter use), so the whole      */
/* CSSStyleDeclaration is a single dispatcher rather than one entry per         */
/* property.                                                                    */
/* ------------------------------------------------------------------------- */

static AntbValue* makeStringResult(AntbEngine* engine, AntbValue* out, const char* text);

/* "backgroundColor" -> "background-color", "WebkitBoxShadow" -> "-webkit-box-shadow" */
static std::string cssNameFromCamel(const char* name)
{
    std::string out;

    if (!name)
        return out;

    for (const char* p = name; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') {
            out += '-';
            out += (char)(*p - 'A' + 'a');
        } else {
            out += *p;
        }
    }

    if (out == "css-float")
        out = "float";

    return out;
}

/* The element a style object belongs to (stored as __agbElement). */
/* element.dataset: a DOMStringMap over the data-* attributes. Same one-dispatcher
 * trick as style: the getter/setter receive the property name and map it to a
 * data- attribute (camelCase -> kebab-case). Vue directives read it constantly
 * (browserscore's v-content does `el.dataset.fallback`). */
/* SVG viewBox -> SVGAnimatedRect. browserscore clones its favicon SVG and
 * writes `clone.viewBox.baseVal.x/y/width/height` before serialising it, so
 * the four fields have to read *and write* the viewBox attribute. */
static WebCore::Element* elementOfValue(AntbValue* self)
{
    AntbValue* owner;
    WebCore::Node* node;

    if (!self)
        return NULL;

    owner = antb_get_prop(g_engine, self, "__agbElement");

    if (!owner)
        return NULL;

    node = nodeOf(owner);
    antb_value_release(owner);

    return (node && node->isElementNode()) ? static_cast<WebCore::Element*>(node) : NULL;
}

static bool parseViewBox(WebCore::Element* element, float* values)
{
    WebCore::String text;
    const char* data;
    int found = 0;

    values[0] = values[1] = values[2] = values[3] = 0;

    if (!element)
        return false;

    text = element->getAttribute(WebCore::AtomicString("viewBox"));

    if (text.isEmpty())
        return false;

    data = text.utf8().data();

    while (data && *data && found < 4) {
        char* end = NULL;
        double v = strtod(data, &end);

        if (end == data) {
            ++data;
            continue;
        }

        values[found++] = (float)v;
        data = end;
    }

    return found == 4;
}

static void storeViewBox(WebCore::Element* element, const float* values)
{
    char buffer[160];
    WebCore::ExceptionCode ec = 0;

    snprintf(buffer, sizeof(buffer), "%g %g %g %g",
             values[0], values[1], values[2], values[3]);

    element->setAttribute(WebCore::String("viewBox"), WebCore::String(buffer), ec);
    element->setChanged();
}

static int viewBoxFieldIndex(const char* name)
{
    if (!name)
        return -1;
    if (!strcmp(name, "x"))
        return 0;
    if (!strcmp(name, "y"))
        return 1;
    if (!strcmp(name, "width"))
        return 2;
    if (!strcmp(name, "height"))
        return 3;
    return -1;
}

int viewBoxFieldGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    WebCore::Element* element = elementOfValue(self);
    float values[4];
    int index = viewBoxFieldIndex(name);

    if (index < 0) {
        antb_value_assign(out, antb_make_number(engine, 0));
        return 1;
    }

    if (!parseViewBox(element, values))
        values[0] = values[1] = values[2] = values[3] = 0;

    antb_value_assign(out, antb_make_number(engine, values[index]));
    return 1;
}

int viewBoxFieldSetter(AntbEngine* engine, AntbValue* self, const char* name, const AntbValue* value, void*)
{
    if (getenv("AGB_CALL_DEBUG"))
        fprintf(stderr, "[vb] set %s value=%p self=%p\n", name ? name : "?", (void*)value, (void*)self);
    WebCore::Element* element = elementOfValue(self);
    float values[4];
    int index = viewBoxFieldIndex(name);

    (void)engine;

    if (index < 0 || !element)
        return 1;

    parseViewBox(element, values);

    {
        double numeric = value ? antb_to_number(const_cast<AntbValue*>(value)) : 0.0;

        if (numeric != numeric)
            numeric = 0.0;

        values[index] = (float)numeric;
    }

    storeViewBox(element, values);
    return 1;
}

static WebCore::Element* datasetElementOf(AntbValue* self)
{
    AntbValue* owner;
    WebCore::Node* node;

    if (!self)
        return NULL;

    owner = antb_get_prop(g_engine, self, "__agbElement");

    if (!owner)
        return NULL;

    node = nodeOf(owner);
    antb_value_release(owner);

    return (node && node->isElementNode()) ? static_cast<WebCore::Element*>(node) : NULL;
}

static std::string dataAttributeName(const char* name)
{
    std::string attr = "data-";

    for (const char* c = name; c && *c; ++c) {
        if (*c >= 'A' && *c <= 'Z') {
            attr += '-';
            attr += (char)(*c - 'A' + 'a');
        } else {
            attr += *c;
        }
    }

    return attr;
}

int datasetGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    WebCore::Element* element = datasetElementOf(self);

    if (!element || !name) {
        makeStringResult(engine, out, "");
        return 1;
    }

    {
        std::string attr = dataAttributeName(name);
        WebCore::String value = element->getAttribute(WebCore::AtomicString(attr.c_str()));

        makeStringResult(engine, out, value.utf8().data());
    }
    return 1;
}

int datasetSetter(AntbEngine* engine, AntbValue* self, const char* name, const AntbValue* value, void*)
{
    WebCore::Element* element = datasetElementOf(self);
    char* text;

    (void)engine;

    if (!element || !name)
        return 1;

    {
        std::string attr = dataAttributeName(name);
        WebCore::String attrName(attr.c_str());
        WebCore::ExceptionCode ec = 0;

        text = value ? antb_to_utf8(const_cast<AntbValue*>(value)) : NULL;

        if (!text || !text[0])
            element->removeAttribute(attrName, ec);
        else
            element->setAttribute(attrName, WebCore::String::fromUTF8(text), ec);

        antb_free(text);
    }
    element->setChanged();
    return 1;
}

static WebCore::StyledElement* styledElementOf(AntbValue* self)
{
    AntbValue* owner;
    WebCore::Node* node;

    if (!self)
        return NULL;

    owner = antb_get_prop(g_engine, self, "__agbElement");

    if (!owner)
        return NULL;

    node = nodeOf(owner);
    antb_value_release(owner);

    if (!node || !node->isStyledElement())
        return NULL;

    return static_cast<WebCore::StyledElement*>(node);
}

static std::string stylePropertyText(WebCore::StyledElement* element, const char* name)
{
    WebCore::String value;

    if (!element || !name)
        return std::string();

    value = element->style()->getPropertyValue(WebCore::String(cssNameFromCamel(name).c_str()));
    return std::string(value.utf8().data());
}

/* Dispatcher for every property read on a style object. */
static int styleGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    ProfScope prof(PROF_STYLE_GET);

    WebCore::StyledElement* element = styledElementOf(self);

    if (getenv("AGB_STYLE_DEBUG")) {
        static int reads = 0;
        static unsigned long insideTotal = 0;
        unsigned long began = GetTickCount();

        ++reads;

        {
            /* Measure the whole read path, not just the lookup. */
            struct ReadTimer {
                unsigned long began;
                ~ReadTimer() {
                    insideTotal += GetTickCount() - began;
                    if (reads % 2000 == 0)
                        fprintf(stderr, "[style] reads=%d inside=%u ms (avg %u us)\n",
                                reads, (unsigned)insideTotal,
                                (unsigned)(insideTotal * 1000 / reads));
                }
            } timer = { began };

            (void)timer;
        }
    }


    if (!name) {
        makeStringResult(engine, out, "");
        return 1;
    }

    if (!strcmp(name, "cssText")) {
        WebCore::String text = element ? element->style()->cssText() : WebCore::String();

        makeStringResult(engine, out, text.utf8().data());
        return 1;
    }

    /* CSSStyleDeclaration.length / item(i) describe the declaration itself, not
     * a property lookup: css3test decides "did the engine accept this value?"
     * by `inline[prop] = value; inline.length > 0`, so these must be real. */
    if (!strcmp(name, "length")) {
        AntbValue* v = antb_make_number(engine, element ? (double)element->style()->length() : 0.0);
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    /* setProperty / getPropertyValue / removeProperty / item come from the
     * prelude helper object, so they behave like prototype methods. */
    if (!strcmp(name, "setProperty") || !strcmp(name, "getPropertyValue")
            || !strcmp(name, "removeProperty") || !strcmp(name, "item")) {
        AntbValue* global = antb_global(engine);
        AntbValue* api = global ? antb_get_prop(engine, global, "__agbStyleApi") : NULL;
        AntbValue* member = api ? antb_get_prop(engine, api, name) : NULL;

        if (member && !antb_is_number(member)) {
            antb_value_assign(out, member);
            antb_value_release(member);
            antb_value_release(api);
            antb_value_release(global);
            return 1;
        }

        antb_value_release(member);
        antb_value_release(api);
        antb_value_release(global);

        makeStringResult(engine, out, "");
        return 1;
    }

    makeStringResult(engine, out, stylePropertyText(element, name).c_str());
    return 1;
}

/* Dispatcher for every property write on a style object. */
static int styleSetter(AntbEngine* engine, AntbValue* self, const char* name, const AntbValue* value, void*)
{
    ProfScope prof(PROF_STYLE_SET);

    traceOp("style %s", name ? name : "?");
    WebCore::StyledElement* element = styledElementOf(self);
    WebCore::ExceptionCode ec = 0;
    char* text;

    (void)engine;

    if (!element || !name)
        return 1;

    text = antb_to_utf8(const_cast<AntbValue*>(value));

    if (getenv("AGB_STYLE_DEBUG"))
        fprintf(stderr, "[style] set %s = '%s'\n", name, text ? text : "(null)");

    if (!strcmp(name, "cssText")) {
        element->style()->setCssText(WebCore::String::fromUTF8(text ? text : ""), ec);
    } else {
        std::string property = cssNameFromCamel(name);

        if (!text || !text[0])
            element->style()->removeProperty(WebCore::String(property.c_str()), ec);
        else
            element->style()->setProperty(WebCore::String(property.c_str()),
                                          WebCore::String::fromUTF8(text),
                                          WebCore::String(""), ec);
    }

    antb_free(text);
    element->setChanged();
    return 1;
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

/* Per-node expando object (el._vnode, el._assign, input._wrapperState, jQuery
 * data...). It is owned here instead of on the wrapper because every property
 * read on a wrapper goes through nodeGetter: a `__agbExpando` field stored on
 * the wrapper itself comes back undefined from inside that hook, so the old
 * scheme built a fresh side object on every write and silently dropped all of
 * its fields (React lost input._wrapperState -> "reading 'initialChecked'").
 * Entries die with the realm (clearExpandoObjects), like the wrapper cache. */
static std::map<WebCore::Node*, AntbValue*>* g_expandoObjects = NULL;

static AntbValue* expandoFor(AntbEngine* engine, WebCore::Node* node, bool create)
{
    if (!engine || !node)
        return NULL;

    if (!g_expandoObjects)
        g_expandoObjects = new std::map<WebCore::Node*, AntbValue*>();

    {
        std::map<WebCore::Node*, AntbValue*>::iterator it = g_expandoObjects->find(node);

        if (it != g_expandoObjects->end())
            return it->second;
    }

    if (!create)
        return NULL;

    {
        AntbValue* obj = antb_make_object(engine);
        AntbValue* kept;

        if (!obj)
            return NULL;

        kept = antb_retain(obj);   /* pinned for the realm's lifetime */
        antb_value_release(obj);

        if (!kept)
            return NULL;

        (*g_expandoObjects)[node] = kept;
        return kept;
    }
}

static void clearExpandoObjects()
{
    if (!g_expandoObjects)
        return;

    for (std::map<WebCore::Node*, AntbValue*>::iterator it = g_expandoObjects->begin();
         it != g_expandoObjects->end(); ++it)
        antb_value_release(it->second);

    g_expandoObjects->clear();
}

int nodeGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    /* Internal bookkeeping names must not go back through the bridge: reading
     * __agbExpando from the setter's own object would re-enter this getter and
     * recurse until the stack overflows. */
    if (name && name[0] == '_' && !strncmp(name, "__agb", 5))
        return 0;


    ProfScope prof(PROF_NODE_GET);

    installCrashFilter();
    /* getters are too hot to trace */
    WebCore::Node* n = nodeOf(self);
    AntbValue* v = NULL;

    if (!n)
        return 0;

    /* document.defaultView: React Router's data router reads
     * `document.defaultView.history` before anything else, and the property
     * set on the shared document object does not reach wrappers handed out
     * for the same Document, so answer it here for every node. */
    if (!strcmp(name, "defaultView")) {
        AntbValue* global = antb_global(engine);

        if (global) {
            antb_value_assign(out, global);
            antb_value_release(global);
        }
        return 1;
    }

    /* node.constructor: React's input value tracker runs
     * Object.getOwnPropertyDescriptor(node.constructor.prototype, 'value')
     * before it installs itself; an undefined constructor throws inside the
     * tracker and takes the whole render down. Answer with the DOM interface
     * for the tag (the constructors exist on the global from the engine's
     * interface objects, and a plain prototype is all the tracker needs). */
    if (!strcmp(name, "constructor")) {
        const char* ctorName = "HTMLElement";

        if (n->isDocumentNode()) {
            ctorName = "HTMLDocument";
        } else if (n->isElementNode()) {
            WebCore::Element* el = static_cast<WebCore::Element*>(n);

            if (el->hasTagName(WebCore::HTMLNames::inputTag))         ctorName = "HTMLInputElement";
            else if (el->hasTagName(WebCore::HTMLNames::textareaTag))  ctorName = "HTMLTextAreaElement";
            else if (el->hasTagName(WebCore::HTMLNames::selectTag))    ctorName = "HTMLSelectElement";
            else if (el->hasTagName(WebCore::HTMLNames::formTag))      ctorName = "HTMLFormElement";
            else if (el->hasTagName(WebCore::HTMLNames::buttonTag))    ctorName = "HTMLButtonElement";
            else if (el->hasTagName(WebCore::HTMLNames::canvasTag))    ctorName = "HTMLCanvasElement";
            else if (el->hasTagName(WebCore::HTMLNames::imgTag))       ctorName = "HTMLImageElement";
            else if (el->hasTagName(WebCore::HTMLNames::aTag))         ctorName = "HTMLAnchorElement";
        }

        {
            AntbValue* global = antb_global(engine);

            if (global) {
                AntbValue* ctor = antb_get_prop(engine, global, ctorName);

                if (!ctor && strcmp(ctorName, "HTMLElement"))
                    ctor = antb_get_prop(engine, global, "HTMLElement");

                if (!ctor)
                    ctor = antb_get_prop(engine, global, "Node");

                antb_value_release(global);

                if (ctor) {
                    antb_value_assign(out, ctor);
                    antb_value_release(ctor);
                    return 1;
                }
            }
        }

        return 0;
    }

    /* Object.prototype helpers: the catch-all getter answers every name, so the
     * wrappers never reach their prototype chain and `node.hasOwnProperty(...)`
     * came back undefined (React's value tracker calls it on the node). Hand
     * back the real functions; `this` is still the wrapper at call time. */
    if (!strcmp(name, "hasOwnProperty") || !strcmp(name, "isPrototypeOf")
            || !strcmp(name, "propertyIsEnumerable")) {
        AntbValue* global = antb_global(engine);
        AntbValue* fn = NULL;

        if (global) {
            AntbValue* objectCtor = antb_get_prop(engine, global, "Object");

            if (objectCtor) {
                AntbValue* proto = antb_get_prop(engine, objectCtor, "prototype");

                if (proto) {
                    fn = antb_get_prop(engine, proto, name);
                    antb_value_release(proto);
                }
                antb_value_release(objectCtor);
            }
            antb_value_release(global);
        }

        if (fn) {
            antb_value_assign(out, fn);
            antb_value_release(fn);
            return 1;
        }
        return 0;
    }

    if (!strcmp(name, "textContent")) {
        WebCore::String s = n->textContent();
        v = antb_make_string(engine, s.utf8().data());
    } else if (!strcmp(name, "nodeName") || !strcmp(name, "tagName")) {
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

        /* Hand out the shared document object: it already carries the
         * document-level methods, and re-wrapping the same Document would
         * create a second wrapper for one native node. */
        if (n->ownerDocument() && n->ownerDocument() == g_document && g_docObj) {
            antb_value_assign(out, g_docObj);
            return 1;
        }
        if (n->ownerDocument()) {
            v = wrapNode(engine, n->ownerDocument());
        } else {
            return 1;
        }
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
    } else if (!strcmp(name, "viewBox")) {
        AntbValue* global = antb_global(engine);
        AntbValue* fn = global ? antb_get_prop(engine, global, "__agbViewBox") : NULL;

        antb_value_release(global);

        if (!fn) {
            antb_value_assign(out, antb_make_number(engine, 0));
            return 1;
        }

        {
            AntbValue* argv[1];
            AntbValue* result;

            argv[0] = self;
            result = antb_call(engine, fn, NULL, argv, 1, NULL);
            antb_value_release(fn);

            if (!result) {
                antb_value_assign(out, antb_make_number(engine, 0));
                return 1;
            }

            antb_value_assign(out, result);
            antb_value_release(result);
        }
        return 1;
    } else if (!strcmp(name, "dataset")) {
        AntbValue* existing = antb_get_prop(engine, self, "__agbDataset");

        if (existing && !antb_is_number(existing)) {
            antb_value_assign(out, existing);
            antb_value_release(existing);
            return 1;
        }
        antb_value_release(existing);

        {
            AntbValue* dataset = antb_make_object(engine);

            if (!dataset)
                return 0;

            antb_set_prop(engine, dataset, "__agbElement", self);
            antb_define_property_rw(engine, dataset, "dataset", datasetGetter, datasetSetter, NULL);
            antb_set_prop(engine, self, "__agbDataset", dataset);

            antb_value_assign(out, dataset);
            antb_value_release(dataset);
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

            /* Back reference plus the single dispatcher; see styleGetter(). */
            antb_set_prop(engine, style, "__agbElement", self);
            antb_define_property_rw(engine, style, "style", styleGetter, styleSetter, NULL);

            antb_set_prop(engine, self, "__agbStyle", style);
            antb_value_assign(out, style);
            antb_value_release(style);
        }
        return 1;
    } else if (!strcmp(name, "classList")) {
        return nodeClassList(engine, self, NULL, 0, out, NULL);
    } else if (!strcmp(name, "offsetWidth") || !strcmp(name, "offsetHeight")
            || !strcmp(name, "offsetTop") || !strcmp(name, "offsetLeft")
            || !strcmp(name, "clientWidth") || !strcmp(name, "clientHeight")
            || !strcmp(name, "clientTop") || !strcmp(name, "clientLeft")
            || !strcmp(name, "scrollWidth") || !strcmp(name, "scrollHeight")
            || !strcmp(name, "scrollTop") || !strcmp(name, "scrollLeft")) {
        int measured = 0;

        if (n->isElementNode()) {
            WebCore::Element* el = static_cast<WebCore::Element*>(n);

            if (!strcmp(name, "offsetWidth")) measured = el->offsetWidth();
            else if (!strcmp(name, "offsetHeight")) measured = el->offsetHeight();
            else if (!strcmp(name, "offsetTop")) measured = el->offsetTop();
            else if (!strcmp(name, "offsetLeft")) measured = el->offsetLeft();
            else if (!strcmp(name, "clientWidth")) measured = el->clientWidth();
            else if (!strcmp(name, "clientHeight")) measured = el->clientHeight();
            else if (!strcmp(name, "clientTop")) measured = el->clientTop();
            else if (!strcmp(name, "clientLeft")) measured = el->clientLeft();
            else if (!strcmp(name, "scrollWidth")) measured = el->scrollWidth();
            else if (!strcmp(name, "scrollHeight")) measured = el->scrollHeight();
            else if (!strcmp(name, "scrollTop")) measured = el->scrollTop();
            else if (!strcmp(name, "scrollLeft")) measured = el->scrollLeft();
        }

        v = antb_make_number(engine, (double)measured);
    } else if (!strcmp(name, "id") || !strcmp(name, "className") || !strcmp(name, "href")
               || !strcmp(name, "src") || !strcmp(name, "srcset") || !strcmp(name, "alt")
               || !strcmp(name, "placeholder") || !strcmp(name, "poster")
               || !strcmp(name, "title") || !strcmp(name, "name") || !strcmp(name, "type")) {
        if (!n->isElementNode())
            return 0;
        {
            WebCore::String attrName = strcmp(name, "className") == 0
                ? WebCore::String("class") : WebCore::String(name);
            WebCore::String s = static_cast<WebCore::Element*>(n)->getAttribute(attrName);
            v = antb_make_string(engine, s.utf8().data());
        }
    }

    /* Unknown name (or a branch that produced nothing): fall through to the
     * expando fields written by nodeSetter (el._vnode, el._assign,
     * input._wrapperState, jQuery data...). These used to be stored on the
     * wrapper itself, which this hook cannot read back - every write built a
     * fresh side object and dropped its fields, so React lost
     * input._wrapperState and reported "reading 'initialChecked'". */
    if (!v) {
        AntbValue* expando = expandoFor(engine, n, false);

        if (expando) {
            AntbValue* field = antb_get_prop(engine, expando, name);

            if (field) {
                antb_value_assign(out, field);
                antb_value_release(field);
                return 1;
            }
        }

        return 0;
    }

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int nodeSetter(AntbEngine* engine, AntbValue* self, const char* name, const AntbValue* value, void*)
{
    /* Internal bookkeeping names must not go back through the bridge: reading
     * __agbExpando from the setter's own object would re-enter this getter and
     * recurse until the stack overflows. */
    if (name && name[0] == '_' && !strncmp(name, "__agb", 5))
        return 0;


    ProfScope prof(PROF_NODE_SET);

    installCrashFilter();
    traceOp("set %s", name ? name : "?");
    WebCore::Node* n = nodeOf(self);

    (void)engine;

    if (!n || !value)
        return 0;

    if (!strcmp(name, "scrollTop") && n->isElementNode()) {
        static_cast<WebCore::Element*>(n)->setScrollTop((int)antb_to_number(value));
        return 1;
    }

    if (!strcmp(name, "scrollLeft") && n->isElementNode()) {
        static_cast<WebCore::Element*>(n)->setScrollLeft((int)antb_to_number(value));
        return 1;
    }

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
    /* Attribute reflection for the common reflected properties: `img.src = ...`
     * and friends are plain attribute writes in the DOM. Kept to a whitelist so
     * that arbitrary JS fields (handlers, expando data) never become attributes. */
    if (n->isElementNode() && (antb_is_string(value) || antb_is_number(value))) {
        /* Deliberately narrow: only properties that JS builds at runtime and that
         * the shim has no dedicated setter for. Everything else (id, class, href,
         * value, onclick, expando data...) must stay a plain JS property, or
         * libraries like jQuery break en masse. */
        static const char* reflected[] = {
            "src", "srcset", "alt", "placeholder", "poster", "width", "height",
            "loading", "decoding", "crossorigin"
        };
        bool allow = false;

        for (unsigned i = 0; i < sizeof(reflected) / sizeof(reflected[0]); ++i) {
            if (!strcmp(name, reflected[i])) {
                allow = true;
                break;
            }
        }

        if (allow) {
            char* text = antb_to_utf8(value);

            if (text) {
                WebCore::ExceptionCode ec = 0;
                WebCore::String attrName = !strcmp(name, "className") ? WebCore::String("class")
                                                                     : WebCore::String(name);
                static_cast<WebCore::Element*>(n)->setAttribute(attrName,
                                                             WebCore::String::fromUTF8(text), ec);
                antb_free(text);
                return 1;
            }
        }
    }

    /* Expando fields: l._assign = fn, l.__vnode = ..., jQuery data...
     * are plain JS properties on the element object. The node wrapper is not a
     * normal object (it has no shape), so writing them on it makes Ant throw
     * "invalid object" - Vue's v-model directives do exactly this on every
     * render. Keep them on a side object. */
    /* The side object is owned by C++ (see expandoFor): reading one of these
     * fields back goes through the generic property hook, which cannot read a
     * `__agbExpando` field stored on the wrapper itself. */
    {
        AntbValue* expando = expandoFor(g_engine, n, true);

        if (expando)
            antb_set_prop(g_engine, expando, name, value);
    }

    return 1;
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
        /* Only functions are callable later: pages happily pass null or objects
         * as the listener, and dispatching those crashed the process. */
        l.fn = antb_is_function(args[1]) ? antb_retain(args[1]) : NULL;

        /* options.once: remove the listener after its first dispatch. The third
         * argument is either a boolean (capture, ignored here) or an object. */
        l.once = false;
        if (argc > 2 && args[2]) {
            AntbValue* once = antb_get_prop(engine, args[2], "once");

            if (once) {
                l.once = antb_to_boolean(once) != 0;
                antb_value_release(once);
            }
        }
        if (l.fn) {
            g_listeners->push_back(l);
            if (getenv("AGB_EVT_DEBUG"))
                fprintf(stderr, "[evt] add %s node=%p listeners=%d\n", type, (void*)n, (int)g_listeners->size());
        }
    }

    antb_free(type);
    return 1;
}

/* removeEventListener(type, fn): without this, `window.onload = f` assignments
 * (which the prelude implements as remove-then-add) piled up listeners and ran
 * the page's load handler once per assignment - css3test ran its whole suite
 * three times that way. */
int nodeRemoveEventListener(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                            AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* type = (argc > 0) ? antb_to_utf8(args[0]) : NULL;

    (void)engine; (void)out;

    if (!n || !type || argc < 2) {
        antb_free(type);
        return 0;
    }

    if (g_listeners) {
        for (size_t i = 0; i < g_listeners->size(); ) {
            Listener& l = (*g_listeners)[i];

            if (l.node == n && l.type == type && l.fn && antb_value_same(l.fn, args[1])) {
                antb_value_release(l.fn);
                g_listeners->erase(g_listeners->begin() + i);
                continue;
            }
            ++i;
        }
    }

    antb_free(type);
    return 1;
}

/* The DOM passes an Event object: use its `type`. Older page code sometimes
 * passes a plain string, which is still accepted. */
static char* eventTypeOf(AntbEngine* engine, AntbValue* arg)
{
    if (!arg)
        return NULL;

    {
        AntbValue* t = antb_get_prop(engine, arg, "type");

        if (t) {
            char* text = (antb_is_string(t) || antb_is_number(t)) ? antb_to_utf8(t) : NULL;
            antb_value_release(t);

            if (text && text[0])
                return text;
            antb_free(text);
        }
    }

    return antb_to_utf8(arg);
}

int nodeDispatchEvent(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* type = (argc > 0) ? eventTypeOf(engine, args[0]) : NULL;
    std::vector<AntbValue*> snapshot;

    (void)out;

    if (!n || !type) {
        antb_free(type);
        return 0;
    }

    if (getenv("AGB_EVT_DEBUG"))
        fprintf(stderr, "[evt] JS dispatchEvent(%s) on node=%p\n", type, (void*)n);

    if (g_listeners) {
        for (size_t i = 0; i < g_listeners->size(); ++i) {
            Listener& l = (*g_listeners)[i];

            if (l.node == n && l.type == type && l.fn)
                snapshot.push_back(antb_retain(l.fn));
        }
    }

    /* Dispatch the snapshot, and hand the caller's own Event object through. */
    for (size_t i = 0; i < snapshot.size(); ++i) {
        AntbValue* argv[1];

        argv[0] = (argc > 0) ? args[0] : NULL;
        antb_call(engine, snapshot[i], self, argv, argv[0] ? 1 : 0, NULL);
    }

    /* once: drop the listeners this dispatch just called. Identity is
     * checked on the JS value, because the snapshot holds retained wrappers. */
    for (size_t i = 0; g_listeners && i < g_listeners->size(); ) {
        Listener& l = (*g_listeners)[i];
        bool called = false;

        if (l.once && l.node == n && l.type == type && l.fn) {
            for (size_t k = 0; k < snapshot.size(); ++k) {
                if (antb_value_same(l.fn, snapshot[k])) { called = true; break; }
            }
        }

        if (called) {
            antb_value_release(l.fn);
            g_listeners->erase(g_listeners->begin() + i);
        } else {
            ++i;
        }
    }

    for (size_t i = 0; i < snapshot.size(); ++i)
        antb_value_release(snapshot[i]);

    antb_free(type);
    return 1;
}

int docCreateElement(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    ProfScope prof(PROF_CREATE_ELEMENT);
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
/* document.createElementNS(ns, name): Vue renders every <svg>/<math> element
 * through it, and pages use it for namespaced markup. */
int docCreateElementNS(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    char* ns;
    char* name;
    RefPtr<WebCore::Element> node;
    WebCore::ExceptionCode ec = 0;

    if (!documentOf(self) || argc < 2)
        return 1;

    ns = antb_to_utf8(args[0]);
    name = antb_to_utf8(args[1]);

    node = documentOf(self)->createElementNS(WebCore::String::fromUTF8(ns ? ns : ""),
                                             WebCore::String::fromUTF8(name ? name : ""), ec);
    antb_free(ns);
    antb_free(name);

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

/* document.createComment(text): Vue renders every v-if placeholder with it, and
 * pages use it for comment nodes they hand to insertBefore(). */
int docCreateComment(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    char* text;
    RefPtr<WebCore::Comment> node;

    if (!documentOf(self) || argc < 1)
        return 1;

    text = antb_to_utf8(args[0]);
    if (!text)
        return 1;

    node = documentOf(self)->createComment(WebCore::String::fromUTF8(text));
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
    ProfScope prof(PROF_APPEND);
    WebCore::Node* parent = nodeOf(self);
    WebCore::Node* child  = (argc > 0) ? nodeOf(args[0]) : 0;
    WebCore::ExceptionCode ec = 0;

    if (!parent || !child)
        return 1;

    {
        bool debugDom = getenv("AGB_DOM_DEBUG") != NULL;
        unsigned long began = debugDom ? GetTickCount() : 0;

        parent->appendChild(child, ec);

        if (debugDom) {
            static int appends = 0;
            static unsigned long insideTotal = 0;

            insideTotal += GetTickCount() - began;
            ++appends;

            if (appends % 2000 == 0)
                fprintf(stderr, "[dom] appends=%d inside=%u ms (avg %u us)\n",
                        appends, (unsigned)insideTotal,
                        (unsigned)(insideTotal * 1000 / appends));
        }
    }

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

/* HTMLElement.click(): dispatches a click event, which runs the default
 * action - that is what activates a link (and what submits a button). Pages
 * call it directly all the time, and without it "open in a new tab" links
 * could not be triggered from a script at all. */
int nodeClick(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
              AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);

    (void)engine; (void)args; (void)argc; (void)out;

    if (n && n->isHTMLElement())
        static_cast<WebCore::HTMLElement*>(n)->click();
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

/* Synchronous binary GET: base64 string, or undefined on network failure. */
int globalHttpGetBin(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
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
            return 1;

        {
            std::string encoded = base64Encode(data.data(), data.size());
            AntbValue* v = antb_make_string(engine, encoded.c_str());

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

/* getBoundingClientRect(): absolute box from the render tree, in CSS pixels. */
int nodeGetBoundingClientRect(AntbEngine* engine, AntbValue* self, AntbValue**, int,
                             AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    WebCore::IntRect rect(0, 0, 0, 0);

    if (!n || !n->renderer())
        return 1;   /* detached / display:none → zero rect */

    rect = n->renderer()->absoluteBoundingBoxRect();

    {
        struct { const char* name; double value; } fields[] = {
            { "left",   (double)rect.x() },
            { "top",    (double)rect.y() },
            { "width",  (double)rect.width() },
            { "height", (double)rect.height() },
            { "right",  (double)(rect.x() + rect.width()) },
            { "bottom", (double)(rect.y() + rect.height()) },
            { "x",      (double)rect.x() },
            { "y",      (double)rect.y() },
        };
        AntbValue* obj = antb_make_object(engine);

        if (!obj)
            return 0;

        for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
            AntbValue* v = antb_make_number(engine, fields[i].value);
            if (v) {
                antb_set_prop(engine, obj, fields[i].name, v);
                antb_value_release(v);
            }
        }

        antb_value_assign(out, obj);
        antb_value_release(obj);
    }
    return 1;
}
/* classList is implemented in JS (see the prelude): one C entry point keeps the
 * bridge's fixed method table from filling up with per-element methods. */
/* getBoundingClientRect(): absolute box from the render tree, in CSS pixels. */
/* classList is implemented in JS (see the prelude): a single C entry point keeps
 * the bridge's fixed method table from filling up with per-element methods. */
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


static WebCore::String tokensWithout(const WebCore::String& tokens, const WebCore::String& name)
{
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
    return rebuilt;
}
/* JS-visible context switching, used by the timer wrapper in the prelude and by
 * async XHR delivery. */
static AntRuntime::OpenWindowHook g_openWindowHook = NULL;
static AntRuntime::PostMessageHook g_postMessageHook = NULL;
static AntRuntime::DialogHook g_dialogHook = NULL;

static AntbValue* makeStringResult(AntbEngine* engine, AntbValue* out, const char* text);
static AntbValue* makeNumberResult(AntbEngine* engine, AntbValue* out, double number);

int globalDialog(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                 AntbValue* out, void*)
{
    int type = (argc > 0) ? (int)antb_to_number(args[0]) : 0;
    char* message = (argc > 1) ? antb_to_utf8(args[1]) : NULL;
    char* defaultValue = (argc > 2) ? antb_to_utf8(args[2]) : NULL;
    char result[1024];
    int accepted = 1;

    result[0] = 0;

    if (g_dialogHook)
        accepted = g_dialogHook(type, message ? message : "",
                                defaultValue ? defaultValue : "",
                                result, sizeof(result));
    else if (type == 1)
        accepted = 1;       /* headless confirm: let the page continue */
    else if (type == 2)
        accepted = 0;       /* headless prompt: cancel */

    antb_free(message);
    antb_free(defaultValue);

    if (type == 2) {
        if (!accepted) {
            AntbValue* v = antb_make_string(engine, "");

            if (!v)
                return 0;

            antb_value_assign(out, v);
            antb_value_release(v);
            return 1;
        }

        return makeStringResult(engine, out, result) ? 1 : 0;
    }

    return makeNumberResult(engine, out, accepted ? 1.0 : 0.0) ? 1 : 0;
}

int globalPostMessage(AntbEngine*, AntbValue*, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    char* message = (argc > 0) ? antb_to_utf8(args[0]) : NULL;

    if (g_postMessageHook)
        g_postMessageHook(message ? message : "");

    antb_free(message);
    (void)out;
    return 1;
}

static AntbValue* makeStringResult(AntbEngine* engine, AntbValue* out, const char* text)
{
    AntbValue* v = antb_make_string(engine, text ? text : "");

    if (!v)
        return NULL;

    antb_value_assign(out, v);
    antb_value_release(v);
    return v;
}

static AntbValue* makeNumberResult(AntbEngine* engine, AntbValue* out, double number)
{
    AntbValue* v = antb_make_number(engine, number);

    if (!v)
        return NULL;

    antb_value_assign(out, v);
    antb_value_release(v);
    return v;
}

int globalStorageGet(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char* key = (argc > 0) ? antb_to_utf8(args[0]) : NULL;
    std::string value;

    if (key) {
        storageGet(key, value);
        antb_free(key);
    }

    return makeStringResult(engine, out, value.c_str()) ? 1 : 0;
}

int globalStorageHas(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char* key = (argc > 0) ? antb_to_utf8(args[0]) : NULL;
    std::string value;
    bool found = false;

    if (key) {
        found = storageGet(key, value) != NULL;
        antb_free(key);
    }

    return makeNumberResult(engine, out, found ? 1.0 : 0.0) ? 1 : 0;
}

int globalStorageSet(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char* key = (argc > 0) ? antb_to_utf8(args[0]) : NULL;
    char* value = (argc > 1) ? antb_to_utf8(args[1]) : NULL;

    if (key)
        storageSet(key, value ? value : "");

    antb_free(key);
    antb_free(value);

    return makeNumberResult(engine, out, 1.0) ? 1 : 0;
}

int globalStorageDel(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char* key = (argc > 0) ? antb_to_utf8(args[0]) : NULL;

    if (key)
        storageDel(key);

    antb_free(key);

    return makeNumberResult(engine, out, 1.0) ? 1 : 0;
}

int globalStorageClear(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    char* origin = (argc > 0) ? antb_to_utf8(args[0]) : NULL;
    std::string prefix = origin ? std::string(origin) + "|" : std::string();
    std::vector<std::pair<std::string, std::string> >& entries = storage();
    size_t i = 0;

    while (i < entries.size()) {
        if (!prefix.empty() && entries[i].first.compare(0, prefix.size(), prefix) == 0)
            entries.erase(entries.begin() + i);
        else
            ++i;
    }

    saveStorage();
    antb_free(origin);

    return makeNumberResult(engine, out, 1.0) ? 1 : 0;
}

int globalStorageKey(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char* origin = (argc > 0) ? antb_to_utf8(args[0]) : NULL;
    int index = (argc > 1) ? (int)antb_to_number(args[1]) : -1;
    std::string prefix = origin ? std::string(origin) + "|" : std::string();
    std::vector<std::pair<std::string, std::string> >& entries = storage();
    int seen = 0;
    const char* found = NULL;
    std::string name;

    for (size_t i = 0; i < entries.size() && index >= 0; ++i) {
        if (prefix.empty() || entries[i].first.compare(0, prefix.size(), prefix) != 0)
            continue;

        if (seen == index) {
            name = entries[i].first.substr(prefix.size());
            found = name.c_str();
            break;
        }

        ++seen;
    }

    antb_free(origin);

    if (!found)
        return makeStringResult(engine, out, "") ? 1 : 0;

    return makeStringResult(engine, out, found) ? 1 : 0;
}

int globalStorageCount(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    char* origin = (argc > 0) ? antb_to_utf8(args[0]) : NULL;
    std::string prefix = origin ? std::string(origin) + "|" : std::string();
    std::vector<std::pair<std::string, std::string> >& entries = storage();
    int count = 0;

    for (size_t i = 0; i < entries.size(); ++i) {
        if (prefix.empty() || entries[i].first.compare(0, prefix.size(), prefix) == 0)
            ++count;
    }

    antb_free(origin);
    return makeNumberResult(engine, out, (double)count) ? 1 : 0;
}


/* '::before' / ':before' / 'before' -> PseudoId (NOPSEUDO when unknown). */
static WebCore::RenderStyle::PseudoId pseudoIdFromName(const char* text)
{
    std::string name(text ? text : "");

    while (!name.empty() && name[0] == ':')
        name.erase(0, 1);

    for (size_t i = 0; i < name.size(); ++i)
        if (name[i] >= 'A' && name[i] <= 'Z')
            name[i] = (char)(name[i] - 'A' + 'a');

    if (name == "before") return WebCore::RenderStyle::BEFORE;
    if (name == "after") return WebCore::RenderStyle::AFTER;
    if (name == "first-line") return WebCore::RenderStyle::FIRST_LINE;
    if (name == "first-letter") return WebCore::RenderStyle::FIRST_LETTER;
    if (name == "selection") return WebCore::RenderStyle::SELECTION;

    return WebCore::RenderStyle::NOPSEUDO;
}

/* getComputedStyle(el).getPropertyValue(name) -> computed value as a string.
 * Goes through WebCore's CSSComputedStyleDeclaration, so every property the
 * layout engine knows about comes back with a real value. A third argument is
 * the pseudo element ('::before'), whose style only exists on the renderer. */
int globalComputedStyle(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                        AntbValue* out, void*)
{
    ProfScope prof(PROF_COMPUTED);
    WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : NULL;
    char* property = (argc > 1) ? antb_to_utf8(args[1]) : NULL;
    char* pseudo = (argc > 2) ? antb_to_utf8(args[2]) : NULL;
    std::string value;
    static int calls = 0;
    static unsigned long totalUs = 0;
    unsigned long beganUs = 0;
    bool timed = getenv("AGB_CSS_DEBUG") != NULL;

    if (timed)
        beganUs = (unsigned long)GetTickCount();

    if (node && node->document() && property && property[0]) {
        /* Style-sheet changes are applied lazily; a computed-style read is the
         * one place that must observe them immediately (getComputedStyle() forces
         * a style recalc in every real browser too). */
        node->document()->updateStyleIfNeeded();

        WebCore::RenderStyle* pseudoStyle = NULL;

        if (pseudo && pseudo[0] && node->isElementNode()) {
            WebCore::RenderStyle::PseudoId id = pseudoIdFromName(pseudo);

            if (id != WebCore::RenderStyle::NOPSEUDO) {
                WebCore::RenderObject* renderer = node->renderer();

                if (renderer)
                    pseudoStyle = renderer->getPseudoStyle(id);
            }
        }

        RefPtr<WebCore::CSSComputedStyleDeclaration> declaration =
            pseudoStyle ? new WebCore::CSSComputedStyleDeclaration(node, pseudoStyle)
                        : new WebCore::CSSComputedStyleDeclaration(node);

        if (declaration) {
            /* CSSComputedStyleDeclaration hides the name-based overload with its
             * own getPropertyValue(int); call it through the base class. */
            WebCore::CSSStyleDeclaration* base = declaration.get();
            WebCore::String computed = base->getPropertyValue(WebCore::String(property));

            if (!computed.isNull())
                value = std::string(computed.utf8().data());
        }
    }

    if (timed) {
        totalUs += (unsigned long)GetTickCount() - beganUs;
        ++calls;

        if (calls % 200 == 0)
            fprintf(stderr, "[gcs] calls=%d total=%u ms\n", calls, (unsigned)totalUs);
    }

    antb_free(property);
    antb_free(pseudo);
    return makeStringResult(engine, out, value.c_str()) ? 1 : 0;
}

int globalOpenWindow(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char* url = (argc > 0) ? antb_to_utf8(args[0]) : NULL;
    int created = 0;
    AntbValue* v;
    std::string resolved;

    /* A relative URL is resolved against the document, like in a browser:
     * window.open('page.html') opens the page next to the current one, not
     * "http://page.html/". */
    if (url && url[0] && g_document) {
        WebCore::KURL base(g_document->URL());
        WebCore::KURL target(base, WebCore::DeprecatedString(url));

        if (target.isValid())
            resolved = target.url().utf8().data();
    }

    if (g_openWindowHook)
        created = g_openWindowHook(resolved.empty() ? (url ? url : "") : resolved.c_str());

    antb_free(url);

    v = antb_make_number(engine, created ? 1.0 : 0.0);

    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int globalFrameSlot(AntbEngine* engine, AntbValue*, AntbValue**, int, AntbValue* out, void*)
{
    AntbValue* v = antb_make_number(engine, (double)g_currentSlot);

    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int globalActivateSlot(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    int previous = g_currentSlot;

    if (argc > 0) {
        int slot = (int)antb_to_number(args[0]);

        if (slot >= 0 && slot < g_docSlotCount)
            activateSlot(slot);
    }

    {
        AntbValue* v = antb_make_number(engine, (double)previous);

        if (!v)
            return 0;

        antb_value_assign(out, v);
        antb_value_release(v);
    }
    return 1;
}

int globalClassOp(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                  AntbValue* out, void*)
{
    WebCore::Node* n = (argc > 0) ? nodeOf(args[0]) : 0;
    char* op;
    char* token;
    if (!n || !n->isElementNode() || argc < 3)
        return 1;
    op = antb_to_utf8(args[1]);
    token = antb_to_utf8(args[2]);
    if (!op || !token) {
        if (op) antb_free(op);
        if (token) antb_free(token);
        return 1;
    }
    {
        WebCore::Element* el = static_cast<WebCore::Element*>(n);
        WebCore::String name = WebCore::String::fromUTF8(token);
        WebCore::String tokens = el->getAttribute(WebCore::String("class"));
        bool contains = tokenListContains(tokens, name);
        if (!strcmp(op, "add")) {
            if (!contains && !name.isEmpty())
                writeClassTokens(el, tokens.isEmpty() ? name : tokens + " " + name);
            contains = true;
        } else if (!strcmp(op, "remove")) {
            writeClassTokens(el, tokensWithout(tokens, name));
            contains = false;
        } else if (!strcmp(op, "toggle")) {
            if (contains) {
                writeClassTokens(el, tokensWithout(tokens, name));
                contains = false;
            } else if (!name.isEmpty()) {
                writeClassTokens(el, tokens.isEmpty() ? name : tokens + " " + name);
                contains = true;
            }
        }
        if (!strcmp(op, "contains") || !strcmp(op, "toggle")) {
            AntbValue* v = antb_make_number(engine, contains ? 1.0 : 0.0);
            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
    }
    antb_free(op);
    antb_free(token);
    return 1;
}
/* classList: backed directly by the class attribute. The Element pointer is
 * handed to the callbacks as userdata and kept alive by a ref. */
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

/* Returns the per-element classList object built by the JS prelude: no bridge
 * method slots are consumed per element. */
static int nodeClassList(AntbEngine* engine, AntbValue* self, AntbValue**, int,
                         AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    AntbValue* global;
    AntbValue* factory;
    AntbValue* wrapped;
    AntbValue* args[1];
    AntbValue* result;
    if (!n || !n->isElementNode())
        return 1;
    global = antb_global(engine);
    if (!global)
        return 1;
    factory = antb_get_prop(engine, global, "__agbMakeClassList");
    antb_value_release(global);
    if (!factory)
        return 1;
    wrapped = wrapNode(engine, n);
    if (!wrapped) {
        antb_value_release(factory);
        return 1;
    }
    args[0] = wrapped;
    result = antb_call(engine, factory, NULL, args, 1, NULL);
    if (result) {
        antb_value_assign(out, result);
        antb_value_release(result);
    }
    antb_value_release(wrapped);
    antb_value_release(factory);
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

/* ------------------------------------------------------------------------- */
/* Canvas 2D                                                                  */
/*                                                                            */
/* The DOM side already has a complete implementation (HTMLCanvasElement     */
/* over the picasso rasteriser) - the ant bridge simply never exposed it,    */
/* because it has no per-tag classes: every element is one Node class.       */
/* getContext/toDataURL therefore live on the shared Node prototype and      */
/* check the tag inside the callback; the context itself gets its own class  */
/* (methods on the prototype, style properties through one dispatching       */
/* getter/setter pair per instance, the same scheme node/style use).         */
/* ------------------------------------------------------------------------- */

#define AGB_TAG_CANVAS2D 0x434E5632u   /* 'CNV2' */

static WebCore::CanvasRenderingContext2D* canvasCtxOf(AntbValue* self)
{
    return self ? (WebCore::CanvasRenderingContext2D*)antb_get_native(self, AGB_TAG_CANVAS2D) : 0;
}

static WebCore::HTMLCanvasElement* canvasElementOf(AntbValue* self)
{
    WebCore::Node* n = self ? nodeOf(self) : 0;

    if (!n || !n->isElementNode())
        return 0;

    {
        WebCore::Element* el = static_cast<WebCore::Element*>(n);

        if (!el->hasTagName(WebCore::HTMLNames::canvasTag))
            return 0;

        return static_cast<WebCore::HTMLCanvasElement*>(el);
    }
}

static double canvasArgNumber(AntbValue** args, int argc, int index)
{
    if (!args || index >= argc || !args[index])
        return 0;
    return antb_to_number(args[index]);
}

static std::string canvasArgString(AntbValue** args, int argc, int index)
{
    std::string out;

    if (!args || index >= argc || !args[index])
        return out;

    {
        char* text = antb_to_utf8(args[index]);

        if (text) {
            out = text;
            antb_free(text);
        }
    }

    return out;
}

static std::string canvasValueString(const AntbValue* value)
{
    std::string out;

    if (!value)
        return out;

    {
        char* text = antb_to_utf8(value);

        if (text) {
            out = text;
            antb_free(text);
        }
    }

    return out;
}

static int canvasReturnNumber(AntbEngine* engine, AntbValue* out, double v)
{
    AntbValue* value = antb_make_number(engine, v);

    if (!value)
        return 1;

    antb_value_assign(out, value);
    antb_value_release(value);
    return 1;
}

static int canvasReturnString(AntbEngine* engine, AntbValue* out, const char* text)
{
    AntbValue* value = antb_make_string(engine, text ? text : "");

    if (!value)
        return 1;

    antb_value_assign(out, value);
    antb_value_release(value);
    return 1;
}

static int canvasNoop(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*)
{
    return 1;
}

/* Shared body used for both gradient flavours: callers only ever call
 * addColorStop on the result, so the placeholder carries that method. */
static int canvasReturnGradient(AntbEngine* engine, AntbValue* out)
{
    AntbValue* obj = antb_make_object(engine);

    if (!obj)
        return 1;

    antb_define_method(engine, obj, "addColorStop", canvasNoop, NULL);
    antb_value_assign(out, obj);
    antb_value_release(obj);
    return 1;
}

static int canvasReturnImageData(AntbEngine* engine, AntbValue* out, int w, int h)
{
    AntbValue* obj = antb_make_object(engine);
    AntbValue* arr = antb_make_array(engine);

    if (!obj)
        return 1;

    if (arr) {
        antb_set_prop(engine, obj, "data", arr);
        antb_value_release(arr);
    }

    {
        AntbValue* width = antb_make_number(engine, (double)(w > 0 ? w : 0));
        if (width) {
            antb_set_prop(engine, obj, "width", width);
            antb_value_release(width);
        }
        {
            AntbValue* height = antb_make_number(engine, (double)(h > 0 ? h : 0));
            if (height) {
                antb_set_prop(engine, obj, "height", height);
                antb_value_release(height);
            }
        }
    }

    antb_value_assign(out, obj);
    antb_value_release(obj);
    return 1;
}

static int canvasCtxMethod(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void* user)
{
    WebCore::CanvasRenderingContext2D* ctx = canvasCtxOf(self);
    int op = (int)(intptr_t)user;
    int ec = 0;

    (void)out;

    if (!ctx)
        return 1;

    switch (op) {
    case CV_SAVE:
        ctx->save();
        break;
    case CV_RESTORE:
        ctx->restore();
        break;
    case CV_SCALE:
        ctx->scale((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1));
        break;
    case CV_ROTATE:
        ctx->rotate((float)canvasArgNumber(args, argc, 0));
        break;
    case CV_TRANSLATE:
        ctx->translate((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1));
        break;
    case CV_CLEAR_RECT:
        ctx->clearRect((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1),
                       (float)canvasArgNumber(args, argc, 2), (float)canvasArgNumber(args, argc, 3), ec);
        break;
    case CV_FILL_RECT:
        ctx->fillRect((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1),
                      (float)canvasArgNumber(args, argc, 2), (float)canvasArgNumber(args, argc, 3), ec);
        break;
    case CV_STROKE_RECT:
        ctx->strokeRect((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1),
                        (float)canvasArgNumber(args, argc, 2), (float)canvasArgNumber(args, argc, 3), ec);
        break;
    case CV_BEGIN_PATH:
        ctx->beginPath();
        break;
    case CV_CLOSE_PATH:
        ctx->closePath();
        break;
    case CV_MOVE_TO:
        ctx->moveTo((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1));
        break;
    case CV_LINE_TO:
        ctx->lineTo((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1));
        break;
    case CV_QUADRATIC:
        ctx->quadraticCurveTo((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1),
                              (float)canvasArgNumber(args, argc, 2), (float)canvasArgNumber(args, argc, 3));
        break;
    case CV_BEZIER:
        ctx->bezierCurveTo((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1),
                           (float)canvasArgNumber(args, argc, 2), (float)canvasArgNumber(args, argc, 3),
                           (float)canvasArgNumber(args, argc, 4), (float)canvasArgNumber(args, argc, 5));
        break;
    case CV_ARC:
        ctx->arc((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1),
                 (float)canvasArgNumber(args, argc, 2), (float)canvasArgNumber(args, argc, 3),
                 (float)canvasArgNumber(args, argc, 4),
                 argc > 5 ? (canvasArgNumber(args, argc, 5) != 0) : false, ec);
        break;
    case CV_ARC_TO:
        ctx->arcTo((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1),
                   (float)canvasArgNumber(args, argc, 2), (float)canvasArgNumber(args, argc, 3),
                   (float)canvasArgNumber(args, argc, 4), ec);
        break;
    case CV_RECT:
        ctx->rect((float)canvasArgNumber(args, argc, 0), (float)canvasArgNumber(args, argc, 1),
                  (float)canvasArgNumber(args, argc, 2), (float)canvasArgNumber(args, argc, 3), ec);
        break;
    case CV_FILL:
        ctx->fill();
        break;
    case CV_STROKE:
        ctx->stroke();
        break;
    case CV_CLIP:
        ctx->clip();
        break;
    case CV_DRAW_IMAGE:
        if (argc >= 1) {
            WebCore::Node* src = nodeOf(args[0]);

            if (src && src->isElementNode()) {
                WebCore::Element* el = static_cast<WebCore::Element*>(src);

                if (el->hasTagName(WebCore::HTMLNames::canvasTag)) {
                    WebCore::HTMLCanvasElement* source =
                        static_cast<WebCore::HTMLCanvasElement*>(el);

                    if (argc >= 5)
                        ctx->drawImage(source, (float)canvasArgNumber(args, argc, 1),
                                       (float)canvasArgNumber(args, argc, 2),
                                       (float)canvasArgNumber(args, argc, 3),
                                       (float)canvasArgNumber(args, argc, 4), ec);
                    else if (argc >= 3)
                        ctx->drawImage(source, (float)canvasArgNumber(args, argc, 1),
                                       (float)canvasArgNumber(args, argc, 2));
                }
            }
        }
        break;
    case CV_CREATE_LINEAR:
    case CV_CREATE_RADIAL:
        canvasReturnGradient(engine, out);
        break;
    case CV_MEASURE_TEXT:
        {
            AntbValue* obj = antb_make_object(engine);
            std::string text = canvasArgString(args, argc, 0);

            if (obj) {
                AntbValue* width = antb_make_number(engine, text.size() * 6.0);
                if (width) {
                    antb_set_prop(engine, obj, "width", width);
                    antb_value_release(width);
                }
                antb_value_assign(out, obj);
                antb_value_release(obj);
            }
        }
        break;
    case CV_GET_IMAGE_DATA:
        canvasReturnImageData(engine, out, (int)canvasArgNumber(args, argc, 2),
                              (int)canvasArgNumber(args, argc, 3));
        break;
    case CV_GET_LINE_DASH:
        {
            AntbValue* arr = antb_make_array(engine);
            if (arr) {
                antb_value_assign(out, arr);
                antb_value_release(arr);
            }
        }
        break;
    case CV_IS_POINT_IN_PATH:
        canvasReturnNumber(engine, out, 0);
        break;
    case CV_FILL_TEXT:
    case CV_STROKE_TEXT:
        /* This rasteriser generation has no glyph rendering yet. The methods
         * must exist: lottie's renderer probes canvas text support by calling
         * fillText, and an absent method kills the whole feature detect. */
        break;
    case CV_SET_TRANSFORM:
    case CV_SET_LINE_DASH:
    case CV_PUT_IMAGE_DATA:
    case CV_CREATE_PATTERN:
    default:
        /* Not implemented by this rasteriser generation: stay silent so
         * libraries that call them degrade instead of dying. */
        break;
    }

    return 1;
}

static int canvasCtxGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    WebCore::CanvasRenderingContext2D* ctx = canvasCtxOf(self);

    if (!ctx || !name)
        return 0;

    if (!strcmp(name, "canvas")) {
        if (ctx->canvas()) {
            AntbValue* v = wrapNode(engine, ctx->canvas());

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
        return 1;
    }

    if (!strcmp(name, "fillStyle") || !strcmp(name, "strokeStyle")) {
        WebCore::CanvasStyle* style = !strcmp(name, "fillStyle")
            ? ctx->fillStyle() : ctx->strokeStyle();

        return canvasReturnString(engine, out, style ? style->color().utf8().data() : "#000000");
    }

    if (!strcmp(name, "lineWidth"))     return canvasReturnNumber(engine, out, ctx->lineWidth());
    if (!strcmp(name, "lineCap"))       return canvasReturnString(engine, out, ctx->lineCap().utf8().data());
    if (!strcmp(name, "lineJoin"))      return canvasReturnString(engine, out, ctx->lineJoin().utf8().data());
    if (!strcmp(name, "miterLimit"))    return canvasReturnNumber(engine, out, ctx->miterLimit());
    if (!strcmp(name, "shadowOffsetX")) return canvasReturnNumber(engine, out, ctx->shadowOffsetX());
    if (!strcmp(name, "shadowOffsetY")) return canvasReturnNumber(engine, out, ctx->shadowOffsetY());
    if (!strcmp(name, "shadowBlur"))    return canvasReturnNumber(engine, out, ctx->shadowBlur());
    if (!strcmp(name, "shadowColor"))   return canvasReturnString(engine, out, ctx->shadowColor().utf8().data());
    if (!strcmp(name, "globalAlpha"))   return canvasReturnNumber(engine, out, ctx->globalAlpha());
    if (!strcmp(name, "globalCompositeOperation"))
        return canvasReturnString(engine, out, ctx->globalCompositeOperation().utf8().data());

    if (!strcmp(name, "isContextLost"))
        return canvasReturnNumber(engine, out, 0);

    return 0;
}

static int canvasCtxSetter(AntbEngine* engine, AntbValue* self, const char* name,
                           const AntbValue* value, void*)
{
    WebCore::CanvasRenderingContext2D* ctx = canvasCtxOf(self);

    (void)engine;

    if (!ctx || !name || !value)
        return 0;

    if (!strcmp(name, "fillStyle") || !strcmp(name, "strokeStyle")) {
        RefPtr<WebCore::CanvasStyle> style;

        if (antb_is_number(value))
            style = new WebCore::CanvasStyle((float)antb_to_number(value));
        else
            style = new WebCore::CanvasStyle(WebCore::String::fromUTF8(canvasValueString(value).c_str()));

        if (!strcmp(name, "fillStyle"))
            ctx->setFillStyle(style.release());
        else
            ctx->setStrokeStyle(style.release());
        return 1;
    }

    if (!strcmp(name, "lineWidth"))     { ctx->setLineWidth((float)antb_to_number(value)); return 1; }
    if (!strcmp(name, "miterLimit"))    { ctx->setMiterLimit((float)antb_to_number(value)); return 1; }
    if (!strcmp(name, "shadowOffsetX")) { ctx->setShadowOffsetX((float)antb_to_number(value)); return 1; }
    if (!strcmp(name, "shadowOffsetY")) { ctx->setShadowOffsetY((float)antb_to_number(value)); return 1; }
    if (!strcmp(name, "shadowBlur"))    { ctx->setShadowBlur((float)antb_to_number(value)); return 1; }
    if (!strcmp(name, "globalAlpha"))   { ctx->setGlobalAlpha((float)antb_to_number(value)); return 1; }
    if (!strcmp(name, "lineCap"))       { ctx->setLineCap(WebCore::String::fromUTF8(canvasValueString(value).c_str())); return 1; }
    if (!strcmp(name, "lineJoin"))      { ctx->setLineJoin(WebCore::String::fromUTF8(canvasValueString(value).c_str())); return 1; }
    if (!strcmp(name, "shadowColor"))   { ctx->setShadowColor(WebCore::String::fromUTF8(canvasValueString(value).c_str())); return 1; }
    if (!strcmp(name, "globalCompositeOperation")) {
        ctx->setGlobalCompositeOperation(WebCore::String::fromUTF8(canvasValueString(value).c_str()));
        return 1;
    }

    /* Let unknown names fall through to a plain JS expando on the wrapper. */
    return 0;
}

AntbValue* wrapCanvasCtx(AntbEngine* engine, WebCore::CanvasRenderingContext2D* ctx)
{
    AntbValue* v;

    if (!engine || !ctx || !g_canvasCtxClass)
        return NULL;

    v = antb_wrap_native_as(engine, g_canvasCtxClass, ctx, AGB_TAG_CANVAS2D);
    if (!v)
        return NULL;

    /* Idempotent: the wrapper cache may hand back an already prepared
     * instance, and re-installing the dispatch pair is harmless. */
    antb_define_property_rw(engine, v, "", canvasCtxGetter, canvasCtxSetter, NULL);

    return v;
}

int nodeGetContext(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                   AntbValue* out, void*)
{
    WebCore::HTMLCanvasElement* canvas = canvasElementOf(self);

    if (!canvas || argc < 1)
        return 1;   /* undefined: the caller sees no context, as with a null */

    {
        std::string type = canvasArgString(args, argc, 0);

        /* "webgl"/"webgl2"/"bitmaprenderer" are not implemented; feature
         * detects get the undefined (falsy) answer and take their fallback. */
        if (type != "2d")
            return 1;

        {
            WebCore::CanvasRenderingContext* generic =
                canvas->getContext(WebCore::String::fromUTF8(type.c_str()));

            if (!generic)
                return 1;

            {
                AntbValue* v = wrapCanvasCtx(engine,
                    static_cast<WebCore::CanvasRenderingContext2D*>(generic));

                if (v) {
                    antb_value_assign(out, v);
                    antb_value_release(v);
                }
            }
        }
    }

    return 1;
}

int nodeToDataURL(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                  AntbValue* out, void*)
{
    /* PNG encoding of the picasso surface is not wired up: answer with an
     * empty data URL so feature probes and image probes stay usable. */
    (void)self; (void)args; (void)argc;

    return canvasReturnString(engine, out, "data:,");
}

AntbValue* wrapNode(AntbEngine* engine, WebCore::Node* node)
{
    AntbValue* v;

    if (!engine || !node || !g_nodeClass)
        return NULL;

    /* The wrapper cache is keyed by the raw node pointer, so a node that gets
     * destroyed and a new one allocated at the same address would share a
     * wrapper (use-after-free). Keep every wrapped node alive instead - but
     * only for as long as the realm that wrapped it lives. */
    {
        unsigned long generation = currentGeneration();
        std::map<unsigned long, std::set<WebCore::Node*>*>& pins = pinnedNodes();
        std::set<WebCore::Node*>* pinned = pins[generation];

        if (!pinned) {
            pinned = new std::set<WebCore::Node*>();
            pins[generation] = pinned;
        }

        if (pinned->find(node) == pinned->end()) {
            node->ref();
            pinned->insert(node);
        }
    }

    v = antb_wrap_native_as_owner(engine, g_nodeClass, node, AGB_TAG_NODE, g_document);
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
    std::vector<AntbValue*> snapshot;

    if (!g_engine || !g_document || !type || !g_listeners)
        return;

    self = wrapNode(g_engine, g_document);
    if (!self)
        return;

    /* Snapshot the listeners first: a callback may add or remove listeners
     * while we dispatch, and walking the live vector would then skip entries
     * (erase shifts them) or run newly added ones inside the same dispatch.
     * Every fn is retained, so a removal during dispatch cannot free it. */
    for (size_t i = 0; i < g_listeners->size(); ++i) {
        Listener& l = (*g_listeners)[i];

        if (l.node == g_document && l.type == type && l.fn)
            snapshot.push_back(antb_retain(l.fn));
    }

    {
        AntbValue* evt = antb_make_object(g_engine);

        if (evt) {
            AntbValue* t = antb_make_string(g_engine, type);

            if (t) {
                antb_set_prop(g_engine, evt, "type", t);
                antb_value_release(t);
            }
        }

        for (size_t i = 0; i < snapshot.size(); ++i) {
            AntbValue* args[1];

            args[0] = evt;
            antb_call(g_engine, snapshot[i], self, args, evt ? 1 : 0, NULL);
        }

        if (getenv("AGB_EVT_DEBUG"))
            fprintf(stderr, "[evt] dispatch %s -> %d listener(s) (eval depth %d)\n",
                    type, (int)snapshot.size(), g_evalDepth);

        antb_value_release(evt);
    }
    /* once: drop the listeners this dispatch just called. Identity is
     * checked on the JS value, because the snapshot holds retained wrappers. */
    for (size_t i = 0; g_listeners && i < g_listeners->size(); ) {
        Listener& l = (*g_listeners)[i];
        bool called = false;

        if (l.once && l.node == g_document && l.type == type && l.fn) {
            for (size_t k = 0; k < snapshot.size(); ++k) {
                if (antb_value_same(l.fn, snapshot[k])) { called = true; break; }
            }
        }

        if (called) {
            antb_value_release(l.fn);
            g_listeners->erase(g_listeners->begin() + i);
        } else {
            ++i;
        }
    }

    for (size_t i = 0; i < snapshot.size(); ++i)
        antb_value_release(snapshot[i]);

    antb_value_release(self);
}

/* The document is about to be replaced: the outgoing page gets its teardown
 * events (pagehide, unload) while it is still the current document. Browsers
 * fire them on every navigation, reload included; without them a page cannot
 * clean up (timers, sockets, subscriptions) and the events simply never
 * happen in this engine. */
void documentWillUnload(void)
{
    if (!g_engine || !g_document)
        return;

    if (getenv("AGB_EVT_DEBUG"))
        fprintf(stderr, "[evt] documentWillUnload doc=%p listeners=%d\n",
                g_document, g_listeners ? (int)g_listeners->size() : 0);

    /* Browser order: pagehide, then unload. The listeners belong to the
     * document that is going away - g_document still points at it. */
    dispatchWindowEvent("pagehide");
    dispatchWindowEvent("unload");

    /* A teardown handler may have thrown: do not leave the exception
     * pending, it would poison the next evaluation. */
    if (char* pending = antb_take_pending_error(g_engine)) {
        fprintf(stderr, "[antjs] (unload) %s\n", pending);
        free(pending);
    }

    /* The old page's listeners close over its DOM and would fire against the
     * next document. */
    dropListenersForOldDocument();

    /* Isolation: the old realm goes away with the old document. Its globals,
     * its module registry and its timers must not be visible to the next
     * document. */
    stopRealm();
}

/* Builds the JS event object: the prelude adds the DOM methods (which only set
 * flags), then we copy the native fields across. */
static AntbValue* makeEventObject(const std::string& type, WebCore::Node* target, WebCore::Event* event)
{
    AntbValue* global;
    AntbValue* factory;
    AntbValue* wrappedTarget;
    AntbValue* args[2];
    AntbValue* evt = NULL;
    global = antb_global(g_engine);
    if (!global)
        return NULL;
    factory = antb_get_prop(g_engine, global, "__agbMakeEvent");
    antb_value_release(global);
    if (!factory)
        return NULL;
    wrappedTarget = wrapNode(g_engine, target);
    args[0] = antb_make_string(g_engine, type.c_str());
    args[1] = wrappedTarget;
    if (args[0])
        evt = antb_call(g_engine, factory, NULL, args, 2, NULL);
    if (args[0])
        antb_value_release(args[0]);
    if (wrappedTarget)
        antb_value_release(wrappedTarget);
    antb_value_release(factory);
    if (!evt)
        return NULL;
    if (event) {
        struct { const char* name; double value; } nums[] = {
            { "timeStamp", (double)event->timeStamp() },
        };
        unsigned i;
        for (i = 0; i < sizeof(nums) / sizeof(nums[0]); ++i) {
            AntbValue* v = antb_make_number(g_engine, nums[i].value);
            if (v) { antb_set_prop(g_engine, evt, nums[i].name, v); antb_value_release(v); }
        }
        /* MouseEvent / KeyboardEvent details, when present. */
        if (event->isMouseEvent()) {
            WebCore::MouseEvent* mouse = static_cast<WebCore::MouseEvent*>(event);
            struct { const char* name; double value; } mouseNums[] = {
                { "clientX", (double)mouse->clientX() },
                { "clientY", (double)mouse->clientY() },
                { "screenX", (double)mouse->screenX() },
                { "screenY", (double)mouse->screenY() },
                { "pageX", (double)mouse->pageX() },
                { "pageY", (double)mouse->pageY() },
                { "button", (double)mouse->button() },
                { "detail", (double)mouse->detail() },
                { "which", (double)mouse->button() },
            };
            for (i = 0; i < sizeof(mouseNums) / sizeof(mouseNums[0]); ++i) {
                AntbValue* v = antb_make_number(g_engine, mouseNums[i].value);
                if (v) { antb_set_prop(g_engine, evt, mouseNums[i].name, v); antb_value_release(v); }
            }
            {
                AntbValue* ctrl = antb_make_number(g_engine, mouse->ctrlKey() ? 1.0 : 0.0);
                AntbValue* shift = antb_make_number(g_engine, mouse->shiftKey() ? 1.0 : 0.0);
                AntbValue* alt = antb_make_number(g_engine, mouse->altKey() ? 1.0 : 0.0);
                AntbValue* meta = antb_make_number(g_engine, mouse->metaKey() ? 1.0 : 0.0);
                if (ctrl) { antb_set_prop(g_engine, evt, "ctrlKey", ctrl); antb_value_release(ctrl); }
                if (shift) { antb_set_prop(g_engine, evt, "shiftKey", shift); antb_value_release(shift); }
                if (alt) { antb_set_prop(g_engine, evt, "altKey", alt); antb_value_release(alt); }
                if (meta) { antb_set_prop(g_engine, evt, "metaKey", meta); antb_value_release(meta); }
            }
        } else if (event->isKeyboardEvent()) {
            WebCore::KeyboardEvent* key = static_cast<WebCore::KeyboardEvent*>(event);
            struct { const char* name; double value; } keyNums[] = {
                { "keyCode", (double)key->keyCode() },
                { "which", (double)key->keyCode() },
                { "charCode", (double)key->charCode() },
            };
            for (i = 0; i < sizeof(keyNums) / sizeof(keyNums[0]); ++i) {
                AntbValue* v = antb_make_number(g_engine, keyNums[i].value);
                if (v) { antb_set_prop(g_engine, evt, keyNums[i].name, v); antb_value_release(v); }
            }
        }
    }
    return evt;
}
/* Runs every listener registered for `node` / `type`. Returns the JS event
 * object so the caller can inspect the preventDefault flag. stringReturn
 * collects the last non-empty string a listener returned (beforeunload's
 * `return "..."` guard). */
static void runListenersFor(WebCore::Node* node, const std::string& type, AntbValue* evt,
                            AntbValue* currentTarget, bool& stopped, bool& stoppedImmediate,
                            std::string* stringReturn = NULL)
{
    AntbValue* value = NULL;
    if (!g_listeners)
        return;
    for (size_t i = 0; i < g_listeners->size(); ++i) {
        Listener& l = (*g_listeners)[i];
        if (l.node != node || l.type != type)
            continue;
        {
            AntbValue* args[1];
            args[0] = evt;
            value = antb_call(g_engine, l.fn, currentTarget, args, evt ? 1 : 0, NULL);
        }
        if (!value) {
            char* pending = antb_take_pending_error(g_engine);
            if (pending) {
                fprintf(stderr, "[antjs] (event %s) %s\n", type.c_str(), pending);
                free(pending);
            }
        } else {
            if (stringReturn && antb_is_string(value)) {
                char* s = antb_to_utf8(value);

                if (s && *s)
                    *stringReturn = s;
                antb_free(s);
            }
            antb_value_release(value);
        }
        if (evt) {
            AntbValue* flag = antb_get_prop(g_engine, evt, "__stoppedImmediate");
            if (flag) {
                stoppedImmediate = antb_to_boolean(flag);
                antb_value_release(flag);
            }
            flag = antb_get_prop(g_engine, evt, "__stopped");
            if (flag) {
                stopped = antb_to_boolean(flag);
                antb_value_release(flag);
            }
        }
        if (stoppedImmediate)
            return;
    }
}
void dispatchEvent(void* nodePtr, void* eventPtr)
{
    WebCore::Node*  node  = (WebCore::Node*)nodePtr;
    WebCore::Event* event = (WebCore::Event*)eventPtr;
    if (!g_engine || !node || !event || !g_listeners)
        return;
    {
        std::string type = event->type().domString().utf8().data();
        AntbValue* evt = makeEventObject(type, node, event);

        if (getenv("AGB_EVT_DEBUG") && (type == "load" || type == "DOMContentLoaded"))
            fprintf(stderr, "[evt] WebCore event %s on node=%p\n", type.c_str(), (void*)node);
        bool stopped = false;
        bool stoppedImmediate = false;
        if (!evt)
            return;
        /* Bubble only when the event says so. load does not bubble: without
         * this check every <script>/<img> load bubbled up to the document and
         * ran the page's window load handler once per async script. */
        bool bubbles = event->bubbles();

        for (WebCore::Node* current = node; current && !stopped;
             current = bubbles ? current->parentNode() : 0) {
            AntbValue* wrapper = wrapNode(g_engine, current);
            if (wrapper) {
                runListenersFor(current, type, evt, wrapper, stopped, stoppedImmediate);
                antb_value_release(wrapper);
            }
        }
        /* preventDefault in JS must reach the engine, or links still navigate. */
        {
            AntbValue* prevented = antb_get_prop(g_engine, evt, "__prevented");
            if (prevented) {
                if (antb_to_boolean(prevented))
                    event->preventDefault();
                antb_value_release(prevented);
            }
        }
        antb_value_release(evt);
    }
}

/* window.onbeforeunload: runs the page's guard listeners and reports whether
 * leaving needs confirming, with the message to show. WebCore's
 * Document::handleWindowEvent only walks m_windowEventListeners (the inline
 * onbeforeunload attribute), which the ant shim never fills - so before this
 * existed, every `window.onbeforeunload = fn` in a page was dead code and
 * navigations were never intercepted. */
bool beforeUnloadGuard(std::string& text)
{
    AntbValue* evt;
    AntbValue* target;
    bool stopped = false;
    bool stoppedImmediate = false;

    text.clear();

    if (!g_engine || !g_document || !g_listeners)
        return false;

    evt = makeEventObject("beforeunload", g_document, NULL);
    if (!evt)
        return false;

    target = wrapNode(g_engine, g_document);
    if (target) {
        runListenersFor(g_document, "beforeunload", evt, target, stopped, stoppedImmediate, &text);
        antb_value_release(target);
    }

    /* Old form: the handler assigns event.returnValue. */
    if (text.empty()) {
        AntbValue* rv = antb_get_prop(g_engine, evt, "returnValue");

        if (rv) {
            if (antb_is_string(rv)) {
                char* s = antb_to_utf8(rv);

                if (s && *s)
                    text = s;
                antb_free(s);
            }
            antb_value_release(rv);
        }
    }

    bool guard = !text.empty();

    if (!guard) {
        AntbValue* prevented = antb_get_prop(g_engine, evt, "__prevented");

        if (prevented) {
            guard = antb_to_boolean(prevented) != 0;
            antb_value_release(prevented);
        }
    }

    /* preventDefault() without a message still asks, with the generic text
     * browsers fall back to. */
    if (guard && text.empty())
        text = "This page is asking you to confirm that you want to leave.";

    antb_value_release(evt);
    return guard;
}

} /* namespace AntRuntime */

namespace AntRuntime {

void setOpenWindowHook(OpenWindowHook hook)
{
    g_openWindowHook = hook;
}

void setPostMessageHook(PostMessageHook hook)
{
    g_postMessageHook = hook;
}

void setDialogHook(DialogHook hook)
{
    g_dialogHook = hook;
}

} /* namespace AntRuntime */



















