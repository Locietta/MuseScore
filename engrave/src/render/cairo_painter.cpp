/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Qt-free Cairo implementation of muse::draw::IPaintProvider.
 *
 * Text uses the font bytes actually registered in IFontsDatabase.  FreeType
 * memory faces are cached for the process lifetime, Cairo FT font faces are
 * created on top of them, and HarfBuzz performs shaping so glyph advances and
 * mark offsets are honoured.
 */

#include "cairo_painter.h"
#include "draw/internal/fontfaceutils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>
#include <hb-ft.h>

#include "draw/internal/ifontsdatabase.h"

namespace muse::draw {
namespace {

// ---------------------------------------------------------------- utilities

bool isFinite(double v)
{
    return std::isfinite(v);
}

bool finiteRect(const RectF& r)
{
    return isFinite(r.x()) && isFinite(r.y()) && isFinite(r.width()) && isFinite(r.height());
}

bool finitePoint(const PointF& p)
{
    return isFinite(p.x()) && isFinite(p.y());
}

bool finiteTransform(const Transform& t)
{
    return isFinite(t.m11()) && isFinite(t.m12()) && isFinite(t.m13())
           && isFinite(t.m21()) && isFinite(t.m22()) && isFinite(t.m23())
           && isFinite(t.m31()) && isFinite(t.m32()) && isFinite(t.m33());
}

bool nearlyEqual(double a, double b)
{
    const double diff = std::abs(a - b);
    const double scale = std::max(1.0, std::max(std::abs(a), std::abs(b)));
    return diff <= 1e-9 * scale;
}

bool transformNearlyEqual(const Transform& a, const Transform& b)
{
    return nearlyEqual(a.m11(), b.m11())
           && nearlyEqual(a.m12(), b.m12())
           && nearlyEqual(a.m13(), b.m13())
           && nearlyEqual(a.m21(), b.m21())
           && nearlyEqual(a.m22(), b.m22())
           && nearlyEqual(a.m23(), b.m23())
           && nearlyEqual(a.m31(), b.m31())
           && nearlyEqual(a.m32(), b.m32())
           && nearlyEqual(a.m33(), b.m33());
}

bool isSpaceChar(char16_t c)
{
    return muse::Char::isSpace(c);
}

void uniqueCoordinates(std::vector<double>& values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end(), [](double a, double b) {
        return std::abs(a - b) <= 1e-9 * std::max({ 1.0, std::abs(a), std::abs(b) });
    }), values.end());
}

//! Exact (floating-point) rectangle subtraction on a rectilinear grid.  The
//! result is a set of disjoint rectangles representing background minus the
//! union of masks.  This keeps PDF/SVG clipping vector-accurate instead of
//! snapping to integer device pixels, and handles overlapping masks.
std::vector<cairo_rectangle_t> subtractRects(const cairo_rectangle_t& background,
                                             const std::vector<cairo_rectangle_t>& masks)
{
    std::vector<cairo_rectangle_t> result;
    if (!(background.width > 0.0) || !(background.height > 0.0)) {
        return result;
    }

    const double backgroundTop = background.y;
    const double backgroundBottom = background.y + background.height;

    std::vector<double> ys;
    ys.push_back(backgroundTop);
    ys.push_back(backgroundBottom);
    for (const cairo_rectangle_t& mask : masks) {
        if (!(mask.width > 0.0) || !(mask.height > 0.0)) {
            continue;
        }
        const double top = std::max(backgroundTop, mask.y);
        const double bottom = std::min(backgroundBottom, mask.y + mask.height);
        if (bottom > top) {
            ys.push_back(top);
            ys.push_back(bottom);
        }
    }
    uniqueCoordinates(ys);
    if (ys.size() < 2) {
        return result;
    }

    const double backgroundLeft = background.x;
    const double backgroundRight = background.x + background.width;

    std::map<std::pair<double, double>, size_t> previousSpans;
    for (size_t yi = 0; yi + 1 < ys.size(); ++yi) {
        const double y0 = ys[yi];
        const double y1 = ys[yi + 1];
        if (!(y1 > y0)) {
            continue;
        }

        const double cy = (y0 + y1) / 2.0;
        std::vector<std::pair<double, double> > blocked;
        for (const cairo_rectangle_t& mask : masks) {
            if (mask.width <= 0.0 || mask.height <= 0.0) {
                continue;
            }
            if (cy >= mask.y && cy <= mask.y + mask.height) {
                blocked.emplace_back(mask.x, mask.x + mask.width);
            }
        }
        std::sort(blocked.begin(), blocked.end());

        std::vector<std::pair<double, double> > merged;
        for (const std::pair<double, double>& interval : blocked) {
            if (merged.empty() || interval.first > merged.back().second) {
                merged.push_back(interval);
            } else {
                merged.back().second = std::max(merged.back().second, interval.second);
            }
        }

        std::map<std::pair<double, double>, size_t> currentSpans;
        auto emitRun = [&](double x0, double x1) {
            if (!(x1 > x0)) {
                return;
            }
            const std::pair<double, double> span(x0, x1);
            auto previous = previousSpans.find(span);
            if (previous != previousSpans.end()) {
                cairo_rectangle_t& rect = result[previous->second];
                if (std::abs(rect.y + rect.height - y0) <= 1e-9 * std::max(1.0, std::abs(y0))) {
                    rect.height = y1 - rect.y;
                    currentSpans[span] = previous->second;
                    return;
                }
            }
            cairo_rectangle_t rect;
            rect.x = x0;
            rect.y = y0;
            rect.width = x1 - x0;
            rect.height = y1 - y0;
            result.push_back(rect);
            currentSpans[span] = result.size() - 1;
        };

        double cursor = backgroundLeft;
        for (const std::pair<double, double>& interval : merged) {
            if (interval.second <= cursor) {
                continue;
            }
            if (interval.first > cursor) {
                emitRun(cursor, std::min(interval.first, backgroundRight));
            }
            cursor = std::max(cursor, interval.second);
            if (cursor >= backgroundRight) {
                break;
            }
        }
        if (cursor < backgroundRight) {
            emitRun(cursor, backgroundRight);
        }

        previousSpans.swap(currentSpans);
    }

    return result;
}

