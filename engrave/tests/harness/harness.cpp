#include "engrave/tests/harness/harness.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <new>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "engrave/src/api/handles.h"
#include "engrave/src/api/pid_support.h"
#include "engrave/src/api/value_codec.h"
#include "engrave/src/api/state.h"
#include "engrave/src/score/score_positions.h"
#include "engrave/src/score/score_semantic.h"

#include "engraving/dom/engravingitem.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/select.h"
#include "engraving/editing/transaction/transaction.h"
#include "engraving/editing/transaction/undoablecommand.h"
#include "engraving/editing/transaction/undostack.h"
#include "engraving/infrastructure/eidregister.h"
#include "global/io/buffer.h"
#include "global/serialization/zipreader.h"

namespace engrave::harness {
namespace {

using mu::engraving::EngravingObject;
using mu::engraving::MasterScore;
using mu::engraving::MScore;
using mu::engraving::Score;
using mu::engraving::UndoableCommand;

// --- fingerprints -------------------------------------------------------------

std::map<std::string, std::string> mscz_files(const std::vector<std::byte>& bytes) {
    muse::ByteArray data(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    muse::io::Buffer buffer(&data);
    buffer.open(muse::io::IODevice::ReadOnly);
    muse::ZipReader zip(&buffer);
    std::map<std::string, std::string> files;
    for (const auto& info : zip.fileInfoList()) {
        if (!info.isFile) continue;
        const muse::ByteArray file = zip.fileData(info.filePath.toStdString());
        files.emplace(info.filePath.toStdString(), std::string(reinterpret_cast<const char*>(file.constData()), file.size()));
    }
    return files;
}

std::string externals(MasterScore& master) {
    std::string text;
    for (const auto& mapping : master.midiMapping()) {
        const auto& parts = master.parts();
        const auto part = std::ranges::find(parts, mapping.part());
        text += std::format("midi part={} port={} channel={}\n", part - parts.begin(), int(mapping.port()), int(mapping.channel()));
    }
    for (const Score* score : master.scoreList()) {
        for (const auto& [key, value] : score->metaTags()) text += std::format("tag {}={}\n", key.toStdString(), value.toStdString());
    }
    text += std::format("selection none={} input track={} note entry={}\n", master.selection().isNone(),
                        static_cast<long long>(master.inputState().track()), master.inputState().noteEntryMode());
    return text;
}

// Where two strings first differ, for a failure note.
std::string first_difference(std::string_view a, std::string_view b) {
    const auto [left, right] = std::ranges::mismatch(a, b);
    const std::size_t at = static_cast<std::size_t>(left - a.begin());
    const std::size_t from = at > 40 ? at - 40 : 0;
    return std::format("at {}: '{}' vs '{}'", at, a.substr(from, 80), b.substr(from, 80));
}

// Layout positions agree to within one unit of the engine's 1200 DPI (or a part in
// 10^5 of a larger number): a snapshot restore reads back style values the writer
// stored with six significant digits, which moves things by fractions of a unit.
// Where two layout texts first disagree (an offset into each), or nothing when they agree.
std::optional<std::pair<std::size_t, std::size_t>> layout_mismatch(const std::string& a, const std::string& b) {
    const auto number = [](char c) { return (c >= '0' && c <= '9') || c == '-' || c == '.'; };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (number(a[i]) && number(b[j])) {
            char* end_a = nullptr;
            char* end_b = nullptr;
            const double x = std::strtod(a.c_str() + i, &end_a);
            const double y = std::strtod(b.c_str() + j, &end_b);
            if (std::abs(x - y) > std::max(1.0, 1e-5 * std::abs(x))) return std::pair(i, j);
            i = static_cast<std::size_t>(end_a - a.c_str());
            j = static_cast<std::size_t>(end_b - b.c_str());
        } else if (a[i] != b[j]) {
            return std::pair(i, j);
        } else {
            ++i;
            ++j;
        }
    }
    if (i != a.size() || j != b.size()) return std::pair(i, j);
    return std::nullopt;
}

bool same_layout(const std::vector<std::string>& before, const std::vector<std::string>& after) {
    if (before.size() != after.size()) return false;
    for (std::size_t n = 0; n < before.size(); ++n) {
        if (layout_mismatch(before[n], after[n])) return false;
    }
    return true;
}

// --- the push hook ------------------------------------------------------------

struct Fault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum struct Inject { none, before, after, alloc };

// Numbers the commands of one edit as they are pushed, and fails the one at `at`.
struct Pushes {
    Inject mode = Inject::none;
    std::size_t at = 0;
    std::size_t count = 0;
    std::vector<std::pair<const UndoableCommand*, std::size_t>> running; // pushed, not yet performed
};
Pushes g_pushes;

void on_push(UndoableCommand* command, bool appended) {
    std::size_t number = 0;
    if (!appended) {
        number = ++g_pushes.count;
        g_pushes.running.emplace_back(command, number);
    } else {
        // A command's redo can push more commands, so the one that finishes is
        // found by pointer, not by order.
        const auto found = std::ranges::find(g_pushes.running, command, &std::pair<const UndoableCommand*, std::size_t>::first);
        number = found->second;
        g_pushes.running.erase(found);
    }
    if (number != g_pushes.at) return;
    switch (g_pushes.mode) {
    case Inject::before:
        if (appended) return;
        command->cleanup(false);
        delete command;
        throw Fault("fault injected before a push");
    case Inject::after:
        if (appended) throw Fault("fault injected after a push");
        return;
    case Inject::alloc:
        if (appended) throw std::bad_alloc();
        return;
    case Inject::none:
        return;
    }
}

class PushHook {
public:
    PushHook(Inject mode, std::size_t at) {
        g_pushes = {mode, at};
        mu::engraving::Transaction::setPushHook(on_push);
    }
    ~PushHook() { mu::engraving::Transaction::setPushHook(nullptr); }
    PushHook(const PushHook&) = delete;
    PushHook& operator=(const PushHook&) = delete;

