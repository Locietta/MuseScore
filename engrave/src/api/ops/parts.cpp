// AddPart, RemovePart, AddStaff, RemoveStaff and GenerateExcerpts: the engine's
// undoable part, staff and excerpt commands, driven the way the parts panel
// drives them.

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "engraving/dom/excerpt.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftype.h"
#include "engraving/editing/editexcerpt.h"
#include "engraving/editing/editpart.h"

#include "engrave/src/api/batch.h"

namespace engrave::api {
namespace {

using mu::engraving::Excerpt;
using mu::engraving::ID;
using mu::engraving::MasterScore;
using mu::engraving::Part;
using mu::engraving::Score;
using mu::engraving::Staff;

std::unexpected<Error> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
}

Result<Part*> part_at(EditContext& context, const Target& target) {
    auto object = context.object(target);
    if (!object) return std::unexpected(object.error());
    if (!(*object)->isPart()) return invalid(std::format("{} is not a part", (*object)->typeName()));
    return mu::engraving::toPart(*object);
}

Result<Staff*> staff_at(EditContext& context, const Target& target) {
    auto object = context.object(target);
    if (!object) return std::unexpected(object.error());
    if (!(*object)->isStaff()) return invalid(std::format("{} is not a staff", (*object)->typeName()));
    return mu::engraving::toStaff(*object);
}

// The part of a score that carries this id: an excerpt's copy of a master part.
Part* copy_of(Score& score, const ID& id) {
    for (Part* part : score.parts()) {
        if (part->id() == id) return part;
    }
    return nullptr;
}

// The excerpts that show a part, and those that exist for it: an excerpt with an
// initial part goes when that part goes, one that shows it as a guest loses it.
struct Excerpts {
    std::vector<Excerpt*> initial;
    std::vector<Excerpt*> guests;

    Excerpts(MasterScore& master, const ID& id) {
        for (Excerpt* excerpt : master.excerpts()) {
            (excerpt->initialPartId() == id ? initial : guests).push_back(excerpt);
        }
    }
};

} // namespace

Result<Step> prepare(EditContext& context, const AddPart& op) {
    const mu::engraving::InstrumentIndex found =
        mu::engraving::searchTemplateIndexForId(muse::String::fromStdString(op.instrument_id));
    if (!found.instrTemplate) return invalid(std::format("no instrument '{}'", op.instrument_id));
    std::optional<Target> before;
    if (op.before) {
        auto target = context.target(*op.before);
        if (!target) return std::unexpected(target.error());
        before = *target;
    }
    const mu::engraving::InstrumentTemplate* templ = found.instrTemplate;
    return [templ, before](EditContext& context) -> Result<void> {
        MasterScore& master = context.master();
        std::size_t at = master.parts().size();
        if (before) {
            auto part = part_at(context, *before);
            if (!part) return std::unexpected(part.error());
            if ((*part)->score() != &master) return invalid("only a part of the master score is ordered here");
            at = static_cast<std::size_t>(std::ranges::find(master.parts(), *part) - master.parts().begin());
        }
        Part* part = new Part(&master);
        part->initFromInstrTemplate(templ);
        master.undoInsertPart(part, at);
        context.created(part);
        // The staves follow their part: the engine's own helper inserts them first,
        // when the part is not in the score yet, which lands them at the end.
        for (std::size_t i = 0; i < templ->staffCount; ++i) {
            Staff* staff = mu::engraving::Factory::createStaff(part);
            mu::engraving::StaffType* type = staff->staffType(mu::engraving::Fraction(0, 1));
            master.undoInsertStaff(staff, i);
            staff->init(templ, type, i);
            context.created(staff);
        }
        return {};
    };
}

