// SPDX-License-Identifier: GPL-3.0-only
// Native DOM pitch editing for the persistent engraving score.
#pragma once

#include <cstddef>
#include <string>
#include <stdexcept>
#include <vector>

namespace mu::engraving {
class MasterScore;
}

namespace music {

// A conservative dependency footprint in standard score measure order. When
// whole_part is false, first_measure/measure_count identify the affected full
// measures (all staves). When true the caller must treat the entire part as
// affected because a linked note lives in another master region (for example
// an excerpt) whose measure coordinates cannot be represented here.
struct ScoreEditScope
{
    size_t part_index = 0;
    size_t first_measure = 0;
    size_t measure_count = 0;
    bool whole_part = false;
};

struct PitchChange
{
    std::string note_id;
    int pitch = 0;
};

struct NativePitchEdit
{
    std::vector<PitchChange> changes;
};

struct NativePitchResult
{
    // Requested notes whose sounding MIDI pitch actually changed. Linked notes
    // may also have changed; they are covered by read_scopes/write_scopes.
    size_t changed_notes = 0;
    std::vector<ScoreEditScope> read_scopes;
    std::vector<ScoreEditScope> write_scopes;
    bool requires_stable_structure = true;
};

class NativeEditFailure : public std::runtime_error {
public:
    NativeEditFailure(const std::string& message, bool poisoned)
        : std::runtime_error(message), poisoned_(poisoned) {}
    bool poisoned() const noexcept { return poisoned_; }
private:
    bool poisoned_;
};

// Validates and computes scopes without changing pitch or undo history. Caller
// must inspect, check revisions, then apply on one owner-thread turn without yielding.
NativePitchResult inspect_score_pitch_edit(mu::engraving::MasterScore*, const NativePitchEdit&);
// Best-effort retirement after publication; never turns a committed edit into failure.
bool cap_native_edit_history(mu::engraving::MasterScore*, size_t maximum = 128) noexcept;

#ifdef MUSIC_CORE_TESTING
enum class NativeEditTestFault { none, engine_error_after_first, exception_after_first };
void set_native_edit_test_fault(NativeEditTestFault);
#endif

// Applies a single bounded undoable command. Every requested note is validated
// before the command starts; ordinary validation failures leave no partial edit.
// Unexpected transaction/layout exceptions throw NativeEditFailure(poisoned=true);
// callers must retire the document rather than assume layout rollback is safe. Notes with ties, non-tie spanners, tablature/drum staves,
// cross-staff displacement or grace-note chords are rejected with
// std::runtime_error. Bad IDs, duplicate IDs, out-of-range pitches, empty or
// larger-than-256 batches and a null score throw std::invalid_argument.
// Read-only scores and locked or already-active undo stacks are rejected.
// Requires execution on the master score's owner thread.
NativePitchResult apply_score_pitch_edit(mu::engraving::MasterScore* score, const NativePitchEdit& edit);

} // namespace music
