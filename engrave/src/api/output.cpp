// The output API: page_count() and render().  Both take the score through the
// handle resolver and the page drawing through the A2 renderer in
// engrave/src/render/, with A2's options and limits.

#include <engrave/document.h>

#include <format>
#include <string>
#include <vector>

#include "engrave/src/api/handles.h"
#include "engrave/src/api/state.h"
#include "engrave/src/render/score_render.h"
#include "engrave/src/score/export_options.h"

#include "engraving/dom/masterscore.h"
#include "engraving/dom/score.h"
#include "engraving/engravingproject.h"

namespace engrave {
namespace {

std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

std::vector<std::byte> bytes_of(std::string data) {
    const auto* first = reinterpret_cast<const std::byte*>(data.data());
    return std::vector<std::byte>(first, first + data.size());
}

// An excerpt the file carried is laid out by the loader, and an edit lays out the
// part it touched; a score that never reached the layout has no page to draw yet.
void prepare_layout(mu::engraving::Score& score) {
    if (score.npages() == 0 && score.nmeasures() > 0) {
        score.doLayout();
        score.update();
    }
}

const char* backend_name(RenderFormat format) {
    switch (format) {
    case RenderFormat::Pdf: return "pdf";
    case RenderFormat::Png: return "png";
    case RenderFormat::Svg: return "svg";
    }
    return "";
}

} // namespace

Result<std::int32_t> Document::page_count(const ScoreSel& selection) {
    impl_->engine->check_thread();
    const Result<mu::engraving::Score*> selected = select_score(*impl_->project->masterScore(), selection);
    if (!selected) return std::unexpected(selected.error());
    prepare_layout(**selected);
    return static_cast<std::int32_t>((*selected)->npages());
}

Result<RenderOutput> Document::render(const RenderOptions& options) {
    impl_->engine->check_thread();
    const Result<mu::engraving::Score*> selected = select_score(*impl_->project->masterScore(), options.score);
    if (!selected) return std::unexpected(selected.error());
    mu::engraving::Score& score = **selected;
    prepare_layout(score);

    if (options.dpi < min_render_dpi || options.dpi > max_render_dpi) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the dpi must be between {} and {}, not {}", min_render_dpi, max_render_dpi, options.dpi));
    }
    // A2's other renderers draw at 72 dpi (PDF points, SVG units) and take no dpi.
    if (options.format != RenderFormat::Png && options.dpi != RenderOptions{}.dpi) {
        return fail(ErrorCode::InvalidArgument, std::format("dpi applies to PNG, not to {}", to_string(options.format)));
    }
    const auto npages = static_cast<std::int32_t>(score.npages());
    if (npages == 0) return fail(ErrorCode::InvalidArgument, "the score has no pages");
    const std::int32_t first = options.first_page;
    const std::int32_t last = options.last_page < 0 ? npages - 1 : options.last_page;
    if (first < 0 || last < first || last >= npages) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the page range {}..{} is outside the score's 0..{}", first, last, npages - 1));
    }

    music::ExportOptions backend;
    backend.png_dpi = options.dpi;
    backend.draw_page_background = options.background;
    backend.transparent = options.transparent;
    backend.first_page = first;
    backend.last_page = last;
    const std::string format = backend_name(options.format);
    try {
        RenderOutput output;
        if (options.format == RenderFormat::Pdf) {
            output.document = bytes_of(music::render_score(*impl_->engine->context, &score, format, backend));
            return output;
        }
        output.pages.reserve(static_cast<std::size_t>(last - first + 1));
        for (std::int32_t page = first; page <= last; ++page) {
            backend.page = page;
            output.pages.push_back(bytes_of(music::render_score(*impl_->engine->context, &score, format, backend)));
        }
        return output;
    } catch (const music::RenderLimitExceeded& error) {
        return fail(ErrorCode::ResourceLimit, error.what());
    } catch (const std::invalid_argument& error) {
        return fail(ErrorCode::InvalidArgument, error.what());
    }
}

} // namespace engrave
