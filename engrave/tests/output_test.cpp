// The render half of the output API: page_count() and render(), compared with
// the A2 render goldens.
//
//   engrave_output_test --assets=<resources> --corpus=<file or dir>... --soundfont=<file>...
//
// The comparison and its limits (what each layer proves) are in
// render_goldens.json and in the slice report:
//   - SVG is the stored golden's exact text, so its sha256 is compared;
//   - PNG is compared the way scripts/golden.py's describe_png does: the file is
//     decoded to the RGBA bytes PIL would see and hashed, and the size and mode
//     must match;
//   - a PDF cannot be normalized without PyMuPDF, so here it is checked to be a
//     PDF whose page count and size behave; the superproject's
//     scripts/test_a4_output.py compares the same files with describe_pdf.
// The audio tests are in audio_test.cpp; the Engine is shared.

#include "output_fixture.h"
#include "digest.h"

#include <engrave/engrave.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace music::test {

std::unique_ptr<engrave::Engine> g_engine;
std::vector<std::filesystem::path> g_corpus;
std::vector<std::filesystem::path> g_soundfonts;
std::filesystem::path g_assets;
std::filesystem::path g_self;

std::filesystem::path corpus_file(std::string_view name) {
    for (const auto& path : g_corpus) {
        if (path.filename() == name) return path;
    }
    return {};
}

std::filesystem::path probe_binary() {
    const auto directory = g_self.parent_path();
    std::filesystem::path candidate = directory / "engrave_output_probe";
#ifdef _WIN32
    candidate += ".exe";
#endif
    return candidate;
}

int run_probe(const std::vector<std::string>& arguments) {
    std::vector<std::string> command;
    command.push_back(probe_binary().string());
    command.insert(command.end(), arguments.begin(), arguments.end());
#ifdef _WIN32
    std::vector<std::wstring> wide;
    wide.reserve(command.size());
    for (const std::string& argument : command) wide.push_back(std::filesystem::path(argument).wstring());
    std::vector<const wchar_t*> pointers;
    for (const std::wstring& argument : wide) pointers.push_back(argument.c_str());
    pointers.push_back(nullptr);
    return _wspawnv(_P_WAIT, std::filesystem::path(command.front()).wstring().c_str(), pointers.data());
#else
    std::vector<char*> pointers;
    for (std::string& argument : command) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, command.front().c_str(), nullptr, nullptr, pointers.data(), ::environ) != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

namespace {

constexpr const char* golden_file_name = "render_goldens.json";

std::string platform_name() {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

std::span<const std::byte> bytes_of(const std::vector<std::byte>& data) {
    return std::span<const std::byte>(data.data(), data.size());
}

std::string as_text(const std::vector<std::byte>& data) {
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

// scripts/golden.py reads and writes the SVG text as UTF-8 with universal
// newlines, so a CRLF from the canvas becomes LF in the golden.
std::vector<std::byte> with_lf_newlines(const std::vector<std::byte>& data) {
    std::vector<std::byte> result;
    result.reserve(data.size());
    for (const std::byte value : data) {
        if (value == std::byte{ '\r' }) continue;
        result.push_back(value);
    }
    return result;
}

nlohmann::json load_goldens() {
    std::filesystem::path path = std::filesystem::path(ENGRAVE_TEST_SOURCE_ROOT) / "engrave/tests" / golden_file_name;
    const char* env = std::getenv("ENGRAVE_RENDER_GOLDENS");
    if (env) path = std::filesystem::u8path(env);
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read the render goldens in '" + path.string() + "'");
    return nlohmann::json::parse(file);
}

struct OpenedScore {
    std::unique_ptr<engrave::Document> document;
};

OpenedScore open(const std::string& name) {
    const std::filesystem::path path = corpus_file(name);
    if (path.empty()) return {};
    auto result = g_engine->open(path);
    if (!result) {
        std::fprintf(stderr, "cannot open %s: %s\n", path.string().c_str(), result.error().message.c_str());
        return {};
    }
    return OpenedScore{ std::move(*result) };
}

void expect_code(const engrave::Result<engrave::RenderOutput>& result, engrave::ErrorCode code, std::string_view what) {
    ASSERT_FALSE(result) << what << ": expected " << engrave::to_string(code) << ", got a value";
    EXPECT_EQ(result.error().code, code) << what << ": " << result.error().message;
}

} // namespace
} // namespace music::test

namespace {

using music::test::g_engine;

void collect(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) files.push_back(entry.path());
        std::ranges::sort(files);
        for (const auto& file : files) collect(file);
        return;
    }
    if (std::filesystem::is_regular_file(path, ec)) music::test::g_corpus.push_back(path);
}

