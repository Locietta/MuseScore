// SPDX-License-Identifier: GPL-3.0-only
// Dev tool: print the format-independent semantic dump of a score file (roadmap-engrave A0).
#include "engrave/src/runtime/engine_context.h"
#include "engrave/src/runtime/noqt_runtime.h"
#include "engrave/src/score/score_excerpts.h"
#include "engrave/src/score/score_semantic.h"
#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        std::string format;
        bool generate = false;
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--generate-excerpts") generate = true;
            else if (arg.rfind("--format=", 0) == 0) format = arg.substr(9);
            else args.push_back(arg);
        }
        if (args.size() != 2)
            throw std::invalid_argument("Usage: semantic_dump ASSETS INPUT [--format=EXT] [--generate-excerpts]");
        const auto input = std::filesystem::u8path(args[1]);
        if (format.empty()) {
            format = input.extension().string();
            if (!format.empty()) format.erase(0, 1);
        }
        std::ifstream stream(input, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot read " + args[1]);
        const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        music::EngineContext context(args[0]);
        auto project = music::load_native_score(context, bytes, format);
        auto* master = project->masterScore();
        if (generate) music::generate_score_excerpts(master);
        std::cout << music::semantic_dump(master).dump(1) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "semantic_dump: " << error.what() << '\n';
        return 1;
    }
}
