// Evidence cases of InsertMeasures, RemoveMeasures, SetTimeSig, SetKeySig,
// InsertClef and SetLayoutBreak.
//
// The cases navigate the score through the engine's own measure and segment lists
// rather than through ids, because a time signature change rewrites (and so
// replaces) the measures of its range.  An id a case does keep is checked against
// the fingerprint, which is what a reopen has to reproduce.

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engrave/tests/harness/harness.h"

#include "engraving/dom/clef.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/layoutbreak.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/timesig.h"
#include "engraving/types/typesconv.h"

#include "engrave/src/api/handles.h"

namespace engrave::harness {
namespace {

using mu::engraving::ClefType;
using mu::engraving::EngravingItem;
using mu::engraving::Key;
using mu::engraving::KeySig;
using mu::engraving::LayoutBreak;
using mu::engraving::LayoutBreakType;
using mu::engraving::Measure;
using mu::engraving::MeasureBase;
using mu::engraving::Score;
using mu::engraving::Segment;
using mu::engraving::SegmentType;
using mu::engraving::Staff;
using mu::engraving::TConv;
using mu::engraving::TimeSig;
using mu::engraving::VOICES;

mu::engraving::Fraction engine_fraction(const Fraction& value) {
    return mu::engraving::Fraction(int(value.num), int(value.den));
}

Fraction fraction_of(const mu::engraving::Fraction& value) {
    return {value.numerator(), value.denominator()};
}

// The scores of the document, the master first and then the excerpts.
std::vector<Score*> scores_of(Document& document) {
    std::vector<Score*> scores;
    for (Score* score : master_of(document).scoreList()) scores.push_back(score);
    return scores;
}

std::vector<Measure*> measures_of(Score* score) {
    std::vector<Measure*> measures;
    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) measures.push_back(measure);
    return measures;
}

Measure* measure_at(Score* score, int index) {
    Measure* measure = score->firstMeasure();
    for (int i = 0; i < index && measure; ++i) measure = measure->nextMeasure();
    return measure;
}

ElementId id_of(const mu::engraving::EngravingObject& object) {
    if (!object.eid().isValid()) throw std::invalid_argument(std::format("a {} has no id", object.typeName()));
    return from_engine(object.eid());
}

std::size_t count_of(const Fingerprint& print, std::string_view type) {
    std::size_t count = 0;
    for (const std::string& row : print.ids) {
        const std::size_t space = row.find(' ');
        if (space == std::string::npos) continue;
        const std::size_t after = row.find(' ', space + 1);
        if (std::string_view(row).substr(space + 1, after - space - 1) == type) ++count;
    }
    return count;
}

bool has_id(const Fingerprint& print, const ElementId& id) {
    const std::string prefix = to_string(id) + " ";
    return std::ranges::any_of(print.ids, [&prefix](const std::string& row) { return row.starts_with(prefix); });
}

bool break_on(const MeasureBase& measure, LayoutBreakType kind) {
    switch (kind) {
    case LayoutBreakType::LINE: return measure.lineBreak();
    case LayoutBreakType::PAGE: return measure.pageBreak();
    case LayoutBreakType::SECTION: return measure.sectionBreak();
    case LayoutBreakType::NOBREAK: return measure.noBreak();
    }
    return false;
}

bool has_break_element(const MeasureBase& measure, LayoutBreakType kind) {
    for (const EngravingItem* item : measure.el()) {
        if (item->isLayoutBreak() && static_cast<const LayoutBreak*>(item)->layoutBreakType() == kind) return true;
    }
    return false;
}

// The clef every staff shows at a tick: the staff clef list, which the clef elements
// maintain and layout reads.
std::vector<std::vector<ClefType>> clefs_of(Document& document, const mu::engraving::Fraction& tick) {
    std::vector<std::vector<ClefType>> clefs;
    for (Score* score : scores_of(document)) {
        std::vector<ClefType> row;
        for (std::size_t staff = 0; staff < score->nstaves(); ++staff) row.push_back(score->staff(staff)->clef(tick));
        clefs.push_back(std::move(row));
    }
    return clefs;
}

// The key every staff shows at a tick, from the key signature element there.
std::vector<std::vector<Key>> keys_of(Document& document, const mu::engraving::Fraction& tick) {
    std::vector<std::vector<Key>> keys;
    for (Score* score : scores_of(document)) {
        std::vector<Key> row;
        const Measure* measure = score->tick2measure(tick);
        const Segment* segment = measure ? measure->findSegment(SegmentType::KeySig, tick) : nullptr;
        for (std::size_t staff = 0; staff < score->nstaves(); ++staff) {
            const EngravingItem* item = segment ? segment->element(staff * VOICES) : nullptr;
            row.push_back(item && item->isKeySig() ? static_cast<const KeySig*>(item)->concertKey() : Key::INVALID);
        }
        keys.push_back(std::move(row));
    }
    return keys;
}

// --- InsertMeasures ----------------------------------------------------------

// `count` measures before `at`, in the master and in every excerpt; the measure that
// was at `at` is still there, `count` places further down.
std::string expect_inserted(Document& document, const Fingerprint& before, const Fingerprint& after,
                            const std::vector<ElementId>& was, int at, int count) {
    const std::vector<Score*> scores = scores_of(document);
    if (count_of(after, "Measure") != count_of(before, "Measure") + std::size_t(count) * scores.size()) {
        return std::format("the score does not have {} more measures per part", count);
    }
    for (std::size_t s = 0; s < scores.size(); ++s) {
        const Measure* moved = measure_at(scores[s], at + count);
        if (!moved || id_of(*moved) != was[s]) return std::format("part {} lost the measure it had at {}", s, at);
        for (int i = 0; i < count; ++i) {
            const Measure* made = measure_at(scores[s], at + i);
            if (!made) return std::format("part {} has no measure at {}", s, at + i);
            if (made->timesig() != moved->timesig()) return "an inserted measure took another time signature";
            if (has_id(before, id_of(*made))) return "an inserted measure holds an id that was taken already";
            if (!has_id(after, id_of(*made))) return "an inserted measure was given no id";
        }
    }
    return {};
}

OpCase insert_case(std::string name, std::string fixture, int at, int count) {
    struct Shared {
        std::vector<ElementId> was;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, at, count](Document& document) {
            shared->was.clear();
            for (Score* score : scores_of(document)) {
                Measure* target = measure_at(score, at);
                if (!target) throw std::invalid_argument(std::format("the fixture has no measure {}", at));
                shared->was.push_back(id_of(*target));
            }
            return Batch{{InsertMeasures{.before = Ref{shared->was.front()}, .count = count}}};
        },
        .expect = [shared, at, count](Document& document, const Fingerprint& before, const Fingerprint& after) {
            return expect_inserted(document, before, after, shared->was, at, count);
        },
    };
}

