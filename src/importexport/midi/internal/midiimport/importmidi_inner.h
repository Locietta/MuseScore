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
#pragma once

#include <cassert>
#include <string>
#include <utility>
#include <vector>

#include "global/types/string.h"

#include "importexport/midi/internal/midishared/generalmidi.h"

// The MIDI importer is compiled both by the Qt application and by the Qt-free
// native host (NO_QT_SUPPORT).  Keep the debug macros local to this module
// instead of pulling in QtGlobal for the Qt-free build.  Release builds retain
// the upstream behavior: assertions compile away.
#ifndef Q_ASSERT
#define Q_ASSERT(condition) assert(condition)
#endif
#ifndef Q_ASSERT_X
#define Q_ASSERT_X(condition, where, what) assert(condition)
#endif
#ifndef Q_UNUSED
#define Q_UNUSED(value) (void)(value)
#endif

#include "importmidi_chord.h"
#include "importmidi_fraction.h"
#include "importmidi_tuplet.h"
#include "importmidi_operation.h"

#include "engraving/types/types.h"

// ---------------------------------------------------------------------------------------
// These inner classes definitions are used in cpp files only
// Include this header to link tests
// ---------------------------------------------------------------------------------------

namespace mu::engraving {
enum class Key : signed char;
class Staff;
class Score;
class TDuration;
class DurationElement;
class Measure;
class KeyList;
}

namespace mu::iex::midi {
struct MidiTimeSig;

namespace Meter {
// max level for tuplets: duration cannot go over the tuplet boundary
// this level should be greater than any other level
inline constexpr int TUPLET_BOUNDARY_LEVEL = 10;

struct MaxLevel
{
    int level = 0;                     // 0 - the biggest, whole bar level; other: -1, -2, ...
    int levelCount = 0;                // number of positions with 'level' value
    ReducedFraction pos = { -1, 1 };   // first position with value 'level'; -1 - undefined pos
};

struct DivLengthInfo
{
    ReducedFraction len;
    int level;
};

struct DivisionInfo
{
    ReducedFraction onTime;          // division start tick (tick is counted from the beginning of bar)
    ReducedFraction len;             // length of this whole division
    bool isTuplet = false;
    std::vector<DivLengthInfo> divLengths;      // lengths of 'len' subdivisions
};

enum class DurationType : char;

ReducedFraction userTimeSigToFraction(
    MidiOperations::TimeSigNumerator timeSigNumerator, MidiOperations::TimeSigDenominator timeSigDenominator);
MidiOperations::TimeSigNumerator fractionNumeratorToUserValue(int n);
MidiOperations::TimeSigDenominator fractionDenominatorToUserValue(int z);
} // namespace Meter

class MidiTrack;
class MidiChord;
class MidiEvent;

class MTrack
{
public:                 // chords store tuplet iterators, so we need to copy class explicitly
    MTrack() = default;
    MTrack(const MTrack& other);
    MTrack& operator=(MTrack other);

    GM1Program program = GM1Program::AcousticGrandPiano;
    engraving::Staff* staff = nullptr;
    const MidiTrack* mtrack = nullptr;
    muse::String name;
    bool hasKey = false;
    int indexOfOperation = 0;
    int division = 0;
    bool isDivisionInTps = false;         // ticks per second
    bool hadInitialNotes = false;

    std::multimap<ReducedFraction, int> volumes;
    std::multimap<ReducedFraction, MidiChord> chords;
    std::multimap<ReducedFraction, MidiTuplet::TupletData> tuplets;     // <tupletOnTime, ...>

    void createNotes(const ReducedFraction& lastTick);
    void processPendingNotes(std::vector<MidiChord>& midiChords, int voice, const ReducedFraction& startChordTickFrac,
                             const ReducedFraction& nextChordTick);
    void processMeta(int tick, const MidiEvent& mm);
    void fillGapWithRests(engraving::Score* score, int voice, const ReducedFraction& startChordTickFrac, const ReducedFraction& restLength,
                          engraving::track_idx_t track);
    std::vector<std::pair<ReducedFraction, engraving::TDuration> >
    toDurationList(const engraving::Measure* measure, int voice, const ReducedFraction& startTick, const ReducedFraction& len,
                   Meter::DurationType durationType);
    void createKeys(engraving::Key defaultKey, const engraving::KeyList& allKeyList);
    void updateTupletsFromChords();

private:
    void updateTuplet(std::multimap<ReducedFraction, MidiTuplet::TupletData>::iterator&);
};

namespace MidiTuplet {
struct TupletInfo
{
    int id;
    ReducedFraction onTime = { -1, 1 };  // invalid
    ReducedFraction len = { -1, 1 };
    int tupletNumber = -1;
    // <chord onTime, chord iterator>
    std::map<ReducedFraction, std::multimap<ReducedFraction, MidiChord>::iterator> chords;
    ReducedFraction tupletSumError;
    ReducedFraction regularSumError;
    ReducedFraction sumLengthOfRests;
    int firstChordIndex = -1;
    std::map<ReducedFraction, int> staccatoChords;        // <onTime, note index>
};

bool haveIntersection(const std::pair<ReducedFraction, ReducedFraction>& interval1, const std::pair<ReducedFraction,
                                                                                                    ReducedFraction>& interval2,
                      bool strictComparison = true);
bool haveIntersection(const std::pair<ReducedFraction, ReducedFraction>& interval, const std::vector<std::pair<ReducedFraction,
                                                                                                               ReducedFraction> >& intervals, bool strictComparison = true);
} // namespace MidiTuplet

namespace MidiCharset {
// Decodes a NUL-terminated MIDI meta-event payload.  The requested charset comes
// from the optional MIDI-import operations XML; without one it is UTF-8.
// See importmidi_inner.cpp for the Qt-free fallback policy.
muse::String convertToCharset(const std::string& text);
std::string defaultCharset();
std::string fromUchar(const unsigned char* text);
} // namespace MidiCharset

namespace MidiBar {
ReducedFraction findBarStart(const ReducedFraction& time, const engraving::TimeSigMap* sigmap);
} // namespace MidiBar

namespace MidiDuration {
double durationCount(const std::vector<std::pair<ReducedFraction, engraving::TDuration> >& durations);
} // namespace MidiDuration
} // namespace mu::iex::midi
