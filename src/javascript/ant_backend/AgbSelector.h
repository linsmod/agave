/*
 * AgbSelector.h - the selector engine behind querySelector(All)/matches/closest.
 *
 * WebCore of this vintage has no Node::querySelectorAll(), so the bridge used
 * to split the string on spaces and match "#id" / ".class" / a tag name. That
 * silently failed for everything else: `[data-x]`, `div.cls`, `a[href]`,
 * `ul > li`, `:nth-child(2)`, `a, b`. The CSS engine itself understands all of
 * it (attribute selectors work in stylesheets), but its matcher is wired into
 * style computation, so this file implements the same rules for the DOM API.
 *
 * Supported: selector lists (comma), combinators (descendant, >, +, ~),
 * type/universal/id/class/attribute selectors with all six operators
 * (= ~= |= ^= $= *=), and the common pseudo-classes (first/last/only-child,
 * nth-child(an+b|odd|even), first/last-of-type, empty, root, checked,
 * disabled, enabled, not(simple)).
 *
 * Matching is allocation free on the hot path: tag/id/class/attribute names are
 * interned once at parse time (AtomicString) and compared with operator==, and
 * attribute values use WebCore::String's case-insensitive compare. The first
 * version built a std::string per element and cost ~27ms per call over 2000
 * elements.
 *
 * Header only and included once, by AntRuntime.cpp.
 */

#ifndef AgbSelector_h
#define AgbSelector_h

#include <string>
#include <vector>

#include "Element.h"
#include "Node.h"
#include "HTMLInputElement.h"
#include "HTMLOptionElement.h"
#include "HTMLNames.h"
#include "PlatformString.h"

namespace AgbSelector {

enum CondKind {
    COND_TAG,
    COND_UNIVERSAL,
    COND_ID,
    COND_CLASS,
    COND_ATTR,
    COND_PSEUDO
};

enum AttrOp {
    ATTR_SET,       /* [attr]      */
    ATTR_EXACT,     /* [attr=val]  */
    ATTR_LIST,      /* [attr~=val] */
    ATTR_HYPHEN,    /* [attr|=val] */
    ATTR_BEGIN,     /* [attr^=val] */
    ATTR_END,       /* [attr$=val] */
    ATTR_CONTAIN    /* [attr*=val] */
};

enum PseudoKind {
    PS_UNKNOWN,
    PS_FIRST_CHILD,
    PS_LAST_CHILD,
    PS_ONLY_CHILD,
    PS_NTH_CHILD,
    PS_FIRST_OF_TYPE,
    PS_LAST_OF_TYPE,
    PS_EMPTY,
    PS_ROOT,
    PS_CHECKED,
    PS_DISABLED,
    PS_ENABLED,
    PS_NOT
};

struct Cond {
    int kind;
    WebCore::AtomicString name;    /* tag (lower case) / id / class / attribute name */
    WebCore::String value;         /* attribute value                                */
    int op;
    int pseudo;
    int nthA;
    int nthB;
    std::vector<Cond> notConds;
};

struct Step {
    std::vector<Cond> conds;
    char combinator;   /* relation to the previous step: ' ', '>', '+', '~' */
};

struct Selector {
    std::vector<Step> steps;
};

/* ---------------------------------------------------------------- parsing */

static inline void skipSpace(const char*& p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        ++p;
}

static inline bool isIdentChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || (unsigned char)c >= 0x80;
}

static inline std::string readIdent(const char*& p)
{
    std::string out;

    while (*p && isIdentChar(*p))
        out += *p++;

    return out;
}

static inline std::string readValue(const char*& p)
{
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        std::string out;

        while (*p && *p != quote) {
            if (*p == '\\' && p[1])
                ++p;
            out += *p++;
        }
        if (*p == quote)
            ++p;
        return out;
    }

    return readIdent(p);
}

static inline std::string lowerOf(const std::string& text)
{
    std::string out(text);

    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = (char)(out[i] - 'A' + 'a');
    }
    return out;
}