OpCase append_frame_case() {
    return {
        .name = "InsertMeasures a frame at the end",
        .fixture = "repeat_with_2_voltas.mscx",
        .make = [](Document&) { return Batch{{InsertMeasures{.before = std::nullopt, .count = 1, .kind = "VBox"}}}; },
        .expect = [](Document& document, const Fingerprint& before, const Fingerprint& after) -> std::string {
            if (count_of(after, "VBox") != count_of(before, "VBox") + 1) return "the frame count is not one higher";
            MeasureBase* last = master_of(document).last();
            if (!last || !last->isVBox()) return "the master does not end with the frame";
            if (!has_id(after, id_of(*last))) return "the new frame has no id";
            return {};
        },
    };
}

// --- RemoveMeasures ----------------------------------------------------------

// The measures of [from, from + count) are gone from every part, and every other
// measure is the one it was.
OpCase remove_case(std::string name, std::string fixture, int from, int count) {
    struct Shared {
        std::vector<std::vector<ElementId>> removed;
        std::vector<std::vector<ElementId>> left;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, from, count](Document& document) {
            shared->removed.clear();
            shared->left.clear();
            for (Score* score : scores_of(document)) {
                const std::vector<Measure*> measures = measures_of(score);
                if (from + count > int(measures.size())) throw std::invalid_argument("the fixture is too short");
                std::vector<ElementId> removed;
                std::vector<ElementId> left;
                for (int i = 0; i < int(measures.size()); ++i) {
                    const ElementId id = id_of(*measures[std::size_t(i)]);
                    (i < from || i >= from + count ? left : removed).push_back(id);
                }
                shared->removed.push_back(std::move(removed));
                shared->left.push_back(std::move(left));
            }
            const std::vector<ElementId>& master = shared->removed.front();
            return Batch{{RemoveMeasures{master.front(), master.back()}}};
        },
        .expect = [shared, count](Document& document, const Fingerprint& before, const Fingerprint& after) -> std::string {
            const std::vector<Score*> scores = scores_of(document);
            if (count_of(before, "Measure") != count_of(after, "Measure") + std::size_t(count) * scores.size()) {
                return "the measure count did not drop by the removed range";
            }
            for (std::size_t s = 0; s < scores.size(); ++s) {
                std::vector<ElementId> now;
                for (Measure* measure : measures_of(scores[s])) now.push_back(id_of(*measure));
                if (now != shared->left[s]) return std::format("part {} does not keep the measures it had", s);
                for (const ElementId& id : shared->removed[s]) {
                    if (has_id(after, id)) return "a removed measure still has its id";
                }
            }
            return {};
        },
    };
}

