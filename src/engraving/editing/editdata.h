/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

#include <memory>

#include "types/flags.h"
#include "types/string.h"
#include "../types/types.h"

namespace mu::engraving {
enum KeyboardModifier {
    NoModifier           = 0x00000000,
    ShiftModifier        = 0x02000000,
    ControlModifier      = 0x04000000,
    AltModifier          = 0x08000000,
    MetaModifier         = 0x10000000,
    KeypadModifier       = 0x20000000,
    GroupSwitchModifier  = 0x40000000,
    // Do not extend the mask to include 0x01000000
    KeyboardModifierMask = 0xfe000000
};
DECLARE_FLAGS(KeyboardModifiers, KeyboardModifier)
DECLARE_OPERATORS_FOR_FLAGS(KeyboardModifiers)

#ifdef __APPLE__
constexpr KeyboardModifier TextEditingControlModifier = AltModifier;
#else
constexpr KeyboardModifier TextEditingControlModifier = ControlModifier;
#endif

//---------------------------------------------------------
//   Grip
//---------------------------------------------------------

enum class Grip : signed char {
    NO_GRIP = -1,
    START = 0, END = 1,                           // arpeggio etc.
    LEFT = START, RIGHT = END,                    // aliases for dynamic
    MIDDLE = 2, APERTURE = 3,                     // Line
    /*START, END , */
    BEZIER1 = 2, SHOULDER = 3, BEZIER2 = 4, DRAG = 5,       // Slur
    GRIPS = 6                       // number of grips for slur
};

//---------------------------------------------------------
//   EditData
//    used in dragGrip
//---------------------------------------------------------
class ElementEditData;
using ElementEditDataPtr = std::shared_ptr<ElementEditData>;

class EngravingItem;
class EditData
{
public:

    PointF pos;
    KeyboardModifiers modifiers;

    // drop data:
    PointF dragOffset;
    EngravingItem* element = nullptr;
    EngravingItem* dropElement = nullptr;
    track_idx_t track = muse::nidx;

    EditData() = default;

    void clear();

    std::shared_ptr<ElementEditData> getData(const EngravingItem*) const;
    void addData(std::shared_ptr<ElementEditData>);
    bool control(bool textEditing = false) const;
    bool shift() const { return modifiers & ShiftModifier; }

private:
    std::vector<std::shared_ptr<ElementEditData> > m_data;
};
}
