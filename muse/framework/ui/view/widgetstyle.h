/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <memory>

#include <QProxyStyle>

class QWidget;

namespace muse::api {
class ThemeApi;
}

namespace muse::ui {
class WidgetStyle : public QProxyStyle
{
public:
    WidgetStyle(std::shared_ptr<api::ThemeApi> t);

    void init();

    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                            const QWidget* widget = nullptr) const override;
    QRect subControlRect(QStyle::ComplexControl control, const QStyleOptionComplex* option, QStyle::SubControl subControl,
                         const QWidget* widget = nullptr) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& contentsSize,
                           const QWidget* widget = nullptr) const override;
    QIcon standardIcon(QStyle::StandardPixmap standardIcon, const QStyleOption* option = nullptr,
                       const QWidget* widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override;
private:
    struct StyleState {
        bool enabled = false;
        bool hovered = false;
        bool pressed = false;
        bool focused = false;
    };

    void update();

    void drawButtonBackground(QPainter* painter, const QRect& rect, const StyleState& styleState, bool accentButton, bool flat,
                              const QColor& defaultBackground) const;
    void drawCheckboxIndicator(QPainter* painter, const QRect& rect, const StyleState& styleState, bool checked, bool indeterminate,
                               bool inMenu) const;
    void drawRadioButtonIndicator(QPainter* painter, const QRect& rect, const StyleState& styleState, bool selected) const;
    void drawLineEditBackground(QPainter* painter, const QRect& rect, const StyleState& styleState, bool editing) const;
    void drawIndicatorIcon(QPainter* painter, const QRect& rect, const StyleState& styleState, QStyle::PrimitiveElement element) const;
    void drawViewItemBackground(QPainter* painter, const QRect& rect, const StyleState& styleState, bool selected) const;
    void drawToolbarGrip(QPainter* painter, const QRect& rect, bool horizontal) const;

    std::shared_ptr<api::ThemeApi> m_theme;
};
}
