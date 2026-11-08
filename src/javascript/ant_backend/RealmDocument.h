/*
 * RealmDocument.h - document-scoped window objects (location, history).
 *
 * Every frame realm owns a window document. Objects that modern pages ask for
 * ("what is my URL?", "what is my history?") must answer for the document they
 * were created for, not for whichever document happens to be bound in the
 * engine right now. This module owns those objects and the per-frame session
 * history; AntRuntime only registers them on the realm global.
 *
 * See docs/plan-realm-document-binding.md (B1).
 */

#ifndef RealmDocument_h
#define RealmDocument_h

#include <AntBridge.h>

namespace WebCore {
class Document;
}

namespace AntRuntime {
namespace RealmDocument {

/* Called once by AntRuntime: tells the module how to read the current binding
 * (shared-realm mode keeps using it, per-frame realms use the object's tag). */
void setContext(bool perFrameRealms, WebCore::Document* (*currentDocument)());

/* One Location per document (per engine in shared-realm mode). */
AntbValue* locationObject(AntbEngine* engine, WebCore::Document* document);
/* window.location = "..." / window.location.href = "..." */
int setLocation(AntbValue* self, const AntbValue* value);

/* One History wrapper per document; the entries live per browsing context
 * (Frame*), so a navigation does not erase the back/forward list. */
AntbValue* historyObject(AntbEngine* engine, WebCore::Document* document);
/* A new main-frame document was bound: append/select its history entry. */
void noteDocument(AntbEngine* engine, WebCore::Document* document);

/* Drop every object/value created for this engine (before engine destroy). */
void releaseEngine(AntbEngine* engine);

/* Drop the location/history wrappers of a document that is going away. The
 * frame's session history entries stay (the Frame* survives navigation). */
void forgetDocument(AntbEngine* engine, WebCore::Document* document);

/* History.pushState/back/forward fires popstate through this callback, so the
 * event is dispatched by AntRuntime in the right realm. */
void setPopstateDispatcher(void (*dispatcher)(const char* type, AntbValue* state));

/* The document a wrapper belongs to (LOCATION/HISTORY tag, or NODE/WINDOW). */
WebCore::Document* documentOfReceiver(AntbValue* self);

/* AGB_REALM_CHECK=1: log once per site when a write API's self document is not
 * the document currently bound in the engine. */
void checkMismatch(const char* where, WebCore::Document* selfDocument);

} /* namespace RealmDocument */
} /* namespace AntRuntime */

#endif /* RealmDocument_h */
