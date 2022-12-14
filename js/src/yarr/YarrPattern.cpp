/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright (C) 2009 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Peter Varga (pvarga@inf.u-szeged.hu), University of Szeged
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "yarr/YarrPattern.h"

#include "yarr/Yarr.h"
#include "yarr/YarrCanonicalizeUCS2.h"
#include "yarr/YarrParser.h"

using namespace WTF;

namespace JSC { namespace Yarr {

#include "yarr/RegExpJitTables.h"

#if WTF_CPU_SPARC
# define BASE_FRAME_SIZE 24
#else
# define BASE_FRAME_SIZE 0
#endif

// Thanks, windows.h!
#undef min
#undef max

class CharacterClassConstructor {
public:
    CharacterClassConstructor(bool isCaseInsensitive = false)
        : m_isCaseInsensitive(isCaseInsensitive)
    {
    }

    void reset()
    {
        m_matches.clear();
        m_ranges.clear();
        m_matchesUnicode.clear();
        m_rangesUnicode.clear();
    }

    void append(const CharacterClass* other)
    {
        for (size_t i = 0; i < other->m_matches.size(); ++i)
            addSorted(m_matches, other->m_matches[i]);
        for (size_t i = 0; i < other->m_ranges.size(); ++i)
            addSortedRange(m_ranges, other->m_ranges[i].begin, other->m_ranges[i].end);
        for (size_t i = 0; i < other->m_matchesUnicode.size(); ++i)
            addSorted(m_matchesUnicode, other->m_matchesUnicode[i]);
        for (size_t i = 0; i < other->m_rangesUnicode.size(); ++i)
            addSortedRange(m_rangesUnicode, other->m_rangesUnicode[i].begin, other->m_rangesUnicode[i].end);
    }

    void putChar(UChar ch)
    {
        // Handle ascii cases.
        if (ch <= 0x7f) {
            if (m_isCaseInsensitive && isASCIIAlpha(ch)) {
                addSorted(m_matches, toASCIIUpper(ch));
                addSorted(m_matches, toASCIILower(ch));
            } else
                addSorted(m_matches, ch);
            return;
        }

        // Simple case, not a case-insensitive match.
        if (!m_isCaseInsensitive) {
            addSorted(m_matchesUnicode, ch);
            return;
        }

        // Add multiple matches, if necessary.
        const UCS2CanonicalizationRange* info = rangeInfoFor(ch);
        if (info->type == CanonicalizeUnique)
            addSorted(m_matchesUnicode, ch);
        else
            putUnicodeIgnoreCase(ch, info);
    }

    void putUnicodeIgnoreCase(UChar ch, const UCS2CanonicalizationRange* info)
    {
        ASSERT(m_isCaseInsensitive);
        ASSERT(ch > 0x7f);
        ASSERT(ch >= info->begin && ch <= info->end);
        ASSERT(info->type != CanonicalizeUnique);
        if (info->type == CanonicalizeSet) {
            for (const uint16_t* set = characterSetInfo[info->value]; (ch = *set); ++set)
                addSorted(m_matchesUnicode, ch);
        } else {
            addSorted(m_matchesUnicode, ch);
            addSorted(m_matchesUnicode, getCanonicalPair(info, ch));
        }
    }

