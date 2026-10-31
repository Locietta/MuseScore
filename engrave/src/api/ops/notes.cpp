// EnterNotes, SetPitch, SetDuration and Remove: the engine's own note entry, pitch
// change, length change and deletion.

#include <algorithm>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/input.h"
#include "engraving/dom/interval.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/noteval.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/staff.h"
#include "engraving/editing/editnote.h"
#include "engraving/editing/noteinput.h"
#include "engraving/editing/transaction/transaction.h"
#include "engraving/editing/transpose.h"

#include "engrave/src/api/batch.h"

namespace engrave::api {
namespace {

using mu::engraving::Chord;
using mu::engraving::ChordRest;
using mu::engraving::DirectionV;
using mu::engraving::EngravingItem;
using mu::engraving::InputState;
using mu::engraving::Note;
using mu::engraving::NoteVal;
using mu::engraving::Score;
using mu::engraving::Segment;
using mu::engraving::Staff;
using mu::engraving::Transaction;
using mu::engraving::Transpose;

std::unexpected<Error> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
}

// A duration in the engine's fraction, whose members the engine keeps as ints.
std::optional<mu::engraving::Fraction> engine_fraction(const Fraction& value) {
    constexpr std::int64_t limit = std::numeric_limits<int>::max();
    if (value.num <= 0 || value.den <= 0 || value.num > limit || value.den > limit) return std::nullopt;
    return mu::engraving::Fraction(static_cast<int>(value.num), static_cast<int>(value.den));
}

// The element a running operation works on.
Result<EngravingItem*> edited_item(EditContext& context, const Target& target) {
    auto object = context.object(target);
    if (!object) return std::unexpected(object.error());
    if (!(*object)->isEngravingItem()) return invalid("this operation works on an element, not on a part or a score");
    return mu::engraving::toEngravingItem(*object);
}

// The concert and the written spelling of a pitch at this note's position: the pitch's
// tpc in the staff's concert key, then transposed into what the staff writes, as
// Note::setTpcFromPitch spells a note that the score enters.
std::pair<int, int> spell(const Note& note, int pitch, std::optional<int> tpc) {
    const int tpc1 = tpc.value_or(note.tpc1default(pitch));
    mu::engraving::Interval interval = note.staff()->transpose(note.chord()->tick());
    int tpc2 = tpc1;
    if (!interval.isZero()) {
        interval.flip();
        tpc2 = Transpose::transposeTpc(tpc1, interval, true);
    }
    return {tpc1, tpc2};
}

// What the engine's pitch change does not do: it sets the pitch of one note and of its
// linked copies, so a tablature note (whose fret and string go with the pitch) and a
// percussion note (whose drum the pitch names) need the engine's own edits for those.
// A tied note changes alone, as it does when the score's own pitch keys move it.
std::optional<std::string> unchangeable(const Note& note) {
    const Chord* chord = note.chord();
    const Staff* staff = note.staff();
    if (!chord || !staff) return "it is not in a chord of the score";
    const mu::engraving::Fraction tick = chord->tick();
    if (staff->isTabStaff(tick)) return "a tablature note changes with its fret and string";
    if (staff->isDrumStaff(tick)) return "a percussion note is chosen from the drum set";
    return std::nullopt;
}

// The note value a Pitch asks for. Its tpc, when given, is the concert-pitch spelling:
// the engine derives the written one from the staff's transposition.
NoteVal note_value(const Pitch& pitch) {
    NoteVal value(pitch.pitch);
    if (pitch.tpc) value.tpc1 = *pitch.tpc;
    return value;
}

} // namespace

