// InsertMeasures, RemoveMeasures, SetTimeSig, SetKeySig, InsertClef and
// SetLayoutBreak: the engine's own undoable edits of the measure list and of the
// elements that start a measure.

#include <cstdint>
#include <format>
#include <optional>
#include <string>

#include "engraving/dom/clef.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/key.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/timesig.h"
#include "engraving/editing/editclef.h"
#include "engraving/editing/editkeysig.h"
#include "engraving/editing/edittimesig.h"
#include "engraving/editing/transaction/transaction.h"
#include "engraving/editing/transaction/undostack.h"
#include "engraving/types/typesconv.h"

#include "engrave/src/api/batch.h"
#include "engrave/src/api/value_codec.h"

namespace engrave::api {
namespace {

using mu::engraving::Clef;
using mu::engraving::ClefType;
using mu::engraving::ElementType;
using mu::engraving::EngravingItem;
using mu::engraving::Key;
using mu::engraving::KeySigEvent;
using mu::engraving::LayoutBreakType;
using mu::engraving::Measure;
using mu::engraving::MeasureBase;
using mu::engraving::Score;
using mu::engraving::Segment;
using mu::engraving::SegmentType;
using mu::engraving::Staff;
using mu::engraving::TConv;
using mu::engraving::TimeSig;
using mu::engraving::TimeSigType;
using mu::engraving::Transaction;
using mu::engraving::VOICES;

std::unexpected<Error> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
}

std::unexpected<Error> refused(std::string message) {
    return std::unexpected(Error{ErrorCode::EditRejected, std::move(message)});
}

// The transaction the batch runs in; the guard has it open while a step runs.
Transaction& transaction(EditContext& context) {
    return context.master().transactionManager()->currentOrDummyTransaction();
}

std::size_t pushed(EditContext& context) {
    return context.master().undoStack()->activeTransaction()->commands().size();
}

// A token of one of the engine's xml tables, and nothing else: an unknown token and
// the fallback's own token are refused, so a typo is never a silent default.
template <typename T>
std::optional<T> token(const std::string& text, T fallback) {
    const T value = TConv::fromXml(muse::AsciiStringView(text), fallback);
    if (muse::AsciiStringView(TConv::toXml(value)) != muse::AsciiStringView(text)) return std::nullopt;
    return value;
}

Result<Measure*> measure_of(const EditContext& context, const Target& target) {
    auto object = context.object(target);
    if (!object) return std::unexpected(object.error());
    if (!(*object)->isMeasure()) return invalid(std::format("a {} is not a measure", (*object)->typeName()));
    return mu::engraving::toMeasure(*object);
}

// The clef a staff shows at a tick: the last clef at or before it, in this score.
Clef* clef_in_effect(Score* score, const mu::engraving::Fraction& tick, mu::engraving::track_idx_t track) {
    for (Measure* measure = score->tick2measure(tick); measure; measure = measure->prevMeasure()) {
        for (Segment* segment = measure->last(); segment; segment = segment->prev()) {
            if (!(segment->segmentType() & (SegmentType::Clef | SegmentType::HeaderClef))) continue;
            if (segment->tick() > tick) continue;
            EngravingItem* item = segment->element(track);
            if (item && item->isClef()) return mu::engraving::toClef(item);
        }
    }
    return nullptr;
}

} // namespace

