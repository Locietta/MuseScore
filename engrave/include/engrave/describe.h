#pragma once

#include <string>
#include <vector>

#include <engrave/value.h>

namespace engrave {

// One property an element type handles.
struct PropInfo {
    // The name an edit or a read uses: the property's MSCX name, or its engine name
    // when that is empty or shared with another property (all names are unique).
    std::string name;
    ValueKind kind = ValueKind::Opaque;
    Unit unit = Unit::None;
    // The accepted MSCX tokens of an Enum kind, in TConv order.  Empty for the
    // kinds that are not enum and for the enum types whose token set is open
    // (SYMID) or composite (Align, OrnamentInterval).
    std::vector<std::string> enum_tokens;
    // The engine's setProperty takes it, and it is not a structural property
    // (track, tick, generated, selected, pitch, tpc) that only structural
    // operations may change.
    bool settable = false;
    // The value propagates to the linked parts of the element.
    bool link = false;
    // "position", "text", "appearance" or "none": how the property propagates
    // between a score and its parts.
    std::string group;
};

// One element type of the engine, named by its MSCX type name.
struct TypeInfo {
    std::string type;            // e.g. "Note"
    std::vector<PropInfo> props; // sorted by name; empty for a type whose getProperty answers nothing
};

// One score style.
struct StyleInfo {
    std::string name; // the Sid name, e.g. "pageWidth"
    ValueKind kind = ValueKind::Opaque;
    Unit unit = Unit::None;
    std::vector<std::string> enum_tokens;
    Value default_value;
};

} // namespace engrave