Result<Step> prepare(EditContext& context, const EnterNotes& op) {
    const auto* at = std::get_if<SegmentAddress>(&op.at);
    if (!at) return invalid("EnterNotes enters notes at a segment address");
    if (at->kind != "ChordRest")
        return invalid(std::format("EnterNotes enters notes in a ChordRest segment, not in a '{}' one", at->kind));
    if (at->staff < 0 || at->voice < 0 || static_cast<std::size_t>(at->voice) >= mu::engraving::VOICES)
        return invalid(std::format("a staff has no voice {}", at->voice));
    const auto duration = engine_fraction(op.duration);
    if (!duration) return invalid("the duration must be a positive fraction of a whole note");
    for (const Pitch& pitch : op.pitches) {
        if (!mu::engraving::pitchIsValid(pitch.pitch)) return invalid(std::format("{} is not a pitch", pitch.pitch));
        if (pitch.tpc && !mu::engraving::tpcIsValid(*pitch.tpc))
            return invalid(std::format("{} is not a tonal pitch class", *pitch.tpc));
    }
    std::vector<int> pitches;
    pitches.reserve(op.pitches.size());
    for (const Pitch& pitch : op.pitches) pitches.push_back(pitch.pitch);
    std::ranges::sort(pitches);
    if (std::ranges::adjacent_find(pitches) != pitches.end())
        return invalid("the pitches of one chord must differ");
    auto target = context.target(op.at);
    if (!target) return std::unexpected(target.error());
    const Segment* segment = mu::engraving::toSegment(target->object);
    const Score* score = segment->score();
    if (static_cast<std::size_t>(at->staff) >= score->nstaves())
        return invalid(std::format("the score has no staff {}", at->staff));
    const Staff* staff = score->staff(at->staff);
    if (staff->isTabStaff(segment->tick()) || staff->isDrumStaff(segment->tick()))
        return invalid("EnterNotes enters notes on a pitched staff, not on a tablature or a drum set");

    return [target = *target, at = *at, duration = *duration, pitches = op.pitches](EditContext& context) -> Result<void> {
        auto object = context.object(target);
        if (!object) return std::unexpected(object.error());
        Segment* start = mu::engraving::toSegment(*object);
        Score* score = start->score();
        // Note entry replaces what holds the position: an empty segment goes and comes
        // back as another one, so the segment is looked up again by its tick.
        const mu::engraving::Fraction tick = start->tick();
        const mu::engraving::track_idx_t track = mu::engraving::staff2track(at.staff, at.voice);
        Transaction& tx = score->transactionManager()->currentOrDummyTransaction();
        // The score's own input state is its cursor: the API brings its own, so note
        // entry moves nothing of the score's outside the transaction.
        InputState input;
        NoteVal first = pitches.empty() ? NoteVal() : note_value(pitches.front());
        if (!score->setNoteRest(start, track, first, duration, DirectionV::AUTO, false, {}, false, &input))
            return std::unexpected(Error{ErrorCode::EditRejected, "the engine could not enter a note at this position"});
        // The first piece of the entry sits where the address points; the rest of a
        // duration that reached past a barline is tied to it.
        Segment* entered_at = score->tick2segment(tick, true, mu::engraving::SegmentType::ChordRest);
        EngravingItem* entered = entered_at ? entered_at->element(track) : nullptr;
        if (!entered || !entered->isChordRest())
            return std::unexpected(Error{ErrorCode::EditRejected, "the engine left no chord or rest at this position"});
        context.created(entered);
        if (!entered->isChord()) return {};
        Chord* chord = mu::engraving::toChord(entered);
        for (Note* note : chord->notes()) context.created(note);
        // The engine's own note input adds a pitch to a tied chord by following the tie
        // chain, which the API does not reach: NoteInput::addNoteToTiedChord has no input
        // state of its own and works through the score's cursor and selection. A chord of
        // several pitches is entered whole, so its entry has to end in its own bar.
        if (pitches.size() > 1 && !chord->notes().empty()
            && (chord->notes().front()->tieFor() || chord->notes().front()->tieBack())) {
            return invalid("a chord of several pitches must end in the bar its entry starts in");
        }
        for (std::size_t i = 1; i < pitches.size(); ++i) {
            NoteVal value = note_value(pitches[i]);
            if (!mu::engraving::NoteInput::addNote(tx, score, chord, value, false, {}, &input))
                return std::unexpected(
                    Error{ErrorCode::EditRejected, std::format("the engine could not add pitch {}", pitches[i].pitch)});
        }
        return {};
    };
}

