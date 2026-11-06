/*
 * This file is part of the DOM implementation for KDE.
 *
 * Copyright (C) 2003 Lars Knoll (knoll@kde.org)
 * Copyright (C) 2004, 2005, 2006 Apple Computer, Inc.
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
 */

#ifndef CSSParser_h
#define CSSParser_h

#include "AtomicString.h"
#include "Color.h"
#include "MediaQuery.h"
#include <wtf/HashSet.h>
#include <wtf/Vector.h>

namespace WebCore {

    class CSSMutableStyleDeclaration;
    class CSSPrimitiveValue;
    class CSSProperty;
    class CSSRule;
    class CSSRuleList;
    class CSSSelector;
    class CSSStyleSheet;
    class CSSValue;
    class CSSValueList;
    class Document;
    class MediaList;
    class MediaList;
    class MediaQueryExp;
    class StyleBase;
    class StyleCustomPropertyData;
    class StyleList;
    struct Function;

    struct ParseString {
        UChar* characters;
        int length;

        void lower();
    };

    struct Value {
        int id;
        bool isInt;
        union {
            double fValue;
            int iValue;
            ParseString string;
            Function* function;
        };
        enum {
            Operator = 0x100000,
            QFunction = 0x100001,
            Q_EMS    = 0x100002
        };
        int unit;
    };

    DeprecatedString deprecatedString(const ParseString&);

    static inline String domString(const ParseString& ps)
    {
        return String(ps.characters, ps.length);
    }

    static inline AtomicString atomicString(const ParseString& ps)
    {
        return AtomicString(ps.characters, ps.length);
    }

    class ValueList {
    public:
        ValueList() : m_current(0) { }
        ~ValueList();

        void addValue(const Value& v) { m_values.append(v); }
        unsigned size() const { return m_values.size(); }
        Value* current() { return m_current < m_values.size() ? &m_values[m_current] : 0; }
        Value* next() { ++m_current; return current(); }
        unsigned currentIndex() const { return m_current; }
        void setCurrent(unsigned i) { m_current = i < m_values.size() ? i : m_values.size(); }

        Value* valueAt(unsigned i) { return i < m_values.size() ? &m_values[i] : 0; }
        void deleteValueAt(unsigned i) { m_values.remove(i); }

    private:
        Vector<Value, 16> m_values;
        unsigned m_current;
    };

    struct Function {
        ParseString name;
        ValueList* args;

        ~Function() { delete args; }
    };

    /* Pseudo property id for a custom property declaration ('--x: value').
     * Well above the generated ids, which stop at the last entry of
     * CSSPropertyNames.in. */
#define CSS_PROP_CUSTOM_PROPERTY 4095

    class CSSParser {
    public:
        CSSParser(bool strictParsing = true);
        ~CSSParser();

        void parseSheet(CSSStyleSheet*, const String&);
        PassRefPtr<CSSRule> parseRule(CSSStyleSheet*, const String&);
        bool parseValue(CSSMutableStyleDeclaration*, int propId, const String&, bool important);
        static bool parseColor(RGBA32& color, const String&, bool strict = false);
        bool parseColor(CSSMutableStyleDeclaration*, const String&);
        bool parseDeclaration(CSSMutableStyleDeclaration*, const String&);
        bool parseMediaQuery(MediaList*, const String&);

        static CSSParser* current() { return currentParser; }

        Document* document() const;
        /* `var()` expanded declarations are re-parsed by a fresh parser that
         * has no style element; without a document the relative URLs inside
         * them (background-image etc.) resolve to nothing. */
        void setValueDocument(Document* doc) { m_valueDocument = doc; }

        void addProperty(int propId, PassRefPtr<CSSValue>, bool important);
        /* Adds a declaration whose value must be resolved per element:
         * '--x: value' (customName set) and properties whose value contains
         * var() (rawValue set, expanded by CSSStyleSelector). The CSSValue is a
         * placeholder and must never be applied directly. */
        void addRawValueProperty(int propId, const String& rawValue, const String& customName, bool important);
        void rollbackLastProperties(int num);
        bool hasProperties() const { return numParsedProperties > 0; }

