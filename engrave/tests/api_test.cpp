// Tests of the public engrave API.
//
//   engrave_api_test --assets=<resources> [gtest options]
//
// One Engine serves the whole process, as the API requires; the death tests
// re-run main in a child process, which creates its own.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include <engrave/engrave.h>

#include "engrave/src/api/sniff.h"

namespace {

using engrave::ErrorCode;
using engrave::Format;

std::filesystem::path g_resources;
std::unique_ptr<engrave::Engine> g_engine;

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

const std::filesystem::path& gp_data() {
    static const std::filesystem::path dir =
        std::filesystem::path(ENGRAVE_TEST_SOURCE_ROOT) / "src/importexport/guitarpro/tests/data";
    return dir;
}

std::unique_ptr<engrave::Document> treble_clef(const std::string& title = {}) {
    auto document = g_engine->create_document({.template_id = "Treble_Clef", .title = title});
    EXPECT_TRUE(document) << document.error().message;
    return document ? std::move(*document) : nullptr;
}

std::string exported(engrave::Document& document, Format format) {
    auto bytes = document.export_score({.format = format});
    EXPECT_TRUE(bytes) << bytes.error().message;
    return bytes ? text_of(*bytes) : std::string{};
}

TEST(Open, RejectsPreMuseScore4) {
    for (const std::string version : {"1.14", "2.07", "3.00", "3.02", "4.00", "4.10", "4.40", "4.50"}) {
        const auto xml = bytes_of("<museScore version=\"" + version + "\"><Score/></museScore>");
        auto result = g_engine->open(xml, {.format = Format::Mscx});
        ASSERT_FALSE(result) << version;
        EXPECT_EQ(result.error().code, ErrorCode::Unsupported);
        EXPECT_NE(result.error().message.find("written by MuseScore " + version
                  + "; scores from before MuseScore 4.6 are not supported"), std::string::npos);
    }
}

TEST(Capabilities, ListsFormatsAndVersion) {
    const auto capabilities = g_engine->capabilities();
    EXPECT_FALSE(capabilities.engine_version.empty());
    EXPECT_EQ(capabilities.import_formats.size(), 7u);
    EXPECT_EQ(capabilities.export_formats,
              (std::vector<Format>{Format::Mscz, Format::MusicXml, Format::Mxl, Format::Midi}));
}

TEST(Templates, ListsCategoriesJsonInOrder) {
    const auto templates = g_engine->templates();
    ASSERT_GE(templates.size(), 40u);
    EXPECT_EQ(templates.front().id, "Treble_Clef");
    EXPECT_EQ(templates.front().title, "Treble Clef");
    EXPECT_EQ(templates.front().category, "General");
    std::set<std::string> ids;
    for (const auto& info : templates) {
        EXPECT_TRUE(ids.insert(info.id).second) << "duplicate template id " << info.id;
        EXPECT_TRUE(std::filesystem::is_regular_file(info.path)) << info.path;
        EXPECT_FALSE(info.category.empty());
    }
    EXPECT_TRUE(ids.contains("SATB_+_Organ"));
}

TEST(Create, AppliesMetadataAtRevisionOne) {
    auto document = treble_clef("An engrave title");
    ASSERT_TRUE(document);
    EXPECT_EQ(document->revision(), 1u);
    EXPECT_TRUE(document->diagnostics().empty());
    const std::string musicxml = exported(*document, Format::MusicXml);
    EXPECT_NE(musicxml.find("An engrave title"), std::string::npos);
    EXPECT_NE(musicxml.find(">Piano</part-name>"), std::string::npos);
}

TEST(Create, FromTemplatePath) {
    const auto templates = g_engine->templates();
    ASSERT_FALSE(templates.empty());
    auto document = g_engine->create_document({.template_path = templates.back().path});
    ASSERT_TRUE(document) << document.error().message;
    EXPECT_EQ((*document)->revision(), 1u);
}

TEST(Create, RejectsBadOptions) {
    auto neither = g_engine->create_document({});
    ASSERT_FALSE(neither);
    EXPECT_EQ(neither.error().code, ErrorCode::InvalidArgument);

    const auto path = g_engine->templates().front().path;
    auto both = g_engine->create_document({.template_id = "Treble_Clef", .template_path = path});
    ASSERT_FALSE(both);
    EXPECT_EQ(both.error().code, ErrorCode::InvalidArgument);

    auto unknown = g_engine->create_document({.template_id = "No_Such_Template"});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::NotFound);

