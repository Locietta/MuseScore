// AddSpanner, AddTie and AddTuplet: the engine's own undoable creation of a
// spanner (Score::addSlur, EditSpanner::addSpanner), of a tie (what EditTie's
// createAndAddTie makes of one) and of a tuplet (Score::addTuplet, which
// EditTuplet::addTuplets calls too).

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/durationtype.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/note.h"
#include "engraving/dom/property.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/utils.h"
#include "engraving/editing/editspanner.h"
#include "engraving/editing/transaction/transaction.h"
#include "engraving/style/style.h"
#include "engraving/style/styledef.h"
#include "engraving/types/fraction.h"
#include "engraving/types/typesconv.h"

#include "engrave/src/api/batch.h"
#include "engrave/src/api/pid_support.h"
#include "engrave/src/api/value_codec.h"

namespace engrave::api {
namespace {

using mu::engraving::ChordRest;
using mu::engraving::ElementType;
using mu::engraving::EngravingItem;
using mu::engraving::EngravingObject;
using mu::engraving::Note;
using mu::engraving::Pid;
using mu::engraving::PropertyFlags;
using mu::engraving::PropertyValue;
using mu::engraving::Score;
using mu::engraving::Segment;
using mu::engraving::SegmentType;
using mu::engraving::Spanner;
using mu::engraving::Tie;
using mu::engraving::Tuplet;
using mu::engraving::TupletBracketType;
using mu::engraving::TupletNumberType;

std::unexpected<Error> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
}

std::unexpected<Error> rejected(std::string message) {
    return std::unexpected(Error{ErrorCode::EditRejected, std::move(message)});
}

// The chord or rest a reference names: a chord rest, or the chord of a note.
Result<ChordRest*> chord_rest(const EditContext& context, const Target& target) {
    auto object = context.object(target);
    if (!object) return std::unexpected(object.error());
    EngravingObject* found = *object;
    if (found->isNote()) found = mu::engraving::toNote(found)->chord();
    if (!found->isChordRest()) return invalid(std::format("{} is not a chord or a rest", found->typeName()));
    return mu::engraving::toChordRest(found);
}

// The properties of an operation, checked against the catalogue's kinds.
Result<std::vector<std::pair<Pid, PropertyValue>>> properties(const std::vector<std::pair<std::string, Value>>& props) {
    std::vector<std::pair<Pid, PropertyValue>> resolved;
    resolved.reserve(props.size());
    for (const auto& [name, value] : props) {
        const Pid pid = pid_of(name);
        if (pid == Pid::END) return invalid(std::format("no property '{}'", name));
        auto checked = from_value(mu::engraving::propertyType(pid), value);
        if (!checked) return std::unexpected(checked.error());
        resolved.emplace_back(pid, *checked);
    }
    return resolved;
}

// Sets the properties of an operation, as SetProperty does: a styled property
// that is given a value of its own stops following the style.
Result<void> set_properties(EngravingObject& object, const std::vector<std::pair<Pid, PropertyValue>>& props) {
    for (const auto& [pid, value] : props) {
        const std::optional<bool> listed = settable(object.typeName(), pid);
        if (!listed) return invalid(std::format("a {} has no property '{}'", object.typeName(), name_of(pid)));
        if (!*listed) {
            return invalid(std::format("'{}' of a {} is changed by structural operations only", name_of(pid),
                                       object.typeName()));
        }
        const PropertyFlags flags = object.propertyFlags(pid);
        static_cast<EngravingItem&>(object).undoChangeProperty(pid, value,
                                                               flags == PropertyFlags::STYLED ? PropertyFlags::UNSTYLED : flags);
    }
    return {};
}

} // namespace

Result<Step> prepare(EditContext& context, const AddSpanner& op) {
    auto start = context.target(op.start);
    if (!start) return std::unexpected(start.error());
    auto end = context.target(op.end);
    if (!end) return std::unexpected(end.error());
    const ElementType type = mu::engraving::TConv::fromXml(muse::AsciiStringView(op.type), ElementType::INVALID);
    if (type == ElementType::INVALID) return invalid(std::format("no element type '{}'", op.type));
    auto props = properties(op.props);
    if (!props) return std::unexpected(props.error());
    return [start = *start, end = *end, type, name = op.type, props = std::move(*props)](EditContext& context) -> Result<void> {
        auto first = chord_rest(context, start);
        if (!first) return std::unexpected(first.error());
        auto last = chord_rest(context, end);
        if (!last) return std::unexpected(last.error());
        Score* score = (*first)->score();
        if ((*last)->score() != score) return invalid("a spanner starts and ends in one score");

        Spanner* spanner = nullptr;
        if (type == ElementType::SLUR) {
            // The engine's own slur: it follows the chain of tied notes up to the note
            // the slur has to end at.
            spanner = score->addSlur(*first, *last, nullptr);
            if (!spanner) return rejected("the engine did not make the slur");
        } else {
            std::unique_ptr<EngravingItem> created(mu::engraving::Factory::createItem(type, score->dummy()));
            if (!created) return invalid(std::format("{} cannot be created", name));
            if (!created->isSpanner()) return invalid(std::format("a {} is not a spanner", created->typeName()));
            Spanner* placed = mu::engraving::toSpanner(created.get());
            if (placed->anchor() != Spanner::Anchor::SEGMENT) {
                return invalid(std::format("AddSpanner places spanners that run from one segment to the next; a {} is not one",
                                           placed->typeName()));
            }
            // All spanners of this kind live on one staff: the one the start names.
            Segment* startSegment = (*first)->segment();
            Segment* endSegment = (*last)->segment();
            if (!startSegment || !endSegment) return invalid("the chord rest has no segment to start at");
            created.release();
            mu::engraving::EditSpanner::addSpanner(score->transactionManager()->currentOrDummyTransaction(), score, placed,
                                                   (*first)->staffIdx(), startSegment, endSegment);
            spanner = placed;
        }
        if (auto set = set_properties(*spanner, props); !set) return set;
        context.created(spanner);
        return {};
    };
}

