/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 *           (C) 2006 Alexey Proskuryakov (ap@webkit.org)
 * Copyright (C) 2004, 2005, 2006, 2007 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef Document_h
#define Document_h

#include "Attr.h"
#include "Color.h"
#include "DeprecatedPtrList.h"
#include "DeprecatedValueList.h"
#include "DocumentMarker.h"
#include "HTMLCollection.h"
#include "HTMLFormElement.h"
#include "KURL.h"
#include "StringHash.h"
#include "StyleCustomPropertyData.h"
#include "Timer.h"
#include <wtf/HashCountedSet.h>
#include <wtf/HashMap.h>
#include <wtf/ListHashSet.h>

namespace WebCore {

    class Attr;
    class Attribute;
    class CDATASection;
    class CSSStyleDeclaration;
    class CSSStyleSelector;
    class CSSStyleSheet;
    class Comment;
    class DOMImplementation;
    class DOMWindow;
    class DocLoader;
    class DocumentFragment;
    class DocumentType;
    class EditingText;
    class Element;
    class EntityReference;
    class Event;
    class EventListener;
    class Frame;
    class FrameView;
    class HTMLDocument;
    class HTMLElement;
    class HTMLFormControlElementWithState;
    class HTMLFormElement;
    class HTMLHeadElement;
    class HTMLImageLoader;
    class HTMLInputElement;
    class HTMLMapElement;
    class IntPoint;
    class JSEditor;
    class MouseEventWithHitTestResults;
    class NameNodeList;
    class NodeFilter;
    class NodeIterator;
    class NodeList;
    class Page;
    class PlatformMouseEvent;
    class ProcessingInstruction;
    class Range;
    class RegisteredEventListener;
    class RenderArena;
    class RenderView;
    class Settings;
    class StyleSheet;
    class StyleSheetList;
    class Text;
    class TextResourceDecoder;
    class Tokenizer;
    class TreeWalker;
#if ENABLE(XBL)
    class XBLBindingManager;
#endif
#if ENABLE(XPATH)
    class XPathEvaluator;
    class XPathExpression;
    class XPathNSResolver;
    class XPathResult;
#endif
    
    struct DashboardRegionValue;
    struct HitTestRequest;

#if ENABLE(SVG)
    class SVGDocumentExtensions;
#endif

    typedef int ExceptionCode;

class FormElementKey {
public:
    FormElementKey(AtomicStringImpl* = 0, AtomicStringImpl* = 0);
    ~FormElementKey();
    FormElementKey(const FormElementKey&);
    FormElementKey& operator=(const FormElementKey&);
    AtomicStringImpl* name() const { return m_name; }
    AtomicStringImpl* type() const { return m_type; }
private:
    void ref() const;
    void deref() const;
    AtomicStringImpl* m_name;
    AtomicStringImpl* m_type;
};

inline bool operator==(const FormElementKey& a, const FormElementKey& b)
    { return a.name() == b.name() && a.type() == b.type(); }

struct FormElementKeyHash {
    static unsigned hash(const FormElementKey&);
    static bool equal(const FormElementKey& a, const FormElementKey& b) { return a == b; }
};

struct FormElementKeyHashTraits : WTF::GenericHashTraits<FormElementKey> {
    static FormElementKey deletedValue();
};

class Document : public ContainerNode {
public:
    Document(DOMImplementation*, Frame*, bool isXHTML = false);
    virtual ~Document();

    virtual void removedLastRef();

    // Nodes belonging to this document hold "self-only" references -
    // these are enough to keep the document from being destroyed, but
    // not enough to keep it from removing its children. This allows a
    // node that outlives its document to still have a valid document
    // pointer without introducing reference cycles

    void selfOnlyRef()
    {
        ASSERT(!m_deletionHasBegun);
        ++m_selfOnlyRefCount;
    }
    void selfOnlyDeref()
    {
        ASSERT(!m_deletionHasBegun);
        --m_selfOnlyRefCount;
        if (!m_selfOnlyRefCount && !refCount()) {
#ifndef NDEBUG
            m_deletionHasBegun = true;
#endif
            delete this;
        }
    }

    // DOM methods & attributes for Document

    virtual DocumentType* doctype() const; // May return 0 for HTML documents
    DocumentType* realDocType() const { return m_docType.get(); }

    DOMImplementation* implementation() const;
    virtual void childrenChanged();
    Element* documentElement() const;
    virtual PassRefPtr<Element> createElement(const String& tagName, ExceptionCode&);
    PassRefPtr<DocumentFragment> createDocumentFragment ();
    PassRefPtr<Text> createTextNode(const String& data);
    PassRefPtr<Comment> createComment(const String& data);
    PassRefPtr<CDATASection> createCDATASection(const String& data, ExceptionCode&);
    PassRefPtr<ProcessingInstruction> createProcessingInstruction(const String& target, const String& data, ExceptionCode&);
    PassRefPtr<Attr> createAttribute(const String& name, ExceptionCode& ec) { return createAttributeNS(String(), name, ec); }
    PassRefPtr<Attr> createAttributeNS(const String& namespaceURI, const String& qualifiedName, ExceptionCode&);
    PassRefPtr<EntityReference> createEntityReference(const String& name, ExceptionCode&);
    PassRefPtr<Node> importNode(Node* importedNode, bool deep, ExceptionCode&);
    virtual PassRefPtr<Element> createElementNS(const String& namespaceURI, const String& qualifiedName, ExceptionCode&);
    PassRefPtr<Element> createElement(const QualifiedName&, bool createdByParser, ExceptionCode& ec);
    Element* getElementById(const AtomicString&) const;