std::u16string normalizeText(const std::u16string& in, bool expandTabs)
{
    std::u16string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char16_t c = in[i];
        if (c == u'\r') {
            if (i + 1 < in.size() && in[i + 1] == u'\n') {
                ++i;
            }
            out.push_back(u'\n');
        } else if (c == u'\t' && expandTabs) {
            out.append(8, u' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<std::u16string> splitParagraphs(const std::u16string& text)
{
    std::vector<std::u16string> out;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == u'\n') {
            out.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(text.substr(start));
    return out;
}

std::u16string joinWithSpaces(const std::vector<std::u16string>& paragraphs)
{
    std::u16string out;
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (i > 0) {
            out.push_back(u' ');
        }
        out += paragraphs[i];
    }
    return out;
}

uint64_t fnv1a(const uint8_t* data, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

struct PngReadClosure {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

cairo_status_t readPngStream(void* closure, unsigned char* data, unsigned int length)
{
    PngReadClosure* c = static_cast<PngReadClosure*>(closure);
    if (!c || !data) {
        return CAIRO_STATUS_READ_ERROR;
    }
    if (length > c->size - c->pos) {
        return CAIRO_STATUS_READ_ERROR;
    }
    std::memcpy(data, c->data + c->pos, length);
    c->pos += length;
    return CAIRO_STATUS_SUCCESS;
}

struct CairoStateGuard {
    explicit CairoStateGuard(cairo_t* context)
        : cr(context) { cairo_save(cr); }
    ~CairoStateGuard() { cairo_restore(cr); }

    CairoStateGuard(const CairoStateGuard&) = delete;
    CairoStateGuard& operator=(const CairoStateGuard&) = delete;

    cairo_t* cr;
};

// ------------------------------------------------------------ font resources

struct FtFaceEntry {
    FontDataKey key;
    std::vector<uint8_t> data;
    FT_Face ft = nullptr;
    hb_face_t* hb = nullptr;
    unsigned int ftUpem = 1000;
    unsigned int hbUpem = 1000;
    int ascender = 0;
    int descender = 0;
    int height = 0;
    int underlinePosition = 0;
    int underlineThickness = 0;
    // Lazily created; mutable so text drawing can use a const face entry.
    mutable std::array<cairo_font_face_t*, 4> cairoFaces { nullptr, nullptr, nullptr, nullptr };
    mutable std::array<FT_Face, 4> variantFaces { nullptr, nullptr, nullptr, nullptr };

    ~FtFaceEntry()
    {
        for (cairo_font_face_t*& f : cairoFaces) {
            if (f) {
                cairo_font_face_destroy(f);
                f = nullptr;
            }
        }
        for (FT_Face variant : variantFaces) {
            if (variant) FT_Done_Face(variant);
        }
        if (hb) {
            hb_face_destroy(hb);
            hb = nullptr;
        }
        if (ft) {
            FT_Done_Face(ft);
            ft = nullptr;
        }
    }

    cairo_font_face_t* cairoFace(bool synthBold, bool synthItalic) const
    {
        const size_t index = (synthBold ? 1u : 0u) | (synthItalic ? 2u : 0u);
        cairo_font_face_t*& slot = cairoFaces[index];
        if (slot) {
            return slot;
        }

        // Cairo can reuse a face for the same FT_Face; synthesis flags must not
        // mutate the face used by a previously rendered plain style.
        if (!variantFaces[index] && FT_New_Memory_Face(ft->glyph->library, data.data(),
                static_cast<FT_Long>(data.size()), ft->face_index, &variantFaces[index]) != 0) {
            throw std::runtime_error("CairoPainter: cannot create independent font style face");
        }
        cairo_font_face_t* face = cairo_ft_font_face_create_for_ft_face(variantFaces[index], 0);
        cairo_status_t status = cairo_font_face_status(face);
        if (status != CAIRO_STATUS_SUCCESS) {
            cairo_font_face_destroy(face);
            throw std::runtime_error(std::string("CairoPainter: cannot create Cairo FT font face: ")
                                     + cairo_status_to_string(status));
        }

        if (synthBold || synthItalic) {
            unsigned int flags = 0;
            if (synthBold) {
                flags |= CAIRO_FT_SYNTHESIZE_BOLD;
            }
            if (synthItalic) {
                flags |= CAIRO_FT_SYNTHESIZE_OBLIQUE;
            }
            cairo_ft_font_face_set_synthesize(face, flags);
            status = cairo_font_face_status(face);
            if (status != CAIRO_STATUS_SUCCESS) {
                cairo_font_face_destroy(face);
                throw std::runtime_error(std::string("CairoPainter: cannot configure synthesized font face: ")
                                         + cairo_status_to_string(status));
            }
        }

        slot = face;
        return slot;
    }
};

class FontCache
{
public:
    std::shared_ptr<FtFaceEntry> get(const Font& font, const IFontsDatabase& db)
    {
        const FontDataKey requested = dataKeyForFont(font);
        if (!requested.valid()) {
            throw std::runtime_error("CairoPainter: text drawing requires a non-empty font family");
        }

        const CacheKey requestKey = std::make_pair(requested, font.type());

        std::lock_guard<std::mutex> lock(m_mutex);
        auto requestIt = m_byRequest.find(requestKey);
        if (requestIt != m_byRequest.end()) {
            return requestIt->second;
        }

        const FontData fontData = db.fontData(requested, font.type());
        if (!fontData.valid()) {
            throw std::runtime_error("CairoPainter: IFontsDatabase returned no font data for family '"
                                     + requested.family().id().toStdString() + "'");
        }

        const CacheKey actualKey = std::make_pair(fontData.key, font.type());
        auto actualIt = m_byActual.find(actualKey);
        if (actualIt != m_byActual.end()) {
            m_byRequest[requestKey] = actualIt->second;
            return actualIt->second;
        }

        if (!m_ftLibrary) {
            if (FT_Init_FreeType(&m_ftLibrary) != 0) {
                throw std::runtime_error("CairoPainter: FT_Init_FreeType failed");
            }
        }

        if (fontData.data.size() > static_cast<size_t>(std::numeric_limits<FT_Long>::max())) {
            throw std::runtime_error("CairoPainter: font file is too large for FreeType");
        }

        auto entry = std::make_shared<FtFaceEntry>();
        entry->key = fontData.key;
        entry->data.assign(fontData.data.constData(), fontData.data.constData() + fontData.data.size());
        if (entry->data.empty()) {
            throw std::runtime_error("CairoPainter: empty font file");
        }

        FT_Error ftError = newMatchingMemoryFace(m_ftLibrary, entry->data.data(),
                                               static_cast<FT_Long>(entry->data.size()), fontData.key, &entry->ft);
        if (ftError != 0 || !entry->ft) {
            throw std::runtime_error("CairoPainter: FT_New_Memory_Face failed for family '"
                                     + fontData.key.family().id().toStdString() + "'");
        }

        entry->ftUpem = entry->ft->units_per_EM ? static_cast<unsigned int>(entry->ft->units_per_EM) : 1000u;
        entry->ascender = static_cast<int>(entry->ft->ascender);
        entry->descender = static_cast<int>(entry->ft->descender);
        entry->height = static_cast<int>(entry->ft->height);
        entry->underlinePosition = static_cast<int>(entry->ft->underline_position);
        entry->underlineThickness = static_cast<int>(entry->ft->underline_thickness);

        entry->hb = hb_ft_face_create_referenced(entry->ft);
        if (!entry->hb) {
            throw std::runtime_error("CairoPainter: hb_ft_face_create_referenced failed");
        }
        entry->hbUpem = hb_face_get_upem(entry->hb);
        if (!entry->hbUpem) {
            entry->hbUpem = entry->ftUpem;
        }

        m_byActual[actualKey] = entry;
        m_byRequest[requestKey] = entry;
        return entry;
    }

private:
    using CacheKey = std::pair<FontDataKey, Font::Type>;

    FT_Library m_ftLibrary = nullptr;
    std::mutex m_mutex;
    std::map<CacheKey, std::shared_ptr<FtFaceEntry> > m_byActual;
    std::map<CacheKey, std::shared_ptr<FtFaceEntry> > m_byRequest;
};

FontCache& fontCache()
{
    // Deliberately process-lifetime: Cairo/FreeType font faces must outlive all
    // painters, and avoiding static destruction removes a shutdown-order hazard.
    static FontCache* cache = new FontCache();
    return *cache;
}

struct ShapedLine {
    std::u16string text;
    std::vector<cairo_glyph_t> glyphs;
    std::vector<unsigned int> clusters;
    double width = 0.0;
};

double ascentFor(const FtFaceEntry& entry, double pixelSize)
{
    return std::max(0.0, double(entry.ascender) * pixelSize / double(entry.ftUpem));
}

double descentFor(const FtFaceEntry& entry, double pixelSize)
{
    return std::max(0.0, -double(entry.descender) * pixelSize / double(entry.ftUpem));
}

double lineHeightFor(const FtFaceEntry& entry, double pixelSize)
{
    const double asc = ascentFor(entry, pixelSize);
    const double desc = descentFor(entry, pixelSize);
    double height = double(entry.height) * pixelSize / double(entry.ftUpem);
    height = std::max(height, asc + desc);
    if (!(height > 0.0)) {
        height = pixelSize;
    }
    return height;
}

} // namespace

// ------------------------------------------------------------------- Impl

struct CairoPainter::Impl {
    struct State {
        Font font;
        Pen pen;
        Brush brush = Brush(BrushStyle::NoBrush);
        Transform worldTransform;
        Transform viewTransform;
        Transform transform;
        RectF window;
        RectF viewport;
        bool hasView = false;
        //! Once setTransform() has been used, its matrix is authoritative.
        //! muse::draw::Painter composes window/viewport itself and passes the
        //! complete matrix here; before that, window/viewport are composed for
        //! callers that use this provider directly.
        bool transformExplicit = false;
        bool antialiasing = false;
        CompositionMode composition = CompositionMode::SourceOver;
        bool clippingEnabled = true;
        bool hasUserClip = false;
        std::vector<cairo_rectangle_t> userClipRects;
    };

    struct CachedSurface {
        size_t size = 0;
        uint64_t hash = 0;
        cairo_surface_t* surface = nullptr;
    };

    cairo_t* cr = nullptr;
    const IFontsDatabase& fonts;
    double dpi = 1200.0;
    bool active = true;
    State state;
    std::vector<State> stack;
    std::vector<cairo_rectangle_t> baseClip;
    // PDF/SVG surfaces may report CLIP_NOT_REPRESENTABLE even for an unclipped
    // context.  In that case we conservatively assume the surface bounds.
    bool baseClipKnown = true;
    std::map<unsigned int, CachedSurface> surfaces;

    Impl(cairo_t* context, const IFontsDatabase& fontsDatabase, double deviceDpi);
    ~Impl();

    bool isActive() const;
    void beginTarget(const std::string& name);
    void beforeEndTargetHook(Painter* painter);
    bool endTarget(bool endDraw);
    void beginObject(const std::string& name);
    void endObject();

    void setAntialiasing(bool arg);
    void setCompositionMode(CompositionMode mode);
    void setWindow(const RectF& window);
    void setViewport(const RectF& viewport);
    void setFont(const Font& font);
    void setPen(const Pen& pen);
    void setNoPen();
    void setBrush(const Brush& brush);
    void save();
    void restore();
    void setTransform(const Transform& transform);

    void drawPath(const PainterPath& path);
    void drawPolygon(const PointF* points, size_t pointCount, PolygonMode mode);
    void drawText(const PointF& point, const String& text);
    void drawText(const RectF& rect, Alignment alignment, TextFlags textFlags, const String& text);
    void drawSymbol(const PointF& point, char32_t ucs4Code);
    void drawPixmap(const PointF& point, const Pixmap& pm);
    void drawTiledPixmap(const RectF& rect, const Pixmap& pm, const PointF& offset);

    bool hasClipping() const;
    void setClipRect(const RectF& rect);
    void setMask(const RectF& background, const std::vector<RectF>& maskRects);
    void setClipping(bool enable);

private:
    void ensureOk(const char* what) const;
    void validateAffine(const Transform& transform) const;
    void applyTransform();
    void updateViewTransform();
    void setSourceColor(const Color& color);
    double fontSizePixels(const Font& font) const;
    std::shared_ptr<FtFaceEntry> faceFor(const Font& font);

    void buildPath(const PainterPath& path);
    void applyPen();
    void applyBrushSource();

    std::vector<cairo_rectangle_t> copyClipRects();
    cairo_rectangle_t deviceBounds(const RectF& rect) const;
    void resetToBaseClip();
    void applyDeviceClipRects(const std::vector<cairo_rectangle_t>& rects);
    void clearUserClip();

    ShapedLine shapeText(const FtFaceEntry& entry, const Font& font, const std::u16string& text);
    double measureText(const FtFaceEntry& entry, const Font& font, const std::u16string& text);
    std::vector<std::u16string> wrapParagraph(const FtFaceEntry& entry, const Font& font,
                                              const std::u16string& paragraph, double maxWidth, bool anywhere);
    std::vector<ShapedLine> layoutLines(const FtFaceEntry& entry, const Font& font,
                                        const std::u16string& text, const RectF& rect, TextFlags textFlags);
    void applyFontState(const FtFaceEntry& entry, const Font& font, double pixelSize);
    void drawTextDecorations(const FtFaceEntry& entry, const Font& font, double pixelSize,
                             double x, double baseline, double width);
    void drawShapedLine(const FtFaceEntry& entry, const Font& font, const ShapedLine& line,
                        double x, double baseline, bool justify, double targetWidth);
    void drawTextAtPoint(const Font& font, const PointF& point, const String& text);
    void drawTextInRect(const Font& font, const RectF& rect, Alignment alignment, TextFlags textFlags, const String& text);

    cairo_surface_t* pixmapSurface(const Pixmap& pm);
};

// --------------------------------------------------------------- lifecycle

CairoPainter::Impl::Impl(cairo_t* context, const IFontsDatabase& fontsDatabase, double deviceDpi)
    : cr(context), fonts(fontsDatabase), dpi(deviceDpi)
{
    if (!cr) {
        throw std::invalid_argument("CairoPainter: cairo_t context must not be null");
    }
    if (!isFinite(dpi) || dpi <= 0.0) {
        throw std::invalid_argument("CairoPainter: device DPI must be a positive finite number");
    }

    const cairo_status_t status = cairo_status(cr);
    if (status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(std::string("CairoPainter: cairo context is in error state: ")
                                 + cairo_status_to_string(status));
    }

    state.font = Font();
    state.pen = Pen();
    state.brush = Brush(BrushStyle::NoBrush);

    // Adopt a transform the caller applied to the cairo_t before handing it
    // over, mirroring QPainterProvider's initial transform read.
    cairo_matrix_t initialMatrix;
    cairo_get_matrix(cr, &initialMatrix);
    const bool identityMatrix = initialMatrix.xx == 1.0 && initialMatrix.yx == 0.0
                               && initialMatrix.xy == 0.0 && initialMatrix.yy == 1.0
                               && initialMatrix.x0 == 0.0 && initialMatrix.y0 == 0.0;
    if (!identityMatrix) {
        state.transform = Transform(initialMatrix.xx, initialMatrix.yx,
                                    initialMatrix.xy, initialMatrix.yy,
                                    initialMatrix.x0, initialMatrix.y0);
        state.worldTransform = state.transform;
        state.transformExplicit = true;
    }

    try {
        baseClip = copyClipRects();
        baseClipKnown = true;
    } catch (const std::runtime_error&) {
        // Some vector backends (PDF/SVG) do not expose their clip as a
        // rectangle list.  A fresh cairo_t's only clip is the surface bounds,
        // so treating it as "no extra base clip" is the safe interpretation.
        baseClip.clear();
        baseClipKnown = false;
    }

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    ensureOk("initialize");
}

CairoPainter::Impl::~Impl()
{
    for (auto& item : surfaces) {
        if (item.second.surface) {
            cairo_surface_destroy(item.second.surface);
            item.second.surface = nullptr;
        }
    }
}

void CairoPainter::Impl::ensureOk(const char* what) const
{
    const cairo_status_t status = cairo_status(cr);
    if (status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(std::string("CairoPainter: ") + what + ": " + cairo_status_to_string(status));
    }
}

bool CairoPainter::Impl::isActive() const
{
    return active;
}

void CairoPainter::Impl::beginTarget(const std::string&)
{
}

void CairoPainter::Impl::beforeEndTargetHook(Painter*)
{
}

bool CairoPainter::Impl::endTarget(bool endDraw)
{
    if (!active) {
        return true;
    }
    if (endDraw) {
        active = false;
    }
    return cairo_status(cr) == CAIRO_STATUS_SUCCESS;
}

void CairoPainter::Impl::beginObject(const std::string&)
{
}

void CairoPainter::Impl::endObject()
{
}

// --------------------------------------------------------------- transform

void CairoPainter::Impl::validateAffine(const Transform& transform) const
{
    if (!finiteTransform(transform)) {
        throw std::runtime_error("CairoPainter: non-finite transform");
    }
    if (std::abs(transform.m13()) > 1e-12
        || std::abs(transform.m23()) > 1e-12
        || std::abs(transform.m33() - 1.0) > 1e-12) {
        throw std::runtime_error("CairoPainter: projective transforms are not supported by Cairo");
    }
}

void CairoPainter::Impl::applyTransform()
{
    validateAffine(state.transform);

    cairo_matrix_t matrix;
    matrix.xx = state.transform.m11();
    matrix.yx = state.transform.m12();
    matrix.xy = state.transform.m21();
    matrix.yy = state.transform.m22();
    matrix.x0 = state.transform.dx();
    matrix.y0 = state.transform.dy();

    cairo_set_matrix(cr, &matrix);
    ensureOk("setTransform");
}

void CairoPainter::Impl::updateViewTransform()
{
    const bool valid = finiteRect(state.window) && finiteRect(state.viewport)
                       && state.window.width() > 0.0 && state.window.height() > 0.0
                       && state.viewport.width() > 0.0 && state.viewport.height() > 0.0;

    if (!valid) {
        state.hasView = false;
        if (!state.transformExplicit) {
            state.transform = state.worldTransform;
            applyTransform();
        }
        return;
    }

    const double scaleX = state.viewport.width() / state.window.width();
    const double scaleY = state.viewport.height() / state.window.height();
    if (!isFinite(scaleX) || !isFinite(scaleY)) {
        state.hasView = false;
        if (!state.transformExplicit) {
            state.transform = state.worldTransform;
            applyTransform();
        }
        return;
    }

    const Transform viewTransform(scaleX, 0.0, 0.0, scaleY,
                                  state.viewport.x() - state.window.x() * scaleX,
                                  state.viewport.y() - state.window.y() * scaleY);
    if (!finiteTransform(viewTransform)) {
        state.hasView = false;
        if (!state.transformExplicit) {
            state.transform = state.worldTransform;
            applyTransform();
        }
        return;
    }

    state.viewTransform = viewTransform;
    state.hasView = true;

    if (!state.transformExplicit) {
        const Transform effective = state.worldTransform * viewTransform;
        if (!finiteTransform(effective)) {
            throw std::runtime_error("CairoPainter: window/viewport composition produced non-finite values");
        }
        state.transform = effective;
        applyTransform();
    }
}

void CairoPainter::Impl::setTransform(const Transform& transform)
{
    if (!active) {
        return;
    }

    // muse::draw::Painter temporarily feeds a viewport transform computed with
    // a zero-sized window; that transform is singular/NaN.  Ignore it rather
    // than poisoning the cairo context.  The real transform follows immediately.
    if (!finiteTransform(transform)) {
        return;
    }
    validateAffine(transform);

    if (!state.transformExplicit && state.hasView) {
        // Two legitimate callers exist:
        //  * muse::draw::Painter passes world * viewport, and has already
        //    written its own window/viewport into our state as well;
        //  * a direct caller passes only the world transform after setting
        //    window/viewport.
        // If the incoming matrix already equals the composition we would
        // produce, accept it as complete.  Otherwise treat it as the world
        // transform and compose it with the current window/viewport.
        const Transform composed = state.worldTransform * state.viewTransform;
        if (transformNearlyEqual(transform, composed)
            || transformNearlyEqual(transform, state.transform)) {
            state.worldTransform = transform;
            state.transform = transform;
            state.transformExplicit = true;
            applyTransform();
            return;
        }

        const Transform effective = transform * state.viewTransform;
        if (!finiteTransform(effective)) {
            throw std::runtime_error("CairoPainter: transform composition produced non-finite values");
        }
        state.worldTransform = transform;
        state.transform = effective;
        applyTransform();
        return;
    }

    // Once an explicit transform has been accepted as the complete matrix, it
    // stays authoritative.  This matches QPainterProvider and prevents double
    // composition when Painter subsequently calls setWindow()/setViewport().
    state.worldTransform = transform;
    state.transform = transform;
    state.transformExplicit = true;
    applyTransform();
}

void CairoPainter::Impl::setWindow(const RectF& window)
{
    if (!active) {
        return;
    }
    state.window = window;
    updateViewTransform();
}

void CairoPainter::Impl::setViewport(const RectF& viewport)
{
    if (!active) {
        return;
    }
    state.viewport = viewport;
    updateViewTransform();
}

// --------------------------------------------------------- paint state

void CairoPainter::Impl::setAntialiasing(bool arg)
{
    if (!active) {
        return;
    }
    state.antialiasing = arg;
    cairo_set_antialias(cr, arg ? CAIRO_ANTIALIAS_GRAY : CAIRO_ANTIALIAS_NONE);
    ensureOk("setAntialiasing");
}

void CairoPainter::Impl::setCompositionMode(CompositionMode mode)
{
    if (!active) {
        return;
    }

    cairo_operator_t op;
    switch (mode) {
    case CompositionMode::SourceOver:
        op = CAIRO_OPERATOR_OVER;
        break;
    case CompositionMode::HardLight:
        op = CAIRO_OPERATOR_HARD_LIGHT;
        break;
    default:
        throw std::runtime_error("CairoPainter: unsupported composition mode "
                                 + std::to_string(static_cast<int>(mode)));
    }

    state.composition = mode;
    cairo_set_operator(cr, op);
    ensureOk("setCompositionMode");
}

void CairoPainter::Impl::setFont(const Font& font)
{
    if (!active) {
        return;
    }
    state.font = font;
}

void CairoPainter::Impl::setPen(const Pen& pen)
{
    if (!active) {
        return;
    }
    state.pen = pen;
}

void CairoPainter::Impl::setNoPen()
{
    if (!active) {
        return;
    }
    state.pen = Pen(PenStyle::NoPen);
}

void CairoPainter::Impl::setBrush(const Brush& brush)
{
    if (!active) {
        return;
    }
    state.brush = brush;
}

void CairoPainter::Impl::save()
{
    if (!active) {
        return;
    }
    stack.push_back(state);
    cairo_save(cr);
    ensureOk("save");
}

void CairoPainter::Impl::restore()
{
    if (!active || stack.empty()) {
        return;
    }
    cairo_restore(cr);
    ensureOk("restore");
    state = std::move(stack.back());
    stack.pop_back();
}

void CairoPainter::Impl::setSourceColor(const Color& color)
{
    const Color c = color.isValid() ? color : Color::BLACK;
    const double alpha = std::clamp(c.alpha(), 0, 255) / 255.0;
    const double red = std::clamp(c.red(), 0, 255) / 255.0;
    const double green = std::clamp(c.green(), 0, 255) / 255.0;
    const double blue = std::clamp(c.blue(), 0, 255) / 255.0;
    cairo_set_source_rgba(cr, red, green, blue, alpha);
}

double CairoPainter::Impl::fontSizePixels(const Font& font) const
{
    if (font.pixelSize() > 0) {
        return double(font.pixelSize());
    }
    if (font.pointSizeF() > 0.0) {
        const double pixelSize = font.pointSizeF() * dpi / 72.0;
        if (isFinite(pixelSize) && pixelSize > 0.0) {
            return pixelSize;
        }
    }
    throw std::runtime_error("CairoPainter: font has neither a point size nor a pixel size");
}

std::shared_ptr<FtFaceEntry> CairoPainter::Impl::faceFor(const Font& font)
{
    return fontCache().get(font, fonts);
}

void CairoPainter::Impl::applyPen()
{
    if (state.pen.style() == PenStyle::NoPen) {
        return;
    }

    setSourceColor(state.pen.color());

    double width = state.pen.widthF();
    if (!isFinite(width) || width < 0.0) {
        width = 0.0;
    }
    // Cairo interprets a zero line width as a one-device-pixel hairline, which
    // matches Qt's cosmetic pen behaviour.
    cairo_set_line_width(cr, width);

    switch (state.pen.capStyle()) {
    case PenCapStyle::FlatCap:
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        break;
    case PenCapStyle::SquareCap:
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
        break;
    case PenCapStyle::RoundCap:
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        break;
    default:
        throw std::runtime_error("CairoPainter: unsupported pen cap style");
    }

    switch (state.pen.joinStyle()) {
    case PenJoinStyle::MiterJoin:
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
        break;
    case PenJoinStyle::BevelJoin:
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);
        break;
    case PenJoinStyle::RoundJoin:
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        break;
    default:
        throw std::runtime_error("CairoPainter: unsupported pen join style");
    }
    // QPen's default miter limit is 2.0; Cairo's default is 10.0.
    cairo_set_miter_limit(cr, 2.0);

    switch (state.pen.style()) {
    case PenStyle::NoPen:
        break;
    case PenStyle::SolidLine:
        cairo_set_dash(cr, nullptr, 0, 0.0);
        break;
    case PenStyle::DashLine:
    case PenStyle::DotLine:
    case PenStyle::DashDotLine:
    case PenStyle::DashDotDotLine:
    case PenStyle::CustomDashLine: {
        const std::vector<double> pattern = state.pen.dashPattern();
        if (pattern.empty()) {
            cairo_set_dash(cr, nullptr, 0, 0.0);
            break;
        }

        // QPen dash patterns are expressed in multiples of the pen width.
        // Cairo expects user-space lengths.  A zero width is a cosmetic pen:
        // its pattern is effectively in device pixels, so convert through the
        // current CTM scale.
        double unit = width;
        if (!(unit > 0.0)) {
            cairo_matrix_t ctm;
            cairo_get_matrix(cr, &ctm);
            const double scaleX = std::hypot(ctm.xx, ctm.yx);
            const double scaleY = std::hypot(ctm.xy, ctm.yy);
            const double scale = (scaleX + scaleY) / 2.0;
            unit = (isFinite(scale) && scale > 0.0) ? 1.0 / scale : 1.0;
        }
        std::vector<double> dashes;
        dashes.reserve(pattern.size());
        bool allZero = true;
        for (double value : pattern) {
            double dash = value * unit;
            if (!isFinite(dash) || dash < 0.0) {
                dash = 0.0;
            }
            if (dash > 0.0) {
                allZero = false;
            }
            dashes.push_back(dash);
        }

        if (allZero || dashes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            cairo_set_dash(cr, nullptr, 0, 0.0);
        } else {
            cairo_set_dash(cr, dashes.data(), static_cast<int>(dashes.size()), 0.0);
        }
    } break;
    default:
        throw std::runtime_error("CairoPainter: unsupported pen style "
                                 + std::to_string(static_cast<int>(state.pen.style())));
    }

    ensureOk("pen");
}

void CairoPainter::Impl::applyBrushSource()
{
    switch (state.brush.style()) {
    case BrushStyle::NoBrush:
        return;
    case BrushStyle::SolidPattern:
        setSourceColor(state.brush.color());
        return;
    default:
        throw std::runtime_error("CairoPainter: unsupported brush style "
                                 + std::to_string(static_cast<int>(state.brush.style())));
    }
}

// --------------------------------------------------------------- clipping

std::vector<cairo_rectangle_t> CairoPainter::Impl::copyClipRects()
{
    std::unique_ptr<cairo_rectangle_list_t, decltype(&cairo_rectangle_list_destroy)> list(cairo_copy_clip_rectangle_list(cr), cairo_rectangle_list_destroy);
    if (!list) {
        throw std::runtime_error("CairoPainter: cairo_copy_clip_rectangle_list failed");
    }

    if (list->status != CAIRO_STATUS_SUCCESS) {
        const std::string message = cairo_status_to_string(list->status);

        throw std::runtime_error("CairoPainter: clip region is not representable as rectangles: " + message);
    }
    if (list->num_rectangles > 0 && !list->rectangles) {

        throw std::runtime_error("CairoPainter: clip rectangle list is missing its rectangles");
    }

    // cairo_copy_clip_rectangle_list() reports rectangles in the *current user
    // space*.  We cache them for re-application under an identity matrix, so
    // convert them to device space with the CTM that was active when the clip
    // was created.
    cairo_matrix_t ctm;
    cairo_get_matrix(cr, &ctm);

    std::vector<cairo_rectangle_t> rects;
    rects.reserve(list->num_rectangles > 0 ? static_cast<size_t>(list->num_rectangles) : 0u);
    for (int i = 0; i < list->num_rectangles; ++i) {
        const cairo_rectangle_t& userRect = list->rectangles[i];
        double minX = std::numeric_limits<double>::max();
        double minY = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double maxY = std::numeric_limits<double>::lowest();

        const double xs[2] = { userRect.x, userRect.x + userRect.width };
        const double ys[2] = { userRect.y, userRect.y + userRect.height };
        for (double x : xs) {
            for (double y : ys) {
                double px = x;
                double py = y;
                cairo_matrix_transform_point(&ctm, &px, &py);
                if (!isFinite(px) || !isFinite(py)) {

                    throw std::runtime_error("CairoPainter: clip rectangle maps to non-finite device coordinates");
                }
                minX = std::min(minX, px);
                minY = std::min(minY, py);
                maxX = std::max(maxX, px);
                maxY = std::max(maxY, py);
            }
        }

        cairo_rectangle_t deviceRect;
        deviceRect.x = minX;
        deviceRect.y = minY;
        deviceRect.width = maxX - minX;
        deviceRect.height = maxY - minY;
        rects.push_back(deviceRect);
    }

    return rects;
}

cairo_rectangle_t CairoPainter::Impl::deviceBounds(const RectF& rect) const
{
    cairo_matrix_t ctm;
    cairo_get_matrix(cr, &ctm);

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();

    const double xs[2] = { rect.left(), rect.right() };
    const double ys[2] = { rect.top(), rect.bottom() };
    for (double x : xs) {
        for (double y : ys) {
            double px = x;
            double py = y;
            cairo_matrix_transform_point(&ctm, &px, &py);
            if (!isFinite(px) || !isFinite(py)) {
                throw std::runtime_error("CairoPainter: clip rectangle maps to non-finite device coordinates");
            }
            minX = std::min(minX, px);
            minY = std::min(minY, py);
            maxX = std::max(maxX, px);
            maxY = std::max(maxY, py);
        }
    }

    cairo_rectangle_t out;
    out.x = minX;
    out.y = minY;
    out.width = maxX - minX;
    out.height = maxY - minY;
    return out;
}

void CairoPainter::Impl::resetToBaseClip()
{
    cairo_matrix_t saved;
    cairo_get_matrix(cr, &saved);

    cairo_matrix_t identity;
    cairo_matrix_init_identity(&identity);
    cairo_set_matrix(cr, &identity);
    cairo_reset_clip(cr);
    cairo_new_path(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);

    if (baseClipKnown) {
        if (baseClip.empty()) {
            // A degenerate rectangle produces an empty clip region: nothing draws.
            cairo_rectangle(cr, 0.0, 0.0, 0.0, 0.0);
        } else {
            for (const cairo_rectangle_t& rect : baseClip) {
                cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
            }
        }
        cairo_clip(cr);
    }
    // If the backend did not expose its clip, leave the default surface clip.

    cairo_set_matrix(cr, &saved);
    ensureOk("reset clip");
}

void CairoPainter::Impl::applyDeviceClipRects(const std::vector<cairo_rectangle_t>& rects)
{
    cairo_matrix_t saved;
    cairo_get_matrix(cr, &saved);

    cairo_identity_matrix(cr);
    cairo_new_path(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    if (rects.empty()) {
        cairo_rectangle(cr, 0.0, 0.0, 0.0, 0.0);
    } else {
        for (const cairo_rectangle_t& rect : rects) {
            cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
        }
    }
    cairo_clip(cr);
    cairo_set_matrix(cr, &saved);
    ensureOk("apply clip");
}

void CairoPainter::Impl::clearUserClip()
{
    resetToBaseClip();
    state.hasUserClip = false;
    state.userClipRects.clear();
}

void CairoPainter::Impl::setClipRect(const RectF& rect)
{
    if (!active) {
        return;
    }

    const RectF r = rect.normalized();
    if (!finiteRect(r) || !r.isValid()) {
        clearUserClip();
        return;
    }

    // Compute the cached device-space bounds before mutating the clip so a
    // failure leaves the context unchanged.
    const cairo_rectangle_t deviceRect = deviceBounds(r);
    const bool clippingEnabled = state.clippingEnabled;

    resetToBaseClip();
    cairo_new_path(cr);
    cairo_rectangle(cr, r.x(), r.y(), r.width(), r.height());
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_clip(cr);

    if (!clippingEnabled) {
        resetToBaseClip();
    }
    ensureOk("setClipRect");

    // The clip currently applied by Cairo is the exact transformed path; this
    // cached form is exact for axis-aligned transforms and a bounding box
    // otherwise, and is used when setClipping() is re-enabled.
    state.userClipRects.clear();
    state.userClipRects.push_back(deviceRect);
    state.hasUserClip = true;
}

void CairoPainter::Impl::setMask(const RectF& background, const std::vector<RectF>& maskRects)
{
    if (!active) {
        return;
    }

    if (maskRects.empty()) {
        clearUserClip();
        return;
    }

    const RectF bg = background.normalized();
    if (!finiteRect(bg) || !bg.isValid()) {
        clearUserClip();
        return;
    }

    const cairo_rectangle_t backgroundDevice = deviceBounds(bg);
    std::vector<cairo_rectangle_t> maskDevices;
    maskDevices.reserve(maskRects.size());
    for (const RectF& rawRect : maskRects) {
        const RectF rect = rawRect.normalized();
        if (!finiteRect(rect) || !rect.isValid()) {
            continue;
        }
        const cairo_rectangle_t maskDevice = deviceBounds(rect);
        if (maskDevice.width > 0.0 && maskDevice.height > 0.0) {
            maskDevices.push_back(maskDevice);
        }
    }

    std::vector<cairo_rectangle_t> clipRects = subtractRects(backgroundDevice, maskDevices);
    const bool clippingEnabled = state.clippingEnabled;

    resetToBaseClip();
    applyDeviceClipRects(clipRects);
    if (!clippingEnabled) {
        resetToBaseClip();
    }
    ensureOk("setMask");

    state.userClipRects = std::move(clipRects);
    state.hasUserClip = true;
}

void CairoPainter::Impl::setClipping(bool enable)
{
    if (!active) {
        return;
    }
    if (state.clippingEnabled == enable) {
        return;
    }

    if (enable && state.hasUserClip) {
        resetToBaseClip();
        applyDeviceClipRects(state.userClipRects);
    } else {
        resetToBaseClip();
    }
    ensureOk("setClipping");
    state.clippingEnabled = enable;
}

bool CairoPainter::Impl::hasClipping() const
{
    return state.clippingEnabled && state.hasUserClip;
}

// ------------------------------------------------------------------- paths

void CairoPainter::Impl::buildPath(const PainterPath& path)
{
    bool haveCurrent = false;
    bool haveCtrl1 = false;
    bool haveCtrl2 = false;
    PointF ctrl1;
    PointF ctrl2;

    for (size_t i = 0; i < path.elementCount(); ++i) {
        const PainterPath::Element e = path.elementAt(i);
        switch (e.type) {
        case PainterPath::ElementType::MoveToElement:
            cairo_move_to(cr, e.x, e.y);
            haveCurrent = true;
            haveCtrl1 = false;
            haveCtrl2 = false;
            break;
        case PainterPath::ElementType::LineToElement:
            if (!haveCurrent) {
                cairo_move_to(cr, e.x, e.y);
                haveCurrent = true;
            } else {
                cairo_line_to(cr, e.x, e.y);
            }
            haveCtrl1 = false;
            haveCtrl2 = false;
            break;
        case PainterPath::ElementType::CurveToElement:
            ctrl1 = PointF(e.x, e.y);
            haveCtrl1 = true;
            haveCtrl2 = false;
            break;
        case PainterPath::ElementType::CurveToDataElement:
            if (!haveCtrl1) {
                if (!haveCurrent) {
                    cairo_move_to(cr, e.x, e.y);
                    haveCurrent = true;
                } else {
                    cairo_line_to(cr, e.x, e.y);
                }
            } else if (!haveCtrl2) {
                ctrl2 = PointF(e.x, e.y);
                haveCtrl2 = true;
            } else {
                if (!haveCurrent) {
                    cairo_move_to(cr, ctrl1.x(), ctrl1.y());
                    haveCurrent = true;
                }
                cairo_curve_to(cr, ctrl1.x(), ctrl1.y(), ctrl2.x(), ctrl2.y(), e.x, e.y);
                haveCtrl1 = false;
                haveCtrl2 = false;
            }
            break;
        }
    }
}

void CairoPainter::Impl::drawPath(const PainterPath& path)
{
    if (!active) {
        return;
    }
    ensureOk("drawPath");
    if (path.isEmpty()) {
        return;
    }

    const bool hasBrush = state.brush.style() != BrushStyle::NoBrush;
    const bool hasPen = state.pen.style() != PenStyle::NoPen;
    if (!hasBrush && !hasPen) {
        return;
    }

    cairo_new_path(cr);
    buildPath(path);

    if (hasBrush) {
        cairo_set_fill_rule(cr, path.fillRule() == PainterPath::FillRule::WindingFill
                            ? CAIRO_FILL_RULE_WINDING : CAIRO_FILL_RULE_EVEN_ODD);
        applyBrushSource();
        if (hasPen) {
            cairo_fill_preserve(cr);
        } else {
            cairo_fill(cr);
        }
    }

    if (hasPen) {
        applyPen();
        cairo_stroke(cr);
    }
    ensureOk("drawPath");
}

void CairoPainter::Impl::drawPolygon(const PointF* points, size_t pointCount, PolygonMode mode)
{
    if (!active) {
        return;
    }
    ensureOk("drawPolygon");
    if (!points || pointCount == 0) {
        return;
    }

    bool fill = true;
    cairo_fill_rule_t fillRule = CAIRO_FILL_RULE_EVEN_ODD;
    switch (mode) {
    case PolygonMode::OddEven:
        fill = true;
        fillRule = CAIRO_FILL_RULE_EVEN_ODD;
        break;
    case PolygonMode::Winding:
        fill = true;
        fillRule = CAIRO_FILL_RULE_WINDING;
        break;
    case PolygonMode::Convex:
        fill = true;
        fillRule = CAIRO_FILL_RULE_WINDING;
        break;
    case PolygonMode::Polyline:
        fill = false;
        break;
    default:
        throw std::runtime_error("CairoPainter: unsupported polygon mode "
                                 + std::to_string(static_cast<int>(mode)));
    }

    const bool hasBrush = fill && state.brush.style() != BrushStyle::NoBrush;
    const bool hasPen = state.pen.style() != PenStyle::NoPen;
    if (!hasBrush && !hasPen) {
        return;
    }

    cairo_new_path(cr);
    cairo_move_to(cr, points[0].x(), points[0].y());
    for (size_t i = 1; i < pointCount; ++i) {
        cairo_line_to(cr, points[i].x(), points[i].y());
    }
    if (fill) {
        cairo_close_path(cr);
    }

    if (hasBrush) {
        cairo_set_fill_rule(cr, fillRule);
        applyBrushSource();
        if (hasPen) {
            cairo_fill_preserve(cr);
        } else {
            cairo_fill(cr);
        }
    }
    if (hasPen) {
        applyPen();
        cairo_stroke(cr);
    }
    ensureOk("drawPolygon");
}

// -------------------------------------------------------------------- text

ShapedLine CairoPainter::Impl::shapeText(const FtFaceEntry& entry, const Font& font, const std::u16string& text)
{
    ShapedLine line;
    line.text = text;
    if (text.empty()) {
        return line;
    }
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("CairoPainter: text is too long");
    }

    const double pixelSize = fontSizePixels(font);
    const double scale = pixelSize / double(entry.hbUpem ? entry.hbUpem : entry.ftUpem);

    hb_font_t* hbFont = hb_font_create(entry.hb);
    if (!hbFont) {
        throw std::runtime_error("CairoPainter: hb_font_create failed");
    }
    hb_font_set_scale(hbFont, static_cast<int>(entry.hbUpem), static_cast<int>(entry.hbUpem));

    hb_buffer_t* buffer = hb_buffer_create();
    if (!buffer) {
        hb_font_destroy(hbFont);
        throw std::runtime_error("CairoPainter: hb_buffer_create failed");
    }

    hb_buffer_add_utf16(buffer, reinterpret_cast<const uint16_t*>(text.data()),
                        static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(hbFont, buffer, nullptr, 0);

    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, nullptr);
    if (count > 0 && (!infos || !positions)) {
        hb_buffer_destroy(buffer);
        hb_font_destroy(hbFont);
        throw std::runtime_error("CairoPainter: HarfBuzz returned an incomplete glyph buffer");
    }

    line.glyphs.reserve(count);
    line.clusters.reserve(count);

    double penX = 0.0;
    double penY = 0.0;
    for (unsigned int i = 0; i < count; ++i) {
        const double xOffset = double(positions[i].x_offset) * scale;
        const double yOffset = double(positions[i].y_offset) * scale;

        cairo_glyph_t glyph;
        glyph.index = static_cast<unsigned long>(infos[i].codepoint);
        glyph.x = penX + xOffset;
        glyph.y = penY - yOffset;
        line.glyphs.push_back(glyph);
        line.clusters.push_back(static_cast<unsigned int>(infos[i].cluster));

        penX += double(positions[i].x_advance) * scale;
        // HarfBuzz y advances point up; Cairo user space points down.
        penY -= double(positions[i].y_advance) * scale;
    }

    if (penX < 0.0) {
        // RTL shaping can produce a negative pen position.  Keep the line
        // origin at zero and draw the visual run to the right of it.
        for (cairo_glyph_t& glyph : line.glyphs) {
            glyph.x -= penX;
        }
        line.width = -penX;
    } else {
        line.width = penX;
    }

    hb_buffer_destroy(buffer);
    hb_font_destroy(hbFont);
    return line;
}

double CairoPainter::Impl::measureText(const FtFaceEntry& entry, const Font& font, const std::u16string& text)
{
    return shapeText(entry, font, text).width;
}

std::vector<std::u16string> CairoPainter::Impl::wrapParagraph(const FtFaceEntry& entry, const Font& font,
                                                             const std::u16string& paragraph, double maxWidth,
                                                             bool anywhere)
{
    std::vector<std::u16string> lines;
    if (paragraph.empty()) {
        lines.push_back(paragraph);
        return lines;
    }

    size_t start = 0;
    while (start < paragraph.size()) {
        while (start < paragraph.size() && isSpaceChar(paragraph[start])) {
            ++start;
        }
        if (start >= paragraph.size()) {
            break;
        }

        size_t end = start;
        size_t lastSpace = std::u16string::npos;
        while (end < paragraph.size()) {
            if (isSpaceChar(paragraph[end])) {
                lastSpace = end;
            }
            ++end;

            const double width = measureText(entry, font, paragraph.substr(start, end - start));
            if (width > maxWidth) {
                if (end > start + 1) {
                    --end;
                }
                break;
            }
        }

        if (end >= paragraph.size()) {
            lines.push_back(paragraph.substr(start));
            break;
        }

        size_t breakAt = end;
        if (!anywhere && lastSpace != std::u16string::npos && lastSpace > start) {
            breakAt = lastSpace;
        }
        if (breakAt <= start) {
            breakAt = start + 1;
        }

        lines.push_back(paragraph.substr(start, breakAt - start));
        start = breakAt;
        while (start < paragraph.size() && isSpaceChar(paragraph[start])) {
            ++start;
        }
    }

    if (lines.empty()) {
        lines.push_back(std::u16string());
    }
    return lines;
}

std::vector<ShapedLine> CairoPainter::Impl::layoutLines(const FtFaceEntry& entry, const Font& font,
                                                        const std::u16string& text, const RectF& rect, TextFlags textFlags)
{
    bool wrap = textFlags.testFlag(TextWordWrap) || textFlags.testFlag(TextWrapAnywhere);
    const bool anywhere = textFlags.testFlag(TextWrapAnywhere);
    const double maxWidth = rect.width();

    std::vector<std::u16string> paragraphs = splitParagraphs(text);
    if (textFlags.testFlag(TextSingleLine)) {
        paragraphs = { joinWithSpaces(paragraphs) };
        wrap = false;
    }

    std::vector<ShapedLine> lines;
    for (const std::u16string& paragraph : paragraphs) {
        if (wrap && maxWidth > 0.0) {
            const std::vector<std::u16string> wrapped = wrapParagraph(entry, font, paragraph, maxWidth, anywhere);
            for (const std::u16string& line : wrapped) {
                lines.push_back(shapeText(entry, font, line));
            }
        } else {
            lines.push_back(shapeText(entry, font, paragraph));
        }
    }
    return lines;
}

void CairoPainter::Impl::applyFontState(const FtFaceEntry& entry, const Font& font, double pixelSize)
{
    const bool faceBold = (entry.ft->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    const bool faceItalic = (entry.ft->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    const bool synthBold = font.bold() && !entry.key.bold() && !faceBold;
    const bool synthItalic = font.italic() && !entry.key.italic() && !faceItalic;

    cairo_set_font_face(cr, entry.cairoFace(synthBold, synthItalic));
    cairo_set_font_size(cr, pixelSize);

    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, state.antialiasing ? CAIRO_ANTIALIAS_GRAY : CAIRO_ANTIALIAS_NONE);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);

    ensureOk("font");
}

void CairoPainter::Impl::drawTextDecorations(const FtFaceEntry& entry, const Font& font, double pixelSize,
                                             double x, double baseline, double width)
{
    if (!font.underline() && !font.strike()) {
        return;
    }
    if (!(width > 0.0)) {
        return;
    }

    const double scale = pixelSize / double(entry.ftUpem);
    const double ascent = ascentFor(entry, pixelSize);
    double thickness = double(entry.underlineThickness) * scale;
    if (!(thickness > 0.0)) {
        thickness = std::max(1.0, pixelSize * 0.05);
    }

    CairoStateGuard guard(cr);
    setSourceColor(state.pen.color());
    cairo_set_dash(cr, nullptr, 0, 0.0);
    cairo_set_line_width(cr, thickness);
    cairo_new_path(cr);

    if (font.underline()) {
        const double y = baseline - double(entry.underlinePosition) * scale;
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x + width, y);
        cairo_stroke(cr);
    }

    if (font.strike()) {
        const double y = baseline - 0.3 * ascent;
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x + width, y);
        cairo_stroke(cr);
    }

    ensureOk("text decoration");
}

