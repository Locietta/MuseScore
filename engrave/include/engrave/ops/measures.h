#pragma once

#include <optional>
#include <string>
#include <tuple>

#include <engrave/handle.h>
#include <engrave/value.h>

namespace engrave {

// Measures, time and key signatures, clefs and layout breaks (A4 S7b).  The slice
// defines its operations here and lists them below; nothing else in the public
// headers changes.

// Inserts measures or frames, before `before` or at the end of the score.  A measure
// goes into every score of the document, a frame into the master only.  Every
// inserted element is created in order, so OpRef{op, 0} is the first one.
struct InsertMeasures {
    std::optional<Ref> before;    // unset: after the last measure or frame
    int count;                    // at least one
    std::string kind = "Measure"; // "Measure", "VBox", "HBox", "TBox" or "FBox"
};

// Removes the measures from `first` to `last`, both included, and replaces their time
// signature after the range the way the engine's own measure deletion does.  Frames
// between the two go as well.
struct RemoveMeasures {
    Ref first, last; // both measures of one score, in score order
};

// Sets the time signature that starts at `measure` and holds until the next one.
// The measures of that range are rewritten, so their ids may be replaced.
struct SetTimeSig {
    Ref measure;
    Fraction sig; // numerator and denominator, both positive
};

// Sets the key signature at `measure`: on one staff, or on every staff of the score
// when `staff` is unset.  Linked staves follow.
struct SetKeySig {
    Ref measure;
    int key;                  // accidentals, -7 (flats) to 7 (sharps)
    std::optional<Ref> staff; // unset: every staff
};

// Puts a clef before the element at `at`: a staff and voice select it within the
// segment.  Every staff the engine links to that one follows, since a clef belongs to
// a staff and not to a voice.  A clef at the start of a measure that has one before it
// goes at the end of that measure, as it does when it is dropped there in the editor.
struct InsertClef {
    Address at;
    std::string clef; // a clef type token: "G", "F", "C3", "PERC", ...
};

// Turns one layout break of a measure or frame on or off: "line", "page", "section"
// or "nobreak".  A break the new one replaces is turned off, as the editor does.  A
// break set on a master measure stays in the master; a part keeps its own, so a break
// for a part is set on the part's measure.
struct SetLayoutBreak {
    Ref measure;
    std::string kind;
    bool on;
};

using MeasureOps = std::tuple<InsertMeasures, RemoveMeasures, SetTimeSig, SetKeySig, InsertClef, SetLayoutBreak>;

} // namespace engrave
