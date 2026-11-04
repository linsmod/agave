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
 *
 * Why not Proxy (measured, see lessons R33): a Proxy as the global's prototype
 * catches window.X and 'X' in window but NOT a bare identifier (ant's global
 * lookup reads the own property table only), and it turns 200k global misses
 * from 30ms into 178ms. A real accessor catches bare identifiers but flips
 * 'X' in window to true, which breaks feature detection. The native table
 * check costs nothing measurable, so it stays.
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

    /* --- element: mutation ----------------------------------------------- */
    { "attachShadow",         "dom", "undefined; no shadow DOM" },

    /* --- element: attributes / text -------------------------------------- */
    { "getAttributeNode",     "dom", "undefined" },
    { "setAttributeNS",       "dom", "undefined; setAttribute works" },
    { "getClientRects",       "dom", "undefined; getBoundingClientRect works" },

    /* --- element: reflected attributes ----------------------------------- */

    /* --- element: geometry / scrolling ----------------------------------- */
    { "requestFullscreen",    "dom", "undefined; no fullscreen" },

    /* --- form ------------------------------------------------------------- */

    /* --- image ------------------------------------------------------------ */

    /* --- frame ------------------------------------------------------------ */

    /* --- document --------------------------------------------------------- */
    { "createRange",          "doc", "undefined; no Range API" },
    { "createTreeWalker",     "doc", "undefined; recursive querySelectorAll works" },
    { "execCommand",          "doc", "undefined; no editing commands" },

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