    Element* elementFromPoint(int x, int y) const;
    String readyState() const;
    String inputEncoding() const;
    String defaultCharset() const;

    String charset() const { return inputEncoding(); }
    String characterSet() const { return inputEncoding(); }

    void setCharset(const String&);

    String xmlEncoding() const { return m_xmlEncoding; }
    String xmlVersion() const { return m_xmlVersion; }
    bool xmlStandalone() const { return m_xmlStandalone; }

    void setXMLEncoding(const String& encoding) { m_xmlEncoding = encoding; } // read-only property, only to be set from XMLTokenizer
    void setXMLVersion(const String&, ExceptionCode&);
    void setXMLStandalone(bool, ExceptionCode&);

    String documentURI() const;
    void setDocumentURI(const String&);

    virtual String baseURI() const;

    PassRefPtr<Node> adoptNode(PassRefPtr<Node> source, ExceptionCode&);
    
    PassRefPtr<NameNodeList> getElementsByName(const String& elementName);

//extends for load
    void setAsync(bool, ExceptionCode&);
    bool async() const { return m_async; }
    bool load(const String &url);

    PassRefPtr<HTMLCollection> images();
    PassRefPtr<HTMLCollection> embeds();
    PassRefPtr<HTMLCollection> plugins(); // an alias for embeds() required for the JS DOM bindings.
    PassRefPtr<HTMLCollection> applets();
    PassRefPtr<HTMLCollection> links();
    PassRefPtr<HTMLCollection> forms();
    PassRefPtr<HTMLCollection> anchors();
    PassRefPtr<HTMLCollection> all();
    PassRefPtr<HTMLCollection> objects();
    PassRefPtr<HTMLCollection> scripts();
    PassRefPtr<HTMLCollection> windowNamedItems(const String& name);
    PassRefPtr<HTMLCollection> documentNamedItems(const String& name);

    HTMLCollection::CollectionInfo* collectionInfo(HTMLCollection::Type type)
    {
        if ((int)type < HTMLCollection::UnnamedCollectionTypes) 
            return m_collectionInfo + type; 
        return 0;
    }

    HTMLCollection::CollectionInfo* nameCollectionInfo(HTMLCollection::Type type, const String& name);

    // DOM methods overridden from  parent classes

    virtual String nodeName() const;
    virtual NodeType nodeType() const;

    // Other methods (not part of DOM)
    virtual bool isDocumentNode() const { return true; }
    virtual bool isHTMLDocument() const { return false; }
    virtual bool isImageDocument() const { return false; }
#if ENABLE(SVG)
    virtual bool isSVGDocument() const { return false; }
#endif
    virtual bool isPluginDocument() const { return false; }
    virtual bool isMediaDocument() const { return false; }
    
    CSSStyleSelector* styleSelector() const { return m_styleSelector; }

    Element* getElementByAccessKey(const String& key) const;
    
    /**
     * Updates the pending sheet count and then calls updateStyleSelector.
     */
    void removePendingSheet();

    /**
     * This method returns true if all top-level stylesheets have loaded (including
     * any @imports that they may be loading).
     */
    bool haveStylesheetsLoaded() const
    {
        return m_pendingStylesheets <= 0 || m_ignorePendingStylesheets
#if USE(LOW_BANDWIDTH_DISPLAY)
            || m_inLowBandwidthDisplay
#endif
            ;
    }

    /**
     * Increments the number of pending sheets.  The <link> elements
     * invoke this to add themselves to the loading list.
     */
    void addPendingSheet() { m_pendingStylesheets++; }

    bool gotoAnchorNeededAfterStylesheetsLoad() { return m_gotoAnchorNeededAfterStylesheetsLoad; }
    void setGotoAnchorNeededAfterStylesheetsLoad(bool b) { m_gotoAnchorNeededAfterStylesheetsLoad = b; }

    /**
     * Called when one or more stylesheets in the document may have been added, removed or changed.
     *
     * Creates a new style selector and assign it to this document. This is done by iterating through all nodes in
     * document (or those before <BODY> in a HTML document), searching for stylesheets. Stylesheets can be contained in
     * <LINK>, <STYLE> or <BODY> elements, as well as processing instructions (XML documents only). A list is
     * constructed from these which is used to create the a new style selector which collates all of the stylesheets
     * found and is used to calculate the derived styles for all rendering objects.
     */
    void updateStyleSelector();

