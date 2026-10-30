-- Pinned libopusenc 0.2.1 for the Ogg/Opus encoder.
--
-- Version, source URL and hash come from muse/buildscripts/muse_deps
-- (recipes/opusenc: 0.2.1, https://github.com/xiph/libopusenc/archive/v0.2.1.tar.gz,
-- sha256 56952a92...).  xmake-repo only has 0.3, and the engine's byte output
-- (Ogg Opus tags, resampler) is golden-checked, so the released 0.2.1 sources
-- are built here the way muse_deps builds them: no upstream CMake, one static
-- library, HAVE_CONFIG_H with PACKAGE_NAME/PACKAGE_VERSION, OUTSIDE_SPEEX +
-- RANDOM_PREFIX for the bundled Speex resampler, and the six library sources.
--
-- Resolution note (muse topic commit 6cd717f2, "opusenc: initialize the OpusTags
-- vendor string"): released 0.2.1 already formats the vendor string in
-- ope_comments_create() ("<opus version>, libopusenc 0.2.1"), which is what the
-- fork patched into its vendored copy.  So the fork patch is obsolete, not
-- lost, and the OpusTags bytes are unchanged.
package("muse-opusenc")
    set_homepage("https://opus-codec.org/")
    set_description("Opus Ogg encoder library, 0.2.1")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/xiph/libopusenc/archive/v$(version).tar.gz")
    add_versions("0.2.1", "56952a926ff962c62a468b43cc8506c069bda767cade4dc92824b74edd570d68")

    add_deps("muse-opus")

    add_links("opusenc")
    add_linkdirs("lib")
    add_includedirs("include", "include/opus")
    -- Public header lands in include/opus and the engine includes both
    -- <opus/opusenc.h> and "opusenc.h"; opusenc.h itself includes <opus.h>,
    -- which muse-opus exports as include/opus.
    add_defines("OUTSIDE_SPEEX", "RANDOM_PREFIX=opusenc_prefix")

    on_install(function (package)
        local opus = package:dep("muse-opus")
        local opus_include = opus:installdir("include/opus")
        if not os.isdir(opus_include) then
            opus_include = opus:installdir("include")
        end
        -- opusenc.c and resample.c include "config.h" unconditionally; muse_deps'
        -- build writes these two macros, which is what the released tarball's
        -- autotools configure would define.
        io.writefile("config.h", [[
#define PACKAGE_NAME "libopusenc"
#define PACKAGE_VERSION "0.2.1"
]])
        -- The mode rules map the package's release mode to optimization flags;
        -- without them the library builds unoptimized (about 4x slower encoding).
        io.writefile("xmake.lua", ([[
add_rules("mode.debug", "mode.release")
target("opusenc")
    set_kind("static")
    add_files(
        "src/ogg_packer.c",
        "src/opus_header.c",
        "src/opusenc.c",
        "src/picture.c",
        "src/resample.c",
        "src/unicode_support.c")
    add_includedirs(".", "src", "include")
    add_headerfiles("include/(opusenc.h)", {prefixdir = "opus"})
    add_defines("HAVE_CONFIG_H", "OUTSIDE_SPEEX", "RANDOM_PREFIX=opusenc_prefix")
    add_includedirs(%q)
]]):format(opus_include))
        import("package.tools.xmake").install(package, {})
    end)

    on_test(function (package)
        -- The public header includes <opus.h>, so a compile test needs the dep's
        -- include dirs, which on_test does not inherit; check the installed
        -- artifacts instead.
        assert(os.isfile(path.join(package:installdir("include/opus"), "opusenc.h")))
        assert(not table.empty(table.wrap(os.files(path.join(package:installdir("lib"), "*")))))
    end)
package_end()
