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
#include "engrave/src/audio/score_audio.h"
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
    // Where a render_audio writes its temporary file. Set by Engine::create.
    std::filesystem::path cache_dir;
    std::vector<std::filesystem::path> soundfonts;
    std::int32_t audio_sample_rate = 44100;
    std::size_t live_documents = 0;
    unsigned audio_render_index = 0; // names the temporary files of one process

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
    // The source MSCZ's audiosettings.json, read before the DOM exists; a
    // document from any other format, or from a template, has neutral values.
    music::ScoreAudioMixDefaults audio_defaults;
    std::uint64_t revision = 1;
    std::vector<Diagnostic> diagnostics;
    // A failed edit could not restore the score: only destruction is left.
    bool poisoned = false;
    api::EditHooks edit_hooks;

    static std::unique_ptr<Document> make(EngineState& engine, music::ScoreProject project,
                                          music::ScoreAudioMixDefaults audio_defaults,
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

// The index of the excerpt the selection names, or -1 for the master score, as
// the audio driver names it. NotFound, StaleHandle and InvalidArgument as
// select_score reports them.
Result<int> excerpt_id_of(mu::engraving::MasterScore& master, const ScoreSel& selection);

} // namespace engrave