    /* Applies a deferred style-sheet change: rebuilds the selector and forces a
     * document-wide style recalc. Called from every path that needs up-to-date
     * style (rendering, layout, computed-style reads). */
    void updateStyleIfNeeded();

    void recalcStyleSelector();

    /* The frame's viewport changed (window resize, iframe relayout). Two
     * things were computed for the old size: the @media rules folded into the
     * selector's rule sets while it was built, and the --agb-v* values that
     * stand in for vw/vh. Both are refreshed here; the caller follows with
     * updateStyleIfNeeded() so the new styles are actually applied. */
    void viewportSizeChanged();

    /* A stylesheet used Nvw/Nvh/Nvmin/Nvmax: the text now says
     * var(--agb-v<unit>-<N>) and this table remembers what N is. */
    void noteViewportUnit(int kind, double number, const String& variableName);

    /* A calc(...) expression containing viewport units. The grammar cannot
     * parse calc() at all (the 2008 productions have no '+'), let alone a
     * calc() with a var() inside, so the whole expression becomes one
     * variable whose value is the calc text with the units substituted at the
     * current viewport - recomputed on resize like the plain units. Returns
     * the custom property name the declaration has to carry. */
    String addViewportCalc(const String& calcText);

    /* The --agb-v* custom properties for the current viewport, cached until
     * the next resize. Style resolution seeds the root element with them. */
    PassRefPtr<StyleCustomPropertyData> viewportCustomProperties();

    bool usesDescendantRules() const { return m_usesDescendantRules; }
    void setUsesDescendantRules(bool b) { m_usesDescendantRules = b; }
    bool usesSiblingRules() const { return m_usesSiblingRules; }
    void setUsesSiblingRules(bool b) { m_usesSiblingRules = b; }
    bool usesFirstLineRules() const { return m_usesFirstLineRules; }
    void setUsesFirstLineRules(bool b) { m_usesFirstLineRules = b; }
    bool usesFirstLetterRules() const { return m_usesFirstLetterRules; }
    void setUsesFirstLetterRules(bool b) { m_usesFirstLetterRules = b; }

    // Machinery for saving and restoring state when you leave and then go back to a page.
    void registerFormElementWithState(HTMLFormControlElementWithState* e) { m_formElementsWithState.add(e); }
    void unregisterFormElementWithState(HTMLFormControlElementWithState* e) { m_formElementsWithState.remove(e); }
    Vector<String> formElementsState() const;
    void setStateForNewFormElements(const Vector<String>&);
    bool hasStateForNewFormElements() const;
    bool takeStateForFormElement(AtomicStringImpl* name, AtomicStringImpl* type, String& state);

    FrameView* view() const; // can be NULL
    Frame* frame() const { return m_frame; } // can be NULL
    Page* page() const; // can be NULL
    Settings* settings() const; // can be NULL

    PassRefPtr<Range> createRange();

    PassRefPtr<NodeIterator> createNodeIterator(Node* root, unsigned whatToShow,
        NodeFilter*, bool expandEntityReferences, ExceptionCode&);

    PassRefPtr<TreeWalker> createTreeWalker(Node* root, unsigned whatToShow, 
        NodeFilter*, bool expandEntityReferences, ExceptionCode&);

    // Special support for editing
    PassRefPtr<CSSStyleDeclaration> createCSSStyleDeclaration();
    PassRefPtr<EditingText> createEditingTextNode(const String&);

    virtual void recalcStyle( StyleChange = NoChange );
    static DeprecatedPtrList<Document>* changedDocuments;
    virtual void updateRendering();
    void updateLayout();
    void updateLayoutIgnorePendingStylesheets();
    static void updateDocumentsRendering();
    DocLoader* docLoader() { return m_docLoader; }

    virtual void attach();
    virtual void detach();

    RenderArena* renderArena() { return m_renderArena; }

    
    // to get visually ordered hebrew and arabic pages right
    void setVisuallyOrdered();

    void updateSelection();
    
    void open();
    void implicitOpen();
    void close();
    void implicitClose();
    void cancelParsing();

    void write(const String& text);
    void write(const DeprecatedString &text);
    void writeln(const String& text);
    void finishParsing();
    void clear();

    bool wellFormed() const { return m_wellFormed; }

    DeprecatedString URL() const { return m_url.isEmpty() ? "about:blank" : m_url; }
    void setURL(const DeprecatedString& url);

    DeprecatedString baseURL() const { return m_baseURL.isEmpty() ? URL() : m_baseURL; }
    void setBaseURL(const DeprecatedString& baseURL);

    String baseTarget() const { return m_baseTarget; }
    void setBaseTarget(const String& baseTarget) { m_baseTarget = baseTarget; }

    DeprecatedString completeURL(const DeprecatedString &);
    String completeURL(const String&);