    void putRange(UChar lo, UChar hi)
    {
        if (lo <= 0x7f) {
            char asciiLo = lo;
            char asciiHi = std::min(hi, (UChar)0x7f);
            addSortedRange(m_ranges, lo, asciiHi);

            if (m_isCaseInsensitive) {
                if ((asciiLo <= 'Z') && (asciiHi >= 'A'))
                    addSortedRange(m_ranges, std::max(asciiLo, 'A')+('a'-'A'), std::min(asciiHi, 'Z')+('a'-'A'));
                if ((asciiLo <= 'z') && (asciiHi >= 'a'))
                    addSortedRange(m_ranges, std::max(asciiLo, 'a')+('A'-'a'), std::min(asciiHi, 'z')+('A'-'a'));
            }
        }
        if (hi <= 0x7f)
            return;

        lo = std::max(lo, (UChar)0x80);
        addSortedRange(m_rangesUnicode, lo, hi);

        if (!m_isCaseInsensitive)
            return;

        const UCS2CanonicalizationRange* info = rangeInfoFor(lo);
        while (true) {
            // Handle the range [lo .. end]
            UChar end = std::min<UChar>(info->end, hi);

            switch (info->type) {
            case CanonicalizeUnique:
                // Nothing to do - no canonical equivalents.
                break;
            case CanonicalizeSet: {
                UChar ch;
                for (const uint16_t* set = characterSetInfo[info->value]; (ch = *set); ++set)
                    addSorted(m_matchesUnicode, ch);
                break;
            }
            case CanonicalizeRangeLo:
                addSortedRange(m_rangesUnicode, lo + info->value, end + info->value);
                break;
            case CanonicalizeRangeHi:
                addSortedRange(m_rangesUnicode, lo - info->value, end - info->value);
                break;
            case CanonicalizeAlternatingAligned:
                // Use addSortedRange since there is likely an abutting range to combine with.
                if (lo & 1)
                    addSortedRange(m_rangesUnicode, lo - 1, lo - 1);
                if (!(end & 1))
                    addSortedRange(m_rangesUnicode, end + 1, end + 1);
                break;
            case CanonicalizeAlternatingUnaligned:
                // Use addSortedRange since there is likely an abutting range to combine with.
                if (!(lo & 1))
                    addSortedRange(m_rangesUnicode, lo - 1, lo - 1);
                if (end & 1)
                    addSortedRange(m_rangesUnicode, end + 1, end + 1);
                break;
            }

            if (hi == end)
                return;

            ++info;
            lo = info->begin;
        };

    }

    CharacterClass* charClass()
    {
        CharacterClass* characterClass = newOrCrash<CharacterClass>();

        characterClass->m_matches.swap(m_matches);
        characterClass->m_ranges.swap(m_ranges);
        characterClass->m_matchesUnicode.swap(m_matchesUnicode);
        characterClass->m_rangesUnicode.swap(m_rangesUnicode);

        return characterClass;
    }

private:
    void addSorted(Vector<UChar>& matches, UChar ch)
    {
        unsigned pos = 0;
        unsigned range = matches.size();

        // binary chop, find position to insert char.
        while (range) {
            unsigned index = range >> 1;

            int val = matches[pos+index] - ch;
            if (!val)
                return;
            else if (val > 0)
                range = index;
            else {
                pos += (index+1);
                range -= (index+1);
            }
        }

        if (pos == matches.size())
            matches.append(ch);
        else
            matches.insert(pos, ch);
    }

    void addSortedRange(Vector<CharacterRange>& ranges, UChar lo, UChar hi)
    {
        unsigned end = ranges.size();

        // Simple linear scan - I doubt there are that many ranges anyway...
        // feel free to fix this with something faster (eg binary chop).
        for (unsigned i = 0; i < end; ++i) {
            // does the new range fall before the current position in the array
            if (hi < ranges[i].begin) {
                // optional optimization: concatenate appending ranges? - may not be worthwhile.
                if (hi == (ranges[i].begin - 1)) {
                    ranges[i].begin = lo;
                    return;
                }
                ranges.insert(i, CharacterRange(lo, hi));
                return;
            }
            // Okay, since we didn't hit the last case, the end of the new range is definitely at or after the begining
            // If the new range start at or before the end of the last range, then the overlap (if it starts one after the
            // end of the last range they concatenate, which is just as good.
            if (lo <= (ranges[i].end + 1)) {
                // found an intersect! we'll replace this entry in the array.
                ranges[i].begin = std::min(ranges[i].begin, lo);
                ranges[i].end = std::max(ranges[i].end, hi);

                // now check if the new range can subsume any subsequent ranges.
                unsigned next = i+1;
                // each iteration of the loop we will either remove something from the list, or break the loop.
                while (next < ranges.size()) {
                    if (ranges[next].begin <= (ranges[i].end + 1)) {
                        // the next entry now overlaps / concatenates this one.
                        ranges[i].end = std::max(ranges[i].end, ranges[next].end);
                        ranges.remove(next);
                    } else
                        break;
                }

                return;
            }
        }

        // CharacterRange comes after all existing ranges.
        ranges.append(CharacterRange(lo, hi));
    }

    bool m_isCaseInsensitive;

    Vector<UChar> m_matches;
    Vector<CharacterRange> m_ranges;
    Vector<UChar> m_matchesUnicode;
    Vector<CharacterRange> m_rangesUnicode;
};

class YarrPatternConstructor {
public:
    YarrPatternConstructor(YarrPattern& pattern)
        : m_pattern(pattern)
        , m_stackBase(nullptr)
        , m_characterClassConstructor(pattern.m_ignoreCase)
        , m_invertParentheticalAssertion(false)
    {
        m_pattern.m_body = newOrCrash<PatternDisjunction>();
        m_alternative = m_pattern.m_body->addNewAlternative();
        m_pattern.m_disjunctions.append(m_pattern.m_body);
    }