Result<Step> prepare(EditContext& context, const InsertMeasures& op) {
    std::optional<Target> before;
    if (op.before) {
        auto target = context.target(*op.before);
        if (!target) return std::unexpected(target.error());
        before = *target;
    }
    if (op.count < 1) return invalid("inserting needs at least one measure");
    const auto kind = token<ElementType>(op.kind, ElementType::INVALID);
    if (!kind) return invalid(std::format("'{}' is not a measure or a frame type", op.kind));
    if (*kind != ElementType::MEASURE && *kind != ElementType::VBOX && *kind != ElementType::HBOX
        && *kind != ElementType::TBOX && *kind != ElementType::FBOX) {
        return invalid(std::format("'{}' is not a measure or a frame type", op.kind));
    }
    return [before, count = op.count, kind = *kind, name = op.kind](EditContext& context) -> Result<void> {
        MeasureBase* at = nullptr;
        if (before) {
            auto object = context.object(*before);
            if (!object) return std::unexpected(object.error());
            if (!(*object)->isMeasureBase()) {
                return invalid(std::format("a {} is not a measure or a frame", (*object)->typeName()));
            }
            at = mu::engraving::toMeasureBase(*object);
        }
        // The master inserts a measure into every score of the document; a frame goes
        // into the master only, which is what the editor does.
        Score* score = &context.master();
        Score::InsertMeasureOptions options;
        options.needDeselectAll = false;
        for (int i = 0; i < count; ++i) {
            MeasureBase* made = kind == ElementType::MEASURE ? score->insertMeasure(kind, at, options)
                                                            : score->insertBox(kind, at, options);
            if (!made) return refused(std::format("the engine did not insert a {}", name));
            context.created(made);
        }
        return {};
    };
}

Result<Step> prepare(EditContext& context, const RemoveMeasures& op) {
    auto first = context.target(op.first);
    if (!first) return std::unexpected(first.error());
    auto last = context.target(op.last);
    if (!last) return std::unexpected(last.error());
    return [first = *first, last = *last](EditContext& context) -> Result<void> {
        auto start = measure_of(context, first);
        if (!start) return std::unexpected(start.error());
        auto end = measure_of(context, last);
        if (!end) return std::unexpected(end.error());
        if ((*start)->score() != (*end)->score()) return invalid("the range starts and ends in different scores");
        if ((*end)->tick() < (*start)->tick()) return invalid("the range ends before it starts");
        // The engine's own deletion: it removes the range from every score, keeps what
        // slurs and ties need, and restores the time signature that follows.
        (*start)->score()->deleteMeasures(*start, *end);
        return {};
    };
}

Result<Step> prepare(EditContext& context, const SetTimeSig& op) {
    auto measure = context.target(op.measure);
    if (!measure) return std::unexpected(measure.error());
    if (op.sig.num <= 0 || op.sig.den <= 0) return invalid("a time signature is a positive fraction");
    if (op.sig.num > INT32_MAX || op.sig.den > INT32_MAX) return invalid("the time signature does not fit in an int");
    const mu::engraving::Fraction sig(int(op.sig.num), int(op.sig.den));
    return [measure = *measure, sig](EditContext& context) -> Result<void> {
        auto target = measure_of(context, measure);
        if (!target) return std::unexpected(target.error());
        Score* score = (*target)->score();
        // The engine's own measure rewrite: takes a temporary signature, applies it to
        // every score and staff, and rewrites the measures up to the next signature.
        TimeSig* temp = mu::engraving::Factory::createTimeSig(score->dummy());
        temp->setSig(sig, TimeSigType::NORMAL);
        const std::size_t before = pushed(context);
        mu::engraving::EditTimeSig::addTimeSig(transaction(context), score, *target, 0, temp, false);
        // The signature that is already there is a no-op, and a rewrite the engine
        // cannot do leaves nothing behind either (it unwinds); both say the score is
        // not what the caller asked for, and nothing of the batch is left to tell.
        if (pushed(context) <= before) return refused("the engine kept the existing time signature");
        return {};
    };
}

