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

#include "engravingcontextconfiguration.h"

using namespace mu::engraving;

double EngravingContextConfiguration::guiScaling() const
{
#ifndef NO_QT_SUPPORT
    if (!uiConfiguration()) {
        return 1.0;
    }

    return uiConfiguration()->guiScaling();
#else
    return 1.0;
#endif
}

bool EngravingContextConfiguration::isAccessibleEnabled() const
{
#ifndef ENGRAVING_NO_ACCESSIBILITY
    if (!accessibilityController()) {
        return false;
    }

    return accessibilityController()->isEnabled();
#else
    return false;
#endif
}
