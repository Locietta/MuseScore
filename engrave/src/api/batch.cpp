#include "engrave/src/api/batch.h"

#include <algorithm>
#include <format>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

#include "engraving/dom/engravingobject.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/score.h"
#include "engraving/editing/transaction/undoablecommand.h"
#include "engraving/editing/transaction/undostack.h"
#include "engraving/engravingproject.h"

#include "engrave/src/api/handles.h"
#include "engrave/src/api/state.h"
#include "engrave/src/runtime/noqt_runtime.h"
#include "engrave/src/score/score_semantic.h"

namespace engrave {

Result<EditResult> Document::edit(const Batch& batch, std::optional<std::uint64_t> expected_revision) {
    impl_->engine->check_thread();
    return api::BatchGuard(*this).edit(batch, expected_revision);
}

namespace api {
namespace {

using mu::engraving::CommandType;
using mu::engraving::EngravingObject;
using mu::engraving::MasterScore;
using mu::engraving::MScore;
using mu::engraving::MsError;
using mu::engraving::UndoableCommand;
using mu::engraving::UndoStack;

std::unexpected<Error> fail(ErrorCode code, std::string message, std::optional<std::size_t> op = {}) {
    return std::unexpected(Error{code, std::move(message), op});
}

// A command that may have added, removed or moved elements, so that a reference
// resolved before it ran has to be resolved again.
bool structural(const UndoableCommand* command) {
    switch (command->type()) {
    case CommandType::ChangeProperty:
    case CommandType::ChangeStyle:
    case CommandType::ChangeStyleValues:
        return false;
    default:
        return true;
    }
}

// The ids of the document, as the harness's fingerprint writes them: the writer's
// order (the spanner map, the excerpt list) is not the order an undo puts them back
// in, so a rollback compares them sorted.
std::vector<std::string> id_list(MasterScore& master) {
    std::vector<std::string> ids;
    for (const LiveId& entry : live_ids(master)) {
        ids.push_back(std::format("{} {} {}/{}/{}", to_string(entry.id), entry.object->typeName(), entry.placement.score,
                                  entry.placement.tick, entry.placement.track));
    }
    std::ranges::sort(ids);
    return ids;
}

bool engine_failed() {
    return MScore::_error != MsError::MS_NO_ERROR && !MScore::_errorIsWarning;
}

bool engine_warned() {
    return MScore::_error != MsError::MS_NO_ERROR && MScore::_errorIsWarning;
}

} // namespace

std::string_view to_string(Policy policy) noexcept {
    switch (policy) {
    case Policy::Transaction: return "Transaction";
    case Policy::Snapshot: return "Snapshot";
    case Policy::Unexposed: return "Unexposed";
    }
    return "Unknown";
}

std::string_view op_name(const Op& op) {
    return std::visit([](const auto& value) { return OpTraits<std::decay_t<decltype(value)>>::name; }, op);
}

Policy policy_of(const Op& op) {
    return std::visit([](const auto& value) { return OpTraits<std::decay_t<decltype(value)>>::policy; }, op);
}

// --- EditContext ------------------------------------------------------------

Result<Target> EditContext::target(const Ref& ref) const {
    if (const auto* made = std::get_if<OpRef>(&ref)) {
        if (made->op >= m_op) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("an OpRef names operation {}, which does not run before this one", made->op));
        }
        return Target{ref, nullptr, m_epoch};
    }
    const Result<Resolved> found = std::holds_alternative<ElementId>(ref) ? resolve(m_master, std::get<ElementId>(ref))
                                                                          : resolve(m_master, std::get<Address>(ref));
    if (!found) return std::unexpected(found.error());
    if (!found->writable) return fail(ErrorCode::InvalidArgument, "layout builds this element; it cannot be edited");
    return Target{ref, found->object, m_epoch};
}

