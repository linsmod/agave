/*
 * AntRuntime.h - process-wide Ant engine lifetime for agave.
 *
 * The Ant engine lives in antbridge.dll (see 3rdparty/ant). This wrapper is the
 * only C++ place that talks to it, so the rest of the engine stays engine-agnostic.
 */

#ifndef ANT_RUNTIME_H
#define ANT_RUNTIME_H

#include <stddef.h>

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
void flushDeferredModules(void);

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

/* Fires a synthetic event on the window/document listeners (used for
 * DOMContentLoaded and load, which jQuery's ready queue waits for). */
void dispatchWindowEvent(const char* type);

/* The document is about to be replaced: fires the outgoing page's teardown
 * events (pagehide, unload) at its own listeners and stops its realm. Call it
 * while the old document is still the current one. */
void documentWillUnload(void);

/* Starts a fresh realm for the next document. Call it before the new document
 * is parsed: every document gets its own JS world (globals, module registry,
 * timers). */
void beginNewRealm(void);

} /* namespace AntRuntime */

#endif /* ANT_RUNTIME_H */
