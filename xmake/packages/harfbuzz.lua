-- Pinned HarfBuzz for the Qt-free engine build.
--
-- Version and source URL follow muse/buildscripts/muse_deps (recipes/harfbuzz:
-- 12.3.0, -DHB_HAVE_FREETYPE=ON -DHB_BUILD_SUBSET=OFF, static).  xmake-repo's
-- harfbuzz recipe has no 12.3.0, so the recipe is local; it builds the upstream
-- single-translation-unit amalgamation (src/harfbuzz.cc) the 4.7.6 baseline
-- used, instead of driving meson/ninja, so the compile set stays reviewable
-- and the build needs no external build tool.  HAVE_FREETYPE enables hb-ft.cc
-- (muse_draw uses hb_ft_font_create); the subsetter stays out, as in muse_deps.
package("muse-harfbuzz")
    set_homepage("https://harfbuzz.github.io/")
    set_description("Text shaping library, 12.3.0 amalgamation with FreeType support")
    set_license("MIT")

    add_urls("https://github.com/harfbuzz/harfbuzz/releases/download/$(version)/harfbuzz-$(version).tar.xz")
    add_versions("12.3.0", "8660ebd3c27d9407fc8433b5d172bafba5f0317cb0bb4339f28e5370c93d42b7")

    -- muse_deps builds freetype static without png/zlib/bzip2/brotli; the same
    -- require (same configs) is declared in xmake.lua so one freetype instance
    -- serves both and the two require sites do not install it twice.
    add_deps("freetype", {configs = {
        zlib = false, png = false, bzip2 = false, woff2 = false, harfbuzz = false}})

    add_includedirs("include/harfbuzz")

    on_install(function (package)
        -- The FreeType headers live in the dep's include/freetype2 (xmake-repo and
        -- muse_deps agree on that layout); the amalgamation only needs headers.
        local freetype = package:dep("freetype")
        local freetype_dir = freetype:installdir("include/freetype2")
        if not os.isdir(freetype_dir) then
            raise("freetype headers not found at " .. freetype_dir)
        end
        -- The mode rules map the package's release mode to optimization flags.
        io.writefile("xmake.lua", ([[
add_rules("mode.debug", "mode.release")
target("harfbuzz")
    set_kind("static")
    set_languages("c++17")
    add_files("src/harfbuzz.cc")
    add_includedirs("src", {public = true})
    add_headerfiles("src/(hb*.h)", {prefixdir = "harfbuzz"})
    add_defines("HAVE_FREETYPE")
    add_includedirs(%q)
]]):format(freetype_dir))
        import("package.tools.xmake").install(package, {})
    end)

    on_test(function (package)
        assert(package:has_cfuncs("hb_buffer_add_utf8", { includes = {"hb.h"} }))
    end)
package_end()