Result<EngravingObject*> EditContext::object(const Target& target) const {
    if (const auto* made = std::get_if<OpRef>(&target.ref)) {
        const auto& list = m_created[made->op];
        if (made->item >= list.size()) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("operation {} created {} element(s), not {}", made->op, list.size(), made->item + 1));
        }
        EngravingObject* object = list[made->item];
        const auto objects = document_objects(m_master);
        if (std::ranges::find(objects, object) == objects.end()) {
            return fail(ErrorCode::StaleHandle,
                        std::format("element {} of operation {} is no longer part of the score", made->item, made->op));
        }
        return object;
    }
    if (target.epoch == m_epoch) return target.object;
    const Result<Target> again = this->target(target.ref);
    if (!again) return std::unexpected(again.error());
    return again->object;
}

Result<mu::engraving::Score*> EditContext::score(const ScoreSel& selection) const {
    if (!selection.excerpt) return &m_master;
    const Result<Resolved> found = resolve(m_master, *selection.excerpt);
    if (!found) return std::unexpected(found.error());
    if (!found->object->isScore()) {
        return fail(ErrorCode::InvalidArgument, std::format("{} is not a score", to_string(*selection.excerpt)));
    }
    return mu::engraving::toScore(found->object);
}

void EditContext::created(EngravingObject* object) {
    m_created[m_op].push_back(object);
}

// --- BatchGuard -------------------------------------------------------------

// The score as an in-memory MSCZ, with the version stamps the writer replaces, per
// score: the master first, then the excerpts. The semantic dump and the ids are what
// a rollback compares against, since the writer's order is not the undo's.
struct BatchGuard::Snapshot {
    struct Versions {
        muse::String version;
        int revision = 0;
        int msc_version = 0;
        bool laid_out = false;
    };
    std::string mscz;
    std::string semantic;
    std::vector<std::string> ids;
    std::vector<Versions> versions;
};

BatchGuard::BatchGuard(Document& document)
    : m_document(document), m_context(*document.impl_->project->masterScore()) {}

Result<void> BatchGuard::admit(std::optional<std::uint64_t> expected_revision) const {
    const Document::Impl& document = *m_document.impl_;
    if (document.poisoned) return fail(ErrorCode::Poisoned, "an earlier edit left the document unusable");
    if (expected_revision && *expected_revision != document.revision) {
        return fail(ErrorCode::RevisionConflict,
                    std::format("the document is at revision {}, not {}", document.revision, *expected_revision));
    }
    const MasterScore& master = m_context.master();
    const UndoStack* stack = master.undoStack();
    if (master.readOnly() || stack->isLocked() || stack->hasActiveTransaction())
        return fail(ErrorCode::EditRejected, "the score cannot be edited now");
    return {};
}

Result<EditResult> BatchGuard::edit(const Batch& batch, std::optional<std::uint64_t> expected_revision) {
    if (const auto admitted = admit(expected_revision); !admitted) return std::unexpected(admitted.error());
    m_context.m_created.assign(batch.ops.size(), {});
    Policy policy = Policy::Transaction;
    std::vector<Step> steps;
    std::vector<bool> snapshot_step;
    steps.reserve(batch.ops.size());
    snapshot_step.reserve(batch.ops.size());
    for (std::size_t i = 0; i < batch.ops.size(); ++i) {
        const Op& op = batch.ops[i];
        m_context.m_op = i;
        const Policy own = policy_of(op);
        if (own == Policy::Unexposed) return fail(ErrorCode::Unsupported, std::format("{} is not available", op_name(op)), i);
        if (own == Policy::Snapshot) policy = Policy::Snapshot;
        snapshot_step.push_back(own == Policy::Snapshot);
        auto step = std::visit([this](const auto& value) { return prepare(m_context, value); }, op);
        if (!step) {
            Error error = std::move(step.error());
            error.op_index = i;
            return std::unexpected(std::move(error));
        }
        steps.push_back(std::move(*step));
    }
    return execute(std::move(steps), policy, std::move(snapshot_step));
}

Result<EditResult> BatchGuard::run(std::vector<Step> steps, Policy policy, std::optional<std::uint64_t> expected_revision) {
    if (const auto admitted = admit(expected_revision); !admitted) return std::unexpected(admitted.error());
    m_context.m_created.assign(steps.size(), {});
    return execute(std::move(steps), policy, {});
}