        /* --- CSS custom properties ('--x: value') and var() ---------------
         * Custom properties are resolved per element (cascade + inheritance) in
         * CSSStyleSelector; the parser only records the raw text. */
        static void clearCustomProperties();

        /* Expands var(--x[, fallback]) in `text` using the custom properties
         * resolved for one element. Returns a null String when a referenced
         * variable is undefined and has no fallback (the declaration is invalid
         * at computed-value time and must be ignored). */
        static String expandVariables(const String& text, StyleCustomPropertyData* properties);
        /* Inverse of the internal `--x` -> `-agb-x` rewrite: turns the stored
         * form back into the author's spelling so getComputedStyle() reports
         * `var(--x)` instead of `var(-agb-x)`. */
        static String restoreCustomPropertyNames(const String& css);
        /* Rewrites a single author name ('--x') to the tokenizer-safe form the
         * declaration stores ('-agb-x'); anything else is returned unchanged. */
        static String rewriteCustomPropertyName(const String& name);
        /* The bulk form: every token-initial '--x' in a declaration value. */
        static String rewriteCustomPropertyNames(const String& css);
        /* Serializes a parsed value list back to CSS text. */
        String valueText(ValueList*);
        /* True when the value list contains a var() call anywhere (cheap walk,
         * so declarations without variables skip the text round trip). */
        bool hasVarFunction(ValueList*);
        /* Takes the properties another parser collected (the re-parse of an
         * expanded value) into this one. */
        void appendProperties(CSSParser& other);
        /* Parses `text` as the value of propId and appends the result to this
         * parser's output (used after var() expansion). */
        bool parseExpandedValue(int propId, const String& text, bool important);

        /* Name of the property the parser is handling (custom properties need
         * it: '--x' has no generated id). */
        String lastPropertyName;

        bool parseValue(int propId, bool important);
        bool parseShorthand(int propId, const int* properties, int numProperties, bool important);
        bool parseFlexShorthand(bool important);
        bool parse4Values(int propId, const int* properties, bool important);
        bool parseContent(int propId, bool important);

        CSSValue* parseBackgroundColor();
        bool parseBackgroundImage(CSSValue*&);
    CSSValue* parseGradientFunction(Value* functionValue);
        CSSValue* parseBackgroundPositionXY(bool& xFound, bool& yFound);
        void parseBackgroundPosition(CSSValue*&, CSSValue*&);
        CSSValue* parseBackgroundSize();
        
        bool parseBackgroundProperty(int propId, int& propId1, int& propId2, CSSValue*&, CSSValue*&);
        bool parseBackgroundShorthand(bool important);

        void addBackgroundValue(CSSValue*& lval, CSSValue* rval);

        bool parseDashboardRegions(int propId, bool important);

        bool parseShape(int propId, bool important);

        bool parseFont(bool important);
        CSSValueList* parseFontFamily();

        bool parseCounter(int propId, int defaultValue, bool important);
        PassRefPtr<CSSValue> parseCounterContent(ValueList* args, bool counters);

        /* Both accept the legacy comma form (`rgb(1, 2, 3)`) and CSS Color 4's
         * space-separated form (`rgb(1 2 3 / 50%)`); the return value is the
         * component count (3, or 4 when an alpha component was present). */
        int parseColorParameters(Value*, int* colorValues);
        int parseHSLParameters(Value*, double* colorValues);
        CSSPrimitiveValue* parseColor(Value* = 0);
        bool parseColorFromValue(Value*, RGBA32&, bool = false);

        static bool parseColor(const String&, RGBA32& rgb, bool strict);

        bool parseFontFaceSrc();

#if ENABLE(SVG)
        bool parseSVGValue(int propId, bool important);
        CSSValue* parseSVGPaint();
        CSSValue* parseSVGColor();
        CSSValue* parseSVGStrokeDasharray();
#endif

