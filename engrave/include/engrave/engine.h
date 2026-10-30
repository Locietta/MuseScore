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
};

struct Capabilities {
    std::string engine_version; // written into saved scores, e.g. "5.0.0"
    std::vector<Format> import_formats;
    std::vector<Format> export_formats;
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

    Result<std::unique_ptr<Document>> open(std::span<const std::byte> data, const OpenOptions& options = {});
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
