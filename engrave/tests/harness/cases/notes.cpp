// Evidence cases of EnterNotes, SetPitch, SetDuration and Remove.

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engrave/tests/harness/harness.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"

#include "engrave/src/api/handles.h"

namespace engrave::harness {
namespace {

using mu::engraving::Chord;
using mu::engraving::EngravingItem;
using mu::engraving::EngravingObject;
using mu::engraving::Measure;
using mu::engraving::Note;
using mu::engraving::Score;
using mu::engraving::Segment;

using Matcher = std::function<bool(const EngravingObject&)>;

// What a removal needs to know about the element it took out.
struct Removal {
    ElementId target;
    std::optional<SegmentAddress> at; // where the chord or rest, or the note's chord, sat
    Fraction length;
    std::vector<int> remaining;      // what the chord keeps when one of its notes goes
    std::vector<ElementId> linked;   // the copies linked to the target
};

// --- the document as the checks find it -----------------------------------------

bool same(Fraction left, Fraction right) {
    return left.num * right.den == right.num * left.den;
}

// The measure at an index of a score, in score order.
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

// Where a chord or rest sits, as an address that outlives its id.
SegmentAddress address_of(const mu::engraving::ChordRest& cr) {
    SegmentAddress address;
    address.measure = from_engine(cr.measure()->eid());
    address.staff = static_cast<std::int32_t>(cr.staffIdx());
    address.voice = static_cast<std::int32_t>(cr.voice());
    address.tick = {cr.tick().numerator(), cr.tick().denominator()};
    address.kind = "ChordRest";
    return address;
}

Segment& segment_of(Document& document, const SegmentAddress& address) {
    const Result<Resolved> found = resolve(master_of(document), Address{address});
    if (!found) throw std::invalid_argument(found.error().message);
    return *mu::engraving::toSegment(found->object);
}

// The chord or rest at an address, or nothing.
EngravingItem* item_at(Document& document, const SegmentAddress& address) {
    return segment_of(document, address).element(mu::engraving::staff2track(address.staff, address.voice));
}

// The element a reference names now, or nothing.
EngravingObject* find(Document& document, const Ref& ref) {
    const Result<Resolved> found = std::holds_alternative<ElementId>(ref)
                                   ? resolve(master_of(document), std::get<ElementId>(ref))
                                   : resolve(master_of(document), std::get<Address>(ref));
    return found ? found->object : nullptr;
}

Fraction length_of(const EngravingItem& item) {
    const mu::engraving::Fraction ticks = mu::engraving::toChordRest(&item)->ticks();
    return {ticks.numerator(), ticks.denominator()};
}

std::vector<int> pitches_of(const EngravingItem& item) {
    std::vector<int> pitches;
    if (!item.isChord()) return pitches;
    for (const Note* note : mu::engraving::toChord(&item)->notes()) pitches.push_back(note->pitch());
    std::ranges::sort(pitches);
    return pitches;
}

// --- the checks -----------------------------------------------------------------

// What a note entry left at an address: a rest, or a chord with these pitches.
std::string expect_entered(Document& document, const SegmentAddress& address, Fraction length, std::vector<int> pitches) {
    EngravingItem* item = item_at(document, address);
    if (!item) return "nothing was entered";
    if (!same(length_of(*item), length)) return "the entered chord or rest has another length";
    if (pitches.empty()) return item->isRest() ? std::string{} : "a rest was not entered";
    if (!item->isChord()) return "a chord was not entered";
    std::ranges::sort(pitches);
    return pitches_of(*item) == pitches ? std::string{} : "the entered chord has other pitches";
}

std::string expect_pitch(Document& document, const ElementId& id, int pitch, std::optional<int> tpc,
                         const std::vector<int> others) {
    EngravingObject* object = find(document, id);
    if (!object) return "the note is gone";
    if (!object->isNote()) return "the id no longer names a note";
    Note& note = *mu::engraving::toNote(object);
    if (note.pitch() != pitch) return std::format("the pitch is {} and not {}", note.pitch(), pitch);
    // The engine's own spelling of that pitch at that position, unless the request gave one.
    if (note.tpc1() != tpc.value_or(note.tpc1default(pitch))) return "the concert spelling is another one";
    if (note.staff()->transpose(note.chord()->tick()).isZero() && note.tpc2() != note.tpc1())
        return "the written spelling differs on a staff that does not transpose";
    for (const int other : others) {
        if (!std::ranges::contains(pitches_of(note), other)) return std::format("the chord lost its pitch {}", other);
    }
    // The engine changes the pitch of every linked note with the one it was given.
    for (const EngravingObject* linked : note.linkList()) {
        if (linked == &note || !linked->isNote()) continue;
        if (mu::engraving::toNote(linked)->pitch() != pitch) return "a linked note kept its pitch";
    }
    return {};
}

// --- the cases ------------------------------------------------------------------

OpCase enter_case(std::string name, std::string fixture, std::size_t measure, std::int32_t staff, std::int32_t voice,
                  Fraction offset, Fraction duration, std::vector<Pitch> pitches,
                  std::function<std::string(Document&, const SegmentAddress&)> expect) {
    struct Shared {
        SegmentAddress at;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, measure, staff, voice, offset, duration, pitches](Document& document) {
            shared->at = address_in(master_of(document), measure, staff, voice, offset);
            return Batch{{EnterNotes{shared->at, duration, pitches}}};
        },
        .expect = [shared, expect](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect(document, shared->at);
        },
    };
}

// One pitch change of the first note that matches; the check reads the note and its links.
OpCase pitch_case(std::string name, std::string fixture, int pitch, std::optional<int> tpc, Matcher match = {}) {
    struct Shared {
        ElementId note;
        std::vector<int> others; // what the rest of the chord keeps
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, match, pitch, tpc](Document& document) {
            shared->note = find_id(document, "Note", match);
            const Note& note = *mu::engraving::toNote(&element(document, shared->note));
            shared->others = pitches_of(note);
            std::erase(shared->others, note.pitch());
            return Batch{{SetPitch{shared->note, pitch, tpc}}};
        },
        .expect = [shared, pitch, tpc](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect_pitch(document, shared->note, pitch, tpc, shared->others);
        },
    };
}

