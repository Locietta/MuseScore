-- Engine targets of the engrave library (MuseScore 5.0 source tree).
--
-- This file is the engine's build interface.  The fork's own xmake.lua includes
-- it for the standalone build; the musescore-mcp superproject includes it from
-- its pinned vendor/musescore submodule (or its --musescore_source override),
-- which is why it sets no project identity, toolchain or package policy: those
-- belong to the including project.
--
-- What it does own:
--   * the pinned third-party package recipes the engine targets need (declared
--     here with add_requires, so any project that includes this file resolves
--     them without repeating the pins);
--   * the checked-in source manifest (xmake/manifest.lua) and the generated
--     framework configuration header;
--   * the engine module targets: muse_global, muse_draw, muse_mpe, engraving,
--     the iex_* importers, native_fluidsynth and muse_audio_engine.
--
-- The including project may set ENGRAVE_SOURCE_ROOT to build the engine
-- sources from a checkout other than this one; the fork itself builds from its
-- own tree.  It may also set ENGRAVE_TEST_SUPPORT (see xmake/tests.lua) when it
-- provides the Qt-free test runtime.

local fork_root = path.directory(os.scriptdir())
local source_root = path.absolute(ENGRAVE_SOURCE_ROOT or fork_root)

---------------------------------------------------------------------------
-- Third-party dependencies
---------------------------------------------------------------------------
-- Every library is a local recipe with a pinned archive and SHA-256, so no
-- build input depends on the xmake-repo revision a machine has: pugixml/opus/
-- opusenc/picojson because xmake-repo's versions or layouts differ,
-- freetype/lame/utfcpp because xmake-repo carried no version pin of its own
-- (see the recipe headers for the copied upstream pins).  Only build tools
-- (cmake, nasm, pkgconf) and the dev-only gtest still come from xmake-repo.
-- freetype: muse_deps builds it static with PNG/bzip2/zlib/harfbuzz disabled.
local package_dir = path.join(fork_root, "xmake", "packages")
includes(path.join(package_dir, "freetype.lua"))
includes(path.join(package_dir, "harfbuzz.lua"))
includes(path.join(package_dir, "flac.lua"))
includes(path.join(package_dir, "lame.lua"))
includes(path.join(package_dir, "opus.lua"))
includes(path.join(package_dir, "opusenc.lua"))
includes(path.join(package_dir, "fdk-aac.lua"))
includes(path.join(package_dir, "pugixml.lua"))
includes(path.join(package_dir, "utfcpp.lua"))
includes(path.join(package_dir, "picojson.lua"))

-- Third-party engine dependencies, pinned to the versions MuseScore 5.0 builds
-- against (muse/buildscripts/muse_deps.ref = 39c897ed, recipes/):
--   freetype 2.14.1, harfbuzz 12.3.0, flac 1.4.3, lame 3.100, opus 1.5.2,
--   opusenc 0.2.1, fdk-aac 2.0.3, pugixml 1.15, utfcpp 4.1.1, picojson 111c9be.
add_requires("freetype 2.14.1", {system = false, configs = {
    zlib = false, png = false, bzip2 = false, woff2 = false, harfbuzz = false}})
add_requires("muse-harfbuzz 12.3.0", {system = false})
add_requires("muse-flac 1.4.3", {system = false})
add_requires("lame 3.100", {system = false})
add_requires("muse-opus 1.5.2", {system = false})
add_requires("muse-opusenc 0.2.1", {system = false})
add_requires("muse-fdk-aac 2.0.3", {system = false})
add_requires("muse-pugixml 1.15", {system = false})
add_requires("utfcpp v4.1.1", {system = false})
add_requires("muse-picojson 111c9be", {system = false})

---------------------------------------------------------------------------
-- Pinned source manifest
---------------------------------------------------------------------------
-- xmake/manifest.lua is generated from the engine's own CMake source lists by
-- tools/capture_xmake_manifest.py and committed, so a normal build never runs
-- CMake or the generator.
--
-- A project-scope xmake script has no raise()/error(); a fatal input problem is
-- reported by defining a target whose on_load raises at configure time and
-- returning from this file.  Do not turn this into a silent continue: a build
-- without the manifest has nothing to compile.
local function input_failure(message)
    target("engrave_build_check")
        set_kind("phony")
        on_load(function ()
            raise("%s", message)
        end)
    target_end()
