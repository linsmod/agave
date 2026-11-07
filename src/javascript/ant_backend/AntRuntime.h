/*
 * AntRuntime.h - process-wide Ant engine lifetime for agave.
 *
 * The Ant engine lives in antjsb.dll (see 3rdparty/ant). This wrapper is the
 * only C++ place that talks to it, so the rest of the engine stays engine-agnostic.
 */

#ifndef ANT_RUNTIME_H
#define ANT_RUNTIME_H

#include <stddef.h>
#include <string>

namespace AntRuntime {

/* window.open() goes through this hook: the embedding layer decides whether a
 * new view is created (return non-zero) or the popup is blocked (return 0). */
typedef int (*OpenWindowHook)(const char* url);
void setOpenWindowHook(OpenWindowHook hook);

/* window.agavePostMessage(str) hands a string to the embedding host. */
typedef void (*PostMessageHook)(const char* message);
void setPostMessageHook(PostMessageHook hook);

/* alert()/confirm()/prompt(): type 0/1/2, see macross_set_dialog_hook. */
typedef int (*DialogHook)(int type, const char* message, const char* defaultValue,
                          char* result, int resultSize);
void setDialogHook(DialogHook hook);

/* Creates the JS engine. Safe to call more than once; returns false on failure. */
bool start();

/* Per-view JS context (own Ant VM). Returns an opaque handle, or NULL when the
 * engine cannot be created. Callers must destroy it with destroyContext(). */
void* createContext();

/* Destroys a context and its engine (no-op for NULL). */
void destroyContext(void* context);

/* Makes `context` the active one; NULL is ignored. */
void activateContext(void* context);

/* Maps a WebCore::Page* to its context so KJSProxy can find the right engine. */
void registerPageContext(void* page, void* context);
void* contextForPage(void* page);

/* Destroys the engine. */
void stop();

/* True once start() succeeded. */
bool isRunning();

/* Evaluates UTF-8 source in the engine's global scope.
 * Returns 1 on success and *out receives a malloc'd UTF-8 representation of the
 * result (caller frees with free()). On failure returns 0 and *err receives a
 * malloc'd message (may be NULL). Passing NULL for out/err is allowed. */
int evaluate(const char* src, size_t len, char** out, char** err);

/* Evaluates UTF-8 `src` as an ES module whose URL is `filename` (so relative
 * imports resolve against it). Same out/err contract as evaluate(). */
int evaluateModule(const char* src, size_t len, const char* filename, char** out, char** err);

/* Module scripts are deferred until the document has been parsed. The script
 * element queues them; the frame loader flushes the queue once parsing ends. */
void queueModule(const char* url, const char* source);
void prefetchModuleGraph(const char* url, const char* source);

/* `document` is the WebCore::Document that finished parsing (the queue is
 * flushed per document, and every entry is bound to its own document before it
 * runs). */
void flushDeferredModules(void* document);

/* Classic <script defer src>: must not block the parser (it used to run before
 * <body> existed) and must run after parsing, before DOMContentLoaded. The
 * element queues the body when it arrives; the post-parse flush runs it in
 * document order. If the flush already happened for `document`, it runs
 * immediately. `scriptElement` (an HTMLScriptElement*, may be NULL) becomes
 * document.currentScript while the body runs. */
void queueDeferredScript(void* document, const char* url, const char* source, void* scriptElement = 0);

/* Module scripts (external entry, inline, or fetched while parsing) are
 * deferred by spec: they run after the document is parsed, in document order
 * together with classic defer scripts. Running them in the parse flow made
 * v.qq.com mount its Vue app before #channel-app existed. */
void queueDeferredModule(void* document, const char* url, const char* source);

/* External <script type="module" src>: loads the body through the module
 * fetcher (the same cache and cancel owner as its imports) and hands it to
 * the notifier below when it arrives - the tokenizer keeps parsing blocked
 * until then and runs the module in the parse flow, so DOMContentLoaded
 * stays behind module execution, per spec. */
typedef void (*ModuleEntryNotifyHook)(const char* url, const char* source, int failed);
void setModuleEntryNotifyHook(ModuleEntryNotifyHook hook);
void fetchAndQueueModuleScript(const char* url);

/* Binds the page's document to the JS `document` object (task 2 acceptance):
 * `document.title` reads/writes WebCore::Document::title(). `document` is a
 * pointer to a WebCore::Document. Safe to call on every script evaluation. */
void bindDocument(void* document);

/* Delivers completed async requests (XHR) to the page. Call from the frame loop. */
void pumpAsyncRequests(void);

/* Sets the name used in stack traces for subsequent evaluations. */
void setFilename(const char* name);

/* Dispatches a WebCore event (node, Event*) to the JS listeners registered via
 * addEventListener(). Called from EventTargetNode::dispatchEvent(). */
void dispatchEvent(void* node, void* event);

/* Page lifecycle events (commit, realm start/stop, bindDocument, pagehide/
 * unload/DOMContentLoaded/load) are emitted through agbLog() on the
 * AGB_LOG_EVT channel (AgbLog.h) so one run explains its own timeline;
 * AGB_EVT_LOG=0 silences them. Verbose plumbing (evaluate depth, listener
 * bookkeeping) stays behind AGB_EVT_DEBUG. */

/* Fires a synthetic event on the window/document listeners (used for
 * DOMContentLoaded and load, which jQuery's ready queue waits for). */
/* Dispatches a window event (DOMContentLoaded, load, ...) at the current
 * document's listeners. */
void dispatchWindowEvent(const char* type);

/* Frame documents only: runs the frame's deferred modules and dispatches the
 * event on the frame's own window (per-frame realms), then restores the parent
 * binding. No-op for the main frame or when realms are disabled. */
void dispatchFrameDocumentEvent(void* document, const char* type);

/* A frame's viewport changed: fires `resize` at the page's own listeners.
 * WebCore dispatches its window listeners (HTML attribute handlers) itself;
 * addEventListener('resize') and window.onresize live in this runtime's
 * listener table and are only reached from here. */
void dispatchResizeEvent(void* document);

/* The document is about to be replaced: fires the outgoing page's teardown
 * events (pagehide, unload) at its own listeners and stops its realm. Call it
 * while the old document is still the current one. */
void documentWillUnload(void);

/* Runs the page's beforeunload guards (window.onbeforeunload /
 * addEventListener). Returns true when leaving must be confirmed and fills
 * `text` with the message to show. Called from Frame::shouldClose(). */
bool beforeUnloadGuard(std::string& text);

/* Starts a fresh realm for the next document. Call it before the new document
 * is parsed: every document gets its own JS world (globals, module registry,
 * timers). */
void beginNewRealm(void);

/* A frame was detached: drop its realm (the JS world of that frame goes away
 * with its document). No-op unless the frame had its own realm
 * (AGB_PER_FRAME_REALM). `document` may be NULL. */
void forgetFrameRealm(void* document);

} /* namespace AntRuntime */

#endif /* ANT_RUNTIME_H */