TEST(Output, CapabilitiesListTheRenderAndAudioFormats) {
    const engrave::Capabilities capabilities = g_engine->capabilities();
    EXPECT_EQ(capabilities.render_formats, (std::vector<engrave::RenderFormat>{
                                               engrave::RenderFormat::Pdf, engrave::RenderFormat::Png, engrave::RenderFormat::Svg }));
    EXPECT_EQ(capabilities.audio_formats,
              (std::vector<engrave::AudioFormat>{ engrave::AudioFormat::Wav, engrave::AudioFormat::Flac, engrave::AudioFormat::Opus,
                                                  engrave::AudioFormat::Mp3, engrave::AudioFormat::Aac }));
    EXPECT_EQ(capabilities.soundfonts, music::test::g_soundfonts);
    EXPECT_EQ(capabilities.audio_sample_rate, 44100);
}

// The A2 golden comparison: every fixture of render_goldens.json, rendered with
// A2's options (144 dpi, transparent PNG, no page background, all pages).
TEST(Output, RenderMatchesTheA2Goldens) {
    const nlohmann::json goldens = music::test::load_goldens().at("fixtures");
    const std::string platform = music::test::platform_name();
    int compared = 0;
    for (const auto& [name, platforms] : goldens.items()) {
        if (!platforms.contains(platform)) continue;
        music::test::OpenedScore opened = music::test::open(name);
        ASSERT_TRUE(opened.document) << "the corpus has no " << name << " (pass --corpus=<directory>)";
        engrave::Document& document = *opened.document;
        const nlohmann::json& golden = platforms.at(platform);

        const auto count = document.page_count();
        ASSERT_TRUE(count) << name << ": " << count.error().message;
        EXPECT_EQ(*count, golden.at("pdf").get<std::int32_t>()) << name << ": page count";

        engrave::RenderOptions options;
        options.format = engrave::RenderFormat::Svg;
        const auto svg = document.render(options);
        ASSERT_TRUE(svg) << name << ": " << svg.error().message;
        const auto& pages = golden.at("svg");
        ASSERT_EQ(svg->pages.size(), pages.size()) << name << ": rendered pages";
        for (std::size_t page = 0; page < svg->pages.size(); ++page) {
            EXPECT_EQ(music::test::sha256_hex(music::test::bytes_of(music::test::with_lf_newlines(svg->pages[page]))),
                      pages[page].get<std::string>())
                << name << ": page " << page << " SVG differs from the A2 golden";
        }

        options.format = engrave::RenderFormat::Png;
        const auto png = document.render(options);
        ASSERT_TRUE(png) << name << ": " << png.error().message;
        ASSERT_EQ(png->pages.size(), svg->pages.size());
        std::string error;
        const auto image = music::test::decode_png(music::test::bytes_of(png->pages.front()), error);
        ASSERT_TRUE(image) << name << ": " << error;
        const nlohmann::json& expected = golden.at("png");
        EXPECT_EQ(image->width, expected.at("width").get<std::uint32_t>()) << name << ": PNG width";
        EXPECT_EQ(image->height, expected.at("height").get<std::uint32_t>()) << name << ": PNG height";
        EXPECT_EQ(music::test::sha256_hex(image->rgba), expected.at("sha256").get<std::string>())
            << name << ": the RGBA pixels differ from the A2 golden";

        options.format = engrave::RenderFormat::Pdf;
        const auto pdf = document.render(options);
        ASSERT_TRUE(pdf) << name << ": " << pdf.error().message;
        ASSERT_FALSE(pdf->document.empty());
        EXPECT_EQ(music::test::as_text(pdf->document).substr(0, 5), "%PDF-") << name << ": not a PDF";
        ++compared;
    }
    EXPECT_GE(compared, 3) << "no fixture was compared";
}

