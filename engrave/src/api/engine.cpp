#include <engrave/engine.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <format>
#include <fstream>
#include <new>

#include <nlohmann/json.hpp>

#include "muse_framework_config.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/text.h"
#include "engraving/engravingproject.h"

#include "engrave/src/api/sniff.h"
#include "engrave/src/api/state.h"
#include "engrave/src/runtime/runtime_assets.h"

namespace engrave {
namespace {
std::atomic<bool> s_engine_created = false;

std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size());
}

const char* backend_name(Format format) {
    switch (format) {
    case Format::Mscz: return "mscz";
    case Format::Mscx: return "mscx";
    case Format::MusicXml: return "musicxml";
    case Format::Mxl: return "mxl";
    case Format::Midi: return "midi";
    case Format::GuitarPro: return "gp";
    }
    return "";
}

// The upstream template names carry an ordering prefix: "01-Treble_Clef".
std::string template_id(const std::filesystem::path& mscx) {
    std::string stem = utf8(mscx.stem());
    const auto dash = stem.find('-');
    if (dash != std::string::npos && dash > 0
        && std::all_of(stem.begin(), stem.begin() + dash, [](unsigned char c) { return c >= '0' && c <= '9'; }))
        stem.erase(0, dash + 1);
    return stem;
}

// The metadata half of upstream's MasterNotation::applyOptions for a score
// created from a template: the meta tags and the title frame's texts.
void apply_metadata(mu::engraving::MasterScore* score, const CreateOptions& options) {
    using mu::engraving::TextStyleType;
    struct Field {
        const char16_t* tag;
        TextStyleType style;
        const std::string& value;
    };
    const Field fields[] = {
        {u"workTitle", TextStyleType::TITLE, options.title},
        {u"subtitle", TextStyleType::SUBTITLE, options.subtitle},
        {u"composer", TextStyleType::COMPOSER, options.composer},
        {u"lyricist", TextStyleType::LYRICIST, options.lyricist},
    };
    mu::engraving::ScoreLoad load;
    for (const Field& field : fields) {
        if (field.value.empty()) continue;
        const muse::String text = muse::String::fromStdString(field.value);
        score->setMetaTag(field.tag, text);
        mu::engraving::TextBase* item = score->getText(field.style);
        if (!item) item = score->addText(field.style);
        if (item) item->setPlainText(text);
    }
    score->setLayoutAll();
    score->update();
}
} // namespace

Engine::Engine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Result<std::unique_ptr<Engine>> Engine::create(const EngineOptions& options) {
    check(!s_engine_created, "engrave supports one Engine per process");
    std::error_code ec;
    const auto resources = std::filesystem::absolute(options.resources, ec);
    if (options.resources.empty() || !std::filesystem::is_regular_file(resources / "engraving/instruments/instruments.xml", ec))
        return fail(ErrorCode::NotFound, "no engrave resources in '" + utf8(options.resources) + "'");
    for (const auto& font : options.fonts) {
        if (!std::filesystem::is_regular_file(font, ec)) return fail(ErrorCode::NotFound, "no font file '" + utf8(font) + "'");
    }
    check(!s_engine_created.exchange(true), "engrave supports one Engine per process");

    auto impl = std::make_unique<Impl>();
    impl->state.resources = resources;
    if (!options.cache_dir.empty()) music::set_runtime_cache_root(utf8(std::filesystem::absolute(options.cache_dir)));
    music::initialize_native_runtime(utf8(resources));
    std::unique_ptr<Engine> engine(new Engine(std::move(impl)));
    for (const auto& font : options.fonts) {
        try {
            music::add_runtime_font(utf8(std::filesystem::absolute(font)));
        } catch (const std::runtime_error& error) {
            return fail(ErrorCode::InvalidInput, error.what());
        }
    }
    return engine;
}

Engine::~Engine() {
    impl_->state.check_thread();
    check(impl_->state.live_documents == 0, "an Engine was destroyed while Documents were open");
    try {
        music::shutdown_runtime_assets();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "engrave: runtime asset cleanup failed: %s\n", error.what());
    }
}

Capabilities Engine::capabilities() const {
    impl_->state.check_thread();
    return {
        .engine_version = MUSE_APP_VERSION,
        .import_formats = {Format::Mscz, Format::Mscx, Format::MusicXml, Format::Mxl, Format::Midi, Format::GuitarPro},
        .export_formats = {Format::Mscz, Format::MusicXml, Format::Mxl, Format::Midi},
    };
}