    ~YarrPatternConstructor()
    {
    }

    void reset()
    {
        m_pattern.reset();
        m_characterClassConstructor.reset();

        m_pattern.m_body = newOrCrash<PatternDisjunction>();
        m_alternative = m_pattern.m_body->addNewAlternative();
        m_pattern.m_disjunctions.append(m_pattern.m_body);
    }

    void assertionBOL()
    {
        if (!m_alternative->m_terms.size() & !m_invertParentheticalAssertion) {
            m_alternative->m_startsWithBOL = true;
            m_alternative->m_containsBOL = true;
            m_pattern.m_containsBOL = true;
        }
        m_alternative->m_terms.append(PatternTerm::BOL());
    }
    void assertionEOL()
    {
        m_alternative->m_terms.append(PatternTerm::EOL());
    }
    void assertionWordBoundary(bool invert)
    {
        m_alternative->m_terms.append(PatternTerm::WordBoundary(invert));
    }

    void atomPatternCharacter(UChar ch)
    {
        // We handle case-insensitive checking of unicode characters which do have both
        // cases by handling them as if they were defined using a CharacterClass.
        if (!m_pattern.m_ignoreCase || isASCII(ch)) {
            m_alternative->m_terms.append(PatternTerm(ch));
            return;
        }

        const UCS2CanonicalizationRange* info = rangeInfoFor(ch);
        if (info->type == CanonicalizeUnique) {
            m_alternative->m_terms.append(PatternTerm(ch));
            return;
        }

        m_characterClassConstructor.putUnicodeIgnoreCase(ch, info);
        CharacterClass* newCharacterClass = m_characterClassConstructor.charClass();
        m_pattern.m_userCharacterClasses.append(newCharacterClass);
        m_alternative->m_terms.append(PatternTerm(newCharacterClass, false));
    }

    void atomBuiltInCharacterClass(BuiltInCharacterClassID classID, bool invert)
    {
        switch (classID) {
        case DigitClassID:
            m_alternative->m_terms.append(PatternTerm(m_pattern.digitsCharacterClass(), invert));
            break;
        case SpaceClassID:
            m_alternative->m_terms.append(PatternTerm(m_pattern.spacesCharacterClass(), invert));
            break;
        case WordClassID:
            m_alternative->m_terms.append(PatternTerm(m_pattern.wordcharCharacterClass(), invert));
            break;
        case NewlineClassID:
            m_alternative->m_terms.append(PatternTerm(m_pattern.newlineCharacterClass(), invert));
            break;
        }
    }

    void atomCharacterClassBegin(bool invert = false)
    {
        m_invertCharacterClass = invert;
    }

    void atomCharacterClassAtom(UChar ch)
    {
        m_characterClassConstructor.putChar(ch);
    }

    void atomCharacterClassRange(UChar begin, UChar end)
    {
        m_characterClassConstructor.putRange(begin, end);
    }

    void atomCharacterClassBuiltIn(BuiltInCharacterClassID classID, bool invert)
    {
        ASSERT(classID != NewlineClassID);

        switch (classID) {
        case DigitClassID:
            m_characterClassConstructor.append(invert ? m_pattern.nondigitsCharacterClass() : m_pattern.digitsCharacterClass());
            break;

        case SpaceClassID:
            m_characterClassConstructor.append(invert ? m_pattern.nonspacesCharacterClass() : m_pattern.spacesCharacterClass());
            break;

        case WordClassID:
            m_characterClassConstructor.append(invert ? m_pattern.nonwordcharCharacterClass() : m_pattern.wordcharCharacterClass());
            break;

        default:
            ASSERT_NOT_REACHED();
        }
    }

    void atomCharacterClassEnd()
    {
        CharacterClass* newCharacterClass = m_characterClassConstructor.charClass();
        m_pattern.m_userCharacterClasses.append(newCharacterClass);
        m_alternative->m_terms.append(PatternTerm(newCharacterClass, m_invertCharacterClass));
    }

    void atomParenthesesSubpatternBegin(bool capture = true)
    {
        unsigned subpatternId = m_pattern.m_numSubpatterns + 1;
        if (capture)
            m_pattern.m_numSubpatterns++;

        PatternDisjunction* parenthesesDisjunction = newOrCrash<PatternDisjunction>(m_alternative);
        m_pattern.m_disjunctions.append(parenthesesDisjunction);
        m_alternative->m_terms.append(PatternTerm(PatternTerm::TypeParenthesesSubpattern, subpatternId, parenthesesDisjunction, capture, false));
        m_alternative = parenthesesDisjunction->addNewAlternative();
    }

