/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Qt-free Cairo implementation of muse::draw::IPaintProvider.
 *
 * The caller owns the cairo_t passed to the constructor; this class never
 * destroys it.  A cairo_t is not a QPaintDevice, so this backend keeps its own
 * font/pen/brush/window/viewport/clip state and mirrors it into Cairo.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "draw/ipaintprovider.h"

struct _cairo;
typedef struct _cairo cairo_t;


namespace muse::draw {
class IFontsDatabase;

class CairoPainter final : public IPaintProvider
{
public:
    /*! deviceDpi is the logical DPI of the Cairo target (the value
     *  Painter::deviceLogicalDpi() must report).  For MuseScore engraving
     *  coordinates the baseline is 1200.
     *
     *  Font point sizes are converted to user-space pixels as
     *  pointSize * deviceDpi / 72.  muse::draw::Painter::applyFontSizeScaling()
     *  first multiplies the point size by 1200 / deviceDpi, so engraving text
     *  ends up at the intended points * 1200 / 72 in score coordinates.
     *
     *  Ownership of context stays with the caller; this object never calls
     *  cairo_destroy().  The cairo_t must outlive the CairoPainter.  Text is
     *  drawn with the font bytes of fontsDatabase, which must outlive it too.
     */
    CairoPainter(cairo_t* context, const IFontsDatabase& fontsDatabase, double deviceDpi = 1200.0);
    ~CairoPainter() override;

    CairoPainter(const CairoPainter&) = delete;
    CairoPainter& operator=(const CairoPainter&) = delete;

    cairo_t* cairoContext() const;

    bool isActive() const override;
    void beginTarget(const std::string& name) override;
    void beforeEndTargetHook(Painter* painter) override;
    bool endTarget(bool endDraw = false) override;

    void beginObject(const std::string& name) override;
    void endObject() override;

    void setAntialiasing(bool arg) override;
    void setCompositionMode(CompositionMode mode) override;
    void setWindow(const RectF& window) override;
    void setViewport(const RectF& viewport) override;

    void setFont(const Font& font) override;
    const Font& font() const override;

    void setPen(const Pen& pen) override;
    void setNoPen() override;
    const Pen& pen() const override;

    void setBrush(const Brush& brush) override;
    const Brush& brush() const override;

    void save() override;
    void restore() override;

    double deviceLogicalDpi() const override;

    /*! setTransform() installs the complete world*viewport matrix, matching
     *  QPainterProvider.  muse::draw::Painter already composes window/viewport
     *  and passes the result here.  For direct users, window/viewport set
     *  before the first setTransform() are composed automatically; after an
     *  explicit transform the matrix is authoritative.
     */
    void setTransform(const Transform& transform) override;
    const Transform& transform() const override;

    void drawPath(const PainterPath& path) override;
    void drawPolygon(const PointF* points, size_t pointCount, PolygonMode mode) override;

    void drawText(const PointF& point, const String& text) override;
    void drawText(const RectF& rect, Alignment alignment, TextFlags textFlags, const String& text) override;

    void drawSymbol(const PointF& point, char32_t ucs4Code) override;

    void drawPixmap(const PointF& point, const Pixmap& pm) override;
    void drawTiledPixmap(const RectF& rect, const Pixmap& pm, const PointF& offset = PointF()) override;


    bool hasClipping() const override;

    void setClipRect(const RectF& rect) override;
    void setMask(const RectF& background, const std::vector<RectF>& maskRects) override;
    void setClipping(bool enable) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/*! Convenience spelling for callers that name the provider after the file. */
using CairoPaintProvider = CairoPainter;
}