    auto missing = g_engine->create_document({.template_path = path.parent_path() / "missing.mscx"});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);

    auto style = g_engine->create_document({.template_path = path.parent_path() / "score_style.mss"});
    ASSERT_FALSE(style);
    EXPECT_EQ(style.error().code, ErrorCode::InvalidArgument);
}

TEST(Export, WritesEachExportFormat) {
    auto document = treble_clef();
    ASSERT_TRUE(document);
    EXPECT_TRUE(exported(*document, Format::Mscz).starts_with("PK\x03\x04"));
    EXPECT_TRUE(exported(*document, Format::Mxl).starts_with("PK\x03\x04"));
    EXPECT_TRUE(exported(*document, Format::Midi).starts_with("MThd"));
    EXPECT_NE(exported(*document, Format::MusicXml).find("<score-partwise"), std::string::npos);
    EXPECT_EQ(document->revision(), 1u);
}

TEST(Export, RejectsFormatsTheEngineDoesNotWrite) {
    auto document = treble_clef();
    ASSERT_TRUE(document);
    for (const auto format : {Format::Mscx, Format::GuitarPro, Format::Mnx}) {
        const auto bytes = document->export_score({.format = format});
        ASSERT_FALSE(bytes);
        EXPECT_EQ(bytes.error().code, ErrorCode::Unsupported);
    }
}

TEST(Sniff, DetectsEveryImportFormat) {
    auto document = treble_clef();
    ASSERT_TRUE(document);
    EXPECT_EQ(engrave::sniff_format(exported(*document, Format::Mscz)), Format::Mscz);
    EXPECT_EQ(engrave::sniff_format(exported(*document, Format::Mxl)), Format::Mxl);
    EXPECT_EQ(engrave::sniff_format(exported(*document, Format::MusicXml)), Format::MusicXml);
    EXPECT_EQ(engrave::sniff_format(exported(*document, Format::Midi)), Format::Midi);
    EXPECT_EQ(engrave::sniff_format(read_file(g_engine->templates().front().path)), Format::Mscx);
    for (const char* name : {"bend.gp3", "bend.gp4", "heavy-accent.gp5", "sforzato.gpx", "sforzato.gp", "ptb-baseline.ptb"})
        EXPECT_EQ(engrave::sniff_format(read_file(gp_data() / name)), Format::GuitarPro) << name;
}

TEST(Sniff, LooksPastByteOrderMarkAndProlog) {
    EXPECT_EQ(engrave::sniff_format("\xEF\xBB\xBF\n<?xml version=\"1.0\"?>\n<!-- c -->\n<museScore version=\"5.00\"/>"),
              Format::Mscx);
    EXPECT_EQ(engrave::sniff_format("<?xml version=\"1.0\"?>\n<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 "
                                    "Partwise//EN\" \"http://www.musicxml.org/dtds/partwise.dtd\">\n<score-timewise/>"),
              Format::MusicXml);
}

TEST(Sniff, RejectsUnknownContent) {
    EXPECT_EQ(engrave::sniff_format(""), std::nullopt);
    EXPECT_EQ(engrave::sniff_format("hello"), std::nullopt);
    EXPECT_EQ(engrave::sniff_format("<html><body/></html>"), std::nullopt);
    EXPECT_EQ(engrave::sniff_format("<museScore"), std::nullopt) << "malformed XML";
    EXPECT_EQ(engrave::sniff_format(std::string("PK\x03\x04", 4) + "not a zip"), std::nullopt);
    EXPECT_EQ(engrave::sniff_format("\x10" "FICHIER GUITAR"), Format::GuitarPro) << "the magic alone decides";
}

TEST(Sniff, MapsExtensions) {
    EXPECT_EQ(engrave::format_from_extension("a.MSCZ"), Format::Mscz);
    EXPECT_EQ(engrave::format_from_extension("a.xml"), Format::MusicXml);
    EXPECT_EQ(engrave::format_from_extension("a.kar"), Format::Midi);
    EXPECT_EQ(engrave::format_from_extension("a.gtp"), Format::GuitarPro);
    EXPECT_EQ(engrave::format_from_extension("a.pdf"), std::nullopt);
    EXPECT_EQ(engrave::format_from_extension("mscz"), std::nullopt);
}