        // CSS3 Parsing Routines (for properties specific to CSS3)
        bool parseShadow(int propId, bool important);
        bool parseBorderImage(int propId, bool important);
        
        PassRefPtr<CSSValue> parseTransform();
        bool parseTransformOrigin(int propId, int& propId1, int& propId2, CSSValue*&, CSSValue*&);
        
        int yyparse();

        CSSSelector* createFloatingSelector();
        CSSSelector* sinkFloatingSelector(CSSSelector*);

        ValueList* createFloatingValueList();
        ValueList* sinkFloatingValueList(ValueList*);

        Function* createFloatingFunction();
        Function* sinkFloatingFunction(Function*);

        Value& sinkFloatingValue(Value&);

        MediaList* createMediaList();
        CSSRule* createCharsetRule(const ParseString&);
        CSSRule* createImportRule(const ParseString&, MediaList*);
        CSSRule* createMediaRule(MediaList*, CSSRuleList*);
        CSSRuleList* createRuleList();
        CSSRule* createStyleRule(CSSSelector*);
        CSSRule* createFontFaceRule();

        MediaQueryExp* createFloatingMediaQueryExp(const AtomicString&, ValueList*);
        MediaQueryExp* sinkFloatingMediaQueryExp(MediaQueryExp*);
        Vector<MediaQueryExp*>* createFloatingMediaQueryExpList();
        Vector<MediaQueryExp*>* sinkFloatingMediaQueryExpList(Vector<MediaQueryExp*>*);
        MediaQuery* createFloatingMediaQuery(MediaQuery::Restrictor, const String&, Vector<MediaQueryExp*>*);
        MediaQuery* sinkFloatingMediaQuery(MediaQuery*);

    public:
        bool strict;
        bool important;
        int id;
        StyleList* styleElement;
    Document* m_valueDocument;
        RefPtr<CSSRule> rule;
        MediaQuery* mediaQuery;
        ValueList* valueList;
        CSSProperty** parsedProperties;
        int numParsedProperties;
        int maxParsedProperties;

        int m_inParseShorthand;
        int m_currentShorthand;
        bool m_implicitShorthand;

        AtomicString defaultNamespace;

        static CSSParser* currentParser;

        // tokenizer methods and data
    public:
        int lex(void* yylval);
        int token() { return yyTok; }
        UChar* text(int* length);
        int lex();
        
    private:
        void clearProperties();

        void setupParser(const char* prefix, const String&, const char* suffix);

        bool inShorthand() const { return m_inParseShorthand?true:false; }

        void checkForOrphanedUnits();
        
        UChar* data;
        UChar* yytext;
        UChar* yy_c_buf_p;
        UChar yy_hold_char;
        int yy_last_accepting_state;
        UChar* yy_last_accepting_cpos;
        int yyleng;
        int yyTok;
        int yy_start;

        Vector<RefPtr<StyleBase> > m_parsedStyleObjects;
        Vector<RefPtr<CSSRuleList> > m_parsedRuleLists;
        HashSet<CSSSelector*> *m_floatingSelectors;
        HashSet<ValueList*> *m_floatingValueLists;
        HashSet<Function*> *m_floatingFunctions;

        MediaQuery* m_floatingMediaQuery;
        MediaQueryExp* m_floatingMediaQueryExp;
        Vector<MediaQueryExp*>* m_floatingMediaQueryExpList;

        // defines units allowed for a certain property, used in parseUnit
        enum Units {
            FUnknown   = 0x0000,
            FInteger   = 0x0001,
            FNumber    = 0x0002,  // Real Numbers
            FPercent   = 0x0004,
            FLength    = 0x0008,
            FAngle     = 0x0010,
            FTime      = 0x0020,
            FFrequency = 0x0040,
            FRelative  = 0x0100,
            FNonNeg    = 0x0200
        };

        friend inline Units operator|(Units a, Units b)
        {
            return static_cast<Units>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
        }

        static bool validUnit(Value*, Units, bool strict);
        
        friend class TransformOperationInfo;
    };

} // namespace WebCore

#endif // CSSParser_h