    void atomParentheticalAssertionBegin(bool invert = false)
    {
        PatternDisjunction* parenthesesDisjunction = newOrCrash<PatternDisjunction>(m_alternative);
        m_pattern.m_disjunctions.append(parenthesesDisjunction);
        m_alternative->m_terms.append(PatternTerm(PatternTerm::TypeParentheticalAssertion, m_pattern.m_numSubpatterns + 1, parenthesesDisjunction, false, invert));
        m_alternative = parenthesesDisjunction->addNewAlternative();
        m_invertParentheticalAssertion = invert;
    }

    void atomParenthesesEnd()
    {
        ASSERT(m_alternative->m_parent);
        ASSERT(m_alternative->m_parent->m_parent);

        PatternDisjunction* parenthesesDisjunction = m_alternative->m_parent;
        m_alternative = m_alternative->m_parent->m_parent;

        PatternTerm& lastTerm = m_alternative->lastTerm();

        unsigned numParenAlternatives = parenthesesDisjunction->m_alternatives.size();
        unsigned numBOLAnchoredAlts = 0;

        for (unsigned i = 0; i < numParenAlternatives; i++) {
            // Bubble up BOL flags
            if (parenthesesDisjunction->m_alternatives[i]->m_startsWithBOL)
                numBOLAnchoredAlts++;
        }

        if (numBOLAnchoredAlts) {
            m_alternative->m_containsBOL = true;
            // If all the alternatives in parens start with BOL, then so does this one
            if (numBOLAnchoredAlts == numParenAlternatives)
                m_alternative->m_startsWithBOL = true;
        }

        lastTerm.parentheses.lastSubpatternId = m_pattern.m_numSubpatterns;
        m_invertParentheticalAssertion = false;
    }

    void atomBackReference(unsigned subpatternId)
    {
        ASSERT(subpatternId);
        m_pattern.m_containsBackreferences = true;
        m_pattern.m_maxBackReference = std::max(m_pattern.m_maxBackReference, subpatternId);

        if (subpatternId > m_pattern.m_numSubpatterns) {
            m_alternative->m_terms.append(PatternTerm::ForwardReference());
            return;
        }

        PatternAlternative* currentAlternative = m_alternative;
        ASSERT(currentAlternative);

        // Note to self: if we waited until the AST was baked, we could also remove forwards refs
        while ((currentAlternative = currentAlternative->m_parent->m_parent)) {
            PatternTerm& term = currentAlternative->lastTerm();
            ASSERT((term.type == PatternTerm::TypeParenthesesSubpattern) || (term.type == PatternTerm::TypeParentheticalAssertion));

            if ((term.type == PatternTerm::TypeParenthesesSubpattern) && term.capture() && (subpatternId == term.parentheses.subpatternId)) {
                m_alternative->m_terms.append(PatternTerm::ForwardReference());
                return;
            }
        }

        m_alternative->m_terms.append(PatternTerm(subpatternId));
    }

    // deep copy the argument disjunction.  If filterStartsWithBOL is true,
    // skip alternatives with m_startsWithBOL set true.
    PatternDisjunction* copyDisjunction(PatternDisjunction* disjunction, bool filterStartsWithBOL = false)
    {
        PatternDisjunction* newDisjunction = 0;
        for (unsigned alt = 0; alt < disjunction->m_alternatives.size(); ++alt) {
            PatternAlternative* alternative = disjunction->m_alternatives[alt];
            if (!filterStartsWithBOL || !alternative->m_startsWithBOL) {
                if (!newDisjunction) {
                    newDisjunction = newOrCrash<PatternDisjunction>();
                    newDisjunction->m_parent = disjunction->m_parent;
                }
                PatternAlternative* newAlternative = newDisjunction->addNewAlternative();
                newAlternative->m_terms.reserve(alternative->m_terms.size());
                for (unsigned i = 0; i < alternative->m_terms.size(); ++i)
                    newAlternative->m_terms.append(copyTerm(alternative->m_terms[i], filterStartsWithBOL));
            }
        }

        if (newDisjunction)
            m_pattern.m_disjunctions.append(newDisjunction);
        return newDisjunction;
    }

