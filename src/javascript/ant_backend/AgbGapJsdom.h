/*
 * AgbGapJsdom.h - the jsdom capability gap registry.
 *
 * This is the machine readable twin of docs/capability-gaps.md §1: every gap a
 * page can run into silently (a property reads as undefined, a collection reads
 * as empty) is listed here once, with the area and the effect the page sees.
 * The bridge reports the FIRST access of each gap:
 *
 *   [  1234.5ms] [gap-jsdom] name=div#app.innerText area=dom effect="undefined; textContent works"
 *
 * Why a registry and a once-only report: pages feature-detect these in loops
 * (`if (el.innerText) ...` per row), so one line per access would bury every
 * other channel. One line per gap per process is the useful signal.
 *
 * AGB_GAP_LOG=0 silences the channel (it is on by default, like [cache]).
 *
 * Not every gap can be reported this way. The ones that live on the global
 * object itself (window.getSelection, IntersectionObserver, ResizeObserver,
 * PerformanceObserver, customElements, devicePixelRatio, history.length) are
 * read through ant's identifier lookup, which does not run an exotic getter on
 * the global object (see R31); reporting them would mean defining real
 * properties, and a page that feature-detects `typeof window.getSelection`
 * would then take the wrong branch. They stay in docs/capability-gaps.md.
 *
 * Adding an entry: append to agbJsdomGaps[], and call agbJsdomGapReport() from
 * the place that returns the missing value (nodeGetter's fall-through for
 * element properties, titleGetter for document collections, ...).
 */

#ifndef AgbGapJsdom_h
#define AgbGapJsdom_h

#include <string.h>

#include "../../webview/AgbLog.h"

typedef struct {
    const char* name;    /* property name as the bridge sees it              */
    const char* area;    /* dom / form / img / frame / doc / history         */
    const char* effect;  /* what the page silently gets instead              */
} AgbJsdomGap;

static const AgbJsdomGap agbJsdomGaps[] = {
    /* --- element: tree navigation ---------------------------------------- */
    { "parentElement",        "dom", "undefined; parentNode works" },
    { "firstElementChild",    "dom", "undefined; children[0] works" },
    { "lastElementChild",     "dom", "undefined; children[children.length-1] works" },
    { "nextElementSibling",   "dom", "undefined; nextSibling works" },
    { "previousElementSibling","dom", "undefined; previousSibling works" },
    { "getRootNode",          "dom", "undefined; document is the only root" },
    { "isConnected",          "dom", "undefined" },

    /* --- element: mutation ----------------------------------------------- */
    { "insertAdjacentHTML",   "dom", "undefined; innerHTML += works" },
    { "insertAdjacentElement","dom", "undefined; appendChild works" },
    { "insertAdjacentText",   "dom", "undefined; appendChild(textNode) works" },
    { "replaceChildren",      "dom", "undefined; remove children then appendChild" },
    { "attachShadow",         "dom", "undefined; no shadow DOM" },

    /* --- element: attributes / text -------------------------------------- */
    { "getAttributeNames",    "dom", "undefined; attributes.length works" },
    { "toggleAttribute",      "dom", "undefined; set/removeAttribute works" },
    { "getAttributeNode",     "dom", "undefined" },
    { "setAttributeNS",       "dom", "undefined; setAttribute works" },
    { "nodeValue",            "dom", "undefined; textContent works" },
    { "innerText",            "dom", "undefined; textContent works" },
    { "outerText",            "dom", "undefined; textContent works" },
    { "getClientRects",       "dom", "undefined; getBoundingClientRect works" },

    /* --- element: reflected attributes ----------------------------------- */
    { "dir",                  "dom", "undefined (reflected attributes are not exposed)" },
    { "lang",                 "dom", "undefined (reflected attributes are not exposed)" },
    { "hidden",               "dom", "undefined; document.hidden is never set, el.hidden not exposed" },
    { "tabIndex",             "dom", "undefined (reflected attributes are not exposed)" },
    { "contentEditable",      "dom", "undefined (reflected attributes are not exposed)" },
    { "draggable",            "dom", "undefined (reflected attributes are not exposed)" },
    { "translate",            "dom", "undefined (reflected attributes are not exposed)" },
    { "slot",                 "dom", "undefined (reflected attributes are not exposed)" },
    { "part",                 "dom", "undefined (reflected attributes are not exposed)" },
    { "spellcheck",           "dom", "undefined (reflected attributes are not exposed)" },
    { "autofocus",            "dom", "undefined (reflected attributes are not exposed)" },

    /* --- element: geometry / scrolling ----------------------------------- */
    { "offsetParent",         "dom", "undefined; offsetTop/Left/Width/Height work" },
    { "scrollIntoView",       "dom", "undefined; no scripted scrolling" },
    { "requestFullscreen",    "dom", "undefined; no fullscreen" },
    { "classList.replace",    "dom", "undefined; add/remove/contains/toggle work" },

    /* --- form ------------------------------------------------------------- */
    { "submit",               "form", "undefined; clicking a submit button still submits" },
    { "action",               "form", "undefined; the form still posts to its own URL" },
    { "reset",                "form", "undefined" },
    { "checkValidity",        "form", "undefined; no constraint validation" },
    { "elements",             "form", "undefined; querySelectorAll('input,select,textarea') works" },

    /* --- image ------------------------------------------------------------ */
    { "complete",             "img", "undefined; the image still loads and paints" },
    { "naturalWidth",         "img", "undefined" },
    { "naturalHeight",        "img", "undefined" },
    { "width",                "img", "undefined (use the CSS/attribute width)" },
    { "height",               "img", "undefined (use the CSS/attribute height)" },
    { "decode",               "img", "undefined; onload still fires" },

    /* --- frame ------------------------------------------------------------ */
    { "contentWindow",        "frame", "undefined; window.frames[i] works" },
    { "contentDocument",      "frame", "undefined; window.frames[i].document works" },

    /* --- document --------------------------------------------------------- */
    { "activeElement",        "doc", "undefined; no focus tracking in script" },
    { "createRange",          "doc", "undefined; no Range API" },
    { "createTreeWalker",     "doc", "undefined; recursive querySelectorAll works" },
    { "elementFromPoint",     "doc", "undefined; no hit testing from script" },
    { "execCommand",          "doc", "undefined; no editing commands" },
    { "importNode",           "doc", "undefined; cloneNode + appendChild works" },
    { "adoptNode",            "doc", "undefined" },
    { "hasFocus",             "doc", "undefined" },
    { "visibilityState",      "doc", "undefined; the page is never hidden" },
    { "fonts",                "doc", "undefined; no FontFaceSet" },
    { "forms",                "doc", "empty array (length 0); querySelectorAll('form') works" },
    { "images",               "doc", "empty array (length 0); querySelectorAll('img') works" },
    { "scripts",              "doc", "empty array (length 0); querySelectorAll('script') works" },
    { "links",                "doc", "empty array (length 0); querySelectorAll('a[href]') works" },
    { "anchors",              "doc", "empty array (length 0)" },
    { "embeds",               "doc", "empty array (length 0)" },
    { "plugins",              "doc", "empty array (length 0)" },
    { "applets",              "doc", "empty array (length 0)" },

    /* --- history ---------------------------------------------------------- */
    { "state",                "history", "undefined; pushState only rewrites the URL" },
    { "length",               "history", "undefined" },

    /* --- selectors -------------------------------------------------------- */
    { "querySelector[attr]",  "css", "attribute selectors ([data-x]) match nothing" }
};

