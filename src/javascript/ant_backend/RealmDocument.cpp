/*
 * RealmDocument.cpp - document-scoped window objects: Location and History.
 *
 * Why this file exists: `g_document` is the document currently bound in the
 * engine, which is *not* necessarily the document a JS object belongs to when
 * two frames have their own realms. Reading it from a getter made
 * `location.href` inside an iframe report the parent URL, so Vue/WeChat pages
 * that parse their own query string took the wrong branch.
 *
 * The fix is object-anchored: the Location object carries its Document, the
 * History object carries its FrameHistory (one per browsing context), and the
 * getters resolve from the receiver instead of the ambient binding.
 *
 * See docs/plan-realm-document-binding.md B1 and R132.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

#include "AntBridge.h"
#include "RealmDocument.h"
#include "RealmTags.h"

#include "AgbLog.h"

#include "Document.h"
#include "Node.h"
#include "Frame.h"
#include "FrameLoader.h"
#include "Page.h"
#include "KURL.h"
#include "PlatformString.h"
#include "DeprecatedString.h"

namespace AntRuntime {
namespace RealmDocument {

/* ------------------------------------------------------------------------- */
/* context                                                                   */
/* ------------------------------------------------------------------------- */

static bool g_perFrame = true;
static WebCore::Document* (*g_currentDocument)() = 0;
static void (*g_popstate)(const char* type, AntbValue* state) = 0;

void setContext(bool perFrameRealms, WebCore::Document* (*currentDocument)())
{
    g_perFrame = perFrameRealms;
    g_currentDocument = currentDocument;
}

void setPopstateDispatcher(void (*dispatcher)(const char* type, AntbValue* state))
{
    g_popstate = dispatcher;
}

static WebCore::Document* currentDocument()
{
    return g_currentDocument ? g_currentDocument() : 0;
}

/* ------------------------------------------------------------------------- */
/* receiver -> document                                                      */
/* ------------------------------------------------------------------------- */

/*
 * A Location/History object carries its own Document/FrameHistory. Window
 * proxies and realm globals carry the document node (AGB_TAG_NODE), and node
 * wrappers are resolved through Node::document(). Only a receiver that has no
 * native at all falls back to the current binding.
 */
WebCore::Document* documentOfReceiver(AntbValue* self)
{
    WebCore::Document* document;

    if (!self)
        return currentDocument();

    document = (WebCore::Document*)antb_get_native(self, AGB_TAG_LOCATION);
    if (document)
        return document;

    {
        WebCore::Node* node = (WebCore::Node*)antb_get_native(self, AGB_TAG_NODE);

        if (!node)
            node = (WebCore::Node*)antb_get_native(self, AGB_TAG_WINDOW);
        if (node) {
            if (node->nodeType() == 9)
                return static_cast<WebCore::Document*>(node);
            return node->document();
        }
    }

    return currentDocument();
}

void checkMismatch(const char* where, WebCore::Document* selfDocument)
{
    static int enabled = -1;
    static std::set<std::string>* seen = 0;
    WebCore::Document* bound;

    if (enabled < 0) {
        const char* value = getenv("AGB_REALM_CHECK");

        enabled = (value && value[0] == '1' && !value[1]) ? 1 : 0;
    }

    if (!enabled || !where || !selfDocument)
        return;

    bound = currentDocument();
    if (!bound || bound == selfDocument)
        return;

    if (!seen)
        seen = new std::set<std::string>();
    if (!seen->insert(where).second)
        return;

    agbLog(AGB_LOG_EVT, "realm-mismatch %s self=%s current=%s", where,
           selfDocument->URL().utf8().data(),
           bound->URL().utf8().data());
}

/* ------------------------------------------------------------------------- */
/* Location                                                                  */
/* ------------------------------------------------------------------------- */

struct LocationEntry {
    AntbEngine*        engine;
    WebCore::Document* document;   /* 0 = shared-realm mode, one per engine */
    AntbValue*         object;
};

static std::vector<LocationEntry>* g_locations = 0;

/* Per-frame realms: the object's own document. Shared realm: the document bound
 * right now, which preserves the old single-global behaviour. */
static WebCore::Document* locationDocument(AntbValue* self)
{
    WebCore::Document* document;

    document = (WebCore::Document*)antb_get_native(self, AGB_TAG_LOCATION);
    if (document)
        return document;

    return documentOfReceiver(self);
}

