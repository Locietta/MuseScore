-- Pinned Fraunhofer FDK AAC 2.0.3 for the engine's AAC encoder.
--
-- Same version, URL and hash as muse/buildscripts/muse_deps (recipes/fdk-aac)
-- and as xmake-repo's fdk-aac, but with the include roots muse_deps uses: the
-- engine includes <aacenc_lib.h> and <FDK_audio.h> directly
-- (muse/framework/audio/engine/internal/export/aacencoder.cpp:28-29), while the
-- upstream CMake install puts them in include/fdk-aac and xmake-repo exports
-- only include/. Building it here also keeps every audio encoder on the same
-- "pinned source, project recipe" footing (see flac.lua, opus.lua).
package("muse-fdk-aac")
    set_homepage("https://sourceforge.net/projects/opencore-amr/")
    set_description("Fraunhofer FDK AAC codec library, 2.0.3")
    set_license("Apache-2.0")

    add_urls("https://github.com/mstorsjo/fdk-aac/archive/refs/tags/v$(version).tar.gz")
    add_versions("2.0.3", "e25671cd96b10bad896aa42ab91a695a9e573395262baed4e4a2ff178d6a3a78")

    add_deps("cmake")

    add_links("fdk-aac")
    add_linkdirs("lib")
    add_includedirs("include/fdk-aac", "include")

    on_install(function (package)
        local configs = {
            "-DBUILD_SHARED_LIBS=OFF",
            "-DBUILD_PROGRAMS=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"),
        }
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("aacEncOpen", { includes = {"aacenc_lib.h"} }))
    end)
package_end()