    std::size_t count() const { return g_pushes.count; }
};

// --- checks -------------------------------------------------------------------

const std::string pass = "pass";

// A fixture with every score laid out: a commit lays out the excerpts too, and an
// excerpt nobody looked at yet would otherwise show up as a layout change.
std::unique_ptr<Document> open(Engine& engine, const std::filesystem::path& fixture) {
    auto document = engine.open(fixture);
    if (!document) throw std::runtime_error(std::format("{}: {}", fixture.string(), document.error().message));
    for (Score* score : master_of(**document).scoreList()) {
        if (score->npages() == 0) score->doLayout();
    }
    return std::move(*document);
}

std::string joined(const std::vector<std::string>& parts) {
    std::string text;
    for (const auto& part : parts) text += (text.empty() ? "" : ", ") + part;
    return text;
}

// The register is a bijection onto what the walk hands out (as handles_test's INV).
std::string inv(Document& document) {
    MasterScore& master = master_of(document);
    std::unordered_set<const EngravingObject*> objects;
    for (EngravingObject* object : document_objects(master)) objects.insert(object);
    for (const auto& [eid, object] : master.eidRegister()->entries()) {
        if (object->eid() != eid) return eid.toStdString() + ": the reverse entry disagrees";
        if (id_class(*object) == IdClass::persisted && !objects.contains(object) && !in_score(master, object))
            return eid.toStdString() + " names an element that left the document";
    }
    for (const LiveId& entry : live_ids(master)) {
        if (master.eidRegister()->itemFromEID(to_engine(entry.id)) != entry.object)
            return to_string(entry.id) + " names another element";
    }
    for (const EngravingObject* object : objects) {
        if (takes_id(*object) && !object->eid().isValid()) return std::string(object->typeName()) + " has no id";
    }
    return {};
}

std::string check_commit(Engine& engine, const OpCase& op_case, const std::filesystem::path& fixture,
                         std::size_t& pushes) {
    auto document = open(engine, fixture);
    const Fingerprint before = fingerprint(*document);
    const Batch batch = op_case.make(*document);
    Result<EditResult> result;
    {
        PushHook hook(Inject::none, 0);
        result = document->edit(batch, before.revision);
        pushes = hook.count();
    }
    if (!result) return "fail: the batch failed: " + std::string(to_string(result.error().code));
    const Fingerprint after = fingerprint(*document);
    if (after.revision != before.revision + 1) return std::format("fail: revision {} after {}", after.revision, before.revision);
    if (after.undo_count != 0 || after.undo_active) return "fail: the history is not clear";
    if (const std::string problem = inv(*document); !problem.empty()) return "fail: INV: " + problem;
    if (const std::string problem = op_case.expect(*document, before, after); !problem.empty()) return "fail: " + problem;

    const auto saved = document->export_score({.format = Format::Mscz});
    if (!saved) return "fail: the save failed";
    auto reopened = engine.open(std::span<const std::byte>(*saved));
    if (!reopened) return "fail: the reopen failed";
    const Fingerprint again = fingerprint(**reopened);
    if (again.semantic != after.semantic) return "fail: the reopen changed the semantic dump";
    if (again.ids != after.ids) return "fail: the reopen changed the ids";
    return pass;
}

std::string check_undo(Engine& engine, const OpCase& op_case, const std::filesystem::path& fixture) {
    auto document = open(engine, fixture);
    DocumentAccess::edit_hooks(*document).keep_history = true;
    const Fingerprint before = fingerprint(*document);
    const auto result = document->edit(op_case.make(*document));
    if (!result) return "fail: the batch failed: " + std::string(to_string(result.error().code));
    master_of(*document).undoRedo(true, nullptr);
    // The engine's undo lays out the master score only.
    for (Score* score : master_of(*document).scoreList()) score->doLayout();
    const auto changed = differences(before, fingerprint(*document), {"revision", "undo"});
    return changed.empty() ? pass : "fail: undo left " + joined(changed);
}

// One injected failure: the batch must fail (with bad_alloc thrown for `alloc`),
// and every fingerprint field must be what it was.
struct Injection {
    Inject mode = Inject::none;
    std::size_t push = 0;
    std::function<void(std::size_t)> before_step;
    std::string label;
};

std::string check_injection(Engine& engine, const OpCase& op_case, const std::filesystem::path& fixture,
                            api::Policy policy, const Injection& injection, std::string& r4) {
    auto document = open(engine, fixture);
    api::EditHooks& hooks = DocumentAccess::edit_hooks(*document);
    hooks.policy = policy;
    hooks.before_step = injection.before_step;
    const Fingerprint before = fingerprint(*document);
    const Batch batch = op_case.make(*document);
    bool failed = false;
    try {
        PushHook hook(injection.mode, injection.push);
        const auto result = document->edit(batch);
        failed = !result && injection.mode != Inject::alloc;
    } catch (const std::bad_alloc&) {
        failed = injection.mode == Inject::alloc;
    }
    hooks = {};
    if (!failed) return injection.label + ": the batch did not fail as injected";
    const Fingerprint after = fingerprint(*document);
    if ((after.undo_count != 0 || after.undo_active) && r4 == pass) r4 = "fail: " + injection.label + ": history left";
    const auto changed = differences(before, after);
    return changed.empty() ? std::string{} : injection.label + ": " + joined(changed);
}

// A restore that fails poisons the document, and the document then refuses work.
std::string check_poison(Engine& engine, const OpCase& op_case, const std::filesystem::path& fixture) {
    auto document = open(engine, fixture);
    api::EditHooks& hooks = DocumentAccess::edit_hooks(*document);
    hooks.policy = api::Policy::Snapshot;
    hooks.fail_restore = true;
    const Batch batch = op_case.make(*document);
    try {
        PushHook hook(Inject::after, 1);
        (void)document->edit(batch);
        return "a failed restore did not throw";
    } catch (const std::exception&) {
    }
    hooks = {};
    const auto again = document->edit(batch);
    const auto saved = document->export_score({});
    if (again || again.error().code != ErrorCode::Poisoned || saved || saved.error().code != ErrorCode::Poisoned)
        return "a poisoned document still works";
    return {};
}

std::string summary(const std::vector<std::string>& failures) {
    if (failures.empty()) return pass;
    std::string text = "fail: " + failures.front();
    if (failures.size() > 1) text += std::format(" (+{} more)", failures.size() - 1);
    return text;
}

// C3 and C4 under one rollback policy.
std::pair<std::string, std::string> check_rollbacks(Engine& engine, const OpCase& op_case,
                                                    const std::filesystem::path& fixture, std::size_t pushes,
                                                    std::size_t ops, api::Policy policy, std::string& r4) {
    std::vector<std::string> c3;
    std::vector<std::string> c4;
    const auto run = [&](std::vector<std::string>& failures, const Injection& injection) {
        if (std::string failure = check_injection(engine, op_case, fixture, policy, injection, r4); !failure.empty())
            failures.push_back(std::move(failure));
    };
    for (std::size_t push = 1; push <= pushes; ++push) {
        run(c3, {Inject::before, push, {}, std::format("before push {}", push)});
        run(c3, {Inject::after, push, {}, std::format("after push {}", push)});
        run(c4, {Inject::alloc, push, {}, std::format("bad_alloc at push {}", push)});
    }
    // Between the operations, and between the last one and the commit.
    for (std::size_t step = 1; step <= ops; ++step) {
        run(c3, {Inject::none, 0, [step](std::size_t at) { if (at == step) throw Fault("fault injected between operations"); },
                 std::format("exception before step {}", step)});
        run(c3, {Inject::none, 0,
                 [step](std::size_t at) { if (at == step) MScore::setError(mu::engraving::MsError::NO_NOTE_SELECTED); },
                 std::format("engine error before step {}", step)});
    }
    if (policy == api::Policy::Snapshot) {
        if (std::string failure = check_poison(engine, op_case, fixture); !failure.empty()) c4.push_back(std::move(failure));
    }
    return {summary(c3), summary(c4)};
}

std::vector<OpCase>& cases() {
    static std::vector<OpCase> all;
    return all;
}

} // namespace

Fingerprint fingerprint(Document& document) {
    MasterScore& master = master_of(document);
    Fingerprint print;
    const auto saved = document.export_score({.format = Format::Mscz});
    if (!saved) throw std::runtime_error("fingerprint: the save failed: " + saved.error().message);
    print.files = mscz_files(*saved);
    print.semantic = music::semantic_dump(&master).dump(1);
    for (const LiveId& entry : live_ids(master)) {
        print.ids.push_back(std::format("{} {} {}/{}/{}", to_string(entry.id), entry.object->typeName(), entry.placement.score,
                                        entry.placement.tick, entry.placement.track));
    }
    std::ranges::sort(print.ids);
    print.revision = document.revision();
    print.undo_count = master.undoStack()->size();
    print.undo_active = master.undoStack()->hasActiveTransaction();
    print.engine_error = static_cast<int>(MScore::_error);
    int index = 0;
    for (Score* score : master.scoreList()) {
        if (score->npages() > 0)
            print.layout.push_back(std::format("score {} pages {} {}", index, score->npages(), music::score_positions(score, true).dump()));
        ++index;
    }
    print.externals = externals(master);
    print.registered = master.eidRegister()->size();
    for (const EngravingObject* object : document_objects(master)) {
        const bool generated = object->isEngravingItem() && static_cast<const mu::engraving::EngravingItem*>(object)->generated();
        if (id_class(*object) != IdClass::layout && !generated) ++print.objects[object->typeName()];
    }
    return print;
}

std::vector<std::string> differences(const Fingerprint& before, const Fingerprint& after,
                                     const std::vector<std::string_view>& ignore) {
    std::vector<std::string> fields;
    const auto compare = [&](std::string_view name, bool equal) {
        if (!equal && std::ranges::find(ignore, name) == ignore.end()) fields.emplace_back(name);
    };
    compare("files", before.files == after.files);
    compare("semantic", before.semantic == after.semantic);
    compare("ids", before.ids == after.ids);
    compare("revision", before.revision == after.revision);
    compare("undo", before.undo_count == after.undo_count && before.undo_active == after.undo_active);
    compare("engine_error", before.engine_error == after.engine_error);
    compare("layout", same_layout(before.layout, after.layout));
    compare("externals", before.externals == after.externals);
    compare("registered", before.registered == after.registered);
    compare("objects", before.objects == after.objects);
    return fields;
}

std::string explain(const Fingerprint& before, const Fingerprint& after) {
    for (const auto& [name, text] : before.files) {
        const auto found = after.files.find(name);
        if (found == after.files.end()) return name + " is gone";
        if (found->second != text) return name + " " + first_difference(text, found->second);
    }
    if (before.semantic != after.semantic) return "semantic " + first_difference(before.semantic, after.semantic);
    if (before.externals != after.externals) return "externals " + first_difference(before.externals, after.externals);
    for (std::size_t i = 0; i < before.layout.size() && i < after.layout.size(); ++i) {
        if (const auto at = layout_mismatch(before.layout[i], after.layout[i])) {
            const auto [a, b] = *at;
            return std::format("layout at {}: '{}' vs '{}'", a, std::string_view(before.layout[i]).substr(a > 40 ? a - 40 : 0, 80),
                               std::string_view(after.layout[i]).substr(b > 40 ? b - 40 : 0, 80));
        }
    }
    if (before.layout.size() != after.layout.size()) return "layout: another number of laid-out scores";
    if (before.ids != after.ids) return "ids";
    for (const auto& [type, count] : before.objects) {
        const auto found = after.objects.find(type);
        const std::size_t now = found == after.objects.end() ? 0 : found->second;
        if (now != count) return std::format("objects: {} {} -> {}", type, count, now);
    }
    return {};
}

bool register_cases(std::vector<OpCase> group) {
    for (OpCase& op_case : group) cases().push_back(std::move(op_case));
    return true;
}

const std::vector<OpCase>& registered_cases() {
    return cases();
}

EvidenceRow run_case(Engine& engine, const OpCase& op_case, const std::filesystem::path& fixture) {
    EvidenceRow row{.name = op_case.name, .fixture = op_case.fixture};
    std::size_t ops = 0;
    {
        auto document = open(engine, fixture);
        const Batch batch = op_case.make(*document);
        ops = batch.ops.size();
        for (const Op& op : batch.ops) {
            if (row.op.empty()) row.op = api::op_name(op);
            else if (row.op != api::op_name(op)) throw std::invalid_argument(op_case.name + ": a case exercises one operation");
        }
    }
    row.c1 = check_commit(engine, op_case, fixture, row.pushes);
    row.c2 = check_undo(engine, op_case, fixture);
    row.r4 = pass;
    std::tie(row.c3_unwind, row.c4_unwind) =
        check_rollbacks(engine, op_case, fixture, row.pushes, ops, api::Policy::Transaction, row.r4);
    std::tie(row.c3_snapshot, row.c4_snapshot) =
        check_rollbacks(engine, op_case, fixture, row.pushes, ops, api::Policy::Snapshot, row.r4);

    const bool committed = row.c1 == pass;
    if (committed && row.c2 == pass && row.c3_unwind == pass && row.c4_unwind == pass && row.r4 == pass)
        row.verdict = api::Policy::Transaction;
    else if (committed && row.c3_snapshot == pass && row.c4_snapshot == pass)
        row.verdict = api::Policy::Snapshot;
    else
        row.verdict = api::Policy::Unexposed;
    return row;
}

nlohmann::ordered_json to_json(const std::vector<EvidenceRow>& rows) {
    std::vector<const EvidenceRow*> sorted;
    for (const EvidenceRow& row : rows) sorted.push_back(&row);
    std::ranges::sort(sorted, {}, [](const EvidenceRow* row) { return std::tie(row->op, row->name); });

    // An operation is as safe as its weakest case; the policies are ordered from strong to weak.
    std::map<std::string, api::Policy> weakest;
    nlohmann::ordered_json list = nlohmann::ordered_json::array();
    for (const EvidenceRow* row : sorted) {
        const auto [entry, added] = weakest.try_emplace(row->op, row->verdict);
        if (!added) entry->second = std::max(entry->second, row->verdict);
        const std::string verdict(api::to_string(row->verdict));
        list.push_back({{"op", row->op},
                        {"case", row->name},
                        {"fixture", row->fixture},
                        {"pushes", row->pushes},
                        {"c1", row->c1},
                        {"c2", row->c2},
                        {"c3_unwind", row->c3_unwind},
                        {"c4_unwind", row->c4_unwind},
                        {"c3_snapshot", row->c3_snapshot},
                        {"c4_snapshot", row->c4_snapshot},
                        {"r4", row->r4},
                        {"verdict", verdict}});
    }
    nlohmann::ordered_json verdicts = nlohmann::ordered_json::object();
    for (const auto& [op, policy] : weakest) verdicts[op] = api::to_string(policy);
    return {{"verdicts", verdicts}, {"cases", list}};
}

MasterScore& master_of(Document& document) {
    return *DocumentAccess::master_score(document);
}

ElementId find_id(Document& document, std::string_view type, const std::function<bool(const EngravingObject&)>& match) {
    // Document order, not id order: an imported score gets new random ids on every
    // open, and each check opens the fixture again.
    for (const EngravingObject* object : document_objects(master_of(document))) {
        if (object->typeName() != type || !takes_id(*object) || !object->eid().isValid()) continue;
        if (!match || match(*object)) return from_engine(object->eid());
    }
    throw std::invalid_argument(std::format("the fixture has no matching {} with an id", type));
}

EngravingObject& element(Document& document, const ElementId& id) {
    const auto found = resolve(master_of(document), id);
    if (!found) throw std::invalid_argument(found.error().message);
    return *found->object;
}

Value property(Document& document, const ElementId& id, std::string_view name) {
    const mu::engraving::Pid pid = api::pid_of(name);
    if (pid == mu::engraving::Pid::END) throw std::invalid_argument(std::format("no property '{}'", name));
    return api::to_value(mu::engraving::propertyType(pid), element(document, id).getProperty(pid));
}

ScoreSel excerpt(Document& document, std::size_t index) {
    const auto& excerpts = master_of(document).excerpts();
    if (index >= excerpts.size()) throw std::invalid_argument(std::format("the fixture has no excerpt {}", index));
    return {from_engine(excerpts[index]->excerptScore()->eid())};
}

} // namespace engrave::harness
