/*
 * AntRuntime.cpp - C++ facade over the Ant bridge (antjsb.dll).
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

#include "AgbLog.h"
/* cb_clipboard_set_data: the host's Win32 clipboard writer, used by
 * __agbClipboardSetText (the engine has no document.execCommand('copy')). */
#include "../../webview/WebCallback.h"
#include "DownloadEvents.h"
#include "DownloadManager.h"
#include "AgbGapJsdom.h"

namespace WebCore { bool prefersDarkColorScheme(); }
#include "AgbSelector.h"
#include "AgbTextCodec.h"
#include "AntBridge.h"
#include "RealmDocument.h"
#include "RealmTags.h"
#include "../../webcore/platform/network/native/NetworkSession.h"
#include "../platform/network/native/NetLog.h"

#include "Document.h"
#include "DOMImplementation.h"
#include "HTMLDocument.h"
#include "Element.h"
#include "RenderObject.h"
#include "Node.h"
#include "CSSComputedStyleDeclaration.h"
#include "CSSParser.h"
#include "CSSMutableStyleDeclaration.h"
#include "CSSStyleDeclaration.h"
#include "StylePropertyNames.h"
#include "CSSStyleSheet.h"
#include "CSSRule.h"
#include "CSSRuleList.h"
#include "CSSStyleRule.h"
#include "CSSMediaRule.h"
#include "CSSImportRule.h"
#include "CSSCharsetRule.h"
#include "CSSFontFaceRule.h"
#include "CSSPageRule.h"
#include "MediaList.h"
#include "StyleSheetList.h"
#include "StyledElement.h"
#include "PlatformString.h"
#include "Event.h"
#include "MouseEvent.h"
#include "KeyboardEvent.h"
#include "Frame.h"
#include "FrameView.h"
#include "FrameTree.h"
#include "CharacterData.h"
#include "NamedAttrMap.h"
#include "Attribute.h"
#include "DocumentFragment.h"
#include "editing/markup.h"
#include "HTMLImageElement.h"
#include "HTMLFormElement.h"
#include "HTMLCollection.h"
#include "HTMLFrameOwnerElement.h"
#include "FrameLoader.h"
#include "Page.h"
#include "KURL.h"
#include "Text.h"
#include "Comment.h"
#include "HTMLInputElement.h"
#include "HTMLSelectElement.h"
#include "HTMLOptionElement.h"
#include "HTMLOptGroupElement.h"
#include "BeforeTextInsertedEvent.h"
#include "PlatformKeyboardEvent.h"
#include "TextEvent.h"
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
#include "HTMLAudioElement.h"
#include "HTMLMediaElement.h"
#include "HTMLSourceElement.h"
#include "HTMLTrackElement.h"
#include "HTMLVideoElement.h"
#include "TextTrack.h"
#include "MediaDecoder.h"

#include <stdio.h>
#include <string>
#include <vector>
#include <set>

namespace AntRuntime {
void dispatchWindowEventState(const char* type, AntbValue* state);
AntbValue* documentObjectFor(AntbEngine*, WebCore::Document*);
AntbValue* windowObjectFor(AntbEngine*, WebCore::Document*);
void installFrameGlobals(AntbEngine*, WebCore::Document*);
AntbValue* windowForDocument(AntbEngine*, WebCore::Document*);
AntbValue* realmWindowFor(AntbEngine*, WebCore::Document*);
int windowGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int windowSetter(AntbEngine*, AntbValue*, const char*, const AntbValue*, void*);
}