// --- SetTimeSig --------------------------------------------------------------

// The signature holds from the measure at `tick` to the end of the score, in every
// part, and the measure starts with a signature element that says so.
std::string expect_time_sig(Document& document, const mu::engraving::Fraction& tick, const Fraction& sig) {
    const mu::engraving::Fraction wanted = engine_fraction(sig);
    for (Score* score : scores_of(document)) {
        Measure* measure = score->tick2measure(tick);
        if (!measure) return "the measure at the tick is gone";
        if (measure->timesig() != wanted) return "the measure kept its time signature";
        if (measure->ticks() != wanted) return "the measure kept its length";
        const Segment* segment = measure->findSegment(SegmentType::TimeSig, measure->tick());
        const EngravingItem* item = segment ? segment->element(0) : nullptr;
        if (!item || !item->isTimeSig()) return "the measure starts without a time signature";
        if (static_cast<const TimeSig*>(item)->sig() != wanted) return "the time signature element kept its value";
        for (Measure* each = measure; each; each = each->nextMeasure()) {
            if (each->timesig() != wanted) return "a measure after it kept its time signature";
        }
    }
    return {};
}

OpCase time_sig_case(std::string name, std::string fixture, int at, Fraction sig) {
    struct Shared {
        mu::engraving::Fraction tick;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, at, sig](Document& document) {
            Measure* target = measure_at(&master_of(document), at);
            if (!target) throw std::invalid_argument(std::format("the fixture has no measure {}", at));
            shared->tick = target->tick();
            return Batch{{SetTimeSig{.measure = id_of(*target), .sig = sig}}};
        },
        .expect = [shared, sig](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect_time_sig(document, shared->tick, sig);
        },
    };
}

// --- SetKeySig ---------------------------------------------------------------

// The staves at `staff` (every staff when unset) carry `key` at the measure, and the
// others keep the key they had.  A key change of one staff reaches every staff the
// engine links to it, which for this part excerpt is the whole part.
std::string expect_key(Document& document, const mu::engraving::Fraction& tick, int staff, int key,
                       const std::vector<std::vector<Key>>& was) {
    const std::vector<std::vector<Key>> now = keys_of(document, tick);
    const std::vector<Staff*> group = staff < 0 ? std::vector<Staff*>{}
                                                : master_of(document).staff(mu::engraving::staff_idx_t(staff))->staffList();
    const std::vector<Score*> scores = scores_of(document);
    for (std::size_t s = 0; s < now.size(); ++s) {
        for (std::size_t i = 0; i < now[s].size(); ++i) {
            const bool changes = staff < 0 || std::ranges::find(group, scores[s]->staff(i)) != group.end();
            const Key wanted = changes ? static_cast<Key>(key) : was[s][i];
            if (now[s][i] != wanted) return std::format("part {} staff {} has key {}", s, i, int(now[s][i]));
        }
    }
    return {};
}

