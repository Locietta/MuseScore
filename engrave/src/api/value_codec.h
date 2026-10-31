#pragma once

#include <span>
#include <string_view>

#include <engrave/error.h>
#include <engrave/value.h>

#include "engraving/types/propertyvalue.h"

// The internal value codec of the API: the one place that knows how a property
// value type maps to engrave::Value.
namespace engrave::api {

using mu::engraving::P_TYPE;
using mu::engraving::PropertyValue;

ValueKind kind_of(P_TYPE type) noexcept;
Unit unit_of(P_TYPE type) noexcept;

// The MSCX tokens of an enum type, in TConv's order.  Empty for the types that
// are not enum, for SYMID (its token set is open: any SMuFL name) and for the
// composite ALIGN and ORNAMENT_INTERVAL tokens.
std::span<const std::string_view> enum_tokens(P_TYPE type) noexcept;

// Mirrors TRead::readPropertyValue and XmlWriter::tagProperty: the switch is over
// the property value type, not over the type the value happens to be stored as.
Value to_value(P_TYPE type, const PropertyValue& value);

// The inverse.  InvalidArgument when value does not fit the type; a bad enum
// token is InvalidArgument too, never a silent default.
Result<PropertyValue> from_value(P_TYPE type, const Value& value);

} // namespace engrave::api
