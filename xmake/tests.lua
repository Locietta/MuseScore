-- Tests of the engine and of the engrave library (dev-only).
--
--   engraving_playback_tests, mpe_tests
--       upstream's own Qt-free GoogleTest suites, compiled against the engine
--       and started by the harness in engrave/tests (gtest_main.cpp,
--       test_environment.cpp), which initialises the engrave runtime;
--   *_test, semantic_dump
--       regression binaries of the engrave adapters, driven by musescore-mcp's
--       scripts/run_lanes.py;
--   noqt_probe, engine_audio_probe, audio_engine_link_check
--       dev probes of the runtime, the audio driver and the audio engine link
--       closure.
--
-- Every binary lands in the including project's build directory under its
-- own name.

local fork_root = path.directory(os.scriptdir())
local source_root = path.absolute(ENGRAVE_SOURCE_ROOT or fork_root)
local tests_dir = path.join(source_root, "engrave", "tests")
local utf8_manifest = path.join(tests_dir, "utf8.manifest")

add_requires("gtest v1.17.0", {system = false, configs = {shared = false}})

local engraving_tests_dir = path.join(source_root, "src", "engraving", "tests")

-------------------------------------------------------------------------------
-- Upstream Engraving Playback unit tests (Qt-free)
-------------------------------------------------------------------------------

target("engraving_playback_tests")
    set_kind("binary")
    set_default(false)
    set_basename("engraving_playback_tests")
    add_deps("muse_generated", "engrave")
    add_packages("gtest")
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_defines("ENGRAVING_NO_ACCESSIBILITY", "KORS_PROFILER_ENABLED")
    add_defines("TEST_HAS_ENGRAVING=1")
    add_defines('ENGRAVING_TESTS_DATA_ROOT="' .. engraving_tests_dir:gsub("\\", "/") .. '"')
    add_includedirs(
        path.join(source_root, "src", "engraving"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global"),
        path.join(source_root, "src", "engraving", "infrastructure"),
        path.join(source_root, "src", "engraving", "thirdparty", "dtl"),
        source_root,
        engraving_tests_dir,
        path.join(source_root, "muse", "framework"),
        tests_dir,
        "$(builddir)/xmake-generated"
    )
    add_includedirs(ENGRAVE_INTERNAL_INCLUDES)
    add_files(
        path.join(engraving_tests_dir, "playback/playbackeventsrendering_tests.cpp"),
        path.join(engraving_tests_dir, "playback/playbackmodel_tests.cpp"),
        path.join(engraving_tests_dir, "playback/playbackcontext_tests.cpp"),
        path.join(engraving_tests_dir, "playback/bendsrenderer_tests.cpp"),
        path.join(engraving_tests_dir, "repeat_tests.cpp"),
        path.join(engraving_tests_dir, "parts_tests.cpp"),
        path.join(engraving_tests_dir, "savehiddenparts_tests.cpp"),
        path.join(engraving_tests_dir, "utils/scorecomp.cpp"),
        path.join(engraving_tests_dir, "utils/scorerw.cpp"),
        path.join(engraving_tests_dir, "utils/testutils.cpp"),
        path.join(tests_dir, "gtest_main.cpp"),
        path.join(tests_dir, "test_environment.cpp")
    )
    if is_plat("windows") then
        add_files(utf8_manifest)
    end
target_end()

-------------------------------------------------------------------------------
-- Upstream MPE unit tests (Qt-free)
-------------------------------------------------------------------------------

target("mpe_tests")
    set_kind("binary")
    set_default(false)
    set_basename("mpe_tests")
    add_deps("muse_generated", "muse_mpe")
    add_packages("gtest")
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_defines("KORS_PROFILER_ENABLED")
    add_includedirs(
        path.join(source_root, "muse", "framework", "mpe"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global"),
        path.join(source_root, "muse", "framework"),
        tests_dir,
        "$(builddir)/xmake-generated"
    )
    add_files(
        path.join(source_root, "muse", "framework/mpe/tests/singlenotearticulationstest.cpp"),
        path.join(source_root, "muse", "framework/mpe/tests/multinotearticulationstest.cpp"),
        path.join(tests_dir, "gtest_main.cpp"),
        path.join(tests_dir, "test_environment.cpp")
    )
    if is_plat("windows") then
        add_files(utf8_manifest)
    else
        add_links("m")
    end
target_end()

-------------------------------------------------------------------------------
-- engrave adapter tests and probes
-------------------------------------------------------------------------------

local function add_adapter_binary(name, file)
    target(name)
        set_kind("binary")
        add_deps("muse_generated", "engrave")
        add_defines(table.unpack(ENGRAVE_DEFINES))
        add_defines(table.unpack(ENGRAVE_GLOBAL_DEFINES))
        add_includedirs(ENGRAVE_ADAPTER_INCLUDES)
        add_includedirs("$(builddir)/xmake-generated")
        add_files(path.join(tests_dir, file))
        if is_plat("windows") then
            add_files(utf8_manifest)
        end
    target_end()
end

add_adapter_binary("archive_test", "archive_test.cpp")
add_adapter_binary("assets_test", "assets_test.cpp")
add_adapter_binary("excerpts_test", "excerpts_test.cpp")
add_adapter_binary("score_audio_test", "score_audio_test.cpp")
add_adapter_binary("score_edit_test", "score_edit_test.cpp")
add_adapter_binary("score_positions_test", "score_positions_test.cpp")
add_adapter_binary("semantic_dump", "semantic_dump.cpp")
add_adapter_binary("noqt_probe", "noqt_probe.cpp")

-- The IoC contract test calls a real entry point that requires the file-system
-- service (initialize_engine_audio, which guards on it in engine_audio.cpp);
-- ioc_tu2.cpp is its second translation unit.
add_adapter_binary("noqt_support_test", "noqt_support_test.cpp")
target("noqt_support_test")
    add_files(path.join(tests_dir, "ioc_tu2.cpp"))
target_end()

-- Dev-only probe used by the engine_audio lane; built explicitly.
add_adapter_binary("engine_audio_probe", "engine_audio_probe.cpp")
target("engine_audio_probe")
    set_default(false)
    if is_plat("windows") then
        -- PeakWorkingSetSize for the probe's memory report.
        add_syslinks("psapi")
    end
target_end()

-- Dev-only closure check: the audio engine links on its own, without engrave.
target("audio_engine_link_check")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "muse_audio_engine")
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_defines(ENGRAVE_AUDIO_ENGINE.defines)
    add_includedirs(ENGRAVE_AUDIO_ENGINE.includes)
    add_includedirs(ENGRAVE_INTERNAL_INCLUDES)
    add_includedirs("$(builddir)/xmake-generated")
    add_files(path.join(tests_dir, "audio_engine_link_check.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    else
        add_links("m")
    end
target_end()
