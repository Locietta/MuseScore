// The read API: outline(), query() and get().  They answer from the live document
// through the handle walk (handles.h), the property catalogue (pid_support.h) and
// the value codec, and take the music the engine itself derives - the metadata, the
// staves, the measures, the signatures - from the A2 score code in
// engrave/src/score/, so that a read says what the host's own tools said.

#include <engrave/document.h>
#include <engrave/query.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "engrave/src/api/handles.h"
#include "engrave/src/api/pid_support.h"
#include "engrave/src/api/state.h"
#include "engrave/src/api/value_codec.h"
#include "engrave/src/score/score_metadata.h"

#include "engraving/engravingproject.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/linkedobjects.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/page.h"
#include "engraving/dom/part.h"
#include "engraving/dom/property.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftype.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/textbase.h"
#include "engraving/dom/timesig.h"
#include "engraving/types/constants.h"
#include "engraving/types/typesconv.h"

namespace engrave {
namespace {

using mu::engraving::ChordRest;
using mu::engraving::EngravingItem;
using mu::engraving::EngravingObject;
using mu::engraving::MasterScore;
using mu::engraving::Measure;
using mu::engraving::MeasureBase;
using mu::engraving::Page;
using mu::engraving::Part;
using mu::engraving::Pid;
using mu::engraving::Score;
using mu::engraving::Segment;
using mu::engraving::Spanner;
using mu::engraving::Staff;
using mu::engraving::StaffGroup;
using mu::engraving::StaffType;

std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

// Ticks per whole note: the engine divides a quarter note into Constants::DIVISION.
constexpr std::int64_t whole_note_ticks = 4 * mu::engraving::Constants::DIVISION;

// A position or length in ticks, as a fraction of a whole note.  Not reduced, so
// that a tick is the same fraction wherever it is reported.
Fraction whole_notes(int ticks) {
    return {ticks, whole_note_ticks};
}

// Whether a is before b.  Cross-multiplied, because a caller's fraction is not
// reduced either.
bool before(const Fraction& a, const Fraction& b) {
    return a.num * b.den < b.num * a.den;
}

Fraction fraction_of(const mu::engraving::Fraction& value) {
    return {value.numerator(), value.denominator()};
}

std::string plain_text(const muse::String& text) {
    return text.toStdString();
}

// --- properties -----------------------------------------------------------

PropertyRead read_property(const EngravingObject& object, Pid pid) {
    const mu::engraving::P_TYPE type = mu::engraving::propertyType(pid);
    PropertyRead read;
    read.name = api::name_of(pid);
    read.value = api::to_value(type, object.getProperty(pid));
    const mu::engraving::PropertyValue fallback = object.propertyDefault(pid);
    read.is_default = fallback.isValid() && read.value == api::to_value(type, fallback);
    read.styled = object.isStyled(pid);
    return read;
}

// The Pids a list of catalogue names names.  NotFound for a name the catalogue does
// not know, before anything is read.
Result<std::vector<Pid>> named_pids(const std::vector<std::string>& names) {
    std::vector<Pid> pids;
    pids.reserve(names.size());
    for (const std::string& name : names) {
        const Pid pid = api::pid_of(name);
        if (pid == Pid::END) return fail(ErrorCode::NotFound, std::format("no property '{}'", name));
        pids.push_back(pid);
    }
    return pids;
}

bool has_property(const std::vector<Pid>& pids, Pid pid) {
    return std::ranges::find(pids, pid) != pids.end();
}

// --- where an element is --------------------------------------------------

int score_index(MasterScore& master, const Score* score) {
    int index = 0;
    for (const Score* candidate : master.scoreList()) {
        if (candidate == score) return index;
        ++index;
    }
    return -1;
}

// The measures of one score in score order, with the start tick of each, so that the
// measure an element sits in is one search rather than a walk per element.
struct MeasureIndex {
    std::vector<Measure*> measures;
    std::vector<int> ticks;

    explicit MeasureIndex(Score& score) {
        for (Measure* measure = score.firstMeasure(); measure; measure = measure->nextMeasure()) {
            measures.push_back(measure);
            ticks.push_back(measure->tick().ticks());
        }
    }

    std::int32_t index_of_tick(int tick) const {
        const auto found = std::upper_bound(ticks.begin(), ticks.end(), tick);
        if (found == ticks.begin()) return -1;
        return static_cast<std::int32_t>(found - ticks.begin() - 1);
    }