static inline WebCore::AtomicString atomOf(const std::string& text)
{
    return WebCore::AtomicString(WebCore::String::fromUTF8(text.c_str()));
}

static inline int pseudoFromName(const std::string& name)
{
    if (name == "first-child")   return PS_FIRST_CHILD;
    if (name == "last-child")    return PS_LAST_CHILD;
    if (name == "only-child")    return PS_ONLY_CHILD;
    if (name == "nth-child")     return PS_NTH_CHILD;
    if (name == "first-of-type") return PS_FIRST_OF_TYPE;
    if (name == "last-of-type")  return PS_LAST_OF_TYPE;
    if (name == "empty")         return PS_EMPTY;
    if (name == "root")          return PS_ROOT;
    if (name == "checked")       return PS_CHECKED;
    if (name == "disabled")      return PS_DISABLED;
    if (name == "enabled")       return PS_ENABLED;
    if (name == "not")           return PS_NOT;
    return PS_UNKNOWN;
}

/* an+b, odd, even. */
static inline bool parseNth(const std::string& text, int& a, int& b)
{
    std::string s;
    size_t i;

    for (i = 0; i < text.size(); ++i) {
        char c = text[i];

        if (c != ' ' && c != '\t')
            s += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }

    if (s == "odd")  { a = 2; b = 1; return true; }
    if (s == "even") { a = 2; b = 0; return true; }

    {
        size_t n = s.find('n');

        if (n == std::string::npos) {
            a = 0;
            b = atoi(s.c_str());
            return !s.empty();
        }

        {
            std::string aText = s.substr(0, n);
            std::string bText = s.substr(n + 1);

            if (aText.empty() || aText == "+")
                a = 1;
            else if (aText == "-")
                a = -1;
            else
                a = atoi(aText.c_str());

            b = bText.empty() ? 0 : atoi(bText.c_str());
            return true;
        }
    }
}

static inline bool parseAttr(const char*& p, Cond& cond)
{
    skipSpace(p);

    cond.kind = COND_ATTR;
    cond.op = ATTR_SET;

    {
        std::string name = readIdent(p);

        if (name.empty())
            return false;
        cond.name = atomOf(lowerOf(name));   /* HTML attribute names are lower case */
    }

    skipSpace(p);

    if (*p == ']') {
        ++p;
        return true;
    }

    if (*p == '=') {
        cond.op = ATTR_EXACT;
        ++p;
    } else if (p[0] == '~' && p[1] == '=') {
        cond.op = ATTR_LIST;
        p += 2;
    } else if (p[0] == '|' && p[1] == '=') {
        cond.op = ATTR_HYPHEN;
        p += 2;
    } else if (p[0] == '^' && p[1] == '=') {
        cond.op = ATTR_BEGIN;
        p += 2;
    } else if (p[0] == '$' && p[1] == '=') {
        cond.op = ATTR_END;
        p += 2;
    } else if (p[0] == '*' && p[1] == '=') {
        cond.op = ATTR_CONTAIN;
        p += 2;
    } else {
        return false;
    }

    skipSpace(p);
    cond.value = WebCore::String::fromUTF8(readValue(p).c_str());
    skipSpace(p);

    if (*p != ']')
        return false;
    ++p;
    return true;
}