void CairoPainter::Impl::drawShapedLine(const FtFaceEntry& entry, const Font& font, const ShapedLine& line,
                                        double x, double baseline, bool justify, double targetWidth)
{
    if (line.glyphs.empty()) {
        return;
    }
    if (line.glyphs.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("CairoPainter: glyph run is too long");
    }

    const double pixelSize = fontSizePixels(font);
    applyFontState(entry, font, pixelSize);
    setSourceColor(state.pen.color());

    std::vector<char> isSpace(line.glyphs.size(), 0);
    size_t spaceCount = 0;
    for (size_t i = 0; i < line.glyphs.size(); ++i) {
        const size_t cluster = line.clusters[i];
        if (cluster < line.text.size() && isSpaceChar(line.text[cluster])) {
            isSpace[i] = 1;
            ++spaceCount;
        }
    }

    double extra = 0.0;
    if (justify && targetWidth > line.width && spaceCount > 0) {
        extra = (targetWidth - line.width) / double(spaceCount);
    }

    std::vector<cairo_glyph_t> glyphs = line.glyphs;
    double shift = 0.0;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        glyphs[i].x += x + shift;
        glyphs[i].y += baseline;
        if (isSpace[i]) {
            shift += extra;
        }
    }

    cairo_show_glyphs(cr, glyphs.data(), static_cast<int>(glyphs.size()));
    ensureOk("drawText");

    const double drawnWidth = line.width + extra * double(spaceCount);
    drawTextDecorations(entry, font, pixelSize, x, baseline, drawnWidth);
}

