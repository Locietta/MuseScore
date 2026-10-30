#include <engrave/handle.h>

#include "engraving/infrastructure/eid.h"

namespace engrave {

std::string to_string(const ElementId& id) {
    return mu::engraving::EID::fromParts(id.hi, id.lo).toStdString();
}

std::optional<ElementId> element_id_from_string(std::string_view text) {
    const mu::engraving::EID eid = mu::engraving::EID::fromStdString(text);
    if (!eid.isValid()) return std::nullopt;
    return ElementId{eid.first(), eid.second()};
}

} // namespace engrave