end

local manifest_path = path.join(fork_root, "xmake", "manifest.lua")
if not os.isfile(manifest_path) then
    input_failure("xmake/manifest.lua is missing. Run once: python tools/capture_xmake_manifest.py")
    return
end
includes(manifest_path)
local manifest = MUSESCORE_XMAKE_MANIFEST
if not manifest or not manifest.targets then
    input_failure("xmake/manifest.lua did not define MUSESCORE_XMAKE_MANIFEST")
    return
end

-- The manifest is engine data, so a missing target means the generator and
-- this file disagree; say which one instead of linking an empty library.
local engine_targets = {
    "muse_global", "muse_draw", "muse_mpe", "engraving", "iex_musicxml",
    "iex_guitarpro", "iex_midi_import", "native_fluidsynth", "muse_audio_engine",
    "noqt_support",
}
for _, name in ipairs(engine_targets) do
    if not manifest.targets[name] then
        input_failure("the engine manifest has no target: " .. name)
        return
    end
end

-- Resolve the portable placeholders used by the captured manifest.  The
-- manifest is engine-only, so every path is a SOURCE_ROOT path; the
-- PROJECT_ROOT form is accepted for manifests captured by older generators.
-- The generator's self-check guarantees only those two exist, so an unknown one
-- is reported and then fails as a missing file.
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

local function add_manifest_target(name)
    local spec = manifest.targets[name]
    local sources = expand_list(spec.sources)
    if #sources > 0 then
        add_files(sources)
    end
    -- The Qt-free feature selection changes class layouts and interfaces, so
    -- the manifest applies it to every engine target.
    if manifest.defines and #manifest.defines > 0 then
        add_defines(table.unpack(manifest.defines))
    end
    if spec.defines and #spec.defines > 0 then
        add_defines(spec.defines)
    end
    add_includedirs(expand_list(spec.includes))
    add_includedirs("$(builddir)/xmake-generated")
    return spec
end

---------------------------------------------------------------------------
-- Generated framework configuration header
---------------------------------------------------------------------------
-- The engine's framework config header. 5.0 moved the template into the
-- absorbed framework tree (muse/framework/cmake); the #cmakedefine shape is
-- unchanged, but the variables it substitutes changed
-- (MUSE_APP_INSTALL_* replaced MUSE_APP_INSTALL_NAME, AUDIO_WORKER_MODE became
-- AUDIO_HYBRID_MODE, MUSE_MODULE_*_COMPAT/MULTICONTEXT_WIP are gone).