    // from cachedObjectClient
    virtual void setCSSStyleSheet(const String& url, const String& charset, const String& sheetStr);
    void setUserStyleSheet(const String& sheet);
    const String& userStyleSheet() const { return m_usersheet; }
    void setPrintStyleSheet(const String& sheet) { m_printSheet = sheet; }
    const String& printStyleSheet() const { return m_printSheet; }

    CSSStyleSheet* elementSheet();
    virtual Tokenizer* createTokenizer();
    Tokenizer* tokenizer() { return m_tokenizer; }
    
    bool printing() const { return m_printing; }
    void setPrinting(bool p) { m_printing = p; }

    enum ParseMode { Compat, AlmostStrict, Strict };
    
    virtual void determineParseMode(const String&);
    void setParseMode(ParseMode m) { pMode = m; }
    ParseMode parseMode() const { return pMode; }

    bool inCompatMode() const { return pMode == Compat; }
    bool inAlmostStrictMode() const { return pMode == AlmostStrict; }
    bool inStrictMode() const { return pMode == Strict; }
    
    enum HTMLMode { Html3, Html4, XHtml };

    void setHTMLMode(HTMLMode m) { hMode = m; }
    HTMLMode htmlMode() const { return hMode; }

    void setParsing(bool);
    bool parsing() const { return m_bParsing; }
    int minimumLayoutDelay();
    bool shouldScheduleLayout();
    int elapsedTime() const;
    
    void setTextColor(const Color& color) { m_textColor = color; }
    Color textColor() const { return m_textColor; }

    const Color& linkColor() const { return m_linkColor; }
    const Color& visitedLinkColor() const { return m_visitedLinkColor; }
    const Color& activeLinkColor() const { return m_activeLinkColor; }
    void setLinkColor(const Color& c) { m_linkColor = c; }
    void setVisitedLinkColor(const Color& c) { m_visitedLinkColor = c; }
    void setActiveLinkColor(const Color& c) { m_activeLinkColor = c; }
    void resetLinkColor();
    void resetVisitedLinkColor();
    void resetActiveLinkColor();
    
    MouseEventWithHitTestResults prepareMouseEvent(const HitTestRequest&, const IntPoint&, const PlatformMouseEvent&);

    virtual bool childTypeAllowed(NodeType);
    virtual PassRefPtr<Node> cloneNode(bool deep);

    virtual bool canReplaceChild(Node* newChild, Node* oldChild);
    
    StyleSheetList* styleSheets();

    /* Newly proposed CSS3 mechanism for selecting alternate
       stylesheets using the DOM. May be subject to change as
       spec matures. - dwh
    */
    String preferredStylesheetSet() const;
    String selectedStylesheetSet() const;
    void setSelectedStylesheetSet(const String&);

    bool setFocusedNode(PassRefPtr<Node>);
    Node* focusedNode() const { return m_focusedNode.get(); }

    void setHoverNode(PassRefPtr<Node>);
    Node* hoverNode() const { return m_hoverNode.get(); }

    void setActiveNode(PassRefPtr<Node>);
    Node* activeNode() const { return m_activeNode.get(); }

    void focusedNodeRemoved(Node*);
    void hoveredNodeDetached(Node*);
    void activeChainNodeDetached(Node*);

    // Updates for :target (CSS3 selector).
    void setCSSTarget(Node*);
    Node* getCSSTarget() const;
    
    void setDocumentChanged(bool);

    void attachNodeIterator(NodeIterator*);
    void detachNodeIterator(NodeIterator*);
    void notifyBeforeNodeRemoval(Node*);
    DOMWindow* defaultView() const;
    PassRefPtr<Event> createEvent(const String& eventType, ExceptionCode&);

    // keep track of what types of event listeners are registered, so we don't
    // dispatch events unnecessarily
    enum ListenerType {
        DOMSUBTREEMODIFIED_LISTENER          = 0x01,
        DOMNODEINSERTED_LISTENER             = 0x02,
        DOMNODEREMOVED_LISTENER              = 0x04,
        DOMNODEREMOVEDFROMDOCUMENT_LISTENER  = 0x08,
        DOMNODEINSERTEDINTODOCUMENT_LISTENER = 0x10,
        DOMATTRMODIFIED_LISTENER             = 0x20,
        DOMCHARACTERDATAMODIFIED_LISTENER    = 0x40,
        OVERFLOWCHANGED_LISTENER             = 0x80
    };

	bool hasListenerType(ListenerType listenerType) const { return (m_listenerTypes & listenerType) ? true : false; }
    void addListenerType(ListenerType listenerType) { m_listenerTypes = m_listenerTypes | listenerType; }

    CSSStyleDeclaration* getOverrideStyle(Element*, const String& pseudoElt);

    virtual void defaultEventHandler(Event*);
    void handleWindowEvent(Event*, bool useCapture);
    void setHTMLWindowEventListener(const AtomicString &eventType, PassRefPtr<EventListener>);
    EventListener* getHTMLWindowEventListener(const AtomicString &eventType);
    void removeHTMLWindowEventListener(const AtomicString &eventType);

    void setHTMLWindowEventListener(const AtomicString& eventType, Attribute*);