void CairoPainter::Impl::drawTextAtPoint(const Font& font, const PointF& point, const String& text)
{
    const std::shared_ptr<FtFaceEntry> entry = faceFor(font);
    const double pixelSize = fontSizePixels(font);
    const double lineHeight = lineHeightFor(*entry, pixelSize);

    const std::vector<std::u16string> paragraphs
        = splitParagraphs(normalizeText(text.toStdU16String(), true));

    double baseline = point.y();
    for (const std::u16string& paragraph : paragraphs) {
        const ShapedLine line = shapeText(*entry, font, paragraph);
        drawShapedLine(*entry, font, line, point.x(), baseline, false, 0.0);
        baseline += lineHeight;
    }
}

void CairoPainter::Impl::drawTextInRect(const Font& font, const RectF& rect, Alignment alignment, TextFlags textFlags, const String& text)
{
    const std::shared_ptr<FtFaceEntry> entry = faceFor(font);
    const double pixelSize = fontSizePixels(font);
    const std::u16string prepared = normalizeText(text.toStdU16String(), true);
    std::vector<ShapedLine> lines = layoutLines(*entry, font, prepared, rect, textFlags);
    if (lines.empty()) {
        return;
    }

    const double ascent = ascentFor(*entry, pixelSize);
    const double descent = descentFor(*entry, pixelSize);
    const double lineHeight = lineHeightFor(*entry, pixelSize);
    const int lineCount = static_cast<int>(lines.size());

    double firstBaseline;
    if (alignment.testFlag(AlignTop)) {
        firstBaseline = rect.y() + ascent;
    } else if (alignment.testFlag(AlignBottom)) {
        firstBaseline = rect.bottom() - descent - double(lineCount - 1) * lineHeight;
    } else {
        const double blockHeight = double(lineCount - 1) * lineHeight + ascent + descent;
        firstBaseline = rect.y() + (rect.height() - blockHeight) / 2.0 + ascent;
    }

    std::unique_ptr<CairoStateGuard> clipGuard;
    if (!textFlags.testFlag(TextDontClip) && rect.isValid()) {
        clipGuard = std::make_unique<CairoStateGuard>(cr);
        cairo_new_path(cr);
        cairo_rectangle(cr, rect.x(), rect.y(), rect.width(), rect.height());
        cairo_clip(cr);
    }

    for (int i = 0; i < lineCount; ++i) {
        const ShapedLine& line = lines[static_cast<size_t>(i)];

        double x = rect.x();
        if (alignment.testFlag(AlignRight)) {
            x = rect.right() - line.width;
        } else if (alignment.testFlag(AlignHCenter)) {
            x = rect.x() + (rect.width() - line.width) / 2.0;
        }

        const bool justify = alignment.testFlag(AlignJustify)
                             && (i + 1) < lineCount
                             && (textFlags.testFlag(TextWordWrap) || textFlags.testFlag(TextWrapAnywhere));

        drawShapedLine(*entry, font, line, x, firstBaseline + double(i) * lineHeight, justify, rect.width());
    }
}