static void navigateTo(WebCore::Document* document, const char* text)
{
    WebCore::Frame* frame;

    if (!document || !text)
        return;

    frame = document->frame();
    if (!frame)
        return;

    {
        WebCore::KURL base(document->URL());
        WebCore::KURL target(base, WebCore::DeprecatedString(text));

        if (!target.isValid())
            return;
        frame->loader()->load(target, 0);
    }
}

static int locationGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    WebCore::Document* document = locationDocument(self);
    WebCore::KURL url;

    if (!document || !name)
        return 0;

    url = WebCore::KURL(document->URL());

    {
        WebCore::String value;
        WebCore::DeprecatedString deprecated = url.url();

        /* CSSOM semantics: protocol keeps its colon, host keeps the port and
         * hostname does not. Pages compare these strings literally. */
        if (!strcmp(name, "href"))
            value = WebCore::String(deprecated);
        else if (!strcmp(name, "protocol")) {
            value = url.protocol();
            if (!value.endsWith(WebCore::String(":")))
                value = value + WebCore::String(":");
        } else if (!strcmp(name, "host")) {
            value = url.host();
            if (url.port())
                value = value + WebCore::String(":") + WebCore::String::number(url.port());
        } else if (!strcmp(name, "hostname"))
            value = url.host();
        else if (!strcmp(name, "port"))
            value = url.port() ? WebCore::String::number(url.port()) : WebCore::String();
        else if (!strcmp(name, "origin")) {
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
        else if (!strcmp(name, "hash")) {
            /* location.hash includes the '#': KURL::ref() does not, and pages
             * compare it literally (scroll-to-anchor code does
             * location.hash.slice(1), which silently ate the first character
             * of the fragment). Empty fragment stays "". */
            value = url.ref();
            if (!value.isEmpty())
                value = WebCore::String("#") + value;
        }
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

static int locationSetter(AntbEngine*, AntbValue* self, const char* name, const AntbValue* value, void*)
{
    char* text;
    WebCore::Document* document;

    if (!name || strcmp(name, "href") != 0 || !value)
        return 0;

    text = antb_to_utf8(value);
    if (!text)
        return 0;

    document = locationDocument(self);
    checkMismatch("location.href", document);
    navigateTo(document, text);
    antb_free(text);
    return 1;
}

static int locationAssign(AntbEngine*, AntbValue* self, AntbValue** args, int argc, AntbValue*, void*)
{
    char* text;

    if (argc < 1)
        return 1;

    text = antb_to_utf8(args[0]);
    if (!text)
        return 1;

    checkMismatch("location.assign", locationDocument(self));
    navigateTo(locationDocument(self), text);
    antb_free(text);
    return 1;
}

static int locationReplace(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                           AntbValue* out, void* user)
{
    return locationAssign(engine, self, args, argc, out, user);
}

static int locationReload(AntbEngine*, AntbValue* self, AntbValue**, int, AntbValue*, void*)
{
    WebCore::Document* document = locationDocument(self);

    if (document && document->frame())
        document->frame()->loader()->reload();
    return 1;
}

static int locationToString(AntbEngine* engine, AntbValue* self, AntbValue**, int, AntbValue* out, void*)
{
    WebCore::Document* document = locationDocument(self);
    WebCore::String href;
    AntbValue* v;

    if (!document)
        return 0;

    href = WebCore::String(WebCore::KURL(document->URL()).url());
    v = antb_make_string(engine, href.utf8().data());
    if (!v)
        return 0;
    antb_value_assign(out, v);
    antb_value_release(v);
    return 1;
}

AntbValue* locationObject(AntbEngine* engine, WebCore::Document* document)
{
    WebCore::Document* key;
    size_t i;

    if (!engine)
        return 0;
    if (!document)
        return 0;

    key = document;

    if (g_locations) {
        for (i = 0; i < g_locations->size(); ++i) {
            if ((*g_locations)[i].engine == engine && (*g_locations)[i].document == key)
                return (*g_locations)[i].object
                     ? antb_retain((*g_locations)[i].object) : 0;
        }
    } else {
        g_locations = new std::vector<LocationEntry>();
    }

    {
        AntbValue* loc = antb_make_object(engine);

        if (!loc)
            return 0;

        antb_define_property_rw(engine, loc, "href", locationGetter, locationSetter, 0);
        antb_define_method(engine, loc, "assign", locationAssign, 0);
        antb_define_method(engine, loc, "replace", locationReplace, 0);
        antb_define_method(engine, loc, "reload", locationReload, 0);
        antb_define_method(engine, loc, "toString", locationToString, 0);
        antb_define_method(engine, loc, "valueOf", locationToString, 0);
        antb_set_native(loc, document, AGB_TAG_LOCATION);

        {
            LocationEntry entry;

            entry.engine = engine;
            entry.document = key;
            entry.object = antb_retain(loc);
            g_locations->push_back(entry);
        }

        return loc;
    }
}

int setLocation(AntbValue* self, const AntbValue* value)
{
    char* text;
    WebCore::Document* document = locationDocument(self);

    if (!value)
        return 0;

    text = antb_to_utf8(value);
    if (!text)
        return 0;

    checkMismatch("window.location", document);
    navigateTo(document, text);
    antb_free(text);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* History                                                                   */
/* ------------------------------------------------------------------------- */

struct HistoryEntry {
    std::string        url;
    AntbValue*         state;      /* retained; 0 means null               */
    WebCore::Document* document;   /* document that created the entry      */
};

/* One per browsing context: survives document navigations, like a real
 * session history. */
struct FrameHistory {
    AntbEngine*        engine;
    WebCore::Frame*    frame;
    WebCore::Document* document;
    std::vector<HistoryEntry> entries;
    int                index;
};

struct HistoryObjectEntry {
    AntbEngine*        engine;
    WebCore::Document* document;   /* 0 = shared-realm mode */
    AntbValue*         object;
};

static std::vector<FrameHistory*>* g_histories = 0;
static std::vector<HistoryObjectEntry>* g_historyObjects = 0;

static void releaseHistoryEntry(HistoryEntry& entry)
{
    if (entry.state) {
        antb_value_release(entry.state);
        entry.state = 0;
    }
}

static FrameHistory* historyForFrame(AntbEngine* engine, WebCore::Frame* frame,
                                     WebCore::Document* document)
{
    size_t i;

    if (!engine || !frame)
        return 0;

    if (!g_histories)
        g_histories = new std::vector<FrameHistory*>();

    for (i = 0; i < g_histories->size(); ++i) {
        FrameHistory* history = (*g_histories)[i];

        if (history->engine == engine && history->frame == frame) {
            if (document)
                history->document = document;
            return history;
        }
    }

    {
        FrameHistory* history = new FrameHistory();

        history->engine = engine;
        history->frame = frame;
        history->document = document;
        history->index = 0;

        if (document) {
            HistoryEntry entry;

            entry.url = document->URL().utf8().data();
            entry.state = 0;
            entry.document = document;
            history->entries.push_back(entry);
        }

        g_histories->push_back(history);
        return history;
    }
}

/* The history object of `self`: its own tag in per-frame mode, the frame of the
 * currently bound document in shared-realm mode. */
static FrameHistory* historyForReceiver(AntbEngine* engine, AntbValue* self)
{
    WebCore::Document* document;

    {
        FrameHistory* history = (FrameHistory*)antb_get_native(self, AGB_TAG_HISTORY);

        if (history)
            return history;
    }

    document = documentOfReceiver(self);
    return historyForFrame(engine, document ? document->frame() : 0, document);
}

void noteDocument(AntbEngine* engine, WebCore::Document* document)
{
    FrameHistory* history;
    std::string url;
    size_t i;

    if (!engine || !document || document->parentDocument())
        return;

    url = document->URL().utf8().data();
    if (url == "about:blank")
        return;

    history = historyForFrame(engine, document->frame(), document);
    if (!history)
        return;

    history->document = document;

    if (!history->entries.empty() && history->index >= 0
            && history->entries[history->index].url == url)
        return;   /* the same document bound again */

    if (history->entries.empty()) {
        HistoryEntry entry;

        entry.url = url;
        entry.state = 0;
        entry.document = document;
        history->entries.push_back(entry);
        history->index = 0;
        return;
    }

    /* Re-entering a URL we already have is a traversal, not a new entry. */
    for (i = 0; i < history->entries.size(); ++i) {
        if (history->entries[i].url == url) {
            history->index = (int)i;
            return;
        }
    }

    /* A real navigation: forward entries are gone. */
    if (history->index + 1 < (int)history->entries.size()) {
        for (i = history->index + 1; i < history->entries.size(); ++i)
            releaseHistoryEntry(history->entries[i]);
        history->entries.resize(history->index + 1);
    }

    {
        HistoryEntry entry;

        entry.url = url;
        entry.state = 0;
        entry.document = document;
        history->entries.push_back(entry);
        history->index = (int)history->entries.size() - 1;
    }
}

static void applyHistoryURL(WebCore::Document* document, const std::string& url)
{
    if (document && !url.empty())
        document->setURL(WebCore::DeprecatedString(url.c_str()));
}

static void historyGoBy(AntbEngine* engine, AntbValue* self, int delta)
{
    FrameHistory* history = historyForReceiver(engine, self);
    WebCore::Document* document = history ? history->document : 0;
    int target;

    if (!history || !document)
        return;

    target = history->index + delta;

    agbLog(AGB_LOG_NAV, "history go delta=%d index=%d entries=%d url=%s",
           delta, history->index, (int)history->entries.size(),
           document->URL().utf8().data());

    if (delta != 0 && history->index >= 0 && target >= 0
            && target < (int)history->entries.size()
            && history->entries[target].document == document) {
        history->index = target;
        applyHistoryURL(document, history->entries[target].url);
        if (g_popstate)
            g_popstate("popstate", history->entries[target].state);
        return;
    }

    /* Cross-document traversal is the FrameLoader's business. A subframe does
     * not have a loader-side back/forward list in this port, so it stays a
     * no-op there instead of moving the top-level page. */
    if (!document->parentDocument() && document->frame() && document->frame()->page()) {
        if (delta < 0)
            document->frame()->page()->goBack();
        else if (delta > 0)
            document->frame()->page()->goForward();
    }
}

static int historyBack(AntbEngine* engine, AntbValue* self, AntbValue**, int, AntbValue*, void*)
{
    historyGoBy(engine, self, -1);
    return 1;
}

static int historyForward(AntbEngine* engine, AntbValue* self, AntbValue**, int, AntbValue*, void*)
{
    historyGoBy(engine, self, 1);
    return 1;
}

static int historyGo(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc, AntbValue*, void*)
{
    historyGoBy(engine, self, argc > 0 && args[0] ? (int)antb_to_number(args[0]) : 0);
    return 1;
}

static int historyPushState(AntbEngine* engine, AntbValue* self, AntbValue** args, int argc,
                            AntbValue*, void* user)
{
    bool replace = user != 0;
    FrameHistory* history = historyForReceiver(engine, self);
    WebCore::Document* document = history ? history->document : 0;
    std::string url;

    if (!history || !document)
        return 1;

    checkMismatch(replace ? "history.replaceState" : "history.pushState", document);

    if (argc > 2 && args[2]) {
        char* urlText = antb_to_utf8(args[2]);

        if (urlText && urlText[0]) {
            WebCore::KURL base(document->URL());
            WebCore::KURL target(base, WebCore::DeprecatedString(urlText));

            if (target.isValid())
                url = target.url().utf8().data();
        }
        antb_free(urlText);
    }

    if (url.empty())
        url = document->URL().utf8().data();

    if (history->entries.empty()) {
        HistoryEntry base;

        base.url = document->URL().utf8().data();
        base.state = 0;
        base.document = document;
        history->entries.push_back(base);
        history->index = 0;
    }

    {
        HistoryEntry entry;

        entry.url = url;
        entry.state = (argc > 0 && args[0]) ? antb_retain(args[0]) : 0;
        entry.document = document;

        if (replace) {
            if (history->index >= 0 && history->index < (int)history->entries.size()) {
                releaseHistoryEntry(history->entries[history->index]);
                history->entries[history->index] = entry;
            } else {
                history->entries.push_back(entry);
                history->index = (int)history->entries.size() - 1;
            }
        } else {
            size_t i;

            if (history->index + 1 < (int)history->entries.size()) {
                for (i = history->index + 1; i < history->entries.size(); ++i)
                    releaseHistoryEntry(history->entries[i]);
                history->entries.resize(history->index + 1);
            }
            history->entries.push_back(entry);
            history->index = (int)history->entries.size() - 1;
        }
    }

    applyHistoryURL(document, url);
    return 1;
}

static int historyStateGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    FrameHistory* history = historyForReceiver(engine, self);
    AntbValue* value = 0;

    if (!name)
        return 0;

    if (!strcmp(name, "state")) {
        if (history && history->index >= 0
                && history->index < (int)history->entries.size()
                && history->entries[history->index].state)
            value = antb_retain(history->entries[history->index].state);
        if (!value)
            value = antb_make_null(engine);
    } else if (!strcmp(name, "length")) {
        int length = history ? (int)history->entries.size() : 0;

        if (length < 1)
            length = 1;
        value = antb_make_number(engine, (double)length);
    } else {
        return 0;
    }

    if (!value)
        return 0;

    antb_value_assign(out, value);
    antb_value_release(value);
    return 1;
}

AntbValue* historyObject(AntbEngine* engine, WebCore::Document* document)
{
    WebCore::Document* key;
    size_t i;

    if (!engine)
        return 0;
    if (!document)
        return 0;

    key = document;

    if (g_historyObjects) {
        for (i = 0; i < g_historyObjects->size(); ++i) {
            if ((*g_historyObjects)[i].engine == engine
                    && (*g_historyObjects)[i].document == key)
                return (*g_historyObjects)[i].object
                     ? antb_retain((*g_historyObjects)[i].object) : 0;
        }
    } else {
        g_historyObjects = new std::vector<HistoryObjectEntry>();
    }

    {
        AntbValue* history = antb_make_object(engine);

        if (!history)
            return 0;

        antb_define_method(engine, history, "back", historyBack, 0);
        antb_define_method(engine, history, "forward", historyForward, 0);
        antb_define_method(engine, history, "go", historyGo, 0);
        antb_define_method(engine, history, "pushState", historyPushState, 0);
        antb_define_method(engine, history, "replaceState", historyPushState, (void*)1);
        antb_define_property_rw(engine, history, "state", historyStateGetter, 0, 0);
        antb_define_property_rw(engine, history, "length", historyStateGetter, 0, 0);

        antb_set_native(history,
                        historyForFrame(engine, document ? document->frame() : 0, document),
                        AGB_TAG_HISTORY);

        {
            HistoryObjectEntry entry;

            entry.engine = engine;
            entry.document = key;
            entry.object = antb_retain(history);
            g_historyObjects->push_back(entry);
        }

        return history;
    }
}

/* ------------------------------------------------------------------------- */
/* teardown                                                                  */
/* ------------------------------------------------------------------------- */

void forgetDocument(AntbEngine* engine, WebCore::Document* document)
{
    size_t i;

    if (!engine || !document)
        return;

    if (g_locations) {
        for (i = 0; i < g_locations->size(); ) {
            if ((*g_locations)[i].engine == engine
                    && (*g_locations)[i].document == document) {
                if ((*g_locations)[i].object)
                    antb_value_release((*g_locations)[i].object);
                g_locations->erase(g_locations->begin() + i);
            } else {
                ++i;
            }
        }
    }

    if (g_historyObjects) {
        for (i = 0; i < g_historyObjects->size(); ) {
            if ((*g_historyObjects)[i].engine == engine
                    && (*g_historyObjects)[i].document == document) {
                if ((*g_historyObjects)[i].object)
                    antb_value_release((*g_historyObjects)[i].object);
                g_historyObjects->erase(g_historyObjects->begin() + i);
            } else {
                ++i;
            }
        }
    }

    /* The browsing context's entries survive; the document pointer must not. */
    if (g_histories) {
        for (i = 0; i < g_histories->size(); ++i) {
            FrameHistory* history = (*g_histories)[i];

            if (history->engine == engine && history->document == document)
                history->document = 0;
        }
    }
}

void releaseEngine(AntbEngine* engine)
{
    size_t i;

    if (g_locations) {
        for (i = 0; i < g_locations->size(); ) {
            if ((*g_locations)[i].engine == engine) {
                if ((*g_locations)[i].object)
                    antb_value_release((*g_locations)[i].object);
                g_locations->erase(g_locations->begin() + i);
            } else {
                ++i;
            }
        }
    }

    if (g_historyObjects) {
        for (i = 0; i < g_historyObjects->size(); ) {
            if ((*g_historyObjects)[i].engine == engine) {
                if ((*g_historyObjects)[i].object)
                    antb_value_release((*g_historyObjects)[i].object);
                g_historyObjects->erase(g_historyObjects->begin() + i);
            } else {
                ++i;
            }
        }
    }

    if (g_histories) {
        for (i = 0; i < g_histories->size(); ) {
            FrameHistory* history = (*g_histories)[i];

            if (history->engine == engine) {
                for (size_t j = 0; j < history->entries.size(); ++j)
                    releaseHistoryEntry(history->entries[j]);
                delete history;
                g_histories->erase(g_histories->begin() + i);
            } else {
                ++i;
            }
        }
    }
}

} /* namespace RealmDocument */
} /* namespace AntRuntime */