    void addWindowEventListener(const AtomicString& eventType, PassRefPtr<EventListener>, bool useCapture);
    void removeWindowEventListener(const AtomicString& eventType, EventListener*, bool useCapture);
    bool hasWindowEventListener(const AtomicString& eventType);

    PassRefPtr<EventListener> createHTMLEventListener(const String& functionName, const String& code, Node*);
    
    /**
     * Searches through the document, starting from fromNode, for the next selectable element that comes after fromNode.
     * The order followed is as specified in section 17.11.1 of the HTML4 spec, which is elements with tab indexes
     * first (from lowest to highest), and then elements without tab indexes (in document order).
     *
     * @param fromNode The node from which to start searching. The node after this will be focused. May be null.
     *
     * @return The focus node that comes after fromNode
     *
     * See http://www.w3.org/TR/html4/interact/forms.html#h-17.11.1
     */
    Node* nextFocusableNode(Node* start, KeyboardEvent*);

    /**
     * Searches through the document, starting from fromNode, for the previous selectable element (that comes _before_)
     * fromNode. The order followed is as specified in section 17.11.1 of the HTML4 spec, which is elements with tab
     * indexes first (from lowest to highest), and then elements without tab indexes (in document order).
     *
     * @param fromNode The node from which to start searching. The node before this will be focused. May be null.
     *
     * @return The focus node that comes before fromNode
     *
     * See http://www.w3.org/TR/html4/interact/forms.html#h-17.11.1
     */
    Node* previousFocusableNode(Node* start, KeyboardEvent*);

    int nodeAbsIndex(Node*);
    Node* nodeWithAbsIndex(int absIndex);

    /**
     * Handles a HTTP header equivalent set by a meta tag using <meta http-equiv="..." content="...">. This is called
     * when a meta tag is encountered during document parsing, and also when a script dynamically changes or adds a meta
     * tag. This enables scripts to use meta tags to perform refreshes and set expiry dates in addition to them being
     * specified in a HTML file.
     *
     * @param equiv The http header name (value of the meta tag's "equiv" attribute)
     * @param content The header value (value of the meta tag's "content" attribute)
     */
    void processHttpEquiv(const String& equiv, const String& content);
    
    void dispatchImageLoadEventSoon(HTMLImageLoader*);
    void dispatchImageLoadEventsNow();
    void removeImage(HTMLImageLoader*);
    
    // Returns the owning element in the parent document.
    // Returns 0 if this is the top level document.
    Element* ownerElement() const;

    String title() const { return m_title; }
    void setTitle(const String&, Element* titleElement = 0);
    void removeTitle(Element* titleElement);

    /* document.currentScript: the <script> element whose code is running right
     * now, or 0. Only classic scripts set it; a module keeps it null (HTML
     * spec), and it is null again as soon as the script returns - which is what
     * makes `document.currentScript` a reliable "am I being executed by a
     * <script> tag" test (JSONP loaders, `document.currentScript.src` for a
     * base URL, data-* attributes on the tag). */
    Element* currentScript() const { return m_currentScript.get(); }
    void setCurrentScript(Element* script) { m_currentScript = script; }

    String cookie() const;
    void setCookie(const String&);

    String referrer() const;

    String domain() const;
    void setDomain(const String& newDomain);
    void setDomainInternal(const String& newDomain);

    String lastModified() const;

    String policyBaseURL() const { return m_policyBaseURL; }
    void setPolicyBaseURL(const String& s) { m_policyBaseURL = s; }
    
    // The following implements the rule from HTML 4 for what valid names are.
    // To get this right for all the XML cases, we probably have to improve this or move it
    // and make it sensitive to the type of document.
    static bool isValidName(const String&);

    // The following breaks a qualified name into a prefix and a local name.
    // It also does a validity check, and returns false if the qualified name is invalid
    // (empty string or invalid characters).
    static bool parseQualifiedName(const String& qualifiedName, String& prefix, String& localName);
    
    void addElementById(const AtomicString& elementId, Element *element);
    void removeElementById(const AtomicString& elementId, Element *element);

    void addImageMap(HTMLMapElement*);
    void removeImageMap(HTMLMapElement*);
    HTMLMapElement* getImageMap(const String& URL) const;

    HTMLElement* body();
    void setBody(PassRefPtr<HTMLElement>, ExceptionCode&);

    HTMLHeadElement* head();

    String toString() const;
    
    bool execCommand(const String& command, bool userInterface = false, const String& value = String());
    bool queryCommandEnabled(const String& command);
    bool queryCommandIndeterm(const String& command);
    bool queryCommandState(const String& command);
    bool queryCommandSupported(const String& command);
    String queryCommandValue(const String& command);
    
