#pragma once

#include <string_view>
#include <vector>

#include "engraving/dom/property.h"

namespace engrave::api {

// The catalogue's name of a property: its xml name, or its Pid name when the xml
// name is empty or already claimed by another Pid (see tools/gen_pid_support.py).
std::string_view name_of(mu::engraving::Pid pid);

// The inverse; Pid::END for a name the catalogue does not know.
mu::engraving::Pid pid_of(std::string_view name);

// Whether the catalogue lists this element type.
bool has_type(std::string_view type);

// The properties of an element type, in the catalogue's order (sorted by name).
// Empty for a type the catalogue does not list.
std::vector<mu::engraving::Pid> pids_of(std::string_view type);

} // namespace engrave::api