// A length change of the first chord or rest of a type that matches.
OpCase duration_case(std::string name, std::string fixture, std::string type, Fraction duration, Matcher match = {}) {
    struct Shared {
        ElementId chord_rest;
        SegmentAddress at;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, type, duration, match](Document& document) {
            shared->chord_rest = find_id(document, type, match);
            shared->at = address_of(*mu::engraving::toChordRest(&element(document, shared->chord_rest)));
            return Batch{{SetDuration{shared->chord_rest, duration}}};
        },
        .expect = [shared, duration](Document& document, const Fingerprint&, const Fingerprint&) {
            EngravingObject* object = find(document, shared->chord_rest);
            if (!object) return std::string("the chord or rest is gone");
            if (item_at(document, shared->at) != object) return std::string("the segment holds another element");
            return same(length_of(*mu::engraving::toEngravingItem(object)), duration) ? std::string{}
                                                                                     : "it has another length";
        },
    };
}

// A removal of the first element of a type that matches.
OpCase remove_case(std::string name, std::string fixture, std::string type, Matcher match,
                   std::function<std::string(Document&, const Removal&)> expect) {
    struct Shared {
        Removal removal;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, type, match](Document& document) {
            Removal& removal = shared->removal;
            removal.target = find_id(document, type, match);
            EngravingObject& object = element(document, removal.target);
            for (const EngravingObject* linked : object.linkList()) {
                if (linked != &object && takes_id(*linked) && linked->eid().isValid())
                    removal.linked.push_back(from_engine(linked->eid()));
            }
            if (object.isEngravingItem() && mu::engraving::toEngravingItem(&object)->isChordRest()) {
                EngravingItem& item = *mu::engraving::toEngravingItem(&object);
                removal.at = address_of(*mu::engraving::toChordRest(&item));
                removal.length = length_of(item);
            } else if (object.isNote()) {
                Note& note = *mu::engraving::toNote(&object);
                removal.at = address_of(*note.chord());
                removal.remaining = pitches_of(*note.chord());
                std::erase(removal.remaining, note.pitch());
            }
            return Batch{{Remove{removal.target}}};
        },
        .expect = [shared, expect](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect(document, shared->removal);
        },
    };
}

Matcher is_pitch(int pitch) {
    return [pitch](const EngravingObject& object) { return mu::engraving::toNote(&object)->pitch() == pitch; };
}

Matcher in_chord_of(std::size_t notes) {
    return [notes](const EngravingObject& object) {
        const Chord* chord = mu::engraving::toNote(&object)->chord();
        return chord && chord->notes().size() == notes;
    };
}

