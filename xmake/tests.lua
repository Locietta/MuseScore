-- Upstream GoogleTest suites (dev-only).
--
-- engraving_playback_tests and mpe_tests compile upstream's own Qt-free tests
-- against the engine.  They are part of the engine's build, but they also need
-- a Qt-free runtime: a file system, font services, the application object and
-- the engraving/MPE modules, plus the test harness that initialises them.  That
-- runtime lives in the musescore-mcp superproject's native/ adapters until S1
-- moves it into the library, so the including project supplies it here:
--
--   ENGRAVE_TEST_SUPPORT = {
--       runtime_target = "score_engine",     -- target providing the runtime
--       harness_dir = "<dir>",               -- holds gtest_main.cpp and
--                                            -- test_environment.cpp
--       utf8_manifest = "<file>",            -- optional: Windows UTF-8 manifest
--   }
--
-- Without it (the fork's standalone build) the suites are not defined.  In S1
-- the harness and its runtime sources move next to this file and the hook goes
-- away.

local function support_failure(message)
    target("engrave_tests_check")
        set_kind("phony")
        on_load(function ()
            raise("%s", message)
        end)
    target_end()
end

if not ENGRAVE_TEST_SUPPORT then
    return
end

local fork_root = path.directory(os.scriptdir())
local source_root = path.absolute(ENGRAVE_SOURCE_ROOT or fork_root)
local harness_dir = ENGRAVE_TEST_SUPPORT.harness_dir
local runtime_target = ENGRAVE_TEST_SUPPORT.runtime_target
local utf8_manifest = ENGRAVE_TEST_SUPPORT.utf8_manifest

if not harness_dir or not os.isfile(path.join(harness_dir, "gtest_main.cpp")) then
    support_failure("ENGRAVE_TEST_SUPPORT.harness_dir must contain gtest_main.cpp")
    return
end
if not runtime_target then
    support_failure("ENGRAVE_TEST_SUPPORT.runtime_target is required")
    return
end

add_requires("gtest v1.17.0", {system = false, configs = {shared = false}})

local engraving_tests_dir = path.join(source_root, "src", "engraving", "tests")

-------------------------------------------------------------------------------
-- Upstream Engraving Playback unit tests (Qt-free)
-------------------------------------------------------------------------------

target("engraving_playback_tests")
    set_kind("binary")
    set_default(false)
    set_basename("engraving_playback_tests")
    add_deps("muse_generated", runtime_target)
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
        harness_dir,
        "$(builddir)/xmake-generated"
    )
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
        path.join(harness_dir, "gtest_main.cpp"),
        path.join(harness_dir, "test_environment.cpp")
    )
    add_links("cairo", "z")
    if is_plat("windows") then
        if utf8_manifest then add_files(utf8_manifest) end
    else
        add_links("m")
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
        harness_dir,
        "$(builddir)/xmake-generated"
    )
    add_files(
        path.join(source_root, "muse", "framework/mpe/tests/singlenotearticulationstest.cpp"),
        path.join(source_root, "muse", "framework/mpe/tests/multinotearticulationstest.cpp"),
        path.join(harness_dir, "gtest_main.cpp"),
        path.join(harness_dir, "test_environment.cpp")
    )
    if is_plat("windows") then
        if utf8_manifest then add_files(utf8_manifest) end
    else
        add_links("m")
    end
target_end()
