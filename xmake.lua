-- Qt-free xmake build for the engrave engine library (MuseScore 5.0 sources).
--
-- Standalone build:
--   xmake f -m release && xmake build -a
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

-- Qt platform macros normally come from <QtGlobal>; the engine branches on them
-- under NO_QT_SUPPORT.  Defined once for every target compiled from the engine:
--   engraving/dom/masterscore.cpp        platform metaTag
--   framework/global/io/fileinfo.cpp     Windows drive-letter branches
--   framework/global/io/dir.cpp          Windows separator normalization
--   framework/global/runtime.cpp         POSIX thread names
--   engraving/editing/textedit.cpp and
--   engraving/editing/editdata.h         platform key handling
--   engraving/rendering/score/tdraw.cpp
-- The compiled files behind those branches include no Qt headers.  The
-- superproject sets the same values project-wide for its host targets.
if is_plat("windows") then
    set_runtimes("MD")
    set_exceptions("cxx")
    add_defines("WIN32", "_WINDOWS", "NOMINMAX", "_USE_MATH_DEFINES", "UNICODE", "_UNICODE",
        "_CRT_SECURE_NO_WARNINGS", "_CRT_NONSTDC_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING")
    add_defines("Q_OS_WIN", "Q_OS_WIN32", "Q_OS_WINDOWS")
    if is_arch("x64", "x86_64") then
        add_defines("Q_OS_WIN64")
    end
    add_cxflags("/utf-8", "/bigobj", "/Zc:__cplusplus", {tools = {"cl", "clang_cl"}})
    add_cflags("/utf-8", {tools = {"cl", "clang_cl"}})
elseif is_plat("macosx") then
    add_defines("Q_OS_MAC", "Q_OS_MACOS", "Q_OS_DARWIN", "Q_OS_UNIX")
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
elseif is_plat("linux") then
    add_defines("Q_OS_LINUX", "Q_OS_UNIX")
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
else
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
end