// What is left of a removed chord: nothing, and a rest of its length in its place.
std::string expect_removed_chord(Document& document, const Removal& removal) {
    if (find(document, removal.target)) return "the chord is still there";
    for (const ElementId& linked : removal.linked) {
        if (find(document, linked)) return "a linked copy is still there";
    }
    if (!removal.at) return "the case did not record where the chord sat";
    EngravingItem* item = item_at(document, *removal.at);
    if (!item || !item->isRest()) return "no rest took its place";
    return same(length_of(*item), removal.length) ? std::string{} : "the rest has another length";
}

// What is left of one removed note of a chord: the chord, with its other notes.
std::string expect_removed_note(Document& document, const Removal& removal) {
    if (find(document, removal.target)) return "the note is still there";
    if (!removal.at) return "the case did not record where the chord sat";
    EngravingItem* item = item_at(document, *removal.at);
    if (!item || !item->isChord()) return "the chord went with the note";
    return pitches_of(*item) == removal.remaining ? std::string{} : "the chord lost another note";
}

const bool registered = register_cases({
    enter_case("EnterNotes rest replaces a chord", "chordline_path.mscx", 0, 0, 0, {0, 1}, {1, 4}, {},
               [](Document& document, const SegmentAddress& at) { return expect_entered(document, at, {1, 4}, {}); }),
    enter_case("EnterNotes chord replaces a rest", "chordline_path.mscx", 0, 0, 0, {3, 4}, {1, 4},
               {Pitch{60}, Pitch{64}, Pitch{67}},
               [](Document& document, const SegmentAddress& at) { return expect_entered(document, at, {1, 4}, {60, 64, 67}); }),
    enter_case("EnterNotes chord in a bar of rests", "chordline_path.mscx", 1, 0, 0, {0, 1}, {1, 2}, {Pitch{62}},
               [](Document& document, const SegmentAddress& at) { return expect_entered(document, at, {1, 2}, {62}); }),
    enter_case("EnterNotes tied across a barline", "chordline_path.mscx", 0, 0, 0, {3, 4}, {1, 2}, {Pitch{65}},
               [](Document& document, const SegmentAddress& at) {
                   if (std::string problem = expect_entered(document, at, {1, 4}, {65}); !problem.empty()) return problem;
                   // The piece in the next bar is tied to the entry.
                   const Note& note = *mu::engraving::toChord(item_at(document, at))->notes().front();
                   if (!note.tieFor()) return std::string("the tie to the next bar is missing");
                   const Note* next = note.tieFor()->endNote();
                   if (!next || next->pitch() != 65) return std::string("the tie ends on another pitch");
                   return std::string{};
               }),
    enter_case("EnterNotes two-note chord", "hairpins_and_repeats.mscx", 1, 0, 0, {0, 1}, {1, 4}, {Pitch{60}, Pitch{63}},
               [](Document& document, const SegmentAddress& at) { return expect_entered(document, at, {1, 4}, {60, 63}); }),

    pitch_case("SetPitch plain", "chordline_path.mscx", 74, std::nullopt),
    pitch_case("SetPitch with a spelling", "chordline_path.mscx", 63, 11),
    pitch_case("SetPitch note of a chord", "layout_elements.mscx", 53, std::nullopt, is_pitch(52)),
    pitch_case("SetPitch linked part", "linked_parts.mscx", 74, std::nullopt),

    duration_case("SetDuration shorter", "chordline_path.mscx", "Chord", {1, 8}),
    duration_case("SetDuration longer", "chordline_path.mscx", "Chord", {1, 2}),
    duration_case("SetDuration of a rest", "chordline_path.mscx", "Rest", {1, 8}),
    duration_case("SetDuration longer in a hairpin score", "hairpins_and_repeats.mscx", "Chord", {1, 2}),

    remove_case("Remove a chord", "chordline_path.mscx", "Chord", {}, expect_removed_chord),
    remove_case("Remove a chord of a custom part", "custom_part_name.mscx", "Chord", {}, expect_removed_chord),
    remove_case("Remove a note of a chord", "layout_elements.mscx", "Note", in_chord_of(3), expect_removed_note),
    remove_case("Remove a dynamic", "hairpins_and_repeats.mscx", "Dynamic", {},
                [](Document& document, const Removal& removal) {
                    return find(document, removal.target) ? std::string("the dynamic is still there") : std::string{};
                }),
});

} // namespace
} // namespace engrave::harness