std::vector<TemplateInfo> Engine::templates() const {
    impl_->state.check_thread();
    const auto root = impl_->state.resources / "templates";
    std::ifstream file(root / "categories.json", std::ios::binary);
    if (!file) return {};
    std::vector<TemplateInfo> result;
    for (const auto& category : nlohmann::json::parse(file)) {
        for (const auto& entry : category.at("files")) {
            const auto mscx = root / path_from_utf8(entry.get<std::string>());
            std::string id = template_id(mscx);
            std::string title = id;
            std::ranges::replace(title, '_', ' ');
            result.push_back({std::move(id), std::move(title), category.at("title").get<std::string>(), mscx});
        }
    }
    return result;
}

Result<std::unique_ptr<Document>> Engine::open(std::span<const std::byte> data, const OpenOptions& options) {
    impl_->state.check_thread();
    if (data.empty()) return fail(ErrorCode::InvalidInput, "the input is empty");
    if (data.size() > max_input_bytes)
        return fail(ErrorCode::ResourceLimit, std::format("the input exceeds {} bytes", max_input_bytes));
    const std::string bytes(reinterpret_cast<const char*>(data.data()), data.size());

    std::vector<Diagnostic> diagnostics;
    std::optional<Format> format = options.format ? options.format : sniff_format(bytes);
    if (!format) {
        format = format_from_extension(options.filename_hint);
        if (!format) return fail(ErrorCode::UnsupportedFormat, "the data matches no score format the engine reads");
        diagnostics.push_back({Severity::Info, std::format("the content was not recognised; reading it as {} because of its file name",
                                                           to_string(*format))});
    }
    music::ScoreProject project;
    try {
        project = music::load_native_score(bytes, backend_name(*format));
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        return fail(ErrorCode::InvalidInput, std::format("cannot read the data as {}: {}", to_string(*format), error.what()));
    }
    if (project->isCorruptedUponLoading())
        diagnostics.push_back({Severity::Warning, "the score failed the engine's sanity check after loading"});
    return Document::Impl::make(impl_->state, std::move(project), std::move(diagnostics));
}

Result<std::unique_ptr<Document>> Engine::open(const std::filesystem::path& file, const OpenOptions& options) {
    impl_->state.check_thread();
    std::error_code ec;
    const auto status = std::filesystem::status(file, ec);
    if (!std::filesystem::exists(status)) return fail(ErrorCode::NotFound, "no file '" + utf8(file) + "'");
    if (!std::filesystem::is_regular_file(status)) return fail(ErrorCode::InvalidArgument, "'" + utf8(file) + "' is not a file");
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) return fail(ErrorCode::Io, "cannot read '" + utf8(file) + "': " + ec.message());
    if (size > max_input_bytes)
        return fail(ErrorCode::ResourceLimit, std::format("'{}' exceeds {} bytes", utf8(file), max_input_bytes));

    std::vector<std::byte> data(size);
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
        return fail(ErrorCode::Io, "cannot read '" + utf8(file) + "'");
    OpenOptions effective = options;
    if (effective.filename_hint.empty()) effective.filename_hint = file;
    return open(data, effective);
}

Result<std::unique_ptr<Document>> Engine::create_document(const CreateOptions& options) {
    impl_->state.check_thread();
    if (options.template_id.empty() == options.template_path.empty())
        return fail(ErrorCode::InvalidArgument, "set exactly one of template_id and template_path");
    auto mscx = options.template_path;
    if (!options.template_id.empty()) {
        const auto all = templates();
        const auto it = std::ranges::find(all, options.template_id, &TemplateInfo::id);
        if (it == all.end()) return fail(ErrorCode::NotFound, "no template '" + options.template_id + "'");
        mscx = it->path;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(mscx, ec)) return fail(ErrorCode::NotFound, "no template file '" + utf8(mscx) + "'");
    if (mscx.extension() != ".mscx")
        return fail(ErrorCode::InvalidArgument, "a template is an .mscx file, not '" + utf8(mscx) + "'");

    music::ScoreProject project;
    try {
        project = music::load_native_score_dir(utf8(std::filesystem::absolute(mscx)));
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        return fail(ErrorCode::InvalidInput, std::format("cannot read the template '{}': {}", utf8(mscx), error.what()));
    }
    apply_metadata(project->masterScore(), options);
    return Document::Impl::make(impl_->state, std::move(project), {});
}

} // namespace engrave
