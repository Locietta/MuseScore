/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef MUSE_VST_VSTPLUGINATTRS_H
#define MUSE_VST_VSTPLUGINATTRS_H

#include <string_view>

#include "global/types/string.h"

namespace muse::vst {
inline constexpr std::string_view AUDIO_RESOURCE_TYPE_NAME = "VstPlugin";

inline const String CATEGORIES_ATTRIBUTE(u"categories");

// Marks a VST as an instrument rather than an FX; shared contract between
// the meta reader (producer) and modulesMetaList (consumer).
inline const String INSTRUMENT_CATEGORY(u"Instrument");
}

#endif // MUSE_VST_VSTPLUGINATTRS_H
