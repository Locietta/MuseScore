#include "score_render.h"
#include "cairo_painter.h"
#include "engrave/src/runtime/engine_context.h"
#include "draw/painter.h"
#include "engraving/dom/score.h"
#include "engraving/dom/mscore.h"
#include "engraving/rendering/iscorerenderer.h"
#include <cairo.h>
#include <cairo-pdf.h>
#include <cairo-svg.h>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace music {
namespace {
cairo_status_t append_bytes(void* context, const unsigned char* bytes, unsigned length) noexcept {
    try { static_cast<std::string*>(context)->append(reinterpret_cast<const char*>(bytes), length); return CAIRO_STATUS_SUCCESS; }
    catch (...) { return CAIRO_STATUS_WRITE_ERROR; }
}
void check(cairo_status_t status) {
    if (status != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(status));
}
}

std::string render_score(const EngineContext& engine, mu::engraving::Score* score, const std::string& format, int page) {
    ExportOptions options;
    options.page = page;
    return render_score(engine, score, format, options);
}

std::string render_score(const EngineContext& engine, mu::engraving::Score* score, const std::string& format,
                         const ExportOptions& options) {
    using namespace mu::engraving;
    if (!score || !score->npages()) throw std::runtime_error("Score has no pages");
    const int npages = static_cast<int>(score->npages());
    if (options.page < 0 || (format != "pdf" && options.page >= npages))
        throw std::invalid_argument("Page out of range");
    // A PDF holds a range of pages; PNG and SVG render options.page alone.
    const int firstPage = options.first_page;
    const int lastPage = options.last_page >= 0 ? options.last_page : npages - 1;
    if (format == "pdf" && (firstPage < 0 || lastPage < firstPage || lastPage >= npages))
        throw std::invalid_argument("Page range out of range");
    // 5.0 drops the context IoC: IScoreRenderer is a global interface registered
    // by EngravingModule, and resolve() looks the export up by interface id.
    const auto renderer = muse::modularity::globalIoc()->resolve<rendering::IScoreRenderer>("native");
    if (!renderer) throw std::runtime_error("Score renderer unavailable");
    const auto inches = renderer->pageSizeInch(score);
    int dpi = 72;
    if (format == "png") {
        if (!std::isfinite(static_cast<double>(options.png_dpi)) || options.png_dpi < 36 || options.png_dpi > 1200)
            throw std::invalid_argument("PNG DPI must be between 36 and 1200");
        dpi = options.png_dpi;
    }
    const double width = inches.width() * dpi, height = inches.height() * dpi;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0
        || width > 32767 || height > 32767 || width * height > 64e6)
        throw RenderLimitExceeded("Page dimensions exceed rendering limit");
    std::string output;
    cairo_surface_t* raw = nullptr;
    if (format == "pdf") raw = cairo_pdf_surface_create_for_stream(append_bytes, &output, width, height);
    else if (format == "svg") raw = cairo_svg_surface_create_for_stream(append_bytes, &output, width, height);
    else if (format == "png") raw = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(std::ceil(width)), static_cast<int>(std::ceil(height)));
    else throw std::invalid_argument("Unsupported render format");
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(raw, cairo_surface_destroy);
    check(cairo_surface_status(surface.get()));
    std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()), cairo_destroy);
    check(cairo_status(context.get()));
    // Vector surfaces start with an unbounded clip, which Cairo cannot enumerate.
    cairo_rectangle(context.get(), 0, 0, std::ceil(width), std::ceil(height));
    cairo_clip(context.get());
    if (format == "png" && !options.transparent) {
        cairo_save(context.get());
        cairo_set_source_rgb(context.get(), 1.0, 1.0, 1.0);
        cairo_paint(context.get());
        cairo_restore(context.get());
        check(cairo_status(context.get()));
    }
    auto provider = std::make_shared<muse::draw::CairoPainter>(context.get(), engine.fontsDatabase(), dpi);
    const bool wasPrinting = score->printing(), wasPdf = MScore::pdfPrinting;
    struct Restore {
        Score* score; bool printing, pdf;
        ~Restore() { score->setPrinting(printing); MScore::pdfPrinting = pdf; }
    } restore{score, wasPrinting, wasPdf};
    {
        muse::draw::Painter painter(provider, "native-export");
        rendering::IScoreRenderer::ScorePaintOptions paintOptions;
        paintOptions.isPrinting = true;
        paintOptions.deviceDpi = dpi;
        paintOptions.fromPage = format == "pdf" ? firstPage : options.page;
        paintOptions.toPage = format == "pdf" ? lastPage : options.page;
        paintOptions.onNewPage = [&] { cairo_show_page(context.get()); check(cairo_status(context.get())); };
        paintOptions.printPageBackground = format == "pdf" ? true : options.draw_page_background;
        renderer->paintScore(&painter, score, paintOptions);
        if (!painter.endDraw()) throw std::runtime_error("Painter did not finish");
    }
    check(cairo_status(context.get()));
    if (format == "png") check(cairo_surface_write_to_png_stream(surface.get(), append_bytes, &output));
    else cairo_show_page(context.get());
    cairo_surface_finish(surface.get());
    check(cairo_surface_status(surface.get()));
    return output;
}
}
