// The edit batch: what an operation sees while it is prepared and run, the
// policy table, and the guard that runs a batch as one transaction (design §5).
//
// An operation is prepared before the batch touches the score: it resolves its
// references and checks its arguments, then returns the step that applies it.
// The guard runs the steps in one transaction and commits, or restores the score
// it had before.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <engrave/document.h>
#include <engrave/edit.h>
#include <engrave/error.h>

namespace mu::engraving {
class EngravingObject;
class MasterScore;
class Score;
}

namespace engrave::api {

// How a batch that contains the operation is rolled back.
enum struct Policy {
    Transaction, // the transaction's unwind restores the score (proven by the harness)
    Snapshot,    // the score is restored from an in-memory MSCZ written before the batch
    Unexposed,   // edit() refuses the operation
};

std::string_view to_string(Policy policy) noexcept;

// A reference as far as it resolves before the batch runs: the element an id or an
// address names, or an OpRef to what an earlier operation will create.
struct Target {
    Ref ref;
    mu::engraving::EngravingObject* object = nullptr; // null for an OpRef
    std::size_t epoch = 0;
};

class BatchGuard;

class EditContext {
public:
    explicit EditContext(mu::engraving::MasterScore& master) : m_master(master) {}

    mu::engraving::MasterScore& master() const { return m_master; }
    // The operation being prepared or run.
    std::size_t op() const { return m_op; }

    // While preparing: NotFound or StaleHandle for an id or address that names
    // nothing live, InvalidArgument for an OpRef that does not name an earlier
    // operation.
    Result<Target> target(const Ref& ref) const;
    // While running: the live element. A target is resolved again when an earlier
    // operation changed more than properties, since it may have removed it.
    Result<mu::engraving::EngravingObject*> object(const Target& target) const;
    // The master score or the excerpt a selection names.
    Result<mu::engraving::Score*> score(const ScoreSel& selection) const;

    // Records an element the running operation created, for OpRef and OpResult.
    void created(mu::engraving::EngravingObject* object);

private:
    friend class BatchGuard;

    mu::engraving::MasterScore& m_master;
    std::size_t m_op = 0;
    std::size_t m_epoch = 0; // advanced by every operation that changed more than properties
    std::vector<std::vector<mu::engraving::EngravingObject*>> m_created;
};

// Applies a prepared operation. An error it returns rolls the batch back.
using Step = std::function<Result<void>(EditContext&)>;

// The policy table: one row per operation, and an operation without a row does
// not compile. Each operation's prepare() is defined in its group's file under
// ops/.
template <typename T>
struct OpTraits;

#define ENGRAVE_OP(op_, policy_)                                                                                    \
    template <>                                                                                                     \
    struct OpTraits<op_> {                                                                                          \
        static constexpr std::string_view name = #op_;                                                              \
        static constexpr Policy policy = Policy::policy_;                                                           \
    };                                                                                                              \
    Result<Step> prepare(EditContext& context, const op_& op);
#include "engrave/src/api/op_policy.inc"
#undef ENGRAVE_OP

std::string_view op_name(const Op& op);
Policy policy_of(const Op& op);

// Test seams of the guard; a Document never sets them.
struct EditHooks {
    std::optional<Policy> policy;  // run every batch under this policy
    bool keep_history = false;     // leave the committed transaction on the undo stack
    bool fail_restore = false;     // make the snapshot restore fail
    // Called before each step and once more before the commit (with the step
    // count); it may throw, or set an engine error that rejects the batch.
    std::function<void(std::size_t step)> before_step;
};

// Runs a batch as one transaction. edit() is Document::edit; run() takes steps
// that are already prepared, for the library's tests that bring their own.
class BatchGuard {
public:
    explicit BatchGuard(Document& document);

    Result<EditResult> edit(const Batch& batch, std::optional<std::uint64_t> expected_revision);
    Result<EditResult> run(std::vector<Step> steps, Policy policy, std::optional<std::uint64_t> expected_revision);

private:
    struct Snapshot;

    // RevisionConflict, EditRejected (the undo stack is busy) or Poisoned, before
    // anything is touched.
    Result<void> admit(std::optional<std::uint64_t> expected_revision) const;
    Result<EditResult> execute(std::vector<Step> steps, Policy policy, std::vector<bool> snapshot_step);
    void roll_back(const Snapshot* snapshot, std::size_t history, bool unwind_unsafe);
    // Whether the undo left the document where the batch found it.
    bool same_score(const Snapshot& snapshot);
    void restore(const Snapshot& snapshot);

    Document& m_document;
    EditContext m_context;
};

} // namespace engrave::api
