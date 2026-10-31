// The edit API (Document::edit), the snapshot round trip (S1) and the evidence of
// every registered OpCase (harness.h).
//
//   engrave_ops_test --assets=<resources> --corpus=<file or dir>... [--update-evidence]
//
// --update-evidence rewrites op_evidence.json from this run instead of comparing
// against it. The engine hands out random ids, as in production: the sequential ids
// of deterministic_ids collide with the ids of a score read back, so a round trip
// under them repairs ids that production never would.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engrave/tests/harness/harness.h"

#include "engrave/src/api/state.h"

#include "engraving/dom/masterscore.h"
#include "engraving/editing/transaction/undostack.h"

namespace {

using engrave::Batch;
using engrave::Color;
using engrave::ElementId;
using engrave::ErrorCode;
using engrave::ResetProperty;
using engrave::SetProperty;
using engrave::SetStyle;
using engrave::harness::find_id;
using engrave::harness::property;

const std::filesystem::path g_evidence = ENGRAVE_TEST_SOURCE_ROOT "/engrave/tests/harness/op_evidence.json";
std::vector<std::filesystem::path> g_corpus;
bool g_update_evidence = false;
std::unique_ptr<engrave::Engine> g_engine;

std::filesystem::path fixture(std::string_view name) {
    for (const auto& path : g_corpus) {
        if (path.filename() == name) return path;
    }
    return {};
}

std::unique_ptr<engrave::Document> open(std::string_view name) {
    const std::filesystem::path path = fixture(name);
    EXPECT_FALSE(path.empty()) << name << " is not in the corpus";
    auto document = g_engine->open(path);
    EXPECT_TRUE(document) << name << ": " << document.error().message;
    return document ? std::move(*document) : nullptr;
}

// --- Document::edit -----------------------------------------------------------

constexpr std::string_view fixture_name = "chordline_path.mscx";

TEST(Edit, CommitAdvancesTheRevisionAndClearsTheHistory) {
    auto document = open(fixture_name);
    ASSERT_TRUE(document);
    const ElementId note = find_id(*document, "Note");
    const auto result = document->edit({{SetProperty{note, "color", Color{255, 0, 0}}}}, 1);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->revision, 2u);
    EXPECT_EQ(document->revision(), 2u);
    ASSERT_EQ(result->ops.size(), 1u);
    EXPECT_TRUE(result->ops[0].created.empty());
    EXPECT_TRUE(result->created.empty() && result->removed.empty() && result->replaced.empty());
    EXPECT_EQ(property(*document, note, "color"), engrave::Value(Color{255, 0, 0}));
    EXPECT_EQ(engrave::harness::master_of(*document).undoStack()->size(), 0u);
}

TEST(Edit, ABatchThatChangesNothingKeepsTheRevision) {
    auto document = open(fixture_name);
    ASSERT_TRUE(document);
    const ElementId note = find_id(*document, "Note");
    EXPECT_EQ(document->edit({})->revision, 1u);
    const auto same = document->edit({{SetProperty{note, "color", property(*document, note, "color")}}});
    ASSERT_TRUE(same) << same.error().message;
    EXPECT_EQ(same->revision, 1u);
}

TEST(Edit, AStaleRevisionConflicts) {
    auto document = open(fixture_name);
    ASSERT_TRUE(document);
    const ElementId note = find_id(*document, "Note");
    const auto result = document->edit({{SetProperty{note, "color", Color{255, 0, 0}}}}, 7);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::RevisionConflict);
    EXPECT_EQ(document->revision(), 1u);
}

// Every error names the operation and leaves the score as it was, whether the
// operation fails while it is prepared or while it runs after others changed the score.
TEST(Edit, AFailingOperationRollsTheBatchBack) {
    auto document = open(fixture_name);
    ASSERT_TRUE(document);
    const ElementId note = find_id(*document, "Note");
    const engrave::Value color = property(*document, note, "color");
    const SetProperty red{note, "color", Color{255, 0, 0}};
    struct Bad {
        engrave::Op op;
        ErrorCode code;
    };
    const Bad bad[] = {
        {SetProperty{note, "noSuchProperty", 1.0}, ErrorCode::InvalidArgument},
        {SetProperty{note, "color", std::int64_t{3}}, ErrorCode::InvalidArgument},
        {SetProperty{note, "pitch", std::int64_t{60}}, ErrorCode::InvalidArgument},    // structural only
        {SetProperty{note, "beamMode", engrave::Enum{"auto"}}, ErrorCode::InvalidArgument}, // not a Note property
        {SetProperty{ElementId{1, 2}, "color", Color{}}, ErrorCode::NotFound},
        {SetProperty{engrave::OpRef{1, 0}, "color", Color{}}, ErrorCode::InvalidArgument},
        {ResetProperty{note, "noSuchProperty"}, ErrorCode::InvalidArgument},
        {SetStyle{{}, "noSuchStyle", 1.0}, ErrorCode::InvalidArgument},
        {SetStyle{{}, "staffDistance", true}, ErrorCode::InvalidArgument},
        {SetStyle{{ElementId{1, 2}}, "staffDistance", 9.0}, ErrorCode::NotFound},
    };
    for (const Bad& entry : bad) {
        const auto result = document->edit({{red, entry.op}});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, entry.code) << result.error().message;
        EXPECT_EQ(result.error().op_index, 1u) << result.error().message;
        EXPECT_EQ(document->revision(), 1u);
        EXPECT_EQ(property(*document, note, "color"), color) << "the first operation was not rolled back";
    }
}