local function read_version(os, io)
    local text = io.readfile(path.join(source_root, "version.cmake")) or ""
    local function value(name)
        return text:match("set%(" .. name .. '%s+"([^"]*)"%)')
    end
    local major = value("MUSE_APP_VERSION_MAJOR") or "0"
    local minor = value("MUSE_APP_VERSION_MINOR") or "0"
    local patch = value("MUSE_APP_VERSION_PATCH") or "0"
    local revision = ""
    local function normalized_path(value)
        local result = path.normalize(tostring(value or "")):gsub("\\", "/")
        if os.host() == "windows" then
            result = result:lower()
        end
        return result
    end
    local root_args = {"-C", source_root, "rev-parse", "--show-toplevel"}
    -- Probe first: iorunv raises in the xmake script sandbox when git is absent
    -- or the source tree is not a checkout, and the revision must fall back.
    local root_probe = os.execv("git", root_args,
        {try = true, stdout = os.nuldev(), stderr = os.nuldev()})
    if root_probe ~= 0 then
        print("warning: cannot read the engine git revision from " .. source_root
            .. "; MUSE_APP_REVISION will be empty")
    else
        local top_level = tostring(os.iorunv("git", root_args) or ""):gsub("%s+", "")
        if normalized_path(top_level) ~= normalized_path(source_root) then
            -- A plain directory inside another checkout must not inherit the
            -- outer repository's SHA as the engine revision.
            print("warning: " .. source_root .. " is not a git checkout root (repository root '"
                .. top_level .. "'); MUSE_APP_REVISION will be empty")
        else
            -- Full SHA + first 7 characters: --short can grow when the prefix
            -- is ambiguous, which would make the reported revision unstable.
            local head_args = {"-C", source_root, "rev-parse", "HEAD"}
            local code = os.execv("git", head_args,
                {try = true, stdout = os.nuldev(), stderr = os.nuldev()})
            if code == 0 then
                local sha = tostring(os.iorunv("git", head_args) or ""):gsub("%s+", "")
                revision = sha:sub(1, 7)
                if not revision:match("^%x%x%x%x%x%x%x$") then
                    print("warning: unexpected engine git revision '" .. sha
                        .. "'; MUSE_APP_REVISION will be empty")
                    revision = ""
                end
            else
                print("warning: cannot read the engine git revision from " .. source_root
                    .. "; MUSE_APP_REVISION will be empty")
            end
        end
    end
    local version_label = value("MUSE_APP_VERSION_LABEL")
    local result = {
        MUSE_APP_NAME_HUMAN_READABLE = value("MUSE_APP_NAME_HUMAN_READABLE") or "MuseScore Studio",
        MUSE_APP_NAME_MACHINE_READABLE = value("MUSE_APP_NAME_MACHINE_READABLE") or "MuseScoreStudio",
        -- Upstream SetupConfigure.cmake leaves MUSE_APP_UNSTABLE undefined for
        -- MUSE_APP_BUILD_MODE=release; debug and releasedbg stay unstable.
        MUSE_APP_UNSTABLE = not is_mode("release"),
        MUSE_APP_BUILD_NUMBER = "1",
        MUSE_APP_REVISION = revision,
        MUSE_APP_VERSION = major .. "." .. minor .. "." .. patch,
        MUSE_APP_VERSION_MAJOR = major,
        MUSE_APP_VERSION_MINOR = minor,
        MUSE_APP_VERSION_PATCH = patch,
        -- Selected module graph.  The engine modules we compile need their
        -- enable macros; the audio work mode is only read by
        -- audio/common/workmode.cpp defaultMode() (no Qt path) and stays at the
        -- 4.7.6 value.
        MUSE_MODULE_AUDIO = true,
        MUSE_MODULE_AUDIO_API = true,
        MUSE_MODULE_AUDIO_EXPORT = true,
        MUSE_MODULE_AUDIO_WORKMODE = 1,
        MUSE_MODULE_DRAW = true,
        MUSE_MODULE_DRAW_API = true,
        MUSE_MODULE_GLOBAL = true,
        MUSE_MODULE_GLOBAL_API = true,
        MUSE_MODULE_MPE = true,
        MUSE_MODULE_MPE_API = true,
    }
    if version_label and version_label ~= "" then
        result.MUSE_APP_VERSION_LABEL = version_label
    end
    return result
end

local function render_framework_config(template, variables)
    local output = {}
    for line in (template .. "\n"):gmatch("([^\r\n]*)\r?\n") do
        local name, rest = line:match("^#cmakedefine%s+([%w_]+)(.*)$")
        if name then
            local value = variables[name]
            if value == nil or value == false then
                table.insert(output, "/* #undef " .. name .. " */")
            else
                local fmt = (rest or ""):gsub("%${([%w_]+)}", function(key)
                    local item = variables[key]
                    if item == nil or item == false then
                        return ""
                    end
                    return tostring(item)
                end)
                if fmt:gsub("%s", "") == "" then
                    table.insert(output, "#define " .. name)
                else
                    table.insert(output, "#define " .. name .. fmt)
                end
            end
        else
            line = line:gsub("%${([%w_]+)}", function(key)
                local item = variables[key]
                if item == nil or item == false then
                    return ""
                end
                return tostring(item)
            end)
            table.insert(output, line)
        end
    end
    return table.concat(output, "\n") .. "\n"
end

local function write_if_changed(os, io, filename, content)
    if os.isfile(filename) then
        local previous = io.readfile(filename)
        if previous == content then
            return false
        end
    end
    io.writefile(filename, content)
    return true
end

