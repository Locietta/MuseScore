-- Pinned utfcpp headers for the framework's UTF-8 helpers.
--
-- This is xmake-repo's utfcpp recipe (packages/u/utfcpp/xmake.lua at xmake-repo
-- e96d4103633e161d259aa307e02a4d0842207651, the revision recorded in the
-- installed package manifests) reduced to the single version the project
-- builds.  Archive URL and SHA-256 are copied unchanged: v4.1.1 / 1ca68016...
-- is exactly the archive the pre-change build already installed (verified
-- against xmake's download cache), so the headers stay identical.
package("utfcpp")
    set_kind("library", {headeronly = true})
    set_homepage("https://github.com/nemtrif/utfcpp")
    set_description("UTF8-CPP: UTF-8 with C++ in a Portable Way")
    set_license("BSL-1.0")

    add_urls("https://github.com/nemtrif/utfcpp/archive/refs/tags/$(version).tar.gz",
             "https://github.com/nemtrif/utfcpp.git")
    add_versions("v4.1.1", "1ca68016f0abc24172998e39ce0d8f8e2b7a26f7579a0ff85d4e1b9a7aea56f8")

    add_deps("cmake")

    add_includedirs("include", "include/utf8cpp")

    on_install(function (package)
        local configs = {"-DUTF8_TESTS=OFF", "-DUTF8_INSTALL=ON", "-DUTF8_SAMPLES=OFF"}
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        local test_snippet = [[
            #define UTF_CPP_CPLUSPLUS 201103L
            #include <utf8.h>
            void test() {
                std::string line("你好，世界");
                std::u16string u16line = utf8::utf8to16(line);
                std::string u8line = utf8::utf16to8(u16line);
            }
        ]]

        if package:is_plat("windows") then
            assert(package:check_cxxsnippets({test = test_snippet}, {configs = {languages = "c++11", cxflags = "/utf-8"}}))
        else
            assert(package:check_cxxsnippets({test = test_snippet}, {configs = {languages = "c++11"}}))
        end
    end)
package_end()