Result<EditResult> BatchGuard::execute(std::vector<Step> steps, Policy policy, std::vector<bool> snapshot_step) {
    Document::Impl& document = *m_document.impl_;
    EditResult result{.revision = document.revision};
    if (steps.empty()) return result;
    const EditHooks& hooks = document.edit_hooks;
    if (hooks.policy) {
        policy = *hooks.policy;
        // A forced policy applies to every step, and the guard cannot tell which of
        // them the unwind is safe for.
        std::ranges::fill(snapshot_step, policy == Policy::Snapshot);
    }
    // Without per-step policies (run(), whose caller brings its own steps), a snapshot
    // policy says nothing about which step is safe to unwind.
    bool snapshot_ran = snapshot_step.empty() && policy == Policy::Snapshot;

    MasterScore& master = m_context.master();
    UndoStack* stack = master.undoStack();
    std::optional<Snapshot> snapshot;
    if (policy == Policy::Snapshot) {
        snapshot = Snapshot{music::export_native_score(*document.engine->context, document.project, "mscz", music::ExportOptions{}, {}),
                            music::semantic_dump(&master).dump(1), id_list(master), {}};
        for (const mu::engraving::Score* score : master.scoreList())
            snapshot->versions.push_back({score->mscoreVersion(), score->mscoreRevision(), score->mscVersion(), score->npages() > 0});
    }
    const std::vector<LiveId> before = live_ids(master);
    const std::size_t history = stack->size();

    // An engine error that a hook or a step leaves behind rejects the batch at that point.
    std::optional<Error> failure;
    bool engine_threw = false;
    const auto check_engine = [&failure, &result, &steps](std::size_t step) {
        if (engine_failed()) {
            failure = Error{ErrorCode::EditRejected, std::format("the engine refused it: {}", MScore::errorToString(MScore::_error)),
                            step < steps.size() ? std::optional(step) : std::nullopt};
        } else if (engine_warned()) {
            result.diagnostics.push_back({Severity::Warning, MScore::errorToString(MScore::_error)});
        }
        MScore::setError(MsError::MS_NO_ERROR);
        return !failure;
    };
    try {
        MScore::setError(MsError::MS_NO_ERROR);
        master.startCmd(muse::TranslatableString::untranslatable("engrave edit"));
        if (!stack->hasActiveTransaction()) return fail(ErrorCode::EditRejected, "the engine did not start a transaction");
        for (std::size_t i = 0; i <= steps.size(); ++i) {
            m_context.m_op = i;
            if (hooks.before_step) hooks.before_step(i);
            if (!check_engine(i) || i == steps.size()) break;
            if (i < snapshot_step.size() && snapshot_step[i]) snapshot_ran = true;
            const std::size_t mark = stack->activeTransaction()->commands().size();
            if (auto applied = steps[i](m_context); !applied) {
                failure = std::move(applied.error());
                failure->op_index = i;
                break;
            }
            if (!check_engine(i)) break;
            const auto& commands = stack->activeTransaction()->commands();
            if (std::any_of(commands.begin() + mark, commands.end(), structural)) ++m_context.m_epoch;
        }
        if (!failure) master.endCmd(false, true);
    } catch (const std::bad_alloc&) {
        roll_back(snapshot ? &*snapshot : nullptr, history, true);
        throw;
    } catch (const std::exception& error) {
        const std::size_t op = m_context.m_op;
        engine_threw = true;
        failure = Error{ErrorCode::EditRejected, std::format("the engine failed: {}", error.what()),
                        op < steps.size() ? std::optional(op) : std::nullopt};
    } catch (...) {
        roll_back(snapshot ? &*snapshot : nullptr, history, true);
        throw;
    }
    if (failure) {
        roll_back(snapshot ? &*snapshot : nullptr, history, engine_threw || snapshot_ran);
        return std::unexpected(std::move(*failure));
    }

    // What the operations created, while a removal is still undoable and has not
    // destroyed anything: the history cleanup below deletes what the batch removed.
    std::unordered_set<const EngravingObject*> objects;
    for (EngravingObject* object : document_objects(master)) objects.insert(object);
    for (auto& list : m_context.m_created) {
        for (EngravingObject*& object : list) {
            if (!objects.contains(object)) object = nullptr;
        }
    }
    const bool changed = stack->size() > history;
    if (!hooks.keep_history) stack->clearHistory();
    assign_missing_ids(master);
    IdDiff diff = diff_ids(before, live_ids(master));
    if (changed) ++document.revision;

    result.revision = document.revision;
    result.created = std::move(diff.created);
    result.removed = std::move(diff.removed);
    result.replaced = std::move(diff.replaced);
    result.ops.resize(steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        for (const EngravingObject* object : m_context.m_created[i]) {
            const bool named = object && takes_id(*object) && object->eid().isValid();
            result.ops[i].created.push_back(named ? std::optional(from_engine(object->eid())) : std::nullopt);
        }
    }
    return result;
}

