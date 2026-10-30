#include "test_environment.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "global/runtime.h"
#include "global/stringutils.h"

#if defined(TEST_HAS_ENGRAVING)
#include "engrave/src/runtime/noqt_runtime.h"
#include "engraving/dom/mscore.h"
#include "utils/scorerw.h"
#include "mocks/engravingconfigurationmock.h"
#include "global/modularity/ioc.h"
#include <gmock/gmock.h>
#endif

namespace music::test {

namespace {

bool is_valid_assets_dir(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p / "engraving" / "instruments" / "instruments.xml", ec);
}

bool is_valid_engraving_data_dir(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::is_directory(p / "playback" / "playbackeventsrenderer_data", ec)
        || std::filesystem::is_directory(p / "playback" / "playbackmodel_data", ec);
}

std::string utf8(const std::filesystem::path& p)
{
    const auto value = p.generic_u8string();
    return std::string(value.begin(), value.end());
}

std::string normalize_path(const std::filesystem::path& p)
{
    std::error_code ec;
    auto canonical = std::filesystem::canonical(p, ec);
    if (!ec) {
        return utf8(canonical);
    }
    auto absolute = std::filesystem::absolute(p, ec);
    if (!ec) {
        return utf8(absolute);
    }
    return utf8(p);
}

#if defined(TEST_HAS_ENGRAVING)
static const mu::engraving::IEngravingConfiguration::DebuggingOptions s_debugOpt{};
#endif

} // namespace

std::string find_assets_dir(const std::string& hint)
{
    namespace fs = std::filesystem;

    // 1. Explicit hint from command line (--assets=...)
    if (!hint.empty()) {
        fs::path p = fs::u8path(hint);
        if (is_valid_assets_dir(p)) {
            return normalize_path(p);
        }
    }

    // 2. Standard project environment variables
    if (const char* env = std::getenv("MUSIC_MCP_RESOURCES")) {
        fs::path p = fs::u8path(env);
        if (is_valid_assets_dir(p)) {
            return normalize_path(p);
        }
    }
    if (const char* env = std::getenv("MUSESCORE_ASSETS")) {
        fs::path p = fs::u8path(env);
        if (is_valid_assets_dir(p)) {
            return normalize_path(p);
        }
    }

    // 3. Compile-time definition if specified in xmake
#ifdef MUSESCORE_RUNTIME_ASSETS
    {
        fs::path p = fs::u8path(MUSESCORE_RUNTIME_ASSETS);
        if (is_valid_assets_dir(p)) {
            return normalize_path(p);
        }
    }
#endif

    // 4. Relative paths from current working directory
    const std::vector<fs::path> candidates = {
        fs::u8path(".runtime/assets"),
        fs::u8path("../.runtime/assets"),
        fs::u8path("../../.runtime/assets"),
    };
    for (const auto& candidate : candidates) {
        if (is_valid_assets_dir(candidate)) {
            return normalize_path(candidate);
        }
    }

    return "";
}

std::string find_engraving_data_dir(const std::string& hint)
{
    namespace fs = std::filesystem;

    // 1. Explicit hint from command line (--engraving-data=... or --data-root=...)
    if (!hint.empty()) {
        fs::path p = fs::u8path(hint);
        if (is_valid_engraving_data_dir(p)) {
            return normalize_path(p);
        }
    }

    // 2. Environment variables
    if (const char* env = std::getenv("ENGRAVING_TESTS_DATA_ROOT")) {
        fs::path p = fs::u8path(env);
        if (is_valid_engraving_data_dir(p)) {
            return normalize_path(p);
        }
    }
    if (const char* env = std::getenv("MUSE_TEST_DATA_ROOT")) {
        fs::path p = fs::u8path(env);
        if (is_valid_engraving_data_dir(p)) {
            return normalize_path(p);
        }
    }

    // 3. Compile-time definition if specified in xmake
#ifdef ENGRAVING_TESTS_DATA_ROOT
    {
        fs::path p = fs::u8path(ENGRAVING_TESTS_DATA_ROOT);
        if (is_valid_engraving_data_dir(p)) {
            return normalize_path(p);
        }
    }
#endif

    // 4. Relative paths from current working directory
    const std::vector<fs::path> candidates = {
        fs::u8path("vendor/musescore/src/engraving/tests"),
        fs::u8path("../vendor/musescore/src/engraving/tests"),
        fs::u8path("../../vendor/musescore/src/engraving/tests"),
    };
    for (const auto& candidate : candidates) {
        if (is_valid_engraving_data_dir(candidate)) {
            return normalize_path(candidate);
        }
    }

    return "";
}