    void addMarker(Range*, DocumentMarker::MarkerType, String description = String());
    void addMarker(Node*, DocumentMarker);
    void copyMarkers(Node *srcNode, unsigned startOffset, int length, Node *dstNode, int delta, DocumentMarker::MarkerType = DocumentMarker::AllMarkers);
    void removeMarkers(Range*, DocumentMarker::MarkerType = DocumentMarker::AllMarkers);
    void removeMarkers(Node*, unsigned startOffset, int length, DocumentMarker::MarkerType = DocumentMarker::AllMarkers);
    void removeMarkers(DocumentMarker::MarkerType = DocumentMarker::AllMarkers);
    void removeMarkers(Node*);
    void repaintMarkers(DocumentMarker::MarkerType = DocumentMarker::AllMarkers);
    void setRenderedRectForMarker(Node*, DocumentMarker, const IntRect&);
    void invalidateRenderedRectsForMarkersInRect(const IntRect&);
    void shiftMarkers(Node*, unsigned startOffset, int delta, DocumentMarker::MarkerType = DocumentMarker::AllMarkers);

    DocumentMarker* markerContainingPoint(const IntPoint&, DocumentMarker::MarkerType = DocumentMarker::AllMarkers);
    Vector<DocumentMarker> markersForNode(Node*);
    Vector<IntRect> renderedRectsForMarkers(DocumentMarker::MarkerType = DocumentMarker::AllMarkers);
    
    // designMode support
    enum InheritedBool { off = false, on = true, inherit };    
    void setDesignMode(InheritedBool value);
    InheritedBool getDesignMode() const;
    bool inDesignMode() const;

    Document* parentDocument() const;
    Document* topDocument() const;

    int docID() const { return m_docID; }

#if ENABLE(XSLT)
    void applyXSLTransform(ProcessingInstruction* pi);
    void setTransformSource(void* doc);
    const void* transformSource() { return m_transformSource; }
    PassRefPtr<Document> transformSourceDocument() { return m_transformSourceDocument; }
    void setTransformSourceDocument(Document* doc) { m_transformSourceDocument = doc; }
#endif

#if ENABLE(XBL)
    // XBL methods
    XBLBindingManager* bindingManager() const { return m_bindingManager; }
#endif

    void incDOMTreeVersion() { ++m_domtree_version; }
    unsigned domTreeVersion() const { return m_domtree_version; }

    void setDocType(PassRefPtr<DocumentType>);

    void finishedParsing();

#if ENABLE(XPATH)
    // XPathEvaluator methods
    PassRefPtr<XPathExpression> createExpression(const String& expression,
                                                 XPathNSResolver* resolver,
                                                 ExceptionCode& ec);
    PassRefPtr<XPathNSResolver> createNSResolver(Node *nodeResolver);
    PassRefPtr<XPathResult> evaluate(const String& expression,
                                     Node* contextNode,
                                     XPathNSResolver* resolver,
                                     unsigned short type,
                                     XPathResult* result,
                                     ExceptionCode& ec);
#endif // ENABLE(XPATH)
    
    enum PendingSheetLayout { NoLayoutWithPendingSheets, DidLayoutWithPendingSheets, IgnoreLayoutWithPendingSheets };

    bool didLayoutWithPendingStylesheets() const { return m_pendingSheetLayout == DidLayoutWithPendingSheets; }
    
    void setHasNodesWithPlaceholderStyle() { m_hasNodesWithPlaceholderStyle = true; }

    String iconURL();
    void setIconURL(const String& iconURL, const String& type);

    bool isAllowedToLoadLocalResources() const { return m_isAllowedToLoadLocalResources; }

    void setUseSecureKeyboardEntryWhenActive(bool);
    bool useSecureKeyboardEntryWhenActive() const;
    
#if USE(LOW_BANDWIDTH_DISPLAY)
    void setDocLoader(DocLoader* loader) { m_docLoader = loader; }
    bool inLowBandwidthDisplay() const { return m_inLowBandwidthDisplay; }
    void setLowBandwidthDisplay(bool lowBandWidth) { m_inLowBandwidthDisplay = lowBandWidth; }
#endif
    
    void addNodeList() { m_numNodeLists++; }
    void removeNodeList() { m_numNodeLists--; }
    bool hasNodeLists() const { return m_numNodeLists != 0; }

    void updateFocusAppearanceSoon();
    void cancelFocusAppearanceUpdate();
    
private:
    CSSStyleSelector* m_styleSelector;
    bool m_didCalculateStyleSelector;
    /* A stylesheet was added/removed/changed and the document still has to
     * rebuild its selector and recalc style; done lazily in
     * updateStyleIfNeeded() so a burst of inline <style> writes costs one
     * recalc instead of one full-document recalc + layout per write. */
    bool m_pendingStyleSelectorUpdate;

