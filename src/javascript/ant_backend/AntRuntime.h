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

/* Creates the JS engine. Safe to call more than once; returns false on failure. */
bool start();

/* Destroys the engine. */
void stop();

/* True once start() succeeded. */
bool isRunning();

/* Evaluates UTF-8 source in the engine's global scope.
 * Returns 1 on success and *out receives a malloc'd UTF-8 representation of the
 * result (caller frees with free()). On failure returns 0 and *err receives a
 * malloc'd message (may be NULL). Passing NULL for out/err is allowed. */
int evaluate(const char* src, size_t len, char** out, char** err);

/* Binds the page's document to the JS `document` object (task 2 acceptance):
 * `document.title` reads/writes WebCore::Document::title(). `document` is a
 * pointer to a WebCore::Document. Safe to call on every script evaluation. */
void bindDocument(void* document);

/* Dispatches a WebCore event (node, Event*) to the JS listeners registered via
 * addEventListener(). Called from EventTargetNode::dispatchEvent(). */
void dispatchEvent(void* node, void* event);

/* Fires a synthetic event on the window/document listeners (used for
 * DOMContentLoaded and load, which jQuery's ready queue waits for). */
void dispatchWindowEvent(const char* type);

} /* namespace AntRuntime */

#endif /* ANT_RUNTIME_H */