    PatternTerm copyTerm(PatternTerm& term, bool filterStartsWithBOL = false)
    {
        if ((term.type != PatternTerm::TypeParenthesesSubpattern) && (term.type != PatternTerm::TypeParentheticalAssertion))
            return PatternTerm(term);

        PatternTerm termCopy = term;
        termCopy.parentheses.disjunction = copyDisjunction(termCopy.parentheses.disjunction, filterStartsWithBOL);
        return termCopy;
    }

    void quantifyAtom(unsigned min, unsigned max, bool greedy)
    {
        ASSERT(min <= max);
        ASSERT(m_alternative->m_terms.size());

        if (!max) {
            m_alternative->removeLastTerm();
            return;
        }

        PatternTerm& term = m_alternative->lastTerm();
        ASSERT(term.type > PatternTerm::TypeAssertionWordBoundary);
        ASSERT((term.quantityCount == 1) && (term.quantityType == QuantifierFixedCount));

        if (term.type == PatternTerm::TypeParentheticalAssertion) {
            // If an assertion is quantified with a minimum count of zero, it can simply be removed.
            // This arises from the RepeatMatcher behaviour in the spec. Matching an assertion never
            // results in any input being consumed, however the continuation passed to the assertion
            // (called in steps, 8c and 9 of the RepeatMatcher definition, ES5.1 15.10.2.5) will
            // reject all zero length matches (see step 2.1). A match from the continuation of the
            // expression will still be accepted regardless (via steps 8a and 11) - the upshot of all
            // this is that matches from the assertion are not required, and won't be accepted anyway,
            // so no need to ever run it.
            if (!min)
                m_alternative->removeLastTerm();
            // We never need to run an assertion more than once. Subsequent interations will be run
            // with the same start index (since assertions are non-capturing) and the same captures
            // (per step 4 of RepeatMatcher in ES5.1 15.10.2.5), and as such will always produce the
            // same result and captures. If the first match succeeds then the subsequent (min - 1)
            // matches will too. Any additional optional matches will fail (on the same basis as the
            // minimum zero quantified assertions, above), but this will still result in a match.
            return;
        }

        if (min == 0)
            term.quantify(max, greedy   ? QuantifierGreedy : QuantifierNonGreedy);
        else if (min == max)
            term.quantify(min, QuantifierFixedCount);
        else {
            term.quantify(min, QuantifierFixedCount);
            m_alternative->m_terms.append(copyTerm(term));
            // NOTE: this term is interesting from an analysis perspective, in that it can be ignored.....
            m_alternative->lastTerm().quantify((max == quantifyInfinite) ? max : max - min, greedy ? QuantifierGreedy : QuantifierNonGreedy);
            if (m_alternative->lastTerm().type == PatternTerm::TypeParenthesesSubpattern)
                m_alternative->lastTerm().parentheses.isCopy = true;
        }
    }

    void disjunction()
    {
        m_alternative = m_alternative->m_parent->addNewAlternative();
    }

