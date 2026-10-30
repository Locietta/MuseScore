#include "engrave/src/api/handles.h"

#include <algorithm>
#include <format>
#include <map>
#include <string_view>
#include <unordered_set>
#include <variant>

#include "engraving/dom/engravingitem.h"
#include "engraving/dom/beam.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/page.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/system.h"
#include "engraving/dom/textbase.h"
#include "engraving/infrastructure/eidregister.h"

namespace engrave {
namespace {

using mu::engraving::ElementType;
using mu::engraving::EngravingItem;
using mu::engraving::EngravingObject;
using mu::engraving::MasterScore;
using mu::engraving::Measure;
using mu::engraving::Page;
using mu::engraving::Part;
using mu::engraving::Score;
using mu::engraving::Segment;
using mu::engraving::SegmentType;
using mu::engraving::Spanner;
using mu::engraving::Staff;
using mu::engraving::System;

// No position: an object that is not in a measure at a track.
constexpr int no_position = -1;

std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

mu::engraving::Fraction to_engine_fraction(const Fraction& value) {
    return mu::engraving::Fraction(static_cast<int>(value.num), static_cast<int>(value.den));
}

Fraction from_engine_fraction(const mu::engraving::Fraction& value) {
    return {value.numerator(), value.denominator()};
}

// Every object of the document, each once: per score (the master first, then the
// excerpts) the score, its parts, its staves, its spanners and its measures with
// what they hold, then what Score::scanElements adds (the layout objects). A spanner
// is kept in the score's spanner map rather than in the ownership tree and a measure
// is not an element, hence the extra lists; an element that an undo command removed
// is in no segment and no map any more, so it is not here.
template <typename Function>
void for_each_object(MasterScore& master, const Function& func) {
    std::unordered_set<const EngravingObject*> seen;
    const auto visit = [&func, &seen](EngravingObject* object) {
        if (object && seen.insert(object).second) func(*object);
    };
    for (Score* score : master.scoreList()) {
        visit(score);
        for (Part* part : score->parts()) visit(part);
        for (Staff* staff : score->staves()) visit(staff);
        for (Spanner* spanner : score->spannerList()) visit(spanner);
        // scanElements skips what is on a hidden staff and never hands out a chord
        // itself (only its notes, stem and the rest), so a measure's own lists and
        // segments are walked as well, and a chord is reached through its parts.
        const auto visit_item = [&visit](EngravingItem* item) {
            visit(item);
            EngravingObject* parent = item->parent();
            if (parent && parent->isChordRest()) visit(parent);
            // A beam is referred to by the chords it joins, not held by a measure.
            if (item->isChordRest()) visit(mu::engraving::toChordRest(item)->beam());
        };
        const auto visit_tree = [&visit_item](EngravingItem* item) {
            visit_item(item);
            item->scanElements(visit_item);
        };
        for (mu::engraving::MeasureBase* mb = score->first(); mb; mb = mb->next()) {
            visit(mb);
            if (!mb->isMeasure()) continue;
            Measure* measure = mu::engraving::toMeasure(mb);
            if (measure->mmRest()) visit(measure->mmRest());
            for (EngravingItem* item : measure->el()) visit_tree(item);
            for (Segment* segment = measure->first(); segment; segment = segment->next()) {
                for (EngravingItem* item : segment->elist()) {
                    if (item) visit_tree(item);
                }
                for (EngravingItem* item : segment->annotations()) visit_tree(item);
            }
        }
        score->scanElements(visit_item);
    }
}

int score_index(MasterScore& master, const Score* score) {
    int index = 0;
    for (const Score* candidate : master.scoreList()) {
        if (candidate == score) return index;
        ++index;
    }
    return no_position;
}

Placement placement_of(MasterScore& master, const EngravingObject& object) {
    Placement placement;
    placement.score = score_index(master, object.score());
    if (!object.isEngravingItem()) return placement;
    const auto& item = static_cast<const EngravingItem&>(object);
    const auto track = static_cast<long long>(item.track());
    if (track < 0) return placement;
    placement.tick = item.tick().ticks();
    placement.track = static_cast<int>(track);
    return placement;
}

// Whether an object is one of the document's.  The walk is what decides: an element
// that an undo command removed is no longer in a segment's element list, a spanner map
// or a staff, even though its parent still points into the tree, so an ownership chain
// cannot tell it from a live one.
bool in_document(MasterScore& master, EngravingObject* object)
{
    bool found = false;
    for_each_object(master, [&found, object](EngravingObject& candidate) {
        if (&candidate == object) found = true;
    });
    return found;
}

} // namespace

bool in_score(MasterScore& master, const EngravingObject* object)
{
    const EngravingObject* top = object;
    while (const EngravingObject* parent = top->ownershipParent()) top = parent;
    for (const Score* score : master.scoreList()) {
        if (static_cast<const EngravingObject*>(score) == top) return true;
    }
    if (!object->isSpanner() || !object->score()) return false;
    const auto spanners = object->score()->spannerList();
    return std::ranges::find(spanners, mu::engraving::toSpanner(object)) != spanners.end();
}

namespace {

// The single-bit segment types an Address can name, with the token that names each.
struct SegmentKind {
    SegmentType type;
    std::string_view token;
};

constexpr SegmentKind segment_kinds[] = {
    {SegmentType::BeginBarLine, "BeginBarLine"},
    {SegmentType::HeaderClef, "HeaderClef"},
    {SegmentType::KeySig, "KeySig"},
    {SegmentType::Ambitus, "Ambitus"},
    {SegmentType::Breath, "Breath"},
    {SegmentType::TimeSig, "TimeSig"},
    {SegmentType::StartRepeatBarLine, "StartRepeatBarLine"},
    {SegmentType::ClefStartRepeatAnnounce, "ClefStartRepeatAnnounce"},
    {SegmentType::KeySigStartRepeatAnnounce, "KeySigStartRepeatAnnounce"},
    {SegmentType::TimeSigStartRepeatAnnounce, "TimeSigStartRepeatAnnounce"},
    {SegmentType::Clef, "Clef"},
    {SegmentType::BarLine, "BarLine"},
    {SegmentType::TimeTick, "TimeTick"},
    {SegmentType::ChordRest, "ChordRest"},
    {SegmentType::ClefRepeatAnnounce, "ClefRepeatAnnounce"},
    {SegmentType::KeySigRepeatAnnounce, "KeySigRepeatAnnounce"},
    {SegmentType::TimeSigRepeatAnnounce, "TimeSigRepeatAnnounce"},
    {SegmentType::EndBarLine, "EndBarLine"},
    {SegmentType::KeySigAnnounce, "KeySigAnnounce"},
    {SegmentType::TimeSigAnnounce, "TimeSigAnnounce"},
};

SegmentType segment_type_of(std::string_view token) {
    for (const SegmentKind& kind : segment_kinds) {
        if (kind.token == token) return kind.type;
    }
    return SegmentType::Invalid;
}

std::string_view segment_token(SegmentType type) {
    for (const SegmentKind& kind : segment_kinds) {
        if (kind.type == type) return kind.token;
    }
    return "Invalid";
}

ScoreSel score_selection(const Score* score) {
    ScoreSel selection;
    if (score && !score->isMaster()) selection.excerpt = from_engine(score->eid());
    return selection;
}

Score* selected_score(MasterScore& master, const ScoreSel& selection) {
    if (!selection.excerpt) return &master;
    const auto found = resolve(master, *selection.excerpt);
    if (!found || found->object->type() != ElementType::SCORE) return nullptr;
    return mu::engraving::toScore(found->object);
}

template <typename T>
Result<Resolved> resolve_at(const std::vector<T*>& items, std::int32_t index, std::string_view what) {
    if (index < 0 || static_cast<std::size_t>(index) >= items.size()) {
        return fail(ErrorCode::StaleHandle, std::format("no {} at index {}", what, index));
    }
    return Resolved{items[static_cast<std::size_t>(index)], false};
}

} // namespace

IdClass id_class(const EngravingObject& object) {
    switch (object.type()) {
    case ElementType::SEGMENT:
        return IdClass::address;
    // Layout builds these from what is written (a chord builds its stem, a system its
    // brackets), so an id on one is not in the file and does not survive the next layout
    // pass.  RootItem, DummyParent and ShadowNote belong to the score itself.
    case ElementType::SYSTEM:
    case ElementType::PAGE:
    case ElementType::BRACKET:
    case ElementType::BRACKET_ITEM:
    case ElementType::INSTRUMENT_NAME:
    case ElementType::MEASURE_NUMBER:
    case ElementType::STAFF_LINES:
    case ElementType::LEDGER_LINE:
    case ElementType::STEM:
    case ElementType::STEM_SLASH:
    case ElementType::HOOK:
    case ElementType::NOTEDOT:
    case ElementType::GUITAR_BEND_TEXT:
    case ElementType::GRACE_NOTES_GROUP:
    case ElementType::FIGURED_BASS_ITEM:
    case ElementType::PARENTHESIS:
    case ElementType::ROOT_ITEM:
    case ElementType::DUMMY:
    case ElementType::SHADOW_NOTE:
        return IdClass::layout;
    default:
        break;
    }
    // A multi-measure rest is layout's: it replaces a range of measures.
    if (object.isMeasure() && mu::engraving::toMeasure(&object)->isMMRest()) return IdClass::layout;
    // A tuplet draws its own number, and layout creates that text.
    if (object.isText() && object.ownershipParent() && object.ownershipParent()->isTuplet()) return IdClass::layout;
    // A spanner is kept in its score's spanner map when it is anchored to a segment. One
    // anchored to a note hangs off the note instead, and a read can drop it (a tie whose
    // other end is missing), so the ones the map holds are what the walk hands out.
    if (object.isSpanner()) {
        const Score* score = object.score();
        if (!score) return IdClass::session;
        const auto spanners = score->spannerList();
        const auto found = std::ranges::find(spanners, mu::engraving::toSpanner(&object));
        return found == spanners.end() ? IdClass::session : IdClass::persisted;
    }
    // A spanner segment is rebuilt by layout from its spanner.
    if (object.isSpannerSegment()) return IdClass::layout;
    // Whatever layout hangs off a layout object (the text of a hairpin, a notehead of a
    // grace group) is rebuilt with it.
    const EngravingObject* parent = object.ownershipParent();
    if (parent && id_class(*parent) == IdClass::layout) return IdClass::layout;
    return IdClass::persisted;
}

bool takes_id(const EngravingObject& object) {
    if (id_class(object) != IdClass::persisted) return false;
    if (!object.isEngravingItem()) return true;
    const auto& item = static_cast<const EngravingItem&>(object);
    if (item.generated()) return false;
    // The writer stores no id for an empty text (a template's empty title, subtitle or
    // poet placeholder), so one of those has none until it has text.
    return !item.isText() || !static_cast<const mu::engraving::TextBase&>(item).empty();
}

mu::engraving::EID to_engine(const ElementId& id) {
    return mu::engraving::EID::fromParts(id.hi, id.lo);
}

ElementId from_engine(const mu::engraving::EID& id) {
    return {id.first(), id.second()};
}

void assign_missing_ids(MasterScore& master) {
    std::unordered_set<const EngravingObject*> document;
    for_each_object(master, [&document, &master](EngravingObject& object) {
        document.insert(&object);
        if (takes_id(object) && !object.eid().isValid()) object.assignNewEID();
    });
    // An id whose element is no longer part of the document must go as well: a read can
    // replace what it built (an excerpt score's first pass, a tie whose other end is
    // missing), and such an entry would answer for an element no caller can address.
    // An element the walk does not hand out but that is still in a score (a guitar bend
    // on its note) keeps the id the file gave it, so that a save writes the same id again.
    for (const auto& [eid, object] : master.eidRegister()->entries()) {
        if (!document.contains(object) && !in_score(master, object)) master.eidRegister()->removeItem(object);
    }
}

std::vector<EngravingObject*> document_objects(MasterScore& master) {
    std::vector<EngravingObject*> objects;
    for_each_object(master, [&objects](EngravingObject& object) { objects.push_back(&object); });
    return objects;
}

std::vector<LiveId> live_ids(MasterScore& master) {
    std::vector<LiveId> ids;
    for_each_object(master, [&master, &ids](EngravingObject& object) {
        if (!takes_id(object)) return;
        const mu::engraving::EID eid = object.eid();
        if (eid.isValid()) ids.push_back({from_engine(eid), &object, placement_of(master, object)});
    });
    std::ranges::sort(ids, {}, &LiveId::id);
    return ids;
}

IdDiff diff_ids(std::span<const LiveId> before, std::span<const LiveId> after) {
    std::map<ElementId, const LiveId*> old;
    for (const LiveId& entry : before) old.emplace(entry.id, &entry);
    std::map<ElementId, const LiveId*> current;
    for (const LiveId& entry : after) current.emplace(entry.id, &entry);
    std::map<Placement, int> appeared;
    for (const LiveId& entry : after) {
        if (!old.contains(entry.id)) ++appeared[entry.placement];
    }

    IdDiff diff;
    for (const LiveId& entry : after) {
        if (!old.contains(entry.id)) diff.created.push_back(entry.id);
    }
    for (const LiveId& entry : before) {
        if (current.contains(entry.id)) continue;
        auto count = appeared.find(entry.placement);
        if (entry.placement.track == no_position || count == appeared.end() || count->second == 0) {
            diff.removed.push_back(entry.id);
        } else {
            --count->second;
            diff.replaced.push_back(entry.id);
        }
    }
    std::ranges::sort(diff.created);
    std::ranges::sort(diff.removed);
    std::ranges::sort(diff.replaced);
    return diff;
}

Result<Resolved> resolve(MasterScore& master, const ElementId& id) {
    const mu::engraving::EID eid = to_engine(id);
    // A score, a part and a shared part own their ids through the document's lists,
    // not through an element's ownership chain: find them there, never through a
    // register pointer that could name an object the lists no longer hold.
    for (Score* score : master.scoreList()) {
        if (score->eid() == eid) return Resolved{score, true};
        for (Part* part : score->parts()) {
            if (part->eid() == eid) return Resolved{part, true};
        }
    }

    EngravingObject* object = master.eidRegister()->itemFromEID(eid);
    if (!object) return fail(ErrorCode::NotFound, std::format("no element has id {}", to_string(id)));
    if (!in_document(master, object)) {
        return fail(ErrorCode::StaleHandle, std::format("the element with id {} is not part of the score", to_string(id)));
    }
    return Resolved{object, true};
}

Result<Resolved> resolve(MasterScore& master, const Address& address) {
    return std::visit(
        [&master](const auto& value) -> Result<Resolved> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, SegmentAddress>) {
                const Result<Resolved> measure = resolve(master, value.measure);
                if (!measure) return measure;
                if (measure->object->type() != ElementType::MEASURE) {
                    return fail(ErrorCode::InvalidArgument, "a segment address names a measure");
                }
                const SegmentType kind = segment_type_of(value.kind);
                if (kind == SegmentType::Invalid) {
                    return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a segment type", value.kind));
                }
                Measure* found = mu::engraving::toMeasure(measure->object);
                Segment* segment = found->findSegment(kind, to_engine_fraction(value.tick));
                if (!segment) {
                    return fail(ErrorCode::StaleHandle, std::format("no {} segment at tick {}/{}", value.kind, value.tick.num,
                                                                    value.tick.den));
                }
                return Resolved{segment, true};
            } else if constexpr (std::is_same_v<T, PageAddress>) {
                Score* score = selected_score(master, value.score);
                if (!score) return fail(ErrorCode::StaleHandle, "the score a page address names is gone");
                return resolve_at(score->pages(), value.index, "page");
            } else if constexpr (std::is_same_v<T, SystemAddress>) {
                Score* score = selected_score(master, value.score);
                if (!score) return fail(ErrorCode::StaleHandle, "the score a system address names is gone");
                const Result<Resolved> page = resolve_at(score->pages(), value.page, "page");
                if (!page) return page;
                return resolve_at(mu::engraving::toPage(page->object)->systems(), value.index, "system");
            } else {
                const Result<Resolved> spanner = resolve(master, value.spanner);
                if (!spanner) return spanner;
                if (!spanner->object->isSpanner()) {
                    return fail(ErrorCode::InvalidArgument, "a spanner segment address names a spanner");
                }
                return resolve_at(mu::engraving::toSpanner(spanner->object)->spannerSegments(), value.index, "spanner segment");
            }
        },
        address);
}