    std::int32_t index_of(const Measure* measure) const {
        const auto found = std::ranges::find(measures, measure);
        return found == measures.end() ? -1 : static_cast<std::int32_t>(found - measures.begin());
    }
};

int duration_ticks(const EngravingItem& item) {
    if (item.isChordRest()) {
        const auto& rest = static_cast<const ChordRest&>(item);
        // A grace note has no length of its own in this model.
        if (rest.isChord() && mu::engraving::toChord(&rest)->isGrace()) return 0;
        return rest.actualTicks().ticks();
    }
    if (item.isSpanner()) return static_cast<const Spanner&>(item).ticks().ticks();
    return 0;
}

// The position of an element and the measure it sits in.
struct Where {
    Fraction tick;
    Fraction duration;
    std::int32_t measure_index = -1;
    std::optional<ElementId> measure;
};

Where where_of(const EngravingObject& object, const MeasureIndex& index) {
    Where where;
    if (object.isMeasure()) {
        const Measure* measure = mu::engraving::toMeasure(&object);
        where.tick = whole_notes(measure->tick().ticks());
        where.duration = whole_notes(measure->ticks().ticks());
        where.measure_index = index.index_of(measure);
    } else if (object.isEngravingItem()) {
        const auto& item = static_cast<const EngravingItem&>(object);
        where.tick = whole_notes(item.tick().ticks());
        where.duration = whole_notes(duration_ticks(item));
        where.measure_index = index.index_of_tick(item.tick().ticks());
    } else {
        return where; // the score, a part or a staff: not at a position
    }
    if (where.measure_index >= 0) {
        const Measure* measure = index.measures[static_cast<std::size_t>(where.measure_index)];
        if (measure->eid().isValid()) where.measure = from_engine(measure->eid());
    }
    return where;
}
// The staff and voice an element sits on; both -1 for an element that is not on one.
std::pair<std::int32_t, std::int32_t> staff_and_voice(const EngravingObject& object) {
    if (!object.isEngravingItem()) return {-1, -1};
    const auto track = static_cast<const EngravingItem&>(object).track();
    if (track == muse::nidx) return {-1, -1};
    return {static_cast<std::int32_t>(track / mu::engraving::VOICES),
            static_cast<std::int32_t>(track % mu::engraving::VOICES)};
}

// The nearest element above this one that has an id.  The score itself is not one:
// it is the document, not the element's parent.
std::optional<ElementId> parent_of(const EngravingObject& object) {
    for (EngravingObject* parent = object.ownershipParent(); parent; parent = parent->ownershipParent()) {
        if (parent->type() == mu::engraving::ElementType::SCORE) return std::nullopt;
        if (parent->eid().isValid()) return from_engine(parent->eid());
    }
    return std::nullopt;
}

// The ids of an element's link group, master first.  Empty when it has no links.
std::vector<ElementId> links_of(const EngravingObject& object, MasterScore& master) {
    std::vector<ElementId> links;
    const mu::engraving::LinkedObjects* group = object.links();
    if (!group) return links;
    std::vector<std::pair<int, ElementId>> ordered;
    for (const EngravingObject* member : *group) {
        if (!member || !member->eid().isValid()) continue;
        ordered.emplace_back(score_index(master, member->score()), from_engine(member->eid()));
    }
    std::ranges::sort(ordered);
    links.reserve(ordered.size());
    for (const auto& [index, id] : ordered) links.push_back(id);
    return links;
}

std::optional<LayoutBox> box_of(const EngravingItem& item, const Score& score) {
    const MeasureBase* base = const_cast<EngravingItem&>(item).findMeasureBase();
    const Page* page = base ? base->page() : nullptr;
    if (!page) return std::nullopt;
    const mu::engraving::page_idx_t index = score.pageIdx(page);
    if (index == muse::nidx) return std::nullopt;
    const mu::engraving::RectF box = item.pageBoundingRect(mu::engraving::LD_ACCESS::MAYBE_NOTINITED);
    return LayoutBox{static_cast<std::int32_t>(index), box.x(), box.y(), box.width(), box.height()};
}

// --- the outline ----------------------------------------------------------

std::string_view staff_group(StaffGroup group) {
    switch (group) {
    case StaffGroup::STANDARD: return "pitched";
    case StaffGroup::PERCUSSION: return "percussion";
    case StaffGroup::TAB: return "tablature";
    }
    return "unknown";
}

// A part's index in its score's part list, which is what StaffInfo::part means.
std::int32_t part_index(const Score& score, const Part* part) {
    const std::vector<Part*>& parts = score.parts();
    const auto found = std::ranges::find(parts, part);
    return found == parts.end() ? -1 : static_cast<std::int32_t>(found - parts.begin());
}

void fill_parts(Outline& outline, Score& score) {
    for (Part* part : score.parts()) {
        PartInfo info;
        if (part->eid().isValid()) info.id = from_engine(part->eid());
        info.name = plain_text(part->longName());
        info.instrument_id = part->instrumentId().toStdString();
        for (Staff* staff : part->staves()) {
            StaffInfo row;
            if (staff->eid().isValid()) row.id = from_engine(staff->eid());
            row.index = static_cast<std::int32_t>(staff->idx());
            row.part = part_index(score, part);
            const StaffType* type = staff->staffType(mu::engraving::Fraction(0, 1));
            if (type) {
                row.group = staff_group(type->group());
                row.lines = type->lines();
                row.invisible = type->invisible();
            }
            info.staves.push_back(std::move(row));
        }
        outline.parts.push_back(std::move(info));
    }
}

void fill_measures(Outline& outline, Score& score) {
    for (Measure* measure = score.firstMeasure(); measure; measure = measure->nextMeasure()) {
        MeasureInfo info;
        if (measure->eid().isValid()) info.id = from_engine(measure->eid());
        info.index = static_cast<std::int32_t>(outline.measures.size());
        info.tick = whole_notes(measure->tick().ticks());
        info.duration = whole_notes(measure->ticks().ticks());
        outline.measures.push_back(std::move(info));
    }
}

// The time signatures, key signatures and tempo markings of a score, in the order a
// score shows them: by tick, then staff, then type.
void fill_signatures(Outline& outline, Score& score) {
    for (Measure* measure = score.firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            for (EngravingItem* item : segment->elist()) {
                // A generated signature is layout's (the clef of a header, a courtesy
                // signature), not one the score holds.
                if (!item || item->generated()) continue;
                if (!item->isTimeSig() && !item->isKeySig()) continue;
                SignatureInfo info;
                info.tick = whole_notes(item->tick().ticks());
                info.staff = staff_and_voice(*item).first;
                if (item->isTimeSig()) {
                    info.type = "TimeSig";
                    info.timesig = fraction_of(mu::engraving::toTimeSig(item)->sig());
                } else {
                    info.type = "KeySig";
                    info.key = static_cast<std::int32_t>(mu::engraving::toKeySig(item)->concertKey());
                }
                outline.signatures.push_back(std::move(info));
            }
            for (EngravingItem* item : segment->annotations()) {
                if (!item || item->generated() || !item->isTempoText()) continue;
                const auto* tempo = mu::engraving::toTempoText(item);
                SignatureInfo info;
                info.type = "Tempo";
                info.tick = whole_notes(item->tick().ticks());
                info.bpm = std::round(tempo->tempo().val * 60.0 * 1e4) / 1e4;
                info.text = plain_text(tempo->plainText());
                outline.signatures.push_back(std::move(info));
            }
        }
    }
    std::ranges::sort(outline.signatures, [](const SignatureInfo& a, const SignatureInfo& b) {
        if (before(a.tick, b.tick)) return true;
        if (before(b.tick, a.tick)) return false;
        return std::tie(a.staff, a.type) < std::tie(b.staff, b.type);
    });
}

void fill_excerpts(Outline& outline, MasterScore& master) {
    for (const mu::engraving::Excerpt* excerpt : master.excerpts()) {
        Score* score = excerpt ? excerpt->excerptScore() : nullptr;
        if (!score) continue;
        ExcerptInfo info;
        if (score->eid().isValid()) info.id = from_engine(score->eid());
        info.name = plain_text(excerpt->name());
        info.pages = static_cast<std::int32_t>(score->npages());
        outline.excerpts.push_back(std::move(info));
    }
}

// --- the query ------------------------------------------------------------

// The measure window a query asks for, as a half-open index range.
struct MeasureWindow {
    std::int32_t first = 0;
    std::int32_t last = 0; // one past the last measure
    bool given = false;
};

Result<MeasureWindow> measure_window(const Query& request, Score& score, MasterScore& master) {
    const std::size_t count = score.nmeasures();
    const auto last_index = static_cast<std::int32_t>(count);
    MeasureWindow window;
    if ((request.first_measure || request.measure_count) && (request.from_measure || request.to_measure)) {
        return fail(ErrorCode::InvalidArgument, "a query names its measure window by index or by id, not both");
    }
    if (!request.first_measure && !request.measure_count && !request.from_measure && !request.to_measure) {
        return window; // the whole score
    }
    const auto index_of = [&](const ElementId& id, std::string_view what) -> Result<std::int32_t> {
        const Result<Resolved> found = resolve(master, id);
        if (!found) return std::unexpected(found.error());
        if (found->object->type() != mu::engraving::ElementType::MEASURE) {
            return fail(ErrorCode::InvalidArgument, std::format("{} is not a measure", what));
        }
        const MeasureIndex index(score);
        const auto position = index.index_of(mu::engraving::toMeasure(found->object));
        if (position < 0) return fail(ErrorCode::InvalidArgument, std::format("{} is not a measure of the score", what));
        return position;
    };
    if (request.from_measure) {
        const Result<std::int32_t> first = index_of(*request.from_measure, "from_measure");
        if (!first) return std::unexpected(first.error());
        window.first = *first;
    } else {
        window.first = request.first_measure.value_or(0);
    }
    if (request.to_measure) {
        const Result<std::int32_t> last = index_of(*request.to_measure, "to_measure");
        if (!last) return std::unexpected(last.error());
        window.last = *last + 1;
    } else if (request.measure_count) {
        window.last = window.first + *request.measure_count;
    } else {
        window.last = last_index;
    }
    window.given = true;
    if (window.first < 0 || window.last < window.first || window.last > last_index) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the measure window [{}, {}) is not inside the score's {} measure(s)", window.first,
                                window.last, count));
    }
    if (static_cast<std::size_t>(window.last - window.first) > max_query_measures) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("a measure window is at most {} measures wide", max_query_measures));
    }
    return window;
}