Result<Step> prepare(EditContext& context, const AddTie& op) {
    auto target = context.target(op.note);
    if (!target) return std::unexpected(target.error());
    return [target = *target](EditContext& context) -> Result<void> {
        auto object = context.object(target);
        if (!object) return std::unexpected(object.error());
        if (!(*object)->isNote()) return invalid(std::format("{} is not a note", (*object)->typeName()));
        Note* note = mu::engraving::toNote(*object);
        if (note->tieFor()) return rejected("the note already has a tie");
        Note* end = mu::engraving::searchTieNote(note);
        if (!end) return rejected("no note of the same pitch follows this one");
        if (end->tieBack()) return rejected("the note to tie to already has a tie");

        // What EditTie's createAndAddTie makes of a tie to a note that is free: the
        // engine sets both notes' tie pointers when Score::addElement adds the tie.
        Score* score = note->score();
        Tie* tie = mu::engraving::Factory::createTie(note);
        tie->setStartNote(note);
        tie->setTrack(note->track());
        tie->setTick(note->chord()->segment()->tick());
        tie->setEndNote(end);
        tie->setTicks(end->chord()->segment()->tick() - note->chord()->segment()->tick());
        score->undoAddElement(tie);
        tie->addTiesToJumpPoints();
        context.created(tie);
        return {};
    };
}

Result<Step> prepare(EditContext& context, const AddTuplet& op) {
    if (op.actual <= 0 || op.normal <= 0) return invalid(std::format("{}:{} is not a tuplet ratio", op.actual, op.normal));
    auto first = context.target(op.first);
    if (!first) return std::unexpected(first.error());
    auto last = context.target(op.last);
    if (!last) return std::unexpected(last.error());
    return [first = *first, last = *last, actual = op.actual,
            normal = op.normal](EditContext& context) -> Result<void> {
        auto firstRest = chord_rest(context, first);
        if (!firstRest) return std::unexpected(firstRest.error());
        auto lastRest = chord_rest(context, last);
        if (!lastRest) return std::unexpected(lastRest.error());
        Score* score = (*firstRest)->score();
        const mu::engraving::track_idx_t track = (*firstRest)->track();
        if ((*lastRest)->score() != score || (*lastRest)->track() != track) {
            return invalid("a tuplet covers the chords and rests of one voice");
        }
        const mu::engraving::Fraction from = (*firstRest)->tick();
        const mu::engraving::Fraction to = (*lastRest)->tick();
        if (to < from) return invalid("the range ends before it starts");

        // The ticks are collected first: making a tuplet replaces the chord rest it
        // starts with, and its rests are not to be tupleted again.
        std::vector<mu::engraving::Fraction> ticks;
        for (Segment* segment = (*firstRest)->segment(); segment && segment->tick() <= to;
             segment = segment->next1(SegmentType::ChordRest)) {
            const EngravingItem* element = segment->element(track);
            if (element && element->isChordRest()) ticks.push_back(segment->tick());
        }
        if (ticks.empty()) return invalid("there is no chord or rest to divide");

        // The tuplet is what the score's style makes of one; Score::addTuplet keeps
        // the number and bracket it is given, and marks them styled when they are the
        // style's.
        const mu::engraving::Fraction ratio(actual, normal);
        const TupletNumberType numberType = static_cast<TupletNumberType>(score->style().styleI(mu::engraving::Sid::tupletNumberType));
        const TupletBracketType bracketType
            = static_cast<TupletBracketType>(score->style().styleI(mu::engraving::Sid::tupletBracketType));
        for (const mu::engraving::Fraction& tick : ticks) {
            ChordRest* rest = score->findCR(tick, track);
            if (!rest) return rejected(std::format("the chord rest at tick {} went away", tick.toString().toStdString()));
            // Score::addTuplet's own ratio check: the base length is the chord rest's
            // reduced duration divided by the normal count.
            const mu::engraving::Fraction base = rest->ticks().reduced() * mu::engraving::Fraction(1, normal);
            if (!mu::engraving::TDuration::isValid(base) || !score->canAddTuplet(rest, ratio, mu::engraving::TDuration(base))) {
                return rejected(std::format("a {}:{} tuplet does not fit the {} at tick {}", actual, normal, rest->typeName(),
                                            tick.toString().toStdString()));
            }
            Tuplet* tuplet = score->addTuplet(rest, ratio, numberType, bracketType);
            if (!tuplet) return rejected("the engine did not make the tuplet");
            context.created(tuplet);
        }
        return {};
    };
}

} // namespace engrave::api
