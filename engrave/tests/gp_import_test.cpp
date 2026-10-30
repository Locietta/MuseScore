// Guitar Pro and PowerTab import of upstream's test data, through the library only.
//
//   engrave_gp_import_test --assets=<resources> [gtest options]
//
// The library contract for bad input is a returned Error{InvalidInput}: the importer must
// neither crash nor hand back a document without a score.  Upstream's fixtures include
// deliberately malformed files (a PowerTab track with no strings, for one), so the test
// opens every score file of src/importexport/guitarpro/tests/data and records which ones
// come back as errors.  The reference MSCX files and the two updateReference scripts in
// that directory are not inputs and are skipped.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <engrave/engrave.h>

#include "engrave/src/api/state.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"

namespace {

std::filesystem::path g_resources;
std::unique_ptr<engrave::Engine> g_engine;

constexpr std::string_view score_extensions[] = { ".gp", ".gp3", ".gp4", ".gp5", ".gpx", ".ptb" };

std::filesystem::path test_data() {
    static const std::filesystem::path dir =
        std::filesystem::path(ENGRAVE_TEST_SOURCE_ROOT) / "src/importexport/guitarpro/tests/data";
    return dir;
}

/// The score files of the test data, sorted, in one run.
std::vector<std::filesystem::path> score_files() {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(test_data())) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string extension = entry.path().extension().string();
        if (std::find(std::begin(score_extensions), std::end(score_extensions), extension)
            != std::end(score_extensions)) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// Chords without a note are not valid model elements: layout reads a chord's first note,
/// so an importer that leaves one behind turns into a crash later.  Counted here so an
/// imported score is not merely present but structurally sound where the crash was.
std::size_t chords_without_notes(mu::engraving::Score* score) {
    std::size_t count = 0;
    for (mu::engraving::Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (mu::engraving::Segment* segment = measure->first(); segment; segment = segment->next()) {
            for (mu::engraving::EngravingItem* element : segment->elist()) {
                if (element && element->isChord() && mu::engraving::toChord(element)->notes().empty()) {
                    ++count;
                }
            }
        }
    }
    return count;
}

TEST(GuitarProImport, EveryTestDataFileOpensOrReturnsAnError) {
    const std::vector<std::filesystem::path> files = score_files();
    ASSERT_GE(files.size(), 240u) << "the Guitar Pro test data is missing under " << test_data();

    std::vector<std::string> errors;
    std::size_t opened = 0;
    for (const std::filesystem::path& path : files) {
        SCOPED_TRACE(path.filename().string());
        // A crash in here aborts the binary, which is what the contract forbids; a returned
        // error is the expected answer for the malformed fixtures.
        auto document = g_engine->open(path);
        if (!document) {
            errors.push_back(path.filename().string() + ": " + document.error().message);
            continue;
        }
        ++opened;
        mu::engraving::MasterScore* master = engrave::DocumentAccess::master_score(**document);
        ASSERT_NE(master, nullptr) << "a document without a score";
        EXPECT_NE(master->firstMeasure(), nullptr) << "a score without a measure";
        EXPECT_FALSE(master->parts().empty()) << "a score without a part";
        EXPECT_EQ(chords_without_notes(master), 0u) << "an imported chord holds no note";
    }

    std::fprintf(stdout, "Guitar Pro test data: %zu score file(s), %zu opened, %zu returned an error\n",
                 files.size(), opened, errors.size());
    for (const std::string& line : errors) {
        std::fprintf(stdout, "  error %s\n", line.c_str());
    }
    std::fflush(stdout);
    EXPECT_GT(opened, 0u);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--assets=")) g_resources = std::filesystem::path(arg.substr(9));
    }
    auto engine = engrave::Engine::create({.resources = g_resources});
    if (!engine) {
        std::fprintf(stderr, "Engine::create: %s (pass --assets=<resources>)\n", engine.error().message.c_str());
        return 1;
    }
    g_engine = std::move(*engine);
    const int result = RUN_ALL_TESTS();
    g_engine.reset();
    return result;
}
