-- Pinned picojson for the framework's JSON serialisation.
--
-- muse_deps does not use a picojson release: recipes/picojson pins master
-- 111c9be5188f7350c2eac9ddaedd8cca3d7bf394 and applies
-- 0001-deterministic-number-serialization.patch, which serialises integers via
-- std::to_string and doubles with 6 fixed decimals (trimmed) instead of
-- "%.17g".  The 4.7.6 in-tree copy carried the same patch (that is what
-- muse/framework/global/serialization/json.cpp was compiled against), so the
-- patched file - not xmake-repo's v1.3.0 release - is what keeps JSON output
-- byte-stable.
--
-- The patch is kept in xmake/packages/patches/ (copied verbatim from
-- muse_deps@39c897ed, sha256 ff34859b5796e91638cf4fddc3dc4fb65eff5a2a4523aa41f7c86ab522442bd6).
package("muse-picojson")
    set_homepage("https://github.com/kazuho/picojson")
    set_description("JSON parser/serializer, muse_deps master pin with the deterministic-number patch")
    set_license("BSD-2-Clause")

    set_kind("library", { headeronly = true })

    add_urls("https://github.com/kazuho/picojson/archive/$(version).tar.gz")
    add_versions("111c9be", "671f89832a17e9e71398f80c0a326afa2ebe81f4c26d5a9992e1fcd0888ae151")
    add_patches("111c9be",
        path.join(os.scriptdir(), "patches", "picojson-0001-deterministic-number-serialization.patch"),
        "ff34859b5796e91638cf4fddc3dc4fb65eff5a2a4523aa41f7c86ab522442bd6")

    add_includedirs("include/picojson")

    on_install(function (package)
        os.cp("picojson.h", path.join(package:installdir("include"), "picojson", "picojson.h"))
    end)

    on_test(function (package)
        assert(os.isfile(path.join(package:installdir("include/picojson"), "picojson.h")))
    end)
package_end()