void setup_test_environment(int* argc, char** argv)
{
    std::string assets_hint;
    std::string engraving_hint;

    // Static buffer to hold modified strings (e.g. unquoted filter arguments)
    static std::vector<std::string> s_retained_strings;

    if (argc && argv && *argc > 0) {
        std::vector<char*> remaining;
        remaining.reserve(static_cast<size_t>(*argc) + 1);
        remaining.push_back(argv[0]);

        for (int i = 1; i < *argc; ++i) {
            std::string arg = argv[i] ? argv[i] : "";
            if (arg.rfind("--assets=", 0) == 0) {
                assets_hint = arg.substr(9);
            } else if (arg.rfind("--engraving-data=", 0) == 0) {
                engraving_hint = arg.substr(17);
            } else if (arg.rfind("--data-root=", 0) == 0) {
                engraving_hint = arg.substr(12);
            } else {
                if (arg.rfind("--gtest_filter=", 0) == 0) {
                    auto eq = arg.find('=');
                    if (eq != std::string::npos && eq + 1 < arg.size()) {
                        std::string val = arg.substr(eq + 1);
                        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                            s_retained_strings.push_back(arg.substr(0, eq + 1) + val.substr(1, val.size() - 2));
                            remaining.push_back(const_cast<char*>(s_retained_strings.back().c_str()));
                            continue;
                        }
                    }
                }
                remaining.push_back(argv[i]);
            }
        }
        remaining.push_back(nullptr);

        *argc = static_cast<int>(remaining.size() - 1);
        for (int i = 0; i < *argc; ++i) {
            argv[i] = remaining[static_cast<size_t>(i)];
        }
        argv[*argc] = nullptr;
    }

    TestEnvironmentConfig config;
    config.assets_dir = find_assets_dir(assets_hint);
    config.engraving_data_dir = find_engraving_data_dir(engraving_hint);

    setup_test_environment(config);
}

void setup_test_environment(const TestEnvironmentConfig& config)
{
    muse::runtime::mainThreadId();
    muse::runtime::setThreadName("main");

#if defined(TEST_HAS_ENGRAVING)
    if (config.assets_dir.empty()) {
        throw std::runtime_error("Engraving test environment requires assets (.runtime/assets not found). "
                                 "Pass --assets=<path> or set MUSIC_MCP_RESOURCES.");
    }
    if (config.engraving_data_dir.empty()) {
        throw std::runtime_error("Engraving test environment requires data root (vendor/musescore/src/engraving/tests not found). "
                                 "Pass --engraving-data=<path> or set ENGRAVING_TESTS_DATA_ROOT.");
    }

    // 1. Initialize native runtime (sets up NativeFileSystem, font services, NoQtApplication,
    //    EngravingModule, MpeModule, MusicXmlConfiguration, and loads instruments.xml).
    music::initialize_native_runtime(config.assets_dir);

    // 2. Set MScore test flags
    mu::engraving::MScore::testMode = true;
    mu::engraving::MScore::noGui = true;

    // 3. Configure ScoreRW root path for resolving relative test score paths (e.g. playback/...)
    mu::engraving::ScoreRW::setRootPath(muse::String::fromStdString(config.engraving_data_dir));

    // 4. Register mock engraving configuration matching upstream engraving/tests/environment.cpp
    using ECMock = ::testing::NiceMock<mu::engraving::EngravingConfigurationMock>;
    auto configurator = std::shared_ptr<ECMock>(new ECMock(), [](ECMock*) {});
    // Lives for the whole process like upstream's test environment; never deleted on purpose.
    ::testing::Mock::AllowLeak(configurator.get());
    ON_CALL(*configurator, displayedDefaultColor(::testing::_)).WillByDefault(::testing::Return(muse::draw::Color::BLACK));
    ON_CALL(*configurator, defaultColor()).WillByDefault(::testing::Return(muse::draw::Color::BLACK));
    ON_CALL(*configurator, debuggingOptions()).WillByDefault(::testing::ReturnRef(s_debugOpt));
    ON_CALL(*configurator, allowReadingImagesFromOutsideMscz()).WillByDefault(::testing::Return(true));

    muse::modularity::globalIoc()->unregister<mu::engraving::IEngravingConfiguration>("utests");
    muse::modularity::globalIoc()->registerExport<mu::engraving::IEngravingConfiguration>("utests", configurator);
#else
    (void)config;
#endif
}

void teardown_test_environment()
{
#if defined(TEST_HAS_ENGRAVING)
    muse::modularity::globalIoc()->unregister<mu::engraving::IEngravingConfiguration>("utests");
#endif
}

} // namespace music::test