// The page range and the renderer's limits, on a two-page score and on a page
// that is too large at the top dpi.
TEST(Output, PageRangesAndLimits) {
    music::test::OpenedScore opened = music::test::open("slurs.mscz");
    ASSERT_TRUE(opened.document);
    engrave::Document& document = *opened.document;
    ASSERT_EQ(*document.page_count(), 2);

    engrave::RenderOptions all;
    all.format = engrave::RenderFormat::Svg;
    const auto everything = document.render(all);
    ASSERT_TRUE(everything);
    ASSERT_EQ(everything->pages.size(), 2u);

    all.first_page = 1;
    const auto second = document.render(all);
    ASSERT_TRUE(second);
    ASSERT_EQ(second->pages.size(), 1u);
    EXPECT_EQ(second->pages.front(), everything->pages.back()) << "the ranged render is not page 1";

    all.first_page = 0;
    all.last_page = 0;
    const auto first = document.render(all);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->pages.size(), 1u);
    EXPECT_EQ(first->pages.front(), everything->pages.front());

    engrave::RenderOptions pdf;
    pdf.format = engrave::RenderFormat::Pdf;
    const auto full = document.render(pdf);
    ASSERT_TRUE(full);
    pdf.first_page = 1;
    pdf.last_page = 1;
    const auto one = document.render(pdf);
    ASSERT_TRUE(one);
    EXPECT_EQ(music::test::as_text(one->document).substr(0, 5), "%PDF-");
    EXPECT_LT(one->document.size(), full->document.size()) << "a one-page PDF is not smaller than the two-page one";

    engrave::RenderOptions bad = all;
    bad.first_page = -1;
    music::test::expect_code(document.render(bad), engrave::ErrorCode::InvalidArgument, "a negative first page");
    bad = all;
    bad.first_page = 1;
    bad.last_page = 0;
    music::test::expect_code(document.render(bad), engrave::ErrorCode::InvalidArgument, "a reversed range");
    bad = all;
    bad.last_page = 5;
    music::test::expect_code(document.render(bad), engrave::ErrorCode::InvalidArgument, "a range past the end");
    bad = all;
    bad.format = engrave::RenderFormat::Png;
    bad.dpi = engrave::min_render_dpi - 1;
    music::test::expect_code(document.render(bad), engrave::ErrorCode::InvalidArgument, "a dpi below the minimum");
    bad.dpi = engrave::max_render_dpi + 1;
    music::test::expect_code(document.render(bad), engrave::ErrorCode::InvalidArgument, "a dpi above the maximum");
    bad = all;
    bad.dpi = 96;
    music::test::expect_code(document.render(bad), engrave::ErrorCode::InvalidArgument, "a dpi on a format that takes none");

    // 8.27 x 11.69 inch at 1200 dpi is 139e6 pixels, over the renderer's 64e6.
    music::test::OpenedScore page = music::test::open("ghost_note.mscx");
    ASSERT_TRUE(page.document);
    engrave::RenderOptions huge;
    huge.format = engrave::RenderFormat::Png;
    huge.dpi = engrave::max_render_dpi;
    music::test::expect_code(page.document->render(huge), engrave::ErrorCode::ResourceLimit, "an oversized page");
}

