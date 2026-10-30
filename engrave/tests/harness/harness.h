// The per-operation evidence harness (design §5.3).
//
// An OpCase names a fixture and builds one batch against it. run_case() runs the
// batch on fresh opens of the fixture and decides how a batch with the operation
// may be rolled back:
//   C1  commit: the case's expectation holds, the revision advances, the history
//       is clear, the register is a bijection, and a save and reopen keeps the
//       content and the ids;
//   C2  commit, then undo: the score is back where it was, so the operation
//       changes nothing behind the undo stack's back;
//   C3  a fault at every command push (before it is appended, after it ran) and
//       between the operations: the rollback restores every fingerprint field;
//   C4  std::bad_alloc at every push: thrown through, and the score restored
//       (with the snapshot policy, a restore that fails poisons the document);
//   R4  after every rollback the undo stack is empty and inactive.
// C3 and C4 run under both rollback policies. The verdict is Transaction when C1
// to C4 hold with the unwind, Snapshot when C1 holds and the snapshot path
// restores the score, and Unexposed otherwise.
//
// A group of operations registers its cases from its own file under cases/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <engrave/engrave.h>

#include "engrave/src/api/batch.h"

namespace mu::engraving {
class EngravingObject;
class MasterScore;
}

namespace engrave::harness {

// Everything about a document that an edit and its rollback can change.
struct Fingerprint {
    std::map<std::string, std::string> files; // every file of an in-memory MSCZ: scores, excerpts, styles
    std::string semantic;                     // the semantic dump (no layout, no ids)
    std::vector<std::string> ids;             // "<id> <type> <score>/<tick>/<track>", sorted
    std::uint64_t revision = 0;
    std::size_t undo_count = 0;
    bool undo_active = false;
    int engine_error = 0;
    std::vector<std::string> layout;          // per laid-out score: its pages and segment positions,
                                              // equal to within a unit of 1/1200 inch
    std::string externals;                    // MIDI mapping, repeats, meta tags, selection, input state
    std::size_t registered = 0;               // entries in the id register
    std::map<std::string, std::size_t> objects; // what the document owns (not layout's), per type
};

Fingerprint fingerprint(Document& document);

// The fields in which two fingerprints differ, by name; `ignore` lists fields to skip.
std::vector<std::string> differences(const Fingerprint& before, const Fingerprint& after,
                                     const std::vector<std::string_view>& ignore = {});
// The first difference in the text fields, with some context, for a failure message.
std::string explain(const Fingerprint& before, const Fingerprint& after);

struct OpCase {
    std::string name;    // unique across all groups
    std::string fixture; // a corpus file name
    // The batch, built against the freshly opened fixture.
    std::function<Batch(Document&)> make;
    // After the commit: what is wrong with the result, or "" when it is right.
    std::function<std::string(Document& after, const Fingerprint& before, const Fingerprint& committed)> expect;
};

// Registers a group's cases; call it from a namespace-scope initializer.
bool register_cases(std::vector<OpCase> cases);
const std::vector<OpCase>& registered_cases();

struct EvidenceRow {
    std::string op;
    std::string name;
    std::string fixture;
    std::size_t pushes = 0;
    // "pass", or "fail: <reason>"; the C3/C4 pair runs once per rollback path.
    std::string c1, c2, c3_unwind, c4_unwind, c3_snapshot, c4_snapshot, r4;
    api::Policy verdict = api::Policy::Unexposed;
};

EvidenceRow run_case(Engine& engine, const OpCase& op_case, const std::filesystem::path& fixture);

nlohmann::ordered_json to_json(const std::vector<EvidenceRow>& rows);

// --- helpers for cases ------------------------------------------------------

mu::engraving::MasterScore& master_of(Document& document);
// The id of the first element of an MSCX type (in document order) that has one
// and matches; throws when there is none, so a case fails on a fixture that lacks it.
ElementId find_id(Document& document, std::string_view type,
                  const std::function<bool(const mu::engraving::EngravingObject&)>& match = {});
// The element an id names; throws when it names nothing live.
mu::engraving::EngravingObject& element(Document& document, const ElementId& id);
// A property of an element, by its catalogue name.
Value property(Document& document, const ElementId& id, std::string_view name);
// The index-th excerpt of the score.
ScoreSel excerpt(Document& document, std::size_t index);

} // namespace engrave::harness