TEST(Open, ReadsEveryImportFormatAtRevisionOne) {
    auto source = treble_clef("Round trip");
    ASSERT_TRUE(source);
    for (const auto format : {Format::Mscz, Format::Mxl, Format::MusicXml, Format::Midi}) {
        auto document = g_engine->open(bytes_of(exported(*source, format)));
        ASSERT_TRUE(document) << to_string(format) << ": " << document.error().message;
        EXPECT_EQ((*document)->revision(), 1u);
        EXPECT_TRUE((*document)->diagnostics().empty()) << to_string(format);
    }
    auto mscx = g_engine->open(g_engine->templates().front().path);
    ASSERT_TRUE(mscx) << mscx.error().message;
    for (const char* name : {"bend.gp3", "heavy-accent.gp5", "sforzato.gpx", "sforzato.gp", "ptb-baseline.ptb"}) {
        auto document = g_engine->open(gp_data() / name);
        EXPECT_TRUE(document) << name << ": " << document.error().message;
    }
}

TEST(Open, ExplicitFormatSkipsSniffing) {
    auto source = treble_clef();
    ASSERT_TRUE(source);
    const auto mscz = bytes_of(exported(*source, Format::Mscz));
    auto document = g_engine->open(mscz, {.format = Format::Midi});
    ASSERT_FALSE(document);
    EXPECT_EQ(document.error().code, ErrorCode::InvalidInput);
}

TEST(Open, FallsBackToTheExtension) {
    // Content the sniffer does not recognise is read as the extension's format.
    auto document = g_engine->open(bytes_of("not xml"), {.filename_hint = "score.musicxml"});
    ASSERT_FALSE(document);
    EXPECT_EQ(document.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(document.error().message.find("MusicXML"), std::string::npos) << document.error().message;
}

TEST(Open, ReportsCallerErrors) {
    const auto expect_error = [](const engrave::Result<std::unique_ptr<engrave::Document>>& result, ErrorCode code) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, code) << result.error().message;
        EXPECT_FALSE(result.error().message.empty());
    };
    expect_error(g_engine->open(std::span<const std::byte>{}), ErrorCode::InvalidInput);
    expect_error(g_engine->open(bytes_of("hello")), ErrorCode::UnsupportedFormat);
    expect_error(g_engine->open(bytes_of("hello"), {.filename_hint = "notes.txt"}), ErrorCode::UnsupportedFormat);
    expect_error(g_engine->open(std::vector<std::byte>(engrave::max_input_bytes + 1)), ErrorCode::ResourceLimit);
    // Malformed XML has no root to sniff; only a file name makes it MusicXML.
    expect_error(g_engine->open(bytes_of("<score-partwise><part")), ErrorCode::UnsupportedFormat);
    expect_error(g_engine->open(bytes_of("<score-partwise><part"), {.filename_hint = "a.xml"}), ErrorCode::InvalidInput);
    expect_error(g_engine->open(bytes_of("MThd")), ErrorCode::InvalidInput);
    expect_error(g_engine->open(g_resources / "no-such-score.mscz"), ErrorCode::NotFound);
    expect_error(g_engine->open(g_resources), ErrorCode::InvalidArgument);
}

TEST(Contract, SecondEngineAborts) {
    EXPECT_DEATH((void)engrave::Engine::create({.resources = g_resources}), "one Engine per process");
}

TEST(Contract, CallFromAnotherThreadAborts) {
    EXPECT_DEATH(std::thread([] { (void)g_engine->templates(); }).join(), "thread other than");
}

TEST(Contract, EngineDestroyedBeforeItsDocumentsAborts) {
    EXPECT_DEATH(
        {
            auto document = treble_clef();
            g_engine.reset();
        },
        "while Documents were open");
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--assets=")) g_resources = std::filesystem::path(arg.substr(9));
    }
    // Failure before binding services must leave startup retryable.
    auto missing = engrave::Engine::create({.resources = g_resources / "c5-missing-resources"});
    if (missing || missing.error().code != ErrorCode::NotFound) {
        std::fprintf(stderr, "C5 startup failure did not leave services unbound\n");
        return 1;
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