void CairoPainter::Impl::drawText(const PointF& point, const String& text)
{
    if (!active) {
        return;
    }
    ensureOk("drawText");
    if (text.empty()) {
        return;
    }
    if (!finitePoint(point)) {
        throw std::invalid_argument("CairoPainter: drawText point is not finite");
    }

    drawTextAtPoint(state.font, point, text);
    ensureOk("drawText");
}

void CairoPainter::Impl::drawText(const RectF& rect, Alignment alignment, TextFlags textFlags, const String& text)
{
    if (!active) {
        return;
    }
    ensureOk("drawText");
    if (text.empty()) {
        return;
    }

    const RectF r = rect.normalized();
    if (!finiteRect(r)) {
        throw std::invalid_argument("CairoPainter: drawText rectangle is not finite");
    }
    if (!r.isValid()) {
        return;
    }

    drawTextInRect(state.font, r, alignment, textFlags, text);
    ensureOk("drawText");
}

void CairoPainter::Impl::drawSymbol(const PointF& point, char32_t ucs4Code)
{
    if (!active) {
        return;
    }
    drawText(point, String::fromUcs4(ucs4Code));
}

// ----------------------------------------------------------------- pixmaps

cairo_surface_t* CairoPainter::Impl::pixmapSurface(const Pixmap& pm)
{
    if (pm.isNull()) {
        throw std::runtime_error("CairoPainter: cannot draw an empty Pixmap");
    }

    const ByteArray data = pm.data();
    const uint64_t hash = fnv1a(data.constData(), data.size());
    const unsigned int key = pm.key();

    auto existing = surfaces.find(key);
    if (existing != surfaces.end()) {
        if (existing->second.size == data.size() && existing->second.hash == hash) {
            return existing->second.surface;
        }
        if (existing->second.surface) {
            cairo_surface_destroy(existing->second.surface);
        }
        surfaces.erase(existing);
    }

#if CAIRO_HAS_PNG_FUNCTIONS
    PngReadClosure closure;
    closure.data = data.constData();
    closure.size = data.size();
    closure.pos = 0;

    cairo_surface_t* surface = cairo_image_surface_create_from_png_stream(&readPngStream, &closure);
    if (!surface) {
        throw std::runtime_error("CairoPainter: cannot allocate a Cairo image surface for Pixmap");
    }
    const cairo_status_t status = cairo_surface_status(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        const std::string message = cairo_status_to_string(status);
        cairo_surface_destroy(surface);
        throw std::runtime_error("CairoPainter: cannot decode PNG Pixmap: " + message);
    }

    if (cairo_image_surface_get_width(surface) <= 0 || cairo_image_surface_get_height(surface) <= 0) {
        cairo_surface_destroy(surface);
        throw std::runtime_error("CairoPainter: decoded PNG has an empty image surface");
    }

    CachedSurface cached;
    cached.size = data.size();
    cached.hash = hash;
    cached.surface = surface;
    surfaces[key] = cached;
    return surface;
#else
    throw std::runtime_error("CairoPainter: this Cairo build has no PNG decoder; Pixmap drawing is unsupported");
#endif
}

