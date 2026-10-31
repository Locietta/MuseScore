// Evidence cases of AddText and Add.

#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engrave/tests/harness/harness.h"

#include "engraving/dom/articulation.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/fermata.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/textbase.h"
#include "engraving/types/symnames.h"
#include "engraving/types/typesconv.h"

#include "engrave/src/api/handles.h"

namespace engrave::harness {
namespace {

using mu::engraving::Articulation;
using mu::engraving::Chord;
using mu::engraving::Dynamic;
using mu::engraving::DynamicType;
using mu::engraving::ElementType;
using mu::engraving::EngravingItem;
using mu::engraving::EngravingObject;
using mu::engraving::Lyrics;
using mu::engraving::Measure;
using mu::engraving::Score;
using mu::engraving::Segment;
using mu::engraving::SymId;
using mu::engraving::TextBase;

using Expect = std::function<std::string(Document&, const SegmentAddress&)>;

// --- the document as the checks find it ----------------------------------------

Measure& measure_of(Score& score, std::size_t index) {
    Measure* measure = score.firstMeasure();
    for (std::size_t i = 0; i < index && measure; ++i) measure = measure->nextMeasure();
    if (!measure) throw std::invalid_argument(std::format("the score has no measure {}", index));
    return *measure;
}

// A ChordRest segment of a score's measure, with the staff and the voice it belongs to.
SegmentAddress address_in(Score& score, std::size_t index, std::int32_t staff, std::int32_t voice, Fraction offset) {
    const Measure& measure = measure_of(score, index);
    const mu::engraving::Fraction tick = measure.tick()
                                         + mu::engraving::Fraction(static_cast<int>(offset.num), static_cast<int>(offset.den));
    SegmentAddress address;
    address.measure = from_engine(measure.eid());
    address.staff = staff;
    address.voice = voice;
    address.tick = {tick.numerator(), tick.denominator()};
    address.kind = "ChordRest";
    return address;
}

Segment& segment_of(Document& document, const SegmentAddress& address) {
    const Result<Resolved> found = resolve(master_of(document), Address{address});
    if (!found) throw std::invalid_argument(found.error().message);
    return *mu::engraving::toSegment(found->object);
}

mu::engraving::track_idx_t track_of(const SegmentAddress& address) {
    return mu::engraving::staff2track(address.staff, address.voice);
}

// --- what a case made ----------------------------------------------------------

// The text of a type at an address's track, found by its text: no fixture has the
// text a case writes.
EngravingItem* find_text(Segment& segment, ElementType type, std::string_view text, mu::engraving::track_idx_t track) {
    for (EngravingItem* item : segment.findAnnotations(type, track, track)) {
        if (item->isTextBase() && mu::engraving::toTextBase(item)->plainText().toStdString() == text) return item;
    }
    return nullptr;
}

std::string expect_text(Document& document, const SegmentAddress& at, ElementType type, std::string_view text) {
    EngravingItem* item = find_text(segment_of(document, at), type, text, track_of(at));
    if (!item) return std::format("the segment holds no {} with the new text", mu::engraving::TConv::toXml(type).ascii());
    return item->eid().isValid() ? std::string{} : "the new text has no id";
}

// A system object is copied to every score; each copy takes its own id.
std::string expect_text_in_every_score(Document& document, const SegmentAddress& at, ElementType type,
                                       std::string_view text) {
    const std::string problem = expect_text(document, at, type, text);
    if (!problem.empty()) return problem;
    const EngravingItem* item = find_text(segment_of(document, at), type, text, track_of(at));
    for (const mu::engraving::Excerpt* excerpt : master_of(document).excerpts()) {
        const EngravingItem* copy = item->findLinkedInScore(excerpt->excerptScore());
        if (!copy) return "an excerpt has no copy of the new text";
        if (!copy->eid().isValid()) return "a copy in an excerpt has no id";
        if (mu::engraving::toTextBase(copy)->plainText().toStdString() != text) return "a copy in an excerpt has another text";
    }
    return {};
}

std::string expect_lyrics(Document& document, const SegmentAddress& at, std::string_view text, int verse) {
    Segment& segment = segment_of(document, at);
    EngravingItem* item = segment.element(track_of(at));
    if (!item || !item->isChord()) return "the address no longer holds a chord";
    for (const Lyrics* lyrics : mu::engraving::toChord(item)->lyrics()) {
        if (lyrics->plainText().toStdString() != text) continue;
        if (!lyrics->eid().isValid()) return "the new lyrics have no id";
        if (lyrics->track() != track_of(at)) return "the lyrics are on another track";
        return lyrics->verse() == verse ? std::string{} : "the lyrics got another verse";
    }
    return "the chord has no lyrics with the new text";
}

std::string expect_articulation(Document& document, const ElementId& chord_id, std::string_view symbol) {
    const EngravingObject& anchor = element(document, chord_id);
    if (!anchor.isChord()) return "the anchor is no longer a chord";
    const Chord& chord = *mu::engraving::toChord(&anchor);
    for (const Articulation* articulation : chord.articulations()) {
        if (mu::engraving::SymNames::nameForSymId(articulation->symId()).ascii() != symbol) continue;
        if (!articulation->eid().isValid()) return "the new articulation has no id";
        return articulation->track() == chord.track() ? std::string{} : "the articulation is on another track";
    }
    return std::format("the chord has no '{}' articulation", symbol);
}

std::string expect_fermata(Document& document, const SegmentAddress& at, std::string_view symbol) {
    Segment& segment = segment_of(document, at);
    for (EngravingItem* item : segment.findAnnotations(ElementType::FERMATA, track_of(at), track_of(at))) {
        if (!item->isFermata()) continue;
        if (mu::engraving::SymNames::nameForSymId(mu::engraving::toFermata(item)->symId()).ascii() != symbol) continue;
        return item->eid().isValid() ? std::string{} : "the new fermata has no id";
    }
    return std::format("the segment has no '{}' fermata", symbol);
}

std::string expect_dynamic(Document& document, const SegmentAddress& at, DynamicType type, std::string_view text) {
    Segment& segment = segment_of(document, at);
    for (EngravingItem* item : segment.findAnnotations(ElementType::DYNAMIC, track_of(at), track_of(at))) {
        if (!item->isDynamic() || mu::engraving::toDynamic(item)->dynamicType() != type) continue;
        if (mu::engraving::toDynamic(item)->xmlText().toStdString() != text) return "the dynamic has another text";
        return item->eid().isValid() ? std::string{} : "the new dynamic has no id";
    }
    return "the segment has no dynamic of the given type";
}

// --- the cases ------------------------------------------------------------------

// AddText at one staff and voice of a position; the check reads the segment.
OpCase add_text_case(std::string name, std::string fixture, std::size_t measure, std::int32_t staff, std::int32_t voice,
                     Fraction offset, std::string style, std::string text, Expect expect) {
    struct Shared {
        SegmentAddress at;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, measure, staff, voice, offset, style, text](Document& document) {
            shared->at = address_in(master_of(document), measure, staff, voice, offset);
            return Batch{{AddText{shared->at, style, text}}};
        },
        .expect = [shared, expect](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect(document, shared->at);
        },
    };
}

// Add on a segment of the master score.
OpCase add_to_segment_case(std::string name, std::string fixture, std::size_t measure, std::int32_t staff,
                           std::int32_t voice, Fraction offset, std::string type,
                           std::vector<std::pair<std::string, Value>> props, Expect expect) {
    struct Shared {
        SegmentAddress at;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, measure, staff, voice, offset, type, props](Document& document) {
            shared->at = address_in(master_of(document), measure, staff, voice, offset);
            return Batch{{Add{shared->at, type, props}}};
        },
        .expect = [shared, expect](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect(document, shared->at);
        },
    };
}

