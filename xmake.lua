-- Qt-free xmake build for the engrave engine library (MuseScore 5.0 sources).
--
-- Standalone build:
--   xmake f -m release && xmake build
-- Tests: xmake f --engrave_tests=y && xmake build c0_gate
--
-- The engine targets themselves live in xmake/engrave.lua, which is also the
-- file the musescore-mcp superproject includes from its pinned vendor/musescore
-- submodule (or --musescore_source checkout).  Everything here is what a build
-- of the engine *as a project* needs but a library must not impose on its
-- consumer: project identity, languages, package policies, the pinned zlib,
-- and the platform feature macros and toolchain flags the engine translation
-- units were configured with.

set_project("engrave")
set_xmakever("3.1.1")
set_version("0.1.0")
set_languages("cxx23")
set_targetdir("$(builddir)")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
-- Packages install into the project (.xmake, gitignored) so a checkout is
-- self-contained, and everything is built from source: a precompiled artifact
-- could carry a different CRT or feature set than the pinned recipe asks for.
set_policy("package.install_locally", true)
set_policy("package.precompiled", false)

-- The engine's third-party libraries (freetype, harfbuzz, flac, lame, opus,
-- opusenc, fdk-aac, pugixml, utfcpp, picojson) are declared by
-- xmake/engrave.lua together with their pinned recipes.
includes("xmake/engrave.lua")

-- zlib: the engine's zip container includes <zlib.h>.  The superproject takes
-- zlib from its native environment; a bare checkout gets the pinned package.
-- It is declared here, not in engrave.lua, because a consumer's zlib choice
-- must stay the consumer's (the superproject's exports are byte-pinned to the
-- pixi zlib it has always used).
includes("xmake/packages/zlib.lua")
add_requires("muse-zlib 1.3.1", {system = false})
add_packages("muse-zlib")

-- nlohmann/json: engrave's score/ headers use it, and so does the code that
-- includes them, so both must see one copy: the consumer's (the superproject
-- has the pixi 3.12.0).  A bare checkout gets the same release from xmake-repo.
add_requires("nlohmann_json v3.12.0", {system = false})
add_packages("nlohmann_json")

-- cairo: engrave's page renderer links it.  Like zlib it is the consumer's
-- choice (the superproject's PDF and PNG goldens are pinned to its pixi cairo).
-- A bare checkout uses the system cairo, or the install prefix given with
-- --cairo_prefix (<prefix>/include/cairo, <prefix>/lib), e.g. a conda env's
-- Library directory on Windows, where no system cairo exists.  Only engrave
-- sees the prefix's headers, so its other libraries (zlib, freetype) never
-- replace the pinned ones in the engine targets.
option("cairo_prefix")
    set_default("")
    set_showmenu(true)
    set_description("cairo install prefix (default: the system cairo)")
option_end()

local cairo_prefix = get_config("cairo_prefix")
if cairo_prefix and cairo_prefix ~= "" then
    cairo_prefix = path.absolute(cairo_prefix)
    target("engrave")
        add_sysincludedirs(path.join(cairo_prefix, "include"), path.join(cairo_prefix, "include", "cairo"))
        add_linkdirs(path.join(cairo_prefix, "lib"), {public = true})
        if not is_plat("windows") then
            -- The prefix's cairo is a shared library, so every binary that links the
            -- engine needs its directory at run time.  Without this, a host with no
            -- system cairo (cachy) starts such a binary in the loader instead: exit
            -- 127, "libcairo.so.2: cannot open shared object file", unless the caller
            -- sets LD_LIBRARY_PATH.  A static library's rpathdirs are exported to the
            -- targets that depend on it, which is what puts the path in each binary.
            add_rpathdirs(path.join(cairo_prefix, "lib"))
        end
    target_end()
else
    add_requires("cairo", {system = true})
    target("engrave")
        add_packages("cairo", {public = true})
    target_end()
end

-- Native platform/toolchain configuration.
if is_plat("windows") then
    set_runtimes("MD")
    set_exceptions("cxx")
    add_defines("WIN32", "_WINDOWS", "NOMINMAX", "_USE_MATH_DEFINES", "UNICODE", "_UNICODE",
        "_CRT_SECURE_NO_WARNINGS", "_CRT_NONSTDC_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING")
    -- C4 owns audio headers that still consume this alias.
    add_defines("Q_OS_WIN")
    add_cxflags("/utf-8", "/bigobj", "/Zc:__cplusplus", {tools = {"cl", "clang_cl"}})
    add_cflags("/utf-8", {tools = {"cl", "clang_cl"}})
elseif is_plat("macosx") then
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
elseif is_plat("linux") then
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
else
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
end