TEST(Edit, APoisonedDocumentRefusesWork) {
    auto document = open(fixture_name);
    ASSERT_TRUE(document);
    auto& hooks = engrave::DocumentAccess::edit_hooks(*document);
    hooks.policy = engrave::api::Policy::Snapshot;
    hooks.fail_restore = true;
    hooks.before_step = [](std::size_t) { throw std::runtime_error("injected"); };
    const Batch batch{{SetStyle{{}, "staffDistance", 9.0}}};
    EXPECT_ANY_THROW((void)document->edit(batch));
    hooks = {};
    const auto again = document->edit(batch);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::Poisoned);
    const auto saved = document->export_score({});
    ASSERT_FALSE(saved);
    EXPECT_EQ(saved.error().code, ErrorCode::Poisoned);
}

// --- S1: the snapshot round trip ------------------------------------------------

// What the engine's writer and reader do not keep symmetric, on the first round trip
// of these corpus files; the second one is stable.
const std::map<std::string, std::string> s1_asymmetries = {
    // The importer gives a title an inline <font size>; read back, it is a size property.
    {"drums_import.musicxml", "files"},
    // The excerpt's page style is written out in full once it was read back.
    {"linked_parts.mscx", "files"},
};

std::string changed_fields(const engrave::harness::Fingerprint& before, const engrave::harness::Fingerprint& after) {
    std::string fields;
    for (const auto& field : engrave::harness::differences(before, after, {"layout", "externals", "objects", "registered"}))
        fields += (fields.empty() ? "" : " ") + field;
    return fields;
}

// A snapshot restore reads back what the writer wrote. Through the guard's own
// restore, the write of the restored score has to be the write it came from, byte
// for byte, with the same meaning and the same ids. Layout and the MIDI mapping are
// the harness's to judge per operation (C3/C4): a score the engine did not write
// itself is laid out differently once it is read back as a current one.
TEST(Snapshot, S1RestoreIsIdempotent) {
    // A throw, not a returned error: the guard unwinds a clean failure and only
    // restores the snapshot when the engine threw (see BatchGuard::roll_back), and
    // this test is about the restore.
    const engrave::api::Step fail = [](engrave::api::EditContext&) -> engrave::Result<void> {
        throw std::runtime_error("restore");
    };
    for (const auto& path : g_corpus) {
        auto document = g_engine->open(path);
        if (!document) continue;
        const std::string name = path.filename().string();
        const auto restore = [&document, &fail] {
            EXPECT_FALSE(engrave::api::BatchGuard(**document).run({fail}, engrave::api::Policy::Snapshot, {}));
            return engrave::harness::fingerprint(**document);
        };
        const auto written = engrave::harness::fingerprint(**document);
        const auto once = restore();
        const auto twice = restore();
        const auto known = s1_asymmetries.find(name);
        const std::string expected = known == s1_asymmetries.end() ? "" : known->second;
        EXPECT_EQ(changed_fields(written, once), expected) << name << ": " << engrave::harness::explain(written, once);
        EXPECT_EQ(changed_fields(once, twice), "") << name << ": " << engrave::harness::explain(once, twice);
    }
}