// Add on the first chord of the fixture.
OpCase add_to_chord_case(std::string name, std::string fixture, std::string type,
                         std::vector<std::pair<std::string, Value>> props,
                         std::function<std::string(Document&, const ElementId&)> expect) {
    struct Shared {
        ElementId chord;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, type, props](Document& document) {
            shared->chord = find_id(document, "Chord");
            return Batch{{Add{shared->chord, type, props}}};
        },
        .expect = [shared, expect](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect(document, shared->chord);
        },
    };
}

const bool registered = register_cases({
    add_text_case("AddText staff text", "chordline_path.mscx", 0, 0, 0, {0, 1}, "staff", "dolce",
                  [](Document& document, const SegmentAddress& at) {
                      return expect_text(document, at, ElementType::STAFF_TEXT, "dolce");
                  }),
    add_text_case("AddText lyrics", "lyrics.mscx", 0, 0, 0, {0, 1}, "lyrics_odd", "la",
                  [](Document& document, const SegmentAddress& at) {
                      return expect_lyrics(document, at, "la", 1);
                  }),
    add_text_case("AddText tempo text", "repeat_with_2_voltas.mscx", 0, 0, 0, {0, 1}, "tempo", "Allegro",
                  [](Document& document, const SegmentAddress& at) {
                      return expect_text(document, at, ElementType::TEMPO_TEXT, "Allegro");
                  }),
    add_text_case("AddText system text in linked parts", "linked_parts.mscx", 0, 0, 0, {0, 1}, "system", "solo",
                  [](Document& document, const SegmentAddress& at) {
                      return expect_text_in_every_score(document, at, ElementType::SYSTEM_TEXT, "solo");
                  }),

    add_to_chord_case("Add articulation to a chord", "chordline_path.mscx", "Articulation",
                      {{"symbol", Enum{"articStaccatoAbove"}}},
                      [](Document& document, const ElementId& chord) {
                          return expect_articulation(document, chord, "articStaccatoAbove");
                      }),
    add_to_segment_case("Add fermata to a segment", "chordline_path.mscx", 0, 0, 0, {0, 1}, "Fermata",
                        {{"symbol", Enum{"fermataAbove"}}},
                        [](Document& document, const SegmentAddress& at) {
                            return expect_fermata(document, at, "fermataAbove");
                        }),
    add_to_segment_case("Add dynamic to a segment", "hairpins_and_repeats.mscx", 0, 0, 0, {0, 1}, "Dynamic",
                        {{"DYNAMIC_TYPE", Enum{"ff"}}, {"text", std::string("<sym>dynamicFF</sym>")}},
                        [](Document& document, const SegmentAddress& at) {
                            return expect_dynamic(document, at, DynamicType::FF, "<sym>dynamicFF</sym>");
                        }),
    add_to_segment_case("Add staff text in linked parts", "linked_parts.mscx", 0, 0, 0, {0, 1}, "StaffText",
                        {{"text", std::string("dolce")}},
                        [](Document& document, const SegmentAddress& at) {
                            return expect_text_in_every_score(document, at, ElementType::STAFF_TEXT, "dolce");
                        }),
    add_to_segment_case("Add tempo text to a segment", "repeat_with_2_voltas.mscx", 0, 0, 0, {0, 1}, "Tempo",
                        {{"text", std::string("Vivace")}},
                        [](Document& document, const SegmentAddress& at) {
                            return expect_text(document, at, ElementType::TEMPO_TEXT, "Vivace");
                        }),
});

} // namespace
} // namespace engrave::harness
