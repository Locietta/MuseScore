#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <engrave/handle.h>

namespace engrave {

// Parts, staves and excerpts (A4 S7c).

// Adds a part from an instrument template (its id in instruments.xml) to the
// master score, before the part the ref names, or at the end.
struct AddPart {
    std::string instrument_id;
    std::optional<Ref> before;
};

// Removes a part of the master score and its staves. The excerpts that exist for
// the part go with it, and the excerpts that show it next to others lose it. A
// part of an excerpt and a score's last part are refused.
struct RemovePart {
    Ref part;
};

// Appends a staff to a part, and a linked staff to every excerpt that shows it:
// refused for a part an excerpt shows, because the linked staff does not survive a
// save and a reopen.
struct AddStaff {
    Ref part;
};

// Removes a staff, and the staff each excerpt links to it. The last staff of a
// part is refused: it goes with the part.
struct RemoveStaff {
    Ref staff;
};

// Generates one excerpt per master part, as the parts panel does. The engine
// cannot roll this back through the transaction's unwind (see api/ops/parts.cpp),
// so the operation is refused until the guard can restore it from the snapshot.
struct GenerateExcerpts {
    std::vector<Ref> parts;
};

using PartOps = std::tuple<AddPart, RemovePart, AddStaff, RemoveStaff, GenerateExcerpts>;

} // namespace engrave