    ErrorCode setupAlternativeOffsets(PatternAlternative* alternative, unsigned currentCallFrameSize, unsigned initialInputPosition,
                                      unsigned* callFrameSizeOut)
    {
        /*
         * Attempt detection of over-recursion:
         * "1MB should be enough stack for anyone."
         */
        uint8_t stackDummy_;
        if (m_stackBase - &stackDummy_ > 1024*1024)
            return PatternTooLarge;

        alternative->m_hasFixedSize = true;
        Checked<unsigned> currentInputPosition = initialInputPosition;

        for (unsigned i = 0; i < alternative->m_terms.size(); ++i) {
            PatternTerm& term = alternative->m_terms[i];

            switch (term.type) {
            case PatternTerm::TypeAssertionBOL:
            case PatternTerm::TypeAssertionEOL:
            case PatternTerm::TypeAssertionWordBoundary:
                if (Checked<int>(currentInputPosition).safeGet(term.inputPosition))
                    return RuntimeError;
                break;

            case PatternTerm::TypeBackReference:
                if (Checked<int>(currentInputPosition).safeGet(term.inputPosition))
                    return RuntimeError;
                term.frameLocation = currentCallFrameSize;
                currentCallFrameSize += YarrStackSpaceForBackTrackInfoBackReference;
                alternative->m_hasFixedSize = false;
                break;

            case PatternTerm::TypeForwardReference:
                break;

            case PatternTerm::TypePatternCharacter:
                if (Checked<int>(currentInputPosition).safeGet(term.inputPosition))
                    return RuntimeError;
                if (term.quantityType != QuantifierFixedCount) {
                    term.frameLocation = currentCallFrameSize;
                    currentCallFrameSize += YarrStackSpaceForBackTrackInfoPatternCharacter;
                    alternative->m_hasFixedSize = false;
                } else
                    currentInputPosition += term.quantityCount;
                break;

            case PatternTerm::TypeCharacterClass:
                if (Checked<int>(currentInputPosition).safeGet(term.inputPosition))
                    return RuntimeError;
                if (term.quantityType != QuantifierFixedCount) {
                    term.frameLocation = currentCallFrameSize;
                    currentCallFrameSize += YarrStackSpaceForBackTrackInfoCharacterClass;
                    alternative->m_hasFixedSize = false;
                } else
                    currentInputPosition += term.quantityCount;
                break;

            case PatternTerm::TypeParenthesesSubpattern:
                // Note: for fixed once parentheses we will ensure at least the minimum is available; others are on their own.
                term.frameLocation = currentCallFrameSize;
                unsigned position;
                if (currentInputPosition.safeGet(position))
                    return RuntimeError;
                if (term.quantityCount == 1 && !term.parentheses.isCopy) {
                    if (term.quantityType != QuantifierFixedCount)
                        currentCallFrameSize += YarrStackSpaceForBackTrackInfoParenthesesOnce;
                    if (ErrorCode error = setupDisjunctionOffsets(term.parentheses.disjunction, currentCallFrameSize, position, &currentCallFrameSize))
                        return error;
                    // If quantity is fixed, then pre-check its minimum size.
                    if (term.quantityType == QuantifierFixedCount)
                        currentInputPosition += term.parentheses.disjunction->m_minimumSize;
                    if (Checked<int>(currentInputPosition).safeGet(term.inputPosition))
                        return RuntimeError;
                } else if (term.parentheses.isTerminal) {
                    currentCallFrameSize += YarrStackSpaceForBackTrackInfoParenthesesTerminal;
                    if (ErrorCode error = setupDisjunctionOffsets(term.parentheses.disjunction, currentCallFrameSize, position, &currentCallFrameSize))
                        return error;
                    if (Checked<int>(currentInputPosition).safeGet(term.inputPosition))
                        return RuntimeError;
                } else {
                    if (Checked<int>(currentInputPosition).safeGet(term.inputPosition))
                        return RuntimeError;
                    unsigned dummy;
                    if (ErrorCode error = setupDisjunctionOffsets(term.parentheses.disjunction, BASE_FRAME_SIZE, position, &dummy))
                        return error;
                    currentCallFrameSize += YarrStackSpaceForBackTrackInfoParentheses;
                }
                // Fixed count of 1 could be accepted, if they have a fixed size *AND* if all alternatives are of the same length.
                alternative->m_hasFixedSize = false;
                break;

            case PatternTerm::TypeParentheticalAssertion:
                if (Checked<int>(currentInputPosition).safeGet(term.inputPosition))
                    return RuntimeError;
                term.frameLocation = currentCallFrameSize;
                if (ErrorCode error = setupDisjunctionOffsets(term.parentheses.disjunction, currentCallFrameSize + YarrStackSpaceForBackTrackInfoParentheticalAssertion, term.inputPosition, &currentCallFrameSize))
                    return error;
                break;

            case PatternTerm::TypeDotStarEnclosure:
                alternative->m_hasFixedSize = false;
                term.inputPosition = initialInputPosition;
                break;
            }
        }

        if ((currentInputPosition - initialInputPosition).safeGet(alternative->m_minimumSize))
            return RuntimeError;
        *callFrameSizeOut = currentCallFrameSize;
        return NoError;
    }

    ErrorCode setupDisjunctionOffsets(PatternDisjunction* disjunction, unsigned initialCallFrameSize, unsigned initialInputPosition, unsigned* maximumCallFrameSizeOut)
    {
        if ((disjunction != m_pattern.m_body) && (disjunction->m_alternatives.size() > 1))
            initialCallFrameSize += YarrStackSpaceForBackTrackInfoAlternative;

        unsigned minimumInputSize = UINT_MAX;
        unsigned maximumCallFrameSize = 0;
        bool hasFixedSize = true;

        for (unsigned alt = 0; alt < disjunction->m_alternatives.size(); ++alt) {
            PatternAlternative* alternative = disjunction->m_alternatives[alt];
            unsigned currentAlternativeCallFrameSize;
            if (ErrorCode error = setupAlternativeOffsets(alternative, initialCallFrameSize, initialInputPosition, &currentAlternativeCallFrameSize))
                return error;
            minimumInputSize = std::min(minimumInputSize, alternative->m_minimumSize);
            maximumCallFrameSize = std::max(maximumCallFrameSize, currentAlternativeCallFrameSize);
            hasFixedSize &= alternative->m_hasFixedSize;
        }

        ASSERT(minimumInputSize != UINT_MAX);
        if (minimumInputSize == UINT_MAX)
            return PatternTooLarge;

        ASSERT(minimumInputSize != UINT_MAX);
        ASSERT(maximumCallFrameSize >= initialCallFrameSize);

        disjunction->m_hasFixedSize = hasFixedSize;
        disjunction->m_minimumSize = minimumInputSize;
        disjunction->m_callFrameSize = maximumCallFrameSize;
        *maximumCallFrameSizeOut = maximumCallFrameSize;
        return NoError;
    }