Result<Step> prepare(EditContext& context, const SetKeySig& op) {
    auto measure = context.target(op.measure);
    if (!measure) return std::unexpected(measure.error());
    std::optional<Target> staff;
    if (op.staff) {
        auto target = context.target(*op.staff);
        if (!target) return std::unexpected(target.error());
        staff = *target;
    }
    if (op.key < int(Key::MIN) || op.key > int(Key::MAX)) {
        return invalid(std::format("a key signature has {} to {} accidentals", int(Key::MIN), int(Key::MAX)));
    }
    KeySigEvent event;
    event.setConcertKey(static_cast<Key>(op.key));
    return [measure = *measure, staff, event](EditContext& context) -> Result<void> {
        auto target = measure_of(context, measure);
        if (!target) return std::unexpected(target.error());
        Score* score = (*target)->score();
        Transaction& tx = transaction(context);
        if (staff) {
            auto object = context.object(*staff);
            if (!object) return std::unexpected(object.error());
            if (!(*object)->isStaff()) return invalid(std::format("a {} is not a staff", (*object)->typeName()));
            Staff* one = mu::engraving::toStaff(*object);
            if (one->score() != score) return invalid("the staff is not in the measure's score");
            mu::engraving::EditKeySig::undoChangeKeySig(tx, score, one, (*target)->tick(), event);
        } else {
            for (Staff* each : score->staves()) mu::engraving::EditKeySig::undoChangeKeySig(tx, score, each, (*target)->tick(), event);
        }
        return {};
    };
}

Result<Step> prepare(EditContext& context, const InsertClef& op) {
    auto target = context.target(Ref{op.at});
    if (!target) return std::unexpected(target.error());
    const auto* address = std::get_if<SegmentAddress>(&op.at);
    if (!address) return invalid("a clef goes at a segment");
    auto value = from_value(mu::engraving::P_TYPE::CLEF_TYPE, Enum{op.clef});
    if (!value) return std::unexpected(value.error());
    const ClefType clef = value->value<ClefType>();
    return [target = *target, staff = address->staff, voice = address->voice, clef](EditContext& context) -> Result<void> {
        auto object = context.object(target);
        if (!object) return std::unexpected(object.error());
        if (!(*object)->isSegment()) return invalid(std::format("a {} is not a segment", (*object)->typeName()));
        Segment* segment = mu::engraving::toSegment(*object);
        Score* score = segment->score();
        if (staff < 0 || std::size_t(staff) >= score->nstaves()) {
            return invalid(std::format("the score has no staff {}", staff));
        }
        if (voice < 0 || std::size_t(voice) >= VOICES) return invalid(std::format("the score has no voice {}", voice));
        Staff* staff_of = score->staff(mu::engraving::staff_idx_t(staff));
        EngravingItem* before = segment->element(mu::engraving::staff2track(mu::engraving::staff_idx_t(staff),
                                                                           mu::engraving::voice_idx_t(voice)));
        if (!before) before = segment;
        const mu::engraving::Fraction tick = before->tick();
        Clef* was = clef_in_effect(score, tick, mu::engraving::staff2track(mu::engraving::staff_idx_t(staff)));
        mu::engraving::EditClef::undoChangeClef(transaction(context), score, staff_of, before, clef);
        // A clef that the change put in place of nothing, or of another clef, is what
        // the operation created; a clef that changed in place was already the caller's.
        if (Clef* now = clef_in_effect(score, tick, mu::engraving::staff2track(mu::engraving::staff_idx_t(staff)));
            now && now != was) {
            context.created(now);
        }
        return {};
    };
}

Result<Step> prepare(EditContext& context, const SetLayoutBreak& op) {
    auto measure = context.target(op.measure);
    if (!measure) return std::unexpected(measure.error());
    const auto kind = token<LayoutBreakType>(op.kind, LayoutBreakType::LINE);
    if (!kind) return invalid(std::format("'{}' is not a layout break kind", op.kind));
    return [measure = *measure, kind = *kind, on = op.on](EditContext& context) -> Result<void> {
        auto object = context.object(measure);
        if (!object) return std::unexpected(object.error());
        if (!(*object)->isMeasureBase()) {
            return invalid(std::format("a {} is not a measure or a frame", (*object)->typeName()));
        }
        // The engine's own switch: it sets the flag, adds or removes the break element
        // through undo, and drops the break that the new one replaces.
        mu::engraving::toMeasureBase(*object)->undoSetBreak(on, kind);
        return {};
    };
}

} // namespace engrave::api
