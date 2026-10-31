// Evidence cases of SetProperty, ResetProperty and SetStyle.

#include <format>
#include <string>

#include "engrave/tests/harness/harness.h"

#include "engraving/dom/engravingitem.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/property.h"
#include "engraving/style/style.h"

#include "engrave/src/api/pid_support.h"
#include "engrave/src/api/value_codec.h"

namespace engrave::harness {
namespace {

using mu::engraving::EngravingObject;

std::string expect_property(Document& document, const ElementId& id, std::string_view name, const Value& value) {
    if (property(document, id, name) != value) return std::format("{} did not take the value", name);
    // The change reached every linked copy the engine propagates it to.
    const mu::engraving::Pid pid = api::pid_of(name);
    EngravingObject& target = element(document, id);
    if (!target.isEngravingItem()) return {};
    const auto& item = static_cast<const mu::engraving::EngravingItem&>(target);
    for (const EngravingObject* linked : target.linkListForPropertyPropagation()) {
        if (linked == &target || !linked->isEngravingItem()) continue;
        const auto& copy = static_cast<const mu::engraving::EngravingItem&>(*linked);
        if (item.propertyPropagation(&copy, pid) == mu::engraving::PropertyPropagation::PROPAGATE
            && copy.getProperty(pid) != item.getProperty(pid)) {
            return std::format("a linked copy kept its old {}", name);
        }
    }
    return {};
}

std::string expect_default(Document& document, const ElementId& id, std::string_view name) {
    const mu::engraving::Pid pid = api::pid_of(name);
    const EngravingObject& target = element(document, id);
    if (target.getProperty(pid) != target.propertyDefault(pid)) return std::format("{} is not back at its default", name);
    return {};
}

// A case that sets or resets one property of the first element that matches.
OpCase property_case(std::string name, std::string fixture, std::string type, std::string property,
                     std::optional<Value> value, std::function<bool(const EngravingObject&)> match = {}) {
    struct Shared {
        ElementId target;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, type, property, value, match](Document& document) {
            shared->target = find_id(document, type, match);
            if (value) return Batch{{SetProperty{shared->target, property, *value}}};
            return Batch{{ResetProperty{shared->target, property}}};
        },
        .expect = [shared, property, value](Document& document, const Fingerprint&, const Fingerprint&) {
            return value ? expect_property(document, shared->target, property, *value)
                         : expect_default(document, shared->target, property);
        },
    };
}

OpCase style_case(std::string name, std::string fixture, std::optional<std::size_t> excerpt_index, std::string style,
                  Value value) {
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [excerpt_index, style, value](Document& document) {
            const ScoreSel score = excerpt_index ? excerpt(document, *excerpt_index) : ScoreSel{};
            return Batch{{SetStyle{score, style, value}}};
        },
        .expect = [excerpt_index, style, value](Document& document, const Fingerprint&, const Fingerprint&) -> std::string {
            mu::engraving::Score* score = &master_of(document);
            if (excerpt_index) score = master_of(document).excerpts()[*excerpt_index]->excerptScore();
            const mu::engraving::Sid sid = mu::engraving::MStyle::styleIdx(muse::String::fromStdString(style));
            if (api::to_value(mu::engraving::MStyle::valueType(sid), score->style().styleV(sid)) != value)
                return style + " did not take the value";
            return {};
        },
    };
}

bool hidden(const EngravingObject& object) {
    return !object.getProperty(mu::engraving::Pid::VISIBLE).toBool();
}

const bool registered = register_cases({
    property_case("SetProperty note color", "chordline_path.mscx", "Note", "color", Color{255, 0, 0}),
    property_case("SetProperty linked note color", "linked_parts.mscx", "Note", "color", Color{0, 0, 255}),
    property_case("SetProperty dynamic direction", "hairpins_and_repeats.mscx", "Dynamic", "direction", Enum{"up"}),
    property_case("SetProperty lyrics placement (styled)", "lyrics.mscx", "Lyrics", "placement", Enum{"above"}),
    property_case("SetProperty chord stem direction", "lyrics.mscx", "Chord", "StemDirection", Enum{"up"}),
    property_case("ResetProperty dynamic visible", "hairpins_and_repeats.mscx", "Dynamic", "visible", std::nullopt, hidden),
    property_case("ResetProperty pedal placement (styled)", "pedal.mscx", "Pedal", "placement", std::nullopt),
    property_case("ResetProperty symbol offset", "layout_elements.mscx", "Symbol", "offset", std::nullopt),
    style_case("SetStyle staff distance", "chordline_path.mscx", std::nullopt, "staffDistance", 9.0),
    style_case("SetStyle measure numbers", "repeat_with_2_voltas.mscx", std::nullopt, "showMeasureNumber", false),
    style_case("SetStyle excerpt page width", "linked_parts.mscx", 0, "pageWidth", 9.0),
});

} // namespace
} // namespace engrave::harness