void CairoPainter::Impl::drawPixmap(const PointF& point, const Pixmap& pm)
{
    if (!active) {
        return;
    }
    ensureOk("drawPixmap");
    if (!finitePoint(point)) {
        throw std::invalid_argument("CairoPainter: drawPixmap point is not finite");
    }

    cairo_surface_t* surface = pixmapSurface(pm);
    const double surfaceWidth = double(cairo_image_surface_get_width(surface));
    const double surfaceHeight = double(cairo_image_surface_get_height(surface));
    if (!(surfaceWidth > 0.0) || !(surfaceHeight > 0.0)) {
        throw std::runtime_error("CairoPainter: invalid Pixmap image surface size");
    }

    const double drawWidth = pm.width() > 0 ? double(pm.width()) : surfaceWidth;
    const double drawHeight = pm.height() > 0 ? double(pm.height()) : surfaceHeight;
    const double scaleX = drawWidth / surfaceWidth;
    const double scaleY = drawHeight / surfaceHeight;

    CairoStateGuard guard(cr);
    cairo_translate(cr, point.x(), point.y());
    cairo_scale(cr, scaleX, scaleY);

    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(surface);
    if (!pattern) {
        throw std::runtime_error("CairoPainter: cannot allocate a Cairo source pattern for Pixmap");
    }
    if (cairo_pattern_status(pattern) != CAIRO_STATUS_SUCCESS) {
        const std::string message = cairo_status_to_string(cairo_pattern_status(pattern));
        cairo_pattern_destroy(pattern);
        throw std::runtime_error("CairoPainter: cannot create Pixmap source pattern: " + message);
    }
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_GOOD);
    cairo_set_source(cr, pattern);
    cairo_pattern_destroy(pattern);

    cairo_new_path(cr);
    cairo_rectangle(cr, 0.0, 0.0, drawWidth, drawHeight);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_fill(cr);
    ensureOk("drawPixmap");
}

