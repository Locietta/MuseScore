-- Tests of the engine and of the engrave library (dev-only).
--
--   engraving_playback_tests, mpe_tests
--       upstream's own Qt-free GoogleTest suites, compiled against the engine
--       and started by the harness in engrave/tests (gtest_main.cpp,
--       test_environment.cpp), which initialises the engrave runtime;
--   *_test, semantic_dump
--       regression binaries of the engrave adapters, driven by musescore-mcp's
--       scripts/run_lanes.py;
--   engrave_api_test, example_embedder
--       the public API's tests and its leak test, an embedder that sees
--       nothing but engrave/include;
--   engrave_handles_test, engrave_describe_test, engrave_read_test,
--   engrave_ops_test, engrave_op_policy_test
--       the library's tests of handles, the catalogue, reads, edits and the
--       edit harness, and of the policy table against the harness evidence;
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
        set_default(false)
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

-- Dev-only probe used by the engine_audio lane; built explicitly.
add_adapter_binary("engine_audio_probe", "engine_audio_probe.cpp")
target("engine_audio_probe")
    set_default(false)
    if is_plat("windows") then
        -- PeakWorkingSetSize for the probe's memory report.
        add_syslinks("psapi")
    end
target_end()

-------------------------------------------------------------------------------
-- engrave public API
-------------------------------------------------------------------------------

