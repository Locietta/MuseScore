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

-- A project-scope xmake script has no raise(); report a broken hook by
-- defining a target whose on_load raises at configure time.
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

-- gtest comes from xmake-repo: MuseScore 5.0 no longer vendors googletest, and
-- the version muse_deps fetches is not reproduced here.  These targets never
-- run in a release build.
add_requires("gtest v1.17.0", {system = false, configs = {shared = false}})

local manifest = ENGRAVE_MANIFEST
local function expand(item)
    return (item:gsub("%${([A-Z_]+)}", function(name)
        if name == "PROJECT_ROOT" or name == "SOURCE_ROOT" then
            return source_root
        end
        print("error: unknown manifest placeholder: " .. name)
        return item
    end))
end

local function expand_list(items)
    local result = {}
    for _, item in ipairs(items or {}) do
        table.insert(result, expand(item))
    end
    return result
end

-- Test data: the engine's own tests directory, passed to the harness through
-- ENGRAVING_TESTS_DATA_ROOT and at run time by --engraving-data.
local engraving_tests_dir = path.join(source_root, "src", "engraving", "tests")

-------------------------------------------------------------------------------
-- Upstream Engraving Playback unit tests (Qt-free)
-------------------------------------------------------------------------------

target("engraving_playback_tests")
    set_kind("binary")
    -- Dev-only upstream tests; built explicitly by the upstream_gtest lane.
    set_default(false)
    set_basename("engraving_playback_tests")
    add_deps("muse_generated", runtime_target)
    add_packages("gtest")
    add_defines(table.unpack(manifest.defines))
    add_defines(manifest.targets.engraving.defines)
    add_defines("TEST_HAS_ENGRAVING=1")
    add_defines('ENGRAVING_TESTS_DATA_ROOT="' .. engraving_tests_dir:gsub("\\", "/") .. '"')
    add_includedirs(expand_list(manifest.targets.engraving.includes))
    add_includedirs(source_root)
    add_includedirs(engraving_tests_dir)
    add_includedirs(path.join(source_root, "muse/framework"))
    add_includedirs(harness_dir)
    add_includedirs("$(builddir)/xmake-generated")
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
    -- Dev-only upstream tests; built explicitly by the upstream_gtest lane.
    set_default(false)
    set_basename("mpe_tests")
    add_deps("muse_generated", "muse_mpe")
    add_packages("gtest")
    add_defines(table.unpack(manifest.defines))
    add_defines(manifest.targets.muse_mpe.defines)
    add_includedirs(expand_list(manifest.targets.muse_mpe.includes))
    add_includedirs(path.join(source_root, "muse/framework"))
    add_includedirs(harness_dir)
    add_includedirs("$(builddir)/xmake-generated")
    add_files(
        path.join(source_root, "muse/framework/mpe/tests/singlenotearticulationstest.cpp"),
        path.join(source_root, "muse/framework/mpe/tests/multinotearticulationstest.cpp"),
        path.join(harness_dir, "gtest_main.cpp"),
        path.join(harness_dir, "test_environment.cpp")
    )
    if is_plat("windows") then
        if utf8_manifest then add_files(utf8_manifest) end
    else
        add_links("m")
    end
target_end()