void CairoPainter::Impl::drawTiledPixmap(const RectF& rect, const Pixmap& pm, const PointF& offset)
{
    if (!active) {
        return;
    }
    ensureOk("drawTiledPixmap");
    if (!finiteRect(rect) || !finitePoint(offset)) {
        throw std::invalid_argument("CairoPainter: drawTiledPixmap arguments are not finite");
    }

    const RectF r = rect.normalized();
    if (!r.isValid()) {
        return;
    }

    cairo_surface_t* surface = pixmapSurface(pm);
    const double surfaceWidth = double(cairo_image_surface_get_width(surface));
    const double surfaceHeight = double(cairo_image_surface_get_height(surface));
    if (!(surfaceWidth > 0.0) || !(surfaceHeight > 0.0)) {
        throw std::runtime_error("CairoPainter: invalid Pixmap image surface size");
    }

    const double drawWidth = pm.width() > 0 ? double(pm.width()) : surfaceWidth;
    const double drawHeight = pm.height() > 0 ? double(pm.height()) : surfaceHeight;
    const double scaleX = drawWidth / surfaceWidth;
    const double scaleY = drawHeight / surfaceHeight;

    CairoStateGuard guard(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, r.x(), r.y(), r.width(), r.height());
    cairo_clip(cr);

    // Qt anchors the tile at rectangle.topLeft() + offset.
    cairo_translate(cr, r.x() + offset.x(), r.y() + offset.y());
    cairo_scale(cr, scaleX, scaleY);

    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(surface);
    if (!pattern) {
        throw std::runtime_error("CairoPainter: cannot allocate a tiled Cairo pattern for Pixmap");
    }
    if (cairo_pattern_status(pattern) != CAIRO_STATUS_SUCCESS) {
        const std::string message = cairo_status_to_string(cairo_pattern_status(pattern));
        cairo_pattern_destroy(pattern);
        throw std::runtime_error("CairoPainter: cannot create tiled Pixmap pattern: " + message);
    }
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_GOOD);
    cairo_set_source(cr, pattern);
    cairo_pattern_destroy(pattern);

    cairo_paint(cr);
    ensureOk("drawTiledPixmap");
}

