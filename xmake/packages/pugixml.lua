-- Pinned pugixml for the framework's XML reader/writer.
--
-- muse_deps does not use a pugixml release package: recipes/pugixml pins 1.15
-- (https://github.com/zeux/pugixml/releases/download/v1.15/pugixml-1.15.tar.gz,
-- sha256 655ade57...) and compiles src/pugixml.cpp straight into the consuming
-- module.  This recipe keeps the version and builds the same single
-- translation unit, but as a small static library through xmake itself, so the
-- package needs no CMake/Ninja bootstrap environment of its own.
package("muse-pugixml")
    set_homepage("https://pugixml.org/")
    set_description("Light-weight XML parser with XPath, 1.15")
    set_license("MIT")

    add_urls("https://github.com/zeux/pugixml/releases/download/v$(version)/pugixml-$(version).tar.gz")
    add_versions("1.15", "655ade57fa703fb421c2eb9a0113b5064bddb145d415dd1f88c79353d90d511a")

    add_includedirs("include/pugixml")

    on_install(function (package)
        -- The mode rules map the package's release mode to optimization flags.
        io.writefile("xmake.lua", [[
add_rules("mode.debug", "mode.release")
target("pugixml")
    set_kind("static")
    add_files("src/pugixml.cpp")
    add_includedirs("src", {public = true})
    add_headerfiles("src/*.hpp", {prefixdir = "pugixml"})
]])
        import("package.tools.xmake").install(package, {})
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({ test = [[
            void test() {
                pugi::xml_document doc;
                pugi::xpath_node_set nset;
            }
        ]] }, { configs = { languages = "c++11" }, includes = "pugixml.hpp" }))
    end)
package_end()
