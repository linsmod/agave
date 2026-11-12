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
#include "AntBindings.h"
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

/* The custom element construct hook (issues #18/#22) is defined with the custom
 * element helpers far below but has to be passed to the bridge when a document
 * binds: a file-scope pointer, assigned right after its definition, bridges the
 * gap without another forward declaration through two anonymous namespaces. */
typedef int (*AgbConstructHookFn)(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*);
static AgbConstructHookFn g_constructHookFn = NULL;
#include "RealmDocument.h"
#include "RealmTags.h"
#include "../../webcore/platform/network/native/NetworkSession.h"
#include "../platform/network/native/NetLog.h"

#include "Document.h"
#include "DOMImplementation.h"
#include "HTMLDocument.h"
#include "Element.h"
#include "SVGNames.h"
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
#include "DOMSelection.h"
#include "Range.h"
#include "TreeWalker.h"
#include "NodeFilter.h"
#include "NodeFilterCondition.h"
#include "Editor.h"
#include "AtomicString.h"
#include "FrameTree.h"
#include "CharacterData.h"
#include "NamedAttrMap.h"
#include "Attribute.h"
#include "DocumentFragment.h"
#include "editing/markup.h"
#include "HTMLImageElement.h"
#include "HTMLStyleElement.h"
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
#include "HTMLButtonElement.h"
#include "HTMLAnchorElement.h"
#include "HTMLFormElement.h"
#include "HTMLTextAreaElement.h"
#include "HTMLGenericFormElement.h"
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

static void ensureCanvasContextClass(AntbEngine*);

namespace AntRuntime {
void dispatchWindowEventState(const char* type, AntbValue* state);
AntbValue* documentObjectFor(AntbEngine*, WebCore::Document*);
AntbValue* windowObjectFor(AntbEngine*, WebCore::Document*);
void installFrameGlobals(AntbEngine*, WebCore::Document*);
AntbValue* windowForDocument(AntbEngine*, WebCore::Document*);
AntbValue* realmWindowFor(AntbEngine*, WebCore::Document*);
int windowGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int windowSetter(AntbEngine*, AntbValue*, const char*, const AntbValue*, void*);
/* Built where the keyboard/mouse detail helpers live (namespace AntRuntime);
 * the anonymous-namespace dispatchers call it through makeEventObject(). */
AntbValue* makeEventObjectForDispatch(const std::string&, WebCore::Node*, WebCore::Event*);
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
struct AgbShadowRoot;
struct Listener;

struct RealmInterfaces;

struct RuntimeState {
    AntbEngine*        engine;
    AntbRealm*         mainRealm;       /* realm of the top-level document */
    WebCore::Document* document;
    /* Interface classes mirrored for the realm the engine is in right now
     * (B2-4): every realm gets its own prototype chain and constructors, so a
     * frame's `HTMLElement` is a different object from its parent's. */
    struct RealmInterfaces* ifacesActive;
    std::map<void*, struct RealmInterfaces*>* realmIfaces;   /* AntbRealm* -> set */
    AntbClass*         nodeClass;
    /* The rest of the class mirrors are per state (== per engine) for a reason:
     * they used to be file-scope statics, so a second live engine (a popup)
     * overwrote them while it built its own interface set. The first engine then
     * wrapped its nodes into the *other* engine's classes - a cross-VM prototype
     * whose every property read comes back undefined (issue #15 §20.5: the
     * probe's eval saw `document.documentElement.tagName === undefined`). */
    AntbClass*         documentIface;
    AntbClass*         htmlDocumentIface;
    AntbClass*         elementIface;
    AntbClass*         htmLElementIface;
    AntbClass*         svgElementIface;
    AntbClass*         svgSvgIface;
    AntbClass*         documentFragmentIface;
    AntbClass*         characterDataIface;
    AntbClass*         textIface;
    AntbClass*         commentIface;
    AntbClass*         unknownElementIface;
    AntbClass*         namedNodeMapIface;
    AntbClass*         attrIface;
    AntbClass*         shadowRootIface;
    AntbClass*         treeWalkerIface;
    std::map<std::string, AntbClass*>* tagClasses;   /* tag -> element interface */
    /* Engine-scoped aux interfaces (not part of the DOM tree; shared by every
     * realm of this view). They were statics too, so the same cross-engine
     * mix-up hit selection/range/media wrappers. */
    AntbClass*         selectionClass;
    AntbClass*         rangeClass;
    AntbClass*         textTrackClass;
    AntbClass*         textTrackListClass;
    AntbClass*         textTrackCueClass;
    AntbClass*         canvasCtxClass;  /* CanvasRenderingContext2D (2d context) */
    AntbClass*         windowClass;     /* Window (per-frame window proxy) */
    AntbClass*         nodeListClass;   /* NodeList (B2-2) */
    AntbClass*         htmlCollectionClass; /* HTMLCollection (B2-2) */
    AntbClass*         styleSheetListClass;
    AntbClass*         styleSheetClass;
    AntbClass*         cssRuleClass;
    AntbClass*         cssRuleListClass;
    AntbClass*         mediaListClass;
    AntbClass*         cssDeclarationClass;
    std::vector<CssomRef>* cssomRefs;   /* ref'd CSSOM objects, released at realm teardown */
    std::set<void*>*   cssomSeen;
    /* Custom element definitions (issues #18 phase A): lowercase tag name ->
     * registered constructor (retained), plus the elements upgraded so far.
     * Both belong to the document's realm and die with it. */
    std::map<std::string, AntbValue*>* customDefs;
    std::set<WebCore::Node*>*          upgradedCustom;
    /* HTMLTemplateElement.content (issues #18 phase B): one fragment per template,
     * created lazily and owned by the document, so it dies with it. */
    std::map<WebCore::Node*, WebCore::DocumentFragment*>* templateContents;
    /* attachShadow() (issues #18 phase C, #27): see DocumentRuntime's copy. */
    std::map<WebCore::Node*, AgbShadowRoot*>* shadowRoots;
    AntbValue*         docObj;          /* the JS document object (rooted) */
    bool               documentBound;
    void*              listeners;       /* std::vector<Listener>* (see below) */
    WebCore::Document* docSlots[AGB_MAX_DOCUMENT_SLOTS];
    unsigned long      docSlotGeneration[AGB_MAX_DOCUMENT_SLOTS];
    int                docSlotCount;
    int                currentSlot;
    unsigned long      generation;      /* id of the realm (see nextGeneration) */
    /* Host-assigned view/tab id (0 = not told). Exposed to JS by
     * window.__agbInfo() so a page and the host logs name the same view; the
     * process backend passes it on the renderer command line. */
    int                hostViewId;
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

/* ---- per-realm interface classes (B2-4) --------------------------------
 * Every realm gets its own prototype chain and constructors, like a browser's
 * bindings: `child.HTMLElement !== parent.HTMLElement`, and each wrapper's
 * prototype belongs to the realm that created the wrapper. The engine-scoped
 * aux classes (CSSOM/Selection/Range/TextTrack/Canvas) are still shared; they
 * are not part of the DOM interface tree.
 *
 * Building a set costs one class tree per realm (a few hundred classes, methods
 * and attribute accessors). It happens once per realm, on the first document
 * bind (see ensureInterfaceClasses), and the set is dropped when the realm is
 * torn down.
 */
struct RealmInterfaces {
    std::map<std::string, AntbClass*> byName;
    std::map<std::string, AntbClass*>* tagClasses;
    AntbClass* nodeClass;
    AntbClass* documentIface;
    AntbClass* htmlDocumentIface;
    AntbClass* elementIface;
    AntbClass* htmLElementIface;
    AntbClass* svgElementIface;
    AntbClass* svgSvgIface;
    AntbClass* documentFragmentIface;
    AntbClass* characterDataIface;
    AntbClass* textIface;
    AntbClass* commentIface;
    AntbClass* unknownElementIface;
    AntbClass* namedNodeMapIface;
    AntbClass* attrIface;
    AntbClass* shadowRootIface;
    AntbClass* treeWalkerIface;
    AntbClass* windowClass;
    AntbClass* nodeListClass;
    AntbClass* htmlCollectionClass;

    RealmInterfaces()
        : tagClasses(NULL), nodeClass(NULL), documentIface(NULL), htmlDocumentIface(NULL),
          elementIface(NULL), htmLElementIface(NULL), svgElementIface(NULL), svgSvgIface(NULL),
          documentFragmentIface(NULL), characterDataIface(NULL), textIface(NULL),
          commentIface(NULL), unknownElementIface(NULL), namedNodeMapIface(NULL),
          attrIface(NULL), shadowRootIface(NULL), treeWalkerIface(NULL), windowClass(NULL),
          nodeListClass(NULL), htmlCollectionClass(NULL) {}
};

/* ------------------------------------------------------------------------- */
/* DocumentRuntime: one bound document (== one realm)                        */
/* ------------------------------------------------------------------------- */
struct AgbCollection;
struct AgbTreeWalker;
/* One attached shadow root (issues #27). The shadow content is physically part
 * of the host subtree in this port; `light` keeps the host's original children
 * (which slot distribution moves next to their slot) so they can be told apart
 * from component-owned nodes (marked with Node::shadowScope() == id). */
struct AgbShadowRoot {
    int mode;                              /* 1 open, 2 closed */
    int id;                                /* shadow scope id, >= 1 */
    std::vector<WebCore::Node*>* light;    /* host's light children */
};

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
    /* Custom element definitions (issues #18 phase A): lowercase tag name ->
     * registered constructor (retained), plus the elements upgraded so far.
     * Both belong to the document's realm and die with it. */
    std::map<std::string, AntbValue*>* customDefs;
    std::set<WebCore::Node*>*          upgradedCustom;
    /* HTMLTemplateElement.content (issues #18 phase B): one fragment per template,
     * created lazily and owned by the document, so it dies with it. */
    std::map<WebCore::Node*, WebCore::DocumentFragment*>* templateContents;
    /* attachShadow() (issues #18 phase C, #27): host element -> shadow root.
     * The shadow children are physically host children; `light` and the scope
     * marker tell them apart. Slot assignment state lives per document too. */
    std::map<WebCore::Node*, AgbShadowRoot*>* shadowRoots;
    std::map<WebCore::Node*, WebCore::Node*>* slotOf;              /* light node -> slot */
    std::map<WebCore::Node*, std::vector<WebCore::Node*>*>* slotAssignments;
    WebCore::DocumentFragment* unassignedLight;                    /* detached light nodes */
    int distributingShadow;
    /* slotchange is a microtask in Chrome (a synchronous probe sees no event);
     * slots whose assignment changed wait here until the queued callback. */
    std::vector<WebCore::Node*>* pendingSlotChanges;
    int slotChangeScheduled;
    /* DOM Selection API: one live DOMSelection per document (a view over the
     * frame's selection) and the Range objects handed to JS. Ranges are ref'd
     * when wrapped and released with the document (see selectionReleaseRefs);
     * the JS side has no finalizers, so this is the same lifetime scheme the
     * CSSOM wrappers use. */
    WebCore::DOMSelection* selection;
    std::vector<WebCore::Range*>* rangeRefs;
    /* NodeList/HTMLCollection wrappers (B2-2): descriptors are owned by the
     * document (no JS finalizers) and the map gives live collections their
     * identity (`el.children === el.children`). */
    std::vector<AgbCollection*>* collections;
    std::map<std::string, AgbCollection*>* collectionCache;
    /* <script type="importmap"> parsed into a std::map<std::string,std::string>
     * (specifier -> absolute URL). Owned by the document: dropped in
     * releaseDocumentRuntime, so a navigation never resolves a specifier
     * through the previous page's map. NULL when the document has no map. */
    void* importMap;
    /* document.createTreeWalker() results (issues #27): the walker is ref'd and
     * released with the document. The filter handle it captured is retained on
     * the AgbTreeWalker, so the JS callback survives GC like a listener. */
    std::vector<AgbTreeWalker*>* treeWalkers;
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
    &g_emptyState, NULL, NULL, NULL, NULL, false, false, -1, 0, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL
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
#define g_documentIface         (g_currentState->documentIface)
#define g_htmlDocumentIface     (g_currentState->htmlDocumentIface)
#define g_elementIface          (g_currentState->elementIface)
#define g_htmLElementIface      (g_currentState->htmLElementIface)
#define g_svgElementIface       (g_currentState->svgElementIface)
#define g_svgSvgIface           (g_currentState->svgSvgIface)
#define g_documentFragmentIface (g_currentState->documentFragmentIface)
#define g_characterDataIface    (g_currentState->characterDataIface)
#define g_namedNodeMapIface     (g_currentState->namedNodeMapIface)
#define g_attrIface             (g_currentState->attrIface)
#define g_shadowRootIface       (g_currentState->shadowRootIface)
#define g_treeWalkerIface       (g_currentState->treeWalkerIface)
#define g_textIface             (g_currentState->textIface)
#define g_commentIface          (g_currentState->commentIface)
#define g_unknownElementIface   (g_currentState->unknownElementIface)
#define g_tagClasses            (g_currentState->tagClasses)
#define g_selectionClass        (g_currentState->selectionClass)
#define g_rangeClass            (g_currentState->rangeClass)
#define g_textTrackClass        (g_currentState->textTrackClass)
#define g_textTrackListClass    (g_currentState->textTrackListClass)
#define g_textTrackCueClass     (g_currentState->textTrackCueClass)
#define g_canvasCtxClass (g_currentState->canvasCtxClass)
#define g_windowClass   (g_currentState->windowClass)
#define g_nodeListClass (g_currentState->nodeListClass)
#define g_htmlCollectionClass (g_currentState->htmlCollectionClass)
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
 * Identity is the bridge wrapper cache: the document is wrapped as an
 * HTMLDocument interface instance, exactly like any other node. */

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
static void releaseCollections(DocumentRuntime*, bool releaseValues);

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
static void selectionReleaseRefs(DocumentRuntime* document);
static void destroyTreeWalkers(DocumentRuntime* document);
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
        /* Selection/Range are WebCore objects, not VM values, but they hold
         * refs on nodes/this document; drop them with the document so a
         * detached page cannot be kept alive by a JS Range left behind. */
        selectionReleaseRefs(document);
    }

    /* NodeList/HTMLCollection descriptors and the static snapshots they retain
     * belong to this document too. On the engine-gone path the retained VM
     * values cannot be touched anymore, only the descriptors are dropped. */
    releaseCollections(document, releaseValues);

    /* The import map is host-side bookkeeping, not a VM value: it must go away
     * even on the engine-gone path (releaseValues == false). */
    if (document->importMap) {
        delete (std::map<std::string, std::string>*)document->importMap;
        document->importMap = NULL;
    }

    /* Custom element definitions retain their constructors; on the engine-gone
     * path the VM values cannot be touched, only the map is dropped. */
    if (document->customDefs) {
        if (releaseValues) {
            for (std::map<std::string, AntbValue*>::iterator it = document->customDefs->begin();
                 it != document->customDefs->end(); ++it) {
                if (it->second)
                    antb_value_release(it->second);
            }
        }
        delete document->customDefs;
        document->customDefs = NULL;
    }
    delete document->upgradedCustom;
    document->upgradedCustom = NULL;

    if (document->shadowRoots) {
        for (std::map<WebCore::Node*, AgbShadowRoot*>::iterator it = document->shadowRoots->begin();
             it != document->shadowRoots->end(); ++it) {
            if (it->second) {
                delete it->second->light;
                delete it->second;
            }
        }
        delete document->shadowRoots;
        document->shadowRoots = NULL;
    }
    if (document->slotAssignments) {
        for (std::map<WebCore::Node*, std::vector<WebCore::Node*>*>::iterator it =
                 document->slotAssignments->begin(); it != document->slotAssignments->end(); ++it)
            delete it->second;
        delete document->slotAssignments;
        document->slotAssignments = NULL;
    }
    delete document->slotOf;
    document->slotOf = NULL;
    if (document->pendingSlotChanges) {
        for (size_t i = 0; i < document->pendingSlotChanges->size(); ++i) {
            if ((*document->pendingSlotChanges)[i])
                (*document->pendingSlotChanges)[i]->deref();
        }
        delete document->pendingSlotChanges;
        document->pendingSlotChanges = NULL;
    }
    document->slotChangeScheduled = 0;
    if (document->unassignedLight) {
        document->unassignedLight->deref();
        document->unassignedLight = NULL;
    }

    /* TreeWalkers hold a RefPtr on their WebCore walker (which refs the root
     * node and the filter); drop them with the document. The retained JS filter
     * handle itself leaks by design (antb_retain has no counterpart). */
    destroyTreeWalkers(document);

