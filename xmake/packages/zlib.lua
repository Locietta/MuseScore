-- Pinned zlib for the standalone engine build.
--
-- The engine's zip container (muse/framework/global/serialization/internal/
-- zipcontainer.cpp) includes <zlib.h> and links zlib.  The superproject takes
-- zlib from its native (pixi) environment, which is what keeps its exported
-- archives byte-identical to the pre-split build; a bare engine checkout gets
-- this pinned source archive instead, so `xmake build -a` works without any
-- external environment.
--
-- Version pin: zlib 1.3.1 is the line MuseScore 5.0's muse_deps builds against.
-- The archive is built static (the engine build must not depend on a zlib DLL);
-- zlib's CMake build always produces both the shared and the static target, so
-- the recipe installs both and consumers link the static one.
package("muse-zlib")
    set_homepage("https://zlib.net/")
    set_description("A massively spiffy yet delicately unobtrusive compression library.")
    set_license("Zlib")

    add_urls("https://github.com/madler/zlib/releases/download/v$(version)/zlib-$(version).tar.gz")
    add_versions("1.3.1", "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23")

    add_deps("cmake")

    if is_plat("windows") then
        -- zlibstatic.lib (BUILD_SHARED_LIBS=OFF still builds zlib1.dll/zlib.lib
        -- next to it, which this build never links).
        add_links("zlibstatic")
    else
        add_links("z")
    end

    on_install(function (package)
        local configs = {
            "-DZLIB_BUILD_EXAMPLES=OFF",
            "-DBUILD_SHARED_LIBS=OFF",
        }
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"))
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({test = [[
            #include <zlib.h>
            void test() {
                z_stream stream{};
                (void)stream;
                (void)zlibVersion();
            }
        ]]}, {configs = {languages = "c++11"}}))
    end)
package_end()