-- The generator runs in target-callback scope, where raise() exists (a
-- project-scope function would not see it).
target("muse_generated")
    set_kind("phony")
    set_default(false)
    set_targetdir("$(builddir)/xmake-generated")
    local function generate_framework_config(target, os, io)
        local output_dir = target:targetdir()
        if not output_dir or output_dir == "" then
            raise("generated target requires a build directory")
        end
        os.mkdir(output_dir)
        local template_path = path.join(source_root, "muse", "framework", "cmake",
            "muse_framework_config.h.in")
        local template = io.readfile(template_path)
        if not template then
            raise("cannot read pinned config template: %s", template_path)
        end
        write_if_changed(os, io, path.join(output_dir, "muse_framework_config.h"),
            render_framework_config(template, read_version(os, io)))
    end
    on_config(function (target)
        generate_framework_config(target, os, io)
    end)
    on_build(function (target)
        generate_framework_config(target, os, io)
    end)
target_end()

---------------------------------------------------------------------------
-- Engine module targets
---------------------------------------------------------------------------
-- Source lists, per-target defines and include directories all come from the
-- manifest; nothing here selects files by hand.

target("muse_global")
    set_kind("static")
    add_deps("muse_generated")
    add_packages("muse-pugixml", "utfcpp", "muse-picojson")
    add_manifest_target("muse_global")
    if is_plat("linux") then
        add_syslinks("dl", "pthread", {public = true})
    end

target("muse_draw")
    set_kind("static")
    add_deps("muse_generated", "muse_global")
    -- public: every consumer includes draw headers that reach <ft2build.h>/<hb-ft.h>
    -- (fontsengine.h, fontfaceutils.h), and xmake only propagates package include
    -- dirs of a dependency when the package is public.
    add_packages("freetype", "muse-harfbuzz", {public = true})
    add_manifest_target("muse_draw")

target("muse_mpe")
    set_kind("static")
    add_deps("muse_generated", "muse_global")
    add_manifest_target("muse_mpe")

target("engraving")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "muse_draw", "muse_mpe")
    add_manifest_target("engraving")

target("iex_musicxml")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "engraving")
    add_manifest_target("iex_musicxml")

-- Import algorithms keep their upstream module boundary; both source lists are
-- generated from the engine's importexport CMakeLists, so they follow
-- upstream's file moves (for example the 5.0 guitarbendimport split and the new
-- MIDI import model, which stays out: it is Qt-only).
target("iex_guitarpro")
    set_kind("static")
    add_deps("muse_generated", "engraving")
    add_manifest_target("iex_guitarpro")
target_end()

target("iex_midi_import")
    set_kind("static")
    add_deps("muse_generated", "engraving")
    add_manifest_target("iex_midi_import")
target_end()
---------------------------------------------------------------------------
-- Audio engine
---------------------------------------------------------------------------
target("native_fluidsynth")
    set_kind("static")
    if is_plat("windows") then
        set_languages("c11", "cxx23")
    else
        -- Upstream's NO_GLIB fallback uses alloca from GNU libc headers.
        set_languages("gnu17", "cxx23")
    end
    add_manifest_target("native_fluidsynth")
    set_warnings("none")

-- MuseScore 5.0's audio engine and its encoders.  The source list is in the
-- manifest (audio/common + audio/engine + fluidsynth); the encoders are the
-- pinned packages.  MUSE_THREADS_SUPPORT is deliberately left undefined so the
-- mixer stays single-threaded, and the engine's own audio worker thread
-- (platform/general/generalaudioworker.cpp) is not compiled: the host drives
-- the engine from its own thread.
target("muse_audio_engine")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "native_fluidsynth")
    add_packages("muse-flac", "lame", "muse-opus", "muse-opusenc", "muse-fdk-aac")
    add_manifest_target("muse_audio_engine")
target_end()

---------------------------------------------------------------------------
-- Exports for the including project
---------------------------------------------------------------------------
-- The host builds its own adapter targets on top of these: it needs the
-- generated header, the engine include directories, the engine's global
-- defines (class layouts must match), and the engine-side source list of
-- noqt_support (see the superproject's xmake.lua).
ENGRAVE_MANIFEST = manifest
ENGRAVE_MANIFEST_INCLUDES = expand_list(manifest.includes)
ENGRAVE_SOURCE_ROOT = source_root
ENGRAVE_FORK_ROOT = fork_root

-- The upstream gtest suites belong to the engine's build as well; they are
-- defined only when the including project supplies the Qt-free test runtime
-- (see the top of xmake/tests.lua).
includes(path.join(fork_root, "xmake", "tests.lua"))
