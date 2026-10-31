#pragma once

#include <optional>
#include <tuple>
#include <vector>

#include <engrave/handle.h>
#include <engrave/value.h>

namespace engrave {

// A note to enter. tpc is the concert-pitch spelling the engine keeps as tpc1; the
// written one follows the staff's transposition. Without it the key at the note's
// position decides, as it does when the same pitch is played in.
struct Pitch {
    int pitch = 0;
    std::optional<int> tpc;
};

// Enters a chord, or a rest when there are no pitches, at one staff and voice of a
// position (a SegmentAddress of kind "ChordRest" on a pitched staff). It is the
// engine's note entry: what already holds the position is shortened or filled up with
// rests, a duration that reaches past a barline is split there and tied, and a rest is
// entered the same way. The operation reports the chord or rest first among what it
// created (for an OpRef of a later operation), then its notes.
// A chord of several pitches is entered whole, so its entry has to end in the bar it
// starts in.
struct EnterNotes {
    Address at;
    Fraction duration; // of a whole note, e.g. {1, 4} for a quarter
    std::vector<Pitch> pitches;
};

// Sets the pitch and the spelling of a note through the engine's own pitch change,
// which reaches the note's linked copies in the other scores. A tied note changes on
// its own, as it does when the score's pitch keys move it, and a note of a tablature
// or of a drum set keeps its pitch: those are set with their fret, string or drum.
struct SetPitch {
    Ref note;
    int pitch = 0;
    std::optional<int> tpc;
};

// Changes the length of a chord or of a rest: the freed time is filled with rests, and
// a longer length is split at the barlines and tied.
struct SetDuration {
    Ref chord_rest;
    Fraction duration;
};

// Removes an element the way Delete does in the score. What can be removed is the
// engine's decision: a chord becomes a rest of its length, one note of a chord goes
// alone, and a rest of the first voice stays.
struct Remove {
    Ref target;
};

using NoteOps = std::tuple<EnterNotes, SetPitch, SetDuration, Remove>;

} // namespace engrave
