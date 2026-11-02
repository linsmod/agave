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
    AntbValue*         locationObj;
    AntbValue*         docObj;          /* the JS document object (rooted) */
    bool               documentBound;
    void*              listeners;       /* std::vector<Listener>* (see below) */
    WebCore::Document* docSlots[AGB_MAX_DOCUMENT_SLOTS];
    int                docSlotCount;
    int                currentSlot;
};

static std::vector<RuntimeState*>* g_states = NULL;
/* Never NULL: the mirrors below are dereferenced by code that can run before a
 * real state exists (isRunning(), teardown paths). */
static RuntimeState  g_emptyState;
static RuntimeState* g_currentState = &g_emptyState;

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
#define g_locationObj   (g_currentState->locationObj)
#define g_docObj        (g_currentState->docObj)
#define g_documentBound (g_currentState->documentBound)
#define g_docSlots      (g_currentState->docSlots)
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

    if (g_docSlotCount >= AGB_MAX_DOCUMENT_SLOTS)
        return -1;

    g_docSlots[g_docSlotCount] = document;
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
WebCore::Document* documentOf(AntbValue* self);
int docGetElementById(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int nodeDispatchEvent(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int locationGetter(AntbEngine*, AntbValue*, const char*, AntbValue*, void*);
int locationSetter(AntbEngine*, AntbValue*, const char*, const AntbValue*, void*);
int locationAssign(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int locationReplace(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
int locationReload(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
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
struct XhrJob : public WebCore::ResourceHandleClient {
    XhrJob(AntbEngine* engine, AntbValue* callback)
        : m_engine(engine)
        , m_callback(antb_retain(callback))
        , m_status(0)
        , m_done(false)
        , m_failed(false)
        , m_startMs(0)
        , m_doneMs(0)
        , m_slot(g_currentSlot)
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

        if (!cookies.empty())
            request.addHTTPHeaderField(WebCore::String("Cookie"),
                                       WebCore::String::fromUTF8(cookies.c_str()));


        m_handle = WebCore::ResourceHandle::create(request, this, false, true, false);
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
int globalClassOp(AntbEngine*, AntbValue*, AntbValue**, int, AntbValue*, void*);
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

/* Persisted cookies live in <data dir>/cookies.txt, one record per line:
 *   domain <TAB> path <TAB> secure <TAB> name <TAB> value
 * The data dir is %APPDATA%\Agave when writable, otherwise the exe folder. */
static const char* dataDir(void)
{
    static char directory[AGB_PATH_CHARS];
    static int  ready = 0;

    if (ready)
        return directory;

    ready = 1;
    directory[0] = 0;

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

static void saveStorage()
{
    FILE* file;

    if (g_storageFile.empty())
        return;

    file = fopen(g_storageFile.c_str(), "wb");

    if (!file)
        return;

    {
        std::vector<std::pair<std::string, std::string> >& entries = storage();

        for (size_t i = 0; i < entries.size(); ++i) {
            fprintf(file, "%s\t%s\n",
                    escapeStorage(entries[i].first).c_str(),
                    escapeStorage(entries[i].second).c_str());
        }
    }

    fclose(file);
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

    ModuleEntry() : done(false), failed(false), id(0) {}
};

static std::map<std::string, ModuleEntry*>* g_moduleCache = NULL;
static Net::Session* g_moduleSession = NULL;
static std::vector<std::string>* g_modulePending = NULL;
static int g_moduleInFlight = 0;
static const int kModuleFetchLimit = 6;

static void moduleStart(const std::string& url, bool force);

/* Imports discovered while the fetch slots are full wait here; a finished
 * request immediately starts the next one, so the graph loads ~6 wide instead
 * of one file at a time. */
static void moduleQueuePending(const std::string& url)
{
    if (!g_modulePending)
        g_modulePending = new std::vector<std::string>();

    for (size_t i = 0; i < g_modulePending->size(); ++i) {
        if ((*g_modulePending)[i] == url)
            return;
    }

    g_modulePending->push_back(url);
}

static void moduleDrainPending(void)
{
    while (g_modulePending && !g_modulePending->empty()
            && g_moduleInFlight < kModuleFetchLimit) {
        std::string url = g_modulePending->front();

        g_modulePending->erase(g_modulePending->begin());
        moduleStart(url, true);
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

    (*g_moduleCache)[url] = entry;
    return entry;
}

struct ModuleFetchClient : public Net::Client {
    std::string url;

    explicit ModuleFetchClient(const std::string& u) : url(u) {}

    ModuleEntry* target() const
    {
        if (!g_moduleCache)
            return NULL;

        std::map<std::string, ModuleEntry*>::iterator it = g_moduleCache->find(url);

        return (it == g_moduleCache->end()) ? NULL : it->second;
    }

    virtual void onResponse(const Net::Response&) {}

    virtual void onData(const char* data, unsigned int length)
    {
        ModuleEntry* entry = target();

        if (entry && data && length)
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

            moduleDrainPending();
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
        }
    }
};

/* Resolves a relative specifier against the URL of the module holding it. */
static std::string moduleUrlJoin(const std::string& baseUrl, const std::string& specifier)
{
    std::string out;
    std::string host;
    size_t schemeEnd;
    size_t slash;
    size_t i;

    if (specifier.empty())
        return specifier;

    if (specifier.compare(0, 7, "http://") == 0 || specifier.compare(0, 8, "https://") == 0)
        return specifier;

    schemeEnd = baseUrl.find("://");

    if (schemeEnd == std::string::npos)
        return specifier;

    slash = baseUrl.find('/', schemeEnd + 3);
    host = (slash == std::string::npos) ? baseUrl : baseUrl.substr(0, slash);

    if (!specifier.empty() && specifier[0] == '/')
        return host + specifier;

    out = (slash == std::string::npos) ? host + "/" : baseUrl.substr(0, slash + 1);

    for (i = 0; i < specifier.size(); ++i) {
        if (specifier.compare(i, 3, "../") == 0) {
            size_t cut = out.size();

            if (cut > host.size() + 1) {
                cut -= 1;
                while (cut > host.size() && out[cut - 1] != '/')
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

static void moduleStart(const std::string& url, bool force)
{
    ModuleEntry* entry = moduleEntryFor(url);

    if (entry->done || entry->id)
        return;

    if (g_moduleInFlight >= kModuleFetchLimit) {
        if (!force)
            moduleQueuePending(url);
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

        entry->id = g_moduleSession->start(request, new ModuleFetchClient(url));

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

    if (outSize)
        *outSize = 0;

    if (url.empty())
        return NULL;

    entry = moduleEntryFor(url);

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
        fprintf(stderr, "[modfetch] %s %u bytes | in-flight=%d\n",
                url.c_str(), (unsigned)entry->body.size(), g_moduleInFlight);

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

int evaluate(const char* src, size_t len, char** out, char** err)
{
    AntbValue* value;

    if (out)
        *out = NULL;
    if (err)
        *err = NULL;

    if (!g_engine || !src)
        return 0;

    /* ES module syntax cannot be evaluated as a classic script. */
    if (looksLikeModule(src, len))
        return evaluateModule(src, len, g_currentFilename.c_str(), out, err);

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
"    var _origin = g.location && g.location.origin ? g.location.origin : '';"
"    if (!_origin) { _origin = (g.location && g.location.protocol ? g.location.protocol : '') + '//' + (g.location && g.location.host ? g.location.host : ''); }"
"    g.localStorage = {"
"      getItem: function (k) { var key = _origin + '|' + String(k); return __agbStorageHas(key) ? String(__agbStorageGet(key)) : null; },"
"      setItem: function (k, v) { __agbStorageSet(_origin + '|' + String(k), String(v)); },"
"      removeItem: function (k) { __agbStorageDel(_origin + '|' + String(k)); },"
"      clear: function () { __agbStorageClear(_origin); },"
"      key: function (i) { var v = __agbStorageKey(_origin, i); return v === undefined ? null : v; },"
"      get length() { var n = __agbStorageCount(_origin); return n > 0 ? n : 0; }"
"    };"
"    var _ssData = {};"
"    g.sessionStorage = {"
"      getItem: function (k) { return Object.prototype.hasOwnProperty.call(_ssData, k) ? _ssData[k] : null; },"
"      setItem: function (k, v) { _ssData[k] = String(v); },"
"      removeItem: function (k) { delete _ssData[k]; },"
"      clear: function () { _ssData = {}; },"
"      key: function (i) { var ks = Object.keys(_ssData); return i < ks.length ? ks[i] : null; },"
"      get length() { return Object.keys(_ssData).length; }"
"    };"
"  }"
"  if (!g.requestIdleCallback) { g.requestIdleCallback = function (cb) { return 1; }; g.cancelIdleCallback = function () {}; }"
"  if (!g.createImageBitmap) g.createImageBitmap = function () { return null; };"
"  if (!g.__agbStyleApi) {"
"    g.__agbStyleApi = {"
"      setProperty: function (name, value, priority) { this[String(name)] = value; },"
"      getPropertyValue: function (name) { var v = this[String(name)]; return v === undefined || v === null ? '' : String(v); },"
"      removeProperty: function (name) { var old = this[String(name)]; this[String(name)] = ''; return old === undefined ? '' : old; },"
"      item: function (i) { return ''; },"
"      length: 0"
"    };"
"  }"
"  if (!g.CSS) g.CSS = {};"
"  if (!g.CSS.supports) {"
"    g.CSS.supports = function (a, b) {"
"      var decl, el, name, value, got;"
"      if (b !== undefined) { decl = String(a) + ':' + String(b); }"
"      else { decl = String(a); }"
"      var colon = decl.indexOf(':');"
"      if (colon < 0) {"
"        /* 'prop: value' was not given: treat the whole string as a declaration only"
"           when it contains a colon, otherwise the query is a property name. */"
"        return false;"
"      }"
"      name = decl.slice(0, colon).replace(/^\\s+|\\s+$/g, '');"
"      value = decl.slice(colon + 1).replace(/^\\s+|\\s+$/g, '');"
"      el = g.document ? g.document.createElement('div') : null;"
"      if (!el || !name || !value) return false;"
"      try {"
"        if (el.style.setProperty) el.style.setProperty(name, value);"
"        else el.style[name] = value;"
"        got = el.style.getPropertyValue ? el.style.getPropertyValue(name) : el.style[name];"
"      } catch (e) { return false; }"
"      return !!(got && String(got).length);"
"    };"
"  }"
"  if (!g.CSS.escape) g.CSS.escape = function (s) { return String(s); };"
"  if (!g.matchMedia) {"
"    g.matchMedia = function (query) {"
"      var list = {"
"        media: String(query),"
"        matches: false,"
"        onchange: null,"
"        addListener: function () {},"
"        removeListener: function () {},"
"        addEventListener: function () {},"
"        removeEventListener: function () {},"
"        dispatchEvent: function () { return false; }"
"      };"
"      return list;"
"    };"
"  }"
"  {"
"    var __evtHandlers = ['onload','onerror','onunload','onbeforeunload','onresize','onscroll','onhashchange',"
"      'onpopstate','onmessage','onfocus','onblur','onclick','onmousedown','onmouseup','onmousemove','onkeydown',"
"      'onkeyup','onkeypress','ondblclick','oncontextmenu','onwheel','ontouchstart','ontouchend','ontouchmove',"
"      'onreadystatechange','onstorage','onoffline','ononline','onbeforeprint','onafterprint','onpageshow','onpagehide'];"
"    for (var __i = 0; __i < __evtHandlers.length; ++__i) {"
"      try { if (typeof g[__evtHandlers[__i]] === 'undefined') g[__evtHandlers[__i]] = null; } catch (e) {}"
"      /* Assigning `onload = fn` is the old way of adding a handler; bind it to a"
"         real listener so the event actually runs it. */"
"      try {"
"        if (g.addEventListener && Object.defineProperty) {"
"          (function (name) {"
"            var slot = null;"
"            Object.defineProperty(g, name, {"
"              configurable: true, enumerable: true,"
"              get: function () { return slot; },"
"              set: function (fn) {"
"                if (slot) { try { g.removeEventListener(name.slice(2), slot); } catch (e) {} }"
"                slot = fn;"
"                if (typeof fn === 'function') { try { g.addEventListener(name.slice(2), fn); } catch (e) {} }"
"              }"
"            });"
"          })(__evtHandlers[__i]);"
"        }"
"      } catch (e) {}"
"    }"
"    try { if (typeof g.name === 'undefined') g.name = ''; } catch (e) {}"
"  }"
"  if (!g.getComputedStyle) {"
"    g.__computedCamel = ['backgroundColor','backgroundImage','backgroundRepeat','backgroundPosition',"
"      'backgroundSize','backgroundAttachment','backgroundClip','backgroundOrigin','color','width',"
"      'height','display','position','top','left','right','bottom','marginTop','marginLeft','marginRight',"
"      'marginBottom','paddingTop','paddingLeft','paddingRight','paddingBottom','borderTopWidth',"
"      'borderLeftWidth','borderTopColor','borderColor','fontSize','fontFamily','fontWeight','lineHeight',"
"      'opacity','visibility','overflow','zIndex','textAlign','verticalAlign','float','clear','cursor',"
"      'whiteSpace','textDecoration','borderRadius','boxShadow','transform'];"
"    g.getComputedStyle = function (el) {"
"      var st = (el && el.style) ? el.style : {};"
"      function camel(n) { return String(n).replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); }); }"
"      function dash(n) { return String(n).replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); }); }"
"      var api = {"
"        getPropertyValue: function (n) {"
"          var name = String(n);"
"          var v = '';"
"          try { v = __agbComputedStyle(el, name); } catch (e) { v = ''; }"
"          if (v) return String(v);"
"          v = st[camel(name)];"
"          if (v === undefined || v === null || v === '') {"
"            if (name === 'width') return el ? (el.offsetWidth + 'px') : '';"
"            if (name === 'height') return el ? (el.offsetHeight + 'px') : '';"
"            return '';"
"          }"
"          return String(v);"
"        },"
"        setProperty: function (n, v) { st[camel(n)] = v; },"
"        removeProperty: function (n) { var c = camel(n); var old = st[c]; st[c] = ''; return old; },"
"        get length() { var i = 0; for (var k in st) i++; return i; },"
"        item: function (i) { var n = 0; for (var k in st) { if (n === i) return k; n++; } return ''; }"
"      };"
"      for (var i = 0; i < g.__computedCamel.length; ++i) {"
"        (function (name) {"
"          var cssName = dash(name);"
"          try {"
"            Object.defineProperty ? Object.defineProperty(api, name, {"
"              get: function () { return api.getPropertyValue(cssName); },"
"              enumerable: true, configurable: true"
"            }) : (api[name] = api.getPropertyValue(cssName));"
"          } catch (e) { api[name] = ''; }"
"        })(g.__computedCamel[i]);"
"      }"
"      return api;"
"    };"
"  }"
"  {"
"    var _ctors = ['Document','HTMLDocument','Element','HTMLElement','HTMLDivElement','Node','NodeList','HTMLCollection','Event','CustomEvent','MouseEvent','KeyboardEvent','DocumentFragment','Text','Comment','Attr','DOMParser','XMLSerializer','MutationObserver','Storage','Location','Navigator','Screen','History','Window','Performance','CSSStyleDeclaration'];"
"  if (typeof g.innerWidth === 'undefined') {"
"    Object.defineProperty ? Object.defineProperty(g, 'innerWidth', { get: function () { return g.document && g.document.documentElement ? g.document.documentElement.clientWidth : 0; } }) : (g.innerWidth = 0);"
"    Object.defineProperty ? Object.defineProperty(g, 'innerHeight', { get: function () { return g.document && g.document.documentElement ? g.document.documentElement.clientHeight : 0; } }) : (g.innerHeight = 0);"
"    g.outerWidth = g.innerWidth; g.outerHeight = g.innerHeight;"
"    g.pageXOffset = 0; g.pageYOffset = 0; g.scrollX = 0; g.scrollY = 0;"
"    g.scrollTo = function (x, y) { if (g.document && g.document.documentElement) { g.document.documentElement.scrollLeft = x; g.document.documentElement.scrollTop = y; } };"
"    g.scrollBy = function (x, y) { g.scrollTo((g.pageXOffset||0)+x, (g.pageYOffset||0)+y); };"
"  }"
"  g.__agbSafe = function (fn, label) {"
"    return function () {"
"      try { return fn.apply(this, arguments); }"
"      catch (e) {"
"        var msg = (g.__agbDescribe ? g.__agbDescribe(e) : String(e));"
"        if (g.console && g.console.error) g.console.error('uncaught' + (label ? ' (' + label + ')' : '') + ': ' + msg);"
"      }"
"    };"
"  };"
"  /* Timers run later, possibly while another frame/control is active: capture"
"     the current document slot and restore it around the callback. */"
"  g.__agbBind = function (fn, label) {"
"    var safe = g.__agbSafe(fn, label);"
"    var slot = (typeof g.__agbFrameSlot === 'function') ? g.__agbFrameSlot() : -1;"
"    if (slot < 0) return safe;"
"    return function () {"
"      var old = g.__agbActivateSlot(slot);"
"      try { return safe.apply(this, arguments); }"
"      finally { g.__agbActivateSlot(old); }"
"    };"
"  };"
"  g.open = function (url) {"
"    var ok = false;"
"    try { ok = !!__agbOpenWindow(url == null ? '' : String(url)); } catch (e) { ok = false; }"
"    if (!ok) return null;"
"    return { closed: false, focus: function () {}, blur: function () {},"
"      close: function () { this.closed = true; }, postMessage: function () {} };"
"  };"
"  g.close = function () {};"
"  g.agavePostMessage = function (message) {"
"    try { __agbPostMessage(message == null ? '' : String(message)); } catch (e) {}"
"  };"
"  g.alert = function (message) { try { __agbDialog(0, message == null ? '' : String(message), ''); } catch (e) {} };"
"  g.confirm = function (message) { try { return !!__agbDialog(1, message == null ? '' : String(message), ''); } catch (e) { return false; } };"
"  g.prompt = function (message, fallback) { try { var r = __agbDialog(2, message == null ? '' : String(message), fallback == null ? '' : String(fallback)); return r === '' || r === null || r === undefined ? null : String(r); } catch (e) { return null; } };"
"  if (typeof g.setTimeout === 'function' && !g.__agbTimersWrapped) {"
"    g.__agbTimersWrapped = true;"
"    var _st = g.setTimeout, _si = g.setInterval;"
"    g.setTimeout = function (fn, delay) {"
"      if (typeof fn !== 'function') return _st.apply(g, arguments);"
"      var rest = Array.prototype.slice.call(arguments, 2);"
"      return _st.apply(g, [g.__agbBind(fn, 'timeout'), delay].concat(rest));"
"    };"
"    if (typeof _si === 'function') {"
"      g.setInterval = function (fn, delay) {"
"        if (typeof fn !== 'function') return _si.apply(g, arguments);"
"        var rest = Array.prototype.slice.call(arguments, 2);"
"        return _si.apply(g, [g.__agbBind(fn, 'interval'), delay].concat(rest));"
"      };"
"    }"
"  }"
"  if (!g.performance) { g.performance = {}; }"
"  if (!g.performance.timing) {"
"    g.performance.timing = { navigationStart: 0, fetchStart: 0, domainLookupStart: 0, domainLookupEnd: 0,"
"      connectStart: 0, connectEnd: 0, requestStart: 0, responseStart: 0, responseEnd: 0,"
"      domLoading: 0, domInteractive: 0, domContentLoadedEventStart: 0, domContentLoadedEventEnd: 0,"
"      domComplete: 0, loadEventStart: 0, loadEventEnd: 0 };"
"  }"
"  if (!g.performance.now) { g.performance.now = function () { return (new Date()).getTime(); }; }"
"  if (!g.performance.getEntriesByType) { g.performance.getEntriesByType = function () { return []; }; }"
"  g.__agbMakeEvent = function (type, target) {"
"    return {"
"      type: type, target: target, currentTarget: target,"
"      bubbles: true, cancelable: true, eventPhase: 2, timeStamp: (new Date()).getTime(),"
"      defaultPrevented: false, returnValue: true,"
"      __prevented: false, __stopped: false, __stoppedImmediate: false,"
"      preventDefault: function () { this.__prevented = true; this.defaultPrevented = true; this.returnValue = false; },"
"      stopPropagation: function () { this.__stopped = true; },"
"      stopImmediatePropagation: function () { this.__stopped = true; this.__stoppedImmediate = true; }"
"    };"
"  };"
"  g.__agbMakeClassList = function (el) {"
"    return {"
"      add: function (t) { __agbClassOp(el, 'add', t); },"
"      remove: function (t) { __agbClassOp(el, 'remove', t); },"
"      contains: function (t) { return !!__agbClassOp(el, 'contains', t); },"
"      toggle: function (t) { return !!__agbClassOp(el, 'toggle', t); }"
"    };"
"  };"
"  g.window = g; g.self = g; g.top = g; g.parent = g; g.frames = g;"
"  if (!g.frameElement) g.frameElement = null;"
"  if (typeof g.requestAnimationFrame === 'undefined') {"
"    g.requestAnimationFrame = function (cb) { return setTimeout(function () { cb((new Date()).getTime()); }, 16); };"
"    g.cancelAnimationFrame = function (id) { clearTimeout(id); };"
"  }"
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
"  /* fetch(): ant's own implementation expects its CLI network stack, so back it"
"   * with our async XHR instead. */"
"  if (typeof Promise === 'function') {"
"    g.fetch = function (input, init) {"
"      var url = (typeof input === 'string') ? input : (input && input.url);"
"      var opts = init || {};"
"      return new Promise(function (resolve, reject) {"
"        var x = new XMLHttpRequest();"
"        x.open(opts.method || 'GET', url, true);"
"        if (opts.headers) { for (var k in opts.headers) x.setRequestHeader(k, opts.headers[k]); }"
"        x.onload = function () {"
"          resolve({"
"            ok: x.status >= 200 && x.status < 300,"
"            status: x.status, statusText: x.statusText, url: url,"
"            headers: { get: function (n) { return x.getResponseHeader(n); } },"
"            text: function () { return Promise.resolve(x.responseText); },"
"            json: function () { try { return Promise.resolve(JSON.parse(x.responseText)); } catch (e) { return Promise.reject(e); } }"
"          });"
"        };"
"        x.onerror = function () { reject(new Error('network error: ' + url)); };"
"        x.send(opts.body || null);"
"      });"
"    };"
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
    /* Deliver finished requests first (JS callbacks), then let the engine run
     * its own timers/microtasks - both happen on the frame loop, never inside
     * the network pump. */
    collectXhrJobs();

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
};

static std::vector<DeferredModule>* g_deferredModules = NULL;

int evaluateModule(const char* src, size_t len, const char* filename, char** out, char** err);

void queueModule(const char* url, const char* source)
{
    if (!g_deferredModules)
        g_deferredModules = new std::vector<DeferredModule>();

    DeferredModule module;

    module.url = url ? url : "";
    module.source = source ? source : "";
    g_deferredModules->push_back(module);
}

void flushDeferredModules(void)
{
    std::vector<DeferredModule> pending;

    if (!g_deferredModules || g_deferredModules->empty())
        return;

    /* Take the list first: running a module may queue more (dynamic import). */
    pending = *g_deferredModules;
    g_deferredModules->clear();

    for (size_t i = 0; i < pending.size(); ++i) {
        char* err = NULL;

        evaluateModule(pending[i].source.c_str(), pending[i].source.size(),
                       pending[i].url.c_str(), NULL, &err);

        if (err) {
            fprintf(stderr, "[antjs] %s: %s\n", pending[i].url.c_str(), err);
            free(err);
        }
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

        value = antb_eval_module(g_engine, src, (int)len, filename, err);

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

/* Loads polyfills.js (next to the binary, or $AGB_POLYFILLS) and evaluates it.
 * Kept out of the built-in prelude so the file can be edited without relinking
 * the engine. */
static void loadPolyfills(void)
{
    static int loaded = 0;
    char path[AGB_PATH_CHARS];
    char* buffer = NULL;
    FILE* file = NULL;
    long size = 0;
    const char* env = getenv("AGB_POLYFILLS");

    if (loaded)
        return;

    loaded = 1;

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

void bindDocument(void* document)
{
    /* The Document object is replaced on every navigation, so always refresh
     * the pointer; only the JS shim objects are created once. */
    g_document = (WebCore::Document*)document;
    g_currentSlot = slotForDocument(g_document);

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
        antb_define_property_rw(g_engine, docObj, "cookie", titleGetter, titleSetter, NULL);

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
                antb_class_add_method(g_engine, g_nodeClass, "getBoundingClientRect", nodeGetBoundingClientRect, NULL);
            }
        }

        antb_define_method(g_engine, docObj, "getElementById", docGetElementById, NULL);
        antb_define_method(g_engine, docObj, "createElement", docCreateElement, NULL);
        antb_define_method(g_engine, docObj, "createTextNode", docCreateTextNode, NULL);
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
        antb_define_method(g_engine, global, "__agbClassOp", globalClassOp, NULL);
        antb_define_method(g_engine, global, "__agbFrameSlot", globalFrameSlot, NULL);
        antb_define_method(g_engine, global, "__agbActivateSlot", globalActivateSlot, NULL);
        antb_define_method(g_engine, global, "__agbOpenWindow", globalOpenWindow, NULL);
        antb_define_method(g_engine, global, "__agbPostMessage", globalPostMessage, NULL);
        antb_define_method(g_engine, global, "__agbDialog", globalDialog, NULL);
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
    /* Ant's Web Worker constructor crashes on anything but a usable script URL
     * (it strdups a NULL) and workers need an environment we do not provide.
     * Hiding the constructors makes pages take their non-worker path instead
     * of killing the process. */
    if (g_engine) {
        static const char kHideWorkers[] =
            "try{window.Worker=undefined}catch(e){}"
            "try{window.SharedWorker=undefined}catch(e){}"
            "try{window.ServiceWorker=undefined}catch(e){}";

        antb_eval(g_engine, kHideWorkers, (int)strlen(kHideWorkers), NULL);
    }

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

    /* setProperty / getPropertyValue / removeProperty come from the prelude
     * helper object, so they behave like prototype methods. */
    if (!strcmp(name, "setProperty") || !strcmp(name, "getPropertyValue")
            || !strcmp(name, "removeProperty") || !strcmp(name, "item")
            || !strcmp(name, "length")) {
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

int nodeGetter(AntbEngine* engine, AntbValue* self, const char* name, AntbValue* out, void*)
{
    WebCore::Node* n = nodeOf(self);
    AntbValue* v = NULL;

    if (!n)
        return 0;

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
        /* Only functions are callable later: pages happily pass null or objects
         * as the listener, and dispatching those crashed the process. */
        l.fn = antb_is_function(args[1]) ? antb_retain(args[1]) : NULL;
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


/* getComputedStyle(el).getPropertyValue(name) -> computed value as a string.
 * Goes through WebCore's CSSComputedStyleDeclaration, so every property the
 * layout engine knows about comes back with a real value. */
int globalComputedStyle(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                        AntbValue* out, void*)
{
    WebCore::Node* node = (argc > 0) ? nodeOf(args[0]) : NULL;
    char* property = (argc > 1) ? antb_to_utf8(args[1]) : NULL;
    std::string value;
    static int calls = 0;
    static unsigned long totalUs = 0;
    unsigned long beganUs = 0;
    bool timed = getenv("AGB_CSS_DEBUG") != NULL;

    if (timed)
        beganUs = (unsigned long)GetTickCount();

    if (node && node->document() && property && property[0]) {
        RefPtr<WebCore::CSSComputedStyleDeclaration> declaration =
            new WebCore::CSSComputedStyleDeclaration(node);

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
    return makeStringResult(engine, out, value.c_str()) ? 1 : 0;
}

int globalOpenWindow(AntbEngine* engine, AntbValue*, AntbValue** args, int argc,
                     AntbValue* out, void*)
{
    char* url = (argc > 0) ? antb_to_utf8(args[0]) : NULL;
    int created = 0;
    AntbValue* v;

    if (g_openWindowHook)
        created = g_openWindowHook(url ? url : "");

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

AntbValue* wrapNode(AntbEngine* engine, WebCore::Node* node)
{
    AntbValue* v;

    if (!engine || !node || !g_nodeClass)
        return NULL;

    /* The wrapper cache is keyed by the raw node pointer, so a node that gets
     * destroyed and a new one allocated at the same address would share a
     * wrapper (use-after-free). Keep every wrapped node alive instead: bounded
     * by the document lifetime, and test pages survive hundreds of wraps. */
    {
        static std::set<WebCore::Node*>* pinned = new std::set<WebCore::Node*>();

        if (pinned->find(node) == pinned->end()) {
            node->ref();
            pinned->insert(node);
        }
    }

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
 * object so the caller can inspect the preventDefault flag. */
static void runListenersFor(WebCore::Node* node, const std::string& type, AntbValue* evt,
                            AntbValue* currentTarget, bool& stopped, bool& stoppedImmediate)
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
        bool stopped = false;
        bool stoppedImmediate = false;
        if (!evt)
            return;
        /* Bubble: target → ancestors → document. */
        for (WebCore::Node* current = node; current && !stopped; current = current->parentNode()) {
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