OpCase key_case(std::string name, std::string fixture, int at, int key, std::optional<int> staff) {
    struct Shared {
        mu::engraving::Fraction tick;
        std::vector<std::vector<Key>> was;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, at, key, staff](Document& document) {
            Measure* target = measure_at(&master_of(document), at);
            if (!target) throw std::invalid_argument(std::format("the fixture has no measure {}", at));
            shared->tick = target->tick();
            shared->was = keys_of(document, shared->tick);
            std::optional<Ref> which;
            if (staff) {
                if (std::size_t(*staff) >= master_of(document).nstaves()) throw std::invalid_argument("no such staff");
                which = id_of(*master_of(document).staff(mu::engraving::staff_idx_t(*staff)));
            }
            return Batch{{SetKeySig{.measure = id_of(*target), .key = key, .staff = which}}};
        },
        .expect = [shared, key, staff](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect_key(document, shared->tick, staff ? *staff : -1, key, shared->was);
        },
    };
}

// --- InsertClef --------------------------------------------------------------

// The clef of the staff at the address changes, and so does the clef of every staff
// the engine links to it; the other staves keep theirs.
std::string expect_clef(Document& document, const mu::engraving::Fraction& tick, int staff, ClefType clef,
                        const std::vector<std::vector<ClefType>>& was) {
    const std::vector<std::vector<ClefType>> now = clefs_of(document, tick);
    const std::vector<Staff*> group = master_of(document).staff(mu::engraving::staff_idx_t(staff))->staffList();
    const std::vector<Score*> scores = scores_of(document);
    for (std::size_t s = 0; s < now.size(); ++s) {
        for (std::size_t i = 0; i < now[s].size(); ++i) {
            const ClefType wanted = std::ranges::find(group, scores[s]->staff(i)) != group.end() ? clef : was[s][i];
            if (now[s][i] != wanted) {
                return std::format("part {} staff {} has clef {} instead of {}", s, i, int(now[s][i]), int(wanted));
            }
        }
    }
    return {};
}

// The `segment`-th ChordRest segment of the `at`-th measure, on `staff`.
OpCase clef_case(std::string name, std::string fixture, int at, int segment, int staff, ClefType clef) {
    struct Shared {
        mu::engraving::Fraction tick;
        std::vector<std::vector<ClefType>> was;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, at, segment, staff, clef](Document& document) {
            Score& master = master_of(document);
            Measure* measure = measure_at(&master, at);
            if (!measure) throw std::invalid_argument(std::format("the fixture has no measure {}", at));
            Segment* where = measure->first(SegmentType::ChordRest);
            for (int i = 0; i < segment && where; ++i) where = where->next(SegmentType::ChordRest);
            if (!where) throw std::invalid_argument("the measure has no such segment");
            shared->tick = where->tick();
            shared->was = clefs_of(document, shared->tick);
            const SegmentAddress address{.measure = id_of(*measure),
                                         .staff = staff,
                                         .voice = 0,
                                         .tick = fraction_of(where->tick()),
                                         .kind = "ChordRest"};
            return Batch{{InsertClef{.at = address, .clef = TConv::toXml(clef).ascii()}}};
        },
        .expect = [shared, staff, clef](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect_clef(document, shared->tick, staff, clef, shared->was);
        },
    };
}

// --- SetLayoutBreak ----------------------------------------------------------

