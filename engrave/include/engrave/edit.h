#pragma once

#include <cstdint>
#include <optional>
#include <tuple>
#include <variant>
#include <vector>

#include <engrave/error.h>
#include <engrave/handle.h>
#include <engrave/ops/measures.h>
#include <engrave/ops/notes.h>
#include <engrave/ops/parts.h>
#include <engrave/ops/property.h>
#include <engrave/ops/spanners.h>
#include <engrave/ops/text.h>

namespace engrave {

namespace detail {
template <typename... Groups>
struct OpVariant;

template <typename... Ops>
struct OpVariant<std::tuple<Ops...>> {
    using type = std::variant<Ops...>;
};

template <typename... A, typename... B, typename... Rest>
struct OpVariant<std::tuple<A...>, std::tuple<B...>, Rest...> : OpVariant<std::tuple<A..., B...>, Rest...> {};
} // namespace detail

// One operation of an edit batch. Each group of operations is declared in its
// own header under engrave/ops/.
using Op = detail::OpVariant<PropertyOps, NoteOps, MeasureOps, PartOps, SpannerOps, TextOps>::type;

// Operations that run in order as one transaction: all of them apply, or none.
struct Batch {
    std::vector<Op> ops;
};

// What one operation of a committed batch did.
struct OpResult {
    // The elements it created, in the order an OpRef{op, item} names them. An
    // element that takes no id (see handle.h), or that a later operation of the
    // batch removed, has none.
    std::vector<std::optional<ElementId>> created;
};

struct EditResult {
    std::uint64_t revision = 0; // the document's revision after the batch
    std::vector<OpResult> ops;  // one per operation
    // The ids the batch handed out and dropped, sorted. A dropped id is replaced
    // when a new element took its place: a duration change rebuilds a chord.
    std::vector<ElementId> created;
    std::vector<ElementId> removed;
    std::vector<ElementId> replaced;
    // Warnings the engine gave while it ran the batch.
    std::vector<Diagnostic> diagnostics;
};

} // namespace engrave