    ErrorCode setupOffsets()
    {
        unsigned dummy;
        return setupDisjunctionOffsets(m_pattern.m_body, BASE_FRAME_SIZE, 0, &dummy);
    }

    // This optimization identifies sets of parentheses that we will never need to backtrack.
    // In these cases we do not need to store state from prior iterations.
    // We can presently avoid backtracking for:
    //   * where the parens are at the end of the regular expression (last term in any of the
    //     alternatives of the main body disjunction).
    //   * where the parens are non-capturing, and quantified unbounded greedy (*).
    //   * where the parens do not contain any capturing subpatterns.
    void checkForTerminalParentheses()
    {
        // This check is much too crude; should be just checking whether the candidate
        // node contains nested capturing subpatterns, not the whole expression!
        if (m_pattern.m_numSubpatterns)
            return;

        Vector<PatternAlternative*>& alternatives = m_pattern.m_body->m_alternatives;
        for (size_t i = 0; i < alternatives.size(); ++i) {
            Vector<PatternTerm>& terms = alternatives[i]->m_terms;
            if (terms.size()) {
                PatternTerm& term = terms.last();
                if (term.type == PatternTerm::TypeParenthesesSubpattern
                    && term.quantityType == QuantifierGreedy
                    && term.quantityCount == quantifyInfinite
                    && !term.capture())
                    term.parentheses.isTerminal = true;
            }
        }
    }

    void optimizeBOL()
    {
        // Look for expressions containing beginning of line (^) anchoring and unroll them.
        // e.g. /^a|^b|c/ becomes /^a|^b|c/ which is executed once followed by /c/ which loops
        // This code relies on the parsing code tagging alternatives with m_containsBOL and
        // m_startsWithBOL and rolling those up to containing alternatives.
        // At this point, this is only valid for non-multiline expressions.
        PatternDisjunction* disjunction = m_pattern.m_body;

        if (!m_pattern.m_containsBOL || m_pattern.m_multiline)
            return;

        PatternDisjunction* loopDisjunction = copyDisjunction(disjunction, true);

        // Set alternatives in disjunction to "onceThrough"
        for (unsigned alt = 0; alt < disjunction->m_alternatives.size(); ++alt)
            disjunction->m_alternatives[alt]->setOnceThrough();

        if (loopDisjunction) {
            // Move alternatives from loopDisjunction to disjunction
            for (unsigned alt = 0; alt < loopDisjunction->m_alternatives.size(); ++alt)
                disjunction->m_alternatives.append(loopDisjunction->m_alternatives[alt]);

            loopDisjunction->m_alternatives.clear();
        }
    }

    bool containsCapturingTerms(PatternAlternative* alternative, size_t firstTermIndex, size_t lastTermIndex)
    {
        Vector<PatternTerm>& terms = alternative->m_terms;

        for (size_t termIndex = firstTermIndex; termIndex <= lastTermIndex; ++termIndex) {
            PatternTerm& term = terms[termIndex];

            if (term.m_capture)
                return true;

            if (term.type == PatternTerm::TypeParenthesesSubpattern) {
                PatternDisjunction* nestedDisjunction = term.parentheses.disjunction;
                for (unsigned alt = 0; alt < nestedDisjunction->m_alternatives.size(); ++alt) {
                    PatternAlternative* pattern = nestedDisjunction->m_alternatives[alt];
                    if (pattern->m_terms.size() == 0)
                        continue;
                    if (containsCapturingTerms(pattern, 0, pattern->m_terms.size() - 1))
                        return true;
                }
            }
        }

        return false;
    }

