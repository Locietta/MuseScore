// Shared fixtures of the output tests (render and audio): the Engine, the corpus
// and the SoundFonts, all created once in each binary's main() because a process
// has one Engine.

#pragma once

#include <engrave/engrave.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace music::test {

extern std::unique_ptr<engrave::Engine> g_engine;
extern std::vector<std::filesystem::path> g_corpus;
extern std::vector<std::filesystem::path> g_soundfonts;
extern std::filesystem::path g_assets;
extern std::filesystem::path g_self;

// The corpus file with this name, or an empty path when the corpus has none.
std::filesystem::path corpus_file(std::string_view name);
// The probe binary beside this test binary.
std::filesystem::path probe_binary();
// Runs the probe beside this binary with these arguments and returns its exit code.
int run_probe(const std::vector<std::string>& arguments);

} // namespace music::test