// Brings the score back to where it was before the batch. A failure here leaves the
// document poisoned and is thrown.
void BatchGuard::roll_back(const Snapshot* snapshot, std::size_t history, bool unwind_unsafe) {
    Document::Impl& document = *m_document.impl_;
    MasterScore& master = m_context.master();
    UndoStack* stack = master.undoStack();
    try {
        if (snapshot) {
            // The transaction's unwind keeps the layout the score had; a reload of the
            // snapshot does not. A score written from a 3.x file reads back with the
            // current reader and lays out a few units differently (S1 of the harness
            // records that, and Edit.AFailedMixedBatchKeepsTheLayout pins it), so the
            // unwind is taken whenever it can be trusted to bring the score back
            // exactly: the engine did not throw, and no operation whose rollback policy
            // is Snapshot has run. When it cannot, the snapshot is the way back.
            const bool active = stack->hasActiveTransaction();
            if (active && !unwind_unsafe) {
                // The undo's own layout is the one the score had, so it is kept: a
                // relayout would read the score back through the layout code that the
                // snapshot's reload uses and move the elements the same way.
                master.endCmd(true, true);
                if (!same_score(*snapshot)) restore(*snapshot);
            } else {
                // The project is replaced rather than unwound: an operation whose rollback
                // policy is Snapshot cannot be undone (the excerpt build frees its links
                // twice), and after a failure in the middle of a command the undo is not
                // to be trusted either.
                if (active) stack->endTransaction(true);
                restore(*snapshot);
            }
        } else if (stack->hasActiveTransaction()) {
            master.endCmd(true, true);
        } else if (stack->size() > history) {
            // The commit itself failed after the transaction was recorded: undo it,
            // which C2 of the harness proves equal to the unwind.
            master.undoRedo(true, nullptr);
            stack->clearHistory();
            // The engine's undo lays out the master score only.
            for (mu::engraving::Score* score : master.scoreList()) {
                if (score->npages() > 0) score->doLayout();
            }
        }
    } catch (...) {
        document.poisoned = true;
        throw;
    }
    MScore::setError(MsError::MS_NO_ERROR);
}

void BatchGuard::restore(const Snapshot& snapshot) {
    Document::Impl& document = *m_document.impl_;
    if (document.edit_hooks.fail_restore) throw std::runtime_error("the snapshot restore failed (test hook)");
    music::ScoreProject project = music::load_native_score(snapshot.mscz, "mscz");
    // The reload reads the version the writer stamped; the scores keep their own, and
    // layout depends on it (a score from an older version keeps its compatibility
    // layout), so what was laid out is laid out again, as a load does.
    mu::engraving::ScoreLoad load;
    auto versions = snapshot.versions.begin();
    for (mu::engraving::Score* score : project->masterScore()->scoreList()) {
        if (versions == snapshot.versions.end()) break;
        score->setMscoreVersion(versions->version);
        score->setMscoreRevision(versions->revision);
        score->setMscVersion(versions->msc_version);
        if (versions->laid_out) score->doLayout();
        ++versions;
    }
    assign_missing_ids(*project->masterScore());
    document.project = std::move(project);
}

// Whether the undo left the document where the batch found it: the same musical
// content and the same ids. A serialized comparison would be stricter than the undo
// can promise (the writer orders spanners and excerpts its own way), and a restore is
// what a difference costs, so the harness's own notion of the score's content is used.
bool BatchGuard::same_score(const Snapshot& snapshot) {
    MasterScore& master = m_context.master();
    return music::semantic_dump(&master).dump(1) == snapshot.semantic && id_list(master) == snapshot.ids;
}

} // namespace api
} // namespace engrave
