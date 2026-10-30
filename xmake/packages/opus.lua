-- Pinned Opus 1.5.2 for the audio engine's Ogg/Opus encoder.
--
-- Version, source URL/hash and CMake args come from muse/buildscripts/muse_deps
-- (recipes/opus: 1.5.2, BUILD_SHARED_LIBS=OFF, OPUS_BUILD_PROGRAMS=OFF,
-- OPUS_BUILD_TESTING=OFF).  xmake-repo's libopus has the same version but
-- exports only its include/ root, while opusenc.h includes <opus.h>; this recipe
-- uses the upstream CMake build (which selects the RTCD/SIMD kernels per
-- architecture) and exports both include roots.
package("muse-opus")
    set_homepage("https://opus-codec.org/")
    set_description("Opus audio codec, 1.5.2")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/xiph/opus/releases/download/v$(version)/opus-$(version).tar.gz")
    add_versions("1.5.2", "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1")

    add_deps("cmake")

    add_links("opus")
    add_linkdirs("lib")
    add_includedirs("include/opus", "include")

    on_install(function (package)
        local configs = {
            "-DBUILD_SHARED_LIBS=OFF",
            "-DOPUS_BUILD_PROGRAMS=OFF",
            "-DOPUS_BUILD_TESTING=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"),
        }
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("opus_encoder_create", { includes = {"opus.h"} }))
    end)
package_end()
