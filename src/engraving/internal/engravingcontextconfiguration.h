/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "../iengravingcontextconfiguration.h"

#include "modularity/ioc.h"
#ifndef NO_QT_SUPPORT
#include "ui/iuicontextconfiguration.h"
#endif
#ifndef ENGRAVING_NO_ACCESSIBILITY
#include "accessibility/iaccessibilitycontroller.h"
#endif

namespace mu::engraving {
class EngravingContextConfiguration : public IEngravingContextConfiguration, public muse::Contextable
{
#ifndef NO_QT_SUPPORT
    muse::ContextInject<muse::ui::IUiContextConfiguration> uiConfiguration = { this };
#endif
#ifndef ENGRAVING_NO_ACCESSIBILITY
    muse::ContextInject<muse::accessibility::IAccessibilityController> accessibilityController = { this };
#endif

public:
    EngravingContextConfiguration(const muse::modularity::ContextPtr& iocCtx)
        : Contextable(iocCtx) {}

    double guiScaling() const override;

    bool isAccessibleEnabled() const override;
};
}