bool contains(const std::vector<std::int32_t>& values, std::int32_t value) {
    return std::ranges::find(values, value) != values.end();
}

// The sort key of a result: score, measure, tick, staff, voice, type, id.  Every field
// is a value of the element itself, so the order survives paging and repeated calls.
struct Order {
    int score = 0;
    std::int32_t measure = -1;
    Fraction tick;
    std::int32_t staff = -1;
    std::int32_t voice = -1;
    std::string type;
    ElementId id;

    auto operator<=>(const Order&) const = default;
};

Order order_of(const ElementInfo& element, int score) {
    return Order{score, element.measure_index, element.tick, element.staff, element.voice, element.type, element.id};
}

} // namespace

Result<Outline> Document::outline(const ScoreSel& selection) {
    impl_->engine->check_thread();
    MasterScore& master = *impl_->project->masterScore();
    const Result<Score*> selected = select_score(master, selection);
    if (!selected) return std::unexpected(selected.error());
    Score& score = **selected;

    Outline outline;
    // The metadata is the engine's own derivation (title recognition, the tempo and
    // time signature summary, the page and measure counts); only the track list is
    // left out, because it is the audio engine's.
    const nlohmann::json metadata = music::score_metadata(&score, std::vector<music::ScoreMetadataTrack>{});
    const auto text = [&metadata](const char* key) { return metadata.at(key).get<std::string>(); };
    outline.title = text("title");
    outline.subtitle = text("subtitle");
    outline.composer = text("composer");
    outline.poet = text("poet");
    outline.timesig = text("timesig");
    outline.mscore_version = text("mscoreVersion");
    outline.file_version = metadata.at("fileVersion").get<std::int32_t>();
    outline.pages = metadata.at("pages").get<std::int32_t>();
    outline.measure_count = metadata.at("measures").get<std::int32_t>();
    outline.key = metadata.at("keysig").get<std::int32_t>();
    outline.duration = metadata.at("duration").get<double>();

    fill_parts(outline, score);
    fill_measures(outline, score);
    fill_signatures(outline, score);
    fill_excerpts(outline, master);
    return outline;
}