static inline bool parsePseudo(const char*& p, Cond& cond)
{
    std::string name = readIdent(p);

    cond.kind = COND_PSEUDO;
    cond.pseudo = pseudoFromName(name);

    if (cond.pseudo == PS_UNKNOWN)
        return false;

    if (*p != '(')
        return true;

    ++p;
    {
        std::string argument;
        int depth = 1;

        while (*p && depth > 0) {
            if (*p == '(')
                ++depth;
            else if (*p == ')') {
                if (--depth == 0)
                    break;
            }
            argument += *p++;
        }

        if (*p == ')')
            ++p;

        if (cond.pseudo == PS_NTH_CHILD) {
            if (!parseNth(argument, cond.nthA, cond.nthB))
                return false;
        } else if (cond.pseudo == PS_NOT) {
            const char* inner = argument.c_str();
            Cond innerCond;

            skipSpace(inner);
            if (!*inner)
                return false;

            while (*inner) {
                if (*inner == '#' || *inner == '.' || *inner == '[' || *inner == ':') {
                    char kind = *inner++;

                    innerCond = Cond();
                    if (kind == '#') {
                        innerCond.kind = COND_ID;
                        innerCond.name = atomOf(readIdent(inner));
                    } else if (kind == '.') {
                        innerCond.kind = COND_CLASS;
                        innerCond.name = atomOf(readIdent(inner));
                    } else if (kind == '[') {
                        if (!parseAttr(inner, innerCond))
                            return false;
                    } else if (!parsePseudo(inner, innerCond)) {
                        return false;
                    }
                    cond.notConds.push_back(innerCond);
                } else if (isIdentChar(*inner) || *inner == '*') {
                    innerCond = Cond();
                    if (*inner == '*') {
                        ++inner;
                        innerCond.kind = COND_UNIVERSAL;
                    } else {
                        innerCond.kind = COND_TAG;
                        innerCond.name = atomOf(lowerOf(readIdent(inner)));
                    }
                    cond.notConds.push_back(innerCond);
                } else {
                    return false;
                }
            }
        }
    }

    return true;
}

static inline bool parseSimple(const char*& p, Step& step)
{
    bool any = false;

    for (;;) {
        if (*p == '*') {
            Cond cond;
            ++p;
            cond.kind = COND_UNIVERSAL;
            step.conds.push_back(cond);
            any = true;
        } else if (*p == '#') {
            Cond cond;
            ++p;
            cond.kind = COND_ID;
            cond.name = atomOf(readIdent(p));
            if (cond.name.isEmpty())
                return false;
            step.conds.push_back(cond);
            any = true;
        } else if (*p == '.') {
            Cond cond;
            ++p;
            cond.kind = COND_CLASS;
            cond.name = atomOf(readIdent(p));
            if (cond.name.isEmpty())
                return false;
            step.conds.push_back(cond);
            any = true;
        } else if (*p == '[') {
            Cond cond;
            ++p;
            if (!parseAttr(p, cond))
                return false;
            step.conds.push_back(cond);
            any = true;
        } else if (*p == ':') {
            Cond cond;
            ++p;
            if (!parsePseudo(p, cond))
                return false;
            step.conds.push_back(cond);
            any = true;
        } else if (isIdentChar(*p)) {
            Cond cond;
            cond.kind = COND_TAG;
            cond.name = atomOf(lowerOf(readIdent(p)));
            if (cond.name.isEmpty())
                return false;
            step.conds.push_back(cond);
            any = true;
        } else {
            break;
        }
    }

    return any;
}

static inline bool parseOne(const char*& p, Selector& selector)
{
    char pendingCombinator = 0;

    for (;;) {
        Step step;

        skipSpace(p);

        if (*p == '>' || *p == '+' || *p == '~') {
            pendingCombinator = *p++;
            skipSpace(p);
        }

        if (!parseSimple(p, step))
            return false;

        step.combinator = selector.steps.empty() ? 0 : (pendingCombinator ? pendingCombinator : ' ');
        selector.steps.push_back(step);
        pendingCombinator = 0;

        skipSpace(p);

        if (!*p || *p == ',')
            break;

        if (*p == '>' || *p == '+' || *p == '~')
            continue;

        if (isIdentChar(*p) || *p == '.' || *p == '#' || *p == '[' || *p == ':' || *p == '*')
            continue;   /* descendant combinator */

        return false;
    }

    return !selector.steps.empty();
}

