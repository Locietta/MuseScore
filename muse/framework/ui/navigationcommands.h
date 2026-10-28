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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include "rcommand/commandtypes.h"

namespace muse::ui {
inline static const muse::rcommand::Command ESCAPE_COMMAND("command://navigation/escape");

inline static const muse::rcommand::Command NEXT_SECTION_COMMAND("command://navigation/next-section");
inline static const muse::rcommand::Command PREV_SECTION_COMMAND("command://navigation/prev-section");
inline static const muse::rcommand::Command NEXT_PANEL_COMMAND("command://navigation/next-panel");
inline static const muse::rcommand::Command PREV_PANEL_COMMAND("command://navigation/prev-panel");
//! NOTE Same as panel at the moment
inline static const muse::rcommand::Command NEXT_TAB_COMMAND("command://navigation/next-tab");
inline static const muse::rcommand::Command PREV_TAB_COMMAND("command://navigation/prev-tab");

inline static const muse::rcommand::Command RIGHT_COMMAND("command://navigation/right");
inline static const muse::rcommand::Command LEFT_COMMAND("command://navigation/left");
inline static const muse::rcommand::Command UP_COMMAND("command://navigation/up");
inline static const muse::rcommand::Command DOWN_COMMAND("command://navigation/down");

inline static const muse::rcommand::Command FIRST_CONTROL_COMMAND("command://navigation/first-control");
inline static const muse::rcommand::Command LAST_CONTROL_COMMAND("command://navigation/last-control");
inline static const muse::rcommand::Command NEXTROW_CONTROL_COMMAND("command://navigation/nextrow-control");
inline static const muse::rcommand::Command PREVROW_CONTROL_COMMAND("command://navigation/prevrow-control");

inline static const muse::rcommand::Command TRIGGER_CONTROL_COMMAND("command://navigation/trigger-control");
}
