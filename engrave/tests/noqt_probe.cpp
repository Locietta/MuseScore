#include "engrave/src/runtime/engine_context.h"
#include "engrave/src/runtime/noqt_runtime.h"
#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

// Imports a MusicXML file, lays it out and exports MusicXML and MIDI through the
// engine context, without Qt or a desktop install.
int main(int argc, char** argv) {
    if (argc != 4) { std::cerr << "Usage: noqt_probe ASSETS INPUT.musicxml OUTPUT.musicxml\n"; return 2; }
    try {
        music::EngineContext context(argv[1]);
        std::ifstream in(std::filesystem::u8path(argv[2]), std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read input");
        const std::string input{std::istreambuf_iterator<char>(in), {}};
        auto project = music::load_native_score(input, "musicxml");
        const std::string xml = music::export_native_score(context, project, "musicxml", 0, {});
        const std::string midi = music::export_native_score(context, project, "midi", 0, {});
        std::ofstream(std::filesystem::u8path(argv[3]), std::ios::binary) << xml;
        std::ofstream(std::filesystem::u8path(std::string(argv[3]) + ".mid"), std::ios::binary) << midi;
        auto* score = project->masterScore();
        std::cout << "Qt-free import/layout/export: parts=" << score->parts().size() << " pages=" << score->npages()
                  << " bytes=" << xml.size() << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
