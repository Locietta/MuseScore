// SPDX-License-Identifier: GPL-3.0-only
// Existing upstream project examples retain the repository GPL-3.0-only licence.
#include <gtest/gtest.h>
#include <engrave/engrave.h>
#include "engrave/src/api/state.h"
#include "engrave/src/score/score_semantic.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
namespace {
std::unique_ptr<engrave::Engine> engine;
std::filesystem::path data = std::filesystem::path(ENGRAVE_TEST_SOURCE_ROOT) / "src/importexport/mnx/tests/data/project_examples";
using Json = nlohmann::ordered_json;
Json events(engrave::Document& doc) {
    return music::semantic_dump(engrave::DocumentAccess::master_score(doc)).at("events");
}
// The MSCX reader subdivides gap rests (including the historical 1/3-tick
// rounding in tupletHiddenRest). Compare sounding events with all their note,
// duration, tie, lyric and articulation semantics; byte/path equality below
// still compares the complete event dump, including every imported rest.
Json sounding_events(Json dump) {
    for (auto& staff : dump) for (auto& voice : staff) {
        auto notes = Json::array();
        for (const auto& event : voice) if (event.at("kind") == "chord") notes.push_back(event);
        voice = std::move(notes);
    }
    return dump;
}
TEST(MnxImport, UpstreamSemanticReferences) {
    unsigned count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(data)) {
        if (entry.path().extension() != ".mnx") continue;
        ++count;
        SCOPED_TRACE(entry.path().filename().string());
        auto opened = engine->open(entry.path());
        ASSERT_TRUE(opened) << opened.error().message;
        auto reference = engine->open(data / (entry.path().stem().string() + "_ref.mscx"));
        ASSERT_TRUE(reference) << reference.error().message;
        EXPECT_EQ(sounding_events(events(**opened)), sounding_events(events(**reference)));
        // Byte input without a name exercises JSON content detection.
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        auto sniffed = engine->open(std::as_bytes(std::span(bytes)));
        ASSERT_TRUE(sniffed) << sniffed.error().message;
        EXPECT_EQ(events(**opened), events(**sniffed));
    }
    EXPECT_EQ(count, 37u);
}
TEST(MnxImport, IndependentSemanticOracle) {
    auto opened = engine->open(data.parent_path() / "engine/two-notes.mnx");
    ASSERT_TRUE(opened) << opened.error().message;
    auto dump = events(**opened).at(0).at(0);
    ASSERT_EQ(dump.size(), 2u);
    for (unsigned i = 0; i < 2; ++i) {
        EXPECT_EQ(dump[i]["kind"], "chord");
        EXPECT_EQ(dump[i]["tick"], i * 960);
        EXPECT_EQ(dump[i]["ticks"], 960);
        EXPECT_EQ(dump[i]["notes"][0]["pitch"], i == 0 ? 60 : 62);
    }
}
TEST(MnxImport, InvalidInputAndRecovery) {
    for (const std::string input : {"{", "{}", "{\"mnx\":{\"version\":27}}",
             "{\"mnx\":{\"version\":27},\"global\":{\"measures\":[]},\"parts\":[]}"}) {
        auto opened = engine->open(std::as_bytes(std::span(input)), {.format = engrave::Format::Mnx});
        EXPECT_FALSE(opened);
        if (!opened) EXPECT_EQ(opened.error().code, engrave::ErrorCode::InvalidInput);
    }
    EXPECT_TRUE(engine->open(data / "tupletSimple.mnx"));
}
}
int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    std::filesystem::path assets;
    for (int i=1; i<argc; ++i) {
        std::string_view arg=argv[i];
        if (arg.starts_with("--assets=")) assets=arg.substr(9);
    }
    auto created=engrave::Engine::create({.resources=assets});
    if (!created) { std::fprintf(stderr,"%s\n",created.error().message.c_str()); return 1; }
    engine=std::move(*created);
    int result=RUN_ALL_TESTS(); engine.reset(); return result;
}