static inline bool parse(const char* text, std::vector<Selector>& out)
{
    const char* p = text;

    if (!text)
        return false;

    for (;;) {
        Selector selector;

        skipSpace(p);
        if (!*p)
            break;
        if (!parseOne(p, selector))
            return false;
        out.push_back(selector);

        skipSpace(p);
        if (*p == ',') {
            ++p;
            continue;
        }
        if (!*p)
            break;
        return false;
    }

    return !out.empty();
}

/* ---------------------------------------------------------------- matching */

static inline WebCore::Element* previousElement(WebCore::Node* node)
{
    WebCore::Node* n = node ? node->previousSibling() : 0;

    while (n && !n->isElementNode())
        n = n->previousSibling();
    return n ? static_cast<WebCore::Element*>(n) : 0;
}

static inline WebCore::Element* nextElement(WebCore::Node* node)
{
    WebCore::Node* n = node ? node->nextSibling() : 0;

    while (n && !n->isElementNode())
        n = n->nextSibling();
    return n ? static_cast<WebCore::Element*>(n) : 0;
}

static inline WebCore::Element* parentElement(WebCore::Element* element)
{
    WebCore::Node* n = element ? element->parentNode() : 0;

    return (n && n->isElementNode()) ? static_cast<WebCore::Element*>(n) : 0;
}

static inline int elementIndex(WebCore::Element* element, bool ofType)
{
    WebCore::Element* parent = parentElement(element);
    int index = 0;

    if (!parent)
        return 0;

    for (WebCore::Node* n = parent->firstChild(); n; n = n->nextSibling()) {
        if (!n->isElementNode())
            continue;
        if (ofType && !static_cast<WebCore::Element*>(n)->hasTagName(element->tagQName()))
            continue;
        ++index;
        if (n == element)
            return index;
    }

    return 0;
}

static inline bool matchAttribute(const Cond& cond, WebCore::Element* element)
{
    if (cond.op == ATTR_SET)
        return element->hasAttribute(cond.name);

    {
        const WebCore::AtomicString& attribute = element->getAttribute(cond.name);

        if (attribute.isNull())
            return false;

        switch (cond.op) {
        case ATTR_EXACT:
            /* WebCore's own matcher ignores case for HTML documents. */
            return WebCore::equalIgnoringCase(attribute, cond.value);
        case ATTR_LIST: {
            /* Whitespace separated word match; rare enough to build a string. */
            std::string value(attribute.domString().utf8().data());
            std::string wanted(cond.value.utf8().data());
            size_t start = 0;

            while (start < value.size()) {
                size_t end = value.find(' ', start);

                if (end == std::string::npos)
                    end = value.size();
                if (end > start && value.compare(start, end - start, wanted) == 0)
                    return true;
                start = end + 1;
            }
            return false;
        }
        case ATTR_HYPHEN:
            if (WebCore::equalIgnoringCase(attribute, cond.value))
                return true;
            return attribute.length() > cond.value.length()
                && attribute.startsWith(cond.value, false)
                && attribute[cond.value.length()] == '-';
        case ATTR_BEGIN:
            return attribute.startsWith(cond.value, false);
        case ATTR_END:
            return attribute.endsWith(cond.value, false);
        case ATTR_CONTAIN:
            return attribute.contains(cond.value, false);
        default:
            return false;
        }
    }
}

static inline bool matchCompound(const std::vector<Cond>& conds, WebCore::Element* element);