    /* Viewport units (R53 rewrote them to px while parsing, so they were frozen
     * at the load-time size). Now a stylesheet keeps `var(--agb-vw-100)` and
     * this table says what the number is; the px value follows the viewport
     * through viewportCustomProperties(), so a resize only restyles. */
    struct ViewportUnit {
        int kind;       /* 0 = vw, 1 = vh, 2 = vmin, 3 = vmax */
        double number;  /* e.g. 100.0 for 100vw */
    };
    HashMap<String, ViewportUnit> m_viewportUnits;
    /* calc(...) texts that contain viewport units, in registration order; the
     * variable is -agb-vwunit-calc-<index+1> and its value is the text with
     * the units substituted (see viewportCustomProperties). */
    Vector<String> m_viewportCalcTemplates;
    RefPtr<StyleCustomPropertyData> m_viewportProperties;
    /* effectiveZoom the cached map was built with (-1 = never built). A zoom
     * change alters what 100vw means, so the cache has to follow it. */
    float m_viewportZoom;

    Frame* m_frame;
    DocLoader* m_docLoader;
    Tokenizer* m_tokenizer;
    bool m_wellFormed;
    DeprecatedString m_url;
    DeprecatedString m_baseURL;
    String m_baseTarget;

    RefPtr<DocumentType> m_docType;
    RefPtr<DOMImplementation> m_implementation;

    RefPtr<StyleSheet> m_sheet;
    String m_usersheet;
    String m_printSheet;

    // Track the number of currently loading top-level stylesheets.  Sheets
    // loaded using the @import directive are not included in this count.
    // We use this count of pending sheets to detect when we can begin attaching
    // elements.
    int m_pendingStylesheets;

    // But sometimes you need to ignore pending stylesheet count to
    // force an immediate layout when requested by JS.
    bool m_ignorePendingStylesheets;

    // If we do ignore the pending stylesheet count, then we need to add a boolean
    // to track that this happened so that we can do a full repaint when the stylesheets
    // do eventually load.
    PendingSheetLayout m_pendingSheetLayout;
    
    bool m_hasNodesWithPlaceholderStyle;

    RefPtr<CSSStyleSheet> m_elemSheet;

    bool m_printing;

    ParseMode pMode;
    HTMLMode hMode;

    Color m_textColor;

    RefPtr<Node> m_focusedNode;
    RefPtr<Node> m_hoverNode;
    RefPtr<Node> m_activeNode;
    mutable RefPtr<Element> m_documentElement;

    unsigned m_domtree_version;
    
    HashSet<NodeIterator*> m_nodeIterators;

    unsigned short m_listenerTypes;
    RefPtr<StyleSheetList> m_styleSheets;
    
    typedef DeprecatedValueList<RefPtr<RegisteredEventListener> > RegisteredEventListenerList;
    RegisteredEventListenerList m_windowEventListeners;

    typedef HashMap<FormElementKey, Vector<String>, FormElementKeyHash, FormElementKeyHashTraits> FormElementStateMap;
    ListHashSet<HTMLFormControlElementWithState*> m_formElementsWithState;
    FormElementStateMap m_stateForNewFormElements;

    HashSet<Element*> m_didRestorePageCallbackSet;
    
    Color m_linkColor;
    Color m_visitedLinkColor;
    Color m_activeLinkColor;

    String m_preferredStylesheetSet;
    String m_selectedStylesheetSet;

    bool m_loadingSheet;
    bool visuallyOrdered;
    bool m_bParsing;
    bool m_docChanged;
    bool m_inStyleRecalc;
    bool m_closeAfterStyleRecalc;
    bool m_usesDescendantRules;
    bool m_usesSiblingRules;
    bool m_usesFirstLineRules;
    bool m_usesFirstLetterRules;
    bool m_gotoAnchorNeededAfterStylesheetsLoad;

    String m_title;
    bool m_titleSetExplicitly;
    RefPtr<Element> m_titleElement;

    RefPtr<Element> m_currentScript;
    
    RenderArena* m_renderArena;

    typedef std::pair<Vector<DocumentMarker>, Vector<IntRect> > MarkerMapVectorPair;
    typedef HashMap<RefPtr<Node>, MarkerMapVectorPair*> MarkerMap;
    MarkerMap m_markers;

    DeprecatedPtrList<HTMLImageLoader> m_imageLoadEventDispatchSoonList;
    DeprecatedPtrList<HTMLImageLoader> m_imageLoadEventDispatchingList;
    Timer<Document> m_imageLoadEventTimer;

    Timer<Document> m_updateFocusAppearanceTimer;

    Node* m_cssTarget;
    
    bool m_processingLoadEvent;
    double m_startTime;
    bool m_overMinimumLayoutThreshold;
    
#if ENABLE(XSLT)
    void* m_transformSource;
    RefPtr<Document> m_transformSourceDocument;
#endif

#if ENABLE(XBL)
    XBLBindingManager* m_bindingManager; // The access point through which documents and elements communicate with XBL.
#endif
    
    typedef HashMap<AtomicStringImpl*, HTMLMapElement*> ImageMapsByName;
    ImageMapsByName m_imageMapsByName;

    String m_policyBaseURL;

    HashSet<Node*> m_disconnectedNodesWithEventListeners;

    int m_docID; // A unique document identifier used for things like document-specific mapped attributes.