std::optional<Address> address_of(const EngravingObject& object) {    if (object.type() == ElementType::SEGMENT) {
        const auto& segment = static_cast<const Segment&>(object);
        const Measure* measure = segment.measure();
        if (!measure || !measure->eid().isValid()) return std::nullopt;
        SegmentAddress address;
        address.measure = from_engine(measure->eid());
        address.tick = from_engine_fraction(segment.tick());
        address.kind = segment_token(segment.segmentType());
        return Address{address};
    }
    if (object.type() == ElementType::PAGE) {
        const auto& page = static_cast<const Page&>(object);
        Score* score = page.score();
        if (!score) return std::nullopt;
        PageAddress address;
        address.score = score_selection(score);
        address.index = static_cast<std::int32_t>(score->pageIdx(&page));
        return Address{address};
    }
    if (object.type() == ElementType::SYSTEM) {
        const auto& system = static_cast<const System&>(object);
        Score* score = system.score();
        const Page* page = system.page();
        if (!score || !page) return std::nullopt;
        SystemAddress address;
        address.score = score_selection(score);
        address.page = static_cast<std::int32_t>(score->pageIdx(page));
        const auto& systems = page->systems();
        const auto index = std::ranges::find(systems, &system);
        if (index == systems.end()) return std::nullopt;
        address.index = static_cast<std::int32_t>(index - systems.begin());
        return Address{address};
    }
    if (object.isSpannerSegment()) {
        const auto& segment = static_cast<const mu::engraving::SpannerSegment&>(object);
        const Spanner* spanner = segment.spanner();
        if (!spanner || !spanner->eid().isValid()) return std::nullopt;
        const auto& segments = spanner->spannerSegments();
        const auto index = std::ranges::find(segments, &segment);
        if (index == segments.end()) return std::nullopt;
        SpannerSegmentAddress address;
        address.spanner = from_engine(spanner->eid());
        address.index = static_cast<std::int32_t>(index - segments.begin());
        return Address{address};
    }
    return std::nullopt;
}

std::vector<Diagnostic> id_repairs(MasterScore& master) {
    const mu::engraving::EIDRepairCount repairs = master.eidRegister()->repairs();
    std::vector<Diagnostic> diagnostics;
    if (repairs.malformed > 0) {
        diagnostics.push_back({Severity::Warning,
                               std::format("{} element id(s) were malformed and ignored; those elements got new ids",
                                           repairs.malformed)});
    }
    if (repairs.duplicate > 0) {
        diagnostics.push_back({Severity::Warning,
                               std::format("{} element(s) shared an id with another; each got a new id", repairs.duplicate)});
    }
    return diagnostics;
}

} // namespace engrave
