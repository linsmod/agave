/*
 * Word boundaries on top of the UAX#29 segment tables.
 *
 * Copyright (C) 2026 agave port
 *
 * The port has carried platform/unicode/TextBoundary.cpp (FirstWordBreak /
 * NextWordBreak / the word-type tables) since the WebKit 2008 import, but the
 * two entry points every word-wise operation goes through were
 * notImplemented() stubs in src/webview/GlobalFunctions.cpp:
 *
 *   findWordBoundary()      - double-click word selection, endOfWord/startOfWord
 *   findNextWordFromIndex() - Ctrl/Option + Arrow, MoveWordForward/Backward
 *
 * Result: double-click built an inverted one-character range and word-wise
 * caret movement did nothing. This file is the real implementation. It lives
 * next to the segmenter (and is compiled by the platform/*.cpp glob) because
 * TextBoundary.h declares an enum constant named IN, which windows.h defines
 * as an empty macro: including it after windows.h does not compile. Keeping it
 * in a translation unit that never includes windows.h avoids that trap.
 */

#include "config.h"

#include <string.h>

#include "TextBoundary.h"
#include "TextBoundaries.h"

namespace WebCore {

static bool isWordSegmentType(WORD_TYPE type)
{
    switch (type) {
    case ALETTER:
    case NUMERIC:
    case EXTENDNUMLET:
    case KATAKANA:
        return true;
    default:
        return false;
    }
}

/* End offset of the segment that starts at `start` with `type`. `nextType`
 * receives the type of the character at the returned boundary, i.e. the type
 * of the following segment. */
static int wordSegmentEnd(const UChar* text, int length, int start, WORD_TYPE type, WORD_TYPE* nextType)
{
    WORD_BREAK info;
    int end;

    memset(&info, 0, sizeof(info));
    info.current = start;
    info.word_type = type;

    end = NextWordBreak(text, length, &info);
    if (end <= start || end > length)
        end = length;

    if (nextType)
        *nextType = info.word_type;

    return end;
}

/* Segments follow UAX#29: alphabetic/numeric runs are words, each space and
 * punctuation character is its own segment, and a CJK ideograph (type "any" in
 * this table) is its own segment. Chrome runs ICU with a word dictionary and
 * selects a multi-character Chinese word on a double-click instead; the
 * difference is recorded in docs/arch-capability-gaps.md. */
void findWordBoundary(UChar const* str, int len, int position, int* start, int* end)
{
    WORD_BREAK info;
    int segStart, segEnd;
    WORD_TYPE type, nextType = w_TYPE_ANY;

    if (!start || !end)
        return;

    *start = 0;
    *end = 0;

    if (!str || len <= 0)
        return;

    if (position < 0)
        position = 0;
    if (position > len)
        position = len;

    memset(&info, 0, sizeof(info));
    segStart = FirstWordBreak(str, len, &info);
    if (segStart < 0) {
        *end = len;
        return;
    }

    type = info.word_type;
    segEnd = wordSegmentEnd(str, len, segStart, type, &nextType);

    /* The word containing `position`; a caret on the end of the text belongs
     * to the last segment. */
    while (position >= segEnd && segEnd < len) {
        int next;

        segStart = segEnd;
        type = nextType;
        next = wordSegmentEnd(str, len, segStart, type, &nextType);
        if (next <= segStart)
            break;
        segEnd = next;
    }

    *start = segStart;
    *end = segEnd;
}

int findNextWordFromIndex(UChar const* str, int len, int position, bool forward)
{
    WORD_BREAK info;
    int segStart, segEnd;
    WORD_TYPE type, nextType = w_TYPE_ANY;

    if (!str || len <= 0)
        return 0;

    if (position < 0)
        position = 0;
    if (position > len)
        position = len;

    memset(&info, 0, sizeof(info));
    segStart = FirstWordBreak(str, len, &info);
    if (segStart < 0)
        return forward ? len : 0;

    type = info.word_type;
    segEnd = wordSegmentEnd(str, len, segStart, type, &nextType);

    if (forward) {
        /* End of the first word that ends after the caret: inside a word it
         * moves to the end of that word, in the middle of spaces it skips to
         * the end of the next word. */
        for (;;) {
            if (isWordSegmentType(type) && segEnd > position)
                return segEnd;
            if (segEnd >= len)
                return len;

            segStart = segEnd;
            type = nextType;

            {
                int next = wordSegmentEnd(str, len, segStart, type, &nextType);
                if (next <= segStart)
                    return len;
                segEnd = next;
            }
        }
    }

    /* Backward: start of the last word that starts before the caret, so a caret
     * at the start of a word jumps to the start of the previous word. */
    {
        int best = 0;

        for (;;) {
            if (isWordSegmentType(type) && segStart < position)
                best = segStart;
            if (segEnd >= len)
                break;

            segStart = segEnd;
            type = nextType;

            {
                int next = wordSegmentEnd(str, len, segStart, type, &nextType);
                if (next <= segStart)
                    break;
                segEnd = next;
            }
        }

        return best;
    }
}

} // namespace WebCore