    String m_xmlEncoding;
    String m_xmlVersion;
    bool m_xmlStandalone;

public:
    bool inPageCache();
    void setInPageCache(bool flag);

    // Elements can register themselves for the "didRestoreFromCache()" callback which will be
    // called if the document is restored from the Page Cache
    void registerForDidRestoreFromCacheCallback(Element*);
    void unregisterForDidRestoreFromCacheCallback(Element*);
    
    void didRestoreFromCache();

    void setShouldCreateRenderers(bool);
    bool shouldCreateRenderers();
    
    void setDecoder(TextResourceDecoder*);
    TextResourceDecoder* decoder() const { return m_decoder.get(); }

    UChar backslashAsCurrencySymbol() const;

    void setDashboardRegionsDirty(bool f) { m_dashboardRegionsDirty = f; }
    bool dashboardRegionsDirty() const { return m_dashboardRegionsDirty; }
    bool hasDashboardRegions () const { return m_hasDashboardRegions; }
    void setHasDashboardRegions (bool f) { m_hasDashboardRegions = f; }
    const Vector<DashboardRegionValue>& dashboardRegions() const;
    void setDashboardRegions(const Vector<DashboardRegionValue>&);

    void removeAllEventListenersFromAllNodes();

    void registerDisconnectedNodeWithEventListeners(Node*);
    void unregisterDisconnectedNodeWithEventListeners(Node*);
    
    HTMLFormElement::CheckedRadioButtons& checkedRadioButtons() { return m_checkedRadioButtons; }
    
#if ENABLE(SVG)
    const SVGDocumentExtensions* svgExtensions();
    SVGDocumentExtensions* accessSVGExtensions();
#endif

    bool domainWasSetInDOM() const { return m_domainWasSetInDOM; }

    void initSecurityPolicyURL();
    const KURL& securityPolicyURL() const { return m_securityPolicyURL; }
    
    bool processingLoadEvent() const { return m_processingLoadEvent; }

protected:
    void clearXMLVersion() { m_xmlVersion = String(); }

private:
    bool shouldBeAllowedToLoadLocalResources() const;

    void updateTitle();
    void removeAllDisconnectedNodeEventListeners();
    void imageLoadEventTimerFired(Timer<Document>*);
    void updateFocusAppearanceTimerFired(Timer<Document>*);

    JSEditor* jsEditor();
    JSEditor* m_jsEditor;

    mutable String m_domain;
    bool m_domainWasSetInDOM;

    KURL m_securityPolicyURL;

    RenderObject* m_savedRenderer;
    int m_secureForms;
    
    RefPtr<TextResourceDecoder> m_decoder;

    mutable HashMap<AtomicStringImpl*, Element*> m_elementsById;
    mutable HashCountedSet<AtomicStringImpl*> m_duplicateIds;
    
    mutable HashMap<StringImpl*, Element*, CaseInsensitiveHash<StringImpl*> > m_elementsByAccessKey;
    
    InheritedBool m_designMode;
    
    int m_selfOnlyRefCount;

    HTMLFormElement::CheckedRadioButtons m_checkedRadioButtons;
    
    HTMLCollection::CollectionInfo m_collectionInfo[HTMLCollection::UnnamedCollectionTypes];
    HashMap<AtomicStringImpl*, HTMLCollection::CollectionInfo*> m_nameCollectionInfo[HTMLCollection::CollectionTypes - HTMLCollection::UnnamedCollectionTypes];

#if ENABLE(XPATH)
    RefPtr<XPathEvaluator> m_xpathEvaluator;
#endif
    
#if ENABLE(SVG)
    SVGDocumentExtensions* m_svgExtensions;
#endif
    
    Vector<DashboardRegionValue> m_dashboardRegions;
    bool m_hasDashboardRegions;
    bool m_dashboardRegionsDirty;

    mutable bool m_accessKeyMapValid;
    bool m_createRenderers;
    bool m_inPageCache;
    String m_iconURL;

    bool m_isAllowedToLoadLocalResources;
    
    bool m_useSecureKeyboardEntryWhenActive;

    bool m_isXHTML;

    unsigned m_numNodeLists;
#if USE(LOW_BANDWIDTH_DISPLAY)
    bool m_inLowBandwidthDisplay;
#endif

    bool m_async;
};

/* Scopes document.currentScript to the script execution that sets it and puts
 * the previous value back afterwards: a classic script may insert another
 * <script>, which executes nested, and the outer script must still see itself
 * when the inner one returns. The document is held by reference so a script
 * that navigates away cannot leave a dangling pointer here. */
class CurrentScriptScope {
public:
    CurrentScriptScope(Document* document, Element* script)
        : m_document(document)
        , m_saved(document ? document->currentScript() : 0)
    {
        if (m_document)
            m_document->setCurrentScript(script);
    }

    ~CurrentScriptScope()
    {
        if (m_document)
            m_document->setCurrentScript(m_saved.get());
    }

private:
    RefPtr<Document> m_document;
    RefPtr<Element> m_saved;
};

} // namespace WebCore

#endif // Document_h