static inline bool matchPseudo(const Cond& cond, WebCore::Element* element)
{
    switch (cond.pseudo) {
    case PS_FIRST_CHILD:
        return !previousElement(element);
    case PS_LAST_CHILD:
        return !nextElement(element);
    case PS_ONLY_CHILD:
        return !previousElement(element) && !nextElement(element);
    case PS_NTH_CHILD: {
        int index = elementIndex(element, false);

        if (index == 0)
            return false;
        if (cond.nthA == 0)
            return index == cond.nthB;
        {
            int diff = index - cond.nthB;

            if (cond.nthA > 0)
                return diff >= 0 && diff % cond.nthA == 0;
            return diff <= 0 && (-diff) % (-cond.nthA) == 0;
        }
    }
    case PS_FIRST_OF_TYPE: {
        WebCore::Element* previous = previousElement(element);

        while (previous) {
            if (previous->hasTagName(element->tagQName()))
                return false;
            previous = previousElement(previous);
        }
        return true;
    }
    case PS_LAST_OF_TYPE: {
        WebCore::Element* next = nextElement(element);

        while (next) {
            if (next->hasTagName(element->tagQName()))
                return false;
            next = nextElement(next);
        }
        return true;
    }
    case PS_EMPTY:
        return !element->firstChild();
    case PS_ROOT:
        return element->parentNode() && element->parentNode()->isDocumentNode();
    case PS_CHECKED:
        if (element->hasTagName(WebCore::HTMLNames::inputTag))
            return static_cast<WebCore::HTMLInputElement*>(element)->checked();
        if (element->hasTagName(WebCore::HTMLNames::optionTag))
            return static_cast<WebCore::HTMLOptionElement*>(element)->selected();
        return false;
    case PS_DISABLED:
        return element->isControl() && !element->isEnabled();
    case PS_ENABLED:
        return element->isControl() && element->isEnabled();
    case PS_NOT:
        return !matchCompound(cond.notConds, element);
    default:
        return false;
    }
}

static inline bool matchCompound(const std::vector<Cond>& conds, WebCore::Element* element)
{
    if (!element)
        return false;

    for (size_t i = 0; i < conds.size(); ++i) {
        const Cond& cond = conds[i];

        switch (cond.kind) {
        case COND_UNIVERSAL:
            break;
        case COND_TAG:
            /* localName is lower case for HTML elements, and the selector was
             * lower cased at parse time. */
            if (element->localName() != cond.name)
                return false;
            break;
        case COND_ID:
            if (!element->hasID() || element->getIDAttribute() != cond.name)
                return false;
            break;
        case COND_CLASS:
            if (!element->hasClass())
                return false;
            {
                bool found = false;

                for (const WebCore::AtomicStringList* c = element->getClassList(); c; c = c->next()) {
                    if (c->string() == cond.name) {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return false;
            }
            break;
        case COND_ATTR:
            if (!matchAttribute(cond, element))
                return false;
            break;
        case COND_PSEUDO:
            if (!matchPseudo(cond, element))
                return false;
            break;
        default:
            return false;
        }
    }

    return true;
}

static inline bool matchStep(const std::vector<Step>& steps, size_t index, WebCore::Element* element)
{
    if (!matchCompound(steps[index].conds, element))
        return false;

    if (index == 0)
        return true;

    switch (steps[index].combinator) {
    case '>': {
        WebCore::Element* parent = parentElement(element);

        return parent && matchStep(steps, index - 1, parent);
    }
    case '+': {
        WebCore::Element* previous = previousElement(element);

        return previous && matchStep(steps, index - 1, previous);
    }
    case '~': {
        WebCore::Element* previous = previousElement(element);

        while (previous) {
            if (matchStep(steps, index - 1, previous))
                return true;
            previous = previousElement(previous);
        }
        return false;
    }
    default: {
        WebCore::Element* parent = parentElement(element);

        while (parent) {
            if (matchStep(steps, index - 1, parent))
                return true;
            parent = parentElement(parent);
        }
        return false;
    }
    }
}

static inline bool matches(const Selector& selector, WebCore::Element* element)
{
    if (!element || selector.steps.empty())
        return false;

    return matchStep(selector.steps, selector.steps.size() - 1, element);
}

static inline bool matchesAny(const std::vector<Selector>& selectors, WebCore::Element* element)
{
    for (size_t i = 0; i < selectors.size(); ++i) {
        if (matches(selectors[i], element))
            return true;
    }

    return false;
}

} /* namespace AgbSelector */

#endif /* AgbSelector_h */