// -------------------------------------------------------- CairoPainter glue

CairoPainter::CairoPainter(cairo_t* context, const IFontsDatabase& fontsDatabase, double deviceDpi)
    : m_impl(std::make_unique<Impl>(context, fontsDatabase, deviceDpi))
{
}

CairoPainter::~CairoPainter() = default;

cairo_t* CairoPainter::cairoContext() const
{
    return m_impl->cr;
}

bool CairoPainter::isActive() const
{
    return m_impl->isActive();
}

void CairoPainter::beginTarget(const std::string& name)
{
    m_impl->beginTarget(name);
}

void CairoPainter::beforeEndTargetHook(Painter* painter)
{
    m_impl->beforeEndTargetHook(painter);
}

bool CairoPainter::endTarget(bool endDraw)
{
    return m_impl->endTarget(endDraw);
}

void CairoPainter::beginObject(const std::string& name)
{
    m_impl->beginObject(name);
}

void CairoPainter::endObject()
{
    m_impl->endObject();
}

void CairoPainter::setAntialiasing(bool arg)
{
    m_impl->setAntialiasing(arg);
}

void CairoPainter::setCompositionMode(CompositionMode mode)
{
    m_impl->setCompositionMode(mode);
}

void CairoPainter::setWindow(const RectF& window)
{
    m_impl->setWindow(window);
}

void CairoPainter::setViewport(const RectF& viewport)
{
    m_impl->setViewport(viewport);
}

void CairoPainter::setFont(const Font& font)
{
    m_impl->setFont(font);
}

const Font& CairoPainter::font() const
{
    return m_impl->state.font;
}

void CairoPainter::setPen(const Pen& pen)
{
    m_impl->setPen(pen);
}

void CairoPainter::setNoPen()
{
    m_impl->setNoPen();
}

const Pen& CairoPainter::pen() const
{
    return m_impl->state.pen;
}

void CairoPainter::setBrush(const Brush& brush)
{
    m_impl->setBrush(brush);
}

const Brush& CairoPainter::brush() const
{
    return m_impl->state.brush;
}

void CairoPainter::save()
{
    m_impl->save();
}

void CairoPainter::restore()
{
    m_impl->restore();
}

double CairoPainter::deviceLogicalDpi() const
{
    return m_impl->dpi;
}

void CairoPainter::setTransform(const Transform& transform)
{
    m_impl->setTransform(transform);
}

const Transform& CairoPainter::transform() const
{
    // Matches QPainterProvider: return the world transform last supplied via
    // setTransform().  When muse::draw::Painter supplies a complete
    // world*viewport matrix this is that matrix; for direct users it is the
    // world part before window/viewport composition.
    return m_impl->state.worldTransform;
}

void CairoPainter::drawPath(const PainterPath& path)
{
    m_impl->drawPath(path);
}

void CairoPainter::drawPolygon(const PointF* points, size_t pointCount, PolygonMode mode)
{
    m_impl->drawPolygon(points, pointCount, mode);
}

void CairoPainter::drawText(const PointF& point, const String& text)
{
    m_impl->drawText(point, text);
}

void CairoPainter::drawText(const RectF& rect, Alignment alignment, TextFlags textFlags, const String& text)
{
    m_impl->drawText(rect, alignment, textFlags, text);
}

void CairoPainter::drawSymbol(const PointF& point, char32_t ucs4Code)
{
    m_impl->drawSymbol(point, ucs4Code);
}

void CairoPainter::drawPixmap(const PointF& point, const Pixmap& pm)
{
    m_impl->drawPixmap(point, pm);
}

void CairoPainter::drawTiledPixmap(const RectF& rect, const Pixmap& pm, const PointF& offset)
{
    m_impl->drawTiledPixmap(rect, pm, offset);
}


bool CairoPainter::hasClipping() const
{
    return m_impl->hasClipping();
}

void CairoPainter::setClipRect(const RectF& rect)
{
    m_impl->setClipRect(rect);
}

void CairoPainter::setMask(const RectF& background, const std::vector<RectF>& maskRects)
{
    m_impl->setMask(background, maskRects);
}

void CairoPainter::setClipping(bool enable)
{
    m_impl->setClipping(enable);
}
}
