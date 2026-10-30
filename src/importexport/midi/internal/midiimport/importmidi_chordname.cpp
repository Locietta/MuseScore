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
#include "importmidi_chordname.h"
#include "importmidi_inner.h"
#include "importmidi_chord.h"
#include "importmidi_fraction.h"
#include "importmidi_operations.h"
#include "../midishared/midifile.h"

#include <string>
#include <vector>

#include "global/types/string.h"

#include "engraving/dom/masterscore.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/factory.h"

// From XF Format Specifications V 2.01 (January 13, 1999, YAMAHA CORPORATION)

using namespace mu::engraving;

namespace mu::iex::midi {
namespace MidiChordName {
int readFirstHalf(uchar byte)
{
    return (byte >> 4) & 0xf;
}

int readSecondHalf(uchar byte)
{
    return byte & 0xf;
}

muse::String readChordRoot(uchar byte)
{
    static const std::vector<std::string> inversions = {
        "bbb", "bb", "b", "", "#", "##", "###"
    };
    static const std::vector<std::string> notes = {
        "", "C", "D", "E", "F", "G", "A", "B"
    };

    muse::String chordRoot;

    const size_t noteIndex = readSecondHalf(byte);
    if (noteIndex < notes.size()) {
        chordRoot += muse::String::fromUtf8(notes[noteIndex]);
    }

    const size_t inversionIndex = readFirstHalf(byte);
    if (inversionIndex < inversions.size()) {
        chordRoot += muse::String::fromUtf8(inversions[inversionIndex]);
    }

    return chordRoot;
}

muse::String readChordType(uchar chordTypeIndex)
{
    static const std::vector<std::string> chordTypes = {
        "",           // Maj
        "6",          // Maj6
        "Maj7",
        "Maj7#11",    // Maj7(#11)
        "Maj9",       // Maj(9)
        "Maj9",       // Maj7(9)
        "6(9)",       // Maj6(9)
        "aug",
        "m",          // min
        "m6",         // min6
        "m7",         // min7
        "m7b5",       // min7b5
        "m9",         // min(9)
        "m9",         // min7(9)
        "m7(11)",     // min7(11)
        "mMaj7",      // minMaj7
        "mMaj9",      // minMaj7(9)
        "dim",
        "dim7",
        "7",          // 7th
        "7sus4",
        "7b5",
        "9",          // 7(9)
        "7#11",       // 7(#11)
        "7(13)",
        "7b9",        // 7(b9)
        "7b13",       // 7(b13)
        "7#9",        // 7(#9)
        "Maj7#5",     // Maj7aug
        "7#5",        // 7aug
        "",           // 1+8
        "5",          // 1+5
        "sus4",
        "sus2",       // 1+2+5
        "N.C."        // cc
    };

    if (chordTypeIndex < chordTypes.size()) {
        return muse::String::fromUtf8(chordTypes[chordTypeIndex]);
    }
    return muse::String();
}

muse::String readChordName(const MidiEvent& e)
{
    if (e.type() != ME_META || e.metaType() != META_SPECIFIC) {
        return muse::String();
    }
    if (e.len() < 4) {
        return muse::String();
    }

    const uchar* data = e.edata();
    if (data[0] != 0x43 || data[1] != 0x7B || data[2] != 0x01) {
        return muse::String();
    }

    muse::String chordName;

    if (e.len() >= 4) {
        chordName += readChordRoot(data[3]);
    }
    if (e.len() >= 5) {
        chordName += readChordType(data[4]);
    }

    if (e.len() >= 6) {
        const muse::String chordRoot = readChordRoot(data[5]);
        if (!chordRoot.isEmpty()) {
            muse::String chordType;
            if (e.len() >= 7) {
                chordType = readChordType(data[6]);
            }
            if (chordRoot + chordType == chordName) {
                chordName += u"/" + chordRoot;
            } else {
                chordName += u"/" + chordRoot + chordType;
            }
        }
    }

    return chordName;
}

muse::String findChordName(
    const std::vector<MidiNote>& notes,
    const std::multimap<ReducedFraction, muse::String>& chordNames)
{
    for (const MidiNote& note: notes) {
        const auto range = chordNames.equal_range(note.origOnTime);
        if (range.second == range.first) {
            continue;
        }
        // found chord names (usually only one)
        muse::String chordName;
        for (auto it = range.first; it != range.second; ++it) {
            if (it != range.first) {
                chordName += u"\n" + it->second;
            } else {
                chordName += it->second;
            }
        }
        return chordName;
    }
    return muse::String();
}

void findChordNames(const std::multimap<int, MTrack>& tracks)
{
    auto& data = *midiImportOperations.data();

    for (const auto& track: tracks) {
        for (const auto& event: track.second.mtrack->events()) {
            const MidiEvent& e = event.second;
            const muse::String chordName = readChordName(e);
            if (!chordName.isEmpty()) {
                const auto time = ReducedFraction::fromTicks(event.first);
                data.chordNames.insert({ time, chordName });
            }
        }
    }
}

// all notes should be already placed to the score

void setChordNames(std::vector<MTrack>& tracks)
{
    const auto& data = *midiImportOperations.data();
    if (data.chordNames.empty() || !data.trackOpers.showChordNames.value()) {
        return;
    }

    // chords here can have on time very different from the original one
    // before quantization, so we look for original on times
    // that are stored in notes
    std::set<ReducedFraction> usedTimes;        // don't use one tick for chord name twice

    for (MTrack& track: tracks) {
        for (const auto& chord: track.chords) {
            if (usedTimes.find(chord.first) != usedTimes.end()) {
                continue;
            }

            const MidiChord& c = chord.second;
            const muse::String chordName = findChordName(c.notes, data.chordNames);

            if (chordName.isEmpty()) {
                continue;
            }

            Staff* staff = track.staff;
            Score* score = staff->score();
            const ReducedFraction& onTime = chord.first;
            usedTimes.insert(onTime);

            Measure* measure = score->tick2measure(onTime.fraction());
            Segment* seg = measure->getSegment(SegmentType::ChordRest, onTime.fraction());
            const track_idx_t t = staff->idx() * VOICES;

            Harmony* h = mu::engraving::Factory::createHarmony(seg);
            h->setHarmony(chordName);
            h->setTrack(t);
            seg->add(h);
        }
    }
}
} // namespace MidiChordName
} // namespace mu::iex::midi
