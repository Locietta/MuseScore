#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <engrave/describe.h>
#include <engrave/document.h>
#include <engrave/error.h>
#include <engrave/output.h>

namespace engrave {

struct EngineOptions {
    // Runtime assets: engraving/, fonts/, mpe/ and templates/.
    std::filesystem::path resources;
    // Parent of the engine's cache directory; empty means the system
    // temporary directory.
    std::filesystem::path cache_dir;
    // Extra text fonts, added in order.
    std::vector<std::filesystem::path> fonts;
    // SoundFonts the audio renderer may use, in order; the first is the default
    // one every track plays unless its AudioOptions name another. The audio
    // engine itself starts on the first render_audio, because it is
    // process-global and costs a synthesizer per font.
    std::vector<std::filesystem::path> soundfonts;
    // The audio sample rate, in Hz. Fixed for the Engine: its audio context is
    // created with it, so AudioOptions::sample_rate must agree.
    std::int32_t audio_sample_rate = 44100;
    // Sequential element ids instead of random ones, so that two opens of the same
    // file agree.  Only for tests that never round-trip a score: the sequence
    // collides with the ids a read-back score already has, and the reader then
    // renumbers them.  It also changes what the writer stores, so leave it off
    // otherwise.
    bool deterministic_ids = false;
};

struct Capabilities {
    std::string engine_version; // written into saved scores, e.g. "5.0.0"
    std::vector<Format> import_formats;
    std::vector<Format> export_formats;
    // Render formats and audio encoders this build has.
    std::vector<RenderFormat> render_formats;
    std::vector<AudioFormat> audio_formats;
    std::vector<std::filesystem::path> soundfonts; // EngineOptions::soundfonts, in order
    std::int32_t audio_sample_rate = 0;
};

struct TemplateInfo {
    std::string id;       // e.g. "Treble_Clef"
    std::string title;    // e.g. "Treble Clef"
    std::string category; // e.g. "General"
    std::filesystem::path path; // the template's .mscx
};

// Inputs larger than this are rejected with ResourceLimit.
inline constexpr std::size_t max_input_bytes = 8 * 1024 * 1024;

struct OpenOptions {
    // Unset: detect the format from the content, then from the extension of
    // filename_hint.
    std::optional<Format> format;
    // Only its extension is used. open(path) defaults it to the path.
    std::filesystem::path filename_hint;
    // Reject a score whose element ids are malformed or duplicated. By default
    // such a score opens, the affected elements get fresh ids, and a warning
    // diagnostic says how many were repaired.
    bool strict_ids = false;
};

// Exactly one of template_id and template_path must be set. Parts, measures,
// key and tempo come from the template.
struct CreateOptions {
    std::string template_id;              // a TemplateInfo::id
    std::filesystem::path template_path;  // an .mscx in an MSCX directory
    std::string title;
    std::string subtitle;
    std::string composer;
    std::string lyricist;
};

// The engine runtime. A process has at most one Engine, created once: a
// second create() aborts, even after the first Engine was destroyed. Every
// call on the Engine and its Documents must come from the thread that created
// it; a call from another thread aborts.
struct Engine {
    // Fails without consuming the one Engine of the process when the
    // resources or a font path do not exist. A font that exists but cannot be
    // used fails after the runtime started, so the process cannot retry.
    static Result<std::unique_ptr<Engine>> create(const EngineOptions& options);

    // Aborts while Documents are still open.
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Capabilities capabilities() const;

    // The templates listed in resources/templates/categories.json, in that
    // file's order.
    std::vector<TemplateInfo> templates() const;

    // Unsupported for MSCX/MSCZ written before MuseScore 4.6 (mscVersion < 460).
    Result<std::unique_ptr<Document>> open(std::span<const std::byte> data, const OpenOptions& options = {});
    // Unsupported for MSCX/MSCZ written before MuseScore 4.6 (mscVersion < 460).
    Result<std::unique_ptr<Document>> open(const std::filesystem::path& file, const OpenOptions& options = {});

    // A new score from a template, at revision 1.
    Result<std::unique_ptr<Document>> create_document(const CreateOptions& options);

    // The catalogue: every element type the engine recognises, sorted by name,
    // with the properties its class handles, and every style in style id order.
    // The tables are static; no score is read.  NotFound for a type name the
    // engine does not know.
    std::vector<TypeInfo> describe() const;
    Result<TypeInfo> describe(std::string_view type) const;
    std::vector<StyleInfo> describe_styles() const;

private:
    struct Impl;
    explicit Engine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace engrave