-- The API's own tests.  Besides the public headers they include only
-- engrave/src/api/sniff.h, which is std-only as well.
target("engrave_api_test")
    set_kind("binary")
    set_default(false)
    add_deps("engrave")
    add_packages("gtest")
    add_includedirs(source_root)
    add_defines('ENGRAVE_TEST_SOURCE_ROOT="' .. source_root:gsub("\\", "/") .. '"')
    add_files(path.join(tests_dir, "api_test.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    end
target_end()

-- The handle tests (H1-H13) reach the score through the library's internal headers:
-- the public read and edit API they will eventually use does not exist yet.
target("engrave_handles_test")
    set_kind("binary")
    set_default(false)
    add_deps("engrave")
    add_packages("gtest")
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_defines(table.unpack(ENGRAVE_GLOBAL_DEFINES))
    add_includedirs(ENGRAVE_ADAPTER_INCLUDES)
    add_includedirs("$(builddir)/xmake-generated")
    add_defines('ENGRAVE_TEST_SOURCE_ROOT="' .. source_root:gsub("\\", "/") .. '"')
    add_files(path.join(tests_dir, "handles_test.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    else
        add_links("m")
    end
target_end()

-- Guitar Pro and PowerTab import of upstream's test data: every score file of the
-- importer's data directory must either open to a score or return an Error.
target("engrave_mnx_import_test")
    set_kind("binary")
    set_default(false)
    add_deps("engrave")
    add_packages("gtest")
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_defines(table.unpack(ENGRAVE_GLOBAL_DEFINES))
    add_includedirs(ENGRAVE_ADAPTER_INCLUDES)
    add_includedirs("$(builddir)/xmake-generated")
    add_defines('ENGRAVE_TEST_SOURCE_ROOT="' .. source_root:gsub("\\", "/") .. '"')
    add_files(path.join(tests_dir, "mnx_import_test.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    else
        add_links("m")
    end
target_end()

target("engrave_gp_import_test")
    set_kind("binary")
    set_default(false)
    add_deps("engrave")
    add_packages("gtest")
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_defines(table.unpack(ENGRAVE_GLOBAL_DEFINES))
    add_includedirs(ENGRAVE_ADAPTER_INCLUDES)
    add_includedirs("$(builddir)/xmake-generated")
    add_defines('ENGRAVE_TEST_SOURCE_ROOT="' .. source_root:gsub("\\", "/") .. '"')
    add_files(path.join(tests_dir, "gp_import_test.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    else
        add_links("m")
    end
target_end()

-- The catalogue's tests: the value codec, the describe tables against live
-- corpus instances, and the generator diff.  They reach into the engine (P_TYPE,
-- Pid, MasterScore), so they link the library and its internal headers.
target("engrave_describe_test")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "engrave")
    add_packages("gtest")
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_defines(table.unpack(ENGRAVE_GLOBAL_DEFINES))
    add_defines('ENGRAVE_TEST_SOURCE_ROOT="' .. source_root:gsub("\\", "/") .. '"')
    add_includedirs(ENGRAVE_ADAPTER_INCLUDES)
    add_includedirs(ENGRAVE_INTERNAL_INCLUDES)
    add_includedirs("$(builddir)/xmake-generated")
    add_files(path.join(tests_dir, "describe_test.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    end
target_end()

-- The read tests: outline, query and get against the A2 semantic dump, the metadata
-- export and the positions export, which walk the same score independently.
target("engrave_read_test")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "engrave")
    add_packages("gtest")
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_defines(table.unpack(ENGRAVE_GLOBAL_DEFINES))
    add_includedirs(ENGRAVE_ADAPTER_INCLUDES)
    add_includedirs(ENGRAVE_INTERNAL_INCLUDES)
    add_includedirs("$(builddir)/xmake-generated")
    add_files(path.join(tests_dir, "read_test.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    else
        add_links("m")
    end
target_end()

-- The edit harness (design 搂5.3): Document::edit, the snapshot round trip and the
-- per-operation evidence.  A group of operations adds its cases as a file under
-- harness/cases/; the folder glob picks it up.
target("engrave_ops_test")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "engrave")
    add_packages("gtest")
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_defines(table.unpack(ENGRAVE_GLOBAL_DEFINES))
    add_defines('ENGRAVE_TEST_SOURCE_ROOT="' .. source_root:gsub("\\", "/") .. '"')
    add_includedirs(ENGRAVE_ADAPTER_INCLUDES)
    add_includedirs(ENGRAVE_INTERNAL_INCLUDES)
    add_includedirs("$(builddir)/xmake-generated")
    add_files(path.join(tests_dir, "harness", "**.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    else
        add_links("m")
    end
target_end()

-- The policy table against the checked-in evidence; it needs no score.
target("engrave_op_policy_test")
    set_kind("binary")
    set_default(false)
    add_deps("engrave")
    add_packages("gtest")
    add_includedirs(ENGRAVE_ADAPTER_INCLUDES)
    add_defines('ENGRAVE_TEST_SOURCE_ROOT="' .. source_root:gsub("\\", "/") .. '"')
    add_files(path.join(tests_dir, "op_policy_test.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    end
target_end()

-- The output tests: render against the A2 render goldens (render_goldens.json),
-- page_count and the render limits, plus the audio API: the encoders, the
-- SoundFont list and the per-track font, the reverb switch, sink chunking and
-- D4 (a second render in one process equals a fresh-process render, which the
-- test checks by starting engrave_output_probe).
target("engrave_output_test")
    set_kind("binary")
    set_default(false)
    add_deps("engrave")
    add_packages("gtest", "muse-zlib")
    add_defines('ENGRAVE_TEST_SOURCE_ROOT="' .. source_root:gsub("\\", "/") .. '"')
    add_files(
        path.join(tests_dir, "output_test.cpp"),
        path.join(tests_dir, "audio_test.cpp"),
        path.join(tests_dir, "digest.cpp")
    )
    if is_plat("windows") then
        add_files(utf8_manifest)
    else
        add_links("m")
    end
target_end()

-- Renders one score through the public API (SVG/PNG/PDF or an audio file), for
-- scripts/test_a4_output.py in the superproject and for the D4 test above.
target("engrave_output_probe")
    set_kind("binary")
    set_default(false)
    add_deps("engrave")
    add_files(path.join(tests_dir, "output_probe.cpp"))
    if is_plat("windows") then
        add_files(utf8_manifest)
    end
target_end()

-- The leak test: an embedder compiled with the public include root as its
-- only include path and none of the project's defines, so it builds only
-- while the public headers stay std-only.  The compile is done here rather
-- than by the target's flags because project-scope settings (engine include
-- roots, feature defines, package and dependency include paths) would
-- otherwise reach it.
local public_includes = ENGRAVE_PUBLIC_INCLUDES
target("example_embedder")
    set_kind("binary")
    set_default(false)
    add_deps("engrave")
    add_files(path.join(tests_dir, "example_embedder.cpp"))
    on_build_file(function (target, sourcefile, opt)
        import("core.project.depend")
        import("core.tool.compiler")
        import("utils.progress")
        local objectfile = target:objectfile(sourcefile)
        local headers = {}
        for _, dir in ipairs(public_includes) do
            table.join2(headers, os.files(path.join(dir, "engrave", "**.h")))
        end
        local compile = {configs = {
            languages = target:get("languages"),
            runtimes = target:get("runtimes"),
            exceptions = target:get("exceptions"),
            includedirs = public_includes,
        }}
        depend.on_changed(function ()
            progress.show(opt.progress, "${color.build.object}compiling.leak-test %s", sourcefile)
            vprint(compiler.compcmd(sourcefile, objectfile, compile))
            os.mkdir(path.directory(objectfile))
            compiler.compile(sourcefile, objectfile, compile)
        end, {dependfile = target:dependfile(objectfile), files = table.join(sourcefile, headers)})
    end)
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

-- The complete engine gate closure; no MCP host dependency.
target("c0_gate")
    set_kind("phony")
    set_default(false)
    add_deps("engrave_api_test", "engrave_handles_test", "engrave_describe_test",
        "engrave_read_test", "engrave_ops_test", "engrave_op_policy_test",
        "engrave_gp_import_test", "engrave_mnx_import_test", "engrave_output_test", "engrave_output_probe",
        "example_embedder", "engraving_playback_tests", "mpe_tests",
        "score_edit_test", "score_audio_test", "score_positions_test",
        "excerpts_test", "archive_test", "assets_test", "semantic_dump", "engine_audio_probe", "diagnostics_test")
target_end()

-- Diagnostic control flow is independent of score initialization.
target("diagnostics_test")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "muse_global")
    add_packages("gtest")
    add_includedirs(ENGRAVE_INTERNAL_INCLUDES)
    add_includedirs(path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global"))
    add_includedirs("$(builddir)/xmake-generated")
    add_files(path.join(tests_dir, "diagnostics_test.cpp"))
target_end()
