/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "importmidi_inner.h"

#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

#include "global/types/string.h"

#include "importmidi_operations.h"
#include "importmidi_chord.h"

#include "engraving/dom/durationtype.h"
#include "engraving/dom/sig.h"

#include "log.h"

namespace mu::iex::midi {
MTrack::MTrack(const MTrack& other)
    : program(other.program)
    , staff(other.staff)
    , mtrack(other.mtrack)
    , name(other.name)
    , hasKey(other.hasKey)
    , indexOfOperation(other.indexOfOperation)
    , division(other.division)
    , isDivisionInTps(other.isDivisionInTps)
    , hadInitialNotes(other.hadInitialNotes)
    , volumes(other.volumes)
    , chords(other.chords)
{
    updateTupletsFromChords();
}

MTrack& MTrack::operator=(MTrack other)
{
    std::swap(program, other.program);
    std::swap(staff, other.staff);
    std::swap(mtrack, other.mtrack);
    std::swap(name, other.name);
    std::swap(hasKey, other.hasKey);
    std::swap(indexOfOperation, other.indexOfOperation);
    std::swap(division, other.division);
    std::swap(isDivisionInTps, other.isDivisionInTps);
    std::swap(hadInitialNotes, other.hadInitialNotes);
    std::swap(volumes, other.volumes);
    std::swap(chords, other.chords);
    updateTupletsFromChords();

    return *this;
}

// chords of the MTrack are considered to be created/copied already;
// tuplets here are extracted and inserted to the tuplet map of the MTrack
// from tuplet iterators in chords/notes
void MTrack::updateTupletsFromChords()
{
    for (auto& chord: chords) {
        if (chord.second.isInTuplet) {
            updateTuplet(chord.second.tuplet);
        }
        for (auto& note: chord.second.notes) {
            if (note.isInTuplet) {
                updateTuplet(note.tuplet);
            }
        }
    }
}

void MTrack::updateTuplet(
    std::multimap<ReducedFraction, MidiTuplet::TupletData>::iterator& tupletIt)
{
    auto foundTuplet = tuplets.end();
    const auto foundTupletsRange = tuplets.equal_range(tupletIt->first);
    for (auto it = foundTupletsRange.first; it != foundTupletsRange.second; ++it) {
        if (it->second.voice == tupletIt->second.voice) {
            foundTuplet = it;
            break;
        }
    }
    if (foundTuplet != tuplets.end()) {
        tupletIt = foundTuplet;
    } else {
        tupletIt = tuplets.insert({ tupletIt->first, tupletIt->second });
    }
}

namespace Meter {
ReducedFraction userTimeSigToFraction(
    MidiOperations::TimeSigNumerator timeSigNumerator,
    MidiOperations::TimeSigDenominator timeSigDenominator)
{
    int numerator = 4;
    int denominator = 4;

    switch (timeSigNumerator) {
    case MidiOperations::TimeSigNumerator::_2:
        numerator = 2;
        break;
    case MidiOperations::TimeSigNumerator::_3:
        numerator = 3;
        break;
    case MidiOperations::TimeSigNumerator::_4:
        numerator = 4;
        break;
    case MidiOperations::TimeSigNumerator::_5:
        numerator = 5;
        break;
    case MidiOperations::TimeSigNumerator::_6:
        numerator = 6;
        break;
    case MidiOperations::TimeSigNumerator::_7:
        numerator = 7;
        break;
    case MidiOperations::TimeSigNumerator::_9:
        numerator = 9;
        break;
    case MidiOperations::TimeSigNumerator::_12:
        numerator = 12;
        break;
    case MidiOperations::TimeSigNumerator::_15:
        numerator = 15;
        break;
    case MidiOperations::TimeSigNumerator::_21:
        numerator = 21;
        break;
    default:
        break;
    }

    switch (timeSigDenominator) {
    case MidiOperations::TimeSigDenominator::_2:
        denominator = 2;
        break;
    case MidiOperations::TimeSigDenominator::_4:
        denominator = 4;
        break;
    case MidiOperations::TimeSigDenominator::_8:
        denominator = 8;
        break;
    case MidiOperations::TimeSigDenominator::_16:
        denominator = 16;
        break;
    case MidiOperations::TimeSigDenominator::_32:
        denominator = 32;
        break;
    default:
        break;
    }

    return ReducedFraction(numerator, denominator);
}

MidiOperations::TimeSigNumerator fractionNumeratorToUserValue(int n)
{
    MidiOperations::TimeSigNumerator numerator = MidiOperations::TimeSigNumerator::_4;

    if (n == 2) {
        numerator = MidiOperations::TimeSigNumerator::_2;
    } else if (n == 3) {
        numerator = MidiOperations::TimeSigNumerator::_3;
    } else if (n == 4) {
        numerator = MidiOperations::TimeSigNumerator::_4;
    } else if (n == 5) {
        numerator = MidiOperations::TimeSigNumerator::_5;
    } else if (n == 6) {
        numerator = MidiOperations::TimeSigNumerator::_6;
    } else if (n == 7) {
        numerator = MidiOperations::TimeSigNumerator::_7;
    } else if (n == 9) {
        numerator = MidiOperations::TimeSigNumerator::_9;
    } else if (n == 12) {
        numerator = MidiOperations::TimeSigNumerator::_12;
    } else if (n == 15) {
        numerator = MidiOperations::TimeSigNumerator::_15;
    } else if (n == 21) {
        numerator = MidiOperations::TimeSigNumerator::_21;
    } else {
        Q_ASSERT_X(false, "Meter::fractionNumeratorToUserValue", "Unknown numerator");
    }

    return numerator;
}

MidiOperations::TimeSigDenominator fractionDenominatorToUserValue(int z)
{
    MidiOperations::TimeSigDenominator denominator = MidiOperations::TimeSigDenominator::_4;

    if (z == 2) {
        denominator = MidiOperations::TimeSigDenominator::_2;
    } else if (z == 4) {
        denominator = MidiOperations::TimeSigDenominator::_4;
    } else if (z == 8) {
        denominator = MidiOperations::TimeSigDenominator::_8;
    } else if (z == 16) {
        denominator = MidiOperations::TimeSigDenominator::_16;
    } else if (z == 32) {
        denominator = MidiOperations::TimeSigDenominator::_32;
    } else {
        Q_ASSERT_X(false, "Meter::fractionDenominatorToUserValue", "Unknown denominator");
    }

    return denominator;
}
} // namespace Meter

namespace MidiTuplet {
bool haveIntersection(const std::pair<ReducedFraction, ReducedFraction>& interval1,
                      const std::pair<ReducedFraction, ReducedFraction>& interval2,
                      bool strictComparison)
{
    if (strictComparison) {
        return interval1.second > interval2.first && interval1.first < interval2.second;
    }
    return interval1.second >= interval2.first && interval1.first <= interval2.second;
}

bool haveIntersection(
    const std::pair<ReducedFraction, ReducedFraction>& interval,
    const std::vector<std::pair<ReducedFraction, ReducedFraction> >& intervals,
    bool strictComparison)
{
    for (const auto& i: intervals) {
        if (haveIntersection(i, interval, strictComparison)) {
            return true;
        }
    }
    return false;
}
}

namespace MidiCharset {
namespace {
// Windows-1252 differs from ISO-8859-1 only in 0x80..0x9F.  Karaoke and MIDI
// files written on Windows commonly use this range (smart quotes, dashes,
// ellipsis, trademark), so decoding it as Latin-1 would silently corrupt the
// lyric text.  Undefined bytes are preserved as the corresponding C1 control
// code point instead of being dropped.
char16_t decodeWindows1252Byte(unsigned char byte)
{
    if (byte >= 0x80 && byte <= 0x9F) {
        static constexpr char16_t map[32] = {
            0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
            0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
        };
        return map[byte - 0x80];
    }
    return static_cast<char16_t>(byte);
}

muse::String fromSingleByte(const std::string& text, bool windows1252)
{
    std::u16string utf16;
    utf16.reserve(text.size());
    for (const char ch: text) {
        const auto byte = static_cast<unsigned char>(ch);
        utf16.push_back(windows1252 ? decodeWindows1252Byte(byte)
                        : static_cast<char16_t>(byte));
    }

    muse::String result;
    result.reserve(utf16.size());
    for (const char16_t ch: utf16) {
        result.append(muse::Char(ch));
    }
    return result;
}

std::string normalizedCharsetName(const std::string& charset)
{
    std::string normalized;
    normalized.reserve(charset.size());
    for (const char ch: charset) {
        if (ch == ' ' || ch == '_') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized;
}

muse::String decodeWithFallback(const std::string& text, const std::string& charset)
{
    const std::string normalized = normalizedCharsetName(charset);

    if (normalized.empty() || normalized == "utf-8" || normalized == "utf8") {
        if (muse::UtfCodec::isValidUtf8(text)) {
            return muse::String::fromUtf8(text);
        }
        // MIDI meta events have no encoding marker.  UTF-8 is the declared
        // default, but a byte sequence that is not valid UTF-8 is far more
        // likely to be Windows-1252 text than a file that should be rejected.
        LOGW() << "MIDI import: meta text is not valid UTF-8; falling back to Windows-1252";
        return fromSingleByte(text, true);
    }

    if (normalized == "windows-1252" || normalized == "cp1252" || normalized == "win1252") {
        return fromSingleByte(text, true);
    }
    if (normalized == "iso-8859-1" || normalized == "iso8859-1"
        || normalized == "latin1" || normalized == "latin-1") {
        return fromSingleByte(text, false);
    }

    // A real multi-codec backend (ICU or libiconv) is required for the full
    // QTextCodec surface.  Until then, never guess an arbitrary encoding:
    // prefer strict UTF-8 and otherwise preserve Latin-1/Windows-1252 bytes.
    LOGW() << "MIDI import: unsupported charset" << charset
           << "in the Qt-free build; using UTF-8/Windows-1252 fallback";
    if (muse::UtfCodec::isValidUtf8(text)) {
        return muse::String::fromUtf8(text);
    }
    return fromSingleByte(text, true);
}
} // namespace

muse::String convertToCharset(const std::string& text)
{
    const auto* data = midiImportOperations.data();
    return decodeWithFallback(text, data ? data->charset : defaultCharset());
}

std::string defaultCharset()
{
    return "UTF-8";
}

std::string fromUchar(const unsigned char* text)
{
    return text ? std::string(reinterpret_cast<const char*>(text)) : std::string();
}
} // namespace MidiCharset

namespace MidiBar {
ReducedFraction findBarStart(const ReducedFraction& time, const engraving::TimeSigMap* sigmap)
{
    int barIndex, beat, tick;
    sigmap->tickValues(time.ticks(), &barIndex, &beat, &tick);
    return ReducedFraction::fromTicks(sigmap->bar2tick(barIndex, 0));
}
} // namespace MidiBar
namespace MidiDuration {
double durationCount(const std::vector<std::pair<ReducedFraction, engraving::TDuration> >& durations)
{
    double count = durations.size();
    for (const auto& d: durations) {
        if (d.second.dots()) {
            count += 0.5;
        }
    }
    return count;
}
} // namespace MidiDuration
} // namespace mu::iex::midi
