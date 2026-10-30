-- Pinned libFTL/FLAC for the audio engine's FLAC encoder.
--
-- muse_deps does not ship a released flac recipe; it builds the engine's
-- third-party copy of flac 1.4.3 with `require_dep(flac)` and links
-- FLAC::FLAC + FLAC::FLAC++ (see muse/buildscripts/cmake/ExtDepsManifest.cmake).
-- xmake-repo's libflac pins BUILD_CXXLIBS=OFF, i.e. no FLAC++ — the engine's
-- flacencoder.cpp includes "FLAC++/encoder.h" — so the recipe is local and
-- turns the C++ library back on.
--
-- WITH_OGG=OFF matches the baseline: the 4.7.6 in-tree build configured
-- FLAC with OGG_FOUND=0 / FLAC__HAS_OGG=0, and the encoder never writes
-- Ogg FLAC.  Dropping it also keeps libogg out of the dependency set.
package("muse-flac")
    set_homepage("https://xiph.org/flac/")
    set_description("Free Lossless Audio Codec, 1.4.3 with the C++ interface")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/xiph/flac/releases/download/$(version)/flac-$(version).tar.xz")
    add_versions("1.4.3", "6c58e69cd22348f441b861092b825e591d0b822e106de6eb0ee4d05d27205b70")

    add_deps("cmake")

    -- FLAC++ depends on FLAC; GNU ld resolves static archives left to right.
    add_links("FLAC++", "FLAC")
    add_linkdirs("lib")
    add_includedirs("include")
    -- Static FLAC on Windows: the headers switch to dllexport/dllimport
    -- without this, which a static archive cannot satisfy.
    add_defines("FLAC__NO_DLL")

    on_install(function (package)
        local configs = {
            "-DBUILD_SHARED_LIBS=OFF",
            "-DBUILD_CXXLIBS=ON",
            "-DBUILD_PROGRAMS=OFF",
            "-DBUILD_EXAMPLES=OFF",
            "-DBUILD_TESTING=OFF",
            "-DBUILD_DOCS=OFF",
            "-DBUILD_UTILS=OFF",
            "-DINSTALL_MANPAGES=OFF",
            "-DWITH_OGG=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"),
        }
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("FLAC__format_sample_rate_is_valid", { includes = {"FLAC/format.h"} }))
    end)
package_end()