Result<Step> prepare(EditContext& context, const RemovePart& op) {
    auto target = context.target(op.part);
    if (!target) return std::unexpected(target.error());
    return [target = *target](EditContext& context) -> Result<void> {
        auto found = part_at(context, target);
        if (!found) return std::unexpected(found.error());
        Part* part = *found;
        Score* score = part->score();
        // An excerpt's own part is a projection of a part of the master score. The editor's
        // parts panel removes them together, never one alone: an excerpt whose part was
        // removed on its own does not get its music back on undo.
        if (!score->isMaster()) return invalid("a part of an excerpt goes with its part of the master score");
        // A score with no part has no staves and no instruments, and the writer and the
        // reader do not keep it: the reopened score differs. So the last part stays.
        if (score->parts().size() <= 1) return invalid("a score keeps at least one part");
        MasterScore& master = context.master();
        const ID id = part->id();
        const Excerpts excerpts(master, id);
        mu::engraving::EditPart::removeParts(&master, {part});
        for (Excerpt* excerpt : excerpts.initial) master.undo(new mu::engraving::RemoveExcerpt(excerpt));
        for (Excerpt* excerpt : excerpts.guests) {
            Score* excerptScore = excerpt->excerptScore();
            Part* copy = copy_of(*excerptScore, id);
            if (!copy) continue;
            mu::engraving::EditPart::removeParts(excerptScore, {copy});
            if (excerpt->parts().empty()) master.undo(new mu::engraving::RemoveExcerpt(excerpt));
        }
        return {};
    };
}

Result<Step> prepare(EditContext& context, const AddStaff& op) {
    auto target = context.target(op.part);
    if (!target) return std::unexpected(target.error());
    return [target = *target](EditContext& context) -> Result<void> {
        auto found = part_at(context, target);
        if (!found) return std::unexpected(found.error());
        Part* part = *found;
        Score* score = part->score();
        if (score->isMaster()) {
            // The linked staff an excerpt gets does not survive a save and a reopen: the
            // excerpt's part is written with one staff more than the file's part holds, and
            // the read-back differs. The editor adds the staff to every excerpt in turn;
            // until that round trip is fixed, a part an excerpt shows is refused.
            for (Excerpt* excerpt : context.master().excerpts()) {
                if (copy_of(*excerpt->excerptScore(), part->id()))
                    return invalid("an excerpt shows this part: a staff added to it does not survive a save");
            }
        }
        Staff* staff = mu::engraving::EditPart::appendStaff(score, part);
        if (!staff) return invalid("the engine added no staff");
        context.created(staff);
        return {};
    };
}

Result<Step> prepare(EditContext& context, const RemoveStaff& op) {
    auto target = context.target(op.staff);
    if (!target) return std::unexpected(target.error());
    return [target = *target](EditContext& context) -> Result<void> {
        auto found = staff_at(context, target);
        if (!found) return std::unexpected(found.error());
        Staff* staff = *found;
        Score* score = staff->score();
        if (staff->part()->nstaves() <= 1) return invalid("an instrument's last staff goes with the instrument");
        const ID id = staff->id();
        mu::engraving::EditPart::removeStaves(score, {staff});
        if (!score->isMaster()) return {};
        // The excerpts link to this staff: the copies go with it.
        for (Excerpt* excerpt : context.master().excerpts()) {
            if (Staff* copy = excerpt->excerptScore()->staffById(id))
                mu::engraving::EditPart::removeStaves(excerpt->excerptScore(), {copy});
        }
        return {};
    };
}

Result<Step> prepare(EditContext& context, const GenerateExcerpts& op) {
    if (op.parts.empty()) return invalid("no parts named");
    std::vector<Target> targets;
    targets.reserve(op.parts.size());
    for (const Ref& ref : op.parts) {
        auto target = context.target(ref);
        if (!target) return std::unexpected(target.error());
        targets.push_back(*target);
    }
    // The excerpt is built by the engine's own excerpt commands, inside the batch's
    // transaction: the links, the cloned elements and the inserted frames and measures are
    // all undo steps of this batch. The excerpt itself has to be added by a command as well
    // (initAndAddExcerpt records an AddExcerpt), or undoing the batch takes the elements the
    // build added out of the excerpt and leaves the excerpt in the master, with freed
    // elements in its score. The build cannot be unwound (see the policy row), so a failed
    // batch is restored from its snapshot rather than unwound.
    return [targets](EditContext& context) -> Result<void> {
        MasterScore& master = context.master();
        for (const Target& target : targets) {
            auto found = part_at(context, target);
            if (!found) return std::unexpected(found.error());
            Part* part = *found;
            if (part->score() != &master) return invalid("an excerpt is generated from a part of the master score");
            Excerpt* excerpt = Excerpt::createExcerptsFromParts({part}, &master).front();
            master.initAndAddExcerpt(excerpt, false);
            context.created(excerpt->excerptScore());
        }
        return {};
    };
}

} // namespace engrave::api
