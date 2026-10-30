#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <engrave/document.h>
#include <engrave/engine.h>

#include "engrave/src/api/batch.h"
#include "engrave/src/runtime/noqt_runtime.h"

namespace mu::engraving {
class MasterScore;
}

namespace engrave {

// Contract checks stay on in release builds: they guard process-global engine
// state that a violation would corrupt.
void check(bool condition, std::string_view message) noexcept;
struct EngineState {
    std::thread::id owner = std::this_thread::get_id();
    std::filesystem::path resources;
    std::size_t live_documents = 0;

    void check_thread() const noexcept {
        check(std::this_thread::get_id() == owner, "engrave called from a thread other than the Engine's");
    }
};

struct Engine::Impl {
    EngineState state;
};

struct Document::Impl {
    EngineState* engine = nullptr;
    music::ScoreProject project;
    std::uint64_t revision = 1;
    std::vector<Diagnostic> diagnostics;
    // A failed edit could not restore the score: only destruction is left.
    bool poisoned = false;
    api::EditHooks edit_hooks;

    static std::unique_ptr<Document> make(EngineState& engine, music::ScoreProject project,
                                          std::vector<Diagnostic> diagnostics);
};

// The score behind a Document. The library's own tests drive undo, layout, saves
// and the engine's MSCX export through it until the read and edit API exist (see
// engrave/tests/handles_test.cpp).
struct DocumentAccess {
    static mu::engraving::MasterScore* master_score(Document& document);
    static music::ScoreProject& project(Document& document);
    static api::EditHooks& edit_hooks(Document& document);
};

// The path as UTF-8 with '/' separators: the engine's path type reads
// "E:\a" as a relative path.
std::string utf8(const std::filesystem::path& path);

} // namespace engrave