Result<QueryResult> Document::query(const Query& request) {
    impl_->engine->check_thread();
    MasterScore& master = *impl_->project->masterScore();
    const Result<Score*> selected = select_score(master, request.score);
    if (!selected) return std::unexpected(selected.error());
    Score& score = **selected;

    if (request.limit == 0) return fail(ErrorCode::InvalidArgument, "a query limit is at least 1");
    if (request.limit > max_query_results) {
        return fail(ErrorCode::ResourceLimit, std::format("a query reports at most {} elements at a time", max_query_results));
    }
    for (const std::string& type : request.types) {
        if (!api::has_type(type)) return fail(ErrorCode::InvalidArgument, std::format("no element type '{}'", type));
    }
    const Result<std::vector<Pid>> props = named_pids(request.props);
    if (!props) return std::unexpected(props.error());
    const auto nstaves = static_cast<std::int32_t>(score.nstaves());
    for (const std::int32_t staff : request.staves) {
        if (staff < 0 || staff >= nstaves) {
            return fail(ErrorCode::InvalidArgument, std::format("staff {} is not one of the score's {}", staff, nstaves));
        }
    }
    for (const std::int32_t voice : request.voices) {
        if (voice < 0 || voice >= static_cast<std::int32_t>(mu::engraving::VOICES)) {
            return fail(ErrorCode::InvalidArgument, std::format("voice {} is not one of the score's 0..{}", voice,
                                                                mu::engraving::VOICES - 1));
        }
    }
    if (request.from_tick && request.to_tick && before(*request.to_tick, *request.from_tick)) {
        return fail(ErrorCode::InvalidArgument, "a tick range ends before it starts");
    }
    const Result<MeasureWindow> window = measure_window(request, score, master);
    if (!window) return std::unexpected(window.error());

    const int index = score_index(master, &score);
    const MeasureIndex measures(score);
    const ScoreSel selection = selection_of(score);
    // The properties of each type the query meets, so that a property an element does
    // not have is left out rather than read.
    std::unordered_map<std::string, std::vector<Pid>> type_props;

    std::vector<std::pair<Order, ElementInfo>> rows;
    for (const LiveId& entry : live_ids(master)) {
        EngravingObject& object = *entry.object;
        if (entry.placement.score != index) continue;
        const std::string type = object.typeName();
        if (!request.types.empty() && std::ranges::find(request.types, type) == request.types.end()) continue;
        const auto [staff, voice] = staff_and_voice(object);
        if (!request.staves.empty() && !contains(request.staves, staff)) continue;
        if (!request.voices.empty() && !contains(request.voices, voice)) continue;
        const Where where = where_of(object, measures);
        if (window->given && (where.measure_index < window->first || where.measure_index >= window->last)) continue;
        if (request.from_tick && before(where.tick, *request.from_tick)) continue;
        if (request.to_tick && !before(where.tick, *request.to_tick)) continue;

        ElementInfo info;
        info.id = entry.id;
        info.type = type;
        info.score = selection;
        info.measure = where.measure;
        info.measure_index = where.measure_index;
        info.staff = staff;
        info.voice = voice;
        info.tick = where.tick;
        info.duration = where.duration;
        info.parent = parent_of(object);
        info.links = links_of(object, master);
        if (object.isEngravingItem()) {
            const auto& item = static_cast<const EngravingItem&>(object);
            if (request.layout_boxes) info.box = box_of(item, score);
        }
        if (!props->empty()) {
            auto& known = type_props[type];
            if (known.empty()) known = api::pids_of(type);
            for (const Pid pid : *props) {
                if (has_property(known, pid)) info.props.push_back(read_property(object, pid));
            }
        }
        rows.emplace_back(order_of(info, index), std::move(info));
    }

    if (rows.size() > max_query_results) {
        return fail(ErrorCode::ResourceLimit,
                    std::format("the query matches {} elements; one query reports at most {}", rows.size(),
                                max_query_results));
    }
    std::ranges::sort(rows, {}, &std::pair<Order, ElementInfo>::first);
    if (request.cursor > rows.size()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the cursor {} is past the {} element(s) the query matches", request.cursor, rows.size()));
    }

    QueryResult result;
    result.total = rows.size();
    const std::size_t end = std::min(rows.size(), request.cursor + request.limit);
    result.elements.reserve(end - request.cursor);
    for (std::size_t i = request.cursor; i < end; ++i) result.elements.push_back(std::move(rows[i].second));
    if (end < rows.size()) result.next_cursor = end;
    return result;
}