// A batch whose rollback policy is Snapshot can fail with an error the operations
// themselves return (a stale handle, an engine refusal), not only with a throw. The
// score then has to come back as it was, layout included: reloading the snapshot is
// what moves it (S1 above), so the unwind is what the guard keeps. The validation's
// rollback probe found this batch; its 39 changed note boxes are the regression.
TEST(Edit, AFailedMixedBatchKeepsTheLayout) {
    auto document = open("layout_elements.mscx");
    ASSERT_TRUE(document);
    engrave::Query query;
    query.types = {"Note"};
    query.layout_boxes = true;
    const auto boxes = [&query](engrave::Document& doc) {
        std::map<engrave::ElementId, engrave::LayoutBox> found;
        const auto result = doc.query(query);
        EXPECT_TRUE(result) << result.error().message;
        if (!result) return found;
        for (const auto& item : result->elements) {
            if (item.box) found.emplace(item.id, *item.box);
        }
        return found;
    };
    const auto before = boxes(*document);
    ASSERT_GE(before.size(), 40u);
    const auto svg_before = document->render({.format = engrave::RenderFormat::Svg, .first_page = 0, .last_page = 0});
    ASSERT_TRUE(svg_before) << svg_before.error().message;
    ASSERT_FALSE(svg_before->pages.empty());

    const engrave::ElementId note = engrave::harness::find_id(*document, "Note");
    const auto outline = document->outline();
    ASSERT_TRUE(outline);
    const engrave::ElementId measure = outline->measures.front().id;
    const engrave::Batch batch{{engrave::Remove{note}, engrave::Remove{note},
                                engrave::SetLayoutBreak{measure, "line", false}}};
    const auto result = document->edit(batch);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::StaleHandle);
    EXPECT_EQ(result.error().op_index, 1u);
    EXPECT_EQ(document->revision(), 1u);

    const auto after = boxes(*document);
    ASSERT_EQ(after.size(), before.size());
    for (const auto& [id, box] : before) {
        const auto found = after.find(id);
        ASSERT_NE(found, after.end()) << engrave::to_string(id) << " is gone after the rollback";
        EXPECT_EQ(found->second.page, box.page) << engrave::to_string(id);
        EXPECT_EQ(found->second.x, box.x) << engrave::to_string(id);
        EXPECT_EQ(found->second.y, box.y) << engrave::to_string(id);
        EXPECT_EQ(found->second.width, box.width) << engrave::to_string(id);
        EXPECT_EQ(found->second.height, box.height) << engrave::to_string(id);
    }
    const auto svg_after = document->render({.format = engrave::RenderFormat::Svg, .first_page = 0, .last_page = 0});
    ASSERT_TRUE(svg_after) << svg_after.error().message;
    ASSERT_FALSE(svg_after->pages.empty());
    EXPECT_TRUE(svg_before->pages[0] == svg_after->pages[0]) << "the rollback changed the first page";
}

// --- the evidence ---------------------------------------------------------------

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(Evidence, EveryCaseMatchesTheCheckedInRow) {
    std::vector<engrave::harness::EvidenceRow> rows;
    for (const auto& op_case : engrave::harness::registered_cases()) {
        const std::filesystem::path path = fixture(op_case.fixture);
        ASSERT_FALSE(path.empty()) << op_case.name << ": " << op_case.fixture << " is not in the corpus";

        rows.push_back(engrave::harness::run_case(*g_engine, op_case, path));
    }
    const std::string current = engrave::harness::to_json(rows).dump(2) + "\n";
    if (g_update_evidence) {
        std::ofstream(g_evidence, std::ios::binary) << current;
        return;
    }
    const auto checked_in = nlohmann::ordered_json::parse(read_text(g_evidence), nullptr, false);
    ASSERT_FALSE(checked_in.is_discarded()) << g_evidence.string() << " is not JSON";
    const auto now = nlohmann::ordered_json::parse(current);
    EXPECT_EQ(now["verdicts"], checked_in["verdicts"]);
    for (const auto& row : now["cases"]) {
        const auto found = std::ranges::find_if(checked_in["cases"], [&row](const auto& old) { return old["case"] == row["case"]; });
        if (found == checked_in["cases"].end()) ADD_FAILURE() << row["case"] << " has no checked-in row";
        else EXPECT_EQ(row, *found) << "rerun with --update-evidence once the change is understood";
    }
}

// --- corpus -----------------------------------------------------------------------

void collect(const std::filesystem::path& path) {
    static const std::set<std::string> extensions = {".mscx", ".mscz", ".musicxml", ".mxl", ".mid", ".midi",
                                                     ".gp",   ".gp3",  ".gp4",      ".gp5", ".gpx", ".ptb"};
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        std::vector<std::filesystem::path> entries;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) entries.push_back(entry.path());
        std::ranges::sort(entries);
        for (const auto& entry : entries) collect(entry);
    } else if (std::filesystem::is_regular_file(path, ec) && extensions.contains(path.extension().string())) {
        g_corpus.push_back(path);
    }
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    std::filesystem::path assets;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument.starts_with("--assets=")) assets = std::filesystem::path(argument.substr(9));
        else if (argument.starts_with("--corpus=")) collect(std::filesystem::path(argument.substr(9)));
        else if (argument == "--update-evidence") g_update_evidence = true;
    }
    if (g_corpus.empty()) {
        std::fprintf(stderr, "no corpus: pass --corpus=<file or directory>\n");
        return 1;
    }
    auto engine = engrave::Engine::create({.resources = assets});
    if (!engine) {
        std::fprintf(stderr, "Engine::create: %s (pass --assets=<resources>)\n", engine.error().message.c_str());
        return 1;
    }
    g_engine = std::move(*engine);
    const int result = RUN_ALL_TESTS();
    g_engine.reset();
    return result;
}