    // This optimization identifies alternatives in the form of
    // [^].*[?]<expression>.*[$] for expressions that don't have any
    // capturing terms. The alternative is changed to <expression>
    // followed by processing of the dot stars to find and adjust the
    // beginning and the end of the match.
    void optimizeDotStarWrappedExpressions()
    {
        Vector<PatternAlternative*>& alternatives = m_pattern.m_body->m_alternatives;
        if (alternatives.size() != 1)
            return;

        PatternAlternative* alternative = alternatives[0];
        Vector<PatternTerm>& terms = alternative->m_terms;
        if (terms.size() >= 3) {
            bool startsWithBOL = false;
            bool endsWithEOL = false;
            size_t termIndex, firstExpressionTerm, lastExpressionTerm;

            termIndex = 0;
            if (terms[termIndex].type == PatternTerm::TypeAssertionBOL) {
                startsWithBOL = true;
                ++termIndex;
            }

            PatternTerm& firstNonAnchorTerm = terms[termIndex];
            if ((firstNonAnchorTerm.type != PatternTerm::TypeCharacterClass) || (firstNonAnchorTerm.characterClass != m_pattern.newlineCharacterClass()) || !((firstNonAnchorTerm.quantityType == QuantifierGreedy) || (firstNonAnchorTerm.quantityType == QuantifierNonGreedy)))
                return;

            firstExpressionTerm = termIndex + 1;

            termIndex = terms.size() - 1;
            if (terms[termIndex].type == PatternTerm::TypeAssertionEOL) {
                endsWithEOL = true;
                --termIndex;
            }

            PatternTerm& lastNonAnchorTerm = terms[termIndex];
            if ((lastNonAnchorTerm.type != PatternTerm::TypeCharacterClass) || (lastNonAnchorTerm.characterClass != m_pattern.newlineCharacterClass()) || (lastNonAnchorTerm.quantityType != QuantifierGreedy))
                return;

            lastExpressionTerm = termIndex - 1;

            if (firstExpressionTerm > lastExpressionTerm)
                return;

            if (!containsCapturingTerms(alternative, firstExpressionTerm, lastExpressionTerm)) {
                for (termIndex = terms.size() - 1; termIndex > lastExpressionTerm; --termIndex)
                    terms.remove(termIndex);

                for (termIndex = firstExpressionTerm; termIndex > 0; --termIndex)
                    terms.remove(termIndex - 1);

                terms.append(PatternTerm(startsWithBOL, endsWithEOL));

                m_pattern.m_containsBOL = false;
            }
        }
    }

    void setStackBase(uint8_t* stackBase) {
        m_stackBase = stackBase;
    }

private:
    YarrPattern& m_pattern;
    uint8_t * m_stackBase;
    PatternAlternative* m_alternative;
    CharacterClassConstructor m_characterClassConstructor;
    bool m_invertCharacterClass;
    bool m_invertParentheticalAssertion;
};

ErrorCode YarrPattern::compile(const String& patternString)
{
    YarrPatternConstructor constructor(*this);

    if (ErrorCode error = parse(constructor, patternString))
        return error;

    // If the pattern contains illegal backreferences reset & reparse.
    // Quoting Netscape's "What's new in JavaScript 1.2",
    //      "Note: if the number of left parentheses is less than the number specified
    //       in \#, the \# is taken as an octal escape as described in the next row."
    if (containsIllegalBackReference()) {
        unsigned numSubpatterns = m_numSubpatterns;

        constructor.reset();
#if !ASSERT_DISABLED
        ErrorCode error =
#endif
            parse(constructor, patternString, numSubpatterns);

        ASSERT(!error);
        ASSERT(numSubpatterns == m_numSubpatterns);
    }

    uint8_t stackDummy_;
    constructor.setStackBase(&stackDummy_);

    constructor.checkForTerminalParentheses();
    constructor.optimizeDotStarWrappedExpressions();
    constructor.optimizeBOL();

    if (ErrorCode error = constructor.setupOffsets())
        return error;

    return NoError;
}

YarrPattern::YarrPattern(const String& pattern, bool ignoreCase, bool multiline, ErrorCode* error)
    : m_ignoreCase(ignoreCase)
    , m_multiline(multiline)
    , m_containsBackreferences(false)
    , m_containsBOL(false)
    , m_numSubpatterns(0)
    , m_maxBackReference(0)
    , newlineCached(0)
    , digitsCached(0)
    , spacesCached(0)
    , wordcharCached(0)
    , nondigitsCached(0)
    , nonspacesCached(0)
    , nonwordcharCached(0)
{
    *error = compile(pattern);
}

} }