Result<std::vector<PropertyRead>> Document::get(const Ref& target, const std::vector<std::string>& props) {
    impl_->engine->check_thread();
    MasterScore& master = *impl_->project->masterScore();
    const Result<Resolved> resolved = std::visit(
        [&master](const auto& value) -> Result<Resolved> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, OpRef>) {
                return fail(ErrorCode::InvalidArgument, "an OpRef names an element of a batch, not of the document");
            } else {
                return resolve(master, value);
            }
        },
        target);
    if (!resolved) return std::unexpected(resolved.error());
    const EngravingObject& object = *resolved->object;

    const std::vector<Pid> known = api::pids_of(object.typeName());
    if (props.empty()) {
        std::vector<PropertyRead> reads;
        reads.reserve(known.size());
        for (const Pid pid : known) reads.push_back(read_property(object, pid));
        return reads;
    }

    const Result<std::vector<Pid>> named = named_pids(props);
    if (!named) return std::unexpected(named.error());
    for (std::size_t i = 0; i < named->size(); ++i) {
        if (!has_property(known, (*named)[i])) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("a {} has no property '{}'", object.typeName(), props[i]));
        }
    }
    std::vector<PropertyRead> reads;
    reads.reserve(named->size());
    for (const Pid pid : *named) reads.push_back(read_property(object, pid));
    return reads;
}

} // namespace engrave
