#include <engrave/document.h>

#include <format>
#include <iterator>

#include "engrave/src/api/handles.h"
#include "engrave/src/api/state.h"
#include "engrave/src/score/export_options.h"

#include "engraving/engravingproject.h"

namespace engrave {

std::unique_ptr<Document> Document::Impl::make(EngineState& engine, music::ScoreProject project,
                                               std::vector<Diagnostic> diagnostics) {
    auto impl = std::make_unique<Impl>();
    impl->engine = &engine;
    impl->project = std::move(project);
    impl->diagnostics = std::move(diagnostics);
    // Every path that produces a document (open, create, and later a restore) hands
    // out ids here, so that an element always has one before a caller can name it.
    assign_missing_ids(*impl->project->masterScore());
    ++engine.live_documents;
    return std::unique_ptr<Document>(new Document(std::move(impl)));}

Document::Document(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

mu::engraving::MasterScore* DocumentAccess::master_score(Document& document) {
    return document.impl_->project->masterScore();
}

music::ScoreProject& DocumentAccess::project(Document& document) {
    return document.impl_->project;
}

api::EditHooks& DocumentAccess::edit_hooks(Document& document) {
    return document.impl_->edit_hooks;
}

Document::~Document() {
    impl_->engine->check_thread();
    --impl_->engine->live_documents;
}

std::uint64_t Document::revision() const {
    impl_->engine->check_thread();
    return impl_->revision;
}

const std::vector<Diagnostic>& Document::diagnostics() const {
    impl_->engine->check_thread();
    return impl_->diagnostics;
}

Result<std::vector<std::byte>> Document::export_score(const ExportOptions& options) {
    impl_->engine->check_thread();
    if (impl_->poisoned) return std::unexpected(Error{ErrorCode::Poisoned, "an earlier edit left the document unusable"});
    const char* name = nullptr;
    switch (options.format) {
    case Format::Mscz: name = "mscz"; break;
    case Format::MusicXml: name = "musicxml"; break;
    case Format::Mxl: name = "mxl"; break;
    case Format::Midi: name = "midi"; break;
    case Format::Mscx:
    case Format::GuitarPro:
        return std::unexpected(Error{ErrorCode::Unsupported,
                                     std::format("the engine does not write {}", to_string(options.format))});
    }
    music::ExportOptions backend;
    backend.midi_expand_repeats = options.midi_expand_repeats;
    backend.midi_export_rpns = options.midi_rpns;
    const std::string data = music::export_native_score(impl_->project, name, backend, {});
    const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
    return std::vector<std::byte>(bytes, bytes + data.size());
}

} // namespace engrave