// The measure of the master has the break, on or off, with its element; the linked
// measure of a part keeps what it had, because the engine does not carry a break
// across to a part (each part sets its own).
std::string expect_break(Document& document, int at, LayoutBreakType kind, bool on, const std::vector<bool>& was) {
    const std::vector<Score*> scores = scores_of(document);
    for (std::size_t s = 0; s < scores.size(); ++s) {
        const Measure* measure = measure_at(scores[s], at);
        if (!measure) return "the measure is gone";
        const bool wanted = s == 0 ? on : was[s];
        if (break_on(*measure, kind) != wanted) {
            return std::format("part {} measure {} has its break {}", s, at, wanted ? "off" : "on");
        }
        if (has_break_element(*measure, kind) != wanted) {
            return std::format("part {}: the break element does not match the flag", s);
        }
        if (wanted && kind == LayoutBreakType::PAGE && measure->lineBreak()) return "a line break survived a page break";
    }
    return {};
}

OpCase break_case(std::string name, std::string fixture, int at, LayoutBreakType kind, bool on) {
    struct Shared {
        std::vector<bool> was;
    };
    auto shared = std::make_shared<Shared>();
    return {
        .name = std::move(name),
        .fixture = std::move(fixture),
        .make = [shared, at, kind, on](Document& document) {
            shared->was.clear();
            for (Score* score : scores_of(document)) {
                const Measure* measure = measure_at(score, at);
                if (!measure) throw std::invalid_argument(std::format("the fixture has no measure {}", at));
                shared->was.push_back(break_on(*measure, kind));
            }
            Measure* target = measure_at(&master_of(document), at);
            return Batch{{SetLayoutBreak{.measure = id_of(*target), .kind = TConv::toXml(kind).ascii(), .on = on}}};
        },
        .expect = [shared, at, kind, on](Document& document, const Fingerprint&, const Fingerprint&) {
            return expect_break(document, at, kind, on, shared->was);
        },
    };
}

const bool registered = register_cases({
    insert_case("InsertMeasures a measure before the second", "custom_part_name.mscx", 1, 1),
    insert_case("InsertMeasures two measures before the fourth", "pedal.mscx", 3, 2),
    append_frame_case(),

    remove_case("RemoveMeasures the second of three", "custom_part_name.mscx", 1, 1),
    remove_case("RemoveMeasures the last measure in linked parts", "linked_parts.mscx", 1, 1),
    remove_case("RemoveMeasures the third and fourth", "repeat_with_2_voltas.mscx", 2, 2),

    time_sig_case("SetTimeSig 3/4 from the second measure", "hairpins_and_repeats.mscx", 1, Fraction{3, 4}),
    time_sig_case("SetTimeSig 3/4 over three measures", "custom_part_name.mscx", 0, Fraction{3, 4}),
    time_sig_case("SetTimeSig 6/8 from the fifth measure", "pedal.mscx", 4, Fraction{6, 8}),

    key_case("SetKeySig three sharps on every staff", "linked_parts.mscx", 0, 3, std::nullopt),
    key_case("SetKeySig two flats on one staff", "custom_part_name.mscx", 1, -2, 0),
    key_case("SetKeySig four flats at the third measure", "repeat_with_2_voltas.mscx", 2, -4, std::nullopt),

    clef_case("InsertClef a bass clef before a chord", "custom_part_name.mscx", 1, 0, 0, ClefType::F),
    clef_case("InsertClef a treble clef on the lower staff", "pedal.mscx", 2, 0, 1, ClefType::G),
    clef_case("InsertClef a tenor clef mid-measure", "custom_part_name.mscx", 0, 1, 0, ClefType::C3),

    break_case("SetLayoutBreak a page break", "custom_part_name.mscx", 1, LayoutBreakType::PAGE, true),
    break_case("SetLayoutBreak a line break off", "layout_elements.mscx", 5, LayoutBreakType::LINE, false),
    break_case("SetLayoutBreak a section break", "repeat_with_2_voltas.mscx", 2, LayoutBreakType::SECTION, true),
});

} // namespace
} // namespace engrave::harness
