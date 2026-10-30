#pragma once

#include <string>
#include <tuple>

#include <engrave/handle.h>
#include <engrave/value.h>

namespace engrave {

// Sets one property of an element. property is the catalogue's name
// (PropInfo::name) and must be settable on the element's type; value must have
// the property's kind. Like an edit in the score, the change reaches the
// element's linked copies in the other scores, and a styled property stops
// following its style.
struct SetProperty {
    Ref target;
    std::string property;
    Value value;
};

// Returns a property to its default: the style's value for a styled property,
// which then follows the style again.
struct ResetProperty {
    Ref target;
    std::string property;
};

// Sets a style of the master score or of one excerpt. style is the style's
// name (StyleInfo::name) and value must have its kind.
struct SetStyle {
    ScoreSel score;
    std::string style;
    Value value;
};

using PropertyOps = std::tuple<SetProperty, ResetProperty, SetStyle>;

} // namespace engrave