namespace {
static std::string cookieHeaderForDocument(WebCore::Document* document);
static void setCookieForDocument(WebCore::Document* document, const std::string& text);

/* Defined with the other internal-page bindings; bindDocument installs it as
 * the download manager's change hook. */
static void downloadEventSink(int id, int state);
static bool isInternalDocument();
int globalDownloadList(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalDownloadCancel(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalDownloadOpen(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);


/* Native tags shared with RealmDocument.h live in RealmTags.h. */

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

struct CssomRef {
    void* ptr;
    int   kind;
};

/* Per-document JS state (one document == one realm). Defined right after
 * RuntimeState; the forward declaration keeps the view struct independent of
 * the document details. */
struct DocumentRuntime;
struct Listener;

struct RuntimeState {
    AntbEngine*        engine;
    AntbRealm*         mainRealm;       /* realm of the top-level document */
    WebCore::Document* document;
    AntbClass*         nodeClass;
    AntbClass*         canvasCtxClass;  /* CanvasRenderingContext2D (2d context) */
    AntbClass*         windowClass;     /* Window (per-frame window proxy) */
    AntbClass*         styleSheetListClass;
    AntbClass*         styleSheetClass;
    AntbClass*         cssRuleClass;
    AntbClass*         cssRuleListClass;
    AntbClass*         mediaListClass;
    AntbClass*         cssDeclarationClass;
    std::vector<CssomRef>* cssomRefs;   /* ref'd CSSOM objects, released at realm teardown */
    std::set<void*>*   cssomSeen;
    AntbValue*         docObj;          /* the JS document object (rooted) */
    bool               documentBound;
    void*              listeners;       /* std::vector<Listener>* (see below) */
    WebCore::Document* docSlots[AGB_MAX_DOCUMENT_SLOTS];
    unsigned long      docSlotGeneration[AGB_MAX_DOCUMENT_SLOTS];
    int                docSlotCount;
    int                currentSlot;
    unsigned long      generation;      /* id of the realm (see nextGeneration) */
    /* The generation whose pinned nodes the *next* teardown of this state
     * releases. Per state, not process wide: one view's teardown must never
     * release the nodes another live view still uses (R141). */
    unsigned long      pinsPendingRelease;
    unsigned long      polyfillsGeneration; /* realm whose VM already got polyfills.js */
    /* Page Visibility: one flag per view, shared by every document in it (a
     * frame inherits the top-level state, like the spec). `viewHidden` is what
     * the host last said; `hidden` is what the current document reports, which
     * the teardown forces to 1 without changing what the host said. */
    int                hidden;
    int                viewHidden;
    /* The engine's main realm already got its document object, window and
     * global API. False until the first document binds after beginNewRealm();
     * a later document in the same realm (a frame in the shared-realm mode)
     * only needs the per-document objects refreshed. */
    bool               realmBound;
    /* iframe realms belong to the view that owns the engine, never to the
     * process: a process-wide table let a navigation in one tab release the
     * frame realms of every other tab (S0 of design-context-ownership.md). */
    std::vector<DocumentRuntime*>* documents;
    /* window.addEventListener() handlers live on the *realm*, not on the
     * document object: a reload binds a provisional and then a committed
     * document in the same realm, and the handler registered by the page's
     * first inline script must still be there when the load event is
     * dispatched for the other one. Per-frame realms keep their own table,
     * which is what isolates a frame's handlers from its parent (R132). */
    std::map<AntbRealm*, std::vector<Listener>*>* realmListeners;
    /* Module / script scheduling is per view (roadmap P1-a): two tabs must not
     * share download slots, the in-flight counter, or each other's queued and
     * deferred modules. The bodies themselves are cached per view as well -
     * a shared in-flight entry would be cancelled by whichever view goes away
     * first (the network layer's HTTP cache still dedupes the transfer). */
    void*              moduleCache;        /* std::map<std::string, ModuleEntry*>* */
    void*              moduleSession;      /* Net::Session* */
    void*              modulePending;      /* std::vector<std::pair<std::string,unsigned long>>* */
    void*              moduleEntryScripts; /* std::map<std::string, unsigned long>* */
    void*              arrivedEntryBodies; /* std::vector<ArrivedEntryBody>* */
    void*              deferredModules;    /* std::vector<DeferredModule>* */
    void*              lastFlushDocument;  /* WebCore::Document* */
    int                moduleInFlight;
    /* Last document activated for this view: `activateState()` restores it when
     * the engine thread switches views. */
    DocumentRuntime*   activeDocument;
    /* Documents saved by contextActivate() while a bridged callback runs, in
     * call order. ant switches realms per function call but the host mirror is
     * switched once on entry, so a callback that belongs to another realm (a
     * child frame calling a node method of the parent realm's class) must put
     * the caller's document back on the way out - see contextLeave(). */
    std::vector<DocumentRuntime*>* bindStack;
    int                nextFrameSlot;
};

/* ------------------------------------------------------------------------- */
/* DocumentRuntime: one bound document (== one realm)                        */
/* ------------------------------------------------------------------------- */
/* Everything that used to live on RuntimeState but belongs to the document:
 * the DOM document, its JS object, its listeners, its CSSOM refs and its
 * realm. The realm handle is the identity used by the (engine, realm) ->
 * document lookup, so "which document am I" no longer depends on whichever
 * activate happened last (R130's boundRealm/mainDocObj patch existed for
 * exactly that). */
struct DocumentRuntime {
    RuntimeState*      view;
    AntbRealm*         realm;     /* this document's realm (NULL before bind) */
    WebCore::Document* document;
    AntbValue*         docObj;
    AntbValue*         keeper;    /* frame realm: pinned closure keeps it alive */
    bool               documentBound;
    bool               installed; /* realm's global API installed */
    int                realmSlot; /* __agbFrameSlot number (0 based), -1 = main */
    /* This document's id (S2): async work started by the document (XHR, module
     * fetch, deferred script) carries it, so dropping *this* document (a frame
     * going away) cancels exactly its work. The view's generation stays the
     * realm-lifetime id used by pins/polyfills and whole-realm teardown. */
    unsigned long      generation;
    std::vector<CssomRef>* cssomRefs;
    std::set<void*>*   cssomSeen;
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
/* The document the mirrors below point at. Never NULL: code that runs before a
 * document is bound (startup, teardown) reads the empty document's fields
 * instead of dereferencing a null pointer. */
static DocumentRuntime  g_emptyDocument = {
    &g_emptyState, NULL, NULL, NULL, NULL, false, false, -1, 0, NULL, NULL
};
static DocumentRuntime* g_currentDocument = &g_emptyDocument;

/* Last context the engine thread entered (see activateState). Read from other
 * threads by agb_busy_js_context(). */
static void* g_busyContext = NULL;

/* AGB_CONTEXT_ASSERT=1: every host->engine entry is expected on the engine
 * thread. AntRuntime's state is engine-thread-only, and a UI-thread call that
 * touches it fails *silently* today (it reads whatever document was left
 * there). This makes that class of bug audible in debug runs. */
static DWORD g_contextEngineThread = 0;
static int   g_contextAssert = -1;

static void assertContextThread(const char* where)
{
    DWORD self;

    if (g_contextAssert < 0) {
        const char* value = getenv("AGB_CONTEXT_ASSERT");

        g_contextAssert = (value && value[0] && value[0] != '0') ? 1 : 0;
    }

    if (!g_contextAssert)
        return;

    self = GetCurrentThreadId();

    if (!g_contextEngineThread) {
        g_contextEngineThread = self;
        return;
    }

    if (self != g_contextEngineThread)
        agbLog(AGB_LOG_EVT, "[ctx] %s on non-engine thread %lu (engine thread %lu)",
               where, (unsigned long)self, (unsigned long)g_contextEngineThread);
}

/* The generation of the realm that is currently running (0 when there is
 * none). */
static unsigned long currentGeneration(void)
{
    if (!g_currentState || g_currentState == &g_emptyState)
        return 0;

    return g_currentState->generation;
}

/* The id of the document that is running right now (0 before a document is
 * bound). Async work is tagged with it so dropping that document cancels its
 * work without touching its neighbours'. */
static unsigned long currentDocumentGeneration(void)
{
    if (!g_currentDocument || g_currentDocument == &g_emptyDocument)
        return 0;

    return g_currentDocument->generation;
}

/* Page Visibility, as the page sees it: document.hidden / visibilityState. */
static int documentHidden(void)
{
    return (g_currentState && g_currentState != &g_emptyState) ? g_currentState->hidden : 0;
}

static const char* visibilityStateName(void)
{
    return documentHidden() ? "hidden" : "visible";
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
        if (getenv("AGB_EVT_DEBUG"))
            agbLog(AGB_LOG_EVT, "pins release gen=%lu nodes=%d", generation, (int)nodes->size());

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
    state->mainRealm = engine ? antb_realm_current(engine) : NULL;
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

static void activateDocument(DocumentRuntime* document)
{
    if (!document)
        document = &g_emptyDocument;

    g_currentDocument = document;

    if (document->view) {
        g_currentState = document->view;
        /* Remember it on the view: activateState() restores this document when
         * the engine thread switches back to the view. */
        document->view->activeDocument = (document == &g_emptyDocument) ? NULL : document;
    }
}

static void activateState(RuntimeState* state)
{
    if (!state)
        return;

    g_currentState = state;
    /* The view's last document comes back with it: a view switch must not leave
     * another view's document in the mirrors. */
    g_currentDocument = state->activeDocument ? state->activeDocument : &g_emptyDocument;

    /* Who the engine thread is in right now. Only the engine thread writes it;
     * the hang watchdog (UI side) reads it to attribute a stuck script to one
     * view instead of broadcasting. A plain pointer exchange is enough: the
     * reader only compares identity. */
    InterlockedExchangePointer((void* volatile*)&g_busyContext, (void*)state);
}

/* The mirrors. Keep these names: 250+ references across this file rely on them.
 * View-scoped fields read through g_currentState, document-scoped ones through
 * g_currentDocument (design-context-ownership.md §3). */
#define g_engine        (g_currentState->engine)
#define g_document      (g_currentDocument->document)
#define g_nodeClass     (g_currentState->nodeClass)
#define g_canvasCtxClass (g_currentState->canvasCtxClass)
#define g_windowClass   (g_currentState->windowClass)
#define g_styleSheetListClass (g_currentState->styleSheetListClass)
#define g_styleSheetClass     (g_currentState->styleSheetClass)
#define g_cssRuleClass        (g_currentState->cssRuleClass)
#define g_cssRuleListClass    (g_currentState->cssRuleListClass)
#define g_mediaListClass      (g_currentState->mediaListClass)
#define g_cssDeclarationClass (g_currentState->cssDeclarationClass)
#define g_cssomRefs     (g_currentDocument->cssomRefs)
#define g_cssomSeen     (g_currentDocument->cssomSeen)
#define g_docObj        (g_currentDocument->docObj)
#define g_documentBound (g_currentDocument->documentBound)
#define g_docSlots      (g_currentState->docSlots)
#define g_docSlotGeneration (g_currentState->docSlotGeneration)
#define g_docSlotCount  (g_currentState->docSlotCount)
#define g_currentSlot   (g_currentState->currentSlot)

/* Per-document JS object. Every frame gets its own document object, so
 * `parent.document` and `document` are two different objects; the same
 * document always maps to the same object (`document === window.document`).
 * Keyed by (engine, document) because one document outlives many binds. */
struct DocObjectEntry {
    AntbEngine*        engine;
    WebCore::Document* document;
    AntbValue*         object;
};

static std::vector<DocObjectEntry>* g_docObjectCache = NULL;

struct WindowEntry {
    AntbEngine*        engine;
    WebCore::Document* document;
    AntbValue*         object;
};

static std::vector<WindowEntry>* g_windowCache = NULL;




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

/* Realms are defined below; activateSlot() needs them first. */
static bool perFrameRealmEnabled();
static DocumentRuntime* documentRuntimeFor(RuntimeState* state, WebCore::Document* document);

static void activateSlot(int slot)
{
    WebCore::Document* document;
    DocumentRuntime* runtime;

    if (slot < 0 || slot >= g_docSlotCount || !g_docSlots[slot])
        return;

    /* Per-frame realms use realm slots (globalActivateSlot handles them); the
     * legacy document slot must never repoint a frame realm's document object
     * at another document (R130). */
    if (perFrameRealmEnabled() && g_engine && g_currentState
            && g_currentState != &g_emptyState && g_currentState->mainRealm
            && antb_realm_current(g_engine) != g_currentState->mainRealm)
        return;

    document = g_docSlots[slot];
    runtime = documentRuntimeFor(g_currentState, document);

    if (!runtime)
        return;

    g_currentSlot = slot;
    activateDocument(runtime);

    if (g_docObj)
        antb_set_native(g_docObj, g_document, AGB_TAG_NODE);

    /* Shared-realm mode: `document` is one global property for every frame of
     * the view, so switching the active document must repoint it (each
     * document has its own object now). With per-frame realms every realm has
     * its own global and applyRealmBinding() already picked the right one. */
    if (!perFrameRealmEnabled() && g_engine) {
        AntbValue* global = antb_global(g_engine);

        if (global) {
            if (g_docObj)
                antb_set_prop(g_engine, global, "document", g_docObj);
            antb_set_native(global, g_document, AGB_TAG_NODE);
            antb_value_release(global);
        }
    }
}

/* ---- per-frame realms (design-per-frame-realm.md, stages S1/S2) --------- */
/* On by default: every frame document runs in its own realm, so `var` does not
 * leak across frames and `top.Array !== Array`. `AGB_PER_FRAME_REALM=0` goes
 * back to the shared-realm model (one realm per engine + per-frame window
 * proxy) for A/B work. Module maps are still per engine (S3) and `parent`
 * is the parent realm's global rather than a WindowProxy (S4). */
static bool perFrameRealmEnabled()
{
    static int cached = -1;

    if (cached < 0) {
        const char* v = getenv("AGB_PER_FRAME_REALM");

        cached = (v && !strcmp(v, "0")) ? 0 : 1;
    }

    return cached == 1;
}

/* ---- documents and realms (design-context-ownership.md §3) ------------- */
/* One bound document owns one realm. The table is per view: a realm handle
 * belongs to the engine that created it, and a navigation may only release the
 * documents of its own view (it used to be a file-level table, so a navigation
 * in one tab dropped every other tab's frame realms). */

static std::vector<DocumentRuntime*>* documentsOf(RuntimeState* state)
{
    if (!state || state == &g_emptyState)
        return NULL;

    return state->documents;
}

static std::vector<DocumentRuntime*>& documentsFor(RuntimeState* state)
{
    static std::vector<DocumentRuntime*> empty;

    if (!state || state == &g_emptyState)
        return empty;

    if (!state->documents)
        state->documents = new std::vector<DocumentRuntime*>();

    return *state->documents;
}

struct Listener;   /* the listener table is per realm (defined below) */
static void cssomReleaseRefs(DocumentRuntime* document);
static void releaseAllRealmListeners(RuntimeState* state);
static void releaseRealmListeners(RuntimeState* state, AntbRealm* realm);

/* Looks a document record up without creating one. */
static DocumentRuntime* documentRuntimeOf(RuntimeState* state, WebCore::Document* document)
{
    std::vector<DocumentRuntime*>* documents = documentsOf(state);

    if (!documents || !document)
        return NULL;

    for (size_t i = 0; i < documents->size(); ++i) {
        if ((*documents)[i]->document == document)
            return (*documents)[i];
    }

    return NULL;
}

/* Get-or-create the record for `document` in `state`. */
static DocumentRuntime* documentRuntimeFor(RuntimeState* state, WebCore::Document* document)
{
    std::vector<DocumentRuntime*>& documents = documentsFor(state);

    if (!state || state == &g_emptyState || !document)
        return NULL;

    for (size_t i = 0; i < documents.size(); ++i) {
        if (documents[i]->document == document)
            return documents[i];
    }

    DocumentRuntime* created = new DocumentRuntime();

    memset(created, 0, sizeof(*created));
    created->view = state;
    created->document = document;
    created->realmSlot = -1;
    created->generation = nextGeneration();
    documents.push_back(created);

    return created;
}

/* The document that owns `realm`: the main realm is the view's top-level
 * document, a frame realm is the document that created it. NULL when the realm
 * has no document bound yet. */
static DocumentRuntime* documentForRealm(RuntimeState* state, AntbRealm* realm)
{
    std::vector<DocumentRuntime*>* documents = documentsOf(state);

    if (!realm || !documents)
        return NULL;

    if (state->mainRealm == realm) {
        for (size_t i = 0; i < documents->size(); ++i) {
            DocumentRuntime* document = (*documents)[i];

            if (document->document && !document->document->parentDocument())
                return document;
        }
        return NULL;
    }

    for (size_t i = 0; i < documents->size(); ++i) {
        if ((*documents)[i]->realm == realm)
            return (*documents)[i];
    }

    return NULL;
}

/* The view that owns `document`, or NULL when none does. */
static RuntimeState* stateForDocument(WebCore::Document* document)
{
    std::vector<RuntimeState*>& states = runtimeStates();

    for (size_t i = 0; i < states.size(); ++i) {
        std::vector<DocumentRuntime*>* documents = documentsOf(states[i]);

        if (!documents)
            continue;

        for (size_t j = 0; j < documents->size(); ++j) {
            if ((*documents)[j]->document == document)
                return states[i];
        }
    }

    return NULL;
}

/* Drops a document record and everything it owns. `releaseValues` is false once
 * the engine is gone: releasing handles of a destroyed VM touches freed memory,
 * and only the record itself is left to drop then. */
static void releaseDocumentRuntime(DocumentRuntime* document, bool releaseValues)
{
    std::vector<DocumentRuntime*>* documents;
    RuntimeState* state;

    if (!document || !document->view)
        return;

    state = document->view;
    documents = documentsOf(state);

    if (releaseValues) {
        if (document->keeper) {
            antb_value_release(document->keeper);
            document->keeper = NULL;
        }
        if (document->docObj) {
            antb_value_release(document->docObj);
            document->docObj = NULL;
        }
        cssomReleaseRefs(document);
    }

    if (documents) {
        for (size_t i = 0; i < documents->size(); ++i) {
            if ((*documents)[i] == document) {
                documents->erase(documents->begin() + i);
                break;
            }
        }
    }

    if (state->activeDocument == document)
        state->activeDocument = NULL;
    if (g_currentDocument == document)
        g_currentDocument = &g_emptyDocument;

    delete document;
}

/* Looks the realm of a frame document up, creating it on first use. Returns
 * NULL when the engine has no realm support (or the allocation failed), which
 * makes the caller fall back to the shared realm. */
static AntbRealm* realmForFrameDocument(AntbEngine* engine, WebCore::Document* document)
{
    RuntimeState* state = stateForEngine(engine);
    DocumentRuntime* runtime = documentRuntimeFor(state, document);

    if (!engine || !document || !runtime)
        return NULL;

    if (runtime->realm)
        return runtime->realm;

    AntbRealm* realm = antb_realm_create(engine);

    if (!realm)
        return NULL;

    runtime->realm = realm;
    runtime->realmSlot = state->nextFrameSlot++;
    runtime->installed = false;

    /* A realm is only kept alive while something in it is reachable: ant marks
     * a realm when a closure or builtin of it is marked, not when its global
     * object is. Pin an empty closure inside the realm so it cannot be
     * collected from under the document record (released with the engine). */
    {
        AntbRealm* prev = antb_realm_enter(engine, realm);
        AntbValue* keeper = antb_eval(engine, "(function(){})", 14, NULL);

        if (keeper) {
            runtime->keeper = antb_retain(keeper);
            antb_value_release(keeper);
        }

        antb_realm_enter(engine, prev);
    }

    agbLog(AGB_LOG_EVT, "frame realm %p for document %p", (void*)realm, (void*)document);

    return realm;
}

/* The current view's document record for `realm`. */
static DocumentRuntime* documentForCurrentRealm(AntbRealm* realm)
{
    return documentForRealm(g_currentState, realm);
}

/* __agbFrameSlot number -> document. 0 is the main realm; frames are slot+1. */
static DocumentRuntime* documentForFrameSlot(int slotNumber)
{
    std::vector<DocumentRuntime*>* documents = documentsOf(g_currentState);

    if (!documents || slotNumber <= 0)
        return NULL;

    for (size_t i = 0; i < documents->size(); ++i) {
        if ((*documents)[i]->realmSlot == slotNumber - 1)
            return (*documents)[i];
    }

    return NULL;
}

/* Point the host-side "current document" mirrors at `realm`'s document. The
 * document record is the authority now: no boundRealm comparison, no
 * mainDocObj bookkeeping (R130's patch existed only because the realm ->
 * document mapping was missing). */
static void applyRealmBinding(AntbRealm* realm)
{
    DocumentRuntime* document = documentForCurrentRealm(realm);

    if (!document)
        return;   /* realm exists but has no document bound yet */

    activateDocument(document);
}

/* The realm the engine is in right now. ant switches realm per function call,
 * without telling the host; this reads the current realm back. */
static AntbRealm* currentRealm()
{
    if (!g_engine)
        return NULL;

    return antb_realm_current(g_engine);
}

/* 0 for the main realm, 1..N for frame realms (the number __agbFrameSlot
 * returns under per-frame realms), -1 when the current realm is unknown. */
static int currentRealmSlotNumber()
{
    AntbRealm* realm;

    if (!perFrameRealmEnabled() || !g_currentState || g_currentState == &g_emptyState)
        return g_currentSlot;

    realm = currentRealm();

    if (!realm || realm == g_currentState->mainRealm)
        return 0;

    if (DocumentRuntime* document = documentForCurrentRealm(realm))
        return document->realmSlot + 1;

    return -1;
}

/* bindDocument() bound the main document into the engine's main realm. With the
 * per-document records this is only a debug trace: the binding *is* the record,
 * and documentForRealm() finds it by its realm. */
static void noteMainBinding()
{
    agbLog(AGB_LOG_EVT, "main binding gen=%lu doc=%p realm=%p",
           g_currentState ? g_currentState->generation : 0UL,
           (void*)g_document, (void*)currentRealm());
}

/* The realm a document runs in: the engine's main realm for a top-level
 * document (or for a frame when per-frame realms are off, in which case the
 * caller only uses it for parent/top lookups), a dedicated realm otherwise. */
static AntbRealm* realmOfDocument(AntbEngine* engine, WebCore::Document* document)
{
    if (!engine || !document)
        return NULL;

    if (!document->parentDocument())
        return (g_currentState && g_currentState != &g_emptyState) ? g_currentState->mainRealm
                                                                  : antb_realm_current(engine);

    return realmForFrameDocument(engine, document);
}

/* Drops every document of the view (main + frames). A document owns its realm,
 * its JS object and its listeners, so this is the one place a view's document
 * state goes away. `releaseValues` is false when the engine that owns those
 * values is already gone: releasing handles of a destroyed VM would touch
 * freed memory, and only the records are left to drop then. */
static void forgetDocuments(RuntimeState* state, bool releaseValues)
{
    std::vector<DocumentRuntime*>* documents = documentsOf(state);

    if (!documents)
        return;

    /* releaseDocumentRuntime() edits the vector; walk from the back. */
    while (!documents->empty())
        releaseDocumentRuntime(documents->back(), releaseValues);

    delete state->documents;
    state->documents = NULL;
}

/* Which realms already evaluated prelude.js / polyfills.js. Keyed by realm, not
 * by generation: every frame realm needs its own copies (a realm has its own
 * global object). Cleared when the engine dies, so a recycled realm address
 * cannot make the next engine skip its shims. */
static std::set<AntbRealm*>* g_preludedRealms = NULL;
static std::set<AntbRealm*>* g_polyfilledRealms = NULL;

static void forgetRealmScripts()
{
    if (g_preludedRealms)
        g_preludedRealms->clear();
    if (g_polyfilledRealms)
        g_polyfilledRealms->clear();
}

int nodeGetAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);int nodeAddEventListener(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeRemoveEventListener(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
WebCore::Document* documentOf(AntbValue* self);
int docGetElementById(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeDispatchEvent(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docWrite(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateHTMLDocument(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docHasFeature(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docWriteln(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
AntbValue* wrapNode(AntbEngine*, WebCore::Node*);
int nodeGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
enum { CSSOM_LIST, CSSOM_SHEET, CSSOM_RULE, CSSOM_RULELIST, CSSOM_MEDIA, CSSOM_DECL };
AntbValue* cssomWrap(AntbEngine*, void*, int kind);
static void cssomReleaseRefs(DocumentRuntime* document);   /* defined below */
int attrsGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int nodeGetElementsByTagName(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeCloneNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateDocumentFragment(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeReplaceChild(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeHasAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeHasChildNodes(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeContains(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeCompareDocumentPosition(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeRemove(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetBoundingClientRect(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetRootNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetAttributeNames(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeToggleAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeInsertAdjacentHTML(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeInsertAdjacentElement(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeInsertAdjacentText(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeReplaceChildren(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeScrollIntoView(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeFormSubmit(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeFormReset(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeFormCheckValidity(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeDocumentImportNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeDocumentAdoptNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeDocumentElementFromPoint(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeDocumentHasFocus(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
template <typename Collection> AntbValue* wrapNodeList(AntbEngine*, Collection*);
int nodeGetContext(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeToDataURL(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);

/* Media elements: one native callback per method, all on the shared Node
 * class (there are no per-tag JS classes), each checking the tag first - the
 * same scheme canvas uses. */
int nodeMediaPlay(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeMediaPause(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeMediaLoad(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeMediaCanPlayType(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeMediaAddTextTrack(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
AntbValue* wrapTextTrack(AntbEngine*, WebCore::TextTrack*);
AntbValue* wrapTextTrackList(AntbEngine*, WebCore::TextTrackList*);
AntbValue* wrapTextTrackCue(AntbEngine*, WebCore::TextTrackCue*);
void installTextTrackClasses(AntbEngine*);
bool mediaElementProperty(AntbEngine*, WebCore::HTMLMediaElement*, const char*, AntbValue* out);
bool mediaElementSetProperty(WebCore::HTMLMediaElement*, const char*, const AntbValue*);

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
int globalClipboardSetText(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalFramePostMessage(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalConsoleLine(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalConsoleLineEx(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);

int globalLayoutInfo(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
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
int globalStylePropertyNames(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalPrefersDark(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalViewportSize(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalInputValue(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
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

/* Monotonic ids for JS-initiated requests. The shim keeps the id so it can
 * abort a request the page gave up on (xhr.timeout, xhr.abort(), a fetch
 * AbortSignal): the request itself is still running on the network thread, and
 * nothing else can reach it from JS. */
static long long nextXhrId(void)
{
    static long long counter = 0;

    return ++counter;
}

struct XhrJob : public WebCore::ResourceHandleClient {
    XhrJob(AntbEngine* engine, AntbValue* callback, bool binary = false,
           const char* method = NULL, const char* contentType = NULL,
           const char* body = NULL)
        : m_engine(engine)
        , m_callback(antb_retain(callback))
        , m_status(0)
        , m_slot(g_currentSlot)
        , m_startMs(0)
        , m_doneMs(0)
        , m_done(false)
        , m_failed(false)
        , m_binary(binary)
        , m_method(method ? method : "GET")
        , m_contentType(contentType ? contentType : "")
        , m_postBody(body ? body : "")
        , m_generation(currentGeneration())
        , m_documentGeneration(currentDocumentGeneration())
        , m_id(nextXhrId())
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
        std::string cookies = cookieHeaderForDocument(g_document);

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
        m_statusText = response.httpStatusText().utf8().data();

        /* XHR exposes the response headers to page code (getResponseHeader /
         * getAllResponseHeaders), and the page's overrideMimeType() can only
         * win over the header charset if the header is known here. Keep the
         * whole map, not just Set-Cookie. */
        {
            const WebCore::HTTPHeaderMap& fields = response.httpHeaderFields();
            WebCore::HTTPHeaderMap::const_iterator end = fields.end();

            for (WebCore::HTTPHeaderMap::const_iterator it = fields.begin(); it != end; ++it)
                m_responseHeaders.push_back(std::make_pair(
                    std::string(it->first.utf8().data()),
                    std::string(it->second.utf8().data())));
        }

        {
            WebCore::String setCookie = response.httpHeaderField("Set-Cookie");

            if (!setCookie.isEmpty())
                setCookieForDocument(g_document, std::string(setCookie.utf8().data()));
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
        AntbValue* callback = m_callback;
        int previousSlot = g_currentSlot;

        /* Take the callback out first: (a) a re-entrant delivery must not run it
         * twice, (b) cancel() during the call must not release what we hold,
         * (c) the handle from antb_retain() has exactly one owner - this frame.
         * It used to be dropped by `m_callback = NULL` without a release, which
         * leaked one AntbValue per delivered request (the GC pin itself stays
         * for the engine's lifetime, see antb_retain()). */
        m_callback = NULL;

        if (m_slot >= 0)
            activateSlot(m_slot);

        if (Net::logEnabled())
            Net::logLine("xhr.deliver lag=%.1fms total=%.1fms status=%d bytes=%u",
                         m_doneMs > 0 ? Net::logElapsedMs() - m_doneMs : -1.0,
                         m_startMs > 0 ? Net::logElapsedMs() - m_startMs : -1.0,
                         m_status, (unsigned)m_body.size());

        if (callback && m_engine) {
            std::string payloadText;

            if (m_binary)
                payloadText = base64Encode(m_body.data(), m_body.size());
            else
                /* m_body holds the raw response bytes: UTF-8 in practice. Going
                 * through WebCore::String(data, size) treats them as Latin-1 and
                 * .utf8() then double-encodes (v.qq's console text arrived as
                 * "é¢„çº¦..." instead of "预约..."). Decode UTF-8 once. */
                payloadText = WebCore::String::fromUTF8(m_body.data(), m_body.size()).utf8().data();

            {
                AntbValue* textValue = antb_make_string(m_engine, payloadText.c_str());
                AntbValue* statusValue = antb_make_number(m_engine, m_failed ? 0.0 : (double)m_status);
                /* Third argument: the response metadata the shim needs to answer
                 * statusText / getResponseHeader / getAllResponseHeaders without
                 * a second round trip. A plain object with "statusText" and
                 * "headers" (an array of name/value pairs, arrival order kept). */
                AntbValue* metaValue = m_failed ? NULL : makeResponseMeta(m_engine);

                if (textValue && statusValue) {
                    AntbValue* argArray[3];
                    AntbValue* result;
                    int argc = 2;

                    argArray[0] = statusValue;
                    argArray[1] = textValue;
                    if (metaValue) {
                        argArray[2] = metaValue;
                        argc = 3;
                    }

                    result = antb_call(m_engine, callback, NULL, argArray, argc, NULL);
                    if (result)
                        antb_value_release(result);
                }

                if (textValue) antb_value_release(textValue);
                if (statusValue) antb_value_release(statusValue);
                if (metaValue) antb_value_release(metaValue);
            }
        }

        if (callback)
            antb_value_release(callback);

        if (previousSlot >= 0)
            activateSlot(previousSlot);
    }

    bool done() const { return m_done; }

    /* {statusText: "OK", headers: [["name","value"], ...]} - the pair shape
     * keeps header-name case and duplicates (Set-Cookie) intact; the shim does
     * the spec's lowercase + sort + CRLF join, because that is presentation. */
    AntbValue* makeResponseMeta(AntbEngine* engine)
    {
        AntbValue* meta = antb_make_object(engine);

        if (!meta)
            return NULL;

        AntbValue* text = antb_make_string(engine, m_statusText.c_str());

        if (text) {
            antb_set_prop(engine, meta, "statusText", text);
            antb_value_release(text);
        }

        {
            AntbValue* list = antb_make_array(engine);

            if (list) {
                for (size_t i = 0; i < m_responseHeaders.size(); ++i) {
                    AntbValue* pair = antb_make_array(engine);
                    AntbValue* name = antb_make_string(engine, m_responseHeaders[i].first.c_str());
                    AntbValue* value = antb_make_string(engine, m_responseHeaders[i].second.c_str());

                    if (!pair || !name || !value) {
                        if (pair) antb_value_release(pair);
                        if (name) antb_value_release(name);
                        if (value) antb_value_release(value);
                        continue;
                    }

                    antb_array_push(engine, pair, name);
                    antb_array_push(engine, pair, value);
                    antb_array_push(engine, list, pair);
                    antb_value_release(pair);
                    antb_value_release(name);
                    antb_value_release(value);
                }

                antb_set_prop(engine, meta, "headers", list);
                antb_value_release(list);
            }
        }

        return meta;
    }

    AntbEngine*                     m_engine;
    AntbValue*                      m_callback;
    RefPtr<WebCore::ResourceHandle> m_handle;
    Vector<char>                    m_body;
    int                             m_status;
    std::string                     m_statusText;
    /* Response header list, in arrival order. Duplicates (Set-Cookie) are
     * kept: XMLHttpRequest's getAllResponseHeaders() joins them. */
    std::vector<std::pair<std::string, std::string> > m_responseHeaders;
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
    unsigned long                   m_documentGeneration; /* the document that started it */
    long long                       m_id;          /* the id JS aborts with */

    unsigned long generation() const { return m_generation; }
    unsigned long documentGeneration() const { return m_documentGeneration; }
    long long id() const { return m_id; }
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

/* S2: one document going away (a frame detached or navigated) cancels the
 * requests that document started, and nothing else. Matched by id, not by
 * "<=": a frame's id is newer than its parent document's, so a range compare
 * would take the parent's work down with it. */
static void cancelXhrJobsForDocument(unsigned long documentGeneration)
{
    Vector<XhrJob*>& jobs = xhrJobs();

    if (!documentGeneration)
        return;

    for (unsigned i = 0; i < jobs.size(); ) {
        XhrJob* job = jobs[i];

        if (job && job->documentGeneration() == documentGeneration) {
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
 * somewhere that is known to be safe.
 *
 * Deliveries run page JS, and that JS re-enters the runtime (appending a
 * <script>, a module, a job boundary that flushes microtasks). The nested
 * evaluation calls this function again, and the first version then walked a
 * vector that the nested frame had already shortened: `delete jobs[i]`
 * destroyed a *different* job and `jobs[i]` past the end tripped
 * `ASSERT(i < size())` in Vector::at (v.qq.com, 0xc0000005, write 0xbbadbeef
 * - the assertion's poison value).
 *
 * Two rules keep it safe:
 *   - the finished jobs are detached from xhrJobs() *before* any JS runs, so a
 *     nested collect() only ever sees the jobs nobody is delivering yet;
 *   - a nested collect() returns immediately: the outer frame delivers the
 *     detached list itself. */
static bool g_inCollectXhr = false;

static void collectXhrJobs()
{
    Vector<XhrJob*>& jobs = xhrJobs();
    Vector<XhrJob*> finished;

    if (g_inCollectXhr)
        return;

    for (unsigned i = 0; i < jobs.size(); ) {
        if (jobs[i]->done()) {
            finished.append(jobs[i]);
            jobs.remove(i);
        } else {
            ++i;
        }
    }

    if (finished.isEmpty())
        return;

    g_inCollectXhr = true;

    for (unsigned i = 0; i < finished.size(); ++i) {
        XhrJob* job = finished[i];

        /* The callback can navigate away (or reload): then the realm that
         * started this request is gone and the response must not be handed to
         * the one that replaces it. */
        if (job->generation() < currentGeneration())
            job->cancel();
        else
            job->deliver();
        delete job;
    }

    g_inCollectXhr = false;
}

/* Hands the job id to JS: __agbHttpRequest* return it so the shim can abort the
 * request later (xhr.timeout / abort / fetch signal). */
static void assignXhrId(AntbEngine* engine, AntbValue* out, XhrJob* job)
{
    AntbValue* value;

    if (!out || !job)
        return;

    value = antb_make_number(engine, (double)job->id());

    if (value) {
        antb_value_assign(out, value);
        antb_value_release(value);
    }
}

int globalHttpRequest(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    char* urlText;
    XhrJob* job;

    if (argc < 2)
        return 1;

    urlText = antb_to_utf8(args[0]);
    if (!urlText)
        return 1;

    job = new XhrJob(engine, args[1]);
    xhrJobs().append(job);
    job->start(urlText);
    assignXhrId(engine, out, job);

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
    assignXhrId(engine, out, job);

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
    assignXhrId(engine, out, job);

    antb_free(urlText);
    antb_free(methodText);
    antb_free(typeText);
    antb_free(bodyText);
    return 1;
}

/* __agbHttpAbort(id): the page gave up on a request (timeout, abort(),
 * AbortSignal). cancel() closes the socket and drops the JS callback; the job
 * stays in the list with done=true, so the next collectXhrJobs() reaps it
 * without delivering anything. */
int globalHttpAbort(AntbEngine*, AntbValue*, AntbValue** args, int argc,
                    AntbValue*, void*)
{
    long long id;
    Vector<XhrJob*>& jobs = xhrJobs();

    if (argc < 1 || !args[0])
        return 1;

    id = (long long)antb_to_number(args[0]);

    if (id <= 0)
        return 1;

    for (unsigned i = 0; i < jobs.size(); ++i) {
        if (jobs[i] && jobs[i]->id() == id) {
            jobs[i]->cancel();
            break;
        }
    }

    return 1;
}

int globalHttpGet(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpGetEx(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpGetBin(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalClassOp(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpRequest(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpRequestBin(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpRequestEx(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalHttpAbort(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
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
static std::string hostOfDocument(WebCore::Document* document)
{
    if (!document)
        return std::string();
    {
        WebCore::KURL url(document->URL());
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
static std::string cookieHeaderForDocument(WebCore::Document* document)
{
    std::string out;
    std::string host = hostOfDocument(document);
    std::vector<CookieEntry>& jar = cookieJar();
    for (size_t i = 0; i < jar.size(); ++i) {
        if (!domainMatches(jar[i].domain, host))
            continue;
        if (jar[i].secure && document && !WebCore::KURL(document->URL()).protocol().startsWith("https"))
            continue;
        if (!out.empty())
            out += "; ";
        out += jar[i].name + "=" + jar[i].value;
    }
    return out;
}
static void setCookieForDocument(WebCore::Document* document, const std::string& text)
{
    std::string host = hostOfDocument(document);
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
/* A getter that returns 1 without touching `out` leaves the property
 * undefined; where the DOM says "null" (document.body before <body> is
 * parsed, a detached cssRule.parentStyleSheet, ...) say null explicitly. */
static int assignNull(AntbEngine* engine, AntbValue* out)
{
    AntbValue* v = antb_make_null(engine);

    if (!v)
        return 0;
    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

/* document.title */
int titleGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    /* The exotic getter is called for every property of the document object, so
     * unknown names must fall through instead of returning the title. */
    WebCore::Document* d = documentOf(self);

    if (!name || !d)
        return 0;

    if (!strcmp(name, "title")) {
        WebCore::String title = d->title();
        AntbValue* v = antb_make_string(engine, title.utf8().data());
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "styleSheets")) {
        /* document.styleSheets: the document object has one catch-all getter,
         * so new properties are dispatched here rather than by another
         * antb_define_property_rw() (which would replace this whole getter). */
        AntbValue* list = cssomWrap(engine, d->styleSheets(), CSSOM_LIST);

        if (!list)
            return 0;
        antb_value_assign(out, list);
        antb_value_release(list);
        return 1;
    }

    if (!strcmp(name, "classList")) {
        return nodeClassList(engine, self, NULL, 0, out, NULL);
    }


    if (!strcmp(name, "head")) {
        RefPtr<WebCore::NodeList> heads = d->getElementsByTagName("head");
        WebCore::Node* headNode = (heads && heads->length()) ? heads->item(0) : 0;
        if (!headNode)
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, headNode);
            if (v) { antb_value_assign(out, v); antb_value_release(v); }
        }
        return 1;
    }

    if (!strcmp(name, "body")) {
        /* document.body is null - not undefined - before the body element has
         * been parsed (a script in <head> reading it early).  Returning
         * undefined here made the difference visible: sites that follow the
         * spec ("Cannot read properties of null") crashed with an undefined
         * message, and page code that tests document.body !== null took the
         * wrong branch. */
        if (!d->body())
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, d->body());
            if (v) { antb_value_assign(out, v); antb_value_release(v); }
        }
        return 1;
    }

    if (!strcmp(name, "defaultView") || !strcmp(name, "parentWindow")) {
        AntbValue* v = AntRuntime::windowForDocument(engine, d);

        if (!v)
            return 1;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "currentScript")) {
        /* The <script> whose code is running, or null (R95). A JSONP loader or a
         * third-party snippet reads its own tag through this - `.src` for a base
         * URL, `data-*` attributes for configuration. */
        WebCore::Element* script = d->currentScript();

        if (!script)
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, script);
            if (v) { antb_value_assign(out, v); antb_value_release(v); }
        }
        return 1;
    }

    if (!strcmp(name, "documentElement")) {
        if (!d->documentElement())
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, d->documentElement());
            if (v) { antb_value_assign(out, v); antb_value_release(v); }
        }
        return 1;
    }

    if (!strcmp(name, "forms") || !strcmp(name, "images") || !strcmp(name, "links")
            || !strcmp(name, "anchors") || !strcmp(name, "scripts") || !strcmp(name, "embeds")
            || !strcmp(name, "plugins") || !strcmp(name, "applets")) {
        /* Real collections now: the empty array this used to return made
         * every `document.forms.length` / `document.images[0]` wrong, and the
         * page silently took the "no such element" branch. Snapshots, not
         * live collections (see docs/arch-capability-gaps.md). */
        const char* tagName = !strcmp(name, "forms")    ? "form"
                            : !strcmp(name, "images")   ? "img"
                            : !strcmp(name, "scripts")  ? "script"
                            : !strcmp(name, "links")    ? "a"
                            : !strcmp(name, "anchors")  ? "a"
                            : !strcmp(name, "embeds")   ? "embed"
                            : !strcmp(name, "applets")  ? "applet"
                            : "object";
        RefPtr<WebCore::NodeList> list = d->getElementsByTagName(tagName);
        AntbValue* array = wrapNodeList(engine, list.get());

        if (!array)
            return 0;
        antb_value_assign(out, array);
        antb_value_release(array);
        return 1;
    }

    if (!strcmp(name, "activeElement")) {
        /* Nothing focused: the body is the active element, like a browser. */
        WebCore::Node* focused = d->focusedNode();

        if (!focused || !focused->isElementNode())
            focused = d->body();
        if (!focused)
            return 1;
        {
            AntbValue* v = wrapNode(engine, focused);

            if (!v)
                return 1;
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        return 1;
    }

    if (!strcmp(name, "visibilityState")) {
        AntbValue* v = antb_make_string(engine, visibilityStateName());

        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "hidden")) {
        /* document.hidden is the counterpart of visibilityState, not the
         * element attribute. */
        AntbValue* v = antb_make_number(engine, documentHidden() ? 1 : 0);

        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "fonts")) {
        AntbValue* global = antb_global(engine);
        AntbValue* fonts = global ? antb_get_prop(engine, global, "__agbFonts") : NULL;

        antb_value_release(global);

        if (!fonts)
            return 1;
        antb_value_assign(out, fonts);
        antb_value_release(fonts);
        return 1;
    }


    if (!strcmp(name, "cookie")) {
        std::string jar = cookieHeaderForDocument(d);
        AntbValue* v = antb_make_string(engine, jar.c_str());
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "readyState")) {
        /* The real state, not the "complete" this used to hardcode.  Pages
         * branch on it to decide whether it is safe to touch the DOM: a
         * report script that saw "complete" while <head> was still being
         * parsed skipped its DOMContentLoaded wait and called
         * document.body.appendChild() too early (bilibili log-reporter).
         * WebKit 2008 spells the middle state "loaded"; the standard name is
         * "interactive", and page code compares against that. */
        WebCore::String state = d->readyState();
        const char* text = "complete";
        AntbValue* v;

        if (state == "loading")
            text = "loading";
        else if (state == "loaded")
            text = "interactive";
        v = antb_make_string(engine, text);
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "URL") || !strcmp(name, "documentURI")) {
        WebCore::String url = d->URL();
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
        AntbValue* loc = AntRuntime::RealmDocument::locationObject(engine, d);

        if (!loc)
            return 0;
        antb_value_assign(out, loc);
        antb_value_release(loc);
        return 1;
    }

    /* Everything else falls through to the node accessors: the document is a
     * Node, and the single exotic getter installed on this object would
     * otherwise swallow nodeType/nodeName/parentNode/... (jQuery's Sizzle
     * bails out when doc.nodeType is undefined). */
    return nodeGetter(engine, self, name, out, NULL);
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
int docWrite(AntbEngine*, AntbValue* self, AntbValue** args, int argc, AntbValue*, void*)
{
    char* text;
    WebCore::Document* document = documentOf(self);

    if (!document || argc < 1)
        return 1;
    text = antb_to_utf8(args[0]);
    if (!text)
        return 1;
    AntRuntime::RealmDocument::checkMismatch("document.write", document);
    document->write(WebCore::String::fromUTF8(text));
    antb_free(text);
    return 1;
}
int docWriteln(AntbEngine*, AntbValue* self, AntbValue** args, int argc, AntbValue*, void*)
{
    char* text;
    WebCore::Document* document = documentOf(self);

    if (!document || argc < 1)
        return 1;
    text = antb_to_utf8(args[0]);
    if (!text)
        return 1;
    AntRuntime::RealmDocument::checkMismatch("document.writeln", document);
    document->writeln(WebCore::String::fromUTF8(text));
    antb_free(text);
    return 1;
}
int titleSetter(AntbEngine* engine, AntbValue* self, const char* name, const AntbValue* value, void*)
{
    if (!name)
        return 0;

    if (!strcmp(name, "title")) {
        WebCore::Document* document = documentOf(self);

        if (!document || !value)
            return 0;
        {
            char* text = antb_to_utf8(value);
            if (!text)
                return 0;
            AntRuntime::RealmDocument::checkMismatch("document.title", document);
            document->setTitle(WebCore::String::fromUTF8(text));
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

        setCookieForDocument(documentOf(self), std::string(text));
        antb_free(text);
        return 1;
    }


    return nodeSetter(engine, self, name, value, NULL);
}

/* (Listener teardown is per realm now: releaseAllRealmListeners().) */
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
static int                g_hangWatchOn = -1;

/* Hang watchdog: the engine thread burning a full core for N seconds with no
 * progress is a JS busy loop (v.qq.com froze the GUI this way). The signal is
 * CPU time, not wall time: a blocked thread stays idle. When it triggers, the
 * engine thread is suspended (VM state frozen) and the bridge prints the live
 * JS stack, so the report names the function and file:line that loops. */
static DWORD WINAPI profilerWatcher(LPVOID)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, g_profThreadId);
    ULONGLONG lastCpu = 0;
    ULONGLONG lastCheck = GetTickCount64();
    int busySeconds = 0;
    int hangSeconds = 5;
    bool sampling = getenv("AGB_PROFILE") != NULL;

    if (!thread)
        return 0;

    if (const char* seconds = getenv("AGB_HANG_SECONDS"))
        hangSeconds = atoi(seconds);

    while (!g_profStop) {
        CONTEXT context;

        Sleep(1);

        /* Sleep(1) is ~15 ms on Windows, so count iterations would stretch the
         * window to ~15 s; use the clock instead. */
        if (GetTickCount64() - lastCheck >= 1000) {
            lastCheck = GetTickCount64();
            FILETIME creation, exitTime, kernel, user;
            ULONGLONG cpu = 0;

            if (GetThreadTimes(thread, &creation, &exitTime, &kernel, &user)) {
                cpu = (((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime)
                    + (((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime);
            }

            if (lastCpu && cpu > lastCpu && (cpu - lastCpu) >= 9000000ULL)   /* >= 0.9 s of a second */
                ++busySeconds;
            else
                busySeconds = 0;

            lastCpu = cpu;

            if (hangSeconds > 0 && busySeconds >= hangSeconds) {
                fprintf(stderr, "\n[hang] engine thread busy for %ds, JS stack:\n", busySeconds);
                fflush(stderr);

                if (SuspendThread(thread) != (DWORD)-1) {
                    if (g_engine)
                        antb_print_js_stack(g_engine);
                    ResumeThread(thread);
                }

                /* Report once per busy streak; reset so a second streak reports again. */
                busySeconds = 0;
                lastCpu = 0;
            }
        }

        if (!sampling)
            continue;

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
    static const char* names[] = { "libagave.dll", "antjsb.dll", NULL };
    static unsigned long long bases[3] = { 0, 0, 0 };
    int i;

    if (!bases[0]) {
        bases[0] = (unsigned long long)GetModuleHandleA("libagave.dll");
        bases[1] = (unsigned long long)GetModuleHandleA("antjsb.dll");
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
    g_hangWatchOn = getenv("AGB_HANG_WATCH") ? 1 : 0;

    if (!g_profSamplerOn && !g_hangWatchOn)
        return;

    g_profThreadId = GetCurrentThreadId();
    g_profWatcher = CreateThread(NULL, 0, profilerWatcher, NULL, 0, NULL);
    atexit(dumpSampleProfile);
}

namespace AntRuntime {

/* Defined with the module scheduler (further down); stop()/destroyContext()
 * need them before that point. */
static void clearModuleScheduling(RuntimeState* state);
static void destroyModuleCache(RuntimeState* state);

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
 * The engine names itself; the realm it is in decides which document the
 * callback belongs to (design-context-ownership.md §4). */
static void contextActivate(AntbEngine* engine)
{
    RuntimeState* state;

    assertContextThread("contextActivate");

    if (!engine)
        return;

    state = stateForEngine(engine);

    if (!state)
        return;

    if (g_currentState != state)
        activateState(state);

    /* ant switches realms per function call without telling the host: a
     * callback of one realm must see that realm's document. The document that
     * was current on entry is saved for contextLeave(): ant switched to the
     * *callee's* realm, which for an engine-scoped class method is the realm
     * that created the class, not the realm the calling script runs in. */
    if (perFrameRealmEnabled()) {
        AntbRealm* realm = antb_realm_current(engine);

        if (!state->bindStack)
            state->bindStack = new std::vector<DocumentRuntime*>();

        state->bindStack->push_back(g_currentDocument);
        applyRealmBinding(realm);
    }
}

/* Runs after that callback returns (antb_set_context_leave). Puts the
 * document binding back to the one the *calling* script had: without this a
 * child frame that called one node method (a method of the main realm's class)
 * left the host mirror on the parent document until the next callback - the
 * window in which document-scoped host functions (setTimeout, fetch, new
 * Image) would land on the parent document (R148 / roadmap P1-aa). */
static void contextLeave(AntbEngine* engine)
{
    RuntimeState* state;
    DocumentRuntime* previous;

    assertContextThread("contextLeave");

    if (!engine)
        return;

    state = stateForEngine(engine);

    if (!state || !state->bindStack || state->bindStack->empty())
        return;

    previous = state->bindStack->back();
    state->bindStack->pop_back();

    /* The document may have been torn down inside the callback (a navigation
     * from a handler): only restore a record that still belongs to the view. */
    if (!previous)
        return;

    if (previous == &g_emptyDocument) {
        activateDocument(NULL);
        return;
    }

    if (documentRuntimeOf(state, previous->document) == previous)
        activateDocument(previous);
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
    unsigned long documentGeneration; /* the document that started it */
    unsigned long serial;       /* identity: a client may only write to the entry it was started for */

    ModuleEntry() : done(false), failed(false), id(0), generation(0),
                    documentGeneration(0), serial(0) {}
};

/* Module scheduling is per view (RuntimeState::module*): the same URL fetched
 * by two tabs is two entries in two tables, and a teardown only cancels the
 * entries of its own view. `g_moduleSerial` stays process wide (it is an
 * identity, not ownership). */
#define g_moduleCache \
    (*(std::map<std::string, ModuleEntry*>**)&g_currentState->moduleCache)
#define g_moduleSession \
    (*(Net::Session**)&g_currentState->moduleSession)
/* An import waiting for a fetch slot. Tagged with the view (whole-realm
 * teardown) and with the document (a frame going away cancels only its own). */
struct PendingModule {
    std::string   url;
    unsigned long generation;
    unsigned long documentGeneration;

    PendingModule(const std::string& u, unsigned long g, unsigned long d)
        : url(u), generation(g), documentGeneration(d) {}
};

#define g_modulePending \
    (*(std::vector<PendingModule>**)&g_currentState->modulePending)
#define g_moduleInFlight (g_currentState->moduleInFlight)

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
 * runs in the parse flow - never from inside a network pump. Per view: an
 * entry of one tab arriving while another is the current view used to be
 * dropped as "stale" (its generation belonged to the other view). */
/* A routed <script type="module" src> entry: which realm and which document
 * asked for it (the document id lets a frame teardown drop exactly its own). */
struct EntryScriptStamp {
    unsigned long generation;
    unsigned long documentGeneration;
    void* owner;                /* the tokenizer that is blocked on it */
};

#define g_moduleEntryScripts \
    (*(std::map<std::string, EntryScriptStamp>**)&g_currentState->moduleEntryScripts)
static ModuleEntryNotifyHook g_moduleEntryNotifyHook = NULL;

void setModuleEntryNotifyHook(ModuleEntryNotifyHook hook)
{
    g_moduleEntryNotifyHook = hook;
}

static void notifyModuleEntry(const std::string& url, const std::string* source, bool failed,
                              void* owner)
{
    if (!g_moduleEntryNotifyHook)
        return;

    g_moduleEntryNotifyHook(url.c_str(),
                            (source && !source->empty() && !failed) ? source->c_str() : NULL,
                            failed ? 1 : 0, owner);
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
        g_modulePending = new std::vector<PendingModule>();

    for (size_t i = 0; i < g_modulePending->size(); ++i) {
        if ((*g_modulePending)[i].url == url)
            return;
    }

    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[modpending] %s gen=%lu docgen=%lu\n", url.c_str(),
                generation, currentDocumentGeneration());

    g_modulePending->push_back(PendingModule(url, generation, currentDocumentGeneration()));
}

static void moduleDrainPending(void)
{
    while (g_modulePending && !g_modulePending->empty()
            && g_moduleInFlight < moduleFetchLimit()) {
        std::string url = g_modulePending->front().url;

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
        std::vector<PendingModule>::iterator it = g_modulePending->begin();

        while (it != g_modulePending->end()) {
            if (it->generation <= generation)
                it = g_modulePending->erase(it);
            else
                ++it;
        }
    }
}

/* S2: a document going away (frame detached/navigated) drops the module work it
 * started - downloads, queued imports and routed entry scripts - matched by the
 * document id. The body cache is shared inside the view and stays. */
static void cancelModuleWorkForDocument(unsigned long documentGeneration)
{
    if (!documentGeneration)
        return;

    if (g_moduleCache) {
        for (std::map<std::string, ModuleEntry*>::iterator it = g_moduleCache->begin();
             it != g_moduleCache->end(); ++it) {
            ModuleEntry* entry = it->second;

            if (!entry || entry->documentGeneration != documentGeneration)
                continue;

            if (entry->id) {
                if (g_moduleSession)
                    g_moduleSession->cancel(entry->id);

                entry->id = 0;

                if (g_moduleInFlight > 0)
                    --g_moduleInFlight;
            }

            if (!entry->done) {
                entry->done = true;
                entry->failed = true;
            }
        }
    }

    if (g_modulePending) {
        std::vector<PendingModule>::iterator it = g_modulePending->begin();

        while (it != g_modulePending->end()) {
            if (it->documentGeneration == documentGeneration)
                it = g_modulePending->erase(it);
            else
                ++it;
        }
    }

    if (g_moduleEntryScripts) {
        std::map<std::string, EntryScriptStamp>::iterator it = g_moduleEntryScripts->begin();

        while (it != g_moduleEntryScripts->end()) {
            if (it->second.documentGeneration == documentGeneration)
                it = g_moduleEntryScripts->erase(it);
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
    void* owner;   /* the tokenizer waiting for it */
};

#define g_arrivedEntryBodies \
    (*(std::vector<ArrivedEntryBody>**)&g_currentState->arrivedEntryBodies)

/* A routed entry script finished (or failed): deposit the result for the
 * frame loop, or report the failure. Dropped when another realm owns the
 * page now. */
static void moduleEntryScriptArrived(const std::string& url, const ModuleEntry* entry)
{
    std::map<std::string, EntryScriptStamp>::iterator it;
    EntryScriptStamp stamp;

    if (!g_moduleEntryScripts)
        return;

    it = g_moduleEntryScripts->find(url);

    if (it == g_moduleEntryScripts->end())
        return;

    stamp = it->second;
    g_moduleEntryScripts->erase(it);

    if (currentGeneration() != stamp.generation) {
        if (getenv("AGB_MODULE_DEBUG"))
            fprintf(stderr, "[modentry] stale %s (gen %lu)\n", url.c_str(), stamp.generation);
        return;
    }

    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[modentry] arrived %s gen=%lu docgen=%lu failed=%d\n",
                url.c_str(), stamp.generation, stamp.documentGeneration,
                (entry && entry->failed) ? 1 : 0);

    if (entry && !entry->failed) {
        ArrivedEntryBody body;

        body.url = url;
        body.source = entry->body;
        body.failed = false;
        body.owner = stamp.owner;

        if (!g_arrivedEntryBodies)
            g_arrivedEntryBodies = new std::vector<ArrivedEntryBody>();

        g_arrivedEntryBodies->push_back(body);
    } else {
        ArrivedEntryBody body;

        body.url = url;
        body.failed = true;
        body.owner = stamp.owner;

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

    /* The download belongs to the realm and the document that ask for it: when
     * either is gone, the download is cancelled with it. */
    entry->generation = currentGeneration();
    entry->documentGeneration = currentDocumentGeneration();

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
        /* 0 = the network layer's default (the system's budgets). The 5 s
         * literal here was the reason slow module graphs "failed" while their
         * requests were still downloading. */
        request.timeoutMs = 0;

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
void fetchAndQueueModuleScript(const char* url, void* owner)
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
        notifyModuleEntry(u, &entry->body, entry->failed, owner);
        return;
    }

    if (!g_moduleEntryScripts)
        g_moduleEntryScripts = new std::map<std::string, EntryScriptStamp>();

    {
        EntryScriptStamp stamp;

        stamp.generation = currentGeneration();
        stamp.documentGeneration = currentDocumentGeneration();
        stamp.owner = owner;
        (*g_moduleEntryScripts)[u] = stamp;
    }
    moduleStart(u, false);

    /* file:// entries (and offline-gated ones) complete synchronously inside
     * moduleStart: notify right away. The caller guards reentrancy, and this
     * still runs in the parse flow. */
    entry = moduleEntryFor(u);

    if (entry->done)
        notifyModuleEntry(u, &entry->body, entry->failed, owner);
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

/* How long the tokenizer may block on the engine thread waiting for one module
 * body. It has to be at least the network budget, otherwise a module that is
 * merely slow "fails" while its request is still in flight (the old hard 5 s
 * did that). 0 network timeout = the system default, whose receive budget is
 * 30 s; AGB_MODULE_WAIT_MS overrides for tests and for very slow links. */
static int moduleWaitMs()
{
    const char* env = getenv("AGB_MODULE_WAIT_MS");

    if (env && atoi(env) > 0)
        return atoi(env);

    {
        int net = Net::netDefaultTimeoutMs();

        return net > 0 ? net : 30000;
    }
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

        while (!entry->done && GetTickCount() - began < moduleWaitMs()) {
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

static void antbLogSink(const char* line, void*);

bool start()
{
    startSampler();
    AntbEngine* engine;

    if (g_engine)
        return true;

    initCookieStorage();
    antb_set_logger(antbLogSink, 0);
    /* Legacy codecs (gbk/big5/shift_jis) come from WebCore's tables: ant has
     * none and asks the host through this hook. Must be registered before the
     * first engine, because TextDecoder resolves labels at construction. */
    Agb::installTextCodecProvider();
    /* Ant needs the address of a stack variable from the calling thread. */
    volatile char stack_anchor = 0;
    engine = antb_engine_create((const void*)&stack_anchor);

    if (!engine)
        return false;

    activateState(createRuntimeState(engine));
    antb_set_context_activate(contextActivate);
    antb_set_context_leave(contextLeave);
    antb_set_module_fetcher(moduleFetch);

    /* Unhandled promise rejections must be logged, not fatal: one bad page
     * promise used to call process_exit_with(EXIT_FAILURE) and close the browser. */
    antb_set_uncaught_nonfatal(g_engine, 1);

    return true;
}

/* antjsb logs through this callback: the bridge lives in the ant build and
 * must not include agave's AgbLog.h. */
static void antbLogSink(const char* line, void*)
{
    agbLog(AGB_LOG_FLAG, "%s", line ? line : "");
}

void* createContext()
{
    volatile char stack_anchor = 0;
    antb_set_logger(antbLogSink, 0);
    /* Process-wide and idempotent; both engine-creation paths install it. */
    Agb::installTextCodecProvider();
    AntbEngine* engine = antb_engine_create((const void*)&stack_anchor);

    if (!engine)
        return NULL;

    if (!g_currentState || g_currentState == &g_emptyState) {
        /* First context in the process: the prelude and the bridge hooks are
         * installed lazily by bindDocument(), but the dispatch funnel must be
         * registered before any script runs. */
        antb_set_context_activate(contextActivate);
        antb_set_context_leave(contextLeave);
        activateState(createRuntimeState(engine));
        agbLog(AGB_LOG_EVT, "realm start gen=%lu engine=%p (view context)",
               g_currentState ? g_currentState->generation : 0UL, (void*)engine);
        return g_currentState;
    }

    {
        RuntimeState* state = createRuntimeState(engine);

        agbLog(AGB_LOG_EVT, "realm start gen=%lu engine=%p (view context)",
               state->generation, (void*)engine);
        return state;
    }
}

/* Defined next to stopRealm(); declared early because destroyContext() (view
 * teardown) has to run the same realm cleanup. */
static void teardownRealm(RuntimeState* state);

void destroyContext(void* context)
{
    RuntimeState* state = (RuntimeState*)context;

    assertContextThread("destroyContext");

    if (!state || state == &g_emptyState)
        return;

    /* The view is going away. macross_view_destroy() normally ran the document
     * teardown first (documentWillUnload: pagehide/unload + stopRealm); this is
     * the fallback for a view that never bound a document, and the one place
     * that releases everything left. Running it twice would only repeat the
     * log line, so key it off the VM: a stopped realm has engine == 0. */
    if (state->engine)
        teardownRealm(state);

    /* Safety net: a view torn down without a realm (or a failed teardown) must
     * not leak its document records or realm handlers. */
    releaseAllRealmListeners(state);
    forgetDocuments(state, false);
    destroyModuleCache(state);

    /* No successor realm will release this generation's pins: the view is
     * gone, so release them now. */
    releasePinnedNodes(state->generation);
    state->pinsPendingRelease = 0;

    {
        std::vector<RuntimeState*>& states = runtimeStates();

        for (size_t i = 0; i < states.size(); ++i) {
            if (states[i] == state) {
                states.erase(states.begin() + i);
                break;
            }
        }
    }

    if (g_currentState == state)
        activateState(&g_emptyState);

    delete state->bindStack;
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

/* Defined with the module scheduler (further down); stop()/destroyContext()
 * need them before that point. */
static void clearModuleScheduling(RuntimeState* state);
static void destroyModuleCache(RuntimeState* state);

void stop()
{
    std::vector<RuntimeState*>& states = runtimeStates();

    for (size_t i = 0; i < states.size(); ++i) {
        RuntimeState* state = states[i];

        releaseAllRealmListeners(state);
        forgetDocuments(state, true);
        destroyModuleCache(state);

        if (state->engine)
            antb_engine_destroy(state->engine);

        delete state;
    }

    states.clear();
    activateState(&g_emptyState);
}

bool isRunning()
{
    return g_engine != NULL;
}


/* Nesting depth of script evaluation: if it is > 1 while the load event is
 * dispatched, the VM is re-entering and that is worth knowing. */
static int g_evalDepth = 0;

/* Page scripts are classic <script>: top-level var/function belong to the
 * global object. antb_eval_script() implements that (R66); it is the default
 * since R67, when the listener-rooting UAF it exposed was fixed.
 * AGB_PAGE_EVAL=eval keeps the old direct-eval semantics for A/B work. */
static bool pageEvalScriptMode()
{
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("AGB_PAGE_EVAL");
        cached = (v && !strcmp(v, "eval")) ? 0 : 1;
    }
    return cached == 1;
}

/* Realms whose global already got the host API (__agb*, window methods). */
static std::set<AntbRealm*>* g_apiRealms = NULL;

static void installGlobalApi(AntbEngine* engine, AntbValue* global);

/* Every realm needs the host API. bindDocument() installs it, but a realm can
 * also become current without a bind - e.g. ant switches into a closure's realm
 * when a value from another frame is called - so the evaluation funnel makes
 * sure the realm it is about to run code in has the API. Without this, a page
 * script could run its prelude-defined `fetch` in a realm that never got
 * `__agbHttpRequestEx` (ReferenceError inside prelude's fetch). */
static void ensureGlobalApi(void)
{
    AntbRealm* realm;
    AntbValue* global;

    if (!g_engine || !perFrameRealmEnabled())
        return;

    realm = antb_realm_current(g_engine);
    if (!realm)
        return;

    if (!g_apiRealms)
        g_apiRealms = new std::set<AntbRealm*>();
    if (g_apiRealms->count(realm))
        return;

    global = antb_global(g_engine);
    if (!global)
        return;

    installGlobalApi(g_engine, global);
    antb_value_release(global);
    g_apiRealms->insert(realm);

    agbLog(AGB_LOG_EVT, "late global api install for realm %p", (void*)realm);
}

static AntbValue* antbEvalPage(const char* src, int len, char** err)
{    ensureGlobalApi();

    return pageEvalScriptMode()
        ? antb_eval_script(g_engine, src, len, err)
        : antb_eval(g_engine, src, len, err);
}
int evaluate(const char* src, size_t len, char** out, char** err)
{
    AntbValue* value;

    assertContextThread("evaluate");
    struct DepthGuard {
        int* depth;
        explicit DepthGuard(int* d) : depth(d) { ++*d; }
        ~DepthGuard() {
            if (getenv("AGB_EVT_DEBUG"))
                agbLog(AGB_LOG_EVT, "evaluate leave depth=%d", *depth);
            --*depth;
        }
    } guard(&g_evalDepth);

    if (getenv("AGB_EVT_DEBUG"))
        agbLog(AGB_LOG_EVT, "evaluate enter depth=%d len=%d", g_evalDepth, (int)len);

    if (out)
        *out = NULL;
    if (err)
        *err = NULL;

    if (!g_engine || !src)
        return 0;

    /* Which scripts are modules is the parser's decision, not the source's:
     * HTMLTokenizer::scriptExecution() and HTMLScriptElement::isModuleScript()
     * know the tag's type and route module scripts to evaluateModule()/
     * queueModule() before they get here. This entry is the classic path, and
     * sniffing the body for `import`/`export` here misclassified a classic
     * script whose first token was `import(` (valid dynamic import) as a
     * module. */

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
                value = antbEvalPage(src, (int)len, err);
                if (!value)
                    return 0;
                if (out)
                    *out = antb_to_utf8(value);
                antb_value_release(value);
                return 1;
            }

            value = antbEvalPage(wrapped, written, err);
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
    /* Opt-in breakdown: a slow frame is usually attributed to "the network
     * pump" but the time can be XHR callbacks, module deliveries or the VM's
     * own timers. AGB_PUMP_BREAKDOWN=1 prints each leg when it exceeds 2 ms
     * so the culprit is visible on the same timeline as [perf] update js=. */
    static int breakdown = -1;
    LARGE_INTEGER bdFreq, bdT0, bdT1, bdT2, bdT3, bdT4;

    assertContextThread("pumpAsyncRequests");

    memset(&bdFreq, 0, sizeof(bdFreq));
    memset(&bdT0, 0, sizeof(bdT0));
    memset(&bdT1, 0, sizeof(bdT1));
    memset(&bdT2, 0, sizeof(bdT2));
    memset(&bdT3, 0, sizeof(bdT3));
    memset(&bdT4, 0, sizeof(bdT4));

    if (breakdown < 0)
        breakdown = getenv("AGB_PUMP_BREAKDOWN") ? 1 : 0;
    if (breakdown) {
        QueryPerformanceFrequency(&bdFreq);
        QueryPerformanceCounter(&bdT0);
    }

    /* Deliver finished requests first (JS callbacks), then let the engine run
     * its own timers/microtasks - both happen on the frame loop, never inside
     * the network pump. */
    collectXhrJobs();
    if (breakdown) QueryPerformanceCounter(&bdT1);

    /* External media decoders (ffprobe / ffmpeg) finish on their own threads;
     * this only moves their results into the table the media elements poll
     * from their WebCore timers, so no WebCore object is touched off-thread. */
    WebCore::MediaDecoder::pump();

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
    if (breakdown) QueryPerformanceCounter(&bdT2);

    if (g_arrivedEntryBodies && !g_arrivedEntryBodies->empty()) {
        std::vector<ArrivedEntryBody> arrived;

        arrived.swap(*g_arrivedEntryBodies);

        for (size_t i = 0; i < arrived.size(); ++i)
            notifyModuleEntry(arrived[i].url, arrived[i].failed ? NULL : &arrived[i].source,
                              arrived[i].failed, arrived[i].owner);
    }

    if (g_engine) {
        if (breakdown) QueryPerformanceCounter(&bdT3);
        antb_pump(g_engine);
        if (breakdown) QueryPerformanceCounter(&bdT4);

        /* Timer callbacks are outside evaluate(), so report and clear anything
         * they threw instead of leaving it pending in the engine. */
        if (char* pending = antb_take_pending_error(g_engine)) {
            fprintf(stderr, "[antjs] (timer) %s\n", pending);
            free(pending);
        }
    }

    if (breakdown) {
        double msXhr = (double)(bdT1.QuadPart - bdT0.QuadPart) * 1000.0 / (double)bdFreq.QuadPart;
        double msMod = (double)(bdT2.QuadPart - bdT1.QuadPart) * 1000.0 / (double)bdFreq.QuadPart;
        double msArr = (double)(bdT3.QuadPart - bdT2.QuadPart) * 1000.0 / (double)bdFreq.QuadPart;
        double msVm  = (double)(bdT4.QuadPart - bdT3.QuadPart) * 1000.0 / (double)bdFreq.QuadPart;

        if (msXhr + msMod + msArr + msVm > 2.0)
            fprintf(stderr, "[pumpbd] xhr=%.0fms module=%.0fms arrived=%.0fms vm=%.0fms\n",
                    msXhr, msMod, msArr, msVm);
    }
}

/* The engine only joins module specifiers against http(s) URL bases
 * (esm_is_url), so file:// URLs are masqueraded as "http://file.ant/<path>"
 * when handed to it (see the masquerade note by kEngineFileUrlPrefix below). */
static bool isFileUrl(const std::string& u);
static std::string engineUrlForFile(const std::string& fileUrl);

void setFilename(const char* name)
{
    if (name)
        g_currentFilename = name;

    if (g_engine && name) {
        /* The engine's view of the script URL: file: scripts must resolve
         * `import()` and relative imports in the masqueraded space too, not
         * only module entries (which already translate at evaluateModule). */
        if (isFileUrl(name)) {
            std::string masqueraded = engineUrlForFile(name);

            antb_set_filename(g_engine, masqueraded.c_str());
        } else {
            antb_set_filename(g_engine, name);
        }
    }
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
    unsigned long documentGeneration; /* the document that queued it */
    void* document;             /* the document it belongs to (bound before it
                                 * runs: the bridge may still point at the
                                 * frame's initial about:blank document) */
    RefPtr<WebCore::Element> scriptElement; /* classic defer: what
                                 * document.currentScript points at while it
                                 * runs. Held by reference: the queue can outlive
                                 * a script element the page removed. */
    bool isClassic;             /* classic <script defer> runs via evaluate(),
                                 * module scripts via evaluateModule() */
    DeferredModule() : generation(0), documentGeneration(0), document(NULL), isClassic(false) {}
};

/* Per view (RuntimeState::deferredModules): a tab's queued defer/module
 * scripts must never run in another tab's realm. */
#define g_deferredModules \
    (*(std::vector<DeferredModule>**)&g_currentState->deferredModules)

/* Set after the post-parse flush ran for a document. A defer script that
 * arrives after that point (slow network, dynamically inserted) must still run,
 * so it executes immediately instead of sitting in a queue that is never
 * flushed. Keyed by document, not by generation: the frame's initial empty
 * document shares the realm generation with the page that replaces it. */
#define g_lastFlushDocument \
    (*(void**)&g_currentState->lastFlushDocument)

/* The view's realm is going away: drop everything its scheduling tables held
 * for that realm (in-flight requests were cancelled by generation just before
 * this call). The module body cache is kept: it is keyed by URL and a reload
 * of the same view can still use the bodies. */
static void clearModuleScheduling(RuntimeState* state)
{
    if (!state || state == &g_emptyState)
        return;

    delete *(std::vector<DeferredModule>**)&state->deferredModules;
    state->deferredModules = NULL;
    state->lastFlushDocument = NULL;

    delete *(std::vector<std::pair<std::string, unsigned long> >**)&state->modulePending;
    state->modulePending = NULL;

    delete *(std::map<std::string, unsigned long>**)&state->moduleEntryScripts;
    state->moduleEntryScripts = NULL;

    delete *(std::vector<ArrivedEntryBody>**)&state->arrivedEntryBodies;
    state->arrivedEntryBodies = NULL;

    state->moduleInFlight = 0;
}

/* The view is gone: module bodies, the fetch session and its worker threads. */
static void destroyModuleCache(RuntimeState* state)
{
    Net::Session* session;

    if (!state || state == &g_emptyState)
        return;

    clearModuleScheduling(state);

    {
        std::map<std::string, ModuleEntry*>* cache =
            *(std::map<std::string, ModuleEntry*>**)&state->moduleCache;

        if (cache) {
            for (std::map<std::string, ModuleEntry*>::iterator it = cache->begin();
                 it != cache->end(); ++it)
                delete it->second;

            delete cache;
            state->moduleCache = NULL;
        }
    }

    session = *(Net::Session**)&state->moduleSession;

    if (session) {
        session->shutdown();
        delete session;
        state->moduleSession = NULL;
    }
}

int evaluateModule(const char* src, size_t len, const char* filename, char** out, char** err);

/* Starts downloading the import graph of a module script as soon as its body is
 * available, without waiting for the deferred execution. `isModule` comes from
 * the parser (the tokenizer knows the tag's type): scanning a classic script for
 * `from './x'` matched arbitrary string literals and flooded the connection
 * pool with bogus requests, starving the scripts the parser was waiting for. */
void prefetchModuleGraph(const char* url, const char* source, int isModule)
{
    if (!url || !source || !isModule)
        return;

    modulePrefetchImports(std::string(url), std::string(source));
}

void queueModule(const char* url, const char* source, void* document)
{
    if (!g_deferredModules)
        g_deferredModules = new std::vector<DeferredModule>();

    /* A module script the element path queues after the post-parse flush has
     * run would sit in this queue forever: flushDeferredModules is not called
     * again. Run it now, the way a late <script defer> is handled (the defer
     * path takes the same shortcut). Dynamic module scripts are async by
     * default in the HTML spec, so immediate evaluation matches. */
    if (g_lastFlushDocument && g_lastFlushDocument == document) {
        char* err = NULL;

        agbLog(AGB_LOG_PARSE, "script (module) running now (parse already finished) url=%s len=%u",
               url ? url : "", source ? (unsigned)strlen(source) : 0u);
        setFilename(url ? url : "");
        evaluateModule(source ? source : "", source ? strlen(source) : 0,
                       url ? url : "", NULL, &err);
        if (err) {
            fprintf(stderr, "[antjs] %s: %s\n", url ? url : "", err);
            free(err);
        }
        return;
    }

    DeferredModule module;

    module.url = url ? url : "";
    module.source = source ? source : "";
    module.generation = currentGeneration();
    module.documentGeneration = currentDocumentGeneration();
    module.document = document ? document : g_document;
    module.isClassic = false;
    g_deferredModules->push_back(module);

    agbLog(AGB_LOG_PARSE, "script (module) queued for parse end url=%s len=%u",
           url ? url : "", source ? (unsigned)strlen(source) : 0u);

    /* Start downloading the import graph now, while the rest of the page is still
     * loading, instead of waiting for the deferred flush; Chrome starts as soon
     * as it sees the script tag. */
    modulePrefetchImports(module.url, module.source);
}

void flushDeferredModules(void* document)
{
    std::vector<DeferredModule> pending;

    /* The initial empty document of a frame reaches DOMContentLoaded too; it
     * has no body and no deferred work, so it must not seal the queue for the
     * page that replaces it (both share one realm generation). A real document
     * whose content is all head-level (a page that is only <script
     * type="module">) has no body yet at this point - that must still flush. */
    {
        bool hasQueued = g_deferredModules && !g_deferredModules->empty();

        if (!document || (!((WebCore::Document*)document)->body() && !hasQueued)) {
            if (getenv("AGB_MODULE_DEBUG"))
                fprintf(stderr, "[flush] skip doc=%p body=%d queued=%d\n", document,
                        document ? (((WebCore::Document*)document)->body() ? 1 : 0) : -1,
                        g_deferredModules ? (int)g_deferredModules->size() : 0);
            return;
        }
    }

    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[flush] doc=%p body=1 queued=%d gen=%lu docgen=%lu\n",
                document, g_deferredModules ? (int)g_deferredModules->size() : 0,
                currentGeneration(), currentDocumentGeneration());

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

            agbLog(AGB_LOG_PARSE, "deferred classic script running url=%s len=%u",
                   pending[i].url.c_str(), (unsigned)pending[i].source.size());
            setFilename(pending[i].url.c_str());
            {
                WebCore::CurrentScriptScope currentScript((WebCore::Document*)pending[i].document,
                                                          pending[i].scriptElement.get());
                evaluate(pending[i].source.c_str(), pending[i].source.size(), &out, &err);
            }
            if (out)
                free(out);
        } else {
            agbLog(AGB_LOG_PARSE, "deferred module running url=%s len=%u",
                   pending[i].url.c_str(), (unsigned)pending[i].source.size());
            evaluateModule(pending[i].source.c_str(), pending[i].source.size(),
                           pending[i].url.c_str(), NULL, &err);
        }

        if (err) {
            fprintf(stderr, "[antjs] %s: %s\n", pending[i].url.c_str(), err);
            free(err);
        }
    }
}

void queueDeferredModule(void* document, const char* url, const char* source)
{
    /* The post-parse flush already ran for this document: execute now, like a
     * defer script that arrives late. The log marks which of the two happened
     * - marking "queued" before the decision is made lies on the timeline. */
    if (g_lastFlushDocument && g_lastFlushDocument == document) {
        char* err = NULL;

        if (document && document != g_document)
            bindDocument(document);

        agbLog(AGB_LOG_PARSE, "script (module) running now (parse already finished) url=%s len=%u",
               url ? url : "", source ? (unsigned)strlen(source) : 0u);
        setFilename(url ? url : "");
        evaluateModule(source ? source : "", source ? strlen(source) : 0,
                       url ? url : "", NULL, &err);
        if (err) {
            fprintf(stderr, "[antjs] %s: %s\n", url ? url : "", err);
            free(err);
        }
        return;
    }

    if (!g_deferredModules)
        g_deferredModules = new std::vector<DeferredModule>();

    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[queue] doc=%p state=%p gen=%lu docgen=%lu\n", document,
                (void*)g_currentState, currentGeneration(), currentDocumentGeneration());

    DeferredModule module;

    module.url = url ? url : "";
    module.source = source ? source : "";
    module.generation = currentGeneration();
    module.documentGeneration = currentDocumentGeneration();
    module.document = document ? document : g_document;
    module.isClassic = false;
    g_deferredModules->push_back(module);

    agbLog(AGB_LOG_PARSE, "script (module) queued for parse end url=%s len=%u",
           url ? url : "", source ? (unsigned)strlen(source) : 0u);

    modulePrefetchImports(module.url, module.source);
}

void queueDeferredScript(void* document, const char* url, const char* source, void* scriptElement)
{
    WebCore::Element* element = (WebCore::Element*)scriptElement;

    /* The post-parse flush already ran for this document: nothing will consume
     * the queue anymore, so run the script right away (slow network, dynamic
     * insertion). Log at the decision point: at the caller the two outcomes
     * are not yet known, so a "queued for parse end" emitted there lies when
     * this branch takes over. */
    if (g_lastFlushDocument && g_lastFlushDocument == document) {
        char* out = NULL;
        char* err = NULL;

        if (document && document != g_document)
            bindDocument(document);

        agbLog(AGB_LOG_PARSE, "script (defer) running now (parse already finished) url=%s len=%u",
               url ? url : "", source ? (unsigned)strlen(source) : 0u);
        setFilename(url ? url : "");
        {
            WebCore::CurrentScriptScope currentScript((WebCore::Document*)document, element);
            evaluate(source ? source : "", source ? strlen(source) : 0, &out, &err);
        }
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
    script.documentGeneration = currentDocumentGeneration();
    script.document = document;
    script.scriptElement = element;
    script.isClassic = true;
    g_deferredModules->push_back(script);

    agbLog(AGB_LOG_PARSE, "script (defer) queued for parse end url=%s len=%u",
           url ? url : "", source ? (unsigned)strlen(source) : 0u);
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

/* S2: a document going away drops the defer/module scripts it queued (they hold
 * its document pointer and would run against a detached page). */
static void cancelDeferredModulesForDocument(unsigned long documentGeneration)
{
    if (!g_deferredModules || !documentGeneration)
        return;

    std::vector<DeferredModule>::iterator it = g_deferredModules->begin();

    while (it != g_deferredModules->end()) {
        if (it->documentGeneration == documentGeneration)
            it = g_deferredModules->erase(it);
        else
            ++it;
    }
}

/* Everything one document started, cancelled together (frame detached or
 * navigated away): its XHRs, its module downloads/queue entries and its
 * deferred scripts. Matched by the document id - the view generation still
 * covers whole-realm teardown. */
static void cancelDocumentResources(unsigned long documentGeneration)
{
    if (!documentGeneration)
        return;

    if (getenv("AGB_EVT_DEBUG"))
        agbLog(AGB_LOG_EVT, "cancel document resources docgen=%lu", documentGeneration);

    cancelXhrJobsForDocument(documentGeneration);
    cancelModuleWorkForDocument(documentGeneration);
    cancelDeferredModulesForDocument(documentGeneration);
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
            AntbValue* result = g_engine ? antbEvalPage(buffer, (int)read, NULL) : NULL;

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
    AntbRealm* realm;

    if (!g_engine)
        return;

    realm = antb_realm_current(g_engine);
    if (!realm)
        return;

    if (!g_preludedRealms)
        g_preludedRealms = new std::set<AntbRealm*>();
    if (g_preludedRealms->count(realm))
        return;

    g_preludedRealms->insert(realm);

    if (getenv("AGB_EVT_DEBUG"))
        agbLog(AGB_LOG_EVT, "prelude.js loaded into realm %p", (void*)realm);

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

    {
        AntbRealm* realm = antb_realm_current(g_engine);

        if (!realm)
            return;

        if (!g_polyfilledRealms)
            g_polyfilledRealms = new std::set<AntbRealm*>();
        if (g_polyfilledRealms->count(realm))
            return;

        g_polyfilledRealms->insert(realm);
    }

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

            if (g_engine && !antbEvalPage(buffer, (int)read, NULL))
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
        std::map<std::string, EntryScriptStamp>::iterator it = g_moduleEntryScripts->begin();

        while (it != g_moduleEntryScripts->end()) {
            if (it->second.generation == generation) {
                notifyModuleEntry(it->first, NULL, true, it->second.owner);
                g_moduleEntryScripts->erase(it++);
            } else
                ++it;
        }
    }
}

/* Set while the outgoing document's teardown (pagehide, visibilitychange,
 * unload, realm stop) runs: the GetTickCount() of its start, 0 when idle.
 * Written on the engine thread, read by the control's watchdog thread (a
 * plain 32-bit aligned load/store), which uses it to budget the teardown: a
 * pagehide/unload handler that never returns must not keep the navigation
 * waiting. */
static volatile LONG g_teardownStart = 0;

/* Releases everything a realm owns: its in-flight resources, its pinned DOM
 * nodes, its wrappers, its CSSOM refs and finally the VM. The VM is destroyed,
 * so its globals, its module registry and its timers cannot be seen by the
 * next document.
 *
 * `state` need not be the active state: a tab close tears down a view whose
 * context is not the one the engine thread last activated. The g_* mirrors are
 * pointed at `state` for the duration and restored afterwards, so the body
 * below keeps operating on the right realm. Engine thread only. */
static void teardownRealm(RuntimeState* state)
{
    RuntimeState* outer = g_currentState;

    assertContextThread("teardownRealm");

    if (!state || state == &g_emptyState)
        return;

    /* Point the mirrors at the state that is going away (its last active
     * document comes with it), not at whatever the engine thread ran last. */
    activateState(state);

    agbLog(AGB_LOG_EVT, "realm stop gen=%lu url=%s",
           state->generation,
           (g_document && !g_document->URL().isEmpty()) ? g_document->URL().utf8().data() : "(none)");

    /* Stop the realm's resources first: a response that arrives later must
     * not be delivered into the realm that replaces this one. */
    cancelInFlightResources(state->generation);

    /* The window handlers belong to the realms that are going away (their
     * closures and AntbValue refs die with the engine). */
    releaseAllRealmListeners(state);

    /* Queued/prefetched modules of this view are useless in the next realm
     * (in-flight ones were cancelled with the other resources below). */
    clearModuleScheduling(state);

    /* The nodes of the previous realm can be released now (their document
     * was replaced a generation ago). */
    if (state->pinsPendingRelease)
        releasePinnedNodes(state->pinsPendingRelease);

    state->pinsPendingRelease = state->generation;

    /* Values owned by the old engine have to go before the engine does: the
     * document records own them (docObj/listeners/CSSOM refs) and are dropped
     * by forgetDocuments() below, while the engine is still alive. */
    /* Location/History objects and their session history values belong to the
     * engine that just died. */
    AntRuntime::RealmDocument::releaseEngine(state->engine);

    /* The per-window proxies of this realm die with it too. */
    if (g_windowCache) {
        for (size_t i = 0; i < g_windowCache->size(); ) {
            if ((*g_windowCache)[i].engine == state->engine) {
                if ((*g_windowCache)[i].object)
                    antb_value_release((*g_windowCache)[i].object);
                g_windowCache->erase(g_windowCache->begin() + i);
            } else {
                ++i;
            }
        }
    }
    /* The per-document objects of this realm die with the realm. */
    if (g_docObjectCache) {
        for (size_t i = 0; i < g_docObjectCache->size(); ) {
            if ((*g_docObjectCache)[i].engine == state->engine) {
                if ((*g_docObjectCache)[i].object)
                    antb_value_release((*g_docObjectCache)[i].object);
                g_docObjectCache->erase(g_docObjectCache->begin() + i);
            } else {
                ++i;
            }
        }
    }

    /* The old page's listeners close over its DOM; they are released together
     * with the document records (each document owns its own listeners now). */

    /* ...and the JS wrappers of its nodes must not be handed out to the next
     * document (the bridge keeps them in a process wide cache). */
    if (state->engine && g_document)
        antb_release_owner(state->engine, g_document);

    /* The node class belongs to the old engine. */
    g_nodeClass = 0;
    g_canvasCtxClass = 0;
    g_windowClass = 0;
    g_styleSheetListClass = 0;
    g_styleSheetClass = 0;
    g_cssRuleClass = 0;
    g_cssRuleListClass = 0;
    g_mediaListClass = 0;
    g_cssDeclarationClass = 0;
    clearExpandoObjects();
    state->realmBound = false;

    /* The slots point at documents that are going away: keeping them would
     * hand out pointers to freed documents. */
    for (int i = 0; i < g_docSlotCount; ++i) {
        g_docSlots[i] = 0;
        g_docSlotGeneration[i] = 0;
    }

    g_docSlotCount = 0;
    g_currentSlot = -1;

    /* Drop every document of this view (main + frames) while the engine that
     * owns their values is still alive. The saved bindings point at those
     * documents: drop them first, nothing may restore a dead record. */
    if (state->bindStack)
        state->bindStack->clear();

    forgetDocuments(state, true);

    if (state->engine) {
        antb_engine_destroy(state->engine);
        state->engine = 0;
    }

    /* The realms (and their prelude/polyfills markers) died with the engine. */
    state->mainRealm = NULL;
    forgetRealmScripts();

    if (g_apiRealms)
        g_apiRealms->clear();

    if (getenv("AGB_EVT_DEBUG"))
        agbLog(AGB_LOG_EVT, "realm stopped (generation=%lu)", state->generation);

    /* A teardown of a non-active state must not leave the mirrors pointing at
     * it: the engine thread may activate another view right after. When the
     * state *is* the active one it stays active with engine == 0, so
     * beginNewRealm() can rebuild it for the next document. */
    if (outer != state)
        g_currentState = outer;
}

/* Stops the current realm (the navigation path). */
static void stopRealm(void)
{
    teardownRealm(g_currentState);
}

/* Starts a fresh realm (a new VM) for the next document. Called before the new
 * document is parsed, so that its module graph is registered in the new realm
 * and not in the one that is going away. */
void beginNewRealm(void)
{
    RuntimeState* state = g_currentState;

    assertContextThread("beginNewRealm");

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

    state->mainRealm = state->engine ? antb_realm_current(state->engine) : NULL;
    /* The old engine (and with it every value of the old documents) is already
     * destroyed: only the records are dropped here. */
    forgetDocuments(state, false);

    /* The new document starts in whatever state the host last reported: a
     * background tab keeps loading hidden, a foreground one visible. */
    state->hidden = state->viewHidden;

    agbLog(AGB_LOG_EVT, "realm start gen=%lu engine=%p",
           state->generation, (void*)state->engine);
}

AntbValue* documentObjectFor(AntbEngine* engine, WebCore::Document* document)
{
    AntbValue* docObj;
    size_t i;

    if (!engine || !document)
        return NULL;

    if (g_docObjectCache) {
        for (i = 0; i < g_docObjectCache->size(); ++i) {
            DocObjectEntry& e = (*g_docObjectCache)[i];

            if (e.engine == engine && e.document == document)
                return e.object ? antb_retain(e.object) : NULL;
        }
    } else {
        g_docObjectCache = new std::vector<DocObjectEntry>();
    }

    docObj = antb_make_object(engine);
    if (!docObj)
        return NULL;

    antb_define_property_rw(engine, docObj, "title", titleGetter, titleSetter, NULL);
    antb_define_property_rw(engine, docObj, "cookie", titleGetter, titleSetter, NULL);

    antb_define_method(engine, docObj, "getElementById", docGetElementById, NULL);
    antb_define_method(engine, docObj, "createElement", docCreateElement, NULL);
    antb_define_method(engine, docObj, "createElementNS", docCreateElementNS, NULL);
    antb_define_method(engine, docObj, "createTextNode", docCreateTextNode, NULL);
    antb_define_method(engine, docObj, "createComment", docCreateComment, NULL);
    antb_define_method(engine, docObj, "createDocumentFragment", docCreateDocumentFragment, NULL);
    /* document.implementation: jQuery and others feature-test
     * document.implementation.createHTMLDocument(). */
    {
        AntbValue* implObj = antb_make_object(engine);
        if (implObj) {
            antb_define_method(engine, implObj, "createHTMLDocument", docCreateHTMLDocument, NULL);
            antb_define_method(engine, implObj, "hasFeature", docHasFeature, NULL);
            antb_set_prop(engine, docObj, "implementation", implObj);
            antb_value_release(implObj);
        }
    }
    antb_define_method(engine, docObj, "createHTMLDocument", docCreateHTMLDocument, NULL);
    antb_define_method(engine, docObj, "write", docWrite, NULL);
    antb_define_method(engine, docObj, "writeln", docWriteln, NULL);
    antb_define_method(engine, docObj, "addEventListener", nodeAddEventListener, NULL);
    antb_define_method(engine, docObj, "removeEventListener", nodeRemoveEventListener, NULL);
    antb_define_method(engine, docObj, "dispatchEvent", nodeDispatchEvent, NULL);
    /* The document is a plain object (not a Node wrapper), so the node
     * methods have to be installed on it explicitly. */
    antb_define_method(engine, docObj, "getElementsByTagName", nodeGetElementsByTagName, NULL);
    antb_define_method(engine, docObj, "getElementsByClassName", nodeGetElementsByClassName, NULL);
    antb_define_method(engine, docObj, "hasChildNodes", nodeHasChildNodes, NULL);
    antb_define_method(engine, docObj, "querySelector", nodeQuerySelector, NULL);
    antb_define_method(engine, docObj, "querySelectorAll", nodeQuerySelectorAll, NULL);
    antb_define_method(engine, docObj, "appendChild", nodeAppendChild, NULL);
    antb_define_method(engine, docObj, "removeChild", nodeRemoveChild, NULL);
    antb_define_method(engine, docObj, "insertBefore", nodeInsertBefore, NULL);
    antb_define_method(engine, docObj, "replaceChild", nodeReplaceChild, NULL);
    antb_define_method(engine, docObj, "getAttribute", nodeGetAttribute, NULL);
    antb_define_method(engine, docObj, "setAttribute", nodeSetAttribute, NULL);
    antb_define_method(engine, docObj, "hasAttribute", nodeHasAttribute, NULL);
    antb_define_method(engine, docObj, "cloneNode", nodeCloneNode, NULL);
    antb_define_method(engine, docObj, "contains", nodeContains, NULL);
    antb_define_method(engine, docObj, "importNode", nodeDocumentImportNode, NULL);
    antb_define_method(engine, docObj, "adoptNode", nodeDocumentAdoptNode, NULL);
    antb_define_method(engine, docObj, "elementFromPoint", nodeDocumentElementFromPoint, NULL);
    antb_define_method(engine, docObj, "hasFocus", nodeDocumentHasFocus, NULL);

    /* Document is a Node: let the node APIs work directly on it. */
    antb_set_native(docObj, document, AGB_TAG_NODE);

    {
        DocObjectEntry e;

        e.engine   = engine;
        e.document = document;
        e.object   = antb_retain(docObj);
        g_docObjectCache->push_back(e);
    }

    return docObj;
}

/* ---- per-frame window objects -------------------------------------------- *
 * Every browsing context has its own Window object. Sharing one global between
 * frames is what made `parent !== self` false, gave `window.frames`/`length`/
 * `name`/`frameElement` nothing to say, and let a child frame's globals leak
 * into the parent. The proxy is a native object holding the frame's Document:
 * the frame-specific properties are answered from it and everything else is
 * delegated to the shared global object, so `parent.jQuery`, polyfill hooks and
 * cross-frame calls keep working (see docs/design-frame-window.md). */

/* AGB_TAG_WINDOW lives in RealmTags.h (shared with RealmDocument). */

static WebCore::Frame* frameOfDocument(WebCore::Document* document)
{
    return document ? document->frame() : 0;
}

static int childFrameCount(WebCore::Document* document)
{
    WebCore::Frame* frame = frameOfDocument(document);

    return (frame && frame->tree()) ? (int)frame->tree()->childCount() : 0;
}

static int isFrameIndex(const char* name)
{
    if (!name || !name[0])
        return 0;
    for (; *name; ++name) {
        if (*name < '0' || *name > '9')
            return 0;
    }
    return 1;
}

/* The window object of a document: the shared global for a top level document
 * (so `window === self === top` still holds there), a proxy for a frame. */
AntbValue* windowForDocument(AntbEngine* engine, WebCore::Document* document)
{
    if (!engine || !document)
        return NULL;

    if (!document->parentDocument())
        return antb_global(engine);

    return windowObjectFor(engine, document);
}

/* The window of a document as seen from ANY realm. With per-frame realms a
 * frame's window is that realm's global object; windowForDocument() would build
 * a legacy proxy for it (a different object than the frame's `window`, and one
 * that answers for the *reading* realm), so `frames[0] === kid.contentWindow`
 * was false and `kid.top` belonged to the wrong realm. */
AntbValue* realmWindowFor(AntbEngine* engine, WebCore::Document* document)
{
    if (!engine || !document)
        return NULL;

    if (perFrameRealmEnabled()) {
        RuntimeState* state = stateForEngine(engine);
        DocumentRuntime* runtime = documentRuntimeOf(state, document);
        AntbRealm* realm = NULL;

        if (document->parentDocument()) {
            if (runtime)
                realm = runtime->realm;
        } else if (state && state != &g_emptyState) {
            realm = state->mainRealm;
        }

        if (realm)
            return antb_realm_global(engine, realm);
    }

    return windowForDocument(engine, document);
}

/* A child frame reachable as window[name]: its browsing context name first, then
 * the id/name attribute of a frame owner element (an iframe created from script
 * with only an id is still `window.fr`, as in HTML's window named properties). */
static WebCore::Frame* childFrameNamed(WebCore::Document* document, const char* name)
{
    WebCore::Frame* frame = frameOfDocument(document);
    WebCore::Frame* child;

    if (!frame || !frame->tree() || !name || !name[0])
        return 0;

    child = frame->tree()->child(WebCore::AtomicString(name));
    if (child && child->document())
        return child;

    if (RefPtr<WebCore::Element> owner = document->getElementById(WebCore::AtomicString(name))) {
        if (owner->isFrameOwnerElement()) {
            WebCore::Document* content = static_cast<WebCore::HTMLFrameOwnerElement*>(owner.get())->contentDocument();
            if (content)
                return content->frame();
        }
    }

    return 0;
}

/* Passed as windowGetter's `user` when the getter is installed on a realm's
 * global object instead of a legacy frame proxy. Such a window IS the global, so
 * the fallback "ask the shared global" branch would call itself. */
static char s_windowGlobalMarker;

AntbValue* windowObjectFor(AntbEngine* engine, WebCore::Document* document)
{
    AntbValue* v;
    size_t i;

    if (!engine || !document)
        return NULL;

    if (g_windowCache) {
        for (i = 0; i < g_windowCache->size(); ++i) {
            WindowEntry& e = (*g_windowCache)[i];

            if (e.engine == engine && e.document == document)
                return e.object ? antb_retain(e.object) : NULL;
        }
    } else {
        g_windowCache = new std::vector<WindowEntry>();
    }

    if (!g_windowClass) {
        g_windowClass = antb_class_create(engine, "Window");
        if (!g_windowClass)
            return NULL;
    }

    v = antb_wrap_native_as_owner(engine, g_windowClass, document, AGB_TAG_WINDOW, document);
    if (!v)
        return NULL;

    /* Catch-all getter/setter for the instance: names the class does not list
     * are dispatched by name (the same scheme nodeGetter/nodeSetter use). */
    antb_define_property_rw(engine, v, "", windowGetter, windowSetter, NULL);

    {
        WindowEntry e;

        e.engine   = engine;
        e.document = document;
        e.object   = antb_retain(v);
        g_windowCache->push_back(e);
    }

    return v;
}

/* A window proxy answers for its own frame; everything else is the shared
 * global's, which keeps direct cross-frame property access working. The same
 * getter is installed on each realm's global object (windowGetter's `user`
 * says which), so `window[0]` / `window.frames["kid"]` work everywhere. */
int windowGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void* user)
{
    WebCore::Document* d = (WebCore::Document*)antb_get_native(self, AGB_TAG_WINDOW);
    int isRealmGlobal = (user == &s_windowGlobalMarker);
    AntbValue* v = NULL;

    if (!d || !name)
        return 0;

    if (!strcmp(name, "window") || !strcmp(name, "self") || !strcmp(name, "globalThis")
            || !strcmp(name, "frames") || !strcmp(name, "defaultView")) {
        antb_value_assign(out, self);
        return 1;
    }

    if (!strcmp(name, "parent") || !strcmp(name, "top")) {
        WebCore::Document* other;

        if (!strcmp(name, "parent")) {
            other = d->parentDocument();
        } else {
            other = d->topDocument();
            if (other == d)
                other = 0;
        }

        if (!other) {
            antb_value_assign(out, self);
            return 1;
        }
        v = realmWindowFor(engine, other);
    } else if (!strcmp(name, "document")) {
        v = documentObjectFor(engine, d);
    } else if (!strcmp(name, "location")) {
        v = AntRuntime::RealmDocument::locationObject(engine, d);
    } else if (!strcmp(name, "history")) {
        v = AntRuntime::RealmDocument::historyObject(engine, d);
    } else if (!strcmp(name, "length")) {
        v = antb_make_number(engine, (double)childFrameCount(d));
    } else if (!strcmp(name, "name")) {
        WebCore::Frame* frame = frameOfDocument(d);

        v = antb_make_string(engine, (frame && frame->tree())
                                       ? frame->tree()->name().domString().utf8().data() : "");
    } else if (!strcmp(name, "frameElement")) {
        WebCore::Element* owner = d->ownerElement();

        if (!owner)
            return 1;   /* undefined for a top level document */
        v = wrapNode(engine, owner);
    } else if (!strcmp(name, "innerWidth") || !strcmp(name, "outerWidth")
            || !strcmp(name, "innerHeight") || !strcmp(name, "outerHeight")) {
        /* window.innerWidth is the LAYOUT viewport: scrollbar included, which
         * is what 100vw and the @media width queries use.
         * documentElement.clientWidth (the old answer) is the content box and
         * is 16px narrower once a scrollbar shows up - a resize handler that
         * sized a full-width element from innerWidth came out short. */
        WebCore::FrameView* view = d->view();
        WebCore::RenderObject* renderer = d->renderer();
        int width = 0;
        int height = 0;

        if (view) {
            width = view->width();
            height = view->height();

            if (renderer) {
                width = WebCore::adjustForAbsoluteZoom(width, renderer);
                height = WebCore::adjustForAbsoluteZoom(height, renderer);
            }
        }

        v = antb_make_number(engine, (double)(strstr(name, "Width") ? width : height));
    } else {
        /* window[N] (child frame by position) and window["name"] (frame name or
         * the id/name of a frame owner element). */
        WebCore::Frame* frame = frameOfDocument(d);
        WebCore::Frame* child = isFrameIndex(name)
                              ? ((frame && frame->tree()) ? frame->tree()->child(atoi(name)) : 0)
                              : childFrameNamed(d, name);

        if (child && child->document()) {
            v = realmWindowFor(engine, child->document());
        } else if (!isRealmGlobal) {
            AntbValue* global = antb_global(engine);

            if (global) {
                v = antb_get_prop(engine, global, name);
                antb_value_release(global);
            }
        } else {
            /* A realm's global object: a miss here is a real miss, and asking
             * the global would re-enter this getter. Returning "not handled"
             * lets the engine continue to the prototype chain (Object.prototype
             * and co). */
            return 0;
        }
    }

    if (!v)
        return 1;   /* undefined */

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int windowSetter(AntbEngine* engine, AntbValue* self, const char* name, const AntbValue* value, void*)
{
    WebCore::Document* d = (WebCore::Document*)antb_get_native(self, AGB_TAG_WINDOW);

    if (!d || !name)
        return 0;

    if (!strcmp(name, "name")) {
        WebCore::Frame* frame = frameOfDocument(d);

        if (frame && frame->tree()) {
            char* s = antb_to_utf8(value);

            frame->tree()->setName(WebCore::AtomicString(s ? s : ""));
            antb_free(s);
        }
        return 1;
    }

    if (!strcmp(name, "location") || !strcmp(name, "href"))
        return AntRuntime::RealmDocument::setLocation(self, value);

    /* Everything else is a property of the shared global. */
    {
        AntbValue* global = antb_global(engine);

        if (global) {
            antb_set_prop(engine, global, name, value);
            antb_value_release(global);
        }
    }
    return 1;
}

/* Bare `document`/`window`/`parent`/... are resolved by ant's identifier
 * lookup, which does not run an exotic getter on the global object; they have
 * to be real properties. So the frame-specific ones are refreshed whenever a
 * document becomes the bound one (bindDocument / slot activation). */
/* RealmDocument needs the current-document provider once. It is also where the
 * Location/History objects get their per-document binding (R132). */
static WebCore::Document* boundDocumentForRealmDocument()
{
    return g_document;
}

static void ensureRealmDocumentContext()
{
    static bool done = false;

    if (done)
        return;
    done = true;

    AntRuntime::RealmDocument::setContext(perFrameRealmEnabled(),
                                          boundDocumentForRealmDocument);
    AntRuntime::RealmDocument::setPopstateDispatcher(dispatchWindowEventState);
}

/* Installs the JS-side global API on the *current realm's* global object:
 * canvas class, window methods, the __agb* host functions and the history /
 * location / navigator / screen objects. Called once per realm (the main
 * document's realm and, with AGB_PER_FRAME_REALM, every frame's realm).
 * The DOM node class itself is engine-scoped (wrappers are shared across
 * realms until S2 of design-per-frame-realm.md). */
static void installGlobalApi(AntbEngine* engine, AntbValue* global)
{
    AntbValue* window = global;   /* window IS the global object */

    (void)engine;

    ensureRealmDocumentContext();

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


        /* window.addEventListener is what jQuery's ready setup calls first;
         * listeners live on the document node so both targets share one list. */
        antb_set_native(window, g_document, AGB_TAG_NODE);
        antb_define_method(g_engine, window, "addEventListener", nodeAddEventListener, NULL);
        antb_define_method(g_engine, window, "removeEventListener", nodeRemoveEventListener, NULL);
        antb_define_method(g_engine, window, "dispatchEvent", nodeDispatchEvent, NULL);

        antb_define_method(g_engine, global, "__agbHttpGet", globalHttpGet, NULL);
        antb_define_method(g_engine, global, "__agbHttpRequest", globalHttpRequest, NULL);
        /* Binary variants: base64 payloads for XHR responseType=arraybuffer and
         * fetch().arrayBuffer() (wasm, zip, images). */
        antb_define_method(g_engine, global, "__agbHttpGetBin", globalHttpGetBin, NULL);
        antb_define_method(g_engine, global, "__agbHttpGetEx", globalHttpGetEx, NULL);
        antb_define_method(g_engine, global, "__agbHttpRequestBin", globalHttpRequestBin, NULL);
        /* (url, method, contentType, body, cb) - POST/PUT with a text body. */
        antb_define_method(g_engine, global, "__agbHttpRequestEx", globalHttpRequestEx, NULL);
        /* (id) - drop a request the page gave up on (xhr.timeout / abort()). */
        antb_define_method(g_engine, global, "__agbHttpAbort", globalHttpAbort, NULL);
        antb_define_method(g_engine, global, "__agbClassOp", globalClassOp, NULL);
        antb_define_method(g_engine, global, "__agbFrameSlot", globalFrameSlot, NULL);
        antb_define_method(g_engine, global, "__agbActivateSlot", globalActivateSlot, NULL);
        antb_define_method(g_engine, global, "__agbOpenWindow", globalOpenWindow, NULL);
        antb_define_method(g_engine, global, "__agbPostMessage", globalPostMessage, NULL);
        /* (text) - write CF_UNICODETEXT; backs navigator.clipboard.writeText. */
        antb_define_method(g_engine, global, "__agbClipboardSetText", globalClipboardSetText, NULL);
        antb_define_method(g_engine, global, "__agbFramePostMessage", globalFramePostMessage, NULL);
        antb_define_method(g_engine, global, "__agbConsoleLine", globalConsoleLine, NULL);
        antb_define_method(g_engine, global, "__agbConsoleLineEx", globalConsoleLineEx, NULL);
        antb_define_method(g_engine, global, "__agbLayoutInfo", globalLayoutInfo, NULL);
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
        antb_define_method(g_engine, global, "__agbStylePropertyNames", globalStylePropertyNames, NULL);
        antb_define_method(g_engine, global, "__agbPrefersDark", globalPrefersDark, NULL);
        antb_define_method(g_engine, global, "__agbViewportWidth", globalViewportSize, (void*)0);
        antb_define_method(g_engine, global, "__agbViewportHeight", globalViewportSize, (void*)1);
        /* Direct value access for the prototype-level value accessor (React tracker). */
        antb_define_method(g_engine, global, "__agbInputValue", globalInputValue, NULL);

        /* history/location: document-scoped objects live in RealmDocument so
         * their getters resolve the receiver's document, not the ambient one
         * (R132). */
        {
            AntbValue* hist = AntRuntime::RealmDocument::historyObject(g_engine, g_document);
            if (hist) {
                antb_set_prop(g_engine, global, "history", hist);
                antb_value_release(hist);
            }
        }
        /* Web pages read bare globals like `location` and `navigator`. */
        {
            AntbValue* loc = AntRuntime::RealmDocument::locationObject(g_engine, g_document);
            if (loc) {
                antb_set_prop(g_engine, global, "location", loc);
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

    /* Remember which realm got the API: ensureGlobalApi() runs on the eval
     * funnel and must not re-install it for this realm. */
    {
        AntbRealm* realm = antb_realm_current(engine);

        if (realm) {
            if (!g_apiRealms)
                g_apiRealms = new std::set<AntbRealm*>();

            g_apiRealms->insert(realm);
        }
    }
}
/* location/history follow the document that is being bound into this global.
 * They are re-published on every bind because shared-realm mode reuses one
 * global for parent and child documents (R132). */
static void installDocumentScopedGlobals(AntbEngine* engine, AntbValue* global,
                                         WebCore::Document* document)
{
    AntbValue* v = AntRuntime::RealmDocument::locationObject(engine, document);

    if (v) {
        antb_set_prop(engine, global, "location", v);
        antb_value_release(v);
    }

    v = AntRuntime::RealmDocument::historyObject(engine, document);
    if (v) {
        antb_set_prop(engine, global, "history", v);
        antb_value_release(v);
    }
}

/* Installs the window catch-all getter on a realm's global object: `window`,
 * `self` and `frames` ARE that object, so the indexed/named window properties
 * (frames[0], frames["kid"], window[0], window.name, window.length) live here
 * and not only on the legacy frame proxy. Called on every bind to refresh the
 * document the getter answers for. */
static void installWindowGlobalGetter(AntbEngine* engine, AntbValue* global,
                                      WebCore::Document* document)
{
    /* With one shared global for every view frame, that object is the TOP
     * window: a frame document binding over it must not change who `window`
     * answers for, or `window[0]` would describe the frame. */
    if (!perFrameRealmEnabled() && document->topDocument())
        document = document->topDocument();

    antb_set_native(global, document, AGB_TAG_WINDOW);
    antb_define_property(engine, global, "", windowGetter, &s_windowGlobalMarker);
}

void installFrameGlobals(AntbEngine* engine, WebCore::Document* document)
{
    AntbValue* global;
    AntbValue* selfWindow;
    AntbValue* topWindow = NULL;
    AntbValue* parentWindow = NULL;
    AntbValue* docObj;
    AntbValue* v;

    if (!engine || !document)
        return;

    global = antb_global(engine);
    if (!global)
        return;

    selfWindow = windowForDocument(engine, document);
    docObj = documentObjectFor(engine, document);

    if (document->parentDocument())
        parentWindow = windowObjectFor(engine, document->parentDocument());
    if (document->topDocument() != document)
        topWindow = windowObjectFor(engine, document->topDocument());

    if (selfWindow) {
        antb_set_prop(engine, global, "window", selfWindow);
        antb_set_prop(engine, global, "self", selfWindow);
        antb_set_prop(engine, global, "frames", selfWindow);
    }
    if (docObj)
        antb_set_prop(engine, global, "document", docObj);
    installDocumentScopedGlobals(engine, global, document);
    installWindowGlobalGetter(engine, global, document);

    v = topWindow ? topWindow : selfWindow;
    if (v)
        antb_set_prop(engine, global, "top", v);

    v = parentWindow ? parentWindow : selfWindow;
    if (v)
        antb_set_prop(engine, global, "parent", v);

    v = antb_make_number(engine, (double)childFrameCount(document));
    if (v) {
        antb_set_prop(engine, global, "length", v);
        antb_value_release(v);
    }

    v = document->ownerElement() ? wrapNode(engine, document->ownerElement()) : NULL;
    if (v) {
        antb_set_prop(engine, global, "frameElement", v);
        antb_value_release(v);
    }

    if (selfWindow)
        antb_value_release(selfWindow);
    if (topWindow)
        antb_value_release(topWindow);
    if (parentWindow)
        antb_value_release(parentWindow);
    if (docObj)
        antb_value_release(docObj);
    antb_value_release(global);
}

/* Globals of a frame that owns a realm: `window`/`self` ARE that realm's global
 * object; `parent`/`top` are the parent realm's global objects. Same VM, so
 * property access and calls work across realms, while identity now differs
 * (`parent !== self`, `top.Array !== Array`) as the spec requires. */
static void installChildFrameGlobals(AntbEngine* engine, WebCore::Document* document)
{
    AntbValue* global;
    AntbValue* docObj;
    AntbValue* parentGlobal = NULL;
    AntbValue* topGlobal = NULL;
    AntbValue* v;

    if (!engine || !document)
        return;

    global = antb_global(engine);
    if (!global)
        return;

    docObj = documentObjectFor(engine, document);

    if (document->parentDocument())
        parentGlobal = antb_realm_global(engine, realmOfDocument(engine, document->parentDocument()));
    if (document->topDocument() != document)
        topGlobal = antb_realm_global(engine, realmOfDocument(engine, document->topDocument()));

    antb_set_prop(engine, global, "window", global);
    antb_set_prop(engine, global, "self", global);
    antb_set_prop(engine, global, "frames", global);
    if (docObj)
        antb_set_prop(engine, global, "document", docObj);
    installDocumentScopedGlobals(engine, global, document);
    if (parentGlobal)
        antb_set_prop(engine, global, "parent", parentGlobal);
    if (topGlobal)
        antb_set_prop(engine, global, "top", topGlobal);

    v = antb_make_number(engine, (double)childFrameCount(document));
    if (v) {
        antb_set_prop(engine, global, "length", v);
        antb_value_release(v);
    }

    v = document->ownerElement() ? wrapNode(engine, document->ownerElement()) : NULL;
    if (v) {
        antb_set_prop(engine, global, "frameElement", v);
        antb_value_release(v);
    }

    /* window.addEventListener/... read the node off the global object.
     * AGB_TAG_WINDOW is a second native entry (tags are independent): it lets
     * windowGetter answer for this frame's window from other realms. */
    antb_set_native(global, document, AGB_TAG_NODE);
    installWindowGlobalGetter(engine, global, document);

    if (docObj)
        antb_value_release(docObj);
    if (parentGlobal)
        antb_value_release(parentGlobal);
    if (topGlobal)
        antb_value_release(topGlobal);
    antb_value_release(global);
}

/* Switches into a frame's realm, installing its global API and shims the first
 * time. Called for every bind: the realm stays, the document object inside it
 * is refreshed. */
static void bindFrameRealm(AntbEngine* engine, DocumentRuntime* runtime)
{
    AntbValue* global;

    if (!runtime || !runtime->realm || !runtime->document)
        return;

    antb_realm_enter(engine, runtime->realm);

    global = antb_global(engine);
    if (!global)
        return;

    if (!runtime->installed) {
        installGlobalApi(engine, global);
        runtime->installed = true;
    }

    installChildFrameGlobals(engine, runtime->document);

    /* Keep this realm's document object on its record: timers and callbacks of
     * the realm are restored from it, never rebuilt/repointed. */
    if (runtime->docObj) {
        antb_value_release(runtime->docObj);
        runtime->docObj = 0;
    }
    runtime->docObj = documentObjectFor(engine, runtime->document);
    applyRealmBinding(runtime->realm);

    antb_value_release(global);

    loadPrelude();
    loadPolyfills();
}

void forgetFrameRealm(void* document)
{
    WebCore::Document* doc = (WebCore::Document*)document;
    RuntimeState* owner = stateForDocument(doc);

    if (!doc || !owner)
        return;

    /* The realm belongs to `owner`'s engine; the current view must be that one
     * to release it (frame teardown always runs inside the owning view). */
    if (owner != g_currentState) {
        agbLog(AGB_LOG_EVT, "frame realm drop skipped for document %p (owner %p, current %p)",
               (void*)doc, (void*)owner, (void*)g_currentState);
        return;
    }

    {
        std::vector<DocumentRuntime*>* documents = documentsOf(owner);

        for (size_t i = 0; documents && i < documents->size(); ++i) {
            DocumentRuntime* runtime = (*documents)[i];

            if (runtime->document != doc)
                continue;

            /* Do not leave the host mirrors on a realm that is going away. */
            if (g_currentDocument == runtime)
                applyRealmBinding(g_currentState->mainRealm);

            AntRuntime::RealmDocument::forgetDocument(g_engine, doc);

            /* Wrapper objects created in this realm (and their permanent pins)
             * go first: with the pins left behind the realm could never be
             * collected. */
            if (runtime->realm && g_engine)
                antb_release_realm(g_engine, runtime->realm);

            releaseRealmListeners(owner, runtime->realm);

            agbLog(AGB_LOG_EVT, "frame realm dropped for document %p", (void*)doc);

            /* S2: the work this document started must not outlive it. */
            cancelDocumentResources(runtime->generation);

            /* Drops the keeper (making the realm unreachable: ant's collector
             * frees it on the next GC), the document object, its listeners and
             * the record itself. */
            releaseDocumentRuntime(runtime, true);
            return;
        }
    }
}

void bindDocument(void* document)
{
    WebCore::Document* target = (WebCore::Document*)document;
    RuntimeState* state = g_currentState;
    WebCore::Document* previous = g_document;
    DocumentRuntime* runtime;

    assertContextThread("bindDocument");

    if (!state || state == &g_emptyState || !target)
        return;

    runtime = documentRuntimeFor(state, target);

    if (!runtime || !state->engine)
        return;

    /* The document owns its realm: the engine's main realm for a top-level
     * document, a realm of its own for a frame (per-frame realms), the main
     * realm again when frames share it. documentForRealm() answers from these
     * records - the R130 mainDocument/mainDocObj/boundRealm bookkeeping existed
     * only because that mapping was missing. */
    if (perFrameRealmEnabled() && target->parentDocument())
        runtime->realm = realmForFrameDocument(state->engine, target);
    else if (!runtime->realm)
        runtime->realm = state->mainRealm;

    activateDocument(runtime);

    ensureRealmDocumentContext();

    /* A frame document with its own realm: switch into it and refresh that
     * realm's globals. Nothing below applies (no engine bootstrap, no shared
     * window proxy). */
    if (perFrameRealmEnabled() && target->parentDocument()) {
        if (runtime->realm) {
            bindFrameRealm(state->engine, runtime);
            return;
        }
    }

    /* Every bind enters the document's realm. The engine stays in the realm of
     * whichever frame ran last, and a main-frame evaluation that came after a
     * child frame ran must not execute in the child's realm. This has to happen
     * before the "already bound" early return below. */
    if (perFrameRealmEnabled() && state->mainRealm) {
        antb_realm_enter(state->engine, state->mainRealm);
        activateDocument(runtime);
    }

    /* Binding the document that is already installed is a no-op. A page with
     * async ad scripts gets here 3-6 times for the same document, and the
     * redundant passes make the VM run a closure body once per pass (css3test
     * ran its whole suite several times over). The record knows whether this
     * document was bound; `previous` says whether it is the one the mirrors
     * (and the realm's `document` property) already point at. Both are needed:
     * switching back from a frame document must refresh the realm's globals
     * even though the record itself was bound long ago. */
    if (runtime->documentBound && runtime->docObj && previous == target)
        return;

    g_currentSlot = slotForDocument(target);

    agbLog(AGB_LOG_EVT, "bindDocument %p (realm-bound=%d prev=%p) url=%s", document,
           (int)state->realmBound, (void*)previous,
           target->URL().isEmpty() ? "?" : target->URL().utf8().data());

    if (!g_engine || !g_document)
        return;

    if (state->realmBound) {
        AntbValue* global;

        /* Same realm, new document (a frame document created by the parser):
         * the JS shims stay, but the document object is per document. */
        if (g_docObj) {
            antb_value_release(g_docObj);
            g_docObj = 0;
        }
        g_docObj = documentObjectFor(g_engine, g_document);

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
        AntRuntime::RealmDocument::noteDocument(g_engine, g_document);
        installFrameGlobals(g_engine, g_document);
        runtime->documentBound = true;
        noteMainBinding();
        return;
    }

    {
        AntbValue* global   = antb_global(g_engine);
        AntbValue* docObj   = documentObjectFor(g_engine, g_document);

        if (!global || !docObj) {
            antb_value_release(global);
            antb_value_release(docObj);
            return;
        }


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
                antb_class_add_method(g_engine, g_nodeClass, "hasChildNodes", nodeHasChildNodes, NULL);
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
                antb_class_add_method(g_engine, g_nodeClass, "getRootNode", nodeGetRootNode, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "getAttributeNames", nodeGetAttributeNames, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "toggleAttribute", nodeToggleAttribute, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "insertAdjacentHTML", nodeInsertAdjacentHTML, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "insertAdjacentElement", nodeInsertAdjacentElement, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "insertAdjacentText", nodeInsertAdjacentText, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "replaceChildren", nodeReplaceChildren, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "scrollIntoView", nodeScrollIntoView, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "submit", nodeFormSubmit, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "reset", nodeFormReset, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "checkValidity", nodeFormCheckValidity, NULL);
                /* Canvas: no per-tag classes exist, so the two element-side
                 * canvas APIs live here and check the tag themselves. */
                antb_class_add_method(g_engine, g_nodeClass, "getContext", nodeGetContext, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "toDataURL", nodeToDataURL, NULL);
                /* Media element methods, same scheme (tag checked inside). */
                antb_class_add_method(g_engine, g_nodeClass, "play", nodeMediaPlay, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "pause", nodeMediaPause, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "load", nodeMediaLoad, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "canPlayType", nodeMediaCanPlayType, NULL);
                antb_class_add_method(g_engine, g_nodeClass, "addTextTrack", nodeMediaAddTextTrack, NULL);

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

        installTextTrackClasses(g_engine);
        installGlobalApi(g_engine, global);

        /* A new main frame document is a new session history entry. */
        AntRuntime::RealmDocument::noteDocument(g_engine, g_document);
        /* window/self/top/parent/frames/document/length: per frame, refreshed
         * by the installer below (a child frame reads the same global object). */
        installFrameGlobals(g_engine, g_document);
        /* The record owns the document object from here on. */
        runtime->docObj = docObj;
        noteMainBinding();

        antb_value_release(global);
    }

    /* Bootstrap shims for this document, from prelude.js next to the binary
     * (or at AGB_PRELUDE). */
    loadPrelude();

    /* Polyfills live in polyfills.js next to the binary (or at AGB_POLYFILLS)
     * so they can be edited without rebuilding the engine. */
    loadPolyfills();

    /* Internal pages (agave://...) are the browser's own UI: they get the
     * download API and internal.js, and nothing else does. A web page must not
     * be able to enumerate the user's files or cancel their downloads, so the
     * names below are simply not defined outside this branch. */
    if (isInternalDocument()) {
        AntbValue* global = 0;

        g_agbDownloadEventSink = downloadEventSink;

        global = antb_global(g_engine);

        if (global) {
            antb_define_method(g_engine, global, "__agbDownloadList", globalDownloadList, NULL);
            antb_define_method(g_engine, global, "__agbDownloadCancel", globalDownloadCancel, NULL);
            antb_define_method(g_engine, global, "__agbDownloadOpen", globalDownloadOpen, NULL);
            antb_value_release(global);
        }

        if (!evalScriptFile("AGB_INTERNAL", "internal.js"))
            fprintf(stderr, "[agave] internal page without internal.js\n");
    }

    runtime->documentBound = true;
    state->realmBound = true;

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
    AntbRealm*     realm;   /* realm the handler was registered in (R132) */
    bool           once;    /* remove after the first dispatch */
    bool           handler; /* set through `el.onxxx = fn`, replaced in place */
};

/* Releases the handlers a realm registered (a realm's window listeners). */
static void releaseRealmListeners(RuntimeState* state, AntbRealm* realm)
{
    std::vector<Listener>* list;

    if (!state || !realm || !state->realmListeners)
        return;

    {
        std::map<AntbRealm*, std::vector<Listener>*>::iterator it =
            state->realmListeners->find(realm);

        if (it == state->realmListeners->end())
            return;

        list = it->second;
        state->realmListeners->erase(it);
    }

    if (!list)
        return;

    for (size_t i = 0; i < list->size(); ++i) {
        if ((*list)[i].fn)
            antb_value_release((*list)[i].fn);
    }

    delete list;
}

/* The view's realm is gone (navigation, tab close): every realm's handlers go,
 * and a frame's own realm was already released by forgetFrameRealm(). */
static void releaseAllRealmListeners(RuntimeState* state)
{
    if (!state || !state->realmListeners)
        return;

    while (!state->realmListeners->empty())
        releaseRealmListeners(state, state->realmListeners->begin()->first);

    delete state->realmListeners;
    state->realmListeners = NULL;
}

/* window.addEventListener() belongs to the realm (the window), not to the
 * document object: a reload binds a provisional and then a committed document
 * in the same realm, and the handler from the page's first inline script must
 * still be visible when the load event is dispatched for the other one. */
static std::vector<Listener>*& listeners()
{
    static std::vector<Listener>* noRealm = NULL;
    RuntimeState* state = g_currentState;
    AntbRealm* realm;

    if (!state || state == &g_emptyState)
        return noRealm;

    realm = currentRealm();

    if (!realm)
        return noRealm;

    if (!state->realmListeners)
        state->realmListeners = new std::map<AntbRealm*, std::vector<Listener>*>();

    return (*state->realmListeners)[realm];
}

#define g_listeners listeners()

/* Event handler properties (`script.onload = fn`, `img.onerror = fn`): the DOM
 * treats them as a single replaceable listener. They were plain expandos here,
 * so pages that wait for onload (DeepSeek's WeChat QR loader among them) never
 * ran their callback and failed silently. */
static AntbValue* findNodeEventHandler(WebCore::Node* n, const char* type)
{
    if (!n || !type || !g_listeners)
        return 0;

    for (size_t i = 0; i < g_listeners->size(); ++i) {
        Listener& l = (*g_listeners)[i];

        if (l.handler && l.node == n && l.type == type && l.fn)
            return l.fn;
    }

    return 0;
}

static int setNodeEventHandler(WebCore::Node* n, const char* type, const AntbValue* value)
{
    if (!n || !type)
        return 0;

    if (!g_listeners)
        g_listeners = new std::vector<Listener>();

    /* One handler per (node, type): assigning replaces the previous one. */
    for (size_t i = 0; i < g_listeners->size(); ) {
        Listener& l = (*g_listeners)[i];

        if (l.handler && l.node == n && l.type == type) {
            if (l.fn)
                antb_value_release(l.fn);
            g_listeners->erase(g_listeners->begin() + i);
        } else {
            ++i;
        }
    }

    if (value && antb_is_function(value)) {
        Listener l;

        l.node = n;
        l.type = type;
        l.fn = antb_retain(value);
        l.realm = currentRealm();
        l.once = false;
        l.handler = true;
        g_listeners->push_back(l);

        if (getenv("AGB_EVT_DEBUG"))
            agbLog(AGB_LOG_EVT, "on%s handler node=%p listeners=%d", type, (void*)n, (int)g_listeners->size());
    }

    return 1;
}


/* Document-scoped listeners were dropped per document: see
 * releaseAllRealmListeners()/releaseRealmListeners(). */

/* "div#app" / "img#qr" / "document" for a [gap-jsdom] line. Only called when
 * a gap is reported for the first time, never on the hot path. */
static void gapNodeWhere(WebCore::Node* node, char* buffer, size_t size)
{
    const char* tag = "node";
    WebCore::String id;
    size_t used;

    if (!node || !buffer || !size)
        return;

    if (node->nodeType() == 9) {
        snprintf(buffer, size, "document");
        return;
    }

    if (node->isElementNode()) {
        WebCore::Element* element = static_cast<WebCore::Element*>(node);

        tag = element->tagName().utf8().data();
        id = element->getAttribute("id");
    }

    snprintf(buffer, size, "%s", tag);
    /* tagName is upper case for HTML elements; the log line reads better in
     * the lower case form pages use in selectors. */
    for (used = 0; buffer[used]; ++used) {
        if (buffer[used] >= 'A' && buffer[used] <= 'Z')
            buffer[used] = (char)(buffer[used] - 'A' + 'a');
    }
    if (!id.isEmpty() && used + 1 < size) {
        snprintf(buffer + used, size - used, "#%s", id.utf8().data());
    }
}

WebCore::Node* nodeOf(AntbValue* self)
{
    WebCore::Node* n;

    if (!self)
        return 0;

    n = (WebCore::Node*)antb_get_native(self, AGB_TAG_NODE);
    if (n)
        return n;

    /* A window proxy stands in for its document, so window.addEventListener()
     * and the other document level methods resolve through it. */
    return (WebCore::Node*)antb_get_native(self, AGB_TAG_WINDOW);
}

/* ------------------------------------------------------------------------- */
/* CSSOM: document.styleSheets / sheet.cssRules                              */
/* ------------------------------------------------------------------------- */
/* Live wrapper objects, built the same way as node/canvas wrappers: one class
 * per CSSOM type (methods live on the class, not on every instance), the
 * WebCore object is reached through a native tag, and wrappers are owned by the
 * document so the realm teardown releases them with the nodes. The WebCore
 * objects are ref'd for as long as a wrapper exists; cssomReleaseRefs() drops
 * those refs when the realm goes away (stopRealm/destroyContext). */

#define CSSOM_TAG_LIST  0x43534C31u  /* 'CSL1' */
#define CSSOM_TAG_SHEET 0x43535331u  /* 'CSS1' */
#define CSSOM_TAG_RULE  0x43535231u  /* 'CSR1' */
#define CSSOM_TAG_RLIST 0x4353524Cu  /* 'CRL1' */
#define CSSOM_TAG_MEDIA 0x43534D31u  /* 'CSM1' */
#define CSSOM_TAG_DECL  0x43534431u  /* 'CSD1' */

enum {
    CSSOM_OP_ITEM,
    CSSOM_OP_INSERT_RULE,
    CSSOM_OP_DELETE_RULE,
    CSSOM_OP_ADD_RULE,
    CSSOM_OP_REMOVE_RULE,
    CSSOM_OP_GET_VALUE,
    CSSOM_OP_GET_PRIORITY,
    CSSOM_OP_SET_PROPERTY,
    CSSOM_OP_REMOVE_PROPERTY
};

static unsigned cssomTag(int kind)
{
    switch (kind) {
    case CSSOM_LIST:    return CSSOM_TAG_LIST;
    case CSSOM_SHEET:   return CSSOM_TAG_SHEET;
    case CSSOM_RULE:    return CSSOM_TAG_RULE;
    case CSSOM_RULELIST:return CSSOM_TAG_RLIST;
    case CSSOM_MEDIA:   return CSSOM_TAG_MEDIA;
    default:            return CSSOM_TAG_DECL;
    }
}

static void* cssomNative(AntbValue* self, int kind)
{
    if (!self)
        return NULL;
    return antb_get_native(self, cssomTag(kind));
}

static AntbClass* cssomClassFor(int kind)
{
    switch (kind) {
    case CSSOM_LIST:     return g_styleSheetListClass;
    case CSSOM_SHEET:    return g_styleSheetClass;
    case CSSOM_RULE:     return g_cssRuleClass;
    case CSSOM_RULELIST: return g_cssRuleListClass;
    case CSSOM_MEDIA:    return g_mediaListClass;
    default:             return g_cssDeclarationClass;
    }
}

static int cssomAssignString(AntbEngine* engine, AntbValue* out, const WebCore::String& text)
{
    AntbValue* v = antb_make_string(engine, text.isNull() ? "" : text.utf8().data());

    if (!v)
        return 0;
    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

static int cssomAssignNumber(AntbEngine* engine, AntbValue* out, double number)
{
    AntbValue* v = antb_make_number(engine, number);

    if (!v)
        return 0;
    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

static void cssomTrack(AntbEngine* engine, void* ptr, int kind)
{
    (void)engine;

    if (!ptr)
        return;

    if (!g_cssomSeen)
        g_cssomSeen = new std::set<void*>();

    if (!g_cssomSeen->insert(ptr).second)
        return;

    if (kind == CSSOM_LIST)
        static_cast<WebCore::StyleSheetList*>(ptr)->ref();
    else if (kind == CSSOM_RULELIST)
        static_cast<WebCore::CSSRuleList*>(ptr)->ref();
    else
        static_cast<WebCore::StyleBase*>(ptr)->ref();

    if (!g_cssomRefs)
        g_cssomRefs = new std::vector<CssomRef>();

    {
        CssomRef ref;
        ref.ptr = ptr;
        ref.kind = kind;
        g_cssomRefs->push_back(ref);
    }
}

/* Per document: CSSOM wrappers keep their DOM objects alive, so a document's
 * refs are dropped when the document goes away (not when the view does). */
static void cssomReleaseRefs(DocumentRuntime* document)
{
    if (!document)
        return;

    if (document->cssomRefs) {
        size_t i;

        for (i = 0; i < document->cssomRefs->size(); ++i) {
            CssomRef& ref = (*document->cssomRefs)[i];

            if (ref.kind == CSSOM_LIST)
                static_cast<WebCore::StyleSheetList*>(ref.ptr)->deref();
            else if (ref.kind == CSSOM_RULELIST)
                static_cast<WebCore::CSSRuleList*>(ref.ptr)->deref();
            else
                static_cast<WebCore::StyleBase*>(ref.ptr)->deref();
        }

        delete document->cssomRefs;
        document->cssomRefs = 0;
    }

    if (document->cssomSeen) {
        delete document->cssomSeen;
        document->cssomSeen = 0;
    }
}

static void cssomEnsureClasses();

AntbValue* cssomWrap(AntbEngine* engine, void* ptr, int kind)
{
    AntbClass* klass;
    AntbValue* v;

    if (!ptr)
        return antb_make_null(engine);

    cssomEnsureClasses();

    klass = cssomClassFor(kind);
    if (!klass)
        return NULL;

    v = antb_wrap_native_as_owner(engine, klass, ptr, cssomTag(kind), g_document);
    if (v)
        cssomTrack(engine, ptr, kind);
    return v;
}

static bool cssomNumericName(const char* name, unsigned* index)
{
    unsigned long value;
    char* end = NULL;

    if (!name || !name[0])
        return false;

    value = strtoul(name, &end, 10);
    if (!end || *end)
        return false;

    *index = (unsigned)value;
    return true;
}

static AntbValue* cssomWrapRule(WebCore::CSSRule* rule)
{
    return cssomWrap(g_engine, rule, CSSOM_RULE);
}

static int cssomGetIndexed(AntbEngine* engine, int kind, void* ptr, unsigned index,
                           AntbValue* out)
{
    switch (kind) {
    case CSSOM_LIST: {
        WebCore::StyleSheet* sheet = static_cast<WebCore::StyleSheetList*>(ptr)->item(index);

        if (!sheet || !sheet->isStyleSheet())
            return 0;
        {
            AntbValue* v = cssomWrap(engine, static_cast<WebCore::CSSStyleSheet*>(sheet),
                                     CSSOM_SHEET);
            if (!v)
                return 0;
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        return 1;
    }
    case CSSOM_RULELIST: {
        WebCore::CSSRule* rule = static_cast<WebCore::CSSRuleList*>(ptr)->item(index);

        if (!rule)
            return 0;
        {
            AntbValue* v = cssomWrapRule(rule);
            if (!v)
                return 0;
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        return 1;
    }
    case CSSOM_MEDIA: {
        WebCore::MediaList* media = static_cast<WebCore::MediaList*>(ptr);

        if (index >= media->length())
            return 0;
        return cssomAssignString(engine, out, media->item(index));
    }
    case CSSOM_DECL: {
        WebCore::CSSStyleDeclaration* decl = static_cast<WebCore::CSSStyleDeclaration*>(ptr);

        if (index >= decl->length())
            return 0;
        return cssomAssignString(engine, out, decl->item(index));
    }
    default:
        return 0;
    }
}

int cssomGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void* user)
{
    int kind = (int)(intptr_t)user;
    void* ptr = cssomNative(self, kind);
    unsigned index;

    if (!ptr || !name)
        return 0;

    if (!strcmp(name, "length")) {
        switch (kind) {
        case CSSOM_LIST:
            return cssomAssignNumber(engine, out,
                (double)static_cast<WebCore::StyleSheetList*>(ptr)->length());
        case CSSOM_RULELIST:
            return cssomAssignNumber(engine, out,
                (double)static_cast<WebCore::CSSRuleList*>(ptr)->length());
        case CSSOM_MEDIA:
            return cssomAssignNumber(engine, out,
                (double)static_cast<WebCore::MediaList*>(ptr)->length());
        case CSSOM_DECL:
            return cssomAssignNumber(engine, out,
                (double)static_cast<WebCore::CSSStyleDeclaration*>(ptr)->length());
        default:
            return 0;
        }
    }

    if (cssomNumericName(name, &index))
        return cssomGetIndexed(engine, kind, ptr, index, out);

    switch (kind) {
    case CSSOM_SHEET: {
        WebCore::CSSStyleSheet* sheet = static_cast<WebCore::CSSStyleSheet*>(ptr);
        AntbValue* v;

        if (!strcmp(name, "href"))
            return cssomAssignString(engine, out, sheet->href());
        if (!strcmp(name, "title"))
            return cssomAssignString(engine, out, sheet->title());
        if (!strcmp(name, "type"))
            return cssomAssignString(engine, out, WebCore::String("text/css"));
        if (!strcmp(name, "disabled"))
            return cssomAssignNumber(engine, out, sheet->disabled() ? 1 : 0);
        if (!strcmp(name, "media"))
            v = cssomWrap(engine, sheet->media(), CSSOM_MEDIA);
        else if (!strcmp(name, "ownerNode")) {
            v = sheet->ownerNode() ? wrapNode(engine, sheet->ownerNode())
                                   : antb_make_null(engine);
        } else if (!strcmp(name, "ownerRule"))
            v = cssomWrap(engine, sheet->ownerRule(), CSSOM_RULE);
        else if (!strcmp(name, "parentStyleSheet"))
            v = cssomWrap(engine, sheet->parentStyleSheet(), CSSOM_SHEET);
        else if (!strcmp(name, "cssRules") || !strcmp(name, "rules"))
            v = cssomWrap(engine, sheet->cssRules(!strcmp(name, "rules")), CSSOM_RULELIST);
        else
            return 0;

        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }
    case CSSOM_RULE: {
        WebCore::CSSRule* rule = static_cast<WebCore::CSSRule*>(ptr);
        AntbValue* v = NULL;

        if (!strcmp(name, "type"))
            return cssomAssignNumber(engine, out, (double)rule->type());
        if (!strcmp(name, "cssText"))
            return cssomAssignString(engine, out, rule->cssText());
        if (!strcmp(name, "parentStyleSheet"))
            v = cssomWrap(engine, rule->parentStyleSheet(), CSSOM_SHEET);

        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
            return 1;
        }

        switch (rule->type()) {
        case WebCore::CSSRule::STYLE_RULE: {
            WebCore::CSSStyleRule* styleRule = static_cast<WebCore::CSSStyleRule*>(rule);

            if (!strcmp(name, "selectorText"))
                return cssomAssignString(engine, out, styleRule->selectorText());
            if (!strcmp(name, "style"))
                v = cssomWrap(engine, styleRule->style(), CSSOM_DECL);
            break;
        }
        case WebCore::CSSRule::MEDIA_RULE: {
            WebCore::CSSMediaRule* mediaRule = static_cast<WebCore::CSSMediaRule*>(rule);

            if (!strcmp(name, "media"))
                v = cssomWrap(engine, mediaRule->media(), CSSOM_MEDIA);
            else if (!strcmp(name, "cssRules"))
                v = cssomWrap(engine, mediaRule->cssRules(), CSSOM_RULELIST);
            break;
        }
        case WebCore::CSSRule::IMPORT_RULE: {
            WebCore::CSSImportRule* importRule = static_cast<WebCore::CSSImportRule*>(rule);

            if (!strcmp(name, "href"))
                return cssomAssignString(engine, out, importRule->href());
            if (!strcmp(name, "media"))
                v = cssomWrap(engine, importRule->media(), CSSOM_MEDIA);
            else if (!strcmp(name, "styleSheet"))
                v = cssomWrap(engine, importRule->styleSheet(), CSSOM_SHEET);
            break;
        }
        case WebCore::CSSRule::CHARSET_RULE:
            if (!strcmp(name, "encoding"))
                return cssomAssignString(
                    engine, out, static_cast<WebCore::CSSCharsetRule*>(rule)->encoding());
            break;
        case WebCore::CSSRule::FONT_FACE_RULE:
            if (!strcmp(name, "style"))
                v = cssomWrap(engine,
                    static_cast<WebCore::CSSFontFaceRule*>(rule)->style(), CSSOM_DECL);
            break;
        case WebCore::CSSRule::PAGE_RULE: {
            WebCore::CSSPageRule* pageRule = static_cast<WebCore::CSSPageRule*>(rule);

            if (!strcmp(name, "selectorText"))
                return cssomAssignString(engine, out, pageRule->selectorText());
            if (!strcmp(name, "style"))
                v = cssomWrap(engine, pageRule->style(), CSSOM_DECL);
            break;
        }
        default:
            break;
        }

        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }
    case CSSOM_MEDIA:
        if (!strcmp(name, "mediaText"))
            return cssomAssignString(
                engine, out, static_cast<WebCore::MediaList*>(ptr)->mediaText());
        return 0;
    case CSSOM_DECL: {
        WebCore::CSSStyleDeclaration* decl = static_cast<WebCore::CSSStyleDeclaration*>(ptr);

        if (!strcmp(name, "cssText"))
            return cssomAssignString(engine, out, decl->cssText());
        if (!strcmp(name, "parentRule"))
            return assignNull(engine, out);
        return 0;
    }
    default:
        return 0;
    }
}

int cssomMethod(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                AntbValue* out, void* user)
{
    int op = (int)(intptr_t)user;
    int kind = -1;
    int k;
    void* ptr = NULL;

    for (k = CSSOM_LIST; k <= CSSOM_DECL; ++k) {
        ptr = cssomNative(self, k);
        if (ptr) {
            kind = k;
            break;
        }
    }

    if (!ptr)
        return 0;

    if (op == CSSOM_OP_ITEM) {
        unsigned index = (argc > 0 && args[0]) ? (unsigned)antb_to_number(args[0]) : 0;

        return cssomGetIndexed(engine, kind, ptr, index, out);
    }

    if (op == CSSOM_OP_GET_VALUE || op == CSSOM_OP_GET_PRIORITY) {
        WebCore::CSSStyleDeclaration* decl = static_cast<WebCore::CSSStyleDeclaration*>(ptr);
        char* name = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
        WebCore::String value;

        if (!name)
            return 0;

        if (op == CSSOM_OP_GET_VALUE)
            value = decl->getPropertyValue(WebCore::String(name));
        else
            value = decl->getPropertyPriority(WebCore::String(name));

        antb_free(name);
        return cssomAssignString(engine, out, value);
    }

    if (op == CSSOM_OP_SET_PROPERTY) {
        WebCore::CSSStyleDeclaration* decl = static_cast<WebCore::CSSStyleDeclaration*>(ptr);
        char* name = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
        char* value = (argc > 1 && args[1]) ? antb_to_utf8(args[1]) : NULL;
        char* priority = (argc > 2 && args[2]) ? antb_to_utf8(args[2]) : NULL;
        WebCore::ExceptionCode ec = 0;

        if (name && value) {
            decl->setProperty(WebCore::String(name), WebCore::String(value),
                              priority ? WebCore::String(priority) : WebCore::String(), ec);
        }

        antb_free(name);
        antb_free(value);
        antb_free(priority);
        return ec ? 0 : 1;
    }

    if (op == CSSOM_OP_REMOVE_PROPERTY) {
        WebCore::CSSStyleDeclaration* decl = static_cast<WebCore::CSSStyleDeclaration*>(ptr);
        char* name = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
        WebCore::ExceptionCode ec = 0;
        WebCore::String removed;

        if (name)
            removed = decl->removeProperty(WebCore::String(name), ec);

        antb_free(name);
        if (ec)
            return 0;
        return cssomAssignString(engine, out, removed);
    }

    if (kind == CSSOM_SHEET
            || (kind == CSSOM_RULE
                && static_cast<WebCore::CSSRule*>(ptr)->isMediaRule())) {
        WebCore::ExceptionCode ec = 0;
        unsigned index = (argc > 1 && args[1]) ? (unsigned)antb_to_number(args[1]) : 0;

        if (op == CSSOM_OP_INSERT_RULE) {
            char* text = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
            unsigned result = 0;

            if (!text)
                return 0;

            if (kind == CSSOM_SHEET)
                result = static_cast<WebCore::CSSStyleSheet*>(ptr)->insertRule(
                    WebCore::String(text), index, ec);
            else
                result = static_cast<WebCore::CSSMediaRule*>(ptr)->insertRule(
                    WebCore::String(text), index, ec);

            antb_free(text);
            if (ec)
                return 0;
            return cssomAssignNumber(engine, out, (double)result);
        }

        if (op == CSSOM_OP_DELETE_RULE) {
            index = (argc > 0 && args[0]) ? (unsigned)antb_to_number(args[0]) : 0;

            if (kind == CSSOM_SHEET)
                static_cast<WebCore::CSSStyleSheet*>(ptr)->deleteRule(index, ec);
            else
                static_cast<WebCore::CSSMediaRule*>(ptr)->deleteRule(index, ec);
            return ec ? 0 : 1;
        }

        if (op == CSSOM_OP_ADD_RULE) {
            char* selector = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
            char* body = (argc > 1 && args[1]) ? antb_to_utf8(args[1]) : NULL;
            int result = -1;

            if (!selector || !body) {
                antb_free(selector);
                antb_free(body);
                return 0;
            }

            if (kind == CSSOM_SHEET)
                result = static_cast<WebCore::CSSStyleSheet*>(ptr)->addRule(
                    WebCore::String(selector), WebCore::String(body),
                    (argc > 2 && args[2]) ? (int)antb_to_number(args[2]) : -1, ec);

            antb_free(selector);
            antb_free(body);
            if (ec)
                return 0;
            return cssomAssignNumber(engine, out, (double)result);
        }

        if (op == CSSOM_OP_REMOVE_RULE) {
            unsigned index2 = (argc > 0 && args[0]) ? (unsigned)antb_to_number(args[0]) : 0;

            if (kind == CSSOM_SHEET)
                static_cast<WebCore::CSSStyleSheet*>(ptr)->removeRule(index2, ec);
            return ec ? 0 : 1;
        }
    }

    return 0;
}

static void cssomEnsureClasses()
{
    if (g_styleSheetListClass)
        return;

    g_styleSheetListClass = antb_class_create(g_engine, "StyleSheetList");
    if (g_styleSheetListClass) {
        antb_class_set_property(g_engine, g_styleSheetListClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_LIST);
        antb_class_add_method(g_engine, g_styleSheetListClass, "item",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_ITEM);
    }

    g_styleSheetClass = antb_class_create(g_engine, "CSSStyleSheet");
    if (g_styleSheetClass) {
        antb_class_set_property(g_engine, g_styleSheetClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_SHEET);
        antb_class_add_method(g_engine, g_styleSheetClass, "insertRule",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_INSERT_RULE);
        antb_class_add_method(g_engine, g_styleSheetClass, "deleteRule",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_DELETE_RULE);
        antb_class_add_method(g_engine, g_styleSheetClass, "addRule",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_ADD_RULE);
        antb_class_add_method(g_engine, g_styleSheetClass, "removeRule",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_REMOVE_RULE);
    }

    g_cssRuleClass = antb_class_create(g_engine, "CSSRule");
    if (g_cssRuleClass) {
        antb_class_set_property(g_engine, g_cssRuleClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_RULE);
        antb_class_add_method(g_engine, g_cssRuleClass, "insertRule",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_INSERT_RULE);
        antb_class_add_method(g_engine, g_cssRuleClass, "deleteRule",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_DELETE_RULE);
    }

    g_cssRuleListClass = antb_class_create(g_engine, "CSSRuleList");
    if (g_cssRuleListClass) {
        antb_class_set_property(g_engine, g_cssRuleListClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_RULELIST);
        antb_class_add_method(g_engine, g_cssRuleListClass, "item",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_ITEM);
    }

    g_mediaListClass = antb_class_create(g_engine, "MediaList");
    if (g_mediaListClass) {
        antb_class_set_property(g_engine, g_mediaListClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_MEDIA);
        antb_class_add_method(g_engine, g_mediaListClass, "item",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_ITEM);
    }

    g_cssDeclarationClass = antb_class_create(g_engine, "CSSStyleDeclaration");
    if (g_cssDeclarationClass) {
        antb_class_set_property(g_engine, g_cssDeclarationClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_DECL);
        antb_class_add_method(g_engine, g_cssDeclarationClass, "item",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_ITEM);
        antb_class_add_method(g_engine, g_cssDeclarationClass, "getPropertyValue",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_GET_VALUE);
        antb_class_add_method(g_engine, g_cssDeclarationClass, "getPropertyPriority",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_GET_PRIORITY);
        antb_class_add_method(g_engine, g_cssDeclarationClass, "setProperty",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_SET_PROPERTY);
        antb_class_add_method(g_engine, g_cssDeclarationClass, "removeProperty",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_REMOVE_PROPERTY);
    }
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
    /* Inline-style change, not a full style change: setProperty() already
     * marked the element with InlineStyleChange, and upgrading that to
     * FullStyleChange made Document::recalcStyle() walk the whole document
     * with Force (Element::recalcStyle turns FullStyleChange + descendant
     * rules into Force). On css3test that turned every `el.style.x = y` into a
     * full-document restyle plus a full layout: ~100ms per frame. */
    element->setChanged(WebCore::InlineStyleChange);
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

/* Element-only children / siblings. WebCore of this vintage has no
 * firstElementChild & co., and every modern framework walks them. */
static WebCore::Element* childElement(WebCore::Node* parent, int index)
{
    WebCore::Node* child;
    int seen = 0;

    if (!parent)
        return 0;

    WebCore::Element* last = 0;

    for (child = parent->firstChild(); child; child = child->nextSibling()) {
        if (!child->isElementNode())
            continue;
        last = static_cast<WebCore::Element*>(child);
        if (index >= 0 && seen == index)
            return last;
        ++seen;
    }

    return index < 0 ? last : 0;
}

static WebCore::Element* siblingElement(WebCore::Node* node, int direction)
{
    WebCore::Node* sibling;

    if (!node)
        return 0;

    sibling = direction > 0 ? node->nextSibling() : node->previousSibling();
    while (sibling) {
        if (sibling->isElementNode())
            return static_cast<WebCore::Element*>(sibling);
        sibling = direction > 0 ? sibling->nextSibling() : sibling->previousSibling();
    }

    return 0;
}

/* isConnected: the node hangs off a document. */
static bool nodeIsConnected(WebCore::Node* node)
{
    WebCore::Node* top = node;

    while (top && top->parent())
        top = top->parent();

    return top && top->isDocumentNode();
}

/* Reflected attributes: element properties that are just an attribute. */
enum ReflectedKind { REFLECT_STRING = 0, REFLECT_BOOL, REFLECT_NUMBER };

static const struct ReflectedAttribute {
    const char* property;
    const char* attribute;
    int         kind;
} kReflectedAttributes[] = {
    { "dir",             "dir",             REFLECT_STRING },
    { "lang",            "lang",            REFLECT_STRING },
    { "slot",            "slot",            REFLECT_STRING },
    { "part",            "part",            REFLECT_STRING },
    { "contentEditable", "contenteditable", REFLECT_STRING },
    { "draggable",       "draggable",       REFLECT_STRING },
    { "translate",       "translate",       REFLECT_STRING },
    { "hidden",          "hidden",          REFLECT_BOOL   },
    { "spellcheck",      "spellcheck",      REFLECT_BOOL   },
    { "autofocus",       "autofocus",       REFLECT_BOOL   },
    { "tabIndex",        "tabindex",        REFLECT_NUMBER },
    /* `iframe.frameBorder = "0"` is how WxLogin (DeepSeek QR code) removes the
     * default 2px inset border of a dynamically created iframe. Without the
     * reflection it was stored as a plain expando, so
     * HTMLIFrameElement::parseMappedAttribute() never added the border-width:0
     * presentational hint and the frame kept its visible 2px border. */
    { "frameBorder",     "frameborder",     REFLECT_STRING }
};

static const struct ReflectedAttribute* reflectedAttribute(const char* name)
{
    size_t i;

    if (!name || !name[0])
        return NULL;

    for (i = 0; i < sizeof(kReflectedAttributes) / sizeof(kReflectedAttributes[0]); ++i) {
        if (strcmp(kReflectedAttributes[i].property, name) == 0)
            return &kReflectedAttributes[i];
    }

    return NULL;
}

/* insertAdjacent*(position, node): the four DOM positions, in one place. */
static bool insertRelativeTo(WebCore::Node* target, WebCore::Node* node, const char* position)
{
    WebCore::ExceptionCode ec = 0;
    RefPtr<WebCore::Node> holder;

    if (!target || !node || !position)
        return false;

    holder = node;   /* our own ref; the tree takes its own */

    if (!strcmp(position, "beforebegin")) {
        if (!target->parent())
            return false;
        target->parent()->insertBefore(holder.release(), target, ec);
    } else if (!strcmp(position, "afterbegin")) {
        target->insertBefore(holder.release(), target->firstChild(), ec);
    } else if (!strcmp(position, "beforeend")) {
        target->appendChild(holder.release(), ec);
    } else if (!strcmp(position, "afterend")) {
        if (!target->parent())
            return false;
        target->parent()->insertBefore(holder.release(), target->nextSibling(), ec);
    } else {
        return false;
    }

    return !ec;
}

int nodeGetRootNode(AntbEngine* engine, AntbValue* self, AntbValue**, int, AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    WebCore::Node* top;

    if (!n)
        return 1;

    top = n;
    while (top->parent())
        top = top->parent();

    {
        AntbValue* v = top->isDocumentNode()
                     ? AntRuntime::documentObjectFor(engine, static_cast<WebCore::Document*>(top))
                     : wrapNode(engine, top);

        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

int nodeGetAttributeNames(AntbEngine* engine, AntbValue* self, AntbValue**, int, AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    AntbValue* array;

    if (!n || !n->isElementNode())
        return 1;

    {
        WebCore::NamedAttrMap* attributes = static_cast<WebCore::Element*>(n)->attributes();
        unsigned i;
        unsigned count;

        if (!attributes)
            return 1;

        array = antb_make_array(engine);
        if (!array)
            return 1;

        count = attributes->length();
        for (i = 0; i < count; ++i) {
            WebCore::Attribute* attribute = attributes->attributeItem(i);
            AntbValue* name;

            if (!attribute)
                continue;
            name = antb_make_string(engine, attribute->name().localName().domString().utf8().data());
            if (name) {
                antb_array_push(engine, array, name);
                antb_value_release(name);
            }
        }

        antb_value_assign(out, array);
        antb_value_release(array);
    }
    return 1;
}

int nodeToggleAttribute(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                        AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    WebCore::Element* element;
    char* name;
    int force = -1;

    if (!n || !n->isElementNode() || argc < 1)
        return 1;

    element = static_cast<WebCore::Element*>(n);
    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    if (argc > 1 && args[1])
        force = antb_to_boolean(args[1]) ? 1 : 0;

    {
        bool present = element->hasAttribute(WebCore::String(name));
        bool result = present;

        WebCore::ExceptionCode ec = 0;

        if (present && force != 1) {
            element->removeAttribute(WebCore::String(name), ec);
            result = false;
        } else if (!present && force != 0) {
            ec = 0;
            element->setAttribute(WebCore::String(name), WebCore::String(""), ec);
            result = true;
        }

        {
            AntbValue* v = antb_make_number(engine, result ? 1 : 0);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
    }

    antb_free(name);
    return 1;
}

int nodeInsertAdjacentHTML(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* position;
    char* html;

    (void)engine; (void)out;

    if (!n || argc < 2)
        return 1;

    position = antb_to_utf8(args[0]);
    html = antb_to_utf8(args[1]);
    if (!position || !html) {
        antb_free(position);
        antb_free(html);
        return 1;
    }

    {
        WebCore::Document* document = n->document();
        RefPtr<WebCore::DocumentFragment> fragment;

        if (document)
            fragment = createFragmentFromMarkup(document, WebCore::String::fromUTF8(html),
                                                document->baseURI());
        if (fragment)
            insertRelativeTo(n, fragment.get(), position);
    }

    antb_free(position);
    antb_free(html);
    return 1;
}

int nodeInsertAdjacentElement(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                              AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    WebCore::Node* child;
    char* position;

    if (!n || argc < 2)
        return 1;

    child = nodeOf(args[1]);
    position = antb_to_utf8(args[0]);
    if (!child || !position) {
        antb_free(position);
        return 1;
    }

    if (insertRelativeTo(n, child, position)) {
        AntbValue* v = wrapNode(engine, child);

        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }

    antb_free(position);
    return 1;
}

int nodeInsertAdjacentText(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* position;
    char* text;

    (void)engine; (void)out;

    if (!n || argc < 2)
        return 1;

    position = antb_to_utf8(args[0]);
    text = antb_to_utf8(args[1]);
    if (position && text) {
        WebCore::Document* document = n->document();

        if (document) {
            RefPtr<WebCore::Node> node = document->createTextNode(WebCore::String::fromUTF8(text));

            if (node)
                insertRelativeTo(n, node.get(), position);
        }
    }

    antb_free(position);
    antb_free(text);
    return 1;
}

int nodeReplaceChildren(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                        AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    WebCore::ExceptionCode ec = 0;

    (void)engine; (void)out;

    if (!n)
        return 1;

    while (n->firstChild()) {
        RefPtr<WebCore::Node> child = n->firstChild();

        n->removeChild(child.get(), ec);
        ec = 0;
    }

    {
        int i;

        for (i = 0; i < argc; ++i) {
            WebCore::Node* arg = nodeOf(args[i]);

            if (arg) {
                RefPtr<WebCore::Node> holder = arg;

                n->appendChild(holder.release(), ec);
                ec = 0;
            } else {
                char* text = antb_to_utf8(args[i]);

                if (text) {
                    WebCore::Document* document = n->document();

                    if (document) {
                        RefPtr<WebCore::Node> node = document->createTextNode(WebCore::String::fromUTF8(text));

                        if (node)
                            n->appendChild(node.release(), ec);
                        ec = 0;
                    }
                    antb_free(text);
                }
            }
        }
    }
    return 1;
}

int nodeScrollIntoView(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    bool alignToTop = true;

    (void)engine; (void)out;

    if (!n || !n->isElementNode())
        return 1;

    if (argc > 0 && args[0])
        alignToTop = antb_to_boolean(args[0]) != 0;

    static_cast<WebCore::Element*>(n)->scrollIntoView(alignToTop);
    return 1;
}

/* Wraps a WebCore NodeList/HTMLCollection as a JS array (a snapshot: these
 * are not live collections, see docs/arch-capability-gaps.md). */
/* NodeList and HTMLCollection share length()/item(); one template covers both. */
template <typename Collection>
static AntbValue* wrapNodeList(AntbEngine* engine, Collection* list)
{
    AntbValue* array;
    unsigned i;
    unsigned count;

    array = antb_make_array(engine);
    if (!array || !list)
        return array;

    count = list->length();
    for (i = 0; i < count; ++i) {
        WebCore::Node* item = list->item(i);
        AntbValue* wrapped;

        if (!item)
            continue;
        wrapped = wrapNode(engine, item);
        if (wrapped) {
            antb_array_push(engine, array, wrapped);
            antb_value_release(wrapped);
        }
    }

    return array;
}

/* form.submit() / form.reset(): the methods live on the node class and check
 * the tag, the same way getContext/toDataURL do. */
int nodeFormSubmit(AntbEngine*, AntbValue* self, AntbValue**, int, AntbValue*, void*)
{
    WebCore::Node* n = nodeOf(self);

    if (n && n->isElementNode()
            && static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::formTag))
        static_cast<WebCore::HTMLFormElement*>(n)->submit();

    return 1;
}

int nodeFormReset(AntbEngine*, AntbValue* self, AntbValue**, int, AntbValue*, void*)
{
    WebCore::Node* n = nodeOf(self);

    if (n && n->isElementNode()
            && static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::formTag))
        static_cast<WebCore::HTMLFormElement*>(n)->reset();

    return 1;
}

int nodeFormCheckValidity(AntbEngine* engine, AntbValue*, AntbValue**, int, AntbValue* out, void*)
{
    /* No constraint validation yet: reporting "valid" keeps submit handlers
     * running instead of silently blocking them. */
    AntbValue* v = antb_make_number(engine, 1);

    if (v) {
        antb_value_assign(out, v);
        antb_value_release(v);
    }
    return 1;
}

int nodeDocumentImportNode(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void*)
{
    WebCore::Document* document = documentOf(self);
    WebCore::Node* source = (argc > 0) ? nodeOf(args[0]) : 0;

    if (!document || !source)
        return 1;

    {
        WebCore::ExceptionCode ec = 0;
        RefPtr<WebCore::Node> imported = document->importNode(source, argc > 1 && antb_to_boolean(args[1]),
                                                              ec);

        if (imported) {
            AntbValue* v = wrapNode(engine, imported.get());

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
    }
    return 1;
}

int nodeDocumentAdoptNode(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                          AntbValue* out, void*)
{
    WebCore::Document* document = documentOf(self);
    WebCore::Node* source = (argc > 0) ? nodeOf(args[0]) : 0;

    if (!document || !source)
        return 1;

    {
        WebCore::ExceptionCode ec = 0;
        RefPtr<WebCore::Node> holder = source;   /* adoptNode takes ownership */
        RefPtr<WebCore::Node> adopted = document->adoptNode(holder.release(), ec);

        if (adopted) {
            AntbValue* v = wrapNode(engine, adopted.get());

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
    }
    return 1;
}

int nodeDocumentHasFocus(AntbEngine* engine, AntbValue*, AntbValue**, int, AntbValue* out, void*)
{
    /* No real window focus tracking: the bound document is the active one. */
    AntbValue* v = antb_make_number(engine, 1);

    if (v) {
        antb_value_assign(out, v);
        antb_value_release(v);
    }
    return 1;
}

int nodeDocumentElementFromPoint(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                                 AntbValue* out, void*)
{
    WebCore::Document* document = documentOf(self);

    if (!document || argc < 2)
        return 1;

    {
        WebCore::Element* element = document->elementFromPoint((int)antb_to_number(args[0]),
                                                               (int)antb_to_number(args[1]));

        if (element) {
            AntbValue* v = wrapNode(engine, element);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
    }
    return 1;
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

    /* Event handler properties: read back the function nodeSetter stored. */
    if (name && !strncmp(name, "on", 2) && name[2]) {
        AntbValue* fn = findNodeEventHandler(n, name + 2);

        if (!fn)
            return 0;

        antb_value_assign(out, fn);
        return 1;
    }

    /* document.defaultView: React Router's data router reads
     * `document.defaultView.history` before anything else, and the property
     * set on the shared document object does not reach wrappers handed out
     * for the same Document, so answer it here for every node.
     * With per-frame realms the view belongs to the node's document, not to
     * whichever realm happens to be current (R132). */
    if (!strcmp(name, "defaultView")) {
        WebCore::Document* document = n->document();
        AntbRealm* realm = (document && perFrameRealmEnabled())
                         ? realmOfDocument(engine, document) : NULL;
        AntbValue* global = realm ? antb_realm_global(engine, realm)
                                  : antb_global(engine);

        if (global) {
            antb_value_assign(out, global);
            antb_value_release(global);
        }
        return 1;
    }

    /* document.styleSheets on a document wrapper (document.documentElement...
     * ownerDocument.styleSheets), and <style>/<link>.sheet; both are live
     * wrapper objects (see the CSSOM block above). */
    if (!strcmp(name, "styleSheets") && n->isDocumentNode()) {
        AntbValue* list = cssomWrap(engine,
            static_cast<WebCore::Document*>(n)->styleSheets(), CSSOM_LIST);

        if (!list)
            return 0;
        antb_value_assign(out, list);
        antb_value_release(list);
        return 1;
    }

    if (!strcmp(name, "sheet") && n->isElementNode()) {
        WebCore::Element* el = static_cast<WebCore::Element*>(n);

        if (el->hasTagName(WebCore::HTMLNames::styleTag)
                || el->hasTagName(WebCore::HTMLNames::linkTag)) {
            AntbValue* sheet = NULL;
            WebCore::Document* doc = n->document();

            if (doc) {
                WebCore::StyleSheetList* sheets = doc->styleSheets();
                unsigned count = sheets ? sheets->length() : 0;
                unsigned i;

                for (i = 0; i < count; ++i) {
                    WebCore::StyleSheet* s = sheets->item(i);

                    if (s && s->ownerNode() == n) {
                        sheet = cssomWrap(engine, s, CSSOM_SHEET);
                        break;
                    }
                }
            }

            if (!sheet)
                sheet = antb_make_null(engine);
            if (sheet) {
                antb_value_assign(out, sheet);
                antb_value_release(sheet);
            }
            return 1;
        }
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

    /* --- element tree navigation ----------------------------------------- *
     * WebCore 2008 only has parentNode/childNodes/nextSibling; every modern
     * framework walks the element-only variants, and reading them as
     * undefined made `el.parentElement.remove()` throw. */
    if (!strcmp(name, "parentElement")) {
        WebCore::Node* parent = n->parent();

        if (!parent || !parent->isElementNode())
            return 1;   /* null for a top level element */
        v = wrapNode(engine, parent);
    } else if (!strcmp(name, "firstElementChild") || !strcmp(name, "lastElementChild")) {
        WebCore::Element* child = childElement(n, name[0] == 'f' ? 0 : -1);

        if (!child)
            return 1;
        v = wrapNode(engine, child);
    } else if (!strcmp(name, "nextElementSibling") || !strcmp(name, "previousElementSibling")) {
        WebCore::Element* sibling = siblingElement(n, name[0] == 'n' ? 1 : -1);

        if (!sibling)
            return 1;
        v = wrapNode(engine, sibling);
    } else if (!strcmp(name, "isConnected")) {
        v = antb_make_number(engine, nodeIsConnected(n) ? 1 : 0);
    } else if (!strcmp(name, "namespaceURI")) {
        /* Vue's hydration picks the render namespace (svg/mathml/html) from
         * element.namespaceURI; without it `hydrate()` threw on
         * `undefined.includes`. HTML documents use the xhtml URI. */
        const WebCore::AtomicString& ns = n->namespaceURI();
        v = antb_make_string(engine, ns.isEmpty() ? "http://www.w3.org/1999/xhtml"
                                                  : ns.deprecatedString().utf8().data());
    } else if (!strcmp(name, "nodeValue")) {
        /* Text/Comment carry data; every other node type is null. */
        if (n->isTextNode() || n->isCommentNode()) {
            WebCore::String data = static_cast<WebCore::CharacterData*>(n)->data();

            v = antb_make_string(engine, data.utf8().data());
        } else {
            return 1;
        }
    } else if (!strcmp(name, "innerText") || !strcmp(name, "outerText")) {
        if (!n->isElementNode())
            return 1;
        {
            WebCore::String text = static_cast<WebCore::Element*>(n)->innerText();

            v = antb_make_string(engine, text.utf8().data());
        }
    } else if (!strcmp(name, "offsetParent")) {
        if (!n->isElementNode())
            return 1;
        {
            WebCore::Element* parent = static_cast<WebCore::Element*>(n)->offsetParent();

            if (!parent)
                return 1;
            v = wrapNode(engine, parent);
        }
    } else if (n->isElementNode()
            && static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::imgTag)
            && (!strcmp(name, "complete") || !strcmp(name, "naturalWidth")
                || !strcmp(name, "naturalHeight") || !strcmp(name, "width")
                || !strcmp(name, "height"))) {
        /* Pages wait for complete/naturalWidth before drawing the image
         * themselves (canvas, QR scanners, lazy loaders). */
        WebCore::HTMLImageElement* image = static_cast<WebCore::HTMLImageElement*>(n);

        if (!strcmp(name, "complete"))
            v = antb_make_number(engine, image->complete() ? 1 : 0);
        else if (!strcmp(name, "naturalWidth"))
            v = antb_make_number(engine, image->naturalWidth());
        else if (!strcmp(name, "naturalHeight"))
            v = antb_make_number(engine, image->naturalHeight());
        else if (!strcmp(name, "width"))
            v = antb_make_number(engine, image->width());
        else
            v = antb_make_number(engine, image->height());
    } else if (n->isElementNode() && !strcmp(name, "decode")
            && static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::imgTag)) {
        AntbValue* global = antb_global(engine);

        if (global) {
            AntbValue* fn = antb_get_prop(engine, global, "__agbDecodeImage");

            if (fn) {
                antb_value_assign(out, fn);
                antb_value_release(fn);
                antb_value_release(global);
                return 1;
            }
            antb_value_release(global);
        }
        return 1;
    } else if (n->isElementNode()
            && (static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::videoTag)
                || static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::audioTag))
            && mediaElementProperty(engine, static_cast<WebCore::HTMLMediaElement*>(
                   static_cast<WebCore::Element*>(n)), name, out)) {
        /* handled by the media element's property table (see below) */
        return 1;
    } else if (n->isElementNode()
            && static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::formTag)
            && (!strcmp(name, "action") || !strcmp(name, "elements"))) {
        WebCore::HTMLFormElement* form = static_cast<WebCore::HTMLFormElement*>(n);

        if (!strcmp(name, "action")) {
            v = antb_make_string(engine, form->action().utf8().data());
        } else {
            RefPtr<WebCore::HTMLCollection> elements = form->elements();

            v = wrapNodeList(engine, elements.get());
        }
    } else if (n->isElementNode()
            && static_cast<WebCore::Element*>(n)->isFrameOwnerElement()
            && (!strcmp(name, "contentWindow") || !strcmp(name, "contentDocument"))) {
        WebCore::Document* child = static_cast<WebCore::HTMLFrameOwnerElement*>(n)->contentDocument();

        if (!child)
            return 1;   /* null until the frame document exists */
        if (!strcmp(name, "contentWindow") && perFrameRealmEnabled()) {
            /* A frame with its own realm: its `window` IS that realm's global
             * object, so contentWindow has to hand out that object (otherwise
             * `f.contentWindow === childWindow` is false and contentWindow.top
             * is the wrong realm). */
            AntbRealm* childRealm = realmOfDocument(engine, child);

            if (childRealm) {
                v = antb_realm_global(engine, childRealm);
            } else {
                v = AntRuntime::windowObjectFor(engine, child);
            }
        } else {
            v = !strcmp(name, "contentWindow")
              ? AntRuntime::windowObjectFor(engine, child)
              : AntRuntime::documentObjectFor(engine, child);
        }
    } else if (n->isElementNode() && reflectedAttribute(name)) {
        /* dir/lang/hidden/tabIndex/... : an attribute with a type. */
        const struct ReflectedAttribute* reflected = reflectedAttribute(name);
        WebCore::Element* element = static_cast<WebCore::Element*>(n);
        WebCore::String attribute = element->getAttribute(WebCore::String(reflected->attribute));

        if (reflected->kind == REFLECT_BOOL) {
            v = antb_make_number(engine, attribute.isNull() ? 0 : 1);
        } else if (reflected->kind == REFLECT_NUMBER) {
            int number = (attribute.isNull() || attribute.isEmpty())
                       ? (strcmp(reflected->attribute, "tabindex") == 0 ? -1 : 0)
                       : atoi(attribute.utf8().data());

            v = antb_make_number(engine, (double)number);
        } else {
            v = antb_make_string(engine, attribute.utf8().data());
        }
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
        /* Real DOM exposes this as a method; nodeHasChildNodes() serves it. */
        return 1;
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
            } else if (element->hasTagName(WebCore::HTMLNames::selectTag)) {
                WebCore::String s = static_cast<WebCore::HTMLSelectElement*>(element)->value();
                v = antb_make_string(engine, s.utf8().data());
            } else if (element->hasTagName(WebCore::HTMLNames::optionTag)) {
                WebCore::String s = static_cast<WebCore::HTMLOptionElement*>(element)->value();
                v = antb_make_string(engine, s.utf8().data());
            } else {
                return 0;
            }
        } else {
            return 0;
        }
    } else if (!strcmp(name, "selectedIndex") || !strcmp(name, "selected")
               || !strcmp(name, "index") || !strcmp(name, "text")
               || !strcmp(name, "options") || !strcmp(name, "length")) {
        /* <select>/<option>: the form-control state pages actually read
         * (css3test's filter.js does `evt.target.value`; libraries walk
         * `select.options`). Nothing here existed before: every one of these
         * names came back undefined, so a <select> was paint-only. */
        if (!n->isElementNode())
            return 0;

        WebCore::Element* element = static_cast<WebCore::Element*>(n);

        if (element->hasTagName(WebCore::HTMLNames::selectTag)) {
            WebCore::HTMLSelectElement* select = static_cast<WebCore::HTMLSelectElement*>(element);

            if (!strcmp(name, "selectedIndex")) {
                v = antb_make_number(engine, (double)select->selectedIndex());
            } else if (!strcmp(name, "options")) {
                const Vector<WebCore::HTMLElement*>& items = select->listItems();
                AntbValue* arr = antb_make_array(engine);

                if (!arr)
                    return 0;
                for (unsigned i = 0; i < items.size(); ++i) {
                    if (!items[i]->hasTagName(WebCore::HTMLNames::optionTag))
                        continue;
                    AntbValue* w = wrapNode(engine, items[i]);
                    if (w) {
                        antb_array_push(engine, arr, w);
                        antb_value_release(w);
                    }
                }
                v = arr;
            } else if (!strcmp(name, "length")) {
                unsigned count = 0;
                const Vector<WebCore::HTMLElement*>& items = select->listItems();

                for (unsigned i = 0; i < items.size(); ++i)
                    if (items[i]->hasTagName(WebCore::HTMLNames::optionTag))
                        ++count;
                v = antb_make_number(engine, (double)count);
            } else {
                return 0;
            }
        } else if (element->hasTagName(WebCore::HTMLNames::optionTag)) {
            WebCore::HTMLOptionElement* option = static_cast<WebCore::HTMLOptionElement*>(element);

            if (!strcmp(name, "selected")) {
                v = antb_make_number(engine, option->selected() ? 1 : 0);
            } else if (!strcmp(name, "index")) {
                v = antb_make_number(engine, (double)option->index());
            } else if (!strcmp(name, "text")) {
                v = antb_make_string(engine, option->text().utf8().data());
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
    } else if (n->isElementNode() && (!strcmp(name, "href") || !strcmp(name, "src"))) {
        /* DOM `el.href` / `el.src` return the resolved absolute URL, not the
         * attribute. Returning the raw value made pages that resolve
         * protocol-relative chunk URLs (`//vfiles.gtimg.cn/...`) through a
         * scratch <a> build `https://v.qq.com//vfiles...` and fail the load. */
        WebCore::Element* el = static_cast<WebCore::Element*>(n);
        WebCore::String rel = el->getAttribute(WebCore::AtomicString(name));
        WebCore::String abs = rel.isEmpty() ? WebCore::String() : el->document()->completeURL(rel);
        v = antb_make_string(engine, abs.utf8().data());
    } else if (n->isElementNode()
               && static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::aTag)
               && (!strcmp(name, "pathname") || !strcmp(name, "search") || !strcmp(name, "hash")
                   || !strcmp(name, "hostname") || !strcmp(name, "host") || !strcmp(name, "port")
                   || !strcmp(name, "protocol") || !strcmp(name, "origin"))) {
        /* URL decomposition of <a href>: sites parse URLs by assigning
         * a.href and reading these (v.qq.com's Vue entry died on
         * a.pathname.charAt(0) because every component was undefined). */
        WebCore::Element* el = static_cast<WebCore::Element*>(n);
        WebCore::String href = el->getAttribute(WebCore::HTMLNames::hrefAttr);
        WebCore::KURL base(el->document()->baseURL());
        WebCore::KURL url = href.isEmpty() ? base : WebCore::KURL(base, href.deprecatedString());
        if (url.isMalformed() || url.isEmpty())
            url = base;

        std::string host = url.host().ascii();
        unsigned short port = url.port();
        std::string hostWithPort = host;
        if (port) {
            char buf[16];
            snprintf(buf, sizeof(buf), ":%u", (unsigned)port);
            hostWithPort += buf;
        }

        std::string s;
        if (!strcmp(name, "protocol")) {
            s = url.protocol().ascii();
            if (!s.empty() && s[s.size() - 1] != ':')
                s += ":";
        } else if (!strcmp(name, "hostname")) {
            s = host;
        } else if (!strcmp(name, "host")) {
            s = hostWithPort;
        } else if (!strcmp(name, "port")) {
            s = port ? hostWithPort.substr(host.size() + 1) : "";
        } else if (!strcmp(name, "pathname")) {
            s = url.path().ascii();
            if (s.empty() || s[0] != '/')
                s = "/" + s;
        } else if (!strcmp(name, "search")) {
            if (url.query().isEmpty())
                s = "";
            else
                s = url.query()[0] == '?' ? url.query().ascii() : std::string("?") + url.query().ascii();
        } else if (!strcmp(name, "hash")) {
            s = url.ref().isEmpty() ? "" : std::string("#") + url.ref().ascii();
        } else if (!strcmp(name, "origin")) {
            s = url.protocol().ascii();
            if (!s.empty() && s[s.size() - 1] != ':')
                s += ":";
            s += "//" + hostWithPort;
        }
        v = antb_make_string(engine, s.c_str());
    } else if (n->isElementNode()
               && (!strcmp(name, "async") || !strcmp(name, "defer"))) {
        /* Boolean IDL reflection (script.async / script.defer): scripts are
         * created as plain DOM nodes via createElement and configured with
         * `s.async = true` / `s.defer = true` - without reflection the write
         * landed on the wrapper only and the engine's script classification
         * (defer order, async) never saw it. Real booleans, not 1/0, so the
         * `=== true` style checks third-party loaders use also hold. */
        WebCore::Element* el = static_cast<WebCore::Element*>(n);
        bool present = !strcmp(name, "async") ? el->hasAttribute(WebCore::AtomicString("async"))
                                              : el->hasAttribute(WebCore::HTMLNames::deferAttr);
        AntbValue* b = antb_make_boolean(engine, present ? 1 : 0);
        if (!b)
            return 0;
        v = b;
    } else if (!strcmp(name, "type") && n->isElementNode()
               && static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::selectTag)) {
        /* <select> has no type attribute: the property is select-one /
         * select-multiple (the attribute reflection below would answer ""). */
        v = antb_make_string(engine,
                static_cast<WebCore::HTMLSelectElement*>(n)->multiple()
                    ? "select-multiple" : "select-one");
    } else if (!strcmp(name, "id") || !strcmp(name, "className") || !strcmp(name, "href")
               || !strcmp(name, "src") || !strcmp(name, "srcset") || !strcmp(name, "alt")
               || !strcmp(name, "placeholder") || !strcmp(name, "poster")
               || !strcmp(name, "title") || !strcmp(name, "name") || !strcmp(name, "type")
               || !strcmp(name, "rel") || !strcmp(name, "media")
               || !strcmp(name, "charset") || !strcmp(name, "as")) {
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

        /* A known capability gap: say it once, then keep returning undefined
         * (AgbGapJsdom.h). Feature detection calls this in loops. */
        if (agbJsdomGapPending(name)) {
            char where[96];

            gapNodeWhere(n, where, sizeof(where));
            agbJsdomGapReport(name, where);
        }

        return 0;
    }

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

/* __agbInputValue(wrapper[, newValue]): direct value access for the
 * prototype-level `value` accessor prelude.js installs for React's tracker.
 * It must go straight to the DOM element - the accessor cannot say
 * `this.value`, because React re-exports exactly this descriptor as an own
 * property on the wrapper, so `this.value` inside the getter would re-enter
 * the accessor and recurse until the page hangs. */
int globalInputValue(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    WebCore::Node* n = argc >= 1 ? nodeOf(args[0]) : NULL;
    WebCore::Element* element;

    (void)self;

    if (!n || !n->isElementNode())
        return 0;

    element = static_cast<WebCore::Element*>(n);

    if (argc >= 2) {
        char* text = antb_to_utf8(args[1]);

        if (text) {
            if (element->hasTagName(WebCore::HTMLNames::inputTag))
                static_cast<WebCore::HTMLInputElement*>(element)->setValue(WebCore::String::fromUTF8(text));
            else if (element->hasTagName(WebCore::HTMLNames::textareaTag))
                static_cast<WebCore::HTMLTextAreaElement*>(element)->setValue(WebCore::String::fromUTF8(text));
            antb_free(text);
        }
        return 0;
    }

    if (element->hasTagName(WebCore::HTMLNames::inputTag)) {
        WebCore::String s = static_cast<WebCore::HTMLInputElement*>(element)->value();
        AntbValue* v = antb_make_string(engine, s.utf8().data());
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
            return 1;
        }
    } else if (element->hasTagName(WebCore::HTMLNames::textareaTag)) {
        WebCore::String s = static_cast<WebCore::HTMLTextAreaElement*>(element)->value();
        AntbValue* v = antb_make_string(engine, s.utf8().data());
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
            return 1;
        }
    }

    return 0;
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

    /* Diagnostic: AGB_PROP_DEBUG=1 logs framework marker assignments
     * (__vue_app__/_vnode) with the node they land on. */
    if (name && getenv("AGB_PROP_DEBUG")
        && (!strcmp(name, "__vue_app__") || !strcmp(name, "_vnode"))) {
        const char* tag = (n && n->isElementNode()) ? static_cast<WebCore::Element*>(n)->tagName().deprecatedString().utf8().data() : "?";
        const char* id = (n && n->isElementNode()) ? static_cast<WebCore::Element*>(n)->getAttribute(WebCore::String("id")).deprecatedString().utf8().data() : "";
        fprintf(stderr, "[prop] set %s on <%s id=%s>", name, tag, id);
        fputc(10, stderr);
    }


    /* Event handler properties are listeners, not expandos: `script.onload = f`
     * has to fire when the script finishes. Placed before the null-value guard
     * so `el.onclick = null` clears the handler. */
    if (n && name && !strncmp(name, "on", 2) && name[2])
        return setNodeEventHandler(n, name + 2, value);

    /* Media element properties (currentTime/volume/muted/src/...): the reads
     * are served by the media branch of nodeGetter, the writes land here. */
    if (n && n->isElementNode() && value
            && (static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::videoTag)
                || static_cast<WebCore::Element*>(n)->hasTagName(WebCore::HTMLNames::audioTag))
            && mediaElementSetProperty(static_cast<WebCore::HTMLMediaElement*>(
                   static_cast<WebCore::Element*>(n)), name, value))
        return 1;

    /* <select>/<option> state. Setting `value` must not fire change (the DOM
     * says the property write is silent); selectedIndex goes through
     * setSelectedIndex(index, true, true) because *that* one does dispatch
     * change, and css3test's filter.js listens for it. */
    if (n && n->isElementNode() && value) {
        WebCore::Element* element = static_cast<WebCore::Element*>(n);

        if (element->hasTagName(WebCore::HTMLNames::selectTag)) {
            WebCore::HTMLSelectElement* select = static_cast<WebCore::HTMLSelectElement*>(element);

            if (!strcmp(name, "selectedIndex")) {
                select->setSelectedIndex((int)antb_to_number(value), true, true);
                return 1;
            }
            if (!strcmp(name, "value")) {
                char* text = antb_to_utf8(value);

                if (text) {
                    select->setValue(WebCore::String::fromUTF8(text));
                    antb_free(text);
                }
                return 1;
            }
        } else if (element->hasTagName(WebCore::HTMLNames::optionTag)) {
            WebCore::HTMLOptionElement* option = static_cast<WebCore::HTMLOptionElement*>(element);

            if (!strcmp(name, "selected")) {
                option->setSelected(antb_to_boolean(value) != 0);
                return 1;
            }
            if (!strcmp(name, "value")) {
                char* text = antb_to_utf8(value);

                if (text) {
                    option->setValue(WebCore::String::fromUTF8(text));
                    antb_free(text);
                }
                return 1;
            }
        }
    }

    /* Reflected attributes: `el.hidden = true`, `el.tabIndex = 3`,
     * `el.dir = 'rtl'` are attribute writes, and reading them back must agree
     * with getAttribute (they are read in nodeGetter). */
    if (n && n->isElementNode() && reflectedAttribute(name)) {
        const struct ReflectedAttribute* reflected = reflectedAttribute(name);
        WebCore::Element* element = static_cast<WebCore::Element*>(n);
        WebCore::ExceptionCode ec = 0;

        if (reflected->kind == REFLECT_BOOL) {
            if (value && antb_to_boolean(value))
                element->setAttribute(WebCore::String(reflected->attribute), WebCore::String(""), ec);
            else
                element->removeAttribute(WebCore::String(reflected->attribute), ec);
        } else if (reflected->kind == REFLECT_NUMBER) {
            char buffer[32];

            snprintf(buffer, sizeof(buffer), "%d", (int)antb_to_number(value));
            element->setAttribute(WebCore::String(reflected->attribute), WebCore::String(buffer), ec);
        } else {
            char* text = value ? antb_to_utf8(value) : NULL;

            if (text) {
                element->setAttribute(WebCore::String(reflected->attribute),
                                      WebCore::String::fromUTF8(text), ec);
                antb_free(text);
            }
        }
        return 1;
    }

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
    /* Boolean IDL reflection: `s.async = false` REMOVES the attribute, any
     * truthy assignment adds it (WebIDL converts the value; "" -> false). */
    if (n->isElementNode()
        && (!strcmp(name, "async") || !strcmp(name, "defer"))) {
        WebCore::ExceptionCode ec = 0;

        if (antb_to_boolean(value))
            static_cast<WebCore::Element*>(n)->setAttribute(WebCore::String(name),
                                                            WebCore::String(""), ec);
        else
            static_cast<WebCore::Element*>(n)->removeAttribute(WebCore::String(name), ec);
        return 1;
    }
    /* Attribute reflection for the common reflected properties: `img.src = ...`
     * and friends are plain attribute writes in the DOM. Kept to a whitelist so
     * that arbitrary JS fields (handlers, expando data) never become attributes. */
    if (n->isElementNode() && (antb_is_string(value) || antb_is_number(value))) {
        /* Deliberately narrow: only properties that JS builds at runtime and that
         * the shim has no dedicated setter for. Everything else (id, class, href,
         * value, onclick, expando data...) must stay a plain JS property, or
         * libraries like jQuery break en masse. type/rel/media/charset/as close
         * the script/link creation path (createElement + .type/.rel, no
         * setAttribute), e.g. `s.type = 'module'` before appendChild. */
        static const char* reflected[] = {
            "src", "srcset", "alt", "placeholder", "poster", "width", "height",
            "loading", "decoding", "crossorigin", "as",
            "type", "rel", "media", "charset"
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
        l.realm = currentRealm();
        l.handler = false;

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
                agbLog(AGB_LOG_EVT, "add %s node=%p listeners=%d", type, (void*)n, (int)g_listeners->size());
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
        agbLog(AGB_LOG_EVT, "JS dispatchEvent(%s) on node=%p", type, (void*)n);

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

    /* No extra ref: wrapNode() pins the node for as long as this realm lives,
     * which is exactly the lifetime the wrapper needs. A second, untracked ref
     * here leaked the node (and, through Node::m_document, its whole document)
     * on every tab close (R141). */
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

/* DOM level 1 method. It used to be a numeric property, which broke every
 * caller that does `node.hasChildNodes()` - Vue 3's hydration calls exactly
 * that and the TypeError aborted mount(). */
int nodeHasChildNodes(AntbEngine* engine, AntbValue* self, AntbValue**, int,
                      AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);

    if (n) {
        AntbValue* v = antb_make_number(engine, n->hasChildNodes() ? 1 : 0);

        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
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
int nodeQuerySelectorAll(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                         AntbValue* out, void*)
{
    WebCore::Node* root = nodeOf(self);
    char* selector;
    AntbValue* arr;
    std::vector<AgbSelector::Selector> selectors;

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

    /* The real selector engine (AgbSelector.h): selector lists, combinators,
     * attribute selectors and the common pseudo-classes. It used to be a
     * space-splitter that only understood "#id", ".class" and a tag name, so
     * `[data-x]`, `a[href]`, `ul > li` and `a, b` silently matched nothing. */
    if (AgbSelector::parse(selector, selectors)) {
        bool fastPath = selectors.size() == 1 && selectors[0].steps.size() == 1
                     && !selectors[0].steps[0].conds.empty()
                     && selectors[0].steps[0].conds[0].kind == AgbSelector::COND_TAG;

        if (fastPath) {
            /* `div`, `div.cls`, `div[data-x]`: let the engine find the
             * candidates instead of walking every node in the tree. */
            const WebCore::AtomicString& tag = selectors[0].steps[0].conds[0].name;
            RefPtr<WebCore::NodeList> candidates = root->getElementsByTagName(tag);

            if (candidates) {
                unsigned count = candidates->length();

                for (unsigned i = 0; i < count; ++i) {
                    WebCore::Node* n = candidates->item(i);

                    if (n && n->isElementNode()
                            && AgbSelector::matchesAny(selectors, static_cast<WebCore::Element*>(n)))
                        pushWrappedNode(engine, arr, n);
                }
            }
        } else {
            for (WebCore::Node* n = root->firstChild(); n; n = n->traverseNextNode(root)) {
                if (!n->isElementNode())
                    continue;
                if (AgbSelector::matchesAny(selectors, static_cast<WebCore::Element*>(n)))
                    pushWrappedNode(engine, arr, n);
            }
        }
    }

    antb_free(selector);
    antb_value_assign(out, arr);
    antb_value_release(arr);
    return 1;
}

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
        if (const char* qsFilter = getenv("AGB_QS_DEBUG")) {
            /* Diagnostic: AGB_QS_DEBUG=<substring> logs every querySelector
             * whose selector contains the substring, with the hit/miss. */
            std::string sel = argc >= 1 ? antb_to_utf8(args[0]) : std::string();
            if (*qsFilter && sel.find(qsFilter) != std::string::npos)
                fprintf(stderr, "[qs] sel=%s found=%d\n", sel.c_str(), first ? 1 : 0);
        }
        if (first) {
            antb_value_assign(out, first);
            antb_value_release(first);
        } else
            assignNull(engine, out);   /* null, not undefined (R95) */
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

/* Synchronous GET with the response metadata: (url) -> {status, statusText,
 * headers, body} or undefined on network failure. Used by the shim's sync
 * branch, which needs the same header view the async path gets.
 *
 * Kept separate from globalHttpGet so the older one-value contract stays:
 * a shim paired with an older engine must keep working. */
int globalHttpGetEx(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
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
            AntbValue* result = antb_make_object(engine);

            if (!result)
                return 1;

            {
                AntbValue* status = antb_make_number(engine, (double)response.httpStatusCode());
                AntbValue* text = antb_make_string(engine, response.httpStatusText().utf8().data());
                AntbValue* body = antb_make_string(
                    engine, WebCore::String::fromUTF8(data.data(), data.size()).utf8().data());

                if (status) { antb_set_prop(engine, result, "status", status); antb_value_release(status); }
                if (text)   { antb_set_prop(engine, result, "statusText", text); antb_value_release(text); }
                if (body)   { antb_set_prop(engine, result, "body", body); antb_value_release(body); }
            }

            {
                const WebCore::HTTPHeaderMap& fields = response.httpHeaderFields();
                WebCore::HTTPHeaderMap::const_iterator end = fields.end();
                AntbValue* list = antb_make_array(engine);

                if (list) {
                    for (WebCore::HTTPHeaderMap::const_iterator it = fields.begin(); it != end; ++it) {
                        AntbValue* pair = antb_make_array(engine);
                        AntbValue* name = antb_make_string(engine, it->first.utf8().data());
                        AntbValue* value = antb_make_string(engine, it->second.utf8().data());

                        if (!pair || !name || !value) {
                            if (pair) antb_value_release(pair);
                            if (name) antb_value_release(name);
                            if (value) antb_value_release(value);
                            continue;
                        }

                        antb_array_push(engine, pair, name);
                        antb_array_push(engine, pair, value);
                        antb_array_push(engine, list, pair);
                        antb_value_release(pair);
                        antb_value_release(name);
                        antb_value_release(value);
                    }

                    antb_set_prop(engine, result, "headers", list);
                    antb_value_release(list);
                }
            }

            antb_value_assign(out, result);
            antb_value_release(result);
        }
    }
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
            /* The response bytes are UTF-8, not Latin-1: WebCore::String(ptr,
             * len) maps every *byte* to U+0000..U+00FF, so feeding it UTF-8 and
             * then calling .utf8() double-encodes (E9 A2 84 became C3 A9 C2 A2
             * C2 84 -> "é¢„" instead of "预"). Decode it as UTF-8 once. */
            WebCore::String body = WebCore::String::fromUTF8(data.data(), data.size());
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

    /* A detached / display:none node still has to yield a zero rect: returning
     * without filling `out` handed JS `undefined`, so the ordinary
     * `el.getBoundingClientRect().height` threw a TypeError (vConsole measures
     * its own hidden panel that way while booting). */
    if (n && n->renderer()) {
        /* A node appended since the last layout still carries its stale box
         * (0x0); Chrome's getBoundingClientRect() forces layout first, and a
         * freshly mounted <video> measured 0x0 here while its computed style
         * already reported 320px. */
        if (WebCore::Document* doc = n->document()) {
            if (doc->renderer() && doc->renderer()->needsLayout())
                doc->updateLayout();
        }

        rect = n->renderer()->absoluteBoundingBoxRect();
        /* absoluteBoundingBoxRect() is in document coordinates; the DOM API is
         * viewport-relative (Chrome: a scrolled document reports a negative
         * top). Without this, scrolling changed nothing for measured geometry. */
        if (WebCore::Document* doc = n->document())
            if (WebCore::FrameView* v = doc->view())
                rect.move(-v->contentsX(), -v->contentsY());
    }

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

/* ---- internal pages (agave://) ------------------------------------------
 *
 * An internal page runs in its own realm like any document, but its realm gets
 * the browser's own API: listing, cancelling and opening downloads. A web page
 * must never see these names - they expose the user's file paths - so they are
 * registered only when the document being bound is an agave:// page
 * (bindDocument) and internal.js is the only script that uses them. */
static bool isInternalDocument()
{
    if (!g_document)
        return false;

    return equalIgnoringCase(WebCore::KURL(g_document->URL()).protocol(), "agave");
}

static void setNumberProp(AntbEngine* engine, AntbValue* object, const char* name, double value)
{
    AntbValue* v = antb_make_number(engine, value);

    if (!v)
        return;

    antb_set_prop(engine, object, name, v);
    antb_value_release(v);
}

static void setStringProp(AntbEngine* engine, AntbValue* object, const char* name, const WebCore::String& value)
{
    WebCore::CString bytes = value.utf8();
    AntbValue* v = antb_make_string(engine, bytes.data());

    if (!v)
        return;

    antb_set_prop(engine, object, name, v);
    antb_value_release(v);
}

int globalDownloadList(AntbEngine* engine, AntbValue*, AntbValue**, int, AntbValue* out, void*)
{
    WebCore::DownloadManager& manager = WebCore::DownloadManager::shared();
    AntbValue* array;
    int i;

    if (!engine || !out)
        return 1;

    array = antb_make_array(engine);

    if (!array)
        return 1;

    for (i = 0; i < manager.jobCount(); ++i) {
        WebCore::DownloadJob* job = manager.jobAt(i);
        AntbValue* item;

        if (!job)
            continue;

        item = antb_make_object(engine);

        if (!item)
            continue;

        setNumberProp(engine, item, "id", job->id());
        setNumberProp(engine, item, "state", job->mcState());
        setNumberProp(engine, item, "received", (double)job->received());
        setNumberProp(engine, item, "total", (double)job->total());
        setStringProp(engine, item, "url", job->url());
        setStringProp(engine, item, "path", job->path());
        setStringProp(engine, item, "mime", job->mime());

        antb_array_push(engine, array, item);
        antb_value_release(item);
    }

    antb_value_assign(out, array);
    antb_value_release(array);
    return 1;
}

int globalDownloadCancel(AntbEngine* engine, AntbValue*, AntbValue** args, int argc, AntbValue* out, void*)
{
    int id = (argc > 0) ? (int)antb_to_number(args[0]) : 0;
    bool ok = WebCore::DownloadManager::shared().cancel(id);

    if (engine && out)
        makeNumberResult(engine, out, ok ? 1.0 : 0.0);

    return 1;
}

int globalDownloadOpen(AntbEngine* engine, AntbValue*, AntbValue** args, int argc, AntbValue* out, void*)
{
    int id = (argc > 0) ? (int)antb_to_number(args[0]) : 0;
    bool ok = WebCore::DownloadManager::shared().openFile(id);

    if (engine && out)
        makeNumberResult(engine, out, ok ? 1.0 : 0.0);

    return 1;
}

/* DownloadManager::emit -> here. Only an internal realm gets the DOM event:
 * a web page must not even learn that a download changed. */
static void downloadEventSink(int, int)
{
    if (!isInternalDocument())
        return;

    AntRuntime::dispatchWindowEventState("agavedownloadchange", NULL);
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

/* __agbClipboardSetText(text) -> 1 on success.
 *
 * The 2008 editor writes the clipboard through Pasteboard -> the host's
 * cb_clipboard_set_data, but only from a selection (ctrl+c). A page (or the
 * injected inspector.js) needs to put an arbitrary string there; without this
 * the only copy path is document.execCommand('copy'), which this engine does
 * not implement. Runs on the engine thread, the same thread ctrl+c uses, so
 * OpenClipboard(NULL) in the control's callback behaves identically. */
int globalClipboardSetText(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                           AntbValue* out, void*)
{
    char* text = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
    int ok = 0;

    if (text && text[0] && MaCross::cb_clipboard_set_data) {
        MaCross::cb_clipboard_set_data(text, (unsigned int)strlen(text));
        ok = 1;
    }

    antb_free(text);

    {
        AntbValue* v = antb_make_number(engine, ok ? 1.0 : 0.0);

        if (!v)
            return 0;

        antb_value_assign(out, v);
        antb_value_release(v);
    }
    return 1;
}

/* window.postMessage / message event. Frames share the window object in this
 * port, so a message from an iframe has to reach the parent's listeners even
 * though they are registered against a different Document node; the normal
 * per-node dispatch would drop it. The origin is the URL of the document that
 * is running right now (the iframe), which is what receivers check. */
int globalFramePostMessage(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                           AntbValue* out, void*)
{
    AntbValue* event;
    AntbValue* global;

    (void)out;

    if (!engine || argc < 1 || !g_listeners)
        return 1;

    event = antb_make_object(engine);
    if (!event)
        return 1;

    antb_set_prop(engine, event, "data", args[0]);

    {
        WebCore::String origin("null");

        if (g_document) {
            WebCore::KURL url(g_document->URL());
            WebCore::String protocol = url.protocol();

            if (!protocol.isEmpty()) {
                origin = protocol + "://" + url.host();
                if (url.port())
                    origin = origin + ":" + WebCore::String::number((unsigned)url.port());
            }
        }

        if (getenv("AGB_EVT_DEBUG"))
            agbLog(AGB_LOG_EVT, "postMessage origin=%s", origin.utf8().data());

        {
            AntbValue* value = antb_make_string(engine, origin.utf8().data());

            if (value) {
                antb_set_prop(engine, event, "origin", value);
                antb_value_release(value);
            }
        }
    }

    global = antb_global(engine);
    if (global)
        antb_set_prop(engine, event, "source", global);

    {
        std::vector<AntbValue*> targets;

        /* Retain first: a listener may remove itself while we dispatch. */
        for (size_t i = 0; i < g_listeners->size(); ++i) {
            Listener& l = (*g_listeners)[i];

            if (l.type == "message" && l.fn)
                targets.push_back(antb_retain(l.fn));
        }

        for (size_t i = 0; i < targets.size(); ++i) {
            AntbValue* result = antb_call(engine, targets[i], global, &event, 1, NULL);

            if (result)
                antb_value_release(result);
            antb_value_release(targets[i]);
        }
    }

    if (global)
        antb_value_release(global);
    antb_value_release(event);
    return 1;
}

/* window.console.*: the polyfills guard formats the arguments and passes one
 * line here, so page console output shares the engine timeline
 * ("[ 123.4ms] [con-error] ...") instead of going straight to ant's console.
 *
 * Two entry points on purpose: __agbConsoleLineEx(level, text) carries the
 * level and becomes the tag ([con-log]/[con-info]/[con-warn]/[con-error]/
 * [con-debug]); the one-argument __agbConsoleLine is the older sink and stays
 * for engine/JS skew (a new polyfills.js next to an old engine must still
 * print the text, and an old polyfills.js next to this engine must not lose
 * it either). */
int globalConsoleLine(AntbEngine*, AntbValue*, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    char* text = (argc > 0) ? antb_to_utf8(args[0]) : NULL;

    if (text) {
        agbLogConsole(NULL, "%s", text);
        antb_free(text);
    }

    (void)out;
    return 1;
}

int globalConsoleLineEx(AntbEngine*, AntbValue*, AntbValue** args, int argc,
                        AntbValue* out, void*)
{
    char* level = (argc > 1) ? antb_to_utf8(args[0]) : NULL;
    char* text = (argc > 1) ? antb_to_utf8(args[1])
                            : (argc > 0 ? antb_to_utf8(args[0]) : NULL);

    if (text) {
        agbLogConsole(level, "%s", text);
        antb_free(text);
    }
    if (level)
        antb_free(level);

    (void)out;
    return 1;
}

/* Length -> text for __agbLayoutInfo ("auto", "200", "100%"). */
static const char* layoutLengthText(const WebCore::Length& len, char* buf, size_t size)
{
    if (len.isAuto())
        snprintf(buf, size, "auto");
    else if (len.isPercent())
        snprintf(buf, size, "%d%%", (int)len.percent());
    else
        snprintf(buf, size, "%d", (int)len.value());

    return buf;
}

/* __agbLayoutInfo(node): the render-tree facts a page script cannot otherwise
 * see.
 *
 * offsetWidth == 0 has two completely different causes and a computed style
 * cannot tell them apart: the element has no renderer at all (its style said
 * display:none the last time styles were recalculated) or it has a renderer
 * that was never laid out (a dirty flag that got lost). Those need opposite
 * fixes, so this dump separates them: renderer=0 means "no render tree
 * object", renderer=1 with w=0 means "attached but never laid out", and the
 * parent/self dirty bits say who was supposed to relayout whom. */
int globalLayoutInfo(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char text[512];

    if (argc < 1 || !nodeOf(args[0])) {
        strcpy(text, "node=null");
    } else {
        WebCore::Node* n = nodeOf(args[0]);
        WebCore::RenderObject* r = n->renderer();
        WebCore::RenderObject* p = r ? r->parent() : 0;
        char styleText[128];

        styleText[0] = 0;
        if (r && r->style()) {
            char hBuf[32];
            char wBuf[32];
            WebCore::RenderStyle* s = r->style();

            snprintf(styleText, sizeof(styleText), " disp=%d sh=%s sw=%s",
                     (int)s->display(),
                     layoutLengthText(s->height(), hBuf, sizeof(hBuf)),
                     layoutLengthText(s->width(), wBuf, sizeof(wBuf)));
        }

        snprintf(text, sizeof(text),
                 "inDoc=%d renderer=%d type=%s x=%d y=%d w=%d h=%d"
                 " self=%d child=%d parent=%s pw=%d ph=%d pself=%d pchild=%d%s",
                 n->inDocument() ? 1 : 0,
                 r ? 1 : 0,
                 (r && r->renderName()) ? r->renderName() : "-",
                 r ? r->xPos() : 0,
                 r ? r->yPos() : 0,
                 r ? r->width() : 0,
                 r ? r->height() : 0,
                 (r && r->selfNeedsLayout()) ? 1 : 0,
                 (r && r->normalChildNeedsLayout()) ? 1 : 0,
                 (p && p->renderName()) ? p->renderName() : "-",
                 p ? p->width() : 0,
                 p ? p->height() : 0,
                 (p && p->selfNeedsLayout()) ? 1 : 0,
                 (p && p->normalChildNeedsLayout()) ? 1 : 0,
                 styleText);
    }

    {
        AntbValue* value = antb_make_string(engine, text);

        if (!value)
            return 1;
        antb_value_assign(out, value);
        antb_value_release(value);
    }
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

/* __agbPrefersDark(): the same flag the (prefers-color-scheme) media query
 * uses, so matchMedia and the stylesheet agree. */
int globalPrefersDark(AntbEngine* engine, AntbValue*, AntbValue**, int, AntbValue* out, void*)
{
    antb_value_assign(out, antb_make_number(engine, WebCore::prefersDarkColorScheme() ? 1 : 0));
    return 1;
}

/* __agbViewportWidth() / __agbViewportHeight(): the layout viewport of the
 * document passed in (the prelude passes the realm's own `document`, so
 * `iframe.contentWindow.innerWidth` answers for the iframe even when the
 * caller is the parent page), scrollbar included - window.innerWidth
 * semantics, and the same number 100vw and the @media width queries use.
 * `user` picks the axis. documentElement.clientWidth is 16px narrower once a
 * scrollbar is present, so the prelude's innerWidth getter must not be built
 * on it. */
int globalViewportSize(AntbEngine* engine, AntbValue*, AntbValue** args, int argc, AntbValue* out, void* user)
{
    int wantWidth = (user == 0);
    int value = 0;
    WebCore::Document* d = g_document;

    if (argc > 0 && args[0]) {
        WebCore::Node* node = nodeOf(args[0]);

        if (node && node->document())
            d = node->document();
    }

    if (d && d->view()) {
        WebCore::FrameView* view = d->view();

        value = wantWidth ? view->width() : view->height();

        if (WebCore::RenderObject* renderer = d->renderer())
            value = WebCore::adjustForAbsoluteZoom(value, renderer);
    }

    antb_value_assign(out, antb_make_number(engine, (double)value));
    return 1;
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

        /* Custom properties ('--x') are not part of the generated property
         * table, so answer them from the style's resolved map. */
        bool isCustomProperty = (property[0] == '-' && property[1] == '-');

        if (isCustomProperty) {
            WebCore::RenderStyle* style = pseudoStyle ? pseudoStyle : node->computedStyle();

            if (style && style->customProperties()) {
                /* The sheet text renames '--x' to '-agb-x' so the CSS tokenizer
                 * accepts the name; the map is keyed by that rewritten name. */
                WebCore::String name = WebCore::String("-agb-") + WebCore::String(property + 2);
                WebCore::String custom = style->customProperties()->getProperty(name);

                if (!custom.isNull()) {
                    /* The value text also carries the rewritten spelling of any
                     * var() it references; hand back the author's names. */
                    WebCore::String restored = WebCore::CSSParser::restoreCustomPropertyNames(custom);
                    value = std::string(restored.utf8().data());
                }
            }
        }

        RefPtr<WebCore::CSSComputedStyleDeclaration> declaration =
            isCustomProperty ? 0 :
            (pseudoStyle ? new WebCore::CSSComputedStyleDeclaration(node, pseudoStyle)
                         : new WebCore::CSSComputedStyleDeclaration(node));

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

/* __agbStylePropertyNames(): camelCase DOM name of every CSS property the
 * engine knows (generated from CSSPropertyNames.in into StylePropertyNames.h).
 * The prelude builds the getComputedStyle() accessors from this instead of a
 * hand-maintained list: that list had drifted, so `cs.margin`, `cs.padding`,
 * `cs.border`, `cs.minWidth` and `cs.alignSelf` simply did not exist (a
 * `getComputedStyle()` property read returned undefined while Chrome answers
 * with the computed value). */
int globalStylePropertyNames(AntbEngine* engine, AntbValue*, AntbValue**, int, AntbValue* out, void*)
{
    AntbValue* list = antb_make_array(engine);

    if (!list)
        return 0;

    for (int i = 0; i < kStylePropertyNameCount; ++i) {
        AntbValue* name = antb_make_string(engine, kStylePropertyNames[i]);

        if (!name)
            continue;

        antb_array_push(engine, list, name);
        antb_value_release(name);
    }

    antb_value_assign(out, list);
    antb_value_release(list);
    return 1;
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
    /* With per-frame realms the number identifies the realm (0 = main frame,
     * 1..N = frame realms), not a per-view document slot: a slot of another
     * realm must never be returned, or __agbBind would restore the parent
     * document inside a child frame (R130). */
    AntbValue* v = antb_make_number(engine, (double)currentRealmSlotNumber());

    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int globalActivateSlot(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    int previous = currentRealmSlotNumber();

    if (argc > 0) {
        int slot = (int)antb_to_number(args[0]);

        if (perFrameRealmEnabled()) {
            /* Switch realm AND its document; the (realm -> document) mapping
             * decides, so a document object is never repointed at another
             * realm's document. */
            if (slot == 0 && g_currentState && g_currentState != &g_emptyState
                    && g_currentState->mainRealm) {
                antb_realm_enter(engine, g_currentState->mainRealm);
                applyRealmBinding(g_currentState->mainRealm);
            } else if (slot > 0) {
                DocumentRuntime* document = documentForFrameSlot(slot);

                if (document && document->realm) {
                    antb_realm_enter(engine, document->realm);
                    applyRealmBinding(document->realm);
                }
            }
        } else if (slot >= 0 && slot < g_docSlotCount) {
            activateSlot(slot);
        }
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

/* ------------------------------------------------------------------------- */
/* Media elements                                                            */
/*                                                                            */
/* Same shape as the canvas block above: there is one Node class, so the      */
/* methods live on it and each one checks the tag before doing anything.      */
/* The DOM side owns all state (HTMLMediaElement); this file only marshals.  */
/* ------------------------------------------------------------------------- */

#define AGB_TAG_TEXTTRACK 0x54545231u   /* 'TTR1' */
#define AGB_TAG_TRACKLIST 0x54524C31u   /* 'TRL1' */
#define AGB_TAG_CUE       0x43554531u   /* 'CUE1' */

static AntbClass* g_textTrackClass = NULL;
static AntbClass* g_textTrackListClass = NULL;
static AntbClass* g_textTrackCueClass = NULL;

/* The media element property table: one function serves every name so the
 * Node class stays the only class (see the canvas block above for the same
 * reasoning). Definitions live in the media section below the canvas one. */
bool mediaElementProperty(AntbEngine*, WebCore::HTMLMediaElement*, const char*, AntbValue* out);
bool mediaElementSetProperty(WebCore::HTMLMediaElement*, const char*, const AntbValue*);

/* Declared here because the wrapper helpers below install them. */
int textTrackGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int textTrackListGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int textTrackCueGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
void installTextTrackClasses(AntbEngine*);

static WebCore::HTMLMediaElement* mediaElementOf(AntbValue* self)
{
    WebCore::Node* n = self ? nodeOf(self) : 0;

    if (!n || !n->isElementNode())
        return 0;

    {
        WebCore::Element* el = static_cast<WebCore::Element*>(n);

        if (!el->hasTagName(WebCore::HTMLNames::videoTag)
                && !el->hasTagName(WebCore::HTMLNames::audioTag))
            return 0;

        return static_cast<WebCore::HTMLMediaElement*>(el);
    }
}

static int mediaReturnBool(AntbEngine* engine, AntbValue* out, bool value)
{
    AntbValue* v = antb_make_number(engine, value ? 1 : 0);

    if (!v)
        return 1;
    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int nodeMediaPlay(AntbEngine* engine, AntbValue* self, AntbValue**, int, AntbValue* out, void*)
{
    WebCore::HTMLMediaElement* media = mediaElementOf(self);

    if (!media)
        return 1;

    media->play();

    /* The spec returns a Promise; players do `.catch(...)` on it and a
     * non-Promise makes `play().catch` throw inside their code. The transport
     * starts synchronously here, so an already-resolved Promise is accurate. */
    {
        AntbValue* global = antb_global(engine);
        AntbValue* promise = NULL;

        if (global) {
            AntbValue* ctor = antb_get_prop(engine, global, "Promise");
            AntbValue* resolved = NULL;

            if (ctor) {
                AntbValue* resolveFn = antb_get_prop(engine, ctor, "resolve");

                if (resolveFn) {
                    resolved = antb_call(engine, resolveFn, ctor, NULL, 0, NULL);
                    antb_value_release(resolveFn);
                }
                antb_value_release(ctor);
            }
            antb_value_release(global);

            promise = resolved;
        }

        if (promise) {
            antb_value_assign(out, promise);
            antb_value_release(promise);
        }
    }

    return 1;
}

int nodeMediaPause(AntbEngine*, AntbValue* self, AntbValue**, int, AntbValue*, void*)
{
    WebCore::HTMLMediaElement* media = mediaElementOf(self);

    if (media)
        media->pause();
    return 1;
}

int nodeMediaLoad(AntbEngine*, AntbValue* self, AntbValue**, int, AntbValue*, void*)
{
    WebCore::HTMLMediaElement* media = mediaElementOf(self);

    if (media)
        media->load();
    return 1;
}

int nodeMediaCanPlayType(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                         AntbValue* out, void*)
{
    WebCore::HTMLMediaElement* media = mediaElementOf(self);
    std::string type;

    if (!media)
        return 1;

    if (argc > 0 && args[0]) {
        char* text = antb_to_utf8(args[0]);

        if (text) {
            type = text;
            antb_free(text);
        }
    }

    return canvasReturnString(engine, out, media->canPlayType(WebCore::String::fromUTF8(type.c_str())).utf8().data());
}

int nodeMediaAddTextTrack(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                          AntbValue* out, void*)
{
    WebCore::HTMLMediaElement* media = mediaElementOf(self);
    std::string kind, label, language;

    if (!media)
        return 1;

    if (argc > 0 && args[0]) { char* t = antb_to_utf8(args[0]); if (t) { kind = t; antb_free(t); } }
    if (argc > 1 && args[1]) { char* t = antb_to_utf8(args[1]); if (t) { label = t; antb_free(t); } }
    if (argc > 2 && args[2]) { char* t = antb_to_utf8(args[2]); if (t) { language = t; antb_free(t); } }

    {
        RefPtr<WebCore::TextTrack> track = new WebCore::TextTrack(
            WebCore::String::fromUTF8(kind.c_str()),
            WebCore::String::fromUTF8(label.c_str()),
            WebCore::String::fromUTF8(language.c_str()));

        /* addTextTrack() per spec creates a track that already shows cues. */
        track->setMode("hidden");
        media->addTextTrack(track.get());

        {
            AntbValue* v = wrapTextTrack(engine, track.get());

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
    }

    return 1;
}

/* textTracks is a live view: the list object asks its own getter, so
 * `el.textTracks.length` is always current. */
int mediaTextTracksGetter(AntbEngine* engine, AntbValue* self, const char* name,
                          AntbValue* out, void*)
{
    WebCore::HTMLMediaElement* media = mediaElementOf(self);

    if (!media || !name)
        return 0;

    if (!strcmp(name, "textTracks")) {
        AntbValue* list = wrapTextTrackList(engine, media->textTracks());

        if (!list)
            return 0;

        antb_value_assign(out, list);
        antb_value_release(list);
        return 1;
    }

    return 0;
}

/* The list needs item()/getTrackById() as real methods. They are installed on
 * the class prototype per engine; this is the shared callback. `user` is the
 * operation id so one callback can serve both. */
enum TextTrackListOp {
    TRACKLIST_OP_ITEM = 1,
    TRACKLIST_OP_BY_ID = 2
};

int textTrackListMethod(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                        AntbValue* out, void* user)
{
    WebCore::TextTrackList* list = self
        ? (WebCore::TextTrackList*)antb_get_native(self, AGB_TAG_TRACKLIST) : 0;
    intptr_t op = (intptr_t)user;
    WebCore::TextTrack* track = 0;

    if (!list)
        return 1;

    if (op == TRACKLIST_OP_ITEM) {
        unsigned index = (argc > 0 && args[0]) ? (unsigned)antb_to_number(args[0]) : 0;

        track = list->item(index);
    } else {
        std::string id;

        if (argc > 0 && args[0]) {
            char* text = antb_to_utf8(args[0]);
            if (text) { id = text; antb_free(text); }
        }
        track = list->getTrackById(WebCore::String::fromUTF8(id.c_str()));
    }

    if (!track)
        return 1;

    {
        AntbValue* v = wrapTextTrack(engine, track);

        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }

    return 1;
}

int textTrackListMethod(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                        AntbValue* out, void* user);

/* --- text track wrappers ------------------------------------------------- */

static AntbValue* mediaWrapObject(AntbEngine* engine, AntbClass* klass, void* ptr, unsigned tag)
{
    AntbValue* v;

    if (!engine || !ptr || !klass)
        return NULL;

    v = antb_wrap_native_as_owner(engine, klass, ptr, tag, g_document);
    if (!v)
        return NULL;

    /* Wrappers are released with the document (antb_release_owner); the C++
     * objects themselves are owned by the media element. */
    return v;
}

/* One class per media wrapper type. There is no per-tag class scheme here
 * (every element shares the Node class), so these are created lazily on the
 * first document bind - they cost three slots in the bridge's class table. */
void installTextTrackClasses(AntbEngine* engine)
{
    if (!engine || g_textTrackClass)
        return;

    g_textTrackClass = antb_class_create(engine, "TextTrack");
    g_textTrackListClass = antb_class_create(engine, "TextTrackList");
    g_textTrackCueClass = antb_class_create(engine, "TextTrackCue");

    if (g_textTrackClass)
        antb_class_set_property(engine, g_textTrackClass, "", textTrackGetter, NULL);
    if (g_textTrackListClass) {
        antb_class_add_method(engine, g_textTrackListClass, "item", textTrackListMethod,
                              (void*)TRACKLIST_OP_ITEM);
        antb_class_add_method(engine, g_textTrackListClass, "getTrackById", textTrackListMethod,
                              (void*)TRACKLIST_OP_BY_ID);
        antb_class_set_property(engine, g_textTrackListClass, "", textTrackListGetter, NULL);
    }
    if (g_textTrackCueClass)
        antb_class_set_property(engine, g_textTrackCueClass, "", textTrackCueGetter, NULL);
}

AntbValue* wrapTextTrack(AntbEngine* engine, WebCore::TextTrack* track)
{
    if (!engine || !track)
        return NULL;

    {
        AntbValue* v = mediaWrapObject(engine, g_textTrackClass, track, AGB_TAG_TEXTTRACK);

        if (v)
            antb_define_property(engine, v, "", textTrackGetter, NULL);
        return v;
    }
}

AntbValue* wrapTextTrackList(AntbEngine* engine, WebCore::TextTrackList* list)
{
    if (!engine || !list)
        return NULL;

    {
        AntbValue* v = mediaWrapObject(engine, g_textTrackListClass, list, AGB_TAG_TRACKLIST);

        if (v) {
            /* The list is a live view: `length` and index reads go through
             * one getter that asks TextTrackList every time. */
            antb_define_property(engine, v, "", textTrackListGetter, NULL);
        }
        return v;
    }
}

AntbValue* wrapTextTrackCue(AntbEngine* engine, WebCore::TextTrackCue* cue)
{
    if (!engine || !cue)
        return NULL;

    {
        AntbValue* v = mediaWrapObject(engine, g_textTrackCueClass, cue, AGB_TAG_CUE);

        if (v)
            antb_define_property(engine, v, "", textTrackCueGetter, NULL);
        return v;
    }
}

/* --- property table ------------------------------------------------------ *
 * Every media property read funnels through this one function (registered on
 * the Node class as the catch-all getter's media branch). Besides the obvious
 * state reads it synthesizes the two object-valued properties:
 *
 *   textTracks -> a live TextTrackList wrapper
 *   buffered/played/seekable -> TimeRanges-shaped objects
 *   error      -> MediaError-shaped object (null when there is no error)
 */
static AntbValue* makeTimeRanges(AntbEngine* engine, double end)
{
    AntbValue* ranges = antb_make_object(engine);

    if (!ranges)
        return NULL;

    /* One range [0, end] while playing/loaded, none before. These objects are
     * snapshots: TimeRanges is live in Chrome, but every consumer we have
     * seen reads it right after the event that made it interesting. */
    {
        AntbValue* length = antb_make_number(engine, end > 0 ? 1 : 0);
        AntbValue* method = NULL;

        if (length) {
            antb_set_prop(engine, ranges, "length", length);
            antb_value_release(length);
        }

        /* start/end as functions on the object. */
        {
            AntbValue* global = antb_global(engine);

            if (global) {
                AntbValue* marker = antb_get_prop(engine, global, "__agbTimeRangesFactory");

                if (marker) {
                    AntbValue* args[1];

                    args[0] = antb_make_number(engine, end);
                    method = antb_call(engine, marker, global, args, 1, NULL);
                    if (args[0])
                        antb_value_release(args[0]);
                    antb_value_release(marker);
                }
                antb_value_release(global);
            }
        }

        if (method) {
            AntbValue* startFn = antb_get_prop(engine, method, "start");
            AntbValue* endFn = antb_get_prop(engine, method, "end");

            if (startFn) {
                antb_set_prop(engine, ranges, "start", startFn);
                antb_value_release(startFn);
            }
            if (endFn) {
                antb_set_prop(engine, ranges, "end", endFn);
                antb_value_release(endFn);
            }
            antb_value_release(method);
        }
    }

    return ranges;
}

bool mediaElementProperty(AntbEngine* engine, WebCore::HTMLMediaElement* media,
                          const char* name, AntbValue* out)
{
    if (!media || !name)
        return false;

    /* --- plain numbers --- */
    if (!strcmp(name, "currentTime"))
        { AntbValue* v = antb_make_number(engine, media->currentTime()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "duration"))
        { AntbValue* v = antb_make_number(engine, media->duration()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "volume"))
        { AntbValue* v = antb_make_number(engine, media->volume()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "playbackRate"))
        { AntbValue* v = antb_make_number(engine, media->playbackRate()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "defaultPlaybackRate"))
        { AntbValue* v = antb_make_number(engine, media->defaultPlaybackRate()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "readyState"))
        { AntbValue* v = antb_make_number(engine, (double)media->readyState()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "networkState"))
        { AntbValue* v = antb_make_number(engine, (double)media->networkState()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "videoWidth"))
        { AntbValue* v = antb_make_number(engine, media->videoWidth()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "videoHeight"))
        { AntbValue* v = antb_make_number(engine, media->videoHeight()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }

    /* --- booleans --- */
    if (!strcmp(name, "paused"))
        { AntbValue* v = antb_make_number(engine, media->paused() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "ended"))
        { AntbValue* v = antb_make_number(engine, media->ended() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "seeking"))
        { AntbValue* v = antb_make_number(engine, media->seeking() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "muted"))
        { AntbValue* v = antb_make_number(engine, media->muted() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "loop"))
        { AntbValue* v = antb_make_number(engine, media->loop() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "autoplay"))
        { AntbValue* v = antb_make_number(engine, media->autoplay() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "controls"))
        { AntbValue* v = antb_make_number(engine, media->controls() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "defaultMuted"))
        { AntbValue* v = antb_make_number(engine, media->defaultMuted() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }

    /* --- strings --- */
    if (!strcmp(name, "src"))
        { AntbValue* v = antb_make_string(engine, media->src().utf8().data()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "currentSrc"))
        { AntbValue* v = antb_make_string(engine, media->currentSrc().utf8().data()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }

    /* Diagnostic (not a spec property): 1 when a real decoder drives this
     * element, 0 when the simulated ticker is the fallback (no ffmpeg).
     * tools/run_media_tests.ps1 uses it to tell the two apart. */
    if (!strcmp(name, "_agbRealPlayback"))
        { AntbValue* v = antb_make_number(engine, media->isRealPlayback() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "poster"))
        { AntbValue* v = antb_make_string(engine, media->poster().utf8().data()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "preload"))
        { AntbValue* v = antb_make_string(engine, media->preload().utf8().data()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }

    /* @@toStringTag-ish marker so library feature detection can tell the
     * constructor name (constructor itself is answered by nodeGetter). */
    if (!strcmp(name, "textTracks")) {
        AntbValue* list = wrapTextTrackList(engine, media->textTracks());

        if (!list)
            return true;
        antb_value_assign(out, list);
        antb_value_release(list);
        return true;
    }

    if (!strcmp(name, "buffered") || !strcmp(name, "played") || !strcmp(name, "seekable")) {
        /* buffered = how far the bytes reach (the download frontier while a
         * resource streams in), played = the clock, seekable = the whole
         * resource (progressive download; no ranges yet). */
        double end;

        if (!strcmp(name, "buffered"))
            end = media->bufferedEnd();
        else if (!strcmp(name, "seekable"))
            end = media->duration();
        else
            end = media->currentTime();

        AntbValue* ranges = makeTimeRanges(engine, end);

        if (!ranges)
            return true;
        antb_value_assign(out, ranges);
        antb_value_release(ranges);
        return true;
    }

    if (!strcmp(name, "error")) {
        if (!media->hasError())
            return true;   /* null */

        {
            AntbValue* err = antb_make_object(engine);

            if (!err)
                return true;

            {
                AntbValue* code = antb_make_number(engine, media->errorCode());
                AntbValue* message = antb_make_string(engine, "MEDIA_ERR");

                if (code) { antb_set_prop(engine, err, "code", code); antb_value_release(code); }
                if (message) { antb_set_prop(engine, err, "message", message); antb_value_release(message); }
            }

            antb_value_assign(out, err);
            antb_value_release(err);
            return true;
        }
    }

    return false;
}

bool mediaElementSetProperty(WebCore::HTMLMediaElement* media, const char* name,
                             const AntbValue* value)
{
    if (!media || !name || !value)
        return false;

    if (!strcmp(name, "currentTime")) {
        media->setCurrentTime(antb_to_number(value));
        return true;
    }
    if (!strcmp(name, "volume")) {
        media->setVolume(antb_to_number(value));
        return true;
    }
    if (!strcmp(name, "playbackRate")) {
        media->setPlaybackRate(antb_to_number(value));
        return true;
    }
    if (!strcmp(name, "defaultPlaybackRate")) {
        media->setDefaultPlaybackRate(antb_to_number(value));
        return true;
    }
    if (!strcmp(name, "muted")) {
        media->setMuted(antb_to_boolean(value) != 0);
        return true;
    }
    if (!strcmp(name, "loop")) {
        media->setLoop(antb_to_boolean(value) != 0);
        return true;
    }
    if (!strcmp(name, "autoplay")) {
        media->setAutoplay(antb_to_boolean(value) != 0);
        return true;
    }
    if (!strcmp(name, "controls")) {
        media->setControls(antb_to_boolean(value) != 0);
        return true;
    }
    if (!strcmp(name, "src")) {
        char* text = antb_to_utf8(value);

        if (text) {
            media->setSrc(WebCore::String::fromUTF8(text));
            antb_free(text);
        }
        return true;
    }
    if (!strcmp(name, "poster")) {
        char* text = antb_to_utf8(value);

        if (text) {
            media->setPoster(WebCore::String::fromUTF8(text));
            antb_free(text);
        }
        return true;
    }
    if (!strcmp(name, "preload")) {
        char* text = antb_to_utf8(value);

        if (text) {
            media->setPreload(WebCore::String::fromUTF8(text));
            antb_free(text);
        }
        return true;
    }

    return false;
}

/* --- TextTrack / TextTrackList / TextTrackCue objects -------------------- */

int textTrackGetter(AntbEngine* engine, AntbValue* self, const char* name,
                    AntbValue* out, void*)
{
    WebCore::TextTrack* track = self
        ? (WebCore::TextTrack*)antb_get_native(self, AGB_TAG_TEXTTRACK) : 0;

    if (!track || !name)
        return 0;

    if (!strcmp(name, "kind"))
        return canvasReturnString(engine, out, track->kind().utf8().data());
    if (!strcmp(name, "label"))
        return canvasReturnString(engine, out, track->label().utf8().data());
    if (!strcmp(name, "language"))
        return canvasReturnString(engine, out, track->language().utf8().data());
    if (!strcmp(name, "id"))
        return canvasReturnString(engine, out, track->id().utf8().data());
    if (!strcmp(name, "mode"))
        return canvasReturnString(engine, out, track->mode().utf8().data());

    if (!strcmp(name, "cues")) {
        AntbValue* array = antb_make_array(engine);

        if (!array)
            return 0;

        for (unsigned i = 0; i < track->cues().size(); ++i) {
            AntbValue* cue = wrapTextTrackCue(engine, track->cues()[i].get());

            if (cue) {
                antb_array_push(engine, array, cue);
                antb_value_release(cue);
            }
        }

        antb_value_assign(out, array);
        antb_value_release(array);
        return 1;
    }

    if (!strcmp(name, "activeCues")) {
        AntbValue* array = antb_make_array(engine);
        Vector<RefPtr<WebCore::TextTrackCue> > active;

        if (!array)
            return 0;

        {
            WebCore::HTMLMediaElement* media = track->mediaElement();
            double now = media ? media->currentTime() : 0;

            track->cuesAt(now, active);
        }

        for (unsigned i = 0; i < active.size(); ++i) {
            AntbValue* cue = wrapTextTrackCue(engine, active[i].get());

            if (cue) {
                antb_array_push(engine, array, cue);
                antb_value_release(cue);
            }
        }

        antb_value_assign(out, array);
        antb_value_release(array);
        return 1;
    }

    return 0;
}

int textTrackListGetter(AntbEngine* engine, AntbValue* self, const char* name,
                        AntbValue* out, void*)
{
    WebCore::TextTrackList* list = self
        ? (WebCore::TextTrackList*)antb_get_native(self, AGB_TAG_TRACKLIST) : 0;
    char* end = NULL;
    long index;

    if (!list || !name)
        return 0;

    if (!strcmp(name, "length"))
        return canvasReturnNumber(engine, out, list->length());

    /* Numeric index access: textTracks[0]. */
    index = strtol(name, &end, 10);

    if (end && *end == '\0' && index >= 0) {
        WebCore::TextTrack* track = list->item((unsigned)index);

        if (!track)
            return 0;

        {
            AntbValue* v = wrapTextTrack(engine, track);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
        return 1;
    }

    return 0;
}

int textTrackCueGetter(AntbEngine* engine, AntbValue* self, const char* name,
                       AntbValue* out, void*)
{
    WebCore::TextTrackCue* cue = self
        ? (WebCore::TextTrackCue*)antb_get_native(self, AGB_TAG_CUE) : 0;

    if (!cue || !name)
        return 0;

    if (!strcmp(name, "startTime"))
        return canvasReturnNumber(engine, out, cue->startTime());
    if (!strcmp(name, "endTime"))
        return canvasReturnNumber(engine, out, cue->endTime());
    if (!strcmp(name, "id"))
        return canvasReturnString(engine, out, cue->id().utf8().data());
    if (!strcmp(name, "text"))
        return canvasReturnString(engine, out, cue->text().utf8().data());
    if (!strcmp(name, "pauseOnExit"))
        return mediaReturnBool(engine, out, false);

    return 0;
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
        return assignNull(engine, out);   /* null, not undefined (R95) */

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
    dispatchWindowEventState(type, NULL);
}

/* Dispatch a window event for a named document. The caller knows which frame
 * the event belongs to: the main frame's load can fire while a child frame's
 * document is the context the engine thread was left in, and window listeners
 * are per document now. Binding the document first puts the mirrors (and the
 * realm) on the event's own document. */
void dispatchWindowEventFor(void* document, const char* type)
{
    WebCore::Document* doc = (WebCore::Document*)document;
    DocumentRuntime* runtime;

    if (!doc || !type || !g_engine)
        return;

    runtime = documentRuntimeOf(g_currentState, doc);

    if (!runtime)
        return;   /* a document of a torn-down realm: its load is stale */

    activateDocument(runtime);
    dispatchWindowEventState(type, NULL);
}

/* A frame document reached DOMContentLoaded / load: with per-frame realms each
 * frame gets its own event on its own window. Bind the frame (which enters its
 * realm), run its deferred modules first for DOMContentLoaded, dispatch, then
 * bind the parent again so the embedding page continues in its own realm. */
void dispatchFrameDocumentEvent(void* document, const char* type)
{
    WebCore::Document* doc = (WebCore::Document*)document;

    if (!doc || !type || !doc->parentDocument() || !g_engine || !perFrameRealmEnabled())
        return;

    if (!realmForFrameDocument(g_engine, doc))
        return;

    bindDocument(doc);

    if (!strcmp(type, "DOMContentLoaded"))
        flushDeferredModules(doc);

    dispatchWindowEvent(type);

    if (doc->parentDocument())
        bindDocument(doc->parentDocument());
}

/* Frame::sendResizeEvent() runs while the frame that changed size is being
 * laid out, which is not necessarily the document currently bound in the
 * engine: a child frame laying out inside its parent's layout must fire at its
 * own window, not the parent's. */
void dispatchResizeEvent(void* document)
{
    WebCore::Document* doc = (WebCore::Document*)document;

    if (!g_engine || !doc)
        return;

    if (doc == g_document) {
        dispatchWindowEvent("resize");
        return;
    }

    {
        WebCore::Document* previous = g_document;

        bindDocument(doc);
        dispatchWindowEvent("resize");

        if (previous)
            bindDocument(previous);
    }
}

/* Same, but the event object carries `state`: popstate hands back the value
 * that pushState/replaceState stored for the entry being restored. */
void dispatchWindowEventState(const char* type, AntbValue* state)
{
    AntbValue* self;
    std::vector<AntbValue*> snapshot;

    if (!g_engine || !g_document || !type)
        return;

    agbLog(AGB_LOG_EVT, "window event %s doc=%p listeners=%d", type, (void*)g_document,
           g_listeners ? (int)g_listeners->size() : 0);

    if (!g_listeners)
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

            if (state) {
                antb_set_prop(g_engine, evt, "state", state);
            } else {
                AntbValue* nullValue = antb_make_null(g_engine);

                if (nullValue) {
                    antb_set_prop(g_engine, evt, "state", nullValue);
                    antb_value_release(nullValue);
                }
            }

            /* PageTransitionEvent.persisted: this engine has no bfcache, so a
             * pagehide/pageshow here is always a real load, but pages branch on
             * the field (`if (!e.persisted) cleanup()`), so it must exist. */
            if (!strcmp(type, "pagehide") || !strcmp(type, "pageshow")) {
                AntbValue* persisted = antb_make_number(g_engine, 0.0);

                if (persisted) {
                    antb_set_prop(g_engine, evt, "persisted", persisted);
                    antb_value_release(persisted);
                }
            }
        }

        for (size_t i = 0; i < snapshot.size(); ++i) {
            AntbValue* args[1];

            args[0] = evt;
            antb_call(g_engine, snapshot[i], self, args, evt ? 1 : 0, NULL);
        }

        if (getenv("AGB_EVT_DEBUG"))
            agbLog(AGB_LOG_EVT, "dispatch %s -> %d listener(s) (eval depth %d)",
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
    assertContextThread("documentWillUnload");

    if (!g_engine || !g_document)
        return;

    agbLog(AGB_LOG_EVT, "documentWillUnload doc=%p listeners=%d",
           g_document, g_listeners ? (int)g_listeners->size() : 0);

    InterlockedExchange(&g_teardownStart, (LONG)GetTickCount());

    /* Browser order (measured in Chrome 132): pagehide -> visibilitychange
     * (hidden) -> unload. The visibility change is part of the teardown: the
     * document must report hidden from here on, but the host's own view state
     * (viewHidden) must not flip, or the next document in a foreground tab
     * would start hidden. */
    dispatchWindowEvent("pagehide");

    if (g_currentState && g_currentState != &g_emptyState && !g_currentState->hidden) {
        g_currentState->hidden = 1;
        dispatchWindowEvent("visibilitychange");
    }

    dispatchWindowEvent("unload");

    /* A teardown handler may have thrown: do not leave the exception
     * pending, it would poison the next evaluation. */
    if (char* pending = antb_take_pending_error(g_engine)) {
        fprintf(stderr, "[antjs] (unload) %s\n", pending);
        free(pending);
    }

    /* The old page's listeners close over its DOM and would fire against the
     * next document. Each document owns its listeners now, so stopRealm()
     * releases exactly this document's handlers when it drops the records. */

    /* Isolation: the old realm goes away with the old document. Its globals,
     * its module registry and its timers must not be visible to the next
     * document. */
    stopRealm();

    InterlockedExchange(&g_teardownStart, 0);
}

/* Same teardown for a child frame document that is being replaced: its own
 * realm gets pagehide/unload, then the realm is dropped (the frame lives on and
 * the new document gets a new realm). */
void frameDocumentWillUnload(void* document)
{
    WebCore::Document* doc = (WebCore::Document*)document;

    if (!doc || !g_engine || !doc->parentDocument() || !perFrameRealmEnabled())
        return;

    if (!realmForFrameDocument(g_engine, doc))
        return;

    bindDocument(doc);

    dispatchWindowEvent("pagehide");
    dispatchWindowEvent("unload");

    if (char* pending = antb_take_pending_error(g_engine)) {
        fprintf(stderr, "[antjs] (frame unload) %s\n", pending);
        free(pending);
    }

    /* Each document record owns its listeners: forgetFrameRealm() releases
     * this frame's handlers together with the record. */
    forgetFrameRealm(doc);

    if (doc->parentDocument())
        bindDocument(doc->parentDocument());
}

/* The host says whether this view is visible (tab switch, window minimize).
 * document.hidden/visibilityState follow it and the page gets the event. */
void setViewVisible(int visible)
{
    int hidden = visible ? 0 : 1;

    if (!g_currentState || g_currentState == &g_emptyState)
        return;

    g_currentState->viewHidden = hidden;

    if (g_currentState->hidden == hidden)
        return;

    g_currentState->hidden = hidden;
    dispatchWindowEvent("visibilitychange");
}

/* Builds the JS event object: the prelude adds the DOM methods (which only set
 * flags), then we copy the native fields across. */
/* --- KeyboardEvent.key / .code ------------------------------------------
 * This WebCore predates DOM Level 3 `key`/`code`: pages only got keyCode and
 * charCode, so every modern handler that tests `e.key === "Enter"` (DeepSeek's
 * forms do) silently failed. Derive both from keyIdentifier()/charCode(); the
 * platform modifiers come from the underlying PlatformKeyboardEvent. */
static bool keyIdentifierCodepoint(const std::string& ki, unsigned* out)
{
    unsigned v = 0;

    if (ki.size() < 6 || (ki[0] != 'U' && ki[0] != 'u') || ki[1] != '+')
        return false;

    for (size_t i = 2; i < ki.size(); ++i) {
        char c = ki[i];
        int d = (c >= '0' && c <= '9') ? c - '0'
              : ((c >= 'a' && c <= 'f') ? c - 'a' + 10
              : ((c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1));
        if (d < 0)
            return false;
        v = v * 16 + (unsigned)d;
    }

    *out = v;
    return true;
}

static void appendUtf8(std::string* out, unsigned cp)
{
    if (cp < 0x80) {
        *out += (char)cp;
    } else if (cp < 0x800) {
        *out += (char)(0xC0 | (cp >> 6));
        *out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += (char)(0xE0 | (cp >> 12));
        *out += (char)(0x80 | ((cp >> 6) & 0x3F));
        *out += (char)(0x80 | (cp & 0x3F));
    } else {
        *out += (char)(0xF0 | (cp >> 18));
        *out += (char)(0x80 | ((cp >> 12) & 0x3F));
        *out += (char)(0x80 | ((cp >> 6) & 0x3F));
        *out += (char)(0x80 | (cp & 0x3F));
    }
}

static const char* keyNameForIdentifier(const std::string& ki)
{
    static const struct { const char* id; const char* name; } map[] = {
        { "Space", " " }, { "Return", "Enter" }, { "BackSpace", "Backspace" },
        { "Tab", "Tab" }, { "Escape", "Escape" }, { "Delete", "Delete" },
        { "Left", "ArrowLeft" }, { "Right", "ArrowRight" },
        { "Up", "ArrowUp" }, { "Down", "ArrowDown" },
        { "Home", "Home" }, { "End", "End" },
        { "PageUp", "PageUp" }, { "PageDown", "PageDown" }, { "Insert", "Insert" },
        { "NumLock", "NumLock" }, { "CapsLock", "CapsLock" },
        { "Shift", "Shift" }, { "Control", "Control" }, { "Alt", "Alt" },
        { "Meta", "Meta" }, { "Pause", "Pause" }, { "Scroll", "ScrollLock" },
        { "PrintScreen", "PrintScreen" }, { "Menu", "ContextMenu" }
    };

    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (ki == map[i].id)
            return map[i].name;

    return NULL;
}

/* "U+0041" -> "KeyA", "U+0031" -> "Digit1", "U+002C" -> "Comma". */
static void codeForCodepoint(unsigned cp, std::string* out)
{
    if (cp >= 'A' && cp <= 'Z') { *out = "Key"; *out += (char)cp; return; }
    if (cp >= 'a' && cp <= 'z') { *out = "Key"; *out += (char)(cp - 'a' + 'A'); return; }
    if (cp >= '0' && cp <= '9') { *out = "Digit"; *out += (char)cp; return; }

    static const struct { unsigned cp; const char* code; } punct[] = {
        { ' ', "Space" }, { ',', "Comma" }, { '.', "Period" }, { '/', "Slash" },
        { ';', "Semicolon" }, { 39, "Quote" }, { '[', "BracketLeft" },
        { ']', "BracketRight" }, { '\\', "Backslash" }, { '-', "Minus" },
        { '=', "Equal" }, { '`', "Backquote" }
    };

    for (unsigned i = 0; i < sizeof(punct) / sizeof(punct[0]); ++i)
        if (cp == punct[i].cp) { *out = punct[i].code; return; }
}

static void codeForIdentifier(const std::string& ki, std::string* out)
{
    if (ki == "Space") { *out = "Space"; return; }
    if (ki == "Return") { *out = "Enter"; return; }
    if (ki == "BackSpace") { *out = "Backspace"; return; }
    if (ki == "Left") { *out = "ArrowLeft"; return; }
    if (ki == "Right") { *out = "ArrowRight"; return; }
    if (ki == "Up") { *out = "ArrowUp"; return; }
    if (ki == "Down") { *out = "ArrowDown"; return; }

    if (ki.size() > 1 && ki[0] == 'F') {          /* F1..F12 */
        bool digits = true;
        for (size_t i = 1; i < ki.size(); ++i)
            if (ki[i] < '0' || ki[i] > '9') { digits = false; break; }
        if (digits) { *out = ki; return; }
    }

    *out = ki;                                    /* Tab/Escape/Delete/Home/... */
}

/* Text of the most recent textInput (the pre-insertion event): the
 * `input` event that follows carries it as `data`. */
static std::string g_lastTextInputData;

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
    /* Engine-dispatched events are trusted; a JS dispatchEvent() passes the
     * caller's own Event object through nodeDispatchEvent and keeps it false. */
    {
        AntbValue* trusted = antb_make_number(g_engine, 1.0);
        if (trusted) {
            antb_set_prop(g_engine, evt, "isTrusted", trusted);
            antb_value_release(trusted);
        }
    }
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
            {
                std::string id = key->keyIdentifier().utf8().data();
                bool shift = false;
                const WebCore::PlatformKeyboardEvent* platform = key->keyEvent();
                unsigned cp = 0;
                std::string keyName, codeName;

                if (platform)
                    shift = platform->shiftKey();

                if (key->charCode() > 0) {
                    appendUtf8(&keyName, (unsigned)key->charCode());
                } else if (keyIdentifierCodepoint(id, &cp)) {
                    unsigned out = cp;
                    /* keyIdentifier spells letters in upper case regardless of
                     * the shift state; Chrome reports the produced character. */
                    if (cp >= 'A' && cp <= 'Z' && !shift)
                        out = cp - 'A' + 'a';
                    appendUtf8(&keyName, out);
                } else {
                    const char* mapped = keyNameForIdentifier(id);
                    if (mapped)
                        keyName = mapped;
                }

                if (keyIdentifierCodepoint(id, &cp))
                    codeForCodepoint(cp, &codeName);
                else
                    codeForIdentifier(id, &codeName);

                if (!keyName.empty()) {
                    AntbValue* v = antb_make_string(g_engine, keyName.c_str());
                    if (v) { antb_set_prop(g_engine, evt, "key", v); antb_value_release(v); }
                }
                if (!codeName.empty()) {
                    AntbValue* v = antb_make_string(g_engine, codeName.c_str());
                    if (v) { antb_set_prop(g_engine, evt, "code", v); antb_value_release(v); }
                }
            }
        } else if (type == "input") {
            /* The 2008 input event carries no inputType; pages read it to tell
             * typing from paste/deletion. Everything this engine produces is
             * an insertion. */
            if (target && target->isElementNode()) {
                WebCore::Element* element = static_cast<WebCore::Element*>(target);

                if (element->hasTagName(WebCore::HTMLNames::inputTag) ||
                    element->hasTagName(WebCore::HTMLNames::textareaTag)) {
                    AntbValue* v = antb_make_string(g_engine, "insertText");
                    if (v) { antb_set_prop(g_engine, evt, "inputType", v); antb_value_release(v); }
                    if (!g_lastTextInputData.empty()) {
                        AntbValue* d = antb_make_string(g_engine, g_lastTextInputData.c_str());
                        if (d) { antb_set_prop(g_engine, evt, "data", d); antb_value_release(d); }
                        g_lastTextInputData.clear();
                    }
                }
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
        AntbRealm* previousRealm = NULL;
        bool switched = false;

        if (l.node != node || l.type != type)
            continue;

        /* Run the handler in the realm it was registered in: a parent event
         * bubbling into a child's handler (or vice versa) must see its own
         * document, not whatever realm was current (R132). */
        if (g_engine && l.realm && l.realm != currentRealm()) {
            previousRealm = currentRealm();
            antb_realm_enter(g_engine, l.realm);
            applyRealmBinding(l.realm);
            switched = true;
        }
        {
            AntbValue* args[1];
            args[0] = evt;
            value = antb_call(g_engine, l.fn, currentTarget, args, evt ? 1 : 0, NULL);
        }
        if (switched && previousRealm) {
            antb_realm_enter(g_engine, previousRealm);
            applyRealmBinding(previousRealm);
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

        /* Two WebCore events are the "text is about to be inserted" moment,
         * which modern pages know as `beforeinput`:
         *   - webkitBeforeTextInserted: dispatched by TypingCommand for every
         *     editable insertion, including <input>/<textarea> (this port
         *     inserts typed characters through insertTextWithoutSendingTextEvent,
         *     so this is the one form fields hit);
         *   - textInput: the legacy DOM event, dispatched by
         *     Editor::insertText() (execCommand and friends).
         * Both are renamed for the JS side and carry data/inputType, so a
         * handler sees the text before it lands and can cancel it with
         * preventDefault(). */
        WebCore::TextEvent* textInputEvt = NULL;

        if (type == "textInput" && event->isTextEvent()) {
            textInputEvt = static_cast<WebCore::TextEvent*>(event);
            type = "beforeinput";
            g_lastTextInputData = textInputEvt->data().utf8().data();
        }

        AntbValue* evt = makeEventObject(type, node, event);

        if (getenv("AGB_EVT_DEBUG") && (type == "load" || type == "DOMContentLoaded"))
            agbLog(AGB_LOG_EVT, "WebCore event %s on node=%p", type.c_str(), (void*)node);
        bool stopped = false;
        bool stoppedImmediate = false;
        if (!evt)
            return;

        if (textInputEvt) {
            const char* text = textInputEvt->data().utf8().data();
            AntbValue* data = antb_make_string(g_engine, text);
            AntbValue* kind = antb_make_string(g_engine, "insertText");

            if (data) { antb_set_prop(g_engine, evt, "data", data); antb_value_release(data); }
            if (kind) { antb_set_prop(g_engine, evt, "inputType", kind); antb_value_release(kind); }
        }

        /* Bubble only when the event says so. load does not bubble: without
         * this check every <script>/<img> load bubbled up to the document and
         * ran the page's window load handler once per async script. */
        bool bubbles = event->bubbles();
        std::vector<WebCore::Node*> visited;

        for (WebCore::Node* current = node; current && !stopped;
             current = bubbles ? current->parentNode() : 0) {
            AntbValue* wrapper = wrapNode(g_engine, current);
            if (wrapper) {
                visited.push_back(current);
                if (getenv("AGB_EVT_TRACE")) {
                    int matched = 0;
                    for (size_t li = 0; g_listeners && li < g_listeners->size(); ++li)
                        if ((*g_listeners)[li].node == current && (*g_listeners)[li].type == type)
                            ++matched;
                    fprintf(stderr, "[evt-trace] %s node=%p listeners=%d\n",
                            type.c_str(), (void*)current, matched);
                }
                runListenersFor(current, type, evt, wrapper, stopped, stoppedImmediate);
                antb_value_release(wrapper);
            }
        }
        /* preventDefault in JS must reach the engine, or links still navigate. */
        bool preventedInJs = false;
        {
            AntbValue* prevented = antb_get_prop(g_engine, evt, "__prevented");
            if (prevented) {
                if (antb_to_boolean(prevented)) {
                    preventedInJs = true;
                    event->preventDefault();
                }
                antb_value_release(prevented);
            }
        }

        /* textInput's preventDefault is honoured by WebCore itself: its default
         * handler only runs when the event was not cancelled, so the
         * preventDefault() above is the whole story. */
        (void)preventedInJs;

        /* {once:true}: the engine path has no listener snapshot to compare
         * against (nodeDispatchEvent, the JS dispatch path, does), so drop the
         * once-listeners of every node this dispatch visited. Without this the
         * option only worked for events a page dispatched itself. */
        if (g_listeners) {
            for (size_t i = 0; i < g_listeners->size(); ) {
                Listener& l = (*g_listeners)[i];
                bool drop = false;

                if (l.once && l.type == type) {
                    for (size_t v = 0; v < visited.size(); ++v)
                        if (l.node == visited[v]) { drop = true; break; }
                }
                if (drop) {
                    if (l.fn)
                        antb_value_release(l.fn);
                    g_listeners->erase(g_listeners->begin() + i);
                } else {
                    ++i;
                }
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

/* C entry point for the webview control (a C translation unit, so it cannot
 * include the C++ runtime header): the tick of the teardown that is currently
 * running, 0 when none is. */
extern "C" unsigned long agb_teardown_started_ms(void)
{
    return (unsigned long)AntRuntime::g_teardownStart;
}

/* C entry point: the context (RuntimeState*) the engine thread is in right
 * now. The hang watchdog (another thread) reads it to attribute a stuck script
 * to one view instead of notifying every view. NULL before the first context,
 * and it deliberately stays on the last context: while a script is stuck,
 * "the last context entered" *is* the stuck one. */
extern "C" void* agb_busy_js_context(void)
{
    return (void*)InterlockedCompareExchangePointer((void* volatile*)&g_busyContext, NULL, NULL);
}

/* Dumps the live JS stack of `context` to `path` (one file per report; the
 * caller passes a fresh name). The caller must freeze the engine thread
 * (SuspendThread) around this call: the VM walk cannot race with a running
 * script. Returns 1 when a file was written. */
extern "C" int agb_dump_js_stack_for(void* context, const char* path)
{
    RuntimeState* state = (RuntimeState*)context;

    if (!state || state == &g_emptyState || !state->engine || !path || !*path)
        return 0;

    return antb_dump_js_stack_to_file(state->engine, path);
}


















