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
--   * the generated framework configuration header;
--   * the engine module targets: muse_global, muse_draw, muse_mpe, engraving,
--     the iex_* import/export modules, native_fluidsynth and
--     muse_audio_engine, compiled via folder globs;
--   * engrave, the library of Qt-free adapters in engrave/src (runtime, io,
--     render, audio, score), and its tests (xmake/tests.lua).
--
-- The including project may set ENGRAVE_SOURCE_ROOT to build the engine
-- sources from a checkout other than this one; the fork itself builds from its
-- own tree.  It supplies zlib and the cairo headers and library directory
-- (the fork's xmake.lua does so for the standalone build).

local fork_root = path.directory(os.scriptdir())
local source_root = path.absolute(ENGRAVE_SOURCE_ROOT or fork_root)

---------------------------------------------------------------------------
-- Third-party dependencies
---------------------------------------------------------------------------
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
-- Generated framework configuration header
---------------------------------------------------------------------------
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
    local root_probe = os.execv("git", root_args,
        {try = true, stdout = os.nuldev(), stderr = os.nuldev()})
    if root_probe ~= 0 then
        print("warning: cannot read the engine git revision from " .. source_root
            .. "; MUSE_APP_REVISION will be empty")
    else
        local top_level = tostring(os.iorunv("git", root_args) or ""):gsub("%s+", "")
        if normalized_path(top_level) ~= normalized_path(source_root) then
            print("warning: " .. source_root .. " is not a git checkout root (repository root '"
                .. top_level .. "'); MUSE_APP_REVISION will be empty")
        else
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
        MUSE_APP_UNSTABLE = not is_mode("release"),
        MUSE_APP_BUILD_NUMBER = "1",
        MUSE_APP_REVISION = revision,
        MUSE_APP_VERSION = major .. "." .. minor .. "." .. patch,
        MUSE_APP_VERSION_MAJOR = major,
        MUSE_APP_VERSION_MINOR = minor,
        MUSE_APP_VERSION_PATCH = patch,
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
-- Exports for the including project
---------------------------------------------------------------------------
ENGRAVE_SOURCE_ROOT = source_root
ENGRAVE_FORK_ROOT = fork_root

ENGRAVE_DEFINES = {
    "NO_QT_SUPPORT",
    "ENGRAVING_NO_ACCESSIBILITY",
    "ENGRAVING_NO_INTERACTIVE",
    "ENGRAVING_NO_API",
    "MUSICXML_NO_INTERNAL",
    "MUSICXML_NO_INTERACTIVE",
    "MUSICXML_NO_VALIDATION",
    "MUSE_MODULE_AUDIO_EXPORT",
    "MUSE_MODULE_DRAW_USE_FONTFACE_FT",
    "MUSE_MODULE_DRAW_USE_QTTEXTDRAW",
}

ENGRAVE_GLOBAL_DEFINES = {
    "KORS_PROFILER_ENABLED",
}

ENGRAVE_INCLUDES = {
    path.join(source_root, "muse"),
    path.join(source_root, "muse", "framework"),
    path.join(source_root, "muse", "framework", "global"),
    path.join(source_root, "src"),
}

-- The adapters' internal headers, included from the checkout root as
-- "engrave/src/<area>/<file>.h" so that no engine include root can supply a
-- header of the same name, whatever the include order.  Besides the library
-- and its tests, only the musescore-mcp host includes them, as a privileged
-- client until A5 moves it onto the public API.
local engrave_src = path.join(source_root, "engrave", "src")
ENGRAVE_INTERNAL_INCLUDES = {
    source_root,
}

local fluidsynth_dir = path.join(source_root, "muse", "framework", "audio", "thirdparty", "fluidsynth", "fluidsynth-2.3.3")
local fluidsynth_src = path.join(fluidsynth_dir, "src")


ENGRAVE_AUDIO_ENGINE = {
    defines = {
        "KORS_PROFILER_ENABLED",
        "NO_GLIB",
        "NO_THREADS",
        "SYSTEM_LAME",
    },
    includes = {
        path.join(source_root, "muse", "framework", "audio", "common"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global"),
        path.join(source_root, "muse", "framework", "audio"),
        path.join(source_root, "muse", "framework", "audio", "engine"),
        path.join(fluidsynth_dir, "include"),
        fluidsynth_src,
        path.join(fluidsynth_src, "bindings"),
        path.join(fluidsynth_src, "drivers"),
        path.join(fluidsynth_src, "external"),
        path.join(fluidsynth_src, "midi"),
        path.join(fluidsynth_src, "rvoice"),
        path.join(fluidsynth_src, "sfloader"),
        path.join(fluidsynth_src, "synth"),
        path.join(fluidsynth_src, "utils"),
    },
}

ENGRAVE_AUDIO_INCLUDES = {
    path.join(fluidsynth_dir, "include"),
    fluidsynth_src,
    path.join(fluidsynth_src, "bindings"),
    path.join(fluidsynth_src, "drivers"),
    path.join(fluidsynth_src, "external"),
    path.join(fluidsynth_src, "midi"),
    path.join(fluidsynth_src, "rvoice"),
    path.join(fluidsynth_src, "sfloader"),
    path.join(fluidsynth_src, "synth"),
    path.join(fluidsynth_src, "utils"),
    path.join(source_root, "muse", "framework", "audio"),
}

---------------------------------------------------------------------------
-- Engine module targets
---------------------------------------------------------------------------
local function add_engine_target_defaults()
    add_defines(table.unpack(ENGRAVE_DEFINES))
    add_includedirs("$(builddir)/xmake-generated")
end

target("muse_global")
    set_kind("static")
    add_deps("muse_generated")
    add_packages("muse-pugixml", "utfcpp", "muse-picojson")
    add_engine_target_defaults()
    add_defines("GLOBAL_NO_INTERNAL", "KORS_PROFILER_ENABLED", "MUSE_MODULE_GLOBAL_LOGGER_DEBUGLEVEL")
    add_includedirs(
        path.join(source_root, "muse", "framework", "global"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework")
    )
    add_files(
        path.join(source_root, "muse", "framework", "global", "*.cpp"),
        path.join(source_root, "muse", "framework", "global", "io", "*.cpp"),
        path.join(source_root, "muse", "framework", "global", "io", "devtools", "*.cpp"),
        path.join(source_root, "muse", "framework", "global", "modularity", "*.cpp"),
        path.join(source_root, "muse", "framework", "global", "serialization", "**.cpp"),
        path.join(source_root, "muse", "framework", "global", "types", "*.cpp"),
        path.join(source_root, "muse", "framework", "global", "thirdparty", "kors_async", "async", "internal", "*.cpp"),
        path.join(source_root, "muse", "framework", "global", "thirdparty", "kors_logger", "src", "*.cpp"),
        path.join(source_root, "muse", "framework", "global", "thirdparty", "kors_modularity", "modularity", "*.cpp"),
        path.join(source_root, "muse", "framework", "global", "thirdparty", "kors_profiler", "profiler", "*.cpp")
    )
    -- engrave/src/global implements framework services that upstream builds
    -- from trimmed Qt sources (muse::Settings).  engraving and the other modules
    -- call them, so they must link with muse_global rather than with engrave,
    -- which comes first on the link line.
    add_files(path.join(engrave_src, "global", "*.cpp"))
    if is_plat("linux") then
        add_syslinks("dl", "pthread", {public = true})
    end

target("muse_draw")
    set_kind("static")
    add_deps("muse_generated", "muse_global")
    add_packages("freetype", "muse-harfbuzz", {public = true})
    add_engine_target_defaults()
    add_defines("DRAW_NO_QSVGRENDER", "KORS_PROFILER_ENABLED", "MUSE_MODULE_DRAW_USE_FONTFACE_FT", "MUSE_MODULE_DRAW_USE_QTTEXTDRAW")
    add_includedirs(
        path.join(source_root, "muse", "framework", "draw"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global")
    )
    add_files(
        path.join(source_root, "muse", "framework", "draw", "*.cpp"),
        path.join(source_root, "muse", "framework", "draw", "internal", "*.cpp"),
        path.join(source_root, "muse", "framework", "draw", "types", "*.cpp"),
        path.join(source_root, "muse", "framework", "draw", "utils", "*.cpp")
    )

target("muse_mpe")
    set_kind("static")
    add_deps("muse_generated", "muse_global")
    add_engine_target_defaults()
    add_defines("KORS_PROFILER_ENABLED")
    add_includedirs(
        path.join(source_root, "muse", "framework", "mpe"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global")
    )
    add_files(
        path.join(source_root, "muse", "framework", "mpe", "*.cpp"),
        path.join(source_root, "muse", "framework", "mpe", "internal", "*.cpp")
    )

target("engraving")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "muse_draw", "muse_mpe")
    add_engine_target_defaults()
    add_defines("ENGRAVING_NO_ACCESSIBILITY", "KORS_PROFILER_ENABLED")
    add_includedirs(
        path.join(source_root, "src", "engraving"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global"),
        path.join(source_root, "src", "engraving", "infrastructure"),
        path.join(source_root, "src", "engraving", "thirdparty", "dtl")
    )
    add_files(
        path.join(source_root, "src", "engraving", "*.cpp"),
        path.join(source_root, "src", "engraving", "automation", "**.cpp"),
        path.join(source_root, "src", "engraving", "compat", "**.cpp"),
        path.join(source_root, "src", "engraving", "dom", "**.cpp"),
        path.join(source_root, "src", "engraving", "editing", "**.cpp"),
        path.join(source_root, "src", "engraving", "infrastructure", "**.cpp"),
        path.join(source_root, "src", "engraving", "internal", "**.cpp"),
        path.join(source_root, "src", "engraving", "playback", "**.cpp"),
        path.join(source_root, "src", "engraving", "rendering", "**.cpp"),
        path.join(source_root, "src", "engraving", "rw", "**.cpp"),
        path.join(source_root, "src", "engraving", "style", "**.cpp"),
        path.join(source_root, "src", "engraving", "types", "**.cpp")
    )

target("iex_musicxml")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "engraving")
    add_engine_target_defaults()
    add_defines("ENGRAVING_NO_ACCESSIBILITY", "KORS_PROFILER_ENABLED", "MUSICXML_NO_INTERACTIVE", "MUSICXML_NO_INTERNAL", "MUSICXML_NO_VALIDATION")
    add_includedirs(
        path.join(source_root, "src", "importexport", "musicxml"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global"),
        path.join(source_root, "src", "engraving"),
        path.join(source_root, "src", "engraving", "infrastructure"),
        path.join(source_root, "src", "engraving", "thirdparty", "dtl")
    )
    add_files(path.join(source_root, "src", "importexport", "musicxml", "**.cpp"))

target("iex_guitarpro")
    set_kind("static")
    add_deps("muse_generated", "engraving")
    add_engine_target_defaults()
    add_defines("ENGRAVING_NO_ACCESSIBILITY", "KORS_PROFILER_ENABLED")
    add_includedirs(
        path.join(source_root, "src", "importexport", "guitarpro"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global"),
        path.join(source_root, "src", "engraving"),
        path.join(source_root, "src", "engraving", "infrastructure"),
        path.join(source_root, "src", "engraving", "thirdparty", "dtl")
    )
    add_files(path.join(source_root, "src", "importexport", "guitarpro", "internal", "**.cpp"))
target_end()

-- MIDI import, export and the MIDI file layer they share: upstream's iex_midi.
target("iex_midi")
    set_kind("static")
    add_deps("muse_generated", "engraving")
    add_engine_target_defaults()
    add_defines("ENGRAVING_NO_ACCESSIBILITY", "KORS_PROFILER_ENABLED")
    add_includedirs(
        path.join(source_root, "src", "importexport", "midi"),
        path.join(source_root, "src"),
        path.join(source_root, "muse"),
        path.join(source_root, "muse", "framework"),
        path.join(source_root, "muse", "framework", "global"),
        path.join(source_root, "src", "engraving"),
        path.join(source_root, "src", "engraving", "infrastructure"),
        path.join(source_root, "src", "engraving", "thirdparty", "dtl")
    )
    add_files(
        path.join(source_root, "src", "importexport", "midi", "internal", "midiexport", "*.cpp"),
        path.join(source_root, "src", "importexport", "midi", "internal", "midiimport", "*.cpp"),
        path.join(source_root, "src", "importexport", "midi", "internal", "midishared", "*.cpp"),
        path.join(source_root, "src", "importexport", "midi", "thirdparty", "beatroot", "*.cpp")
    )
target_end()

target("native_fluidsynth")
    set_kind("static")
    if is_plat("windows") then
        set_languages("c11", "cxx23")
    else
        set_languages("gnu17", "cxx23")
    end
    add_engine_target_defaults()
    add_defines("EXTERN_VORBIS_SUPPORT", "KORS_PROFILER_ENABLED", "NO_GLIB", "NO_THREADS")
    add_includedirs(
        path.join(fluidsynth_dir, "include"),
        fluidsynth_src,
        path.join(fluidsynth_src, "bindings"),
        path.join(fluidsynth_src, "drivers"),
        path.join(fluidsynth_src, "external"),
        path.join(fluidsynth_src, "midi"),
        path.join(fluidsynth_src, "rvoice"),
        path.join(fluidsynth_src, "sfloader"),
        path.join(fluidsynth_src, "synth"),
        path.join(fluidsynth_src, "utils")
    )
    add_files(
        path.join(source_root, "muse", "framework", "audio", "thirdparty", "fluidsynth", "vorbis_decode.cpp"),
        path.join(fluidsynth_src, "bindings", "*.c"),
        path.join(fluidsynth_src, "drivers", "*.c"),
        path.join(fluidsynth_src, "midi", "*.c"),
        path.join(fluidsynth_src, "midi", "*.cpp"),
        path.join(fluidsynth_src, "rvoice", "*.c"),
        path.join(fluidsynth_src, "sfloader", "*.c"),
        path.join(fluidsynth_src, "synth", "*.c"),
        path.join(fluidsynth_src, "utils", "*.c")
    )
    set_warnings("none")

target("muse_audio_engine")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "native_fluidsynth")
    add_packages("muse-flac", "lame", "muse-opus", "muse-opusenc", "muse-fdk-aac")
    add_engine_target_defaults()
    add_defines(ENGRAVE_AUDIO_ENGINE.defines)
    add_includedirs(ENGRAVE_AUDIO_ENGINE.includes)
    add_files(
        path.join(source_root, "muse", "framework", "audio", "common", "*.cpp"),
        path.join(source_root, "muse", "framework", "audio", "common", "rpc", "platform", "general", "*.cpp"),
        path.join(source_root, "muse", "framework", "audio", "engine", "*.cpp"),
        path.join(source_root, "muse", "framework", "audio", "engine", "internal", "**.cpp")
    )
target_end()

---------------------------------------------------------------------------
-- engrave: the Qt-free adapters
---------------------------------------------------------------------------
-- Every .cpp under engrave/src except the global/ area (compiled by
-- muse_global) is part of the library; a new file in an area folder needs no
-- edit here.  The areas include each other's headers as
-- "engrave/src/<area>/<file>.h" (source_root is on the include path).  The
-- include path is shared with the adapter tests in xmake/tests.lua.
ENGRAVE_ADAPTER_INCLUDES = {
    path.join(source_root, "src", "engraving", "thirdparty", "dtl"),
    source_root,
    path.join(source_root, "src"),
    path.join(source_root, "muse", "framework"),
    path.join(source_root, "muse", "framework", "global"),
    path.join(source_root, "src", "engraving"),
    path.join(source_root, "muse", "framework", "draw"),
}
for _, list in ipairs({ENGRAVE_AUDIO_INCLUDES, ENGRAVE_INCLUDES}) do
    for _, dir in ipairs(list) do
        table.insert(ENGRAVE_ADAPTER_INCLUDES, dir)
    end
end

target("engrave")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "muse_draw", "muse_mpe", "engraving",
        "iex_musicxml", "iex_guitarpro", "iex_midi", "muse_audio_engine", "native_fluidsynth")
    add_engine_target_defaults()
    add_defines(table.unpack(ENGRAVE_GLOBAL_DEFINES))
    add_includedirs(ENGRAVE_ADAPTER_INCLUDES)
    add_files(path.join(engrave_src, "**.cpp|global/*.cpp"))
    -- cairo renders pages (render/); its headers and library directory come
    -- from the including project, like zlib.
    add_links("cairo", {public = true})
    if not is_plat("windows") then
        add_links("m", {public = true})
    end
target_end()

---------------------------------------------------------------------------
-- Tests: the upstream gtest suites, the adapter tests and the dev probes
---------------------------------------------------------------------------
includes(path.join(fork_root, "xmake", "tests.lua"))
