#pragma once

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <engrave/handle.h>
#include <engrave/value.h>

namespace engrave {

// Spanners, ties and tuplets (A4 S7d).

// Adds a spanner of an element type (its name as describe() gives it: "Slur",
// "HairPin", "Pedal", "TextLine", "Ottava", ...) from start to end. A slur spans
// two chords or rests; a spanner the engine anchors to a segment starts at the
// segment of start and ends at the segment of end, on the staff of start, and
// reaches the linked staves the engine copies it to. The values in props are set
// as SetProperty sets them, once the spanner is in the score.
//
// A type the engine creates some other way (a tie, a glissando, a volta) is
// InvalidArgument: AddTie is the tie's operation.
struct AddSpanner {
    std::string type;
    Ref start;
    Ref end;
    std::vector<std::pair<std::string, Value>> props;
};

// Ties note to the next note of the same pitch in its voice, and to the tie jump
// points when a repeat lies between them. It changes nothing else: EditRejected
// when the note is already tied, when nothing follows it, or when the note it
// would tie to already has a tie.
//
// A tie has no id the document hands out (an id is a persisted handle, and a tie
// is rebuilt from its notes on read), so it is not in created/removed and an
// OpRef to it is StaleHandle.
struct AddTie {
    Ref note;
};

// Divides every chord or rest from first to last into a tuplet of actual notes in
// the time of normal: both name one voice, and the engine refuses a range that
// crosses a barline or a parent tuplet (EditRejected). The tuplet's number and
// bracket follow the score's style. The chord or rest it starts with is replaced
// by the tuplet's first element, so its id is in the batch's replaced.
struct AddTuplet {
    Ref first;
    Ref last;
    int actual = 3;
    int normal = 2;
};

using SpannerOps = std::tuple<AddSpanner, AddTie, AddTuplet>;

} // namespace engrave