    if (document->templateContents) {
        for (std::map<WebCore::Node*, WebCore::DocumentFragment*>::iterator it =
                 document->templateContents->begin();
             it != document->templateContents->end(); ++it) {
            if (it->second)
                it->second->deref();
        }
        delete document->templateContents;
        document->templateContents = NULL;
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

/* Interface classes are per realm (B2-4); this mirrors one realm's set into the
 * g_* class pointers so the C dispatchers see that realm's prototypes. */
static void applyRealmInterfaces(AntbRealm* realm);

/* Point the host-side "current document" mirrors at `realm`'s document. The
 * document record is the authority now: no boundRealm comparison, no
 * mainDocObj bookkeeping (R130's patch existed only because the realm ->
 * document mapping was missing). */
static void applyRealmBinding(AntbRealm* realm)
{
    DocumentRuntime* document = documentForCurrentRealm(realm);

    applyRealmInterfaces(realm);

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
/* DOM collections (B2-2): what a NodeList/HTMLCollection wrapper stands for.
 * The enum lives up here because the document getter (which is much earlier in
 * the file) builds the document.forms/images/... collections. */
enum {
    AgbCollectionChildNodes = 1,   /* NodeList: every child (live) */
    AgbCollectionChildren,         /* HTMLCollection: element children (live) */
    AgbCollectionTags,             /* HTMLCollection: getElementsByTagName (live) */
    AgbCollectionClass,            /* HTMLCollection: getElementsByClassName (live) */
    AgbCollectionDocumentList,     /* HTMLCollection: document.forms/images/... */
    AgbCollectionFormElements,     /* HTMLCollection: form.elements */
    AgbCollectionStatic            /* NodeList: querySelectorAll snapshot */
};
static AntbValue* wrapCollection(AntbEngine*, WebCore::Node* root, int kind, const std::string& name);
static AntbValue* wrapStaticCollection(AntbEngine*, WebCore::Node* root,
                                       const std::vector<WebCore::Node*>& nodes);
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
int nodeGetElementsByTagName(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeCloneNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateDocumentFragment(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeReplaceChild(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeHasAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeHasChildNodes(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeContains(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeCompareDocumentPosition(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeRemove(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeAppendChildren(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodePrependChildren(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeBefore(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeAfter(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeReplaceWith(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeAssignedNodes(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalAttachShadow(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeAssignedElements(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateTreeWalker(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int treeWalkerMethod(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int treeWalkerGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int treeWalkerSetter(AntbEngine*, AntbValue*, const char*, const AntbValue*, void*);
int elementRequestFullscreen(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docExitFullscreen(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);

/* Shadow DOM helpers (defined with the shadow code below). */
static WebCore::Element* shadowHostOfWrapper(AntbValue* self);
static AgbShadowRoot* shadowInfo(WebCore::Node* host);
static void markShadowSubtree(WebCore::Node* node, int scope);
static void shadowAfterInsert(AntbValue* selfValue, WebCore::Node* parent, WebCore::Node* child);
static void shadowAfterRemove(AntbValue* selfValue, WebCore::Node* parent, WebCore::Node* child);
static void replaceShadowChildren(WebCore::Element* host, AntbValue** args, int argc);
int namedMapItemFn(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int namedMapGetNamedItemFn(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int namedMapSetNamedItem(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int namedMapRemoveNamedItem(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int namedMapGetNamedItemNS(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int namedMapSetNamedItemNS(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int namedMapRemoveNamedItemNS(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetBoundingClientRect(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetRootNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
AntbValue* wrapShadowRoot(AntbEngine*, WebCore::Element*);
int nodeGetAttributeNames(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeToggleAttribute(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetAttributeNode(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetAttributeNS(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeSetAttributeNS(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeGetClientRects(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
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

/* The JS event facade (prelude.js) owns the event interface prototypes; these
 * helpers let the C dispatcher build events through it and update the
 * per-listener currentTarget/eventPhase slots it creates. */
static AntbValue* makeEventObject(const std::string& type, WebCore::Node* target, WebCore::Event* event)
{
    return AntRuntime::makeEventObjectForDispatch(type, target, event);
}

/* Set while a *script-called* method (el.click(), el.focus(), ...) walks through
 * the engine's dispatch: Chrome reports isTrusted=false for those, and true only
 * for events the user really caused. */
static bool g_scriptInitiatedEvent = false;

struct ScriptInitiatedEventScope {
    ScriptInitiatedEventScope() { g_scriptInitiatedEvent = true; }
    ~ScriptInitiatedEventScope() { g_scriptInitiatedEvent = false; }
};

/* prelude's __agbEventPhase(evt, current, phase): currentTarget/eventPhase are
 * own data properties on facade events and have to be defined on a native
 * Event (its prototype accessors are getter-only), so the update goes through
 * the JS layer. */
static AntbValue* eventPhaseFn(AntbEngine* engine)
{
    AntbValue* global = antb_global(engine);
    AntbValue* fn;

    if (!global)
        return NULL;
    fn = antb_get_prop(engine, global, "__agbEventPhase");
    antb_value_release(global);
    return fn;   /* caller releases */
}

static void setEventPhase(AntbEngine* engine, AntbValue* fn, AntbValue* evt,
                          AntbValue* current, int phase)
{
    AntbValue* args[3];
    AntbValue* nullValue = NULL;
    AntbValue* phaseValue;
    AntbValue* result;

    if (!fn || !evt)
        return;
    if (!current)
        nullValue = antb_make_null(engine);
    phaseValue = antb_make_number(engine, (double)phase);
    args[0] = evt;
    args[1] = current ? current : nullValue;
    args[2] = phaseValue;
    result = antb_call(engine, fn, NULL, args, 3, NULL);
    if (result)
        antb_value_release(result);
    if (phaseValue)
        antb_value_release(phaseValue);
    if (nullValue)
        antb_value_release(nullValue);
}

int nodeGetContext(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeToDataURL(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);

/* DOM Selection / Range (window.getSelection, document.createRange,
 * document.execCommand). Implementation next to documentOf() further down. */
int docGetSelection(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docCreateRange(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docExecCommand(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docQueryCommandSupported(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docQueryCommandEnabled(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int docQueryCommandState(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
AntbValue* wrapRange(AntbEngine*, WebCore::Range*);
void selectionReleaseRefs(DocumentRuntime* document);

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

/* DOM interface registry (one bridge class per interface): created once per
 * engine, before the document object exists, exposed per realm. */
void ensureInterfaceClasses(AntbEngine*);
AntbClass* classForNode(WebCore::Node*);
void resetInterfaceClasses(RuntimeState* state);
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
int nodeGetElementsByName(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeQuerySelectorAll(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeQuerySelector(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalFrameSlot(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalInfo(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
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
int globalCEHas(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalCEGet(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalCEGetName(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalCEDefine(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int globalCEUpgrade(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
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

            if (!setCookie.isEmpty()) {
                /* fillResponse() joins repeated Set-Cookie headers with '\n'
                 * (HTTPHeaderMap can only hold one value per name). One line is
                 * one cookie: a response that set BAIDUID plus neighbours used
                 * to keep only the last one. */
                std::string all = setCookie.utf8().data();
                size_t start = 0;

                while (start <= all.size()) {
                    size_t end = all.find('\n', start);

                    if (end == std::string::npos)
                        end = all.size();
                    if (end > start)
                        setCookieForDocument(g_document, all.substr(start, end - start));
                    if (end == all.size())
                        break;
                    start = end + 1;
                }
            }
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
static void setCookieForHost(const std::string& defaultHost, const std::string& text)
{
    std::string host = defaultHost;
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

static void setCookieForDocument(WebCore::Document* document, const std::string& text)
{
    setCookieForHost(hostOfDocument(document), text);
}

/* Called from ResourceHandleNet::fillResponse for every response (main
 * document, scripts, images, XHR). The default domain is the RESPONSE host
 * (hector.baidu.com), not the document host; Domain= still overrides it. */
void addCookiesFromResponseImpl(const char* url, const char* setCookie)
{
    std::string host;
    std::string all;
    size_t start = 0;

    if (!url || !setCookie)
        return;

    {
        WebCore::KURL kurl(url);
        host = std::string(kurl.host().utf8().data());
    }
    if (host.empty())
        return;

    all = setCookie;
    while (start <= all.size()) {
        size_t end = all.find('\n', start);

        if (end == std::string::npos)
            end = all.size();
        if (end > start)
            setCookieForHost(host, all.substr(start, end - start));
        if (end == all.size())
            break;
        start = end + 1;
    }
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

static int assignBoolean(AntbEngine* engine, AntbValue* out, int value)
{
    AntbValue* v = antb_make_boolean(engine, value);

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

    if (!strcmp(name, "fullscreenElement") || !strcmp(name, "webkitFullscreenElement")) {
        WebCore::Element* fullscreen = d->fullscreenElement();

        if (!fullscreen)
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, fullscreen);

            if (!v)
                return assignNull(engine, out);
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        return 1;
    }

    if (!strcmp(name, "fullscreenEnabled") || !strcmp(name, "webkitFullscreenEnabled")) {
        AntbValue* v = antb_make_boolean(engine, 1);

        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

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
        /* Live HTMLCollection over the document's own tree (B2-2); this used
         * to be a snapshot array, so a form/script added later never showed up
         * in document.forms / document.scripts. */
        AntbValue* list = wrapCollection(engine, d, AgbCollectionDocumentList, std::string(name));

        if (!list)
            return 0;
        antb_value_assign(out, list);
        antb_value_release(list);
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
        AntbValue* v = antb_make_boolean(engine, documentHidden() ? 1 : 0);

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

    if (!strcmp(name, "referrer") || !strcmp(name, "domain") || !strcmp(name, "compatMode")) {
        AntbValue* v = antb_make_string(engine, strcmp(name, "compatMode") == 0 ? "CSS1Compat" : "");
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    /* The decoder really knows this one now (transport charset / meta / BOM).
     * Sites and test harnesses branch on it to detect mojibake, and the empty
     * string this used to return made that impossible; inputEncoding was not
     * even answered here (undefined). */
    if (!strcmp(name, "characterSet") || !strcmp(name, "charset") || !strcmp(name, "inputEncoding")) {
        WebCore::String encoding = d->inputEncoding();
        AntbValue* v = antb_make_string(engine, encoding.utf8().data());
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

/* --- HTML import maps ---------------------------------------------------- *
 * <script type="importmap"> (JSON) is captured by the tokenizer and registered
 * here before any module runs. The bridge asks importMapResolve() for every
 * bare module specifier: a hit hands back the mapped URL, a miss returns NULL
 * and the engine keeps its own resolution (Node-style package lookup, which is
 * what ant does for a CLI module).
 *
 * Lifetime: the parsed map belongs to the document's DocumentRuntime and is
 * freed by releaseDocumentRuntime(), so a navigation never resolves a
 * specifier through the previous page's map. Only the first map of a document
 * is used, per the standard.
 *
 * Targets are resolved against the document base URL here (relative targets are
 * legal), and for file:// documents the result is translated into the engine's
 * masqueraded http://file.ant/ space like every other module filename. */
static bool importMapLookup(const std::string& specifier, std::string* out)
{
    std::map<std::string, std::string>* map;

    if (!g_currentDocument || !g_currentDocument->importMap || !out)
        return false;

    map = (std::map<std::string, std::string>*)g_currentDocument->importMap;

    {
        std::map<std::string, std::string>::const_iterator exact = map->find(specifier);

        if (exact != map->end()) {
            *out = exact->second;
            return true;
        }
    }

    /* Prefix entries end with '/': the longest match wins and the rest of the
     * specifier is appended to the target (HTML import map semantics). */
    {
        std::string bestKey;
        std::string bestTarget;
        std::map<std::string, std::string>::const_iterator it = map->begin();

        for (; it != map->end(); ++it) {
            const std::string& key = it->first;

            if (key.empty() || key[key.size() - 1] != '/')
                continue;
            if (specifier.size() <= key.size() || specifier.compare(0, key.size(), key) != 0)
                continue;
            if (key.size() <= bestKey.size())
                continue;

            bestKey = key;
            bestTarget = it->second;
        }

        if (!bestKey.empty()) {
            *out = bestTarget + specifier.substr(bestKey.size());
            return true;
        }
    }

    return false;
}

/* The bridge resolver (antb_set_import_map_resolver): a malloc'd URL, or NULL
 * for "not in the map". Runs inside module resolution, so it must not call back
 * into JS - it only reads the parsed map. */
static char* importMapResolve(const char* specifier, const char* base)
{
    std::string mapped;

    (void)base;   /* the map belongs to the document being evaluated, which is
                   * the current binding while a module resolves */

    if (!specifier || !specifier[0] || !importMapLookup(std::string(specifier), &mapped))
        return NULL;

    {
        char* out = (char*)malloc(mapped.size() + 1);

        if (out)
            memcpy(out, mapped.c_str(), mapped.size() + 1);
        return out;
    }
}

/* A mapped bare specifier as the module cache/fetcher sees it, or "" when the
 * map has no entry. Used by the prefetcher to start the download early. */
static std::string importMapResolveUrl(const std::string& specifier)
{
    std::string mapped;

    if (!importMapLookup(specifier, &mapped))
        return std::string();

    return mapped;
}

/* Object.keys(value) through the engine (no key enumeration in the bridge). */
static AntbValue* objectOwnKeys(AntbValue* object)
{
    AntbValue* global = antb_global(g_engine);
    AntbValue* constructor = global ? antb_get_prop(g_engine, global, "Object") : NULL;
    AntbValue* keysFn = constructor ? antb_get_prop(g_engine, constructor, "keys") : NULL;
    AntbValue* keys = NULL;

    if (keysFn) {
        AntbValue* args[1];

        args[0] = object;
        keys = antb_call(g_engine, keysFn, constructor, args, 1, NULL);
    }
    if (keysFn)
        antb_value_release(keysFn);
    if (constructor)
        antb_value_release(constructor);
    if (global)
        antb_value_release(global);
    return keys;
}

/* Parses the import map JSON with the engine's JSON.parse (ant ships one; no
 * second parser here) and stores specifier -> absolute URL. Returns NULL for
 * malformed JSON or a map with no entries. */
static std::map<std::string, std::string>* parseImportMapJson(const char* json,
                                                              WebCore::Document* document)
{
    std::map<std::string, std::string>* map = new std::map<std::string, std::string>();
    AntbValue* global = antb_global(g_engine);
    AntbValue* jsonCtor = global ? antb_get_prop(g_engine, global, "JSON") : NULL;
    AntbValue* parse = jsonCtor ? antb_get_prop(g_engine, jsonCtor, "parse") : NULL;
    AntbValue* text = antb_make_string(g_engine, json);
    AntbValue* parsed = NULL;

    if (getenv("AGB_MODULE_DEBUG"))
        fprintf(stderr, "[importmap] parse %u bytes\n", (unsigned)strlen(json));

    if (parse && text) {
        AntbValue* args[1];

        args[0] = text;
        parsed = antb_call(g_engine, parse, jsonCtor, args, 1, NULL);
    }

    if (parsed) {
        AntbValue* imports = antb_get_prop(g_engine, parsed, "imports");
        AntbValue* keys = imports ? objectOwnKeys(imports) : NULL;

        if (keys) {
            AntbValue* lengthValue = antb_get_prop(g_engine, keys, "length");
            int count = lengthValue ? (int)antb_to_number(lengthValue) : 0;
            WebCore::KURL baseUrl(document->baseURL());
            int i;

            for (i = 0; i < count; ++i) {
                AntbValue* keyValue = antb_array_get(keys, i);
                char* key = (keyValue && antb_is_string(keyValue)) ? antb_to_utf8(keyValue) : NULL;

                if (key && key[0]) {
                    AntbValue* targetValue = antb_get_prop(g_engine, imports, key);

                    if (targetValue && antb_is_string(targetValue)) {
                        char* target = antb_to_utf8(targetValue);

                        if (target && target[0]) {
                            WebCore::String resolved =
                                WebCore::KURL(baseUrl, WebCore::String::fromUTF8(target).deprecatedString()).url();
                            std::string url = resolved.utf8().data();

                            /* file:// import map targets live in the same
                             * masqueraded space as module filenames. */
                            if (isFileUrl(url))
                                url = engineUrlForFile(url);
                            (*map)[key] = url;
                        }

                        if (target)
                            antb_free(target);
                    }
                    if (targetValue)
                        antb_value_release(targetValue);
                }

                if (key)
                    antb_free(key);
                if (keyValue)
                    antb_value_release(keyValue);
            }

            if (lengthValue)
                antb_value_release(lengthValue);
            antb_value_release(keys);
        }

        if (imports)
            antb_value_release(imports);
        antb_value_release(parsed);
    }

    if (text)
        antb_value_release(text);
    if (parse)
        antb_value_release(parse);
    if (jsonCtor)
        antb_value_release(jsonCtor);
    if (global)
        antb_value_release(global);

    return map;
}

/* <script type="importmap">: registers this document's map (called by the
 * tokenizer). A document keeps its first map; later ones are ignored. */
void registerImportMap(const char* json, void* documentPtr)
{
    WebCore::Document* document = (WebCore::Document*)documentPtr;
    DocumentRuntime* runtime;
    std::map<std::string, std::string>* map;

    if (!g_engine || !json || !json[0] || !document)
        return;

    runtime = documentRuntimeFor(g_currentState, document);

    if (!runtime)
        return;

    if (runtime->importMap) {
        agbLog(AGB_LOG_PARSE, "import map ignored (document already has one)");
        return;
    }

    map = parseImportMapJson(json, document);

    if (!map || map->empty()) {
        delete map;
        return;
    }

    runtime->importMap = map;

    agbLog(AGB_LOG_PARSE, "import map registered (%u entries)", (unsigned)map->size());
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
                } else if (!specifier.empty() && specifier[0] != '/') {
                    /* A bare specifier: only the document's import map can say
                     * where it points (GitHub's react/react-dom). Prefetch the
                     * mapped URL when it is a network one; file-masqueraded
                     * targets are left to the engine's fetch path, which knows
                     * how to translate them. */
                    std::string mapped = importMapResolveUrl(specifier);

                    if (!mapped.empty() && mapped.compare(0, 5, "http:") == 0
                            && !isEngineFileUrl(mapped)) {
                        ++found;
                        moduleStart(mapped, false);
                    }
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
    /* HTML import maps: bare module specifiers resolve through the current
     * document's <script type="importmap"> (see importMapResolve). Process
     * wide like the fetcher; both are idempotent. */
    antb_set_import_map_resolver(importMapResolve);

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
    /* Likewise: the import-map resolver is process wide and reads the current
     * document's map, so installing it again here is harmless. */
    antb_set_import_map_resolver(importMapResolve);
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

void setContextHostId(void* context, int viewId)
{
    RuntimeState* state = (RuntimeState*)context;

    if (state && state != &g_emptyState)
        state->hostViewId = viewId;
}

bool contextInfo(void* context, ContextInfo* out)
{
    RuntimeState* state = (RuntimeState*)context;
    DocumentRuntime* doc;

    if (!out)
        return false;

    memset(out, 0, sizeof(*out));

    if (!state || state == &g_emptyState)
        return false;

    /* activeDocument is what activateState() restores for this view, so this is
     * right even while the engine thread is acting for a different view. */
    doc = state->activeDocument ? state->activeDocument : &g_emptyDocument;

    out->viewId = state->hostViewId;
    out->pid = (unsigned long)GetCurrentProcessId();
    out->realmGeneration = state->generation;
    out->documentGeneration = doc->generation;
    out->engine = (unsigned long long)(size_t)state->engine;
    out->realm = (unsigned long long)(size_t)(doc->realm ? doc->realm : state->mainRealm);
    out->document = (unsigned long long)(size_t)doc->document;
    /* Same numbering as __agbFrameSlot(): 0 = main frame, 1..N = frame realms
     * (realmSlot stores 0-based frame slots, -1 for the main document). */
    out->slot = doc->realmSlot >= 0 ? doc->realmSlot + 1 : 0;

    /* mainFrame from the engine's own frame tree, not from the realm bookkeeping:
     * in the shared-realm fallback a frame document shares the main realm, so
     * "realmSlot < 0" would call every frame the main frame. */
    {
        WebCore::Frame* frame = doc->document ? doc->document->frame() : 0;
        WebCore::Page* page = frame ? frame->page() : 0;

        out->mainFrame = (frame && page && page->mainFrame() == frame) ? 1 : 0;
    }

    out->hidden = state->hidden ? 1 : 0;
    return true;
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
        agbLog(AGB_LOG_EVT, "evaluate enter depth=%d len=%d realm=%p", g_evalDepth, (int)len, (void*)currentRealm());

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
    /* The old page's listeners close over its DOM; they are released together
     * with the document records (each document owns its own listeners now). */

    /* ...and the JS wrappers of its nodes must not be handed out to the next
     * document (the bridge keeps them in a process wide cache). */
    if (state->engine && g_document)
        antb_release_owner(state->engine, g_document);

    /* The interface classes (and the media/selection classes they own) belong
     * to the old engine. */
    g_nodeClass = 0;
    g_canvasCtxClass = 0;
    g_windowClass = 0;
    g_nodeListClass = 0;
    g_htmlCollectionClass = 0;
    g_styleSheetListClass = 0;
    g_styleSheetClass = 0;
    g_cssRuleClass = 0;
    g_cssRuleListClass = 0;
    g_mediaListClass = 0;
    g_cssDeclarationClass = 0;
    resetInterfaceClasses(state);
    /* Per-realm interface sets (B2-4): the class objects die with the engine,
     * so only the bookkeeping is dropped here. */
    if (state->realmIfaces) {
        std::map<void*, RealmInterfaces*>::iterator it = state->realmIfaces->begin();

        for (; it != state->realmIfaces->end(); ++it) {
            if (!it->second)
                continue;
            delete it->second->tagClasses;
            delete it->second;
        }
        delete state->realmIfaces;
        state->realmIfaces = NULL;
    }
    state->ifacesActive = NULL;
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
    AntbClass* klass;

    if (!engine || !document)
        return NULL;

    /* The document is a regular interface instance: classForNode() picks
     * HTMLDocument and the wrapper cache keeps `document === document.body.
     * ownerDocument` true. Its methods live on Document.prototype (installed
     * once per engine), not on every document object. */
    ensureInterfaceClasses(engine);
    klass = classForNode(document);
    if (!klass)
        return NULL;

    docObj = antb_wrap_native_as_owner(engine, klass, document, AGB_TAG_NODE, document);
    if (!docObj)
        return NULL;

    /* Same as wrapNode(): remember the creating realm for the attribute
     * accessors, which run on the engine-level prototype. */
    if (!antb_get_native(docObj, AGB_TAG_WRAPREALM))
        antb_set_native(docObj, (void*)antb_realm_current(engine), AGB_TAG_WRAPREALM);

    /* document.implementation: per document object, created on first wrap.
     * jQuery and others feature-test document.implementation.createHTMLDocument. */
    {
        AntbValue* existing = antb_get_prop(engine, docObj, "implementation");

        if (!existing) {
            AntbValue* implObj = antb_make_object(engine);

            if (implObj) {
                antb_define_method(engine, implObj, "createHTMLDocument", docCreateHTMLDocument, NULL);
                antb_define_method(engine, implObj, "hasFeature", docHasFeature, NULL);
                antb_set_prop(engine, docObj, "implementation", implObj);
                antb_value_release(implObj);
            }
        } else {
            antb_value_release(existing);
        }
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
        g_windowClass = antb_class_create_iface(engine, "Window", NULL);
        if (!g_windowClass)
            return NULL;
        antb_class_expose(engine, g_windowClass);
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

        ensureCanvasContextClass(g_engine);


        /* window.addEventListener is what jQuery's ready setup calls first;
         * listeners live on the document node so both targets share one list. */
        antb_set_native(window, g_document, AGB_TAG_NODE);
        antb_define_method(g_engine, window, "addEventListener", nodeAddEventListener, NULL);
        antb_define_method(g_engine, window, "removeEventListener", nodeRemoveEventListener, NULL);
        antb_define_method(g_engine, window, "dispatchEvent", nodeDispatchEvent, NULL);
        /* window.getSelection() and document.getSelection() are the same live
         * view over the frame's selection (the wrapper is cached per object). */
        antb_define_method(g_engine, window, "getSelection", docGetSelection, NULL);

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
        antb_define_method(g_engine, global, "__agbInfo", globalInfo, NULL);
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
        /* custom element registry (prelude's `customElements` object calls these;
         * the storage is per document, see customDefs). */
        antb_define_method(g_engine, global, "__agbCEHas", globalCEHas, NULL);
        antb_define_method(g_engine, global, "__agbCEGet", globalCEGet, NULL);
        antb_define_method(g_engine, global, "__agbCEGetName", globalCEGetName, NULL);
        antb_define_method(g_engine, global, "__agbCEDefine", globalCEDefine, NULL);
        antb_define_method(g_engine, global, "__agbCEUpgrade", globalCEUpgrade, NULL);
        antb_define_method(g_engine, global, "__agbAttachShadow", globalAttachShadow, NULL);
        /* Fullscreen state transitions (issues #27): prelude wraps these in the
         * Promise + task deferral Chrome has; the engine owns the state and the
         * fullscreenchange event. */
        antb_define_method(g_engine, global, "__agbRequestFullscreen", elementRequestFullscreen, NULL);
        antb_define_method(g_engine, global, "__agbExitFullscreen", docExitFullscreen, NULL);
        /* Custom element constructors run through this hook (issues #18/#22). */
        antb_set_construct_hook(g_engine, g_constructHookFn);
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
/* Frame realms are created by the bridge, and ant's per-realm bootstrap only
 * covers intrinsics plus the timers the bridge installs itself: the web globals
 * from ant_bootstrap_modules (URL, URLSearchParams, TextEncoder/Decoder, atob,
 * Blob/File/FileReader, FormData, Headers, Request/Response, WebSocket,
 * EventSource, MessageChannel/Port, BroadcastChannel, structuredClone, crypto,
 * EventTarget, AbortController/Signal, DOMException, the streams) exist only on
 * the *main* realm's global. Without them iframe code that touches any of them
 * throws ReferenceError - monaco's isolated playground iframe died on
 * `URLSearchParams` and the playground never rendered.
 *
 * They are copied from the parent realm's global (same VM, so a cross-realm
 * reference is valid) and only when the frame realm still lacks the name: a
 * page/frame that installed its own shim keeps it. The copy runs before
 * prelude/polyfills for the realm, so the URLSearchParams wrapper sees the
 * native constructor. */
static void installSharedWebGlobals(AntbEngine* engine, WebCore::Document* document)
{
    static const char* const kNames[] = {
        "URL", "URLSearchParams", "URLPattern",
        "TextEncoder", "TextDecoder", "TextEncoderStream", "TextDecoderStream",
        "atob", "btoa",
        "Blob", "File", "FileReader", "FormData", "Headers",
        "Request", "Response",
        "WebSocket", "EventSource",
        "MessageChannel", "MessagePort", "BroadcastChannel",
        "structuredClone", "crypto",
        "EventTarget", "AbortController", "AbortSignal", "DOMException",
        /* The ant events module only initialises the main realm, so a frame
         * realm was left with prelude stubs for these: `new Event()` there
         * produced an empty object and the event facade had no native base.
         * Copy the constructors like EventTarget (R176). */
        "Event", "CustomEvent", "MessageEvent", "ErrorEvent", "PromiseRejectionEvent",
        "ReadableStream", "WritableStream", "TransformStream",
        "CompressionStream", "DecompressionStream"
    };
    WebCore::Document* parent = document ? document->parentDocument() : NULL;
    AntbRealm* parentRealm = parent ? realmOfDocument(engine, parent) : NULL;
    AntbValue* source = parentRealm ? antb_realm_global(engine, parentRealm) : NULL;
    AntbValue* target = antb_global(engine);
    size_t i;

    if (!source || !target) {
        if (source)
            antb_value_release(source);
        if (target)
            antb_value_release(target);
        return;
    }

    for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        AntbValue* value = antb_get_prop(engine, source, kNames[i]);

        if (!value)
            continue;

        {
            AntbValue* existing = antb_get_prop(engine, target, kNames[i]);

            if (!existing)
                antb_set_prop(engine, target, kNames[i], value);
            else
                antb_value_release(existing);
        }

        antb_value_release(value);
    }

    antb_value_release(source);
    antb_value_release(target);
}

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
        installSharedWebGlobals(engine, runtime->document);
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

    /* Interface constructors of this engine on the frame realm's global (the
     * prototypes themselves are engine-scoped, like every bridge class). */
    antb_expose_classes(engine);

    loadPrelude();
    loadPolyfills();
}

/* Aborts whatever page script is on this engine thread. The bridge's interrupt
 * registry is lock protected (R146), so the renderer's watchdog thread may call
 * it while the engine thread is stuck inside a script. */
void interruptScripts(void)
{
    antb_interrupt_all_engines();
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

            /* The realm's interface set dies with it (the class objects
             * themselves belong to the engine and are freed with it). */
            if (runtime->realm && owner->realmIfaces) {
                std::map<void*, RealmInterfaces*>::iterator sit =
                    owner->realmIfaces->find((void*)runtime->realm);

                if (sit != owner->realmIfaces->end()) {
                    delete sit->second->tagClasses;
                    delete sit->second;
                    owner->realmIfaces->erase(sit);
                }
            }

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

    if (getenv("AGB_BIND_TRACE"))
        agbLog(AGB_LOG_EVT, "bindDocument target=%p parent=%p prev=%p depth=%d",
               (void*)target, (void*)(target->parentDocument() ? target->parentDocument() : target),
               (void*)previous, antb_engines_js_depth());

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

    /* The realm is part of the binding: the engine may be left in a frame realm
     * by the previous dispatch (a script that appends an <iframe> binds the
     * child while it runs), and installing this document's globals on the wrong
     * global object silently moves the caller's `document` to that frame. */
    if (runtime->realm && antb_realm_current(state->engine) != runtime->realm)
        antb_realm_enter(state->engine, runtime->realm);

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
        AntbValue* global;
        AntbValue* docObj;

        /* The interface classes must exist before the document object can be
         * wrapped (the document is an HTMLDocument instance like any node). */
        ensureInterfaceClasses(g_engine);

        global = antb_global(g_engine);
        docObj = documentObjectFor(g_engine, g_document);

        if (!global || !docObj) {
            antb_value_release(global);
            antb_value_release(docObj);
            return;
        }

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

    /* Interface constructors before prelude: its stub list only fills names
     * that are still undefined. */
    antb_expose_classes(g_engine);

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

/* CanvasRenderingContext2D: one interface class, created with the rest so
 * its constructor exists before prelude and getContext() can wrap into it. */
static void ensureCanvasContextClass(AntbEngine* engine)
{
    if (!engine || g_canvasCtxClass)
        return;

    g_canvasCtxClass = antb_class_create_iface(engine, "CanvasRenderingContext2D", NULL);
    if (!g_canvasCtxClass)
        return;
    antb_class_expose(engine, g_canvasCtxClass);

    antb_class_add_method(engine, g_canvasCtxClass, "save", canvasCtxMethod, (void*)(intptr_t)CV_SAVE);
    antb_class_add_method(engine, g_canvasCtxClass, "restore", canvasCtxMethod, (void*)(intptr_t)CV_RESTORE);
    antb_class_add_method(engine, g_canvasCtxClass, "scale", canvasCtxMethod, (void*)(intptr_t)CV_SCALE);
    antb_class_add_method(engine, g_canvasCtxClass, "rotate", canvasCtxMethod, (void*)(intptr_t)CV_ROTATE);
    antb_class_add_method(engine, g_canvasCtxClass, "translate", canvasCtxMethod, (void*)(intptr_t)CV_TRANSLATE);
    antb_class_add_method(engine, g_canvasCtxClass, "clearRect", canvasCtxMethod, (void*)(intptr_t)CV_CLEAR_RECT);
    antb_class_add_method(engine, g_canvasCtxClass, "fillRect", canvasCtxMethod, (void*)(intptr_t)CV_FILL_RECT);
    antb_class_add_method(engine, g_canvasCtxClass, "strokeRect", canvasCtxMethod, (void*)(intptr_t)CV_STROKE_RECT);
    antb_class_add_method(engine, g_canvasCtxClass, "beginPath", canvasCtxMethod, (void*)(intptr_t)CV_BEGIN_PATH);
    antb_class_add_method(engine, g_canvasCtxClass, "closePath", canvasCtxMethod, (void*)(intptr_t)CV_CLOSE_PATH);
    antb_class_add_method(engine, g_canvasCtxClass, "moveTo", canvasCtxMethod, (void*)(intptr_t)CV_MOVE_TO);
    antb_class_add_method(engine, g_canvasCtxClass, "lineTo", canvasCtxMethod, (void*)(intptr_t)CV_LINE_TO);
    antb_class_add_method(engine, g_canvasCtxClass, "quadraticCurveTo", canvasCtxMethod, (void*)(intptr_t)CV_QUADRATIC);
    antb_class_add_method(engine, g_canvasCtxClass, "bezierCurveTo", canvasCtxMethod, (void*)(intptr_t)CV_BEZIER);
    antb_class_add_method(engine, g_canvasCtxClass, "arc", canvasCtxMethod, (void*)(intptr_t)CV_ARC);
    antb_class_add_method(engine, g_canvasCtxClass, "arcTo", canvasCtxMethod, (void*)(intptr_t)CV_ARC_TO);
    antb_class_add_method(engine, g_canvasCtxClass, "rect", canvasCtxMethod, (void*)(intptr_t)CV_RECT);
    antb_class_add_method(engine, g_canvasCtxClass, "fill", canvasCtxMethod, (void*)(intptr_t)CV_FILL);
    antb_class_add_method(engine, g_canvasCtxClass, "stroke", canvasCtxMethod, (void*)(intptr_t)CV_STROKE);
    antb_class_add_method(engine, g_canvasCtxClass, "clip", canvasCtxMethod, (void*)(intptr_t)CV_CLIP);
    antb_class_add_method(engine, g_canvasCtxClass, "drawImage", canvasCtxMethod, (void*)(intptr_t)CV_DRAW_IMAGE);
    antb_class_add_method(engine, g_canvasCtxClass, "createLinearGradient", canvasCtxMethod, (void*)(intptr_t)CV_CREATE_LINEAR);
    antb_class_add_method(engine, g_canvasCtxClass, "createRadialGradient", canvasCtxMethod, (void*)(intptr_t)CV_CREATE_RADIAL);
    antb_class_add_method(engine, g_canvasCtxClass, "createPattern", canvasCtxMethod, (void*)(intptr_t)CV_CREATE_PATTERN);
    antb_class_add_method(engine, g_canvasCtxClass, "measureText", canvasCtxMethod, (void*)(intptr_t)CV_MEASURE_TEXT);
    antb_class_add_method(engine, g_canvasCtxClass, "fillText", canvasCtxMethod, (void*)(intptr_t)CV_FILL_TEXT);
    antb_class_add_method(engine, g_canvasCtxClass, "strokeText", canvasCtxMethod, (void*)(intptr_t)CV_STROKE_TEXT);
    antb_class_add_method(engine, g_canvasCtxClass, "setTransform", canvasCtxMethod, (void*)(intptr_t)CV_SET_TRANSFORM);
    antb_class_add_method(engine, g_canvasCtxClass, "transform", canvasCtxMethod, (void*)(intptr_t)CV_SET_TRANSFORM);
    antb_class_add_method(engine, g_canvasCtxClass, "setLineDash", canvasCtxMethod, (void*)(intptr_t)CV_SET_LINE_DASH);
    antb_class_add_method(engine, g_canvasCtxClass, "getLineDash", canvasCtxMethod, (void*)(intptr_t)CV_GET_LINE_DASH);
    antb_class_add_method(engine, g_canvasCtxClass, "getImageData", canvasCtxMethod, (void*)(intptr_t)CV_GET_IMAGE_DATA);
    antb_class_add_method(engine, g_canvasCtxClass, "putImageData", canvasCtxMethod, (void*)(intptr_t)CV_PUT_IMAGE_DATA);
    antb_class_add_method(engine, g_canvasCtxClass, "isPointInPath", canvasCtxMethod, (void*)(intptr_t)CV_IS_POINT_IN_PATH);
}


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
static std::vector<Listener>*& listenersForRealm(AntbRealm* realm)
{
    static std::vector<Listener>* noRealm = NULL;
    RuntimeState* state = g_currentState;

    if (!state || state == &g_emptyState || !realm)
        return noRealm;

    if (!state->realmListeners)
        state->realmListeners = new std::map<AntbRealm*, std::vector<Listener>*>();

    return (*state->realmListeners)[realm];
}

static std::vector<Listener>*& listeners()
{
    return listenersForRealm(currentRealm());
}

/* The realm the *receiver* of the call belongs to.
 *
 * A bridged class method runs in the realm that created the class (the class
 * prototypes are engine-scoped, created in the main realm), so currentRealm()
 * inside a child frame's callback names the main realm. The host mirror is
 * switched to the callee's document for the duration of the callback
 * (contextActivate), but listeners belong to the node's own document/realm:
 * registering them on the callee realm made a frame's DOMContentLoaded
 * listener land in the parent's list and never fire. */
static AntbRealm* receiverRealm(WebCore::Node* n)
{
    AntbRealm* realm;

    if (n && n->document() && g_engine) {
        realm = realmOfDocument(g_engine, n->document());
        if (realm)
            return realm;
    }

    return currentRealm();
}

#define g_listeners listeners()

/* Event handler properties (`script.onload = fn`, `img.onerror = fn`): the DOM
 * treats them as a single replaceable listener. They were plain expandos here,
 * so pages that wait for onload (DeepSeek's WeChat QR loader among them) never
 * ran their callback and failed silently. */
static AntbValue* findNodeEventHandler(WebCore::Node* n, const char* type)
{
    std::vector<Listener>* list;

    if (!n || !type || !g_engine)
        return 0;

    list = listenersForRealm(receiverRealm(n));

    if (!list)
        return 0;

    for (size_t i = 0; i < list->size(); ++i) {
        Listener& l = (*list)[i];

        if (l.handler && l.node == n && l.type == type && l.fn)
            return l.fn;
    }

    return 0;
}

static int setNodeEventHandler(WebCore::Node* n, const char* type, const AntbValue* value)
{    std::vector<Listener>* list;

    if (!n || !type || !g_engine)
        return 0;

    list = listenersForRealm(receiverRealm(n));

    if (!list) {
        AntbRealm* realm = receiverRealm(n);

        if (!realm)
            return 0;

        list = new std::vector<Listener>();
        listenersForRealm(realm) = list;
    }

    /* One handler per (node, type): assigning replaces the previous one. */
    for (size_t i = 0; i < list->size(); ) {
        Listener& l = (*list)[i];

        if (l.handler && l.node == n && l.type == type) {
            if (l.fn)
                antb_value_release(l.fn);
            list->erase(list->begin() + i);
        } else {
            ++i;
        }
    }

    if (value && antb_is_function(value)) {
        Listener l;

        l.node = n;
        l.type = type;
        l.fn = antb_retain(value);
        l.realm = receiverRealm(n);
        l.once = false;
        l.handler = true;
        list->push_back(l);

        if (getenv("AGB_EVT_DEBUG"))
            agbLog(AGB_LOG_EVT, "on%s handler node=%p listeners=%d", type, (void*)n, (int)list->size());
    }

    return 1;
}


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

    g_styleSheetListClass = antb_class_create_iface(g_engine, "StyleSheetList", NULL);
    if (g_styleSheetListClass) {
        antb_class_set_property(g_engine, g_styleSheetListClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_LIST);
        antb_class_add_method(g_engine, g_styleSheetListClass, "item",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_ITEM);
    }

    g_styleSheetClass = antb_class_create_iface(g_engine, "CSSStyleSheet", NULL);
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

    g_cssRuleClass = antb_class_create_iface(g_engine, "CSSRule", NULL);
    if (g_cssRuleClass) {
        antb_class_set_property(g_engine, g_cssRuleClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_RULE);
        antb_class_add_method(g_engine, g_cssRuleClass, "insertRule",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_INSERT_RULE);
        antb_class_add_method(g_engine, g_cssRuleClass, "deleteRule",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_DELETE_RULE);
    }

    g_cssRuleListClass = antb_class_create_iface(g_engine, "CSSRuleList", NULL);
    if (g_cssRuleListClass) {
        antb_class_set_property(g_engine, g_cssRuleListClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_RULELIST);
        antb_class_add_method(g_engine, g_cssRuleListClass, "item",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_ITEM);
    }

    g_mediaListClass = antb_class_create_iface(g_engine, "MediaList", NULL);
    if (g_mediaListClass) {
        antb_class_set_property(g_engine, g_mediaListClass, "",
                                cssomGetter, (void*)(intptr_t)CSSOM_MEDIA);
        antb_class_add_method(g_engine, g_mediaListClass, "item",
                              cssomMethod, (void*)(intptr_t)CSSOM_OP_ITEM);
    }

    g_cssDeclarationClass = antb_class_create_iface(g_engine, "CSSStyleDeclaration", NULL);
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

    /* Indexed access: style[0] is the first property name and style[i] the i-th
     * (CSSStyleDeclaration is array-like in Chrome; `style[0]` is how a lot of
     * code enumerates the inline properties). */
    {
        unsigned index;

        if (cssomNumericName(name, &index)) {
            WebCore::String property = element ? element->style()->item(index) : WebCore::String();

            makeStringResult(engine, out, property.isNull() ? "" : property.utf8().data());
            return 1;
        }
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

    /* A shadow node's root is the ShadowRoot object, not the document. */
    if (n->shadowScope() > 0 && n->document()) {
        WebCore::Element* host = n->document()->shadowHostForScope(n->shadowScope());

        if (host) {
            AntbValue* v = wrapShadowRoot(engine, host);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
            return 1;
        }
    }

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

/* element.getAttributeNode(name): the Attr node, or null. */
int nodeGetAttributeNode(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                         AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* name;

    if (!n || !n->isElementNode() || argc < 1 || !args[0])
        return assignNull(engine, out);

    name = antb_to_utf8(args[0]);
    if (!name)
        return assignNull(engine, out);

    {
        RefPtr<WebCore::Attr> attr =
            static_cast<WebCore::Element*>(n)->getAttributeNode(WebCore::String::fromUTF8(name));

        antb_free(name);
        if (!attr)
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, attr.get());

            if (!v)
                return assignNull(engine, out);
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        return 1;
    }
}

/* element.getAttributeNS(ns, name) / setAttributeNS(ns, name, value). */
int nodeGetAttributeNS(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* ns;
    char* name;

    if (!n || !n->isElementNode() || argc < 2 || !args[0] || !args[1])
        return assignNull(engine, out);

    ns = antb_to_utf8(args[0]);
    name = antb_to_utf8(args[1]);
    if (!name)
        return assignNull(engine, out);

    {
        WebCore::String value = static_cast<WebCore::Element*>(n)->getAttributeNS(
            WebCore::String::fromUTF8(ns ? ns : ""), WebCore::String::fromUTF8(name));
        AntbValue* v;

        if (ns)
            antb_free(ns);
        antb_free(name);
        v = antb_make_string(engine, value.utf8().data());
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }
}

int nodeSetAttributeNS(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* ns;
    char* name;
    char* value;

    (void)engine;
    (void)out;

    if (!n || !n->isElementNode() || argc < 3)
        return 1;

    ns = antb_to_utf8(args[0]);
    name = antb_to_utf8(args[1]);
    value = antb_to_utf8(args[2]);
    if (!name)
        return 1;

    {
        WebCore::ExceptionCode ec = 0;

        static_cast<WebCore::Element*>(n)->setAttributeNS(
            WebCore::String::fromUTF8(ns ? ns : ""),
            WebCore::String::fromUTF8(name),
            WebCore::String::fromUTF8(value ? value : ""), ec);
        if (ns)
            antb_free(ns);
        antb_free(name);
        if (value)
            antb_free(value);
    }
    return 1;
}

/* element.getClientRects(): the spec returns a list; one rect is what this
 * engine's own geometry reports, so an array with that rect answers the callers
 * that iterate it. */
int nodeGetClientRects(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    AntbValue* list = antb_make_array(engine);
    AntbValue* rect = NULL;

    if (!list)
        return 0;

    nodeGetBoundingClientRect(engine, self, args, argc, rect = antb_make_object(engine), NULL);
    if (rect)
        antb_array_push(engine, list, rect);
    if (rect)
        antb_value_release(rect);

    antb_value_assign(out, list);
    antb_value_release(list);
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

    {
        WebCore::Element* shadowHost = shadowHostOfWrapper(self);

        if (shadowHost) {
            replaceShadowChildren(shadowHost, args, argc);
            return 1;
        }
    }

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

/* ParentNode/ChildNode mixins: the 2008 IDL has none of these, so they are extra
 * methods (kExtraMethods). Chrome's signatures are variadic and every non-node
 * argument is converted with String() - append(null) really inserts the text
 * "null" - so the conversion goes through antb_to_utf8 rather than skipping
 * non-node values. These used to be a JS polyfill in polyfills.js (G1-3). */
static WebCore::Node* mixinArgNode(AntbValue* arg, WebCore::Document* document,
                                   RefPtr<WebCore::Node>& hold)
{
    WebCore::Node* n = arg ? nodeOf(arg) : NULL;

    if (n) {
        hold = n;
        return n;
    }
    if (!arg || !document)
        return NULL;

    {
        char* text = antb_to_utf8(arg);

        if (!text)
            return NULL;
        hold = document->createTextNode(WebCore::String::fromUTF8(text));
        antb_free(text);
    }
    return hold.get();
}

/* op: 0 append, 1 prepend, 2 before, 3 after, 4 replaceWith. */
static int mixinInsert(AntbValue* self, AntbValue** args, int argc, int op)
{
    WebCore::Node* receiver = nodeOf(self);
    WebCore::Document* document;
    WebCore::Node* parent = NULL;
    WebCore::Node* ref = NULL;
    RefPtr<WebCore::Node> refHold;
    WebCore::ExceptionCode ec = 0;
    int i;

    if (!receiver)
        return 1;

    document = receiver->document();

    switch (op) {
    case 0: parent = receiver; ref = NULL; break;
    case 1: parent = receiver; ref = receiver->firstChild(); break;
    case 2: parent = receiver->parentNode(); ref = receiver; break;
    case 3: parent = receiver->parentNode(); ref = receiver->nextSibling(); break;
    default: parent = receiver->parentNode(); ref = receiver; break;
    }

    if (!parent)
        return 1;
    if (ref)
        refHold = ref;          /* keep the insertion point alive while moving nodes */

    for (i = 0; i < argc; ++i) {
        RefPtr<WebCore::Node> hold;
        WebCore::Node* node = mixinArgNode(args[i], document, hold);

        if (!node || node == receiver || node == parent)
            continue;
        if (node->parentNode())
            node->parentNode()->removeChild(node, ec);   /* move, like Chrome */
        ec = 0;
        if (ref)
            parent->insertBefore(node, ref, ec);
        else
            parent->appendChild(node, ec);
        ec = 0;

        /* append()/prepend() on a ShadowRoot wrapper add component nodes, not
         * light DOM (issues #27). */
        if ((op == 0 || op == 1) && shadowHostOfWrapper(self)) {
            AgbShadowRoot* info = shadowInfo(shadowHostOfWrapper(self));

            if (info)
                markShadowSubtree(node, info->id);
        }
        shadowAfterInsert(self, parent, node);
    }

    if (op == 4 && receiver->parentNode()) {
        WebCore::Node* oldParent = receiver->parentNode();

        oldParent->removeChild(receiver, ec);
        shadowAfterRemove(self, oldParent, receiver);
    }

    return 1;
}

int nodeAppendChildren(AntbEngine*, AntbValue* self, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    (void)out;
    return mixinInsert(self, args, argc, 0);
}

int nodePrependChildren(AntbEngine*, AntbValue* self, AntbValue** args, int argc,
                        AntbValue* out, void*)
{
    (void)out;
    return mixinInsert(self, args, argc, 1);
}

int nodeBefore(AntbEngine*, AntbValue* self, AntbValue** args, int argc,
               AntbValue* out, void*)
{
    (void)out;
    return mixinInsert(self, args, argc, 2);
}

int nodeAfter(AntbEngine*, AntbValue* self, AntbValue** args, int argc,
              AntbValue* out, void*)
{
    (void)out;
    return mixinInsert(self, args, argc, 3);
}

int nodeReplaceWith(AntbEngine*, AntbValue* self, AntbValue** args, int argc,
                    AntbValue* out, void*)
{
    (void)out;
    return mixinInsert(self, args, argc, 4);
}

/* ---- Shadow DOM (issues #18 phase C) ------------------------------------ *
 * A ShadowRoot wrapper is bound to its host element: it carries both the
 * shadow-root tag (host/mode) and the node tag (everything else delegates to the
 * host through the normal node machinery), and its children ARE the host's
 * children, so component output renders through the normal layout path. */
static AgbShadowRoot* shadowInfo(WebCore::Node* host)
{
    DocumentRuntime* runtime;

    if (!host || !g_currentState || !host->document())
        return NULL;
    runtime = documentRuntimeFor(g_currentState, host->document());
    if (!runtime || !runtime->shadowRoots)
        return NULL;
    {
        std::map<WebCore::Node*, AgbShadowRoot*>::iterator it = runtime->shadowRoots->find(host);

        return it == runtime->shadowRoots->end() ? NULL : it->second;
    }
}

static int shadowRootMode(WebCore::Node* host)
{
    AgbShadowRoot* info = shadowInfo(host);

    return info ? info->mode : 0;
}

static void rememberShadowRoot(WebCore::Node* host, int mode)
{
    DocumentRuntime* runtime;

    if (!host || !host->document() || !g_currentState || shadowInfo(host))
        return;
    runtime = documentRuntimeFor(g_currentState, host->document());
    if (!runtime)
        return;
    if (!runtime->shadowRoots)
        runtime->shadowRoots = new std::map<WebCore::Node*, AgbShadowRoot*>();

    {
        AgbShadowRoot* info = new AgbShadowRoot();

        info->mode = mode;
        info->id = host->document()->registerShadowRoot(static_cast<WebCore::Element*>(host));
        info->light = new std::vector<WebCore::Node*>();

        /* Everything that is currently a child of the host is light DOM. */
        for (WebCore::Node* child = host->firstChild(); child; child = child->nextSibling()) {
            if (child->shadowScope() == host->shadowScope())
                info->light->push_back(child);
        }
        (*runtime->shadowRoots)[host] = info;
    }
}

/* One wrapper per host (the bridge cache keys on pointer + tag), so
 * `el.shadowRoot === el.shadowRoot` and `root.host === el`. */
static AntbValue* wrapShadowRoot(AntbEngine* engine, WebCore::Element* host)
{
    AntbValue* v;

    if (!engine || !host || !g_shadowRootIface)
        return NULL;

    v = antb_wrap_native_as_owner(engine, g_shadowRootIface, host, AGB_TAG_SHADOWROOT,
                                  host->document());
    if (!v)
        return NULL;

    /* The same wrapper also answers as the host node, which is what makes
     * appendChild/querySelector/innerHTML/childNodes work on a shadow root. */
    antb_set_native(v, host, AGB_TAG_NODE);
    return v;
}

/* ---- Shadow DOM: scope marking, slot distribution, slotchange (#27) ----- */

static bool isSlotElement(WebCore::Node* node)
{
    return node && node->isElementNode() &&
           static_cast<WebCore::Element*>(node)->localName() == WebCore::AtomicString("slot");
}

static WebCore::Element* shadowHostOfWrapper(AntbValue* self)
{
    return self ? static_cast<WebCore::Element*>(antb_get_native(self, AGB_TAG_SHADOWROOT)) : NULL;
}

/* The nearest ancestor (or the node itself) that hosts a shadow root. */
static WebCore::Element* nearestShadowHost(WebCore::Node* node, bool includeSelf = true)
{
    for (WebCore::Node* n = node; n; n = n->parentNode()) {
        if (n != node || includeSelf) {
            if (shadowInfo(n))
                return static_cast<WebCore::Element*>(n);
        }
    }
    return NULL;
}

static void markShadowSubtree(WebCore::Node* node, int scope)
{
    if (!node || scope <= 0)
        return;
    node->setShadowScope(scope);
    for (WebCore::Node* child = node->firstChild(); child; child = child->nextSibling())
        markShadowSubtree(child, scope);
}

/* A <style> created before the subtree was marked still carries scope 0; its
 * sheet has to be parsed again now that the scope is known. */
static void rebuildShadowStyleSheets(WebCore::Node* node)
{
    if (!node)
        return;
    if (node->isElementNode()) {
        WebCore::Element* element = static_cast<WebCore::Element*>(node);

        if (element->isHTMLElement() && element->localName() == WebCore::AtomicString("style"))
            static_cast<WebCore::HTMLStyleElement*>(element)->rebuildSheet();
    }
    for (WebCore::Node* child = node->firstChild(); child; child = child->nextSibling())
        rebuildShadowStyleSheets(child);
}

static void collectShadowSlots(WebCore::Node* parent, int scope,
                               std::vector<WebCore::Element*>& slots)
{
    for (WebCore::Node* child = parent->firstChild(); child; child = child->nextSibling()) {
        if (child->shadowScope() != scope)
            continue;
        if (!child->isElementNode())
            continue;

        {
            WebCore::Element* element = static_cast<WebCore::Element*>(child);

            if (element->localName() == WebCore::AtomicString("slot"))
                slots.push_back(element);
            collectShadowSlots(element, scope, slots);
        }
    }
}

static void fireSlotChange(WebCore::Element* slot)
{
    WebCore::ExceptionCode ec = 0;

    if (slot)
        slot->dispatchEvent(new WebCore::Event(WebCore::AtomicString("slotchange"), true, false), ec);
}

struct SlotChangeFlushJob {
    WebCore::Document* document;
};

static int slotChangeFlush(AntbEngine* engine, AntbValue*, AntbValue**, int,
                           AntbValue*, void* user)
{
    SlotChangeFlushJob* job = (SlotChangeFlushJob*)user;
    DocumentRuntime* runtime;

    (void)engine;
    if (!job)
        return 1;

    runtime = (job->document && g_currentState)
        ? documentRuntimeFor(g_currentState, job->document) : NULL;

    if (getenv("AGB_SHADOW_DEBUG"))
        agbLog(AGB_LOG_EVT, "[shadow] flush slotchange runtime=%p pending=%p",
               (void*)runtime, runtime ? (void*)runtime->pendingSlotChanges : NULL);

    if (runtime && runtime->pendingSlotChanges) {
        std::vector<WebCore::Node*>* slots = runtime->pendingSlotChanges;
        size_t i;

        runtime->pendingSlotChanges = NULL;
        runtime->slotChangeScheduled = 0;
        for (i = 0; i < slots->size(); ++i) {
            WebCore::Node* node = (*slots)[i];

            if (node && node->document() && node->inDocument() && node->isElementNode())
                fireSlotChange(static_cast<WebCore::Element*>(node));
            if (node)
                node->deref();
        }
        delete slots;
    } else if (runtime) {
        runtime->slotChangeScheduled = 0;
    }

    delete job;
    return 1;
}

/* One microtask per batch, like the spec's "signal a slot change". */
static void scheduleSlotChanges(DocumentRuntime* runtime)
{
    SlotChangeFlushJob* job;
    AntbValue* global;
    AntbValue* queue = NULL;
    AntbValue* holder = NULL;
    AntbValue* fn = NULL;

    if (!runtime || !runtime->pendingSlotChanges || runtime->pendingSlotChanges->empty())
        return;
    if (runtime->slotChangeScheduled)
        return;

    runtime->slotChangeScheduled = 1;
    job = new SlotChangeFlushJob();
    job->document = runtime->document;
    if (getenv("AGB_SHADOW_DEBUG"))
        agbLog(AGB_LOG_EVT, "[shadow] schedule slotchange doc=%p pending=%d",
               (void*)runtime->document, (int)runtime->pendingSlotChanges->size());

    global = g_engine ? antb_global(g_engine) : NULL;
    if (!global) {
        delete job;
        runtime->slotChangeScheduled = 0;
        if (runtime->pendingSlotChanges) {
            for (size_t i = 0; i < runtime->pendingSlotChanges->size(); ++i) {
                if ((*runtime->pendingSlotChanges)[i])
                    (*runtime->pendingSlotChanges)[i]->deref();
            }
            delete runtime->pendingSlotChanges;
            runtime->pendingSlotChanges = NULL;
        }
        return;
    }

    queue = antb_get_prop(g_engine, global, "queueMicrotask");
    holder = antb_make_object(g_engine);
    if (holder) {
        antb_define_method(g_engine, holder, "run", slotChangeFlush, job);
        fn = antb_get_prop(g_engine, holder, "run");
    }
    if (queue && fn)
        antb_call(g_engine, queue, global, &fn, 1, NULL);
    else {
        AntbValue* timer = antb_get_prop(g_engine, global, "setTimeout");

        if (timer && fn) {
            AntbValue* args[2];
            AntbValue* delay = antb_make_number(g_engine, 0.0);

            args[0] = fn;
            args[1] = delay;
            antb_call(g_engine, timer, global, args, 2, NULL);
            if (delay)
                antb_value_release(delay);
        } else {
            /* No scheduling facility: fire synchronously (better than losing
             * the notification). */
            slotChangeFlush(g_engine, NULL, NULL, 0, NULL, job);
            job = NULL;
        }
        if (timer)
            antb_value_release(timer);
    }

    if (fn)
        antb_value_release(fn);
    if (holder)
        antb_value_release(holder);
    if (queue)
        antb_value_release(queue);
    antb_value_release(global);
}

static void addLightNode(WebCore::Node* host, WebCore::Node* child)
{
    AgbShadowRoot* info = shadowInfo(host);

    if (!info || !child)
        return;
    for (size_t i = 0; i < info->light->size(); ++i)
        if ((*info->light)[i] == child)
            return;
    info->light->push_back(child);
}

static void removeLightNode(WebCore::Node* host, WebCore::Node* child)
{
    AgbShadowRoot* info = shadowInfo(host);

    if (!info || !child)
        return;
    for (size_t i = 0; i < info->light->size(); ++i) {
        if ((*info->light)[i] == child) {
            info->light->erase(info->light->begin() + i);
            return;
        }
    }
}

static void clearSlottedMarker(WebCore::Node* node, DocumentRuntime* runtime)
{
    if (!node)
        return;
    if (node->isElementNode()) {
        WebCore::ExceptionCode ec = 0;

        static_cast<WebCore::Element*>(node)->removeAttribute(
            WebCore::String("data-agb-slotted"), ec);
    }
    if (runtime && runtime->slotOf)
        runtime->slotOf->erase(node);
}

/* Recompute which light node goes to which slot, move the assigned nodes next
 * to their slot (the physical flat tree), and fire slotchange on the slots
 * whose assignment changed. Reentrancy: moving nodes fires the DOM mutation
 * hooks again, which is what distributingShadow guards. */
static void distributeShadowHost(WebCore::Element* host)
{
    AgbShadowRoot* info = shadowInfo(host);
    DocumentRuntime* runtime;
    std::vector<WebCore::Element*> slots;
    std::map<WebCore::Node*, std::vector<WebCore::Node*> > next;
    std::vector<WebCore::Node*> unassigned;
    std::vector<WebCore::Element*> changed;
    size_t i;

    if (!info || !host || !host->document() || !g_currentState)
        return;
    runtime = documentRuntimeFor(g_currentState, host->document());
    if (!runtime || runtime->distributingShadow)
        return;

    runtime->distributingShadow = 1;
    if (!runtime->slotAssignments)
        runtime->slotAssignments = new std::map<WebCore::Node*, std::vector<WebCore::Node*>*>();
    if (!runtime->slotOf)
        runtime->slotOf = new std::map<WebCore::Node*, WebCore::Node*>();

    collectShadowSlots(host, info->id, slots);

    for (i = 0; i < info->light->size(); ++i) {
        WebCore::Node* light = (*info->light)[i];
        WebCore::String slotName;
        WebCore::Element* target = NULL;
        size_t s;

        if (!light || light->document() != host->document())
            continue;
        if (light->isElementNode())
            slotName = static_cast<WebCore::Element*>(light)->getAttribute("slot");

        for (s = 0; s < slots.size(); ++s) {
            if (slots[s]->getAttribute("name") == slotName) {
                target = slots[s];
                break;
            }
        }

        if (target)
            next[target].push_back(light);
        else
            unassigned.push_back(light);
    }

    /* Markers/assignments of nodes that are no longer assigned. */
    for (i = 0; i < slots.size(); ++i) {
        std::map<WebCore::Node*, std::vector<WebCore::Node*>*>::iterator old =
            runtime->slotAssignments->find(slots[i]);
        std::vector<WebCore::Node*>& now = next[slots[i]];

        if (old != runtime->slotAssignments->end() && old->second) {
            size_t k;

            for (k = 0; k < old->second->size(); ++k) {
                WebCore::Node* node = (*old->second)[k];
                bool still = false;
                size_t j;

                for (j = 0; j < now.size(); ++j)
                    if (now[j] == node) { still = true; break; }
                if (!still)
                    clearSlottedMarker(node, runtime);
            }
        }
    }
    for (i = 0; i < unassigned.size(); ++i)
        clearSlottedMarker(unassigned[i], runtime);

    /* Place the assigned nodes immediately after their slot, in light order. */
    for (i = 0; i < slots.size(); ++i) {
        WebCore::Element* slot = slots[i];
        std::vector<WebCore::Node*>& nodes = next[slot];
        WebCore::Node* parent = slot->parentNode();
        WebCore::Node* anchor = slot;
        WebCore::ExceptionCode ec = 0;
        size_t k;

        if (nodes.empty()) {
            if (!slot->getAttribute(WebCore::String("data-agb-slot-filled")).isNull())
                slot->removeAttribute(WebCore::String("data-agb-slot-filled"), ec);
        } else {
            if (slot->getAttribute(WebCore::String("data-agb-slot-filled")).isNull())
                slot->setAttribute(WebCore::String("data-agb-slot-filled"), WebCore::String(""), ec);

            for (k = 0; k < nodes.size(); ++k) {
                WebCore::Node* node = nodes[k];

                if (parent && (node->parentNode() != parent || node->previousSibling() != anchor)) {
                    if (anchor->nextSibling() != node) {
                        RefPtr<WebCore::Node> keep(node);

                        parent->insertBefore(keep.release(), anchor->nextSibling(), ec);
                    }
                }
                anchor = node;
                if (node->isElementNode())
                    static_cast<WebCore::Element*>(node)->setAttribute(
                        WebCore::String("data-agb-slotted"),
                        WebCore::String::number(info->id), ec);
                (*runtime->slotOf)[node] = slot;
            }
        }

        {
            std::map<WebCore::Node*, std::vector<WebCore::Node*>*>::iterator old =
                runtime->slotAssignments->find(slot);
            bool differs = (old == runtime->slotAssignments->end() || !old->second ||
                            *old->second != nodes);

            if (differs)
                changed.push_back(slot);
            if (old != runtime->slotAssignments->end() && old->second)
                delete old->second;
            (*runtime->slotAssignments)[slot] = new std::vector<WebCore::Node*>(nodes);
        }
    }

    /* Unassigned light nodes do not render at all. Keep them in a fragment so
     * their owner document and listeners stay intact. */
    for (i = 0; i < unassigned.size(); ++i) {
        WebCore::Node* node = unassigned[i];

        if (!node->parentNode())
            continue;
        if (!runtime->unassignedLight) {
            PassRefPtr<WebCore::DocumentFragment> fragment =
                host->document()->createDocumentFragment();

            runtime->unassignedLight = fragment.releaseRef();
        }
        {
            WebCore::ExceptionCode ec = 0;
            RefPtr<WebCore::Node> keep(node);

            runtime->unassignedLight->appendChild(keep.release(), ec);
        }
    }

    runtime->distributingShadow = 0;

    if (getenv("AGB_SHADOW_DEBUG"))
        agbLog(AGB_LOG_EVT, "[shadow] distribute host=%p slots=%d light=%d changed=%d",
               (void*)host, (int)slots.size(), (int)info->light->size(), (int)changed.size());

    if (!changed.empty()) {
        if (!runtime->pendingSlotChanges)
            runtime->pendingSlotChanges = new std::vector<WebCore::Node*>();
        for (i = 0; i < changed.size(); ++i) {
            bool queued = false;
            size_t q;

            /* Chrome signals a slot once per microtask even if its assignment
             * changed twice; the handler reads the final assignedNodes(). */
            for (q = 0; q < runtime->pendingSlotChanges->size(); ++q) {
                if ((*runtime->pendingSlotChanges)[q] == changed[i]) {
                    queued = true;
                    break;
                }
            }
            if (queued)
                continue;
            changed[i]->ref();
            runtime->pendingSlotChanges->push_back(changed[i]);
        }
        if (!runtime->slotChangeScheduled)
            scheduleSlotChanges(runtime);
    }
}

/* Mutations through a shadow-root wrapper: the inserted nodes are the
 * component's own, so they get the root's scope before elementDidInsert. */
static void shadowAfterInsert(AntbValue* selfValue, WebCore::Node* parent, WebCore::Node* child)
{
    AgbShadowRoot* selfInfo;

    if (!g_currentDocument || !g_currentDocument->shadowRoots ||
        g_currentDocument->distributingShadow || !child)
        return;

    selfInfo = selfValue ? shadowInfo(shadowHostOfWrapper(selfValue)) : NULL;
    if (selfInfo) {
        markShadowSubtree(child, selfInfo->id);
        rebuildShadowStyleSheets(child);
        distributeShadowHost(shadowHostOfWrapper(selfValue));
        return;
    }

    if (parent) {
        int scope = parent->shadowScope();

        if (scope > 0) {
            markShadowSubtree(child, scope);
            rebuildShadowStyleSheets(child);
        }
        if (shadowInfo(parent))
            addLightNode(parent, child);
    }

    {
        WebCore::Element* host = nearestShadowHost(parent);

        if (host)
            distributeShadowHost(host);
    }
}

static void shadowAfterRemove(AntbValue* selfValue, WebCore::Node* parent, WebCore::Node* child)
{
    (void)selfValue;

    if (!g_currentDocument || !g_currentDocument->shadowRoots ||
        g_currentDocument->distributingShadow || !child)
        return;

    if (parent && shadowInfo(parent))
        removeLightNode(parent, child);

    {
        WebCore::Element* host = nearestShadowHost(parent);

        if (!host)
            host = nearestShadowHost(child);
        if (host)
            distributeShadowHost(host);
    }
}

/* Detach every light node from the shadow tree (it may sit next to a slot) and
 * hand it back to the host as a direct child. */
static void collectLightChildren(WebCore::Element* host)
{
    AgbShadowRoot* info = shadowInfo(host);
    WebCore::ExceptionCode ec = 0;
    size_t i;

    if (!info)
        return;
    for (i = 0; i < info->light->size(); ++i) {
        WebCore::Node* node = (*info->light)[i];

        if (node && node->parentNode() && node->parentNode() != host) {
            RefPtr<WebCore::Node> keep(node);

            host->appendChild(keep.release(), ec);
            ec = 0;
        }
    }
}

static void removeShadowContent(WebCore::Element* host)
{
    AgbShadowRoot* info = shadowInfo(host);
    WebCore::ExceptionCode ec = 0;
    WebCore::Node* child;

    if (!info || !g_currentDocument)
        return;
    g_currentDocument->distributingShadow = 1;
    collectLightChildren(host);
    child = host->firstChild();
    while (child) {
        WebCore::Node* next = child->nextSibling();

        if (child->shadowScope() == info->id) {
            RefPtr<WebCore::Node> keep(child);

            host->removeChild(keep.get(), ec);
            ec = 0;
        }
        child = next;
    }
    g_currentDocument->distributingShadow = 0;
}

/* shadowRoot.innerHTML = html: replace only the component's own nodes; the
 * host's light children survive and are re-distributed afterwards. */
static void setShadowInnerHTML(WebCore::Element* host, const WebCore::String& html)
{
    AgbShadowRoot* info = shadowInfo(host);
    WebCore::Document* document = host ? host->document() : NULL;
    WebCore::ExceptionCode ec = 0;
    RefPtr<WebCore::Element> scratch;

    if (!info || !document)
        return;

    removeShadowContent(host);

    scratch = document->createElement("div", ec);
    if (!scratch)
        return;
    static_cast<WebCore::HTMLElement*>(scratch.get())->setInnerHTML(html, ec);

    if (g_currentDocument)
        g_currentDocument->distributingShadow = 1;
    while (scratch->firstChild()) {
        RefPtr<WebCore::Node> child = scratch->firstChild();

        scratch->removeChild(child.get(), ec);
        ec = 0;
        markShadowSubtree(child.get(), info->id);
        host->appendChild(child.release(), ec);
        ec = 0;
    }
    if (g_currentDocument)
        g_currentDocument->distributingShadow = 0;

    distributeShadowHost(host);
}

/* shadowRoot.replaceChildren(...): replace the component's own tree, keep the
 * host's light children. */
static void replaceShadowChildren(WebCore::Element* host, AntbValue** args, int argc)
{
    AgbShadowRoot* info = shadowInfo(host);
    WebCore::Document* document = host ? host->document() : NULL;
    WebCore::ExceptionCode ec = 0;
    int i;

    if (!info || !document)
        return;

    removeShadowContent(host);
    if (g_currentDocument)
        g_currentDocument->distributingShadow = 1;
    for (i = 0; i < argc; ++i) {
        RefPtr<WebCore::Node> hold;
        WebCore::Node* node = mixinArgNode(args[i], document, hold);

        if (!node)
            continue;
        if (node->parentNode())
            node->parentNode()->removeChild(node, ec);
        ec = 0;
        markShadowSubtree(node, info->id);
        if (hold)
            host->appendChild(hold.release(), ec);
        else {
            RefPtr<WebCore::Node> keep(node);

            host->appendChild(keep.release(), ec);
        }
        ec = 0;
    }
    if (g_currentDocument)
        g_currentDocument->distributingShadow = 0;
    distributeShadowHost(host);
}


static void setLightInnerHTML(WebCore::Element* host, const WebCore::String& html)
{
    AgbShadowRoot* info = shadowInfo(host);
    WebCore::Document* document = host ? host->document() : NULL;
    WebCore::ExceptionCode ec = 0;
    RefPtr<WebCore::Element> scratch;
    size_t i;

    if (!info || !document)
        return;

    if (!g_currentDocument)
        return;
    g_currentDocument->distributingShadow = 1;
    for (i = 0; i < info->light->size(); ++i) {
        WebCore::Node* node = (*info->light)[i];

        if (node && node->parentNode()) {
            RefPtr<WebCore::Node> keep(node);

            node->parentNode()->removeChild(keep.get(), ec);
            ec = 0;
        }
    }
    info->light->clear();
    g_currentDocument->distributingShadow = 0;

    scratch = document->createElement("div", ec);
    if (!scratch)
        return;
    static_cast<WebCore::HTMLElement*>(scratch.get())->setInnerHTML(html, ec);

    if (g_currentDocument)
        g_currentDocument->distributingShadow = 1;
    while (scratch->firstChild()) {
        RefPtr<WebCore::Node> child = scratch->firstChild();

        scratch->removeChild(child.get(), ec);
        ec = 0;
        child->setShadowScope(host->shadowScope());
        host->appendChild(child.release(), ec);
        ec = 0;
        addLightNode(host, host->lastChild());
    }
    if (g_currentDocument)
        g_currentDocument->distributingShadow = 0;

    distributeShadowHost(host);
}


static int shadowRootGetter(AntbEngine* engine, AntbValue* self, const char* name,
                            AntbValue* out, void*)
{
    WebCore::Element* host =
        self ? static_cast<WebCore::Element*>(antb_get_native(self, AGB_TAG_SHADOWROOT)) : NULL;
    int mode;

    if (!host || !name)
        return 0;

    if (!strcmp(name, "host")) {
        AntbValue* v = wrapNode(engine, host);

        if (!v)
            return assignNull(engine, out);
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (!strcmp(name, "mode")) {
        AntbValue* v;

        mode = shadowRootMode(host);
        v = antb_make_string(engine, mode == 2 ? "closed" : "open");
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    return 0;
}

/* Elements that cannot host a shadow root (same list Chrome uses). */
static bool cannotHostShadowRoot(WebCore::Element* element)
{
    static const char* kForbidden[] = {
        "a", "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    std::string tag;
    size_t i;

    if (!element)
        return true;

    tag = element->tagName().utf8().data();
    for (i = 0; i < tag.size(); ++i)
        if (tag[i] >= 'A' && tag[i] <= 'Z')
            tag[i] = (char)(tag[i] - 'A' + 'a');

    for (i = 0; kForbidden[i]; ++i)
        if (tag == kForbidden[i])
            return true;
    return false;
}

/* ---- HTMLTemplateElement (issues #18 phase B) --------------------------- *
 * This tree has no template-contents mode in the parser and no
 * HTMLNames::templateTag, so a <template>'s children are ordinary children until
 * `content` is asked for; at that point they move into the fragment Chrome would
 * have parsed them into. The fragment is per document (templateContents). */
static bool isTemplateElement(WebCore::Element* element)
{
    WebCore::String tag;

    if (!element || !element->isHTMLElement())
        return false;

    tag = element->tagName();
    {
        const char* text = tag.utf8().data();
        static const char kTemplate[] = "template";
        size_t i;

        if (!text)
            return false;
        for (i = 0; kTemplate[i]; ++i) {
            char c = text[i];

            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != kTemplate[i])
                return false;
        }
        return text[i] == 0;
    }
}

static WebCore::DocumentFragment* templateContents(WebCore::Element* element, bool create)
{
    DocumentRuntime* runtime;
    std::map<WebCore::Node*, WebCore::DocumentFragment*>::iterator it;

    if (!element || !g_currentState || !element->document())
        return NULL;

    runtime = documentRuntimeFor(g_currentState, element->document());
    if (!runtime || !create)
        return NULL;

    if (!runtime->templateContents)
        runtime->templateContents =
            new std::map<WebCore::Node*, WebCore::DocumentFragment*>();

    it = runtime->templateContents->find(element);
    if (it != runtime->templateContents->end())
        return it->second;

    {
        RefPtr<WebCore::DocumentFragment> fragment = element->document()->createDocumentFragment();
        WebCore::ExceptionCode ec = 0;

        if (!fragment)
            return NULL;
        fragment->ref();                    /* owned by the map, deref'd on teardown */

        /* Whatever the parser left inside the element moves into the content. */
        while (element->firstChild()) {
            RefPtr<WebCore::Node> child = element->firstChild();

            element->removeChild(child.get(), ec);
            ec = 0;
            fragment->appendChild(child.release(), ec);
            ec = 0;
        }

        (*runtime->templateContents)[element] = fragment.get();
        return fragment.get();
    }
}

/* DocumentFragment has no innerHTML in this tree: serialize by moving the children
 * into a scratch element of the same document and reading that. */
static std::string templateInnerHTML(WebCore::Element* element)
{
    RefPtr<WebCore::DocumentFragment> fragment;
    RefPtr<WebCore::Element> scratch;
    WebCore::ExceptionCode ec = 0;
    std::string result;

    if (!element || !element->document())
        return result;

    fragment = templateContents(element, true);
    if (!fragment)
        return result;

    scratch = element->document()->createElement("div", ec);
    if (!scratch)
        return result;

    while (fragment->firstChild()) {
        RefPtr<WebCore::Node> child = fragment->firstChild();

        fragment->removeChild(child.get(), ec);
        ec = 0;
        scratch->appendChild(child.release(), ec);
        ec = 0;
    }

    if (scratch->isHTMLElement())
        result = static_cast<WebCore::HTMLElement*>(scratch.get())->innerHTML().utf8().data();

    while (scratch->firstChild()) {
        RefPtr<WebCore::Node> child = scratch->firstChild();

        scratch->removeChild(child.get(), ec);
        ec = 0;
        fragment->appendChild(child.release(), ec);
        ec = 0;
    }
    return result;
}

/* `template.innerHTML = html`: parse into a scratch element, then move the result
 * into the content fragment. */
static void setTemplateInnerHTML(WebCore::Element* element, const char* html)
{
    RefPtr<WebCore::DocumentFragment> fragment;
    RefPtr<WebCore::Element> scratch;
    WebCore::ExceptionCode ec = 0;

    if (!element || !element->document())
        return;

    fragment = templateContents(element, true);
    if (!fragment)
        return;

    scratch = element->document()->createElement("div", ec);
    if (!scratch)
        return;

    if (scratch->isHTMLElement())
        static_cast<WebCore::HTMLElement*>(scratch.get())->setInnerHTML(
            html ? WebCore::String::fromUTF8(html) : WebCore::String(), ec);

    while (scratch->firstChild()) {
        RefPtr<WebCore::Node> child = scratch->firstChild();

        scratch->removeChild(child.get(), ec);
        ec = 0;
        fragment->appendChild(child.release(), ec);
        ec = 0;
    }
}

/* Deep-copies the content of one template into another (cloneNode on a template
 * does not copy the external fragment by itself). */
static void copyTemplateContents(WebCore::Element* from, WebCore::Element* to)
{
    RefPtr<WebCore::DocumentFragment> source;
    RefPtr<WebCore::DocumentFragment> target;
    WebCore::ExceptionCode ec = 0;

    if (!isTemplateElement(from) || !isTemplateElement(to))
        return;

    source = templateContents(from, true);
    target = templateContents(to, true);
    if (!source || !target)
        return;

    source = static_cast<WebCore::DocumentFragment*>(source->cloneNode(true).get());
    if (!source)
        return;

    while (source->firstChild()) {
        RefPtr<WebCore::Node> child = source->firstChild();

        source->removeChild(child.get(), ec);
        ec = 0;
        target->appendChild(child.release(), ec);
        ec = 0;
    }
}

/* ---- custom elements (issues #18 phase A) ------------------------------- *
 * The registry is per document (per realm): tag -> constructor. An element is
 * upgraded by replacing its wrapper's [[Prototype]] with the registered
 * constructor's prototype (the native dispatch hook is untouched, so attributes
 * and methods keep working), then the lifecycle callbacks run. The constructor
 * itself is not executed: our HTMLElement constructor has no allocation path for
 * a subclass to call (documented limitation, needs an ant-side capability). */
static bool g_inCustomElementHook = false;

static void customElementDidInsert(void* elementPointer);

static DocumentRuntime* customElementRuntime(WebCore::Element* element)
{
    if (!element || !g_currentState)
        return NULL;
    return documentRuntimeFor(g_currentState, element->document());
}

static std::string customElementTag(WebCore::Element* element)
{
    WebCore::String tag;

    if (!element)
        return std::string();

    tag = element->tagName();
    {
        std::string text = tag.utf8().data();
        size_t i;

        for (i = 0; i < text.size(); ++i) {
            if (text[i] >= 'A' && text[i] <= 'Z')
                text[i] = (char)(text[i] - 'A' + 'a');
        }
        return text;
    }
}

static AntbValue* customElementCtor(WebCore::Element* element)
{
    DocumentRuntime* runtime = customElementRuntime(element);
    std::string tag;

    if (!runtime || !runtime->customDefs || runtime->customDefs->empty())
        return NULL;

    tag = customElementTag(element);
    {
        std::map<std::string, AntbValue*>::iterator it = runtime->customDefs->find(tag);

        return it == runtime->customDefs->end() ? NULL : it->second;
    }
}

static bool customElementWasUpgraded(WebCore::Element* element)
{
    DocumentRuntime* runtime = customElementRuntime(element);

    return runtime && runtime->upgradedCustom &&
           runtime->upgradedCustom->count(element) > 0;
}

static void customElementMarkUpgraded(WebCore::Element* element)
{
    DocumentRuntime* runtime = customElementRuntime(element);

    if (!runtime)
        return;
    if (!runtime->upgradedCustom)
        runtime->upgradedCustom = new std::set<WebCore::Node*>();
    runtime->upgradedCustom->insert(element);
}

/* Element that the in-progress custom element construction adopts: set by
 * document.createElement()/upgrade before calling the constructor, consumed by
 * customElementConstruct() (issues #18/#22). Construction is synchronous. */
static WebCore::Element* g_pendingCustomElement = NULL;

static char* customElementNameForCtor(AntbValue* ctor)
{
    DocumentRuntime* runtime;

    if (!ctor || !g_document || !g_currentState)
        return NULL;
    runtime = documentRuntimeFor(g_currentState, g_document);
    if (!runtime || !runtime->customDefs)
        return NULL;

    for (std::map<std::string, AntbValue*>::iterator it = runtime->customDefs->begin();
         it != runtime->customDefs->end(); ++it) {
        if (it->second && antb_value_same(it->second, ctor)) {
            char* copy = (char*)malloc(it->first.size() + 1);

            if (!copy)
                return NULL;
            memcpy(copy, it->first.c_str(), it->first.size() + 1);
            return copy;
        }
    }
    return NULL;
}

/* Bridge hook: new.target is the user class; super() lands in the interface
 * constructor and asks us for the instance. We build it from new.target.prototype
 * and adopt the element onto it. Returning 0 leaves the WebIDL error in place. */
static int customElementConstruct(AntbEngine* engine, AntbValue* newTarget,
                                  AntbValue** args, int argc, AntbValue* out)
{
    char* name;
    WebCore::Element* element;
    AntbValue* instance;
    AntbValue* proto;

    (void)args;
    (void)argc;

    if (!engine || !newTarget || !out)
        return 0;

    name = customElementNameForCtor(newTarget);
    if (!name)
        return 0;

    element = g_pendingCustomElement;
    if (!element && g_document) {
        /* `new X()` with no element yet: create one for the registered tag. The
         * PassRefPtr temporary must be held, and the node pinned the same way
         * wrapNode() pins (it is kept alive for the realm's lifetime). */
        WebCore::ExceptionCode ec = 0;
        RefPtr<WebCore::Element> created =
            g_document->createElement(WebCore::String::fromUTF8(name), ec);

        element = created.get();
        if (element) {
            AntbValue* pinned = wrapNode(engine, element);

            if (pinned)
                antb_value_release(pinned);
        }
    }
    free(name);

    if (!element)
        return 0;

    instance = antb_make_object(engine);
    if (!instance)
        return 0;

    proto = antb_get_prop(engine, newTarget, "prototype");
    if (proto) {
        antb_object_set_proto(instance, proto);
        antb_value_release(proto);
    }

    if (!antb_object_adopt_native(engine, instance, classForNode(element), element,
                                  AGB_TAG_NODE, element->document())) {
        antb_value_release(instance);
        return 0;
    }

    antb_value_assign(out, instance);
    antb_value_release(instance);
    return 1;
}

/* Publish the hook for the per-document registration in bindDocument(). */
static const bool s_constructHookPublished = (g_constructHookFn = customElementConstruct, true);


/* Calls `hook` on the definition's prototype with the element's wrapper as
 * `this`. Returns false when the hook does not exist. */
static bool customElementCall(AntbValue* ctor, const char* hook, AntbValue* self,
                             AntbValue** args, int argc)
{
    AntbValue* proto;
    AntbValue* fn;
    bool called = false;

    if (!ctor || !self || !g_engine)
        return false;

    proto = antb_get_prop(g_engine, ctor, "prototype");
    if (!proto)
        return false;
    fn = antb_get_prop(g_engine, proto, hook);
    if (fn && antb_is_function(fn)) {
        AntbValue* result = antb_call(g_engine, fn, self, args, argc, NULL);

        if (result)
            antb_value_release(result);
        called = true;
    }
    if (fn)
        antb_value_release(fn);
    antb_value_release(proto);
    return called;
}

static bool customElementObserves(AntbValue* ctor, const char* name)
{
    AntbValue* list;
    AntbValue* lengthValue;
    int length, i;
    bool observed = false;

    if (!ctor || !name)
        return false;

    list = antb_get_prop(g_engine, ctor, "observedAttributes");
    if (!list)
        return false;
    lengthValue = antb_get_prop(g_engine, list, "length");
    length = lengthValue ? (int)antb_to_number(lengthValue) : 0;
    if (lengthValue)
        antb_value_release(lengthValue);

    for (i = 0; i < length; ++i) {
        AntbValue* item = antb_array_get(list, i);

        if (!item)
            continue;
        {
            char* text = antb_to_utf8(item);

            if (text) {
                if (!strcmp(text, name))
                    observed = true;
                antb_free(text);
            }
        }
        antb_value_release(item);
        if (observed)
            break;
    }
    antb_value_release(list);
    return observed;
}

/* Replaces the wrapper's prototype with the definition's, so instanceof and the
 * class' methods resolve. Safe to call twice. */
static bool customElementApplyPrototype(AntbValue* ctor, AntbValue* wrapper)
{
    AntbValue* proto;
    bool ok = false;

    if (!ctor || !wrapper || !g_engine)
        return false;

    proto = antb_get_prop(g_engine, ctor, "prototype");
    if (proto) {
        antb_object_set_proto(wrapper, proto);
        antb_value_release(proto);
        ok = true;
    }
    return ok;
}

/* Upgraded on demand: `wrapper` may be NULL (hook path), in which case the cached
 * wrapper is asked for. Returns a retained wrapper or NULL. */
static AntbValue* upgradeCustomElement(WebCore::Element* element, AntbValue* ctor,
                                       AntbValue* wrapper)
{
    AntbValue* owned = NULL;

    if (!element || !ctor || !g_engine)
        return NULL;

    if (!wrapper) {
        owned = wrapNode(g_engine, element);
        wrapper = owned;
    }
    if (!wrapper)
        return NULL;

    customElementApplyPrototype(ctor, wrapper);
    customElementMarkUpgraded(element);

    return owned ? owned : antb_retain(wrapper);
}

static void customElementAttributeChanged(WebCore::Element* element, const char* name,
                                          const char* value)
{
    AntbValue* ctor = customElementCtor(element);
    AntbValue* wrapper;
    AntbValue* args[3];
    AntbValue* nullOld;

    if (!ctor || !name || !customElementWasUpgraded(element) ||
        !customElementObserves(ctor, name))
        return;

    wrapper = wrapNode(g_engine, element);
    if (!wrapper)
        return;

    nullOld = antb_make_null(g_engine);
    args[0] = antb_make_string(g_engine, name);
    args[1] = nullOld;                       /* the previous value is not tracked */
    args[2] = antb_make_string(g_engine, value ? value : "");
    customElementCall(ctor, "attributeChangedCallback", wrapper, args, 3);
    for (int i = 0; i < 3; ++i)
        if (args[i])
            antb_value_release(args[i]);
    antb_value_release(wrapper);
}

int globalCEHas(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue* out, void*)
{
    AntbValue* result;
    char* name;
    bool found = false;

    if (argc < 1 || !args[0] || !g_document)
        return 1;
    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    {
        DocumentRuntime* runtime = documentRuntimeFor(g_currentState, g_document);

        if (runtime && runtime->customDefs) {
            std::string tag = name;

            for (size_t i = 0; i < tag.size(); ++i)
                if (tag[i] >= 'A' && tag[i] <= 'Z')
                    tag[i] = (char)(tag[i] - 'A' + 'a');
            found = runtime->customDefs->count(tag) > 0;
        }
    }
    antb_free(name);

    result = antb_make_boolean(g_engine, found ? 1 : 0);
    if (!result)
        return 0;
    antb_value_assign(out, result);
    antb_value_release(result);
    return 1;
}

int globalCEGet(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue* out, void*)
{
    char* name;

    if (argc < 1 || !args[0] || !g_document)
        return 1;
    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    {
        DocumentRuntime* runtime = documentRuntimeFor(g_currentState, g_document);

        if (runtime && runtime->customDefs) {
            std::string tag = name;
            std::map<std::string, AntbValue*>::iterator it;

            for (size_t i = 0; i < tag.size(); ++i)
                if (tag[i] >= 'A' && tag[i] <= 'Z')
                    tag[i] = (char)(tag[i] - 'A' + 'a');
            it = runtime->customDefs->find(tag);
            if (it != runtime->customDefs->end()) {
                antb_value_assign(out, it->second);
                antb_free(name);
                return 1;
            }
        }
    }
    antb_free(name);
    return 0;
}

int globalCEGetName(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue* out, void*)
{
    if (argc < 1 || !args[0] || !g_document)
        return 1;

    {
        DocumentRuntime* runtime = documentRuntimeFor(g_currentState, g_document);

        if (runtime && runtime->customDefs) {
            for (std::map<std::string, AntbValue*>::iterator it = runtime->customDefs->begin();
                 it != runtime->customDefs->end(); ++it) {
                if (antb_value_same(it->second, args[0])) {
                    AntbValue* name = antb_make_string(g_engine, it->first.c_str());

                    if (!name)
                        return 0;
                    antb_value_assign(out, name);
                    antb_value_release(name);
                    return 1;
                }
            }
        }
    }
    return 0;
}

int globalCEDefine(AntbEngine*, AntbValue*, AntbValue** args, int argc, AntbValue* out, void*)
{
    char* name;
    AntbValue* result;
    DocumentRuntime* runtime;
    static int g_ceDefineCount = 0;

    if (argc < 2 || !args[0] || !args[1] || !g_document || !antb_is_function(args[1]))
        return 0;
    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    if (getenv("AGB_CE_DEBUG"))
        fprintf(stderr, "[cedef] #%d %s doc=%p\n", ++g_ceDefineCount, name, (void*)g_document);

    runtime = documentRuntimeFor(g_currentState, g_document);
    if (!runtime) {
        antb_free(name);
        return 0;
    }
    if (!runtime->customDefs)
        runtime->customDefs = new std::map<std::string, AntbValue*>();

    {
        std::string tag = name;

        for (size_t i = 0; i < tag.size(); ++i)
            if (tag[i] >= 'A' && tag[i] <= 'Z')
                tag[i] = (char)(tag[i] - 'A' + 'a');
        std::map<std::string, AntbValue*>::iterator existing = runtime->customDefs->find(tag);

        if (existing != runtime->customDefs->end()) {
            /* Chrome throws NotSupportedError for a re-definition, but this engine
             * can execute a webpack chunk factory twice (issues #23: GitHub's second
             * webpack runtime requires the chunk again and the factory makes a fresh
             * class each run), and the throw aborts the rest of the page's setup.
             * Keep the first registration and treat the re-definition as a no-op;
             * AGB_CE_DEBUG logs it with the JS stack so the deviation stays visible. */
            if (getenv("AGB_CE_DEBUG")) {
                fprintf(stderr, "[cedef] re-define %s ignored (first ctor kept%s)\n", name,
                        (existing->second && antb_value_same(existing->second, args[1]))
                            ? ", same ctor" : ", different ctor");
                if (getenv("AGB_CE_DEBUG_STACK") && g_engine)
                    antb_print_js_stack(g_engine);
            }
            antb_free(name);
            result = antb_make_boolean(g_engine, 1);
            if (result) {
                antb_value_assign(out, result);
                antb_value_release(result);
            }
            return 1;
        }
        (*runtime->customDefs)[tag] = antb_retain(args[1]);
    }
    antb_free(name);

    /* Upgrade what is already in the document, in document order. */
    if (g_document) {
        char* tag = antb_to_utf8(args[0]);

        if (tag) {
            RefPtr<WebCore::NodeList> existing =
                g_document->getElementsByTagName(WebCore::String::fromUTF8(tag));

            if (existing) {
                for (unsigned i = 0; i < existing->length(); ++i) {
                    WebCore::Node* node = existing->item(i);

                    if (node && node->isElementNode())
                        customElementDidInsert(node);
                }
            }
            antb_free(tag);
        }
    }

    result = antb_make_boolean(g_engine, 1);
    if (result) {
        antb_value_assign(out, result);
        antb_value_release(result);
    }
    return 1;
}

int globalCEUpgrade(AntbEngine* engine, AntbValue*, AntbValue** args, int argc, AntbValue* out, void*)
{
    AntbValue* result;

    (void)engine;

    if (argc > 0 && args[0]) {
        WebCore::Node* root = nodeOf(args[0]);

        if (root) {
            std::vector<WebCore::Element*> elements;
            size_t i;

            elements.push_back(root->isElementNode() ? static_cast<WebCore::Element*>(root) : NULL);
            for (WebCore::Node* n = root->firstChild(); n; n = n->traverseNextNode(root))
                if (n->isElementNode())
                    elements.push_back(static_cast<WebCore::Element*>(n));

            for (i = 0; i < elements.size(); ++i) {
                if (!elements[i] || !customElementCtor(elements[i]))
                    continue;
                customElementDidInsert(elements[i]);
            }
        }
    }

    result = antb_make_boolean(g_engine, 1);
    if (result) {
        antb_value_assign(out, result);
        antb_value_release(result);
    }
    return 1;
}

/* Called by document.createElement() and by the WebCore hooks below. */
static void upgradeCustomElementIfDefined(WebCore::Element* element, AntbValue* wrapper)
{
    AntbValue* ctor = customElementCtor(element);

    if (!ctor || !wrapper)
        return;
    if (customElementWasUpgraded(element))
        return;

    {
        AntbValue* owned = upgradeCustomElement(element, ctor, wrapper);

        if (owned)
            antb_value_release(owned);
    }
}

static void customElementDidInsert(void* elementPointer)
{
    WebCore::Element* element = static_cast<WebCore::Element*>(elementPointer);
    AntbValue* ctor;
    AntbValue* owned;

    if (!element || !g_engine || g_inCustomElementHook)
        return;
    ctor = customElementCtor(element);
    if (!ctor)
        return;

    g_inCustomElementHook = true;

    /* An element that existed before define() (parser-inserted, or created while
     * the tag was still unknown) gets its constructor run here: it adopts the
     * element onto the class instance, which is what makes class fields such as
     * `this.delegate = ...` exist (issues #18/#22). */
    if (!customElementWasUpgraded(element)) {
        g_pendingCustomElement = element;
        owned = antb_construct(g_engine, ctor, NULL, 0);
        g_pendingCustomElement = NULL;
    }
    if (!owned)
        owned = upgradeCustomElement(element, ctor, NULL);

    if (owned) {
        static const char* kHooks[] = { "connectedCallback" };

        customElementMarkUpgraded(element);
        customElementCall(ctor, kHooks[0], owned, NULL, 0);
        antb_value_release(owned);
    }
    g_inCustomElementHook = false;
}

static void customElementDidRemove(void* elementPointer)
{
    WebCore::Element* element = static_cast<WebCore::Element*>(elementPointer);
    AntbValue* ctor;
    AntbValue* wrapper;

    if (!element || !g_engine || g_inCustomElementHook)
        return;
    ctor = customElementCtor(element);
    if (!ctor || !customElementWasUpgraded(element))
        return;

    g_inCustomElementHook = true;
    wrapper = wrapNode(g_engine, element);
    if (wrapper) {
        customElementCall(ctor, "disconnectedCallback", wrapper, NULL, 0);
        antb_value_release(wrapper);
    }
    g_inCustomElementHook = false;
}

static void customElementAttributeHook(void* elementPointer, const char* name)
{
    WebCore::Element* element = static_cast<WebCore::Element*>(elementPointer);

    if (!element || !g_engine || g_inCustomElementHook || !name)
        return;
    if (!customElementCtor(element))
        return;

    {
        const WebCore::String& value = element->getAttribute(name);

        g_inCustomElementHook = true;
        customElementAttributeChanged(element, name, value.utf8().data());
        g_inCustomElementHook = false;
    }
}

/* __agbAttachShadow(el, mode): records the root and hands back its wrapper. The
 * spec checks (invalid mode, already attached, elements that cannot host one) run
 * in prelude.js, which can throw the right error types (issues #18 phase C). */
int globalAttachShadow(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                       AntbValue* out, void*)
{
    WebCore::Node* n = (argc > 0 && args[0]) ? nodeOf(args[0]) : NULL;
    AntbValue* root;
    int mode = 1;

    if (!n || !n->isElementNode())
        return 0;

    if (argc > 1 && args[1]) {
        char* text = antb_to_utf8(args[1]);

        if (text) {
            if (!strcmp(text, "closed"))
                mode = 2;
            antb_free(text);
        }
    }

    if (shadowRootMode(n))
        return 0;                          /* already attached: prelude throws */

    rememberShadowRoot(n, mode);
    root = wrapShadowRoot(engine, static_cast<WebCore::Element*>(n));
    if (!root)
        return 0;

    /* No slots yet: the host's existing children become unassigned light DOM
     * (they render again as soon as the shadow tree has slots for them). */
    distributeShadowHost(static_cast<WebCore::Element*>(n));

    antb_value_assign(out, root);
    antb_value_release(root);
    return 1;
}
/* slot.assignedNodes({flatten}) / assignedElements(): the light nodes that
 * distribution placed next to this slot (issues #27). */
int nodeAssignedNodes(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    WebCore::Node* slot = nodeOf(self);
    AntbValue* list = antb_make_array(engine);
    std::vector<WebCore::Node*>* assigned = NULL;

    (void)argc;
    (void)args;

    if (!list)
        return 0;

    if (slot && g_currentDocument && g_currentDocument->slotAssignments) {
        std::map<WebCore::Node*, std::vector<WebCore::Node*>*>::iterator it =
            g_currentDocument->slotAssignments->find(slot);

        if (it != g_currentDocument->slotAssignments->end())
            assigned = it->second;
    }

    if (assigned) {
        for (size_t i = 0; i < assigned->size(); ++i) {
            AntbValue* wrapped = wrapNode(engine, (*assigned)[i]);

            if (!wrapped)
                continue;
            antb_array_push(engine, list, wrapped);
            antb_value_release(wrapped);
        }
    }

    antb_value_assign(out, list);
    antb_value_release(list);
    return 1;
}

int nodeAssignedElements(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                         AntbValue* out, void*)
{
    WebCore::Node* slot = nodeOf(self);
    AntbValue* list = antb_make_array(engine);
    std::vector<WebCore::Node*>* assigned = NULL;

    (void)argc;
    (void)args;

    if (!list)
        return 0;

    if (slot && g_currentDocument && g_currentDocument->slotAssignments) {
        std::map<WebCore::Node*, std::vector<WebCore::Node*>*>::iterator it =
            g_currentDocument->slotAssignments->find(slot);

        if (it != g_currentDocument->slotAssignments->end())
            assigned = it->second;
    }

    if (assigned) {
        for (size_t i = 0; i < assigned->size(); ++i) {
            AntbValue* wrapped;

            if (!(*assigned)[i]->isElementNode())
                continue;
            wrapped = wrapNode(engine, (*assigned)[i]);
            if (!wrapped)
                continue;
            antb_array_push(engine, list, wrapped);
            antb_value_release(wrapped);
        }
    }

    antb_value_assign(out, list);
    antb_value_release(list);
    return 1;
}

/* ---- TreeWalker (DOM2 Traversal, issues #27) ---------------------------- *
 * WebCore 2008 already ships the traversal engine (Document::createTreeWalker,
 * TreeWalker, NodeFilter). Only the ant binding was missing, so library code
 * (highlighters, relative-time, Markdown renderers) recursed over childNodes by
 * hand or took its "no TreeWalker" branch. The filter is a host object: either
 * a function or { acceptNode }, called back through the bridge on every node.
 *
 * Lifetime: the walker is ref'd by the document record and released with it;
 * the filter handle is retained (like an event listener), because ant's GC has
 * no finalizers to tell us when the JS walker is gone. */
struct AgbTreeWalker {
    RefPtr<WebCore::TreeWalker> walker;
    AntbValue* filter;
};

static void destroyTreeWalkers(DocumentRuntime* document)
{
    if (!document || !document->treeWalkers)
        return;

    for (size_t i = 0; i < document->treeWalkers->size(); ++i)
        delete (*document->treeWalkers)[i];
    delete document->treeWalkers;
    document->treeWalkers = NULL;
}

class AntNodeFilterCondition : public WebCore::NodeFilterCondition {
public:
    AntNodeFilterCondition(AntbValue* filter) : m_filter(filter) { }
    virtual ~AntNodeFilterCondition() { }

    virtual short acceptNode(WebCore::Node* node) const
    {
        AntbEngine* engine = g_engine;
        AntbValue* fn = NULL;
        AntbValue* argument;
        AntbValue* result;
        char* error = NULL;
        short verdict;

        if (!engine || !m_filter || !node)
            return WebCore::NodeFilter::FILTER_REJECT;

        fn = antb_is_function(m_filter) ? m_filter : antb_get_prop(engine, m_filter, "acceptNode");
        if (!fn)
            return WebCore::NodeFilter::FILTER_REJECT;

        argument = wrapNode(engine, node);
        if (!argument) {
            if (fn != m_filter)
                antb_value_release(fn);
            return WebCore::NodeFilter::FILTER_REJECT;
        }

        result = antb_call(engine, fn, m_filter, &argument, 1, &error);
        antb_value_release(argument);
        if (fn != m_filter)
            antb_value_release(fn);

        if (error) {
            antb_free(error);
            if (result)
                antb_value_release(result);
            return WebCore::NodeFilter::FILTER_REJECT;
        }

        verdict = (short)antb_to_number(result);
        if (result)
            antb_value_release(result);
        return verdict;
    }

    virtual void mark() { }

private:
    AntbValue* m_filter;
};

static AgbTreeWalker* treeWalkerRecord(AntbValue* self)
{
    return self ? (AgbTreeWalker*)antb_get_native(self, AGB_TAG_TREEWALKER) : NULL;
}

static WebCore::TreeWalker* treeWalkerOf(AntbValue* self)
{
    AgbTreeWalker* record = treeWalkerRecord(self);

    return record ? record->walker.get() : NULL;
}

static AntbValue* wrapTreeWalker(AntbEngine* engine, AgbTreeWalker* record)
{
    if (!engine || !record || !g_treeWalkerIface)
        return NULL;
    return antb_wrap_native_as_owner(engine, g_treeWalkerIface, record,
                                     AGB_TAG_TREEWALKER, g_document);
}

/* document.createTreeWalker(root, whatToShow, filter) */
int docCreateTreeWalker(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                        AntbValue* out, void*)
{
    WebCore::Document* document = documentOf(self);
    WebCore::Node* root = (argc > 0 && args[0]) ? nodeOf(args[0]) : NULL;
    unsigned whatToShow = WebCore::NodeFilter::SHOW_ALL;
    RefPtr<WebCore::NodeFilter> filter;
    WebCore::ExceptionCode ec = 0;
    AgbTreeWalker* record;
    AntbValue* wrapper;

    if (!document || !root)
        return 0;

    if (argc > 1 && args[1] && antb_is_number(args[1]))
        whatToShow = (unsigned)antb_to_number(args[1]);

    if (argc > 2 && args[2]) {
        AntbValue* accept = antb_is_function(args[2])
            ? args[2] : antb_get_prop(engine, args[2], "acceptNode");

        if (accept) {
            filter = new WebCore::NodeFilter(
                new AntNodeFilterCondition(antb_retain(args[2])));
            if (accept != args[2])
                antb_value_release(accept);
        }
    }

    {
        PassRefPtr<WebCore::TreeWalker> created =
            document->createTreeWalker(root, whatToShow, filter.get(), false, ec);

        if (!created)
            return 0;

        record = new AgbTreeWalker();
        record->walker = created;
        record->filter = (argc > 2 && args[2]) ? antb_retain(args[2]) : NULL;
        if (!g_currentDocument->treeWalkers)
            g_currentDocument->treeWalkers = new std::vector<AgbTreeWalker*>();
        g_currentDocument->treeWalkers->push_back(record);
    }

    wrapper = wrapTreeWalker(engine, record);
    if (!wrapper)
        return 0;
    antb_value_assign(out, wrapper);
    antb_value_release(wrapper);
    return 1;
}

enum {
    TW_PARENT_NODE, TW_FIRST_CHILD, TW_LAST_CHILD, TW_PREVIOUS_SIBLING,
    TW_NEXT_SIBLING, TW_PREVIOUS_NODE, TW_NEXT_NODE
};

int treeWalkerMethod(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                     AntbValue* out, void* user)
{
    WebCore::TreeWalker* walker = treeWalkerOf(self);
    WebCore::Node* result = NULL;

    (void)args;
    (void)argc;

    if (!walker)
        return 0;

    switch ((intptr_t)user) {
        case TW_PARENT_NODE:     result = walker->parentNode(); break;
        case TW_FIRST_CHILD:     result = walker->firstChild(); break;
        case TW_LAST_CHILD:      result = walker->lastChild(); break;
        case TW_PREVIOUS_SIBLING: result = walker->previousSibling(); break;
        case TW_NEXT_SIBLING:    result = walker->nextSibling(); break;
        case TW_PREVIOUS_NODE:   result = walker->previousNode(); break;
        case TW_NEXT_NODE:       result = walker->nextNode(); break;
        default:                 return 0;
    }

    if (!result)
        return assignNull(engine, out);
    {
        AntbValue* wrapper = wrapNode(engine, result);

        if (!wrapper)
            return assignNull(engine, out);
        antb_value_assign(out, wrapper);
        antb_value_release(wrapper);
    }
    return 1;
}

int treeWalkerGetter(AntbEngine* engine, AntbValue* self, const char* name,
                     AntbValue* out, void*)
{
    AgbTreeWalker* record = treeWalkerRecord(self);
    WebCore::TreeWalker* walker = record ? record->walker.get() : NULL;

    if (!walker || !name)
        return 0;

    if (!strcmp(name, "root")) {
        AntbValue* value = wrapNode(engine, walker->root());

        if (!value)
            return assignNull(engine, out);
        antb_value_assign(out, value);
        antb_value_release(value);
        return 1;
    }
    if (!strcmp(name, "whatToShow")) {
        AntbValue* value = antb_make_number(engine, (double)walker->whatToShow());

        if (!value)
            return 0;
        antb_value_assign(out, value);
        antb_value_release(value);
        return 1;
    }
    if (!strcmp(name, "filter")) {
        if (!record->filter)
            return assignNull(engine, out);
        antb_value_assign(out, record->filter);
        return 1;
    }
    if (!strcmp(name, "expandEntityReferences")) {
        AntbValue* value = antb_make_boolean(engine, 0);

        if (!value)
            return 0;
        antb_value_assign(out, value);
        antb_value_release(value);
        return 1;
    }
    if (!strcmp(name, "currentNode")) {
        AntbValue* value = wrapNode(engine, walker->currentNode());

        if (!value)
            return assignNull(engine, out);
        antb_value_assign(out, value);
        antb_value_release(value);
        return 1;
    }
    return 0;
}

int treeWalkerSetter(AntbEngine* engine, AntbValue* self, const char* name,
                     const AntbValue* value, void*)
{
    WebCore::TreeWalker* walker = treeWalkerOf(self);
    WebCore::Node* node;

    (void)engine;

    if (!walker || !name || strcmp(name, "currentNode"))
        return 1;

    node = value ? nodeOf(const_cast<AntbValue*>(value)) : NULL;
    if (!node)
        return 0;                    /* Chrome throws TypeError for null/non-node */
    {
        WebCore::ExceptionCode ec = 0;

        walker->setCurrentNode(node, ec);
    }
    return 1;
}

/* ---- Fullscreen (issues #27) -------------------------------------------- *
 * The shell has no borderless-window hook, so requestFullscreen() implements
 * the document-visible contract Chrome exposes to pages: state
 * (document.fullscreenElement), a Promise (players .catch() it), and
 * fullscreenchange on the document. The window itself is unchanged; a host
 * that wants real fullscreen can listen for the event (roadmap: fullscreen
 * window API). */
static void fireFullscreenChange(WebCore::Document* document)
{
    WebCore::ExceptionCode ec = 0;

    if (!document)
        return;
    document->dispatchEvent(new WebCore::Event(WebCore::AtomicString("fullscreenchange"),
                                               false, false), ec);
}

/* A pending fullscreen transition. Chrome applies requestFullscreen()/
 * exitFullscreen() in a task and fires fullscreenchange from there, so a page
 * that attaches its listener after the call still sees the event. The job is
 * the user data of a setTimeout(0) callback; it refs the element for the wait
 * and retains the promise settle functions. */
int elementRequestFullscreen(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                             AntbValue* out, void*)
{
    WebCore::Node* node = (argc > 0 && args[0]) ? nodeOf(args[0]) : NULL;
    WebCore::Element* element = (node && node->isElementNode())
        ? static_cast<WebCore::Element*>(node) : NULL;
    WebCore::Document* document = element ? element->document() : NULL;
    AntbValue* value;

    if (!document || !element->inDocument())
        return assignBoolean(engine, out, 0);

    if (document->fullscreenElement() != element) {
        document->setFullscreenElement(element);
        fireFullscreenChange(document);
    }
    value = antb_make_boolean(engine, 1);
    if (!value)
        return 0;
    antb_value_assign(out, value);
    antb_value_release(value);
    return 1;
}

int docExitFullscreen(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    WebCore::Document* document = (argc > 0 && args[0]) ? documentOf(args[0]) : NULL;

    if (!document)
        return assignBoolean(engine, out, 0);

    if (document->fullscreenElement()) {
        document->setFullscreenElement(NULL);
        fireFullscreenChange(document);
    }
    return assignBoolean(engine, out, 1);
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

/* --- DOM collections (B2-2) ----------------------------------------------
 * NodeList/HTMLCollection are interface classes; a wrapper carries an
 * AgbCollection descriptor that records what the collection is (direct
 * children, tag/class/selector query, a document collection, ...). There is no
 * JS finalizer, so descriptors are registered on the document record and
 * dropped with it, like the CSSOM refs. Live kinds read the DOM on every
 * access (WebCore's live lists are created on demand and cache themselves, so
 * childNodes/children/getElementsBy* see mutations); the static kind keeps the
 * querySelectorAll snapshot's wrapped (pinned) nodes, so it stays static the
 * way a browser's NodeList is.
 */
struct AgbCollection {
    WebCore::Document* document;
    WebCore::Node*     root;
    int                kind;
    std::string        name;
    std::vector<AntbValue*> items;   /* AgbCollectionStatic: retained wrappers */
    /* getElementsByClassName used to walk the whole subtree on every length /
     * index read, so a loop over the collection was O(n^2). The walk result is
     * cached and rebuilt only when the document's DOM tree version changes
     * (`Document::domTreeVersion()` is bumped by every insert/remove), so the
     * cached pointers are only ever dereferenced while the tree is unchanged. */
    std::vector<WebCore::Node*>* nodes;
    unsigned           nodesVersion;
    bool               nodesValid;
    AgbCollection() : document(NULL), root(NULL), kind(0), nodes(NULL), nodesVersion(0), nodesValid(false) {}
};

static bool isElementClassToken(WebCore::Element* element, const std::string& wanted)
{
    if (!element || wanted.empty())
        return false;

    std::string classes = element->getAttribute(WebCore::String("class")).domString().utf8().data();
    size_t pos = 0;

    while (pos < classes.size()) {
        size_t start;

        while (pos < classes.size() && (classes[pos] == ' ' || classes[pos] == '\t'
                || classes[pos] == '\n' || classes[pos] == '\r' || classes[pos] == '\f'))
            ++pos;

        start = pos;

        while (pos < classes.size() && !(classes[pos] == ' ' || classes[pos] == '\t'
                || classes[pos] == '\n' || classes[pos] == '\r' || classes[pos] == '\f'))
            ++pos;

        if (pos > start && (pos - start) == wanted.size()
                && classes.compare(start, wanted.size(), wanted) == 0)
            return true;
    }

    return false;
}

static PassRefPtr<WebCore::HTMLCollection> documentListCollection(AgbCollection* c)
{
    WebCore::Document* document = c ? c->document : NULL;

    if (!document)
        return NULL;
    if (c->name == "forms")
        return document->forms();
    if (c->name == "images")
        return document->images();
    if (c->name == "links")
        return document->links();
    if (c->name == "anchors")
        return document->anchors();
    if (c->name == "scripts")
        return document->scripts();
    if (c->name == "embeds" || c->name == "plugins")
        return document->embeds();
    if (c->name == "applets")
        return document->applets();
    if (c->name == "objects")
        return document->objects();
    return NULL;
}

static bool collectionIsStatic(AgbCollection* c)
{
    return c && c->kind == AgbCollectionStatic;
}

/* Elements of `c` whose class list contains c->name, cached per DOM version.
 * Only valid while c->nodesValid && nodesVersion == document->domTreeVersion():
 * a mutation bumps the version, so a removed node can never be handed out from
 * the cache (the next read rebuilds). */
static const std::vector<WebCore::Node*>& classCollectionNodes(AgbCollection* c)
{
    static const std::vector<WebCore::Node*> empty;

    if (!c || !c->root)
        return empty;

    {
        unsigned version = c->document ? c->document->domTreeVersion() : 0;

        if (c->nodesValid && c->nodesVersion == version && c->nodes)
            return *c->nodes;

        if (!c->nodes)
            c->nodes = new std::vector<WebCore::Node*>();
        c->nodes->clear();

        for (WebCore::Node* n = c->root->firstChild(); n; n = n->traverseNextNode(c->root))
            if (n->isElementNode()
                    && isElementClassToken(static_cast<WebCore::Element*>(n), c->name))
                c->nodes->push_back(n);

        c->nodesVersion = version;
        c->nodesValid = true;
        return *c->nodes;
    }
}

static unsigned collectionCount(AgbCollection* c)
{
    if (!c)
        return 0;

    if (collectionIsStatic(c))
        return (unsigned)c->items.size();

    if (!c->root)
        return 0;

    if (c->kind == AgbCollectionChildNodes || c->kind == AgbCollectionChildren) {
        unsigned count = 0;

        for (WebCore::Node* child = c->root->firstChild(); child; child = child->nextSibling())
            if (c->kind == AgbCollectionChildNodes || child->isElementNode())
                ++count;
        return count;
    }

    if (c->kind == AgbCollectionTags) {
        RefPtr<WebCore::NodeList> list =
            c->root->getElementsByTagName(WebCore::String::fromUTF8(c->name.c_str()));

        return list ? list->length() : 0;
    }

    if (c->kind == AgbCollectionDocumentList) {
        RefPtr<WebCore::HTMLCollection> list = documentListCollection(c);

        return list ? list->length() : 0;
    }

    if (c->kind == AgbCollectionFormElements) {
        RefPtr<WebCore::HTMLCollection> list;

        if (c->root->isElementNode() &&
            static_cast<WebCore::Element*>(c->root)->hasTagName(WebCore::HTMLNames::formTag))
            list = static_cast<WebCore::HTMLFormElement*>(c->root)->elements();
        return list ? list->length() : 0;
    }

    /* getElementsByClassName: the 2008 tree has no live class collection. */
    return (unsigned)classCollectionNodes(c).size();
}

static WebCore::Node* collectionItemAt(AgbCollection* c, unsigned index)
{
    if (!c || !c->root || collectionIsStatic(c))
        return NULL;

    if (c->kind == AgbCollectionChildNodes || c->kind == AgbCollectionChildren) {
        unsigned i = 0;

        for (WebCore::Node* child = c->root->firstChild(); child; child = child->nextSibling()) {
            if (c->kind == AgbCollectionChildren && !child->isElementNode())
                continue;
            if (i++ == index)
                return child;
        }
        return NULL;
    }

    if (c->kind == AgbCollectionTags) {
        RefPtr<WebCore::NodeList> list =
            c->root->getElementsByTagName(WebCore::String::fromUTF8(c->name.c_str()));

        return list ? list->item(index) : NULL;
    }

    if (c->kind == AgbCollectionDocumentList) {
        RefPtr<WebCore::HTMLCollection> list = documentListCollection(c);

        return list ? list->item(index) : NULL;
    }

    if (c->kind == AgbCollectionFormElements) {
        RefPtr<WebCore::HTMLCollection> list;

        if (c->root->isElementNode() &&
            static_cast<WebCore::Element*>(c->root)->hasTagName(WebCore::HTMLNames::formTag))
            list = static_cast<WebCore::HTMLFormElement*>(c->root)->elements();
        return list ? list->item(index) : NULL;
    }

    {
        const std::vector<WebCore::Node*>& nodes = classCollectionNodes(c);

        return index < nodes.size() ? nodes[index] : NULL;
    }
}

/* element.attributes: a live NamedAttrMap wrapper (NamedNodeMap). Attributes are
 * real Attr nodes, so item()/getNamedItem() return wrappers like Chrome; the
 * indexes are own keys through antb_object_set_indexed_keys (the count comes
 * from `length`, answered below). */
static WebCore::NamedAttrMap* namedMapOf(AntbValue* self)
{
    return self ? static_cast<WebCore::NamedAttrMap*>(antb_get_native(self, AGB_TAG_NAMEDMAP)) : NULL;
}

static AntbValue* wrapNamedNodeMap(AntbEngine* engine, WebCore::NamedAttrMap* map)
{
    AntbValue* v;

    if (!engine || !map || !g_namedNodeMapIface)
        return NULL;

    v = antb_wrap_native_as_owner(engine, g_namedNodeMapIface, map, AGB_TAG_NAMEDMAP, g_document);
    if (!v)
        return NULL;

    antb_object_set_indexed_keys(engine, v);
    return v;
}

static int namedMapGetter(AntbEngine* engine, AntbValue* self, const char* name,
                          AntbValue* out, void*)
{
    WebCore::NamedAttrMap* map = namedMapOf(self);
    unsigned index = 0;


    if (!map || !name)
        return 0;

    if (!strcmp(name, "length")) {
        AntbValue* v = antb_make_number(engine, (double)map->length());

        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    if (cssomNumericName(name, &index)) {
        /* The RefPtr matters: map->item() returns a PassRefPtr, and a lazily
         * created Attr is owned only by it until the wrapper takes it - reading
         * .get() from the temporary left a dangling pointer (crash in wrapNode). */
        RefPtr<WebCore::Node> attr = map->item(index);

        if (!attr)
            return 0;
        {
            AntbValue* v = wrapNode(engine, attr.get());

            if (!v)
                return 0;
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        return 1;
    }

    /* Named properties: `element.attributes["onclick"]` is an Attr, exactly
     * like the indices. jQuery 1.x feature-detects event bubbling with
     * `div.attributes[eventName].expando`; without this it throws and every
     * script after it (Baidu's placeholder setup included) dies. */
    {
        RefPtr<WebCore::Node> attr = map->getNamedItem(WebCore::String::fromUTF8(name));

        if (!attr)
            return 0;
        {
            AntbValue* v = wrapNode(engine, attr.get());

            if (!v)
                return 0;
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        return 1;
    }
}

static AntbValue* collectionWrapItem(AntbEngine* engine, AgbCollection* c, unsigned index)
{
    WebCore::Node* node;

    if (!c)
        return NULL;

    if (collectionIsStatic(c)) {
        if (index >= c->items.size() || !c->items[index])
            return NULL;
        return antb_retain(c->items[index]);
    }

    node = collectionItemAt(c, index);
    return node ? wrapNode(engine, node) : NULL;
}

static bool nodeMatchesNamed(WebCore::Node* node, const WebCore::String& name)
{
    WebCore::Element* element;

    if (!node || !node->isElementNode())
        return false;
    element = static_cast<WebCore::Element*>(node);
    return element->getAttribute(WebCore::String("id")) == name
        || element->getAttribute(WebCore::String("name")) == name;
}

static WebCore::Node* collectionNamed(AgbCollection* c, const WebCore::String& name)
{
    unsigned count, i;

    if (!c || name.isEmpty())
        return NULL;

    count = collectionCount(c);
    for (i = 0; i < count; ++i) {
        WebCore::Node* node = collectionItemAt(c, i);

        if (nodeMatchesNamed(node, name))
            return node;
    }
    return NULL;
}

static AgbCollection* collectionNew(WebCore::Node* root, int kind, const std::string& name)
{
    AgbCollection* c = new AgbCollection();

    c->document = root ? root->document() : NULL;
    c->root = root;
    c->kind = kind;
    c->name = name;

    if (c->document) {
        DocumentRuntime* runtime = documentRuntimeFor(g_currentState, c->document);

        if (runtime) {
            if (!runtime->collections)
                runtime->collections = new std::vector<AgbCollection*>();
            runtime->collections->push_back(c);
        }
    }
    return c;
}

static std::string collectionKey(WebCore::Node* root, int kind, const std::string& name)
{
    char buffer[64];

    sprintf(buffer, "%d:%p:", kind, (void*)root);
    return std::string(buffer) + name;
}

/* Live collections keep their identity (`el.children === el.children`,
 * `document.forms === document.forms`, as in Chrome). querySelectorAll builds
 * a fresh NodeList on every call, like a browser. */
static AgbCollection* collectionFor(WebCore::Node* root, int kind, const std::string& name)
{
    DocumentRuntime* runtime = (root && root->document())
        ? documentRuntimeFor(g_currentState, root->document()) : NULL;
    std::string key;

    if (!runtime)
        return collectionNew(root, kind, name);

    key = collectionKey(root, kind, name);
    if (runtime->collectionCache) {
        std::map<std::string, AgbCollection*>::iterator it = runtime->collectionCache->find(key);

        if (it != runtime->collectionCache->end())
            return it->second;
    }

    {
        AgbCollection* created = collectionNew(root, kind, name);

        if (!runtime->collectionCache)
            runtime->collectionCache = new std::map<std::string, AgbCollection*>();
        (*runtime->collectionCache)[key] = created;
        return created;
    }
}

static AntbValue* collectionWrap(AntbEngine* engine, AgbCollection* c)
{
    AntbClass* klass;
    AntbValue* v;

    if (!engine || !c)
        return NULL;

    klass = (c->kind == AgbCollectionChildNodes || collectionIsStatic(c))
        ? g_nodeListClass : g_htmlCollectionClass;
    if (!klass)
        return NULL;

    v = antb_wrap_native_as_owner(engine, klass, c, AGB_TAG_COLLECTION, c->document);
    if (!v)
        return NULL;

    /* Array-like own keys: Object.keys(nl) / for-in / hasOwnProperty('0') see
     * 0..length-1, like a browser. The engine asks `length` through the normal
     * dispatch, so live collections stay live (issues §34.2). */
    antb_object_set_indexed_keys(engine, v);
    return v;
}

static AntbValue* wrapCollection(AntbEngine* engine, WebCore::Node* root, int kind,
                                 const std::string& name)
{
    if (!engine || !root)
        return NULL;

    ensureInterfaceClasses(engine);
    return collectionWrap(engine, collectionFor(root, kind, name));
}

static AntbValue* wrapStaticCollection(AntbEngine* engine, WebCore::Node* root,
                                       const std::vector<WebCore::Node*>& nodes)
{
    AgbCollection* c;
    size_t i;

    if (!engine || !root)
        return NULL;

    ensureInterfaceClasses(engine);
    c = collectionNew(root, AgbCollectionStatic, std::string());

    for (i = 0; i < nodes.size(); ++i) {
        AntbValue* wrapped = wrapNode(engine, nodes[i]);

        if (wrapped) {
            c->items.push_back(antb_retain(wrapped));
            antb_value_release(wrapped);
        }
    }

    return collectionWrap(engine, c);
}

static void releaseCollection(AgbCollection* c, bool releaseValues)
{
    if (!c)
        return;

    if (releaseValues) {
        for (size_t i = 0; i < c->items.size(); ++i)
            if (c->items[i])
                antb_value_release(c->items[i]);
    }
    c->items.clear();
    delete c->nodes;
    c->nodes = NULL;
    delete c;
}

static void releaseCollections(DocumentRuntime* document, bool releaseValues)
{
    if (!document)
        return;

    if (document->collections) {
        for (size_t i = 0; i < document->collections->size(); ++i)
            releaseCollection((*document->collections)[i], releaseValues);
        delete document->collections;
        document->collections = NULL;
    }

    if (document->collectionCache) {
        delete document->collectionCache;
        document->collectionCache = NULL;
    }
}

int collectionGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    AgbCollection* c = (AgbCollection*)antb_get_native(self, AGB_TAG_COLLECTION);
    const char* p;

    if (!c || !name)
        return 0;

    if (!strcmp(name, "length")) {
        AntbValue* v = antb_make_number(engine, (double)collectionCount(c));

        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    /* Indexed access (`list[0]`). Non-numeric names fall through to the
     * prototype (item/namedItem/forEach/iterators); an out-of-range index
     * falls through too, which reads as undefined like in a browser. */
    p = name;
    if (*p >= '0' && *p <= '9') {
        unsigned long index = 0;
        AntbValue* v;

        while (*p >= '0' && *p <= '9') {
            index = index * 10 + (unsigned long)(*p - '0');
            if (index > 0x7fffffffUL)
                return 0;
            ++p;
        }
        if (*p != '\0')
            return 0;

        v = collectionWrapItem(engine, c, (unsigned)index);
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    return 0;
}

/* NamedNodeMap methods (element.attributes). The name -> implementation table is
 * global per method name, so `item` is shared with NodeList/HTMLCollection and
 * dispatches on the receiver tag. */
/* getNamedItem returns a PassRefPtr; the caller keeps it in `hold` so the node
 * (created lazily on first access) stays alive while the wrapper is built. */
static void namedMapLookup(WebCore::NamedAttrMap* map, AntbValue* arg,
                           RefPtr<WebCore::Node>& hold)
{
    char* name;

    if (!map || !arg)
        return;

    name = antb_to_utf8(arg);
    if (name) {
        hold = map->getNamedItem(WebCore::String::fromUTF8(name));
        antb_free(name);
    }
}

static void assignNodeOrNull(AntbEngine* engine, AntbValue* out, WebCore::Node* node)
{
    if (node) {
        AntbValue* v = wrapNode(engine, node);

        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        return;
    }

    {
        AntbValue* nullValue = antb_make_null(engine);

        if (nullValue) {
            antb_value_assign(out, nullValue);
            antb_value_release(nullValue);
        }
    }
}

int namedMapItemFn(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                   AntbValue* out, void*)
{
    WebCore::NamedAttrMap* map = namedMapOf(self);
    double index;

    (void)out;

    if (!map || argc < 1 || !args[0])
        return 1;

    index = antb_to_number(args[0]);
    if (index < 0)
        index = 0;

    {
        RefPtr<WebCore::Node> node = map->item((unsigned)index);

        assignNodeOrNull(engine, out, node.get());
    }
    return 1;
}

int namedMapGetNamedItemFn(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void*)
{
    WebCore::NamedAttrMap* map = namedMapOf(self);

    if (!map || argc < 1 || !args[0])
        return 1;

    {
        RefPtr<WebCore::Node> node;

        namedMapLookup(map, args[0], node);
        assignNodeOrNull(engine, out, node.get());
    }
    return 1;
}

int namedMapSetNamedItem(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                         AntbValue* out, void*)
{
    WebCore::NamedAttrMap* map = namedMapOf(self);
    WebCore::Node* node = (argc > 0 && args[0]) ? nodeOf(args[0]) : NULL;
    WebCore::ExceptionCode ec = 0;

    (void)engine;

    if (!map || !node)
        return 1;
    map->setNamedItem(node, ec);
    antb_value_assign(out, args[0]);
    return 1;
}

int namedMapRemoveNamedItem(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                            AntbValue* out, void*)
{
    WebCore::NamedAttrMap* map = namedMapOf(self);
    WebCore::ExceptionCode ec = 0;
    char* name;

    if (!map || argc < 1 || !args[0])
        return 1;
    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    {
        RefPtr<WebCore::Node> removed = map->removeNamedItem(WebCore::String::fromUTF8(name), ec);

        antb_free(name);
        assignNodeOrNull(engine, out, removed.get());
    }
    return 1;
}

int namedMapGetNamedItemNS(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void*)
{
    WebCore::NamedAttrMap* map = namedMapOf(self);
    char* ns;
    char* local;

    if (!map || argc < 2 || !args[0] || !args[1])
        return 1;

    ns = antb_to_utf8(args[0]);
    local = antb_to_utf8(args[1]);

    {
        RefPtr<WebCore::Node> node;

        if (ns && local)
            node = map->getNamedItemNS(WebCore::String::fromUTF8(ns),
                                       WebCore::String::fromUTF8(local));
        if (ns)
            antb_free(ns);
        if (local)
            antb_free(local);
        if (!node)
            return 1;
        assignNodeOrNull(engine, out, node.get());
    }
    return 1;
}

int namedMapSetNamedItemNS(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void*)
{
    return namedMapSetNamedItem(engine, self, args, argc, out, NULL);
}

int namedMapRemoveNamedItemNS(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                              AntbValue* out, void*)
{
    WebCore::NamedAttrMap* map = namedMapOf(self);
    WebCore::ExceptionCode ec = 0;
    char* ns;
    char* local;

    if (!map || argc < 2 || !args[0] || !args[1])
        return 1;

    ns = antb_to_utf8(args[0]);
    local = antb_to_utf8(args[1]);

    {
        RefPtr<WebCore::Node> removed;

        if (ns && local)
            removed = map->removeNamedItemNS(WebCore::String::fromUTF8(ns),
                                             WebCore::String::fromUTF8(local), ec);
        if (ns)
            antb_free(ns);
        if (local)
            antb_free(local);
        if (!removed)
            return 1;
        assignNodeOrNull(engine, out, removed.get());
    }
    return 1;
}

int collectionItem(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                   AntbValue* out, void*)
{
    AgbCollection* c = (AgbCollection*)antb_get_native(self, AGB_TAG_COLLECTION);
    double index;
    AntbValue* v;

    if (antb_get_native(self, AGB_TAG_NAMEDMAP))
        return namedMapItemFn(engine, self, args, argc, out, NULL);

    if (!c || argc < 1 || !args[0])
        return 1;

    index = antb_to_number(args[0]);
    if (index < 0)
        index = 0;

    v = collectionWrapItem(engine, c, (unsigned)index);
    if (v) {
        antb_value_assign(out, v);
        antb_value_release(v);
    } else {
        AntbValue* nullValue = antb_make_null(engine);

        if (nullValue) {
            antb_value_assign(out, nullValue);
            antb_value_release(nullValue);
        }
    }
    return 1;
}

int collectionNamedItemMethod(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                              AntbValue* out, void*)
{
    AgbCollection* c = (AgbCollection*)antb_get_native(self, AGB_TAG_COLLECTION);
    char* name;
    AntbValue* v;

    if (!c || argc < 1)
        return 1;

    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    {
        WebCore::Node* found = collectionNamed(c, WebCore::String::fromUTF8(name));

        v = found ? wrapNode(engine, found) : NULL;
    }
    antb_free(name);

    if (!v) {
        AntbValue* nullValue = antb_make_null(engine);

        if (nullValue) {
            antb_value_assign(out, nullValue);
            antb_value_release(nullValue);
        }
        return 1;
    }

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
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

/* ---- reflected attributes (A3) ------------------------------------------- *
 * The 2008 IDL declares many attributes the facade never implemented: before
 * the generated tables existed they were simply invisible (`'x' in el` false),
 * so nobody noticed. Now the accessor exists and nodeGetter returns undefined -
 * this table answers them the way Chrome's reflected IDL attributes work.
 *
 * Only pure reflection is here. Anything whose value is DOM state rather than
 * the attribute (input.checked, canvas.width, textarea.selectionStart,
 * form.elements/length, element.form, form.method's normalisation) stays on the
 * A3 worklist: answering it with the raw attribute would be wrong. */
enum ReflectKind {
    REFLECT_STRING_ATTR,
    REFLECT_BOOL_ATTR,
    REFLECT_LONG_ATTR,
    REFLECT_ELEMENT_PREFIX,
    REFLECT_ELEMENT_LOCALNAME
};

struct ReflectDef {
    const char* name;
    unsigned    kind;
    const char* attr;   /* HTML attribute name (unused for the element kinds) */
};

static const ReflectDef kReflected[] = {
    /* HTMLAnchorElement */
    { "coords",        REFLECT_STRING_ATTR, "coords" },
    { "hreflang",      REFLECT_STRING_ATTR, "hreflang" },
    { "rev",           REFLECT_STRING_ATTR, "rev" },
    { "shape",         REFLECT_STRING_ATTR, "shape" },
    { "target",        REFLECT_STRING_ATTR, "target" },

    /* HTMLFormElement (acceptCharset/encoding are the odd attribute names) */
    { "acceptCharset", REFLECT_STRING_ATTR, "accept-charset" },
    { "encoding",      REFLECT_STRING_ATTR, "enctype" },
    { "enctype",       REFLECT_STRING_ATTR, "enctype" },
    { "method",        REFLECT_STRING_ATTR, "method" },

    /* HTMLInputElement / HTMLImageElement / HTMLIFrameElement legacy attributes */
    { "accept",        REFLECT_STRING_ATTR, "accept" },
    { "align",         REFLECT_STRING_ATTR, "align" },
    { "useMap",        REFLECT_STRING_ATTR, "usemap" },
    { "longDesc",      REFLECT_STRING_ATTR, "longdesc" },
    { "marginHeight",  REFLECT_LONG_ATTR,   "marginheight" },
    { "marginWidth",   REFLECT_LONG_ATTR,   "marginwidth" },
    { "scrolling",     REFLECT_STRING_ATTR, "scrolling" },

    /* Content attributes whose IDL name differs from the attribute name
     * (button.value / input.defaultValue both read the value attribute; a
     * bespoke branch above handles the live value of a form control). */
    { "value",         REFLECT_STRING_ATTR, "value" },
    { "defaultValue",  REFLECT_STRING_ATTR, "value" },

    /* HTMLOptionElement / HTMLElement */
    { "label",         REFLECT_STRING_ATTR, "label" },
    { "accessKey",     REFLECT_STRING_ATTR, "accesskey" },

    /* boolean attributes (presence == true, like Chrome's reflected booleans) */
    { "disabled",      REFLECT_BOOL_ATTR,   "disabled" },
    { "multiple",      REFLECT_BOOL_ATTR,   "multiple" },
    { "readOnly",      REFLECT_BOOL_ATTR,   "readonly" },
    { "defaultChecked", REFLECT_BOOL_ATTR,  "checked" },
    { "defaultSelected", REFLECT_BOOL_ATTR, "selected" },

    /* long attributes */
    { "maxLength",     REFLECT_LONG_ATTR,   "maxlength" },
    { "size",          REFLECT_LONG_ATTR,   "size" },
    { "cols",          REFLECT_LONG_ATTR,   "cols" },
    { "rows",          REFLECT_LONG_ATTR,   "rows" },

    /* Element DOM properties, not attributes */
    { "prefix",        REFLECT_ELEMENT_PREFIX,    "" },
    { "localName",     REFLECT_ELEMENT_LOCALNAME, "" }
};

static const ReflectDef* findReflected(const char* name)
{
    size_t i;

    for (i = 0; i < sizeof(kReflected) / sizeof(kReflected[0]); ++i) {
        if (!strcmp(kReflected[i].name, name))
            return &kReflected[i];
    }
    return 0;
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

    /* --- element tree navigation ----------------------------------------- *
     * WebCore 2008 only has parentNode/childNodes/nextSibling; every modern
     * framework walks the element-only variants, and reading them as
     * undefined made `el.parentElement.remove()` throw. */
    if (!strcmp(name, "parentElement")) {
        WebCore::Node* parent = n->parent();

        if (!parent || !parent->isElementNode())
            return assignNull(engine, out);   /* null for a top level element */
        v = wrapNode(engine, parent);
    } else if (!strcmp(name, "firstElementChild") || !strcmp(name, "lastElementChild")) {
        WebCore::Element* child = childElement(n, name[0] == 'f' ? 0 : -1);

        if (!child)
            return assignNull(engine, out);
        v = wrapNode(engine, child);
    } else if (!strcmp(name, "nextElementSibling") || !strcmp(name, "previousElementSibling")) {
        WebCore::Element* sibling = siblingElement(n, name[0] == 'n' ? 1 : -1);

        if (!sibling)
            return assignNull(engine, out);
        v = wrapNode(engine, sibling);
    } else if (!strcmp(name, "isConnected")) {
        v = antb_make_boolean(engine, nodeIsConnected(n) ? 1 : 0);
    } else if (!strcmp(name, "namespaceURI")) {
        /* Vue's hydration picks the render namespace (svg/mathml/html) from
         * element.namespaceURI; without it `hydrate()` threw on
         * `undefined.includes`. HTML documents use the xhtml URI. */
        const WebCore::AtomicString& ns = n->namespaceURI();
        v = antb_make_string(engine, ns.isEmpty() ? "http://www.w3.org/1999/xhtml"
                                                  : ns.deprecatedString().utf8().data());
    } else if (!strcmp(name, "baseURI")) {
        /* Node.baseURI: the document base URL (DOM4), answered for every node.
         * Pages resolve their own relative URLs against it (monaco's playground
         * does `document.baseURI.toString()` before anything else), and the
         * property was simply absent - the read was `undefined`. */
        WebCore::String base = n->baseURI();
        v = antb_make_string(engine, base.utf8().data());
    } else if (!strcmp(name, "nodeValue")) {
        /* Text/Comment carry data; every other node type is null. */
        if (n->isTextNode() || n->isCommentNode()) {
            WebCore::String data = static_cast<WebCore::CharacterData*>(n)->data();

            v = antb_make_string(engine, data.utf8().data());
        } else {
            return assignNull(engine, out);
        }
    } else if (n->isAttributeNode() && (!strcmp(name, "name") || !strcmp(name, "value")
                                        || !strcmp(name, "specified") || !strcmp(name, "ownerElement"))) {
        /* Returned immediately: later independent blocks in this function also
         * claim some of these names (name/value) and would discard the answer. */
        WebCore::Attr* attr = static_cast<WebCore::Attr*>(n);

        if (!strcmp(name, "ownerElement")) {
            WebCore::Element* owner = attr->ownerElement();

            if (!owner)
                return assignNull(engine, out);
            v = wrapNode(engine, owner);
        } else if (!strcmp(name, "specified")) {
            v = antb_make_boolean(engine, attr->specified() ? 1 : 0);
        } else {
            WebCore::String text = !strcmp(name, "name") ? attr->name() : attr->value();

            v = antb_make_string(engine, text.utf8().data());
        }

        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    } else if (!strcmp(name, "data") && n->isCharacterDataNode()) {
        /* CharacterData.data: text/comment content (the generated table declares
         * it, so without this the accessor answered undefined). */
        WebCore::String data = static_cast<WebCore::CharacterData*>(n)->data();

        v = antb_make_string(engine, data.utf8().data());
    } else if (!strcmp(name, "length") && n->isCharacterDataNode()) {
        /* Answered here, and returned immediately: a later (independent) block
         * in this function claims "length" for <select> and bails out for any
         * non-element, which would discard the value computed here. */
        v = antb_make_number(engine, (double)static_cast<WebCore::CharacterData*>(n)->length());
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
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
                return assignNull(engine, out);
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
            v = antb_make_boolean(engine, image->complete() ? 1 : 0);
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
            /* A live HTMLCollection (B2-2) over the form's controls. */
            v = wrapCollection(engine, n, AgbCollectionFormElements, std::string());
        }
    } else if (n->isElementNode()
            && static_cast<WebCore::Element*>(n)->isFrameOwnerElement()
            && (!strcmp(name, "contentWindow") || !strcmp(name, "contentDocument"))) {
        WebCore::Document* child = static_cast<WebCore::HTMLFrameOwnerElement*>(n)->contentDocument();

        if (!child)
            return assignNull(engine, out);   /* null until the frame document exists */
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

    /* Object.prototype helpers (hasOwnProperty/isPrototypeOf/...) are inherited
     * from Object.prototype now that every interface prototype chains to it;
     * the catch-all must fall through instead of shadowing them. */

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
            return assignNull(engine, out);   /* null */
        }
    } else if (!strcmp(name, "firstChild")) {
        if (n->firstChild()) {
            v = wrapNode(engine, n->firstChild());
        } else {
            return assignNull(engine, out);
        }
    } else if (!strcmp(name, "lastChild")) {
        if (n->lastChild()) {
            v = wrapNode(engine, n->lastChild());
        } else {
            return assignNull(engine, out);
        }
    } else if (!strcmp(name, "previousSibling")) {
        if (n->previousSibling()) {
            v = wrapNode(engine, n->previousSibling());
        } else {
            return assignNull(engine, out);
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
            return assignNull(engine, out);
        }
    } else if (!strcmp(name, "nodeValue")) {
        if (n->isTextNode() || n->isCommentNode())
            v = antb_make_string(engine, n->nodeValue().utf8().data());
        else
            return assignNull(engine, out);
    } else if (!strcmp(name, "hasChildNodes")) {
        /* Real DOM exposes this as a method; nodeHasChildNodes() serves it. */
        return 1;
    } else if (!strcmp(name, "nextSibling")) {
        if (n->nextSibling()) {
            v = wrapNode(engine, n->nextSibling());
        } else {
            return assignNull(engine, out);
        }
    } else if (!strcmp(name, "childNodes")) {
        AntbValue* list = wrapCollection(engine, n, AgbCollectionChildNodes, std::string());
        if (!list)
            return 0;
        antb_value_assign(out, list);
        antb_value_release(list);
        return 1;
    } else if (!strcmp(name, "children")) {
        AntbValue* list = wrapCollection(engine, n, AgbCollectionChildren, std::string());
        if (!list)
            return 0;
        antb_value_assign(out, list);
        antb_value_release(list);
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
            } else if (element->hasTagName(WebCore::HTMLNames::buttonTag)) {
                WebCore::String s = static_cast<WebCore::HTMLButtonElement*>(element)->value();
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
                v = antb_make_boolean(engine, option->selected() ? 1 : 0);
            } else if (!strcmp(name, "index")) {
                v = antb_make_number(engine, (double)option->index());
            } else if (!strcmp(name, "text")) {
                v = antb_make_string(engine, option->text().utf8().data());
            } else {
                return 0;
            }
        } else if (element->hasTagName(WebCore::HTMLNames::formTag)
                   && !strcmp(name, "length")) {
            /* form.length: the number of listed controls (Chrome counts them). */
            v = antb_make_number(engine, (double)static_cast<WebCore::HTMLFormElement*>(element)->length());
        } else if (element->hasTagName(WebCore::HTMLNames::aTag)
                   && !strcmp(name, "text")) {
            WebCore::String s = static_cast<WebCore::HTMLAnchorElement*>(element)->text();
            v = antb_make_string(engine, s.utf8().data());
        } else {
            return 0;
        }
    } else if (!strcmp(name, "shadowRoot") && n->isElementNode()) {
        /* Only an open root is exposed on the host (Chrome: closed ones are not). */
        if (shadowRootMode(n) != 1)
            return assignNull(engine, out);
        {
            AntbValue* root = wrapShadowRoot(engine, static_cast<WebCore::Element*>(n));

            if (!root)
                return assignNull(engine, out);
            antb_value_assign(out, root);
            antb_value_release(root);
        }
        return 1;
    } else if (!strcmp(name, "assignedSlot")) {
        WebCore::Node* slot = NULL;

        if (g_currentDocument && g_currentDocument->slotOf) {
            std::map<WebCore::Node*, WebCore::Node*>::iterator it =
                g_currentDocument->slotOf->find(n);

            if (it != g_currentDocument->slotOf->end())
                slot = it->second;
        }
        if (!slot)
            return assignNull(engine, out);
        {
            AntbValue* wrapped = wrapNode(engine, slot);

            if (!wrapped)
                return assignNull(engine, out);
            antb_value_assign(out, wrapped);
            antb_value_release(wrapped);
        }
        return 1;
    } else if (!strcmp(name, "content") && n->isElementNode()
               && isTemplateElement(static_cast<WebCore::Element*>(n))) {
        /* HTMLTemplateElement.content: the lazily created fragment. */
        WebCore::DocumentFragment* fragment =
            templateContents(static_cast<WebCore::Element*>(n), true);

        if (!fragment)
            return assignNull(engine, out);
        v = wrapNode(engine, fragment);
    } else if (!strcmp(name, "innerHTML")) {
        if (!n->isElementNode())
            return 0;
        {
            WebCore::Element* element = static_cast<WebCore::Element*>(n);
            if (!element->isHTMLElement())
                return 0;
            if (isTemplateElement(element)) {
                std::string html = templateInnerHTML(element);

                v = antb_make_string(engine, html.c_str());
            } else {
                WebCore::String html = static_cast<WebCore::HTMLElement*>(element)->innerHTML();
                v = antb_make_string(engine, html.utf8().data());
            }
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
        /* element.attributes is a live NamedNodeMap of Attr nodes (G1-2); the
         * wrapper is cached by the bridge, so el.attributes === el.attributes. */
        if (!n->isElementNode())
            return assignNull(engine, out);
        {
            WebCore::NamedAttrMap* map = static_cast<WebCore::Element*>(n)->attributes();

            if (!map)
                return assignNull(engine, out);
            {
                AntbValue* attrs = wrapNamedNodeMap(engine, map);

                if (!attrs)
                    return 0;
                antb_value_assign(out, attrs);
                antb_value_release(attrs);
            }
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
        /* element.style is a real CSSStyleDeclaration instance (Chrome's shape:
         * instanceof, @@toStringTag, live inline styles), not a plain object.
         * The declaration belongs to the element, so the bridge cache gives the
         * same wrapper back every time (`el.style === el.style`); the property
         * dispatch stays the facade's styleGetter/styleSetter, installed as this
         * wrapper's instance hook (it knows cssText/length/methods and the
         * camelCase -> CSS property translation). */
        WebCore::StyledElement* styled = n->isStyledElement()
            ? static_cast<WebCore::StyledElement*>(n) : NULL;

        if (!styled)
            return 0;

        {
            AntbValue* style = cssomWrap(engine, styled->style(), CSSOM_DECL);

            if (!style)
                return 0;

            antb_set_prop(engine, style, "__agbElement", self);
            antb_define_property_rw(engine, style, "style", styleGetter, styleSetter, NULL);
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

    /* Last resort before the expando fallback: attributes the IDL declares and
     * the facade has no bespoke branch for. These are pure HTML reflection -
     * string attributes as-is, boolean attributes by presence, long ones parsed -
     * exactly what Chrome's reflected IDL attributes do. A bespoke branch above
     * always wins, so this can only turn `undefined` into a real value.
     *
     * Deliberately *not* here: attributes whose value is a DOM property, not the
     * attribute (input.checked, canvas.width, form.method's normalisation,
     * textarea.selectionStart, form.elements/length, element.form). Those need
     * the engine's state machine and stay on the A3 worklist. */
    if (!v && n->isElementNode()) {
        WebCore::Element* el = static_cast<WebCore::Element*>(n);
        const ReflectDef* reflected = findReflected(name);

        if (reflected) {
            if (reflected->kind == REFLECT_STRING_ATTR) {
                WebCore::String s = el->getAttribute(WebCore::AtomicString(reflected->attr));
                v = antb_make_string(engine, s.utf8().data());
            } else if (reflected->kind == REFLECT_BOOL_ATTR) {
                v = antb_make_boolean(engine,
                        el->hasAttribute(WebCore::AtomicString(reflected->attr)) ? 1 : 0);
            } else if (reflected->kind == REFLECT_LONG_ATTR) {
                WebCore::String s = el->getAttribute(WebCore::AtomicString(reflected->attr));
                int value = s.isEmpty() ? 0 : s.toInt();
                v = antb_make_number(engine, (double)value);
            } else if (reflected->kind == REFLECT_ELEMENT_PREFIX) {
                WebCore::String s = el->prefix();
                if (s.isNull())
                    v = antb_make_null(engine);
                else
                    v = antb_make_string(engine, s.utf8().data());
            } else if (reflected->kind == REFLECT_ELEMENT_LOCALNAME) {
                WebCore::String s = el->localName();
                v = antb_make_string(engine, s.utf8().data());
            }
        }
    }


    /* DOM-state attributes the reflection table deliberately leaves out: the
     * value comes from the engine's state, not from the attribute. */
    if (!v && n->isElementNode()) {
        WebCore::Element* el = static_cast<WebCore::Element*>(n);

        if (!strcmp(name, "isContentEditable")) {
            v = antb_make_boolean(engine, n->isContentEditable() ? 1 : 0);
        } else if (el->hasTagName(WebCore::HTMLNames::canvasTag)) {
            WebCore::HTMLCanvasElement* canvas = static_cast<WebCore::HTMLCanvasElement*>(el);

            if (!strcmp(name, "width"))
                v = antb_make_number(engine, (double)canvas->width());
            else if (!strcmp(name, "height"))
                v = antb_make_number(engine, (double)canvas->height());
        } else if (!strcmp(name, "form")) {
            /* The form owner: a DOM relationship, not an attribute. An element
             * outside a form answers null (Chrome), not undefined. */
            WebCore::HTMLFormElement* form = 0;

            if (el->hasTagName(WebCore::HTMLNames::inputTag)
                || el->hasTagName(WebCore::HTMLNames::selectTag)
                || el->hasTagName(WebCore::HTMLNames::textareaTag)
                || el->hasTagName(WebCore::HTMLNames::buttonTag)) {
                form = static_cast<WebCore::HTMLGenericFormElement*>(el)->form();
            } else if (el->hasTagName(WebCore::HTMLNames::optionTag)) {
                WebCore::HTMLSelectElement* select =
                    static_cast<WebCore::HTMLOptionElement*>(el)->getSelect();

                if (select)
                    form = select->form();
            } else {
                return 0;
            }

            /* WebCore 2008 only sets m_form from the parser (parserSetForm), so a
             * control created by script and appended into a <form> has no form
             * owner. Chrome resolves it from the ancestor chain, which is what
             * pages rely on. */
            if (!form) {
                WebCore::Node* ancestor = el->hasTagName(WebCore::HTMLNames::optionTag)
                    ? static_cast<WebCore::HTMLOptionElement*>(el)->getSelect()
                    : el;

                for (WebCore::Node* p = ancestor ? ancestor->parentNode() : 0; p; p = p->parentNode()) {
                    if (p->hasTagName(WebCore::HTMLNames::formTag)) {
                        form = static_cast<WebCore::HTMLFormElement*>(p);
                        break;
                    }
                }
            }

            if (!form) {
                v = antb_make_null(engine);
            } else {
                v = wrapNode(engine, form);
            }
        } else if (!strcmp(name, "checked")
                   && el->hasTagName(WebCore::HTMLNames::inputTag)) {
            v = antb_make_boolean(engine,
                    static_cast<WebCore::HTMLInputElement*>(el)->checked() ? 1 : 0);
        } else if (!strcmp(name, "indeterminate")
                   && el->hasTagName(WebCore::HTMLNames::inputTag)) {
            v = antb_make_boolean(engine,
                    static_cast<WebCore::HTMLInputElement*>(el)->indeterminate() ? 1 : 0);
        } else if ((!strcmp(name, "selectionStart") || !strcmp(name, "selectionEnd"))
                   && el->hasTagName(WebCore::HTMLNames::textareaTag)) {
            WebCore::HTMLTextAreaElement* area = static_cast<WebCore::HTMLTextAreaElement*>(el);

            v = antb_make_number(engine, (double)(!strcmp(name, "selectionStart")
                                                  ? area->selectionStart()
                                                  : area->selectionEnd()));
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
            WebCore::Element* shadowHost = shadowHostOfWrapper(self);

            if (shadowHost) {
                setShadowInnerHTML(shadowHost, WebCore::String::fromUTF8(text));
                antb_free(text);
                return 1;
            }
        }
        if (shadowInfo(element)) {
            setLightInnerHTML(element, WebCore::String::fromUTF8(text));
            antb_free(text);
            return 1;
        }
        if (isTemplateElement(element)) {
            setTemplateInnerHTML(element, text);
            antb_free(text);
            return 1;
        }
        {
            WebCore::ExceptionCode ec = 0;
            static_cast<WebCore::HTMLElement*>(element)->setInnerHTML(WebCore::String::fromUTF8(text), ec);
        }
        antb_free(text);
        return 1;
    }

    /* Input state that is not an attribute: `checked`/`indeterminate` are the
     * form control's state, and the button's `value` is the value attribute. */
    if (n->isElementNode() && (!strcmp(name, "checked") || !strcmp(name, "indeterminate"))) {
        WebCore::Element* element = static_cast<WebCore::Element*>(n);

        if (element->hasTagName(WebCore::HTMLNames::inputTag)) {
            WebCore::HTMLInputElement* input = static_cast<WebCore::HTMLInputElement*>(element);

            if (!strcmp(name, "checked"))
                input->setChecked(value ? antb_to_boolean(value) != 0 : false);
            else
                input->setIndeterminate(value ? antb_to_boolean(value) != 0 : false);
            return 1;
        }
    }

    if (n->isAttributeNode() && !strcmp(name, "value")) {
        char* text = antb_to_utf8(value);
        WebCore::ExceptionCode ec = 0;

        if (!text)
            return 0;
        static_cast<WebCore::Attr*>(n)->setValue(WebCore::String::fromUTF8(text), ec);
        antb_free(text);
        return 1;
    }

    if (!strcmp(name, "data") && n->isCharacterDataNode()) {
        char* text = antb_to_utf8(value);
        WebCore::ExceptionCode ec = 0;

        if (!text)
            return 0;
        static_cast<WebCore::CharacterData*>(n)->setData(WebCore::String::fromUTF8(text), ec);
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
        else if (element->hasTagName(WebCore::HTMLNames::buttonTag))
            static_cast<WebCore::HTMLButtonElement*>(element)->setValue(WebCore::String::fromUTF8(text));
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
    /* A3: the same reflection table nodeGetter answers from, on the write path -
     * `a.hreflang = 'en'`, `input.disabled = true`, `textarea.cols = 42` become
     * attribute writes exactly like Chrome's reflected IDL attributes. A bespoke
     * setter above always wins (form controls' live value, className, ...). */
    if (n->isElementNode()) {
        const ReflectDef* reflected = findReflected(name);

        if (reflected && reflected->kind != REFLECT_ELEMENT_PREFIX
                       && reflected->kind != REFLECT_ELEMENT_LOCALNAME) {
            WebCore::Element* el = static_cast<WebCore::Element*>(n);
            WebCore::ExceptionCode ec = 0;
            WebCore::AtomicString attr(reflected->attr);

            if (reflected->kind == REFLECT_STRING_ATTR) {
                char* text = antb_to_utf8(value);

                if (text) {
                    el->setAttribute(attr, WebCore::String::fromUTF8(text), ec);
                    antb_free(text);
                    return 1;
                }
            } else if (reflected->kind == REFLECT_BOOL_ATTR) {
                if (antb_to_boolean(value))
                    el->setAttribute(attr, WebCore::String(""), ec);
                else
                    el->removeAttribute(attr, ec);
                return 1;
            } else if (reflected->kind == REFLECT_LONG_ATTR) {
                char buf[32];

                snprintf(buf, sizeof(buf), "%d", (int)antb_to_number(value));
                el->setAttribute(attr, WebCore::String(buf), ec);
                return 1;
            }
        }
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
    /* Names a prototype accessor owns: the expando store would shadow it and the
     * accessor would never run (prelude installs adoptedStyleSheets on
     * Document.prototype/ShadowRoot.prototype, issues #18 phase D). */
    if (!strcmp(name, "adoptedStyleSheets"))
        return 0;

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

    {
        AntbRealm* realm = receiverRealm(n);
        std::vector<Listener>* list = listenersForRealm(realm);
        Listener l;

        if (!list) {
            if (!realm) {
                antb_free(type);
                return 0;
            }
            list = new std::vector<Listener>();
            listenersForRealm(realm) = list;
        }

        l.node = n;
        l.type = type;
        /* Only functions are callable later: pages happily pass null or objects
         * as the listener, and dispatching those crashed the process. */
        l.fn = antb_is_function(args[1]) ? antb_retain(args[1]) : NULL;
        l.realm = realm;
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
            list->push_back(l);
            if (getenv("AGB_EVT_DEBUG"))
                agbLog(AGB_LOG_EVT, "add %s node=%p listeners=%d realm=%p", type, (void*)n, (int)list->size(), (void*)realm);
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

    {
        std::vector<Listener>* list = listenersForRealm(receiverRealm(n));

        if (list) {
            for (size_t i = 0; i < list->size(); ) {
                Listener& l = (*list)[i];

                if (l.node == n && l.type == type && l.fn && antb_value_same(l.fn, args[1])) {
                    antb_value_release(l.fn);
                    list->erase(list->begin() + i);
                    continue;
                }
                ++i;
            }
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

/* True when the event asked the dispatch to stop (stopPropagation /
 * stopImmediatePropagation, or the legacy cancelBubble). */
static bool eventStopped(AntbEngine* engine, AntbValue* evt)
{
    AntbValue* v;

    if (!engine || !evt)
        return false;

    v = antb_get_prop(engine, evt, "__stoppedImmediate");
    if (v) {
        bool stopped = antb_to_boolean(v) != 0;
        antb_value_release(v);
        if (stopped)
            return true;
    }

    v = antb_get_prop(engine, evt, "__stopped");
    if (v) {
        bool stopped = antb_to_boolean(v) != 0;
        antb_value_release(v);
        if (stopped)
            return true;
    }

    v = antb_get_prop(engine, evt, "cancelBubble");
    if (v) {
        bool stopped = antb_to_boolean(v) != 0;
        antb_value_release(v);
        if (stopped)
            return true;
    }
    return false;
}

/* Runs the listeners `node` has for `type`: the snapshot, the calls and the
 * `once` cleanup, which is what both the target phase and the bubbling phase
 * need (they differ only in currentTarget/eventPhase). */
static void dispatchListenersOnNode(AntbEngine* engine, WebCore::Node* node, const char* type,
                                    AntbValue* evt, AntbValue* receiver, AntbValue* phaseFn,
                                    int phase)
{
    std::vector<Listener>* list = listenersForRealm(receiverRealm(node));
    std::vector<AntbValue*> snapshot;
    size_t i;

    if (!list || !type || !evt)
        return;

    for (i = 0; i < list->size(); ++i) {
        Listener& l = (*list)[i];

        if (l.node == node && l.type == type && l.fn)
            snapshot.push_back(antb_retain(l.fn));
    }

    if (snapshot.empty())
        return;

    if (phaseFn)
        setEventPhase(engine, phaseFn, evt, receiver, phase);

    for (i = 0; i < snapshot.size(); ++i) {
        AntbValue* argv[1];
        AntbValue* value;

        /* stopImmediatePropagation() must keep the rest of this node's listeners
         * from running - the same check the engine's own dispatch does. */
        if (i > 0 && eventStopped(engine, evt))
            break;

        argv[0] = evt;
        value = antb_call(engine, snapshot[i], receiver, argv, 1, NULL);

        if (!value) {
            char* pending = antb_take_pending_error(engine);

            if (pending) {
                fprintf(stderr, "[antjs] (dispatchEvent %s) %s\n", type, pending);
                free(pending);
            }
        } else {
            antb_value_release(value);
        }
    }

    /* once: drop the listeners this dispatch just called, by JS identity (the
     * snapshot holds retained wrappers). */
    for (i = 0; listenersForRealm(receiverRealm(node)) && i < listenersForRealm(receiverRealm(node))->size(); ) {
        list = listenersForRealm(receiverRealm(node));
        Listener& l = (*list)[i];
        bool called = false;

        if (l.once && l.node == node && l.type == type && l.fn) {
            for (size_t k = 0; k < snapshot.size(); ++k) {
                if (antb_value_same(l.fn, snapshot[k])) { called = true; break; }
            }
        }

        if (called) {
            antb_value_release(l.fn);
            list->erase(list->begin() + i);
        } else {
            ++i;
        }
    }

    for (i = 0; i < snapshot.size(); ++i)
        antb_value_release(snapshot[i]);
}

int nodeDispatchEvent(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    char* type = (argc > 0) ? eventTypeOf(engine, args[0]) : NULL;
    AntbValue* evt = (argc > 0) ? args[0] : NULL;
    AntbValue* phaseFn = NULL;

    if (!n || !type || !evt) {
        antb_free(type);
        return 0;
    }

    if (getenv("AGB_EVT_DEBUG"))
        agbLog(AGB_LOG_EVT, "JS dispatchEvent(%s) on node=%p", type, (void*)n);

    /* target/currentTarget belong to the node the page dispatched on; the
     * event stays untrusted (the page built it, not the engine). The facade
     * keeps those slots as own data properties, and the same helper also
     * patches a native Event, whose prototype accessors are getter-only. */
    phaseFn = eventPhaseFn(engine);
    if (phaseFn) {
        AntbValue* global = antb_global(engine);
        AntbValue* targetFn = global ? antb_get_prop(engine, global, "__agbEventSetTarget") : NULL;

        if (global)
            antb_value_release(global);
        if (targetFn) {
            AntbValue* targetArgs[2];
            AntbValue* result;

            targetArgs[0] = evt;
            targetArgs[1] = self;
            result = antb_call(engine, targetFn, NULL, targetArgs, 2, NULL);
            if (result)
                antb_value_release(result);
            antb_value_release(targetFn);
        }
        setEventPhase(engine, phaseFn, evt, self, 2);
    }

    /* Target phase: listeners on the node the page called dispatchEvent on. */
    dispatchListenersOnNode(engine, n, type, evt, self, phaseFn, 2);

    /* Bubbling phase: the event walks up the tree while it says `bubbles` and
     * nobody called stopPropagation(). Before this the JS-side dispatchEvent
     * only ran the target's listeners, so `el.dispatchEvent(new Event('x',
     * {bubbles:true}))` never reached the parent - the engine's own dispatch
     * has always bubbled (issues §18.5, item 3). */
    {
        AntbValue* bubblesValue = antb_get_prop(engine, evt, "bubbles");
        bool bubbles = bubblesValue ? (antb_to_boolean(bubblesValue) != 0) : false;

        if (bubblesValue)
            antb_value_release(bubblesValue);

        if (bubbles) {
            for (WebCore::Node* parent = n->parentNode(); parent && !eventStopped(engine, evt);
                 parent = parent->parentNode()) {
                AntbValue* receiver = wrapNode(engine, parent);

                if (!receiver)
                    continue;
                dispatchListenersOnNode(engine, parent, type, evt, receiver, phaseFn, 3);
                antb_value_release(receiver);
            }
        }
    }

    /* dispatchEvent() returns false when a cancelable event was cancelled. */
    {
        bool prevented = false;
        bool cancelable = true;
        AntbValue* v;

        v = antb_get_prop(engine, evt, "defaultPrevented");
        if (v) { prevented = antb_to_boolean(v) != 0; antb_value_release(v); }
        if (!prevented) {
            v = antb_get_prop(engine, evt, "__prevented");
            if (v) { prevented = antb_to_boolean(v) != 0; antb_value_release(v); }
        }
        v = antb_get_prop(engine, evt, "cancelable");
        if (v) { cancelable = antb_to_boolean(v) != 0; antb_value_release(v); }

        /* Chrome resets currentTarget before dispatchEvent() returns. */
        if (phaseFn)
            setEventPhase(engine, phaseFn, evt, NULL, 0);

        if (out) {
            AntbValue* result = antb_make_boolean(engine, (prevented && cancelable) ? 0 : 1);

            if (result) {
                antb_value_assign(out, result);
                antb_value_release(result);
            }
        }
    }

    if (phaseFn)
        antb_value_release(phaseFn);

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
        /* wrapNode() first: it pins the node for the realm's lifetime. */
        AntbValue* v = wrapNode(engine, element.get());

        if (v) {
            AntbValue* ctor = customElementCtor(element.get());

            if (ctor) {
                /* A registered custom element: run its constructor, which builds
                 * the instance from the class prototype and adopts this element
                 * (issues #18/#22). Null means the constructor threw or declined;
                 * fall back to the prototype-only upgrade. */
                AntbValue* instance;

                g_pendingCustomElement = element.get();
                instance = antb_construct(engine, ctor, NULL, 0);
                g_pendingCustomElement = NULL;

                if (instance) {
                    antb_value_assign(out, instance);
                    antb_value_release(instance);
                    antb_value_release(v);
                    return 1;
                }
                upgradeCustomElementIfDefined(element.get(), v);
            }
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

    shadowAfterInsert(self, parent, child);

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
    shadowAfterRemove(self, parent, child);

    /* Chrome returns the removed node; callers chain on it. */
    {
        AntbValue* v = wrapNode(engine, child);

        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
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
    shadowAfterInsert(self, parent, child);

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

    if (n && n->isHTMLElement()) {
        ScriptInitiatedEventScope scope;

        static_cast<WebCore::HTMLElement*>(n)->click();
    }
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
    old->ref();                   /* the replaced node is handed back to JS */
    parent->replaceChild(child, old, ec);
    if (!ec) {
        shadowAfterRemove(self, parent, old);
        shadowAfterInsert(self, parent, child);
    }

    {
        AntbValue* v = wrapNode(engine, old);

        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    old->deref();
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
        AntbValue* v = antb_make_boolean(engine, n->hasChildNodes() ? 1 : 0);

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
        AntbValue* v = antb_make_boolean(engine, has ? 1 : 0);
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
        AntbValue* v = antb_make_boolean(engine, (other && (other == n || other->isDescendantOf(n))) ? 1 : 0);
        if (v) {
            antb_value_assign(out, v);
            antb_value_release(v);
        }
    }
    return 1;
}

/* Elements whose class attribute has `name` as a token (live HTMLCollection). */
/* document.getElementsByName(name): every element whose name attribute matches.
 * Returned as a static NodeList (Chrome's is live; the callers use length/[i] or
 * [0], so a snapshot answers the same). */
int nodeGetElementsByName(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                          AntbValue* out, void*)
{
    WebCore::Node* root = nodeOf(self);
    std::vector<WebCore::Node*> matches;
    char* name;

    if (!root || argc < 1)
        return 1;

    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    {
        WebCore::String wanted = WebCore::String::fromUTF8(name);

        for (WebCore::Node* n = root; n; n = n->traverseNextNode(root)) {
            if (!n->isElementNode())
                continue;
            {
                WebCore::String value =
                    static_cast<WebCore::Element*>(n)->getAttribute(WebCore::String("name"));

                if (!value.isEmpty() && value == wanted)
                    matches.push_back(n);
            }
        }
    }
    antb_free(name);

    {
        AntbValue* list = wrapStaticCollection(engine, root, matches);

        if (!list)
            return 1;
        antb_value_assign(out, list);
        antb_value_release(list);
    }
    return 1;
}

int nodeGetElementsByClassName(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                               AntbValue* out, void*)
{
    WebCore::Node* root = nodeOf(self);
    char* name;
    AntbValue* list;

    if (!root || argc < 1)
        return 1;

    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    list = wrapCollection(engine, root, AgbCollectionClass, std::string(name));
    antb_free(name);

    if (!list)
        return 1;

    antb_value_assign(out, list);
    antb_value_release(list);
    return 1;
}

/* Selector support: descendant selectors made of #id, .class and tag parts
 * ("#s-top-bar .s-top-tabs", "div.foo", "a"). Anything fancier yields an
 * empty list, which is what most callers already handle. */
/* Collects the nodes matching a selector (the real selector engine,
 * AgbSelector.h: selector lists, combinators, attribute selectors and the
 * common pseudo-classes). Used by querySelector/querySelectorAll, which then
 * wrap the result as a static NodeList. */
static void selectorCollect(WebCore::Node* root, const char* selector,
                            std::vector<WebCore::Node*>& matches)
{
    std::vector<AgbSelector::Selector> selectors;
    bool fastPath;

    if (!root || !selector || !AgbSelector::parse(selector, selectors))
        return;

    fastPath = selectors.size() == 1 && selectors[0].steps.size() == 1
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
                    matches.push_back(n);
            }
        }
        return;
    }

    for (WebCore::Node* n = root->firstChild(); n; n = n->traverseNextNode(root)) {
        if (!n->isElementNode())
            continue;
        if (AgbSelector::matchesAny(selectors, static_cast<WebCore::Element*>(n)))
            matches.push_back(n);
    }
}

int nodeQuerySelectorAll(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                         AntbValue* out, void*)
{
    WebCore::Node* root = nodeOf(self);
    char* selector;
    std::vector<WebCore::Node*> matches;

    if (!root || argc < 1)
        return 1;

    selector = antb_to_utf8(args[0]);
    if (!selector)
        return 1;

    selectorCollect(root, selector, matches);
    antb_free(selector);

    {
        /* A static NodeList: later DOM changes must not show up here (the
         * snapshot's nodes are pinned by wrapNode, like a browser's list). */
        AntbValue* list = wrapStaticCollection(engine, root, matches);

        if (!list)
            return 1;
        antb_value_assign(out, list);
        antb_value_release(list);
    }
    return 1;
}

int nodeQuerySelector(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                      AntbValue* out, void*)
{
    WebCore::Node* root = nodeOf(self);
    char* selector;
    std::vector<WebCore::Node*> matches;

    if (!root || argc < 1)
        return 1;

    selector = antb_to_utf8(args[0]);
    if (!selector)
        return 1;

    selectorCollect(root, selector, matches);

    if (const char* qsFilter = getenv("AGB_QS_DEBUG")) {
        /* Diagnostic: AGB_QS_DEBUG=<substring> logs every querySelector
         * whose selector contains the substring, with the hit/miss. */
        std::string sel = selector;

        if (*qsFilter && sel.find(qsFilter) != std::string::npos)
            fprintf(stderr, "[qs] sel=%s found=%d\n", sel.c_str(), matches.empty() ? 0 : 1);
    }
    antb_free(selector);

    if (matches.empty()) {
        assignNull(engine, out);   /* null, not undefined (R95) */
        return 1;
    }

    {
        AntbValue* first = wrapNode(engine, matches[0]);

        if (first) {
            antb_value_assign(out, first);
            antb_value_release(first);
        }
    }
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

    /* cloneNode() copies the element tree, not the template's external content
     * fragment: copy that explicitly (issues #18 phase B). */
    if (deep && n->isElementNode() && copy->isElementNode() &&
        isTemplateElement(static_cast<WebCore::Element*>(n)))
        copyTemplateContents(static_cast<WebCore::Element*>(n),
                             static_cast<WebCore::Element*>(copy.get()));

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

/* args/env for window.__agbInfo(): the process the JS actually runs in (the
 * shell for the local backend, agave_render_host.exe for the process backend).
 * Exposed as data, like the rest of this diagnostic API; env is limited to the
 * AGB_* switches on purpose - the rest is the host's private environment. */
static void addInfoArgs(AntbEngine* engine, AntbValue* object, int* hasIpc)
{
    const char* raw = GetCommandLineA();
    AntbValue* array = antb_make_array(engine);
    char* copy;
    char* p;

    if (hasIpc)
        *hasIpc = 0;

    if (!array)
        return;

    copy = raw ? (char*)malloc(strlen(raw) + 1) : NULL;

    if (copy) {
        strcpy(copy, raw);
        p = copy;

        while (*p) {
            char* start;

            while (*p == ' ' || *p == '\t')
                ++p;
            if (!*p)
                break;

            if (*p == '"') {
                start = ++p;
                while (*p && *p != '"')
                    ++p;
            } else {
                start = p;
                while (*p && *p != ' ' && *p != '\t')
                    ++p;
            }

            if (*p)
                *p++ = 0;

            if (*start) {
                AntbValue* v;

                if (hasIpc && !strcmp(start, "--ipc"))
                    *hasIpc = 1;

                v = antb_make_string(engine, start);
                if (v) {
                    antb_array_push(engine, array, v);
                    antb_value_release(v);
                }
            }
        }

        free(copy);
    }

    antb_set_prop(engine, object, "args", array);
    antb_value_release(array);
}

/* The CRT environment (what getenv() sees), not GetEnvironmentStringsA(): the
 * shell/probe putenv the switches like AGB_USERDATA into the CRT copy, and those
 * are exactly the values a page needs for correlation. Only AGB_* is exposed -
 * the rest is the host's private environment. */
extern "C" char** _environ;

static void addInfoEnv(AntbEngine* engine, AntbValue* object)
{
    AntbValue* env = antb_make_object(engine);
    char** entry;

    if (!env)
        return;

    for (entry = _environ; entry && *entry; ++entry) {
        const char* text = *entry;
        const char* eq = strchr(text, '=');
        char name[128];
        size_t nameLen;

        if (!eq || eq == text)
            continue;

        nameLen = (size_t)(eq - text);

        if (nameLen < 4 || nameLen >= sizeof(name))
            continue;

        if (_strnicmp(text, "AGB_", 4) != 0)
            continue;

        memcpy(name, text, nameLen);
        name[nameLen] = 0;

        {
            AntbValue* v = antb_make_string(engine, eq + 1);

            if (v) {
                antb_set_prop(engine, env, name, v);
                antb_value_release(v);
            }
        }
    }

    antb_set_prop(engine, object, "env", env);
    antb_value_release(env);
}

/* window.__agbInfo(): the identity the [evt] timeline prints (pid, gen, realm,
 * doc, engine) plus the host's view/tab id and the realm slot. This is what
 * lets a page's own log line, the host log and the render backend's context
 * report name the same objects - a page can print `__agbInfo().realm` and grep
 * it in the [evt] lines. Values are numbers; pointers stay exact in a JS number
 * for 64-bit user addresses. A function (not a static object) on purpose: the
 * document/realm change on navigation. */
int globalInfo(AntbEngine* engine, AntbValue*, AntbValue**, int, AntbValue* out, void*)
{
    AntbValue* object;
    AntRuntime::ContextInfo info;
    int hasIpc = 0;

    if (!engine || !out)
        return 0;

    object = antb_make_object(engine);

    if (!object)
        return 0;

    if (!AntRuntime::contextInfo(g_currentState, &info)) {
        antb_value_release(object);
        return 0;
    }

    setNumberProp(engine, object, "pid", (double)info.pid);
    setNumberProp(engine, object, "view", (double)info.viewId);
    setNumberProp(engine, object, "engine", (double)info.engine);
    setNumberProp(engine, object, "realm", (double)info.realm);
    setNumberProp(engine, object, "document", (double)info.document);
    setNumberProp(engine, object, "realmGeneration", (double)info.realmGeneration);
    setNumberProp(engine, object, "documentGeneration", (double)info.documentGeneration);
    setNumberProp(engine, object, "slot", (double)info.slot);
    setNumberProp(engine, object, "mainFrame", info.mainFrame ? 1.0 : 0.0);
    setNumberProp(engine, object, "hidden", info.hidden ? 1.0 : 0.0);

    addInfoArgs(engine, object, &hasIpc);
    addInfoEnv(engine, object);
    {
        /* The process backend's renderer carries --ipc; a local engine runs
         * inside whatever process the host started. */
        AntbValue* backend = antb_make_string(engine, hasIpc ? "process" : "local");

        if (backend) {
            antb_set_prop(engine, object, "backend", backend);
            antb_value_release(backend);
        }
    }

    antb_value_assign(out, object);
    antb_value_release(object);
    return 1;
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

    /* A MessageEvent instance, not a plain object: receivers check
     * `e instanceof MessageEvent` / Object.prototype.toString. */
    event = makeEventObject("message", g_document, NULL);
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
    char* name = (argc > 1) ? antb_to_utf8(args[1]) : NULL;
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
        created = g_openWindowHook(resolved.empty() ? (url ? url : "") : resolved.c_str(),
                                   name ? name : "");

    antb_free(url);
    antb_free(name);

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
    AntbValue* list;

    if (!n || argc < 1)
        return 1;

    name = antb_to_utf8(args[0]);
    if (!name)
        return 1;

    list = wrapCollection(engine, n, AgbCollectionTags, std::string(name));
    antb_free(name);

    if (!list)
        return 1;

    antb_value_assign(out, list);
    antb_value_release(list);
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

    if (!engine || !ctx)
        return NULL;

    ensureCanvasContextClass(engine);
    if (!g_canvasCtxClass)
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

/* The TextTrack classes live in RuntimeState (g_textTrackClass & co): they are
 * per engine, and a second engine must not reuse (or overwrite) the first
 * engine's class objects. */

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

    g_textTrackClass = antb_class_create_iface(engine, "TextTrack", NULL);
    g_textTrackListClass = antb_class_create_iface(engine, "TextTrackList", NULL);
    g_textTrackCueClass = antb_class_create_iface(engine, "TextTrackCue", NULL);

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
        { AntbValue* v = antb_make_boolean(engine, media->paused() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "ended"))
        { AntbValue* v = antb_make_boolean(engine, media->ended() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "seeking"))
        { AntbValue* v = antb_make_boolean(engine, media->seeking() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "muted"))
        { AntbValue* v = antb_make_boolean(engine, media->muted() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "loop"))
        { AntbValue* v = antb_make_boolean(engine, media->loop() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "autoplay"))
        { AntbValue* v = antb_make_boolean(engine, media->autoplay() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "controls"))
        { AntbValue* v = antb_make_boolean(engine, media->controls() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "defaultMuted"))
        { AntbValue* v = antb_make_boolean(engine, media->defaultMuted() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }

    /* --- strings --- */
    if (!strcmp(name, "src"))
        { AntbValue* v = antb_make_string(engine, media->src().utf8().data()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
    if (!strcmp(name, "currentSrc"))
        { AntbValue* v = antb_make_string(engine, media->currentSrc().utf8().data()); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }

    /* Diagnostic (not a spec property): 1 when a real decoder drives this
     * element, 0 when the simulated ticker is the fallback (no ffmpeg).
     * tools/run_media_tests.ps1 uses it to tell the two apart. */
    if (!strcmp(name, "_agbRealPlayback"))
        { AntbValue* v = antb_make_boolean(engine, media->isRealPlayback() ? 1 : 0); if (!v) return true; antb_value_assign(out, v); antb_value_release(v); return true; }
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
    AntbClass* klass;

    if (!engine || !node)
        return NULL;

    ensureInterfaceClasses(engine);
    klass = classForNode(node);
    if (!klass)
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

    if (getenv("AGB_WRAP_TRACE"))
        agbLog(AGB_LOG_EVT, "wrap node=%p type=%d realm=%p doc=%p", (void*)node,
               node->nodeType(), (void*)antb_realm_current(engine), (void*)g_document);

    /* The interface class carries the prototype chain, the constructor and
     * @@toStringTag; the property table (nodeGetter/nodeSetter) is installed
     * once by the class, not per wrapper. */
    v = antb_wrap_native_as_owner(engine, klass, node, AGB_TAG_NODE, g_document);
    if (!v)
        return NULL;

    /* Remember which realm this wrapper belongs to. The wrapper cache already
     * keys by realm; attribute accessors need it because they run on the
     * engine-level prototype and would otherwise wrap child-document nodes
     * into the creating (main) realm. Set once: an existing wrapper keeps the
     * realm it was created in. */
    if (!antb_get_native(v, AGB_TAG_WRAPREALM))
        antb_set_native(v, (void*)antb_realm_current(engine), AGB_TAG_WRAPREALM);

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

/* ------------------------------------------------------------------------- */
/* DOM Selection / Range                                                     */
/* ------------------------------------------------------------------------- */
/* window.getSelection / document.getSelection / document.createRange /
 * document.execCommand.
 *
 * WebCore 2008 already implements the selection engine (SelectionController,
 * DOMSelection) and DOM Range; only the JS surface was missing. Without it
 * `window.getSelection` was undefined and every modern "copy" button - and
 * every editor/UI that reads or sets the selection - took its "unsupported"
 * branch (AgbGapJsdom recorded exactly that).
 *
 *   Selection - a live view over the frame's selection (like CSSStyleSheet):
 *               reading it after the caret moved returns the new state.
 *   Range     - a real WebCore::Range, ref'd and released with the document.
 *               The ant GC has no finalizers, so the document owns the refs
 *               (selectionReleaseRefs at document teardown). */

#define AGB_TAG_SELECTION 0x53454C31u  /* 'SEL1' */
#define AGB_TAG_RANGE     0x524E4731u  /* 'RNG1' */

/* g_selectionClass / g_rangeClass live in RuntimeState (per engine): the class
 * objects belong to the VM that created them. */

enum {
    SEL_OP_TOSTRING, SEL_OP_GET_RANGE_AT, SEL_OP_ADD_RANGE, SEL_OP_REMOVE_ALL,
    SEL_OP_COLLAPSE, SEL_OP_COLLAPSE_TO_START, SEL_OP_COLLAPSE_TO_END,
    SEL_OP_EXTEND, SEL_OP_SELECT_ALL_CHILDREN, SEL_OP_DELETE_FROM_DOCUMENT,
    SEL_OP_CONTAINS_NODE, SEL_OP_MODIFY, SEL_OP_SET_BASE_AND_EXTENT,
    SEL_OP_SET_POSITION
};

enum {
    RNG_OP_SET_START, RNG_OP_SET_END, RNG_OP_SET_START_BEFORE, RNG_OP_SET_START_AFTER,
    RNG_OP_SET_END_BEFORE, RNG_OP_SET_END_AFTER, RNG_OP_SELECT_NODE,
    RNG_OP_SELECT_NODE_CONTENTS, RNG_OP_COLLAPSE, RNG_OP_CLONE, RNG_OP_DETACH,
    RNG_OP_TOSTRING, RNG_OP_COMPARE_BOUNDARY_POINTS, RNG_OP_GET_BOUNDING_CLIENT_RECT,
    RNG_OP_GET_CLIENT_RECTS, RNG_OP_CREATE_CONTEXTUAL_FRAGMENT, RNG_OP_INSERT_NODE,
    RNG_OP_SURROUND_CONTENTS
};

static WebCore::DOMSelection* selectionNative(AntbValue* self)
{
    return self ? (WebCore::DOMSelection*)antb_get_native(self, AGB_TAG_SELECTION) : NULL;
}

static void ensureSelectionClasses();

static WebCore::Range* rangeNative(AntbValue* self)
{
    return self ? (WebCore::Range*)antb_get_native(self, AGB_TAG_RANGE) : NULL;
}

static void selectionAssignBool(AntbEngine* engine, AntbValue* out, bool value)
{
    AntbValue* v = antb_make_boolean(engine, value ? 1 : 0);

    if (!v)
        return;
    antb_value_assign(out, v);
    antb_value_release(v);
}

static void selectionAssignNumber(AntbEngine* engine, AntbValue* out, double value)
{
    AntbValue* v = antb_make_number(engine, value);

    if (!v)
        return;
    antb_value_assign(out, v);
    antb_value_release(v);
}

static void selectionAssignString(AntbEngine* engine, AntbValue* out, const WebCore::String& text)
{
    AntbValue* v = antb_make_string(engine, text.utf8().data());

    if (!v)
        return;
    antb_value_assign(out, v);
    antb_value_release(v);
}

/* A DOMRect-like plain object (left/top/right/bottom/width/height/x/y), the
 * same shape element.getBoundingClientRect() already produces. */
static AntbValue* selectionMakeRect(AntbEngine* engine, const WebCore::IntRect& rect)
{
    static const char* names[] = { "left", "top", "right", "bottom", "width", "height", "x", "y" };
    double values[8];
    AntbValue* obj;
    int i;

    values[0] = rect.x();
    values[1] = rect.y();
    values[2] = rect.x() + rect.width();
    values[3] = rect.y() + rect.height();
    values[4] = rect.width();
    values[5] = rect.height();
    values[6] = rect.x();
    values[7] = rect.y();

    obj = antb_make_object(engine);
    if (!obj)
        return NULL;

    for (i = 0; i < 8; ++i) {
        AntbValue* v = antb_make_number(engine, values[i]);

        if (v) {
            antb_set_prop(engine, obj, names[i], v);
            antb_value_release(v);
        }
    }

    return obj;
}

static void selectionTrackRange(WebCore::Range* range)
{
    DocumentRuntime* runtime;

    if (!range || !range->ownerDocument())
        return;

    runtime = documentRuntimeOf(g_currentState, range->ownerDocument());
    if (!runtime)
        return;

    if (!runtime->rangeRefs)
        runtime->rangeRefs = new std::vector<WebCore::Range*>();

    range->ref();
    runtime->rangeRefs->push_back(range);
}

/* Drops the selection/Range objects of a document that is going away. Called
 * from releaseDocumentRuntime (same place the CSSOM refs are dropped). */
void selectionReleaseRefs(DocumentRuntime* document)
{
    if (!document)
        return;

    if (document->selection) {
        /* detach from the Frame first: the controller may already be gone. */
        document->selection->disconnectFrame();
        document->selection->deref();
        document->selection = NULL;
    }

    if (document->rangeRefs) {
        size_t i;

        for (i = 0; i < document->rangeRefs->size(); ++i)
            (*document->rangeRefs)[i]->deref();

        delete document->rangeRefs;
        document->rangeRefs = NULL;
    }
}

static WebCore::DOMSelection* domSelectionForDocument(WebCore::Document* document)
{
    DocumentRuntime* runtime;
    WebCore::Frame* frame;

    if (!document || !g_currentState)
        return NULL;

    frame = document->frame();
    if (!frame)
        return NULL;

    runtime = documentRuntimeOf(g_currentState, document);
    if (!runtime)
        return NULL;

    if (!runtime->selection) {
        runtime->selection = new WebCore::DOMSelection(frame);
        runtime->selection->ref();
    }

    return runtime->selection;
}

static AntbValue* wrapSelection(AntbEngine* engine, WebCore::DOMSelection* selection)
{
    WebCore::Document* document;

    if (!selection || !selection->frame())
        return antb_make_null(engine);

    document = selection->frame()->document();
    if (!document)
        return antb_make_null(engine);

    ensureSelectionClasses();
    return antb_wrap_native_as_owner(engine, g_selectionClass, selection,
                                     AGB_TAG_SELECTION, document);
}

AntbValue* wrapRange(AntbEngine* engine, WebCore::Range* range)
{
    if (!range)
        return antb_make_null(engine);

    ensureSelectionClasses();

    selectionTrackRange(range);

    return antb_wrap_native_as_owner(engine, g_rangeClass, range, AGB_TAG_RANGE,
                                     range->ownerDocument());
}

/* --- document.getSelection / createRange / execCommand ------------------- */

int docGetSelection(AntbEngine* engine, AntbValue* self, AntbValue**, int,
                    AntbValue* out, void*)
{
    WebCore::Document* document = documentOf(self);
    WebCore::DOMSelection* selection = domSelectionForDocument(document);
    AntbValue* v = wrapSelection(engine, selection);

    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int docCreateRange(AntbEngine* engine, AntbValue* self, AntbValue**, int,
                   AntbValue* out, void*)
{
    WebCore::Document* document = documentOf(self);
    AntbValue* v;

    if (!document)
        return 0;

    v = wrapRange(engine, document->createRange().get());
    if (!v)
        return 0;

    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

int docExecCommand(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                   AntbValue* out, void*)
{
    WebCore::Document* document = documentOf(self);
    char* name = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
    bool done = false;

    if (document && document->frame() && name && name[0])
        done = document->frame()->editor()->execCommand(WebCore::AtomicString(name));

    antb_free(name);
    selectionAssignBool(engine, out, done);
    return 1;
}

int docQueryCommandSupported(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                             AntbValue* out, void*)
{
    WebCore::Document* document = documentOf(self);
    char* name = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
    bool supported = false;

    if (document && document->frame() && name && name[0])
        supported = document->frame()->editor()->commandIsSupported(WebCore::AtomicString(name));

    antb_free(name);
    selectionAssignBool(engine, out, supported);
    return 1;
}

int docQueryCommandEnabled(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void*)
{
    WebCore::Document* document = documentOf(self);
    char* name = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
    bool enabled = false;

    if (document && document->frame() && name && name[0])
        enabled = document->frame()->editor()->commandIsEnabled(WebCore::AtomicString(name));

    antb_free(name);
    selectionAssignBool(engine, out, enabled);
    return 1;
}

/* queryCommandState(): Chrome reports "is this style currently applied".
 * Editing styles are not implemented here, so the answer is always false -
 * which is also what Chrome returns for a page without the style. */
int docQueryCommandState(AntbEngine* engine, AntbValue*, AntbValue**, int,
                         AntbValue* out, void*)
{
    selectionAssignBool(engine, out, false);
    return 1;
}

/* --- Selection methods --------------------------------------------------- */

static int selectionGetter(AntbEngine* engine, AntbValue* self, const char* name,
                           AntbValue* out, void*)
{
    WebCore::DOMSelection* selection = selectionNative(self);

    if (!selection || !name) {
        static int selDebug = -1;

        if (selDebug < 0)
            selDebug = getenv("AGB_SEL_DEBUG") ? 1 : 0;
        if (selDebug)
            fprintf(stderr, "[seljs] selection getter %s on a receiver without native=%p (self=%p)\n",
                    name ? name : "(null)", (void*)selection, (void*)self);
        return 0;
    }

    if (!strcmp(name, "type"))
        { selectionAssignString(engine, out, selection->type()); return 1; }
    if (!strcmp(name, "rangeCount"))
        { selectionAssignNumber(engine, out, selection->rangeCount()); return 1; }
    if (!strcmp(name, "isCollapsed"))
        { selectionAssignBool(engine, out, selection->isCollapsed()); return 1; }

    if (!strcmp(name, "anchorNode") || !strcmp(name, "baseNode")) {
        WebCore::Node* node = selection->baseNode();

        if (!node) {
            static int selDebug = -1;

            if (selDebug < 0)
                selDebug = getenv("AGB_SEL_DEBUG") ? 1 : 0;
            if (selDebug)
                fprintf(stderr, "[seljs] %s: no node (rangeCount=%d state=%s)\n",
                        name, selection->rangeCount(),
                        selection->type().utf8().data());
            return assignNull(engine, out);
        }
        {
            AntbValue* v = wrapNode(engine, node);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
        return 1;
    }
    if (!strcmp(name, "focusNode") || !strcmp(name, "extentNode")) {
        WebCore::Node* node = selection->extentNode();

        if (!node) {
            static int selDebug = -1;

            if (selDebug < 0)
                selDebug = getenv("AGB_SEL_DEBUG") ? 1 : 0;
            if (selDebug)
                fprintf(stderr, "[seljs] %s: no node (rangeCount=%d state=%s)\n",
                        name, selection->rangeCount(),
                        selection->type().utf8().data());
            return assignNull(engine, out);
        }
        {
            AntbValue* v = wrapNode(engine, node);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
        return 1;
    }
    if (!strcmp(name, "anchorOffset") || !strcmp(name, "baseOffset"))
        { selectionAssignNumber(engine, out, selection->baseOffset()); return 1; }
    if (!strcmp(name, "focusOffset") || !strcmp(name, "extentOffset"))
        { selectionAssignNumber(engine, out, selection->extentOffset()); return 1; }

    return 0;
}

static int selectionMethod(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void* user)
{
    WebCore::DOMSelection* selection = selectionNative(self);
    long op = (long)(intptr_t)user;
    WebCore::ExceptionCode ec = 0;

    if (!selection)
        return 0;

    switch (op) {
    case SEL_OP_TOSTRING:
        selectionAssignString(engine, out, selection->toString());
        return 1;

    case SEL_OP_GET_RANGE_AT: {
        int index = (argc > 0 && args[0]) ? (int)antb_to_number(args[0]) : 0;
        PassRefPtr<WebCore::Range> range = selection->getRangeAt(index, ec);
        AntbValue* v;

        if (!range || ec)
            return assignNull(engine, out);

        v = wrapRange(engine, range.get());
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    case SEL_OP_ADD_RANGE:
        if (argc > 0 && args[0])
            selection->addRange(rangeNative(args[0]));
        return 1;

    case SEL_OP_REMOVE_ALL:
        selection->removeAllRanges();
        return 1;

    case SEL_OP_COLLAPSE: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;
        int offset = (argc > 1 && args[1]) ? (int)antb_to_number(args[1]) : 0;

        if (node)
            selection->collapse(node, offset, ec);
        return 1;
    }

    case SEL_OP_COLLAPSE_TO_START:
        selection->collapseToStart();
        return 1;

    case SEL_OP_COLLAPSE_TO_END:
        selection->collapseToEnd();
        return 1;

    case SEL_OP_EXTEND: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;
        int offset = (argc > 1 && args[1]) ? (int)antb_to_number(args[1]) : 0;

        if (node)
            selection->setBaseAndExtent(selection->baseNode(), selection->baseOffset(),
                                        node, offset, ec);
        return 1;
    }

    case SEL_OP_SELECT_ALL_CHILDREN: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;

        if (!node || !node->document())
            return 1;
        {
            RefPtr<WebCore::Range> range = node->document()->createRange();

            range->selectNodeContents(node, ec);
            if (!ec) {
                selection->removeAllRanges();
                selection->addRange(range.get());
            }
        }
        return 1;
    }

    case SEL_OP_DELETE_FROM_DOCUMENT: {
        RefPtr<WebCore::Range> range = selection->getRangeAt(0, ec);

        if (range && !ec) {
            WebCore::Node* start = range->startContainer(ec);
            int offset = range->startOffset(ec);

            ec = 0;
            range->deleteContents(ec);
            if (start)
                selection->collapse(start, offset, ec);
        }
        return 1;
    }

    case SEL_OP_CONTAINS_NODE: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;
        bool allowPartial = (argc > 1 && args[1]) ? (antb_to_boolean(args[1]) != 0) : false;
        RefPtr<WebCore::Range> range = selection->getRangeAt(0, ec);
        bool contains = false;

        if (node && range && !ec) {
            if (allowPartial) {
                contains = range->intersectsNode(node, ec);
            } else {
                RefPtr<WebCore::Range> nodeRange = node->document()->createRange();

                nodeRange->selectNode(node, ec);

                if (!ec)
                    contains = range->compareBoundaryPoints(WebCore::Range::START_TO_START,
                                                            nodeRange.get(), ec) <= 0
                        && range->compareBoundaryPoints(WebCore::Range::END_TO_END,
                                                        nodeRange.get(), ec) >= 0;
            }
        }
        selectionAssignBool(engine, out, contains);
        return 1;
    }

    case SEL_OP_MODIFY: {
        char* alter = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
        char* direction = (argc > 1 && args[1]) ? antb_to_utf8(args[1]) : NULL;
        char* granularity = (argc > 2 && args[2]) ? antb_to_utf8(args[2]) : NULL;

        if (alter && direction && granularity)
            selection->modify(WebCore::String::fromUTF8(alter),
                              WebCore::String::fromUTF8(direction),
                              WebCore::String::fromUTF8(granularity));

        antb_free(alter);
        antb_free(direction);
        antb_free(granularity);
        return 1;
    }

    case SEL_OP_SET_BASE_AND_EXTENT: {
        WebCore::Node* base = (argc > 0) ? nodeOf(args[0]) : 0;
        int baseOffset = (argc > 1 && args[1]) ? (int)antb_to_number(args[1]) : 0;
        WebCore::Node* extent = (argc > 2) ? nodeOf(args[2]) : 0;
        int extentOffset = (argc > 3 && args[3]) ? (int)antb_to_number(args[3]) : 0;

        if (base && extent)
            selection->setBaseAndExtent(base, baseOffset, extent, extentOffset, ec);
        return 1;
    }

    case SEL_OP_SET_POSITION: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;
        int offset = (argc > 1 && args[1]) ? (int)antb_to_number(args[1]) : 0;

        if (node)
            selection->setPosition(node, offset, ec);
        return 1;
    }

    default:
        return 0;
    }
}

/* --- Range methods ------------------------------------------------------- */

static int rangeGetter(AntbEngine* engine, AntbValue* self, const char* name,
                       AntbValue* out, void*)
{
    WebCore::Range* range = rangeNative(self);
    WebCore::ExceptionCode ec = 0;

    if (!range || !name)
        return 0;

    if (!strcmp(name, "startContainer")) {
        WebCore::Node* node = range->startContainer(ec);

        if (!node)
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, node);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
        return 1;
    }
    if (!strcmp(name, "endContainer")) {
        WebCore::Node* node = range->endContainer(ec);

        if (!node)
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, node);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
        return 1;
    }
    if (!strcmp(name, "commonAncestorContainer")) {
        WebCore::Node* node = range->commonAncestorContainer(ec);

        if (!node)
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, node);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
        return 1;
    }
    if (!strcmp(name, "startOffset"))
        { selectionAssignNumber(engine, out, range->startOffset(ec)); return 1; }
    if (!strcmp(name, "endOffset"))
        { selectionAssignNumber(engine, out, range->endOffset(ec)); return 1; }
    if (!strcmp(name, "collapsed"))
        { selectionAssignBool(engine, out, range->collapsed(ec)); return 1; }
    if (!strcmp(name, "detached"))
        { selectionAssignBool(engine, out, range->isDetached()); return 1; }

    return 0;
}

static int rangeMethod(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                       AntbValue* out, void* user)
{
    WebCore::Range* range = rangeNative(self);
    long op = (long)(intptr_t)user;
    WebCore::ExceptionCode ec = 0;

    if (!range)
        return 0;

    switch (op) {
    case RNG_OP_SET_START:
    case RNG_OP_SET_END: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;
        int offset = (argc > 1 && args[1]) ? (int)antb_to_number(args[1]) : 0;

        if (!node)
            return 1;
        if (op == RNG_OP_SET_START)
            range->setStart(node, offset, ec);
        else
            range->setEnd(node, offset, ec);
        return 1;
    }

    case RNG_OP_SET_START_BEFORE:
    case RNG_OP_SET_START_AFTER:
    case RNG_OP_SET_END_BEFORE:
    case RNG_OP_SET_END_AFTER: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;

        if (!node)
            return 1;
        if (op == RNG_OP_SET_START_BEFORE)
            range->setStartBefore(node, ec);
        else if (op == RNG_OP_SET_START_AFTER)
            range->setStartAfter(node, ec);
        else if (op == RNG_OP_SET_END_BEFORE)
            range->setEndBefore(node, ec);
        else
            range->setEndAfter(node, ec);
        return 1;
    }

    case RNG_OP_SELECT_NODE:
    case RNG_OP_SELECT_NODE_CONTENTS: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;

        if (!node)
            return 1;
        if (op == RNG_OP_SELECT_NODE)
            range->selectNode(node, ec);
        else
            range->selectNodeContents(node, ec);
        return 1;
    }

    case RNG_OP_COLLAPSE: {
        bool toStart = (argc > 0 && args[0]) ? (antb_to_boolean(args[0]) != 0) : false;

        range->collapse(toStart, ec);
        return 1;
    }

    case RNG_OP_CLONE: {
        PassRefPtr<WebCore::Range> clone = range->cloneRange(ec);
        AntbValue* v;

        if (!clone || ec)
            return assignNull(engine, out);
        v = wrapRange(engine, clone.get());
        if (!v)
            return 0;
        antb_value_assign(out, v);
        antb_value_release(v);
        return 1;
    }

    case RNG_OP_DETACH:
        range->detach(ec);
        return 1;

    case RNG_OP_TOSTRING:
        selectionAssignString(engine, out, range->toString(ec));
        return 1;

    case RNG_OP_COMPARE_BOUNDARY_POINTS: {
        int how = (argc > 0 && args[0]) ? (int)antb_to_number(args[0]) : 0;
        WebCore::Range* source = (argc > 1) ? rangeNative(args[1]) : 0;

        if (!source || how < 0 || how > 3)
            { selectionAssignNumber(engine, out, 0); return 1; }
        selectionAssignNumber(engine, out,
                              range->compareBoundaryPoints((WebCore::Range::CompareHow)how, source, ec));
        return 1;
    }

    case RNG_OP_GET_BOUNDING_CLIENT_RECT: {
        /* Chrome reports the line-box rect of a text range (a 16px font on a
         * 24px line gives height 24 and the line's top), not the glyph box;
         * Range::boundingBox() is the latter. Use the selection-height rects so
         * a popover anchored to a selection lands where Chrome puts it. */
        WTF::Vector<WebCore::IntRect> rects;
        WebCore::IntRect rect;
        WebCore::Document* document = range->ownerDocument();

        range->addLineBoxRects(rects, true);
        for (size_t i = 0; i < rects.size(); ++i)
            rect.unite(rects[i]);

        /* ...and in viewport coordinates (see node.getBoundingClientRect). */
        if (document && document->view())
            rect.move(-document->view()->contentsX(), -document->view()->contentsY());
        {
            AntbValue* v = selectionMakeRect(engine, rect);

            if (v) {
                antb_value_assign(out, v);
                antb_value_release(v);
            }
        }
        return 1;
    }

    case RNG_OP_GET_CLIENT_RECTS: {
        WebCore::Document* document = range->ownerDocument();
        WTF::Vector<WebCore::IntRect> rects;
        AntbValue* array;
        size_t i;

        range->addLineBoxRects(rects, false);

        array = antb_make_array(engine);
        if (!array)
            return 0;

        for (i = 0; i < rects.size(); ++i) {
            WebCore::IntRect rect = rects[i];

            if (document && document->view())
                rect.move(-document->view()->contentsX(), -document->view()->contentsY());
            {
                AntbValue* item = selectionMakeRect(engine, rect);

                if (item) {
                    antb_array_push(engine, array, item);
                    antb_value_release(item);
                }
            }
        }

        antb_value_assign(out, array);
        antb_value_release(array);
        return 1;
    }

    case RNG_OP_CREATE_CONTEXTUAL_FRAGMENT: {
        char* html = (argc > 0 && args[0]) ? antb_to_utf8(args[0]) : NULL;
        PassRefPtr<WebCore::DocumentFragment> fragment;

        if (html)
            fragment = range->createContextualFragment(WebCore::String::fromUTF8(html), ec);

        antb_free(html);

        if (!fragment || ec)
            return assignNull(engine, out);
        {
            AntbValue* v = wrapNode(engine, fragment.get());

            if (!v)
                return 0;
            antb_value_assign(out, v);
            antb_value_release(v);
        }
        return 1;
    }

    case RNG_OP_INSERT_NODE: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;

        if (node)
            range->insertNode(node, ec);
        return 1;
    }

    case RNG_OP_SURROUND_CONTENTS: {
        WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : 0;

        if (node)
            range->surroundContents(node, ec);
        return 1;
    }

    default:
        return 0;
    }
}

static void ensureSelectionClasses()
{
    if (!g_engine || g_selectionClass)
        return;

    g_selectionClass = antb_class_create_iface(g_engine, "Selection", NULL);
    if (g_selectionClass) {
        antb_class_set_property(g_engine, g_selectionClass, "", selectionGetter, NULL);
        antb_class_add_method(g_engine, g_selectionClass, "toString", selectionMethod, (void*)(intptr_t)SEL_OP_TOSTRING);
        antb_class_add_method(g_engine, g_selectionClass, "getRangeAt", selectionMethod, (void*)(intptr_t)SEL_OP_GET_RANGE_AT);
        antb_class_add_method(g_engine, g_selectionClass, "addRange", selectionMethod, (void*)(intptr_t)SEL_OP_ADD_RANGE);
        antb_class_add_method(g_engine, g_selectionClass, "removeAllRanges", selectionMethod, (void*)(intptr_t)SEL_OP_REMOVE_ALL);
        antb_class_add_method(g_engine, g_selectionClass, "empty", selectionMethod, (void*)(intptr_t)SEL_OP_REMOVE_ALL);
        antb_class_add_method(g_engine, g_selectionClass, "collapse", selectionMethod, (void*)(intptr_t)SEL_OP_COLLAPSE);
        antb_class_add_method(g_engine, g_selectionClass, "collapseToStart", selectionMethod, (void*)(intptr_t)SEL_OP_COLLAPSE_TO_START);
        antb_class_add_method(g_engine, g_selectionClass, "collapseToEnd", selectionMethod, (void*)(intptr_t)SEL_OP_COLLAPSE_TO_END);
        antb_class_add_method(g_engine, g_selectionClass, "extend", selectionMethod, (void*)(intptr_t)SEL_OP_EXTEND);
        antb_class_add_method(g_engine, g_selectionClass, "selectAllChildren", selectionMethod, (void*)(intptr_t)SEL_OP_SELECT_ALL_CHILDREN);
        antb_class_add_method(g_engine, g_selectionClass, "deleteFromDocument", selectionMethod, (void*)(intptr_t)SEL_OP_DELETE_FROM_DOCUMENT);
        antb_class_add_method(g_engine, g_selectionClass, "containsNode", selectionMethod, (void*)(intptr_t)SEL_OP_CONTAINS_NODE);
        antb_class_add_method(g_engine, g_selectionClass, "modify", selectionMethod, (void*)(intptr_t)SEL_OP_MODIFY);
        antb_class_add_method(g_engine, g_selectionClass, "setBaseAndExtent", selectionMethod, (void*)(intptr_t)SEL_OP_SET_BASE_AND_EXTENT);
        antb_class_add_method(g_engine, g_selectionClass, "setPosition", selectionMethod, (void*)(intptr_t)SEL_OP_SET_POSITION);
    }

    g_rangeClass = antb_class_create_iface(g_engine, "Range", NULL);
    if (g_rangeClass) {
        antb_class_set_property(g_engine, g_rangeClass, "", rangeGetter, NULL);
        antb_class_add_method(g_engine, g_rangeClass, "setStart", rangeMethod, (void*)(intptr_t)RNG_OP_SET_START);
        antb_class_add_method(g_engine, g_rangeClass, "setEnd", rangeMethod, (void*)(intptr_t)RNG_OP_SET_END);
        antb_class_add_method(g_engine, g_rangeClass, "setStartBefore", rangeMethod, (void*)(intptr_t)RNG_OP_SET_START_BEFORE);
        antb_class_add_method(g_engine, g_rangeClass, "setStartAfter", rangeMethod, (void*)(intptr_t)RNG_OP_SET_START_AFTER);
        antb_class_add_method(g_engine, g_rangeClass, "setEndBefore", rangeMethod, (void*)(intptr_t)RNG_OP_SET_END_BEFORE);
        antb_class_add_method(g_engine, g_rangeClass, "setEndAfter", rangeMethod, (void*)(intptr_t)RNG_OP_SET_END_AFTER);
        antb_class_add_method(g_engine, g_rangeClass, "selectNode", rangeMethod, (void*)(intptr_t)RNG_OP_SELECT_NODE);
        antb_class_add_method(g_engine, g_rangeClass, "selectNodeContents", rangeMethod, (void*)(intptr_t)RNG_OP_SELECT_NODE_CONTENTS);
        antb_class_add_method(g_engine, g_rangeClass, "collapse", rangeMethod, (void*)(intptr_t)RNG_OP_COLLAPSE);
        antb_class_add_method(g_engine, g_rangeClass, "cloneRange", rangeMethod, (void*)(intptr_t)RNG_OP_CLONE);
        antb_class_add_method(g_engine, g_rangeClass, "detach", rangeMethod, (void*)(intptr_t)RNG_OP_DETACH);
        antb_class_add_method(g_engine, g_rangeClass, "toString", rangeMethod, (void*)(intptr_t)RNG_OP_TOSTRING);
        antb_class_add_method(g_engine, g_rangeClass, "compareBoundaryPoints", rangeMethod, (void*)(intptr_t)RNG_OP_COMPARE_BOUNDARY_POINTS);
        antb_class_add_method(g_engine, g_rangeClass, "getBoundingClientRect", rangeMethod, (void*)(intptr_t)RNG_OP_GET_BOUNDING_CLIENT_RECT);
        antb_class_add_method(g_engine, g_rangeClass, "getClientRects", rangeMethod, (void*)(intptr_t)RNG_OP_GET_CLIENT_RECTS);
        antb_class_add_method(g_engine, g_rangeClass, "createContextualFragment", rangeMethod, (void*)(intptr_t)RNG_OP_CREATE_CONTEXTUAL_FRAGMENT);
        antb_class_add_method(g_engine, g_rangeClass, "insertNode", rangeMethod, (void*)(intptr_t)RNG_OP_INSERT_NODE);
        antb_class_add_method(g_engine, g_rangeClass, "surroundContents", rangeMethod, (void*)(intptr_t)RNG_OP_SURROUND_CONTENTS);
    }
}


/* ------------------------------------------------------------------------- */
/* DOM interfaces: one bridge class per interface                             */
/* ------------------------------------------------------------------------- */
/* Historically every node was wrapped as a single "Node" class whose prototype
 * object had neither a parent nor a constructor. That made instanceof only
 * aware of the wrapping class, Object.getPrototypeOf(el) return a hidden
 * object instead of HTMLElement.prototype, el.constructor empty and
 * Object.prototype.toString.call(el) "[object Object]".
 *
 * The facade (nodeGetter/nodeSetter, titleGetter/titleSetter and the CSSOM
 * getters) is still the custom layer. These classes only carry what the IDL
 * supplies: the prototype chain, the constructor objects (with their parent
 * as [[Prototype]]), @@toStringTag and the static constants. Attribute
 * accessors stay in the facade until the generated bindings replace them.
 *
 * The data below is the source of truth for the class set. "parent" is
 * resolved by name; the order must create parents before children. */
struct DomIfaceDef {
    const char* name;
    const char* parent;   /* NULL: the prototype inherits from Object.prototype */
};

static const DomIfaceDef kDomIfaces[] = {
    { "Window",                 NULL },
    { "DocumentFragment",       "Node" },
    { "CharacterData",          "Node" },
    { "Text",                   "CharacterData" },
    { "Comment",                "CharacterData" },
    { "Document",               "Node" },
    { "HTMLDocument",           "Document" },
    { "NamedNodeMap",           NULL },
    { "ShadowRoot",             "DocumentFragment" },
    { "TreeWalker",             NULL },
    { "HTMLTemplateElement",    "HTMLElement" },
    { "Attr",                   "Node" },
    { "Element",                "Node" },
    { "SVGElement",             "Element" },
    { "SVGSVGElement",          "SVGElement" },
    { "HTMLElement",            "Element" },
    { "HTMLUnknownElement",     "HTMLElement" },
    { "HTMLAnchorElement",      "HTMLElement" },
    { "HTMLAppletElement",      "HTMLElement" },
    { "HTMLAreaElement",        "HTMLElement" },
    { "HTMLBRElement",          "HTMLElement" },
    { "HTMLBaseElement",        "HTMLElement" },
    { "HTMLBaseFontElement",    "HTMLElement" },
    { "HTMLBodyElement",        "HTMLElement" },
    { "HTMLButtonElement",      "HTMLElement" },
    { "HTMLCanvasElement",      "HTMLElement" },
    { "HTMLDListElement",       "HTMLElement" },
    { "HTMLDataElement",        "HTMLElement" },
    { "HTMLDataListElement",    "HTMLElement" },
    { "HTMLDetailsElement",     "HTMLElement" },
    { "HTMLDialogElement",      "HTMLElement" },
    { "HTMLDirectoryElement",   "HTMLElement" },
    { "HTMLDivElement",         "HTMLElement" },
    { "HTMLEmbedElement",       "HTMLElement" },
    { "HTMLFieldSetElement",    "HTMLElement" },
    { "HTMLFontElement",        "HTMLElement" },
    { "HTMLFormElement",        "HTMLElement" },
    { "HTMLFrameElement",       "HTMLElement" },
    { "HTMLFrameSetElement",    "HTMLElement" },
    { "HTMLHRElement",          "HTMLElement" },
    { "HTMLHeadElement",        "HTMLElement" },
    { "HTMLHeadingElement",     "HTMLElement" },
    { "HTMLHtmlElement",        "HTMLElement" },
    { "HTMLIFrameElement",      "HTMLElement" },
    { "HTMLImageElement",       "HTMLElement" },
    { "HTMLInputElement",       "HTMLElement" },
    { "HTMLIsIndexElement",     "HTMLElement" },
    { "HTMLLIElement",          "HTMLElement" },
    { "HTMLLabelElement",       "HTMLElement" },
    { "HTMLLegendElement",      "HTMLElement" },
    { "HTMLLinkElement",        "HTMLElement" },
    { "HTMLMapElement",         "HTMLElement" },
    { "HTMLMarqueeElement",     "HTMLElement" },
    { "HTMLMediaElement",       "HTMLElement" },
    { "HTMLAudioElement",       "HTMLMediaElement" },
    { "HTMLVideoElement",       "HTMLMediaElement" },
    { "HTMLMenuElement",        "HTMLElement" },
    { "HTMLMetaElement",        "HTMLElement" },
    { "HTMLMeterElement",       "HTMLElement" },
    { "HTMLModElement",         "HTMLElement" },
    { "HTMLOListElement",       "HTMLElement" },
    { "HTMLObjectElement",      "HTMLElement" },
    { "HTMLOptGroupElement",    "HTMLElement" },
    { "HTMLOptionElement",      "HTMLElement" },
    { "HTMLOutputElement",      "HTMLElement" },
    { "HTMLParagraphElement",   "HTMLElement" },
    { "HTMLParamElement",       "HTMLElement" },
    { "HTMLPictureElement",     "HTMLElement" },
    { "HTMLPreElement",         "HTMLElement" },
    { "HTMLProgressElement",    "HTMLElement" },
    { "HTMLQuoteElement",       "HTMLElement" },
    { "HTMLScriptElement",      "HTMLElement" },
    { "HTMLSelectElement",      "HTMLElement" },
    { "HTMLSlotElement",        "HTMLElement" },
    { "HTMLSourceElement",      "HTMLElement" },
    { "HTMLSpanElement",        "HTMLElement" },
    { "HTMLStyleElement",       "HTMLElement" },
    { "HTMLTableCaptionElement", "HTMLElement" },
    { "HTMLTableCellElement",   "HTMLElement" },
    { "HTMLTableColElement",    "HTMLElement" },
    { "HTMLTableElement",       "HTMLElement" },
    { "HTMLTableRowElement",    "HTMLElement" },
    { "HTMLTableSectionElement", "HTMLElement" },
    { "HTMLTemplateElement",    "HTMLElement" },
    { "HTMLTextAreaElement",    "HTMLElement" },
    { "HTMLTimeElement",        "HTMLElement" },
    { "HTMLTitleElement",       "HTMLElement" },
    { "HTMLTrackElement",       "HTMLElement" },
    { "HTMLUListElement",       "HTMLElement" },
    /* Collections (B2-2): live list wrappers; querySelectorAll returns a
     * static NodeList of these. */
    { "NodeList",               NULL },
    { "HTMLCollection",         NULL }
};

/* HTML tag -> interface. The names are lower case; WebCore reports HTML tag
 * names upper case, so classForNode() lower-cases before the lookup. A tag that
 * is not listed and has no dash is an HTMLUnknownElement (custom elements with
 * a dash are plain HTMLElement, as in the modern DOM). */
struct DomTagDef {
    const char* tag;
    const char* iface;
};

static const DomTagDef kDomTags[] = {
    { "a",          "HTMLAnchorElement" },
    { "applet",     "HTMLAppletElement" },
    { "area",       "HTMLAreaElement" },
    { "audio",      "HTMLAudioElement" },
    { "base",       "HTMLBaseElement" },
    { "basefont",   "HTMLBaseFontElement" },
    { "blockquote", "HTMLQuoteElement" },
    { "body",       "HTMLBodyElement" },
    { "br",         "HTMLBRElement" },
    { "button",     "HTMLButtonElement" },
    { "canvas",     "HTMLCanvasElement" },
    { "caption",    "HTMLTableCaptionElement" },
    { "col",        "HTMLTableColElement" },
    { "colgroup",   "HTMLTableColElement" },
    { "data",       "HTMLDataElement" },
    { "datalist",   "HTMLDataListElement" },
    { "del",        "HTMLModElement" },
    { "details",    "HTMLDetailsElement" },
    { "dialog",     "HTMLDialogElement" },
    { "dir",        "HTMLDirectoryElement" },
    { "div",        "HTMLDivElement" },
    { "dl",         "HTMLDListElement" },
    { "embed",      "HTMLEmbedElement" },
    { "fieldset",   "HTMLFieldSetElement" },
    { "font",       "HTMLFontElement" },
    { "form",       "HTMLFormElement" },
    { "frame",      "HTMLFrameElement" },
    { "frameset",   "HTMLFrameSetElement" },
    { "h1",         "HTMLHeadingElement" },
    { "h2",         "HTMLHeadingElement" },
    { "h3",         "HTMLHeadingElement" },
    { "h4",         "HTMLHeadingElement" },
    { "h5",         "HTMLHeadingElement" },
    { "h6",         "HTMLHeadingElement" },
    { "head",       "HTMLHeadElement" },
    { "hr",         "HTMLHRElement" },
    { "html",       "HTMLHtmlElement" },
    { "iframe",     "HTMLIFrameElement" },
    { "img",        "HTMLImageElement" },
    { "input",      "HTMLInputElement" },
    { "ins",        "HTMLModElement" },
    { "isindex",    "HTMLIsIndexElement" },
    { "label",      "HTMLLabelElement" },
    { "legend",     "HTMLLegendElement" },
    { "li",         "HTMLLIElement" },
    { "link",       "HTMLLinkElement" },
    { "map",        "HTMLMapElement" },
    { "marquee",    "HTMLMarqueeElement" },
    { "menu",       "HTMLMenuElement" },
    { "meta",       "HTMLMetaElement" },
    { "meter",      "HTMLMeterElement" },
    { "object",     "HTMLObjectElement" },
    { "ol",         "HTMLOListElement" },
    { "optgroup",   "HTMLOptGroupElement" },
    { "option",     "HTMLOptionElement" },
    { "output",     "HTMLOutputElement" },
    { "p",          "HTMLParagraphElement" },
    { "param",      "HTMLParamElement" },
    { "picture",    "HTMLPictureElement" },
    { "pre",        "HTMLPreElement" },
    { "progress",   "HTMLProgressElement" },
    { "q",          "HTMLQuoteElement" },
    { "script",     "HTMLScriptElement" },
    { "select",     "HTMLSelectElement" },
    { "slot",       "HTMLSlotElement" },
    { "source",     "HTMLSourceElement" },
    { "span",       "HTMLSpanElement" },
    { "style",      "HTMLStyleElement" },
    { "table",      "HTMLTableElement" },
    { "tbody",      "HTMLTableSectionElement" },
    { "td",         "HTMLTableCellElement" },
    { "template",   "HTMLTemplateElement" },
    { "textarea",   "HTMLTextAreaElement" },
    { "tfoot",      "HTMLTableSectionElement" },
    { "th",         "HTMLTableCellElement" },
    { "thead",      "HTMLTableSectionElement" },
    { "time",       "HTMLTimeElement" },
    { "title",      "HTMLTitleElement" },
    { "tr",         "HTMLTableRowElement" },
    { "track",      "HTMLTrackElement" },
    { "ul",         "HTMLUListElement" },
    { "video",      "HTMLVideoElement" }
};

/* Method ownership follows the IDL (and Chrome): Node has the tree operations,
 * Element the attribute/selector ones, HTMLElement focus/click, and the form/
 * canvas/media classes their own. The implementations are the same callbacks
 * the facade used to register on the single Node class; they are tag-checked
 * internally where needed. */
struct DomMethodDef {
    const char* iface;
    const char* name;
    AntbNativeFn fn;
    void* user;
};

/* Method implementations by name.
 *
 * The generated tables decide *which* interface owns a method (and, where the
 * 2008 IDL disagrees with Chrome, the prototype it is registered on - see
 * @kMethodOwnershipMoves in CodeGeneratorAnt.pm). This table decides *how* it is
 * implemented: one dispatcher per name, because every dispatcher checks the
 * receiver itself (a document load is not the media one) and both answer the
 * same name.
 *
 * An IDL method with no row here is not registered at all - AGB_BINDING_CHECK=1
 * lists them, which is the honest "IDL knows it, the engine does not" inventory.
 */
struct DomMethodImpl {
    const char*  name;
    AntbNativeFn fn;
};

static const DomMethodImpl kDomMethodImpls[] = {
    { "addEventListener"          , nodeAddEventListener },
    { "addTextTrack"              , nodeMediaAddTextTrack },
    { "adoptNode"                 , nodeDocumentAdoptNode },
    { "after"                     , nodeAfter },
    { "append"                    , nodeAppendChildren },
    { "appendChild"               , nodeAppendChild },
    { "assignedNodes"             , nodeAssignedNodes },
    { "assignedElements"          , nodeAssignedElements },
    { "before"                    , nodeBefore },
    { "blur"                      , nodeBlur },
    { "canPlayType"               , nodeMediaCanPlayType },
    { "checkValidity"             , nodeFormCheckValidity },
    { "click"                     , nodeClick },
    { "cloneNode"                 , nodeCloneNode },
    { "compareDocumentPosition"   , nodeCompareDocumentPosition },
    { "contains"                  , nodeContains },
    { "createComment"             , docCreateComment },
    { "createDocumentFragment"    , docCreateDocumentFragment },
    { "createElement"             , docCreateElement },
    { "createElementNS"           , docCreateElementNS },
    { "createHTMLDocument"        , docCreateHTMLDocument },
    { "createRange"               , docCreateRange },
    { "createTextNode"            , docCreateTextNode },
    { "createTreeWalker"          , docCreateTreeWalker },
    { "dispatchEvent"             , nodeDispatchEvent },
    { "elementFromPoint"          , nodeDocumentElementFromPoint },
    { "execCommand"               , docExecCommand },
    { "focus"                     , nodeFocus },
    { "getAttribute"              , nodeGetAttribute },
    { "getAttributeNames"         , nodeGetAttributeNames },
    { "getAttributeNode"          , nodeGetAttributeNode },
    { "getAttributeNS"            , nodeGetAttributeNS },
    { "setAttributeNS"            , nodeSetAttributeNS },
    { "getClientRects"            , nodeGetClientRects },
    { "getNamedItem"              , namedMapGetNamedItemFn },
    { "getNamedItemNS"            , namedMapGetNamedItemNS },
    { "setNamedItem"              , namedMapSetNamedItem },
    { "setNamedItemNS"            , namedMapSetNamedItemNS },
    { "removeNamedItem"           , namedMapRemoveNamedItem },
    { "removeNamedItemNS"         , namedMapRemoveNamedItemNS },
    { "getBoundingClientRect"     , nodeGetBoundingClientRect },
    { "getContext"                , nodeGetContext },
    { "getElementById"            , docGetElementById },
    { "getElementsByClassName"    , nodeGetElementsByClassName },
    { "getElementsByName"         , nodeGetElementsByName },
    { "getElementsByTagName"      , nodeGetElementsByTagName },
    { "getRootNode"               , nodeGetRootNode },
    { "getSelection"              , docGetSelection },
    { "hasAttribute"              , nodeHasAttribute },
    { "hasChildNodes"             , nodeHasChildNodes },
    { "hasFocus"                  , nodeDocumentHasFocus },
    { "importNode"                , nodeDocumentImportNode },
    { "insertAdjacentElement"     , nodeInsertAdjacentElement },
    { "insertAdjacentHTML"        , nodeInsertAdjacentHTML },
    { "insertAdjacentText"        , nodeInsertAdjacentText },
    { "insertBefore"              , nodeInsertBefore },
    { "item"                      , collectionItem },
    { "load"                      , nodeMediaLoad },
    { "namedItem"                 , collectionNamedItemMethod },
    { "pause"                     , nodeMediaPause },
    { "play"                      , nodeMediaPlay },
    { "queryCommandEnabled"       , docQueryCommandEnabled },
    { "queryCommandState"         , docQueryCommandState },
    { "queryCommandSupported"     , docQueryCommandSupported },
    { "querySelector"             , nodeQuerySelector },
    { "prepend"                   , nodePrependChildren },
    { "querySelectorAll"          , nodeQuerySelectorAll },
    { "replaceWith"               , nodeReplaceWith },
    { "remove"                    , nodeRemove },
    { "removeAttribute"           , nodeRemoveAttribute },
    { "removeChild"               , nodeRemoveChild },
    { "removeEventListener"       , nodeRemoveEventListener },
    { "replaceChild"              , nodeReplaceChild },
    { "replaceChildren"           , nodeReplaceChildren },
    { "reset"                     , nodeFormReset },
    { "scrollIntoView"            , nodeScrollIntoView },
    { "setAttribute"              , nodeSetAttribute },
    { "submit"                    , nodeFormSubmit },
    { "toDataURL"                 , nodeToDataURL },
    { "toggleAttribute"           , nodeToggleAttribute },
    { "write"                     , docWrite },
    { "writeln"                   , docWriteln },
};

static AntbNativeFn findMethodImpl(const char* name)
{
    size_t i;

    for (i = 0; i < sizeof(kDomMethodImpls) / sizeof(kDomMethodImpls[0]); ++i) {
        if (!strcmp(kDomMethodImpls[i].name, name))
            return kDomMethodImpls[i].fn;
    }
    return 0;
}

/* Methods the 2008 IDL never declared (querySelector, insertAdjacentHTML,
 * getRootNode, ...): the permanent custom layer. Registered on the interface
 * Chrome puts them on; the implementation comes from kDomMethodImpls, so a name
 * has exactly one C++ function. */
struct DomMethodExtra {
    const char* iface;
    const char* name;
};

static const DomMethodExtra kExtraMethods[] = {
    { "Document", "createHTMLDocument" },
    { "Document", "getElementsByClassName" },
    { "Document", "getElementsByName" },
    { "Document", "getSelection" },
    { "Document", "hasFocus" },
    { "Document", "querySelector" },
    { "Document", "querySelectorAll" },
    { "Document", "replaceChildren" },
    { "Document", "write" },
    { "Document", "writeln" },
    { "DocumentFragment", "replaceChildren" },
    { "Element", "getAttributeNode" },
    { "Element", "getAttributeNS" },
    { "Element", "setAttributeNS" },
    { "Element", "getClientRects" },
    { "Element", "getAttributeNames" },
    { "Element", "getBoundingClientRect" },
    { "Element", "getElementsByClassName" },
    { "Element", "insertAdjacentHTML" },
    { "Element", "insertAdjacentText" },
    { "Element", "querySelector" },
    { "Element", "querySelectorAll" },
    { "Element", "toggleAttribute" },
    { "HTMLCanvasElement", "toDataURL" },
    { "HTMLFormElement", "checkValidity" },
    { "HTMLMediaElement", "addTextTrack" },
    { "HTMLMediaElement", "canPlayType" },
    { "HTMLMediaElement", "pause" },
    { "HTMLMediaElement", "play" },
    { "Node", "addEventListener" },
    { "Node", "compareDocumentPosition" },
    { "Node", "dispatchEvent" },
    { "Node", "getRootNode" },
    { "Node", "removeEventListener" },
    { "ShadowRoot", "querySelector" },
    { "ShadowRoot", "querySelectorAll" },
    { "HTMLSlotElement", "assignedNodes" },
    { "HTMLSlotElement", "assignedElements" },
    { "Element", "append" },
    { "Element", "prepend" },
    { "Element", "before" },
    { "Element", "after" },
    { "Element", "replaceWith" },
    { "Element", "replaceChildren" },
    { "Element", "remove" },
    { "CharacterData", "before" },
    { "CharacterData", "after" },
    { "CharacterData", "replaceWith" },
    { "CharacterData", "remove" },
    { "Document", "append" },
    { "Document", "prepend" },
    { "DocumentFragment", "append" },
    { "DocumentFragment", "prepend" },
    { "HTMLMediaElement", "load" },
};

/* The interface class mirrors (g_documentIface & co, g_tagClasses, the
 * selection/media classes) live in RuntimeState and are reached through the
 * g_* macros: one state is one engine, and they must never be shared between
 * two engines that are alive at the same time (a tab and its popup). */

/* Called by teardownRealm(): the classes belong to the engine that is going
 * away, so every pointer here must be dropped with it (a stale class would be
 * a use-after-free on the next document - the selection/media installers used
 * to keep their classes in statics and never reset them). */
void resetInterfaceClasses(RuntimeState* state)
{
    if (!state)
        return;

    state->nodeClass = NULL;
    state->documentIface = NULL;
    state->htmlDocumentIface = NULL;
    state->elementIface = NULL;
    state->htmLElementIface = NULL;
    state->svgElementIface = NULL;
    state->svgSvgIface = NULL;
    state->documentFragmentIface = NULL;
    state->characterDataIface = NULL;
    state->namedNodeMapIface = NULL;
    state->attrIface = NULL;
    state->shadowRootIface = NULL;
    state->treeWalkerIface = NULL;
    state->textIface = NULL;
    state->commentIface = NULL;
    state->unknownElementIface = NULL;
    state->windowClass = NULL;
    state->nodeListClass = NULL;
    state->htmlCollectionClass = NULL;
    state->styleSheetListClass = NULL;
    state->styleSheetClass = NULL;
    state->cssRuleClass = NULL;
    state->cssRuleListClass = NULL;
    state->mediaListClass = NULL;
    state->cssDeclarationClass = NULL;

    /* Owned by the per-realm interface sets (B2-4): clear the mirror only. */
    state->tagClasses = NULL;

    state->textTrackClass = NULL;
    state->textTrackListClass = NULL;
    state->textTrackCueClass = NULL;
    state->selectionClass = NULL;
    state->rangeClass = NULL;
    state->canvasCtxClass = NULL;
}

/* ---- WebIDL attribute accessors (B2-3) ----------------------------------
 * The facade catch-all (nodeGetter/titleGetter) used to answer every DOM
 * attribute from an instance-level exotic hook, so attributes were invisible
 * to `in`, Object.getOwnPropertyDescriptor(Interface.prototype, name) and
 * for-in, and a generated IDL accessor would have been shadowed. These
 * definitions put the common attributes on the interface prototypes as real
 * WebIDL accessors (enumerable, configurable) whose implementation is the same
 * facade dispatch, so behaviour does not change - only the property shape does.
 */
struct DomAttrDef {
    const char* iface;
    const char* name;
    int         writable;
};

static bool isDocumentWrapper(AntbValue* self)
{
    WebCore::Node* node = nodeOf(self);

    return node && node->nodeType() == 9;
}

/* WebIDL accessors live on the engine-level interface prototypes (created in
 * the main realm), so the getter/setter would run with the main realm current
 * and wrap nodes into that realm - but a wrapper must belong to the realm of
 * the object it is read from (`cd.body` read by the parent is the parent's
 * wrapper, the iframe's own body is the iframe's). Enter the receiver's
 * document realm around the facade dispatch, the same way the class catch-all
 * hook did before the accessors existed. */
static AntbRealm* attributeRealm(AntbEngine* engine, AntbValue* self)
{
    AntbRealm* wrapperRealm = self
        ? (AntbRealm*)antb_get_native(self, AGB_TAG_WRAPREALM) : NULL;
    WebCore::Node* node;

    if (wrapperRealm)
        return wrapperRealm;

    node = nodeOf(self);
    if (!engine || !node || !node->document())
        return NULL;
    return receiverRealm(node);
}

static int attrHostGet(AntbEngine* engine, AntbValue* self, const char* name,
                       AntbValue* out, void*)
{
    AntbRealm* want = attributeRealm(engine, self);
    AntbRealm* previous = NULL;
    int result;

    if (want && (previous = antb_realm_current(engine)) != want) {
        antb_realm_enter(engine, want);
        applyRealmInterfaces(want);
    }

    /* NodeList/HTMLCollection declare `length` in their IDL too, and their
     * receiver is a collection wrapper, not a node: the collection dispatch must
     * answer (it also handles the numeric indices). */
    result = 0;
    if (antb_get_native(self, AGB_TAG_SHADOWROOT)) {
        result = shadowRootGetter(engine, self, name, out, NULL);
        if (result > 0)
            return result;
    }

    if (antb_get_native(self, AGB_TAG_NAMEDMAP))
        result = namedMapGetter(engine, self, name, out, NULL);
    else if (antb_get_native(self, AGB_TAG_COLLECTION))
        result = collectionGetter(engine, self, name, out, NULL);
    else
        result = isDocumentWrapper(self) ? titleGetter(engine, self, name, out, NULL)
                                         : nodeGetter(engine, self, name, out, NULL);



    if (want && previous != want) {
        antb_realm_enter(engine, previous);
        applyRealmInterfaces(previous);
    }
    return result;
}

static int attrHostSet(AntbEngine* engine, AntbValue* self, const char* name,
                       const AntbValue* value, void*)
{
    AntbRealm* want = attributeRealm(engine, self);
    AntbRealm* previous = NULL;
    int result;

    if (want && (previous = antb_realm_current(engine)) != want) {
        antb_realm_enter(engine, want);
        applyRealmInterfaces(want);
    }

    result = isDocumentWrapper(self) ? titleSetter(engine, self, name, value, NULL)
                                     : nodeSetter(engine, self, name, value, NULL);

    if (want && previous != want) {
        antb_realm_enter(engine, previous);
        applyRealmInterfaces(previous);
    }
    return result;
}

/* Attributes the 2008 IDL does not declare (the modern DOM grew after it): the
 * permanent custom layer. They install exactly like the generated ones, so in,
 * for-in and Object.getOwnPropertyDescriptor keep seeing them; ownership and
 * writability follow Chrome (probed with tools/chrome_probe.py).
 *
 * Everything else is generated from the IDL (buildANT/Ant<Interface>.cpp) -
 * there is no hand-written attribute list for a migrated interface any more. */
static const DomAttrDef kExtraAttributes[] = {
    { "Node", "isConnected",      0 },
    { "Element", "childElementCount", 0 },
    { "Element", "firstElementChild", 0 },
    { "Element", "lastElementChild", 0 },
    { "Element", "nextElementSibling", 0 },
    { "Element", "previousElementSibling", 0 },
    { "Element", "classList",        0 },
    { "Element", "dataset",          0 },
    { "HTMLElement", "hidden",           1 },
    { "HTMLElement", "draggable",        1 },
    { "HTMLElement", "spellcheck",       1 },
    { "HTMLElement", "autofocus",        1 },
    { "Document", "activeElement",    0 },
    { "Document", "head",             0 },
    { "Document", "contentType",      0 },
    { "Document", "compatMode",       0 },
    { "Document", "hidden",           0 },
    { "Document", "visibilityState",  0 },
    { "Document", "fullscreenElement", 0 },
    { "Document", "fullscreenEnabled", 0 },
    { "Document", "webkitFullscreenElement", 0 },
    { "Document", "webkitFullscreenEnabled", 0 },
    { "HTMLInputElement", "placeholder",      1 },
    { "HTMLInputElement", "required",         1 },
    { "HTMLInputElement", "min",              1 },
    { "HTMLInputElement", "max",              1 },
    { "HTMLInputElement", "step",             1 },
    { "HTMLInputElement", "autocomplete",     1 },
    { "HTMLInputElement", "pattern",          1 },
    { "HTMLInputElement", "files",            0 },
    { "HTMLTextAreaElement", "placeholder",      1 },
    { "HTMLTextAreaElement", "required",         1 },
    { "HTMLTextAreaElement", "wrap",             1 },
    { "HTMLSelectElement", "required",         1 },
    { "HTMLFormElement", "noValidate",       1 },
    { "HTMLImageElement", "loading",          1 },
    { "HTMLImageElement", "currentSrc",       0 },
    { "HTMLMediaElement", "currentTime",      1 },
    { "HTMLMediaElement", "volume",           1 },
    { "HTMLMediaElement", "muted",            1 },
    { "HTMLMediaElement", "autoplay",         1 },
    { "HTMLMediaElement", "loop",             1 },
    { "HTMLMediaElement", "controls",         1 },
    { "HTMLMediaElement", "playbackRate",     1 },
    { "HTMLMediaElement", "defaultPlaybackRate", 1 },
    { "HTMLMediaElement", "preload",          1 },
    { "HTMLMediaElement", "duration",         0 },
    { "HTMLMediaElement", "paused",           0 },
    { "HTMLMediaElement", "ended",            0 },
    { "HTMLMediaElement", "networkState",     0 },
    { "HTMLMediaElement", "currentSrc",       0 },
    { "HTMLMediaElement", "videoWidth",       0 },
    { "HTMLMediaElement", "videoHeight",      0 },
    /* Names that also happen to exist on another interface (the extras list is
     * keyed by interface, not by name): input.multiple, textarea.maxLength,
     * script.async, media.src/readyState. Without these rows the name-match
     * against the generated tables silently drops them. */
    { "HTMLInputElement", "multiple",   1 },
    { "HTMLTextAreaElement", "maxLength", 1 },
    { "HTMLScriptElement", "async",     1 },
    { "HTMLMediaElement", "src",        1 },
    { "HTMLMediaElement", "readyState", 0 },
};

/* Event handler properties (`el.onclick`): nodeSetter stores them as
 * listeners and nodeGetter reads them back, so the accessor is just the same
 * dispatch. Chrome exposes them on HTMLElement.prototype and
 * Document.prototype; having them there fixes `'onclick' in el`. */
static const char* const kEventHandlerAttrs[] = {
    "onclick", "ondblclick", "onmousedown", "onmouseup", "onmousemove",
    "onmouseover", "onmouseout", "onmouseenter", "onmouseleave",
    "onkeydown", "onkeyup", "onkeypress", "oninput", "onchange",
    "onfocus", "onblur", "onfocusin", "onfocusout", "onsubmit", "onreset",
    "onload", "onerror", "onscroll", "onwheel", "oncontextmenu",
    "onpointerdown", "onpointerup", "onpointermove", "onpointerover", "onpointerout",
    "ontouchstart", "ontouchend", "ontouchmove",
    "ondragstart", "ondrag", "ondragend", "ondragover", "ondrop",
    "onanimationstart", "onanimationend", "ontransitionend",
    "oncopy", "oncut", "onpaste", "onselect"
};

static void installAttributeAccessors(AntbEngine* engine,
                                      std::map<std::string, AntbClass*>& byName)
{
    size_t i;
    unsigned d;

    if (getenv("AGB_NO_DOM_ATTRS"))
        return;

    /* Migrated interfaces: the table *is* the declaration list, so a name the
     * IDL has cannot be forgotten here (and one it does not have cannot sneak
     * in). The generator already moved members to the prototype Chrome puts
     * them on (@kOwnershipMoves in CodeGeneratorAnt.pm). */
    for (d = 0; d < AntBindings::interfaceCount(); ++d) {
        const AntbInterfaceDef* def = AntBindings::interfaceAt(d);
        std::map<std::string, AntbClass*>::iterator it;

        if (!def || !def->name)
            continue;

        it = byName.find(def->name);
        if (it == byName.end())
            continue;

        for (i = 0; i < def->attributeCount; ++i) {
            unsigned writable = (def->attributes[i].flags & ANTB_ATTR_WRITABLE) ? 1u : 0u;

            if (getenv("AGB_BINDING_TRACE"))
                agbLog(AGB_LOG_EVT, "[bind] define %s.%s", def->name, def->attributes[i].name);

            antb_class_define_attribute(engine, it->second, def->attributes[i].name,
                                        attrHostGet, writable ? attrHostSet : NULL, NULL);
        }
    }

    /* The names the 2008 IDL never declared (the permanent custom layer). */
    for (i = 0; i < sizeof(kExtraAttributes) / sizeof(kExtraAttributes[0]); ++i) {
        const DomAttrDef& def = kExtraAttributes[i];
        std::map<std::string, AntbClass*>::iterator it = byName.find(def.iface);

        if (it == byName.end())
            continue;
        antb_class_define_attribute(engine, it->second, def.name,
                                    attrHostGet, def.writable ? attrHostSet : NULL, NULL);
    }

    for (i = 0; i < sizeof(kEventHandlerAttrs) / sizeof(kEventHandlerAttrs[0]); ++i) {
        static const char* const kOwners[] = { "HTMLElement", "Document" };
        size_t owner;

        for (owner = 0; owner < sizeof(kOwners) / sizeof(kOwners[0]); ++owner) {
            std::map<std::string, AntbClass*>::iterator it = byName.find(kOwners[owner]);

            if (it == byName.end())
                continue;
            antb_class_define_attribute(engine, it->second, kEventHandlerAttrs[i],
                                        attrHostGet, attrHostSet, NULL);
        }
    }
}

/* struct RealmInterfaces is defined right after RuntimeState. */

/* The set of one state (one engine) for a realm. NULL is a valid key: it is the
 * single shared set of the AGB_PER_REALM_IFACES=0 escape hatch. */
static RealmInterfaces* realmInterfaceSetIn(RuntimeState* state, AntbRealm* realm)
{
    std::map<void*, RealmInterfaces*>::iterator it;

    if (!state || !state->realmIfaces)
        return NULL;

    it = state->realmIfaces->find((void*)realm);
    return it == state->realmIfaces->end() ? NULL : it->second;
}

/* Mirror one realm's interface set into `state`'s class pointers; the C
 * dispatchers read them through the g_* macros. Writing the state explicitly
 * (instead of through the macros) is what keeps two engines apart: a wrap for
 * engine A must never be answered out of engine B's prototypes. */
static void applyRealmInterfacesFor(RuntimeState* state, AntbRealm* realm)
{
    RealmInterfaces* set;

    if (!state)
        return;

    set = realmInterfaceSetIn(state, realm);
    state->ifacesActive = set;

    state->nodeClass             = set ? set->nodeClass : NULL;
    state->documentIface         = set ? set->documentIface : NULL;
    state->htmlDocumentIface     = set ? set->htmlDocumentIface : NULL;
    state->elementIface          = set ? set->elementIface : NULL;
    state->htmLElementIface      = set ? set->htmLElementIface : NULL;
    state->svgElementIface       = set ? set->svgElementIface : NULL;
    state->svgSvgIface           = set ? set->svgSvgIface : NULL;
    state->documentFragmentIface = set ? set->documentFragmentIface : NULL;
    state->characterDataIface    = set ? set->characterDataIface : NULL;
    state->namedNodeMapIface     = set ? set->namedNodeMapIface : NULL;
    state->attrIface             = set ? set->attrIface : NULL;
    state->shadowRootIface       = set ? set->shadowRootIface : NULL;
    state->treeWalkerIface       = set ? set->treeWalkerIface : NULL;
    state->textIface             = set ? set->textIface : NULL;
    state->commentIface          = set ? set->commentIface : NULL;
    state->unknownElementIface   = set ? set->unknownElementIface : NULL;
    state->windowClass           = set ? set->windowClass : NULL;
    state->nodeListClass         = set ? set->nodeListClass : NULL;
    state->htmlCollectionClass   = set ? set->htmlCollectionClass : NULL;
    state->tagClasses            = set ? set->tagClasses : NULL;
}

static void applyRealmInterfaces(AntbRealm* realm)
{
    applyRealmInterfacesFor(g_currentState, realm);
}

static int g_buildingInterfaces = 0;

/* One interface set per realm is the browser-faithful default; the switch goes
 * back to a single shared set per engine, which is the A/B baseline for the
 * per-realm cost and for any binding regression (R182). */
static bool perRealmIfacesEnabled()
{
    const char* value = getenv("AGB_PER_REALM_IFACES");

    return !value || strcmp(value, "0") != 0;
}

/* ---- generated interface tables (A1) -------------------------------------- *
 * The attributes of a migrated interface come from its generated table
 * (buildANT/Ant<Interface>.cpp, CodeGeneratorAnt.pm) instead of a hand-written
 * row in kDomAttributes, and their ownership is resolved by the generator
 * against Chrome (see @kOwnershipMoves there).
 *
 * AGB_BINDING_CHECK=1 reports the inventory once per process. The one failure
 * mode that would otherwise be silent: a table whose owning interface has no
 * class in this realm - nothing would be installed and every attribute would
 * quietly fall back to the catch-all.
 */
static void reportGeneratedBindingTables(const std::map<std::string, AntbClass*>& byName)
{
    unsigned i, missingOwners = 0, attributes = 0, methods = 0, constants = 0;
    std::string unimplemented;

    for (i = 0; i < AntBindings::interfaceCount(); ++i) {
        const AntbInterfaceDef* def = AntBindings::interfaceAt(i);

        if (!def || !def->name)
            continue;

        if (!byName.count(def->name)) {
            ++missingOwners;
            agbLog(AGB_LOG_EVT, "[bind] %s: table present but no interface class", def->name);
        }

        attributes += def->attributeCount;
        methods += def->methodCount;
        constants += def->constantCount;

        for (unsigned m = 0; m < def->methodCount; ++m) {
            if (!findMethodImpl(def->methods[m].name)) {
                if (!unimplemented.empty())
                    unimplemented += ",";
                unimplemented += def->methods[m].name;
            }
        }

        agbLog(AGB_LOG_EVT, "[bind] %s: idl %u attrs / %u methods / %u consts (parent %s)",
               def->name, def->attributeCount, def->methodCount, def->constantCount,
               def->parent ? def->parent : "-");
    }

    if (!unimplemented.empty())
        agbLog(AGB_LOG_EVT, "[bind] idl methods with no implementation (not registered): %s",
               unimplemented.c_str());

    agbLog(AGB_LOG_EVT,
           "[bind] generated tables: %u interfaces, %u attrs, %u methods, %u consts, %u missing owner(s)",
           AntBindings::interfaceCount(), attributes, methods, constants, missingOwners);
}
void ensureInterfaceClasses(AntbEngine* engine)
{
    RuntimeState* state;
    AntbRealm* realm;
    RealmInterfaces* set;

    if (!engine || g_buildingInterfaces)
        return;

    /* The classes belong to one engine, so they are looked up (and mirrored)
     * in that engine's state - not in whichever state the engine thread happens
     * to be in. */
    state = stateForEngine(engine);

    if (!state || state == &g_emptyState)
        return;

    if (!state->realmIfaces)
        state->realmIfaces = new std::map<void*, RealmInterfaces*>();

    realm = perRealmIfacesEnabled() ? antb_realm_current(engine) : NULL;
    set = realmInterfaceSetIn(state, realm);

    if (set) {
        if (state->ifacesActive != set)
            applyRealmInterfacesFor(state, realm);
        return;
    }

    /* The build writes the mirrors through the g_* macros, so this engine must
     * be the one the engine thread is in. Every call site (a wrap, a document
     * bind, an expose) runs inside the engine's own context. */
    if (g_currentState != state) {
        agbLog(AGB_LOG_EVT,
               "ensureInterfaceClasses: engine %p is not the current context, skipped",
               (void*)engine);
        return;
    }

    {
        RealmInterfaces* set;
        std::map<std::string, AntbClass*> byName;
        AntbValue* global;
        AntbValue* builtinCtor = NULL;
        AntbValue* builtinProto = NULL;
        AntbClass* eventTarget = NULL;
        AntbClass* nodeClass;
        size_t i;

        g_buildingInterfaces = 1;

        if (getenv("AGB_EVT_DEBUG"))
            agbLog(AGB_LOG_EVT, "ensureInterfaceClasses engine=%p state=%p realm=%p",
                   (void*)engine, (void*)g_currentState, (void*)realm);

        global = antb_global(engine);
        if (global) {
            builtinCtor = antb_get_prop(engine, global, "EventTarget");
            if (builtinCtor)
                builtinProto = antb_get_prop(engine, builtinCtor, "prototype");
        }

        /* The engine already ships EventTarget (and its prototype methods).
         * Chain to that one instead of creating a second, unrelated interface,
         * or `el instanceof EventTarget` would test the wrong prototype. */
        if (!builtinProto)
            eventTarget = antb_class_create_iface(engine, "EventTarget", NULL);

        nodeClass = antb_class_create_iface(engine, "Node", eventTarget);

        if (builtinProto && nodeClass)
            antb_class_set_parent_value(engine, nodeClass, builtinProto, builtinCtor);

        if (!nodeClass) {
            if (builtinProto) antb_value_release(builtinProto);
            if (builtinCtor) antb_value_release(builtinCtor);
            if (global) antb_value_release(global);
            g_buildingInterfaces = 0;
            return;
        }

        g_nodeClass = nodeClass;
        byName["Node"] = nodeClass;
        if (eventTarget)
            byName["EventTarget"] = eventTarget;

        for (i = 0; i < sizeof(kDomIfaces) / sizeof(kDomIfaces[0]); ++i) {
            const DomIfaceDef& def = kDomIfaces[i];
            AntbClass* parent = NULL;
            AntbClass* klass;

            if (def.parent) {
                std::map<std::string, AntbClass*>::iterator it = byName.find(def.parent);
                if (it != byName.end())
                    parent = it->second;
            }

            klass = antb_class_create_iface(engine, def.name, parent);
            if (klass)
                byName[def.name] = klass;
        }

        g_documentIface         = byName["Document"];
        g_htmlDocumentIface     = byName["HTMLDocument"];
        g_elementIface          = byName["Element"];
        g_htmLElementIface      = byName["HTMLElement"];
        g_svgElementIface       = byName["SVGElement"];
        g_svgSvgIface           = byName["SVGSVGElement"];
        g_documentFragmentIface = byName["DocumentFragment"];
        g_characterDataIface    = byName["CharacterData"];
        g_namedNodeMapIface     = byName["NamedNodeMap"];
        g_attrIface             = byName["Attr"];
        g_shadowRootIface       = byName["ShadowRoot"];
        g_treeWalkerIface       = byName["TreeWalker"];
        g_textIface             = byName["Text"];
        g_commentIface          = byName["Comment"];
        g_unknownElementIface   = byName["HTMLUnknownElement"];
        g_windowClass           = byName["Window"];
        g_nodeListClass         = byName["NodeList"];
        g_htmlCollectionClass   = byName["HTMLCollection"];

        /* Window inherits EventTarget in the DOM. prelude relinks each realm's
         * global object to this prototype, which is what makes `window
         * instanceof Window` / `window instanceof EventTarget` /
         * `[object Window]` hold. */
        if (g_windowClass && builtinProto)
            antb_class_set_parent_value(engine, g_windowClass, builtinProto, builtinCtor);

        /* Implemented classes that are not element interfaces get the same
         * shape, so `x instanceof CSSStyleDeclaration` / `Range` / `TextTrack`
         * work. These stay engine-scoped (first realm builds them). */
        cssomEnsureClasses();
        ensureSelectionClasses();
        installTextTrackClasses(engine);
        ensureCanvasContextClass(engine);

        g_tagClasses = new std::map<std::string, AntbClass*>();
        for (i = 0; i < sizeof(kDomTags) / sizeof(kDomTags[0]); ++i) {
            const DomTagDef& def = kDomTags[i];
            std::map<std::string, AntbClass*>::iterator it = byName.find(def.iface);

            if (it != byName.end())
                (*g_tagClasses)[def.tag] = it->second;
        }

        /* Methods: the generated tables own the membership, kDomMethodImpls the
         * implementation. An IDL method without an implementation is skipped
         * (AGB_BINDING_CHECK=1 counts them). */
        for (unsigned d = 0; d < AntBindings::interfaceCount(); ++d) {
            const AntbInterfaceDef* def = AntBindings::interfaceAt(d);
            std::map<std::string, AntbClass*>::iterator it;

            if (!def || !def->name)
                continue;

            it = byName.find(def->name);
            if (it == byName.end())
                continue;

            for (i = 0; i < def->methodCount; ++i) {
                AntbNativeFn impl = findMethodImpl(def->methods[i].name);

                if (!impl)
                    continue;
                if (getenv("AGB_BINDING_TRACE"))
                    agbLog(AGB_LOG_EVT, "[bind] method %s.%s", def->name, def->methods[i].name);
                antb_class_add_method(engine, it->second, def->methods[i].name, impl, NULL);
            }
        }

        /* Methods the IDL never declared (the permanent custom layer). */
        for (i = 0; i < sizeof(kExtraMethods) / sizeof(kExtraMethods[0]); ++i) {
            const DomMethodExtra& def = kExtraMethods[i];
            std::map<std::string, AntbClass*>::iterator it = byName.find(def.iface);
            AntbNativeFn impl = findMethodImpl(def.name);

            if (it == byName.end() || !impl)
                continue;
            antb_class_add_method(engine, it->second, def.name, impl, NULL);
        }

        /* The whole facade is per class now: one dispatching getter/setter pair
         * that receives every property the class does not declare. */
        antb_class_set_property_rw(engine, g_nodeClass, "", nodeGetter, nodeSetter, NULL);
        /* The generated table installs element.attributes' members as prototype
         * accessors; the catch-all also answers the numeric indexes and gives the
         * indexed-own-keys hook a length to ask (agb_class_catch_all). */
        if (g_namedNodeMapIface)
            antb_class_set_property_rw(engine, g_namedNodeMapIface, "", namedMapGetter, NULL, NULL);
        if (g_attrIface)
            antb_class_set_property_rw(engine, g_attrIface, "", nodeGetter, nodeSetter, NULL);
        /* The shadow root wrapper is host-backed: host/mode answered by its own
         * getter, everything else (innerHTML, childNodes, ...) by the node one. */
        if (g_shadowRootIface)
            antb_class_set_property_rw(engine, g_shadowRootIface, "", shadowRootGetter, nodeSetter, NULL);
        /* TreeWalker: the walker's own attributes and DOM2 traversal methods;
         * its receiver is an AgbTreeWalker, not a node, so it has its own
         * getter (currentNode is writable, the rest read-only). */
        if (g_treeWalkerIface) {
            static const struct { const char* name; int op; } kWalkerMethods[] = {
                { "parentNode",      TW_PARENT_NODE },
                { "firstChild",      TW_FIRST_CHILD },
                { "lastChild",       TW_LAST_CHILD },
                { "previousSibling", TW_PREVIOUS_SIBLING },
                { "nextSibling",     TW_NEXT_SIBLING },
                { "previousNode",    TW_PREVIOUS_NODE },
                { "nextNode",        TW_NEXT_NODE }
            };
            size_t w;

            antb_class_set_property_rw(engine, g_treeWalkerIface, "",
                                       treeWalkerGetter, treeWalkerSetter, NULL);
            antb_class_define_attribute(engine, g_treeWalkerIface, "root",
                                        treeWalkerGetter, NULL, NULL);
            antb_class_define_attribute(engine, g_treeWalkerIface, "whatToShow",
                                        treeWalkerGetter, NULL, NULL);
            antb_class_define_attribute(engine, g_treeWalkerIface, "filter",
                                        treeWalkerGetter, NULL, NULL);
            antb_class_define_attribute(engine, g_treeWalkerIface, "expandEntityReferences",
                                        treeWalkerGetter, NULL, NULL);
            antb_class_define_attribute(engine, g_treeWalkerIface, "currentNode",
                                        treeWalkerGetter, treeWalkerSetter, NULL);
            for (w = 0; w < sizeof(kWalkerMethods) / sizeof(kWalkerMethods[0]); ++w)
                antb_class_add_method(engine, g_treeWalkerIface, kWalkerMethods[w].name,
                                      treeWalkerMethod, (void*)(intptr_t)kWalkerMethods[w].op);
        }
        /* Collections answer length/numeric indices through the same catch-all
         * mechanism; item()/namedItem()/forEach/iterators stay on the prototype. */
        if (g_nodeListClass)
            antb_class_set_property_rw(engine, g_nodeListClass, "", collectionGetter,
                                       (AntbSetterFn)NULL, NULL);
        if (g_htmlCollectionClass)
            antb_class_set_property_rw(engine, g_htmlCollectionClass, "", collectionGetter,
                                       (AntbSetterFn)NULL, NULL);
        if (g_documentIface)
            antb_class_set_property_rw(engine, g_documentIface, "", titleGetter, titleSetter, NULL);
        if (g_htmlDocumentIface)
            antb_class_set_property_rw(engine, g_htmlDocumentIface, "", titleGetter, titleSetter, NULL);

        /* Common attributes on the interface prototypes (B2-3). After the
         * catch-alls: each definition registers a decline entry that makes the
         * catch-all yield to the prototype accessor. */
        installAttributeAccessors(engine, byName);

        if (getenv("AGB_BINDING_CHECK")) {
            static int reported = 0;

            if (!reported) {
                reported = 1;
                reportGeneratedBindingTables(byName);
            }
        }

        /* DOM constants, inherited by every child interface object. */
        if (g_nodeClass) {
            static const double kNodeTypes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
            static const char* const kNodeTypeNames[] = {
                "ELEMENT_NODE", "ATTRIBUTE_NODE", "TEXT_NODE", "CDATA_SECTION_NODE",
                "ENTITY_REFERENCE_NODE", "ENTITY_NODE", "PROCESSING_INSTRUCTION_NODE",
                "COMMENT_NODE", "DOCUMENT_NODE", "DOCUMENT_TYPE_NODE",
                "DOCUMENT_FRAGMENT_NODE", "NOTATION_NODE"
            };
            size_t c;

            for (c = 0; c < sizeof(kNodeTypes) / sizeof(kNodeTypes[0]); ++c)
                antb_class_add_constant(engine, g_nodeClass, kNodeTypeNames[c], kNodeTypes[c]);
        }

        set = new RealmInterfaces();
        set->byName = byName;
        set->tagClasses = g_tagClasses;
        set->nodeClass = g_nodeClass;
        set->documentIface = g_documentIface;
        set->htmlDocumentIface = g_htmlDocumentIface;
        set->elementIface = g_elementIface;
        set->htmLElementIface = g_htmLElementIface;
        set->svgElementIface = g_svgElementIface;
        set->svgSvgIface = g_svgSvgIface;
        set->documentFragmentIface = g_documentFragmentIface;
        set->characterDataIface = g_characterDataIface;
    set->namedNodeMapIface  = g_namedNodeMapIface;
    set->attrIface          = g_attrIface;
    set->shadowRootIface    = g_shadowRootIface;
    set->treeWalkerIface    = g_treeWalkerIface;
        set->textIface = g_textIface;
        set->commentIface = g_commentIface;
        set->unknownElementIface = g_unknownElementIface;
        set->windowClass = g_windowClass;
        set->nodeListClass = g_nodeListClass;
        set->htmlCollectionClass = g_htmlCollectionClass;

        /* Store it even for the NULL key: that is the shared set of the
         * escape hatch, and without the entry every call would rebuild the
         * whole class tree. */
        (*state->realmIfaces)[(void*)realm] = set;
        state->ifacesActive = set;

        /* Expose this realm's own constructors on its global. Expose skips
         * names already present, so a later antb_expose_classes() (or another
         * realm's set) can never override them. */
        for (std::map<std::string, AntbClass*>::iterator it = byName.begin();
             it != byName.end(); ++it)
            antb_class_expose(engine, it->second);

        if (builtinProto) antb_value_release(builtinProto);
        if (builtinCtor) antb_value_release(builtinCtor);
        if (global) antb_value_release(global);

        g_buildingInterfaces = 0;
    }
}

static AntbClass* elementClassForTag(WebCore::Element* element)
{
    WebCore::String tagName;
    std::string key;
    std::map<std::string, AntbClass*>::iterator it;

    if (!element)
        return g_elementIface;

    tagName = element->tagName();
    {
        const char* text = tagName.utf8().data();

        key.assign(text ? text : "");
    }

    for (size_t i = 0; i < key.size(); ++i) {
        char c = key[i];
        if (c >= 'A' && c <= 'Z')
            key[i] = (char)(c + ('a' - 'A'));
    }

    if (g_tagClasses) {
        it = g_tagClasses->find(key);
        if (it != g_tagClasses->end())
            return it->second;
    }

    /* <my-widget>: a custom element is a plain HTMLElement; an unknown legacy
     * tag is an HTMLUnknownElement (Chrome draws the same line). */
    if (key.find('-') != std::string::npos)
        return g_htmLElementIface;
    return g_unknownElementIface ? g_unknownElementIface : g_htmLElementIface;
}

AntbClass* classForNode(WebCore::Node* node)
{
    if (!node)
        return NULL;

    if (node->isDocumentNode()) {
        WebCore::Document* document = static_cast<WebCore::Document*>(node);
        return document->isHTMLDocument() ? g_htmlDocumentIface : g_documentIface;
    }

    if (node->isAttributeNode())
        return g_attrIface;

    if (node->isTextNode())
        return g_textIface;

    if (node->isCommentNode())
        return g_commentIface;

    if (node->isCharacterDataNode())
        return g_characterDataIface;

    if (node->nodeType() == 11)
        return g_documentFragmentIface;

    if (node->isElementNode()) {
        WebCore::Element* element = static_cast<WebCore::Element*>(node);

        if (element->isSVGElement()) {
            if (element->hasTagName(WebCore::SVGNames::svgTag))
                return g_svgSvgIface ? g_svgSvgIface : g_svgElementIface;
            return g_svgElementIface;
        }
        if (element->isHTMLElement())
            return elementClassForTag(element);
        return g_elementIface;
    }

    return g_nodeClass;
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

/* Public entry for ResourceHandleNet::fillResponse (the helper above lives in
 * the anonymous namespace because it shares the cookie jar internals). */
void addCookiesFromResponse(const char* url, const char* setCookie)
{
    addCookiesFromResponseImpl(url, setCookie);
}

/* Inline content attributes (`onkeydown="…"`) are compiled when the attribute
 * is parsed; the function body sees `this` (the element) and `event`, exactly
 * like a browser. WebCore's legacy path went through KJSProxy, which this
 * build does not have, so Document::createHTMLEventListener used to return 0:
 * Baidu's textarea `onkeydown="if (event.keyCode===13) event.preventDefault()"`
 * never ran and Enter inserted a newline instead of searching. */
void setInlineEventHandler(void* node, const char* eventName, const char* code, size_t codeLen)
{
    WebCore::Node* n = static_cast<WebCore::Node*>(node);
    const char* type;

    if (!n || !eventName || !g_engine)
        return;

    type = eventName;
    if (type[0] == 'o' && type[1] == 'n')
        type += 2;

    if (!code || !codeLen) {
        setNodeEventHandler(n, type, NULL);
        return;
    }

    {
        std::string src = "(function(event){";
        char* err = NULL;
        AntbValue* fn;

        src.append(code, codeLen);
        src += "\n})";

        fn = antb_eval(g_engine, src.data(), (int)src.size(), &err);
        if (!fn) {
            if (err) {
                /* A broken inline handler must not abort the page parse. */
                if (getenv("AGB_EVT_DEBUG"))
                    agbLog(AGB_LOG_EVT, "inline %s parse error: %s", eventName, err);
                free(err);
            }
            return;
        }
        if (antb_is_function(fn))
            setNodeEventHandler(n, type, fn);
        antb_value_release(fn);
    }
}


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
/* The document the engine is currently bound to (NULL before the first bind).
 * Callers that bind temporarily (a frame's DCL/load inside the parent's script)
 * use it to put the caller's document back: binding is not reference counted,
 * it is a stack the callers have to keep balanced. */
void* boundDocument()
{
    return (void*)g_document;
}

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
        /* A real event of the right interface (popstate → PopStateEvent,
         * pagehide → PageTransitionEvent, …): window listeners get an Event
         * instance instead of a plain object. */
        AntbValue* evt = makeEventObject(type, g_document, NULL);

        if (evt) {
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

/* Event interface for a WebCore event (or for a synthetic window event, whose
 * `event` is NULL). Chrome's mapping: mouse → MouseEvent (wheel → WheelEvent),
 * key → KeyboardEvent, text insertion → InputEvent, everything else
 * UIEvent/Event. */
static const char* eventInterfaceFor(WebCore::Event* event, const std::string& type)
{
    if (event) {
        if (event->isWheelEvent())
            return "WheelEvent";
        /* Chrome's mouse events are PointerEvent instances (PointerEvent extends
         * MouseEvent, so `e instanceof MouseEvent` still holds). prelude defines
         * PointerEvent with the pointer fields; the values are filled in
         * makeEventObjectForDispatch(). */
        if (event->isMouseEvent())
            return "PointerEvent";
        if (event->isKeyboardEvent())
            return "KeyboardEvent";
        if (event->isTextEvent())
            return "InputEvent";
        if (event->isUIEvent())
            return "UIEvent";
    }
    if (type == "focus" || type == "blur" || type == "focusin" || type == "focusout")
        return "FocusEvent";
    if (type == "popstate")
        return "PopStateEvent";
    if (type == "hashchange")
        return "HashChangeEvent";
    if (type == "pagehide" || type == "pageshow")
        return "PageTransitionEvent";
    if (type == "message" || type == "messageerror")
        return "MessageEvent";
    if (type == "resize")
        return "UIEvent";
    return "Event";
}

/* prelude's __agbEventPhase(evt, current, phase) is defined with the other
 * dispatcher helpers at the top of this file (they must be reachable from the
 * anonymous-namespace dispatchers too). */

AntbValue* makeEventObjectForDispatch(const std::string& type, WebCore::Node* target, WebCore::Event* event)
{
    AntbValue* global;
    AntbValue* factory;
    AntbValue* wrappedTarget;
    AntbValue* args[5];
    AntbValue* evt = NULL;
    unsigned i;
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
    args[2] = antb_make_string(g_engine, eventInterfaceFor(event, type));
    args[3] = antb_make_number(g_engine, (event && !event->bubbles()) ? 0.0 : 1.0);
    args[4] = antb_make_number(g_engine, (event && !event->cancelable()) ? 0.0 : 1.0);
    if (args[0]) {
        char* callErr = NULL;

        evt = antb_call(g_engine, factory, NULL, args, 5, &callErr);
        if (callErr) {
            if (getenv("AGB_EVT_DEBUG"))
                fprintf(stderr, "[antjs] event factory (%s): %s\n", type.c_str(), callErr);
            free(callErr);
        }
    }
    for (i = 0; i < 5; ++i) {
        if (args[i])
            antb_value_release(args[i]);
    }
    antb_value_release(factory);
    if (!evt)
        return NULL;

    /* el.click() / el.focus() were called by script, not by the user. The
     * factory marks engine events trusted; override it here (the property is
     * writable: prelude's `own()` defines it that way). */
    if (g_scriptInitiatedEvent) {
        AntbValue* untrusted = antb_make_boolean(g_engine, 0);

        if (untrusted) {
            antb_set_prop(g_engine, evt, "isTrusted", untrusted);
            antb_value_release(untrusted);
        }
    }

    /* PointerEvent fields for engine-dispatched mouse events. Chrome reports
     * pointerType "mouse" for a real mouse and "" for a scripted el.click();
     * pointerId 1 / isPrimary true / 1x1 contact are its defaults. */
    if (event && event->isMouseEvent()) {
        static const struct { const char* name; double value; } pointerNums[] = {
            { "pointerId", 1.0 }, { "isPrimary", 1.0 },
            { "width", 1.0 }, { "height", 1.0 }, { "pressure", 0.0 }
        };
        AntbValue* pointerType = antb_make_string(g_engine, g_scriptInitiatedEvent ? "" : "mouse");

        if (pointerType) {
            antb_set_prop(g_engine, evt, "pointerType", pointerType);
            antb_value_release(pointerType);
        }
        for (i = 0; i < sizeof(pointerNums) / sizeof(pointerNums[0]); ++i) {
            AntbValue* v = antb_make_number(g_engine, pointerNums[i].value);

            if (v) {
                antb_set_prop(g_engine, evt, pointerNums[i].name, v);
                antb_value_release(v);
            }
        }
    }
    /* target / currentTarget / isTrusted / eventPhase come from
     * __agbMakeEvent: the engine is the trusted source, while a JS
     * dispatchEvent() passes the caller's own object through nodeDispatchEvent
     * and keeps isTrusted false. */
    if (event) {
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
                            std::string* stringReturn = NULL,
                            AntbValue* phaseFn = NULL, int phase = 2)
{
    AntbValue* value = NULL;
    std::vector<Listener>* list = listenersForRealm(receiverRealm(node));

    if (!list)
        return;
    for (size_t i = 0; i < list->size(); ++i) {
        Listener& l = (*list)[i];
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
            /* Chrome updates currentTarget/eventPhase for every handler. */
            if (phaseFn)
                setEventPhase(g_engine, phaseFn, evt, currentTarget, phase);
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
    if (!g_engine || !node || !event)
        return;

    {
        AntbRealm* realm = receiverRealm(node);
        std::vector<Listener>* list = listenersForRealm(realm);
        std::string type = event->type().domString().utf8().data();

        if (getenv("AGB_EVT_DEBUG") && type == "DOMContentLoaded")
            agbLog(AGB_LOG_EVT, "dispatchEvent DCL node=%p realm=%p listeners=%d",
                   (void*)node, (void*)realm, list ? (int)list->size() : -1);

        if (!list)
            return;

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
        AntbValue* phaseFn = eventPhaseFn(g_engine);

        if (getenv("AGB_EVT_DEBUG") && (type == "load" || type == "DOMContentLoaded"))
            agbLog(AGB_LOG_EVT, "WebCore event %s on node=%p", type.c_str(), (void*)node);
        bool stopped = false;
        bool stoppedImmediate = false;
        if (!evt) {
            char* pending = antb_take_pending_error(g_engine);

            if (pending) {
                fprintf(stderr, "[antjs] (event %s) event object failed: %s\n", type.c_str(), pending);
                free(pending);
            }
            return;
        }

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
                    for (size_t li = 0; li < list->size(); ++li)
                        if ((*list)[li].node == current && (*list)[li].type == type)
                            ++matched;
                    fprintf(stderr, "[evt-trace] %s node=%p listeners=%d\n",
                            type.c_str(), (void*)current, matched);
                }
                runListenersFor(current, type, evt, wrapper, stopped, stoppedImmediate,
                                NULL, phaseFn, current == node ? 2 : 3);
                antb_value_release(wrapper);
            }
        }
        /* Chrome resets currentTarget after the dispatch. */
        if (phaseFn)
            setEventPhase(g_engine, phaseFn, evt, NULL, 0);
        /* preventDefault in JS must reach the engine, or links still navigate. */
        bool preventedInJs = false;
        {
            AntbValue* prevented = antb_get_prop(g_engine, evt, "__prevented");

            if (!prevented)
                prevented = antb_get_prop(g_engine, evt, "defaultPrevented");
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
        {
            for (size_t i = 0; i < list->size(); ) {
                Listener& l = (*list)[i];
                bool drop = false;

                if (l.once && l.type == type) {
                    for (size_t v = 0; v < visited.size(); ++v)
                        if (l.node == visited[v]) { drop = true; break; }
                }
                if (drop) {
                    if (l.fn)
                        antb_value_release(l.fn);
                    list->erase(list->begin() + i);
                } else {
                    ++i;
                }
            }
        }

        if (phaseFn)
            antb_value_release(phaseFn);
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

/* Fires a document-level event at the listeners registered on the document
 * (document.addEventListener('selectionchange', ...)). WebCore's Document is
 * not an EventTargetNode, so this bridge's listener table is the only route -
 * the same one beforeunload already uses.
 *
 * Re-entrancy: a handler that moves the selection again would recurse forever;
 * Chrome coalesces these into tasks. One event per outer change is enough for
 * the UI patterns this exists for (enabling a Copy button, counters). */
void dispatchDocumentEvent(void* document, const char* type)
{
    WebCore::Document* doc = (WebCore::Document*)document;
    static bool inDispatch = false;
    DocumentRuntime* runtime;
    AntbValue* evt;
    AntbValue* target;
    bool stopped = false;
    bool stoppedImmediate = false;
    bool hasListener = false;

    if (!doc || !type || !g_engine || inDispatch || !g_listeners)
        return;

    runtime = documentRuntimeOf(g_currentState, doc);
    if (!runtime)
        return;   /* document of a torn-down realm */

    for (size_t i = 0; i < g_listeners->size(); ++i) {
        Listener& l = (*g_listeners)[i];

        if (l.node == doc && l.type == type && l.fn) {
            hasListener = true;
            break;
        }
    }

    if (!hasListener)
        return;

    activateDocument(runtime);

    evt = makeEventObject(type, doc, NULL);
    if (!evt)
        return;

    target = wrapNode(g_engine, doc);
    if (target) {
        inDispatch = true;
        runListenersFor(doc, type, evt, target, stopped, stoppedImmediate);
        inDispatch = false;
        antb_value_release(target);
    }

    antb_value_release(evt);
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

/* Custom element lifecycle entry points (issues #18 phase A): WebCore calls these
 * from Element::insertedIntoDocument/removedFromDocument and
 * StyledElement::attributeChanged; the real work lives with the rest of the
 * custom element helpers (anonymous namespace above). */
namespace AntRuntime {

void elementDidInsert(void* element)
{
    customElementDidInsert(element);

    /* A node inserted into a shadow subtree inherits that tree's scope; this is
     * the path for component code that appends to a shadow element (the
     * ShadowRoot wrapper marks the subtree before insertion itself). */
    if (g_currentDocument && g_currentDocument->shadowRoots &&
        !g_currentDocument->distributingShadow && element) {
        WebCore::Node* node = static_cast<WebCore::Node*>(element);
        WebCore::Node* parent = node->parentNode();

        if (parent) {
            int scope = parent->shadowScope();

            if (scope > 0 && node->shadowScope() != scope) {
                markShadowSubtree(node, scope);
                rebuildShadowStyleSheets(node);
            }
            {
                WebCore::Element* host = nearestShadowHost(parent);

                if (host)
                    distributeShadowHost(host);
            }
        }
    }
}

void elementDidRemove(void* element)
{
    customElementDidRemove(element);

    if (g_currentDocument && g_currentDocument->shadowRoots &&
        !g_currentDocument->distributingShadow && element) {
        WebCore::Node* node = static_cast<WebCore::Node*>(element);
        WebCore::Element* host = nearestShadowHost(node);

        if (host) {
            removeLightNode(host, node);
            distributeShadowHost(host);
        }
    }
}

void elementAttributeChanged(void* element, const char* name)
{
    customElementAttributeHook(element, name);

    if (!g_currentDocument || !g_currentDocument->shadowRoots ||
        g_currentDocument->distributingShadow || !element || !name)
        return;

    /* <slot name> changes which light nodes it takes; a light node's slot=""
     * changes which slot takes it. */
    if (!strcmp(name, "slot") || !strcmp(name, "name")) {
        WebCore::Node* node = static_cast<WebCore::Node*>(element);
        WebCore::Element* host = nearestShadowHost(node);

        if (host)
            distributeShadowHost(host);
    }
}

} /* namespace AntRuntime */