Result<Step> prepare(EditContext& context, const SetPitch& op) {
    if (!mu::engraving::pitchIsValid(op.pitch)) return invalid(std::format("{} is not a pitch", op.pitch));
    if (op.tpc && !mu::engraving::tpcIsValid(*op.tpc))
        return invalid(std::format("{} is not a tonal pitch class", *op.tpc));
    auto target = context.target(op.note);
    if (!target) return std::unexpected(target.error());
    return [target = *target, pitch = op.pitch, tpc = op.tpc](EditContext& context) -> Result<void> {
        auto item = edited_item(context, target);
        if (!item) return std::unexpected(item.error());
        if (!(*item)->isNote()) return invalid("SetPitch sets the pitch of a note");
        Note* note = mu::engraving::toNote(*item);
        if (const auto why = unchangeable(*note)) return invalid(std::format("the note keeps its pitch: {}", *why));
        const auto [tpc1, tpc2] = spell(*note, pitch, tpc);
        if (!mu::engraving::tpcIsValid(tpc1) || !mu::engraving::tpcIsValid(tpc2))
            return invalid(std::format("the pitch cannot be spelled at this position as {}", pitch));
        mu::engraving::EditNote::undoChangePitch(note->score(), note, pitch, tpc1, tpc2);
        return {};
    };
}

Result<Step> prepare(EditContext& context, const SetDuration& op) {
    const auto duration = engine_fraction(op.duration);
    if (!duration) return invalid("the duration must be a positive fraction of a whole note");
    auto target = context.target(op.chord_rest);
    if (!target) return std::unexpected(target.error());
    return [target = *target, duration = *duration](EditContext& context) -> Result<void> {
        auto item = edited_item(context, target);
        if (!item) return std::unexpected(item.error());
        if (!(*item)->isChordRest()) return invalid("SetDuration changes the length of a chord or of a rest");
        ChordRest* chord_rest = mu::engraving::toChordRest(*item);
        Score* score = chord_rest->score();
        // changeCRlen works through the selection: it drops it first and selects what it
        // changed at the end, and the selection moves the score's input cursor with it.
        // The edit API hands a score back without a selection, so the step drops the one
        // the engine made and puts the cursor back; C2 of the harness keeps this honest.
        // A command that fails throws out of changeCRlen before it selects, so a rollback
        // never has a selection to undo.
        const mu::engraving::track_idx_t cursor = score->inputState().track();
        score->changeCRlen(chord_rest, duration);
        if (!score->selection().isNone()) {
            score->deselectAll();
            score->inputState().setTrack(cursor);
        }
        return {};
    };
}

Result<Step> prepare(EditContext& context, const Remove& op) {
    auto target = context.target(op.target);
    if (!target) return std::unexpected(target.error());
    return [target = *target](EditContext& context) -> Result<void> {
        auto item = edited_item(context, target);
        if (!item) return std::unexpected(item.error());
        EngravingItem* target_item = *item;
        Score* score = target_item->score();
        if (target_item->isNote()) {
            // deleteItem's note case: one note of a chord goes alone; the last one goes
            // with its chord, which deleteItem replaces with a rest of the same length.
            // Only the selection it makes of the remaining down note is left out.
            Note* note = mu::engraving::toNote(target_item);
            Chord* chord = note->chord();
            if (chord && chord->notes().size() > 1) {
                score->undoRemoveElement(note);
                return {};
            }
            score->deleteItem(chord);
            return {};
        }
        score->deleteItem(target_item);
        return {};
    };
}

} // namespace engrave::api