#define AGB_JSDOM_GAP_COUNT (sizeof(agbJsdomGaps) / sizeof(agbJsdomGaps[0]))

static unsigned char agbJsdomGapSeen[AGB_JSDOM_GAP_COUNT];

static inline const AgbJsdomGap* agbJsdomGapFind(const char* name)
{
    size_t i;

    if (!name || !name[0])
        return NULL;

    for (i = 0; i < AGB_JSDOM_GAP_COUNT; ++i) {
        if (strcmp(agbJsdomGaps[i].name, name) == 0)
            return &agbJsdomGaps[i];
    }

    return NULL;
}

/* 1 when `name` is a registered gap that has not been reported yet: callers
 * use it to skip building the receiver description for every plain miss. */
static inline int agbJsdomGapPending(const char* name)
{
    const AgbJsdomGap* gap = agbJsdomGapFind(name);

    return gap && !agbJsdomGapSeen[gap - agbJsdomGaps];
}

/* Reports the first access of a gap.
 *
 * `where` is the receiver as the page sees it ("div#app", "document", "img#qr")
 * and is prefixed to the name; pass NULL when `name` already carries its
 * receiver ("querySelector[attr]"). */
static inline void agbJsdomGapReport(const char* name, const char* where)
{
    const AgbJsdomGap* gap = agbJsdomGapFind(name);

    if (!gap || agbJsdomGapSeen[gap - agbJsdomGaps])
        return;

    agbJsdomGapSeen[gap - agbJsdomGaps] = 1;

    if (where && where[0] && !strchr(name, '.'))
        agbLog(AGB_LOG_GAP, "name=%s.%s area=%s effect=\"%s\"",
               where, name, gap->area, gap->effect);
    else
        agbLog(AGB_LOG_GAP, "name=%s area=%s effect=\"%s\"",
               name, gap->area, gap->effect);
}

#endif /* AgbGapJsdom_h */