TEST(Output, SelectionsNameTheScoreOrOneOfItsExcerpts) {
    music::test::OpenedScore opened = music::test::open("linked_parts.mscx");
    ASSERT_TRUE(opened.document);
    engrave::Document& document = *opened.document;
    const auto outline = document.outline();
    ASSERT_TRUE(outline);
    ASSERT_FALSE(outline->excerpts.empty()) << "the fixture has no linked part";

    const engrave::ScoreSel excerpt{ outline->excerpts.front().id };
    // An excerpt is not laid out until something asks for its pages, so the
    // outline that has never done so reports none; page_count lays it out.
    const auto excerpt_pages = document.page_count(excerpt);
    ASSERT_TRUE(excerpt_pages) << excerpt_pages.error().message;
    EXPECT_GE(*excerpt_pages, 1);
    const auto laid_out = document.outline();
    ASSERT_TRUE(laid_out);
    EXPECT_EQ(laid_out->excerpts.front().pages, *excerpt_pages) << "page_count did not lay the excerpt out";

    engrave::RenderOptions options;
    options.format = engrave::RenderFormat::Svg;
    options.score = excerpt;
    const auto rendered = document.render(options);
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered->pages.size(), static_cast<std::size_t>(*excerpt_pages));

    const auto master = document.render({});
    ASSERT_TRUE(master);
    if (!master->pages.empty() && !rendered->pages.empty()) {
        EXPECT_NE(rendered->pages.front(), master->pages.front()) << "the excerpt rendered as the full score";
    }

    engrave::ElementId absent;
    absent.hi = 1;
    absent.lo = 2;
    music::test::expect_code(document.render(engrave::RenderOptions{ .score = engrave::ScoreSel{ absent } }),
                             engrave::ErrorCode::NotFound, "an id no element has");

    const auto score = document.page_count(engrave::ScoreSel{ outline->excerpts.front().id });
    ASSERT_TRUE(score);

    const auto note = document.query(engrave::Query{ .types = { "Note" }, .limit = 1 });
    ASSERT_TRUE(note);
    ASSERT_FALSE(note->elements.empty());
    music::test::expect_code(document.render(engrave::RenderOptions{ .score = engrave::ScoreSel{ note->elements.front().id } }),
                             engrave::ErrorCode::InvalidArgument, "a selection that names a note");
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    std::filesystem::path assets;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument.starts_with("--assets=")) {
            assets = std::filesystem::path(argument.substr(9));
        } else if (argument.starts_with("--corpus=")) {
            collect(std::filesystem::path(argument.substr(9)));
        } else if (argument.starts_with("--soundfont=")) {
            music::test::g_soundfonts.push_back(std::filesystem::path(argument.substr(12)));
        }
    }
    if (music::test::g_soundfonts.empty()) {
        if (const char* env = std::getenv("MUSIC_MCP_SOUNDFONT")) music::test::g_soundfonts.push_back(std::filesystem::u8path(env));
    }
    music::test::g_self = std::filesystem::absolute(argv[0]);
    music::test::g_assets = assets;
    if (music::test::g_corpus.empty()) {
        for (const auto& candidate : { std::filesystem::path("fixtures/golden"), std::filesystem::path("../fixtures/golden"),
                                       std::filesystem::path("../../fixtures/golden") }) {
            if (std::filesystem::is_directory(candidate)) {
                collect(candidate);
                break;
            }
        }
    }
    engrave::EngineOptions options;
    options.resources = assets;
    options.soundfonts = music::test::g_soundfonts;
    auto engine = engrave::Engine::create(options);
    if (!engine) {
        std::fprintf(stderr, "Engine::create: %s (pass --assets=<resources>)\n", engine.error().message.c_str());
        return 1;
    }
    music::test::g_engine = std::move(*engine);
    const int result = RUN_ALL_TESTS();
    music::test::g_engine.reset();
    return result;
}
