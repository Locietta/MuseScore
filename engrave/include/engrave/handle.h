#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <engrave/value.h>

namespace engrave {

// An element's id: the two values the writer stores in an MSCX <eid>, high half
// first. Ids are document-scoped, so two documents can hand out equal ones. A
// default-constructed ElementId is the id "A_A", not "no id": an element without
// one is never handed out.
struct ElementId {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    friend auto operator<=>(const ElementId&, const ElementId&) = default;
};

// The id's MSCX text, e.g. "Zt3Ql7kQ0f_BCAEMZTgx3K", so a value can be looked up
// in a saved score.
std::string to_string(const ElementId& id);
// The id that text names, or nothing when the text is not a valid id.
std::optional<ElementId> element_id_from_string(std::string_view text);

// The master score, or one of its excerpts (linked parts).
struct ScoreSel {
    std::optional<ElementId> excerpt; // unset: the master score
};

// A position in a measure. Segments have no id of their own (layout rebuilds
// them), so an operation that works at a position names it this way. staff and
// voice select the element within the segment; the segment itself is found from
// the measure, the tick and the kind.
struct SegmentAddress {
    ElementId measure;
    std::int32_t staff = 0; // staff index, 0-based
    std::int32_t voice = 0; // voice, 0-based
    Fraction tick;          // position in the score, not in the measure
    std::string kind;       // segment type token, e.g. "ChordRest"
};

// A page or a system of the layout. Read-only: layout recreates both.
struct PageAddress {
    ScoreSel score;
    std::int32_t index = 0;
};

struct SystemAddress {
    ScoreSel score;
    std::int32_t page = 0;
    std::int32_t index = 0;
};

// A spanner's layout segment, likewise read-only.
struct SpannerSegmentAddress {
    ElementId spanner;
    std::int32_t index = 0;
};

using Address = std::variant<SegmentAddress, PageAddress, SystemAddress, SpannerSegmentAddress>;

// The item-th element that the op-th operation of the same batch created.
struct OpRef {
    std::size_t op = 0;
    std::size_t item = 0;
};

// What an operation acts on: an element by id, a position, or something an
// earlier operation of the same batch created.
using Ref = std::variant<ElementId, Address, OpRef>;

} // namespace engrave
