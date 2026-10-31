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

#include "engravingitem.h"

namespace mu::engraving {
class Chord;
class Transaction;

class Stem final : public EngravingItem
{
    OBJECT_ALLOCATOR(engraving, Stem)
    DECLARE_CLASSOF(ElementType::STEM)

public:

    Stem(const Stem&) = default;
    Stem& operator=(const Stem&) = delete;

    Stem* clone() const override { return new Stem(*this); }

    EngravingItem* elementBase() const override;


    bool acceptDrop(EditData&) const override;
    EngravingItem* drop(Transaction& tx, EditData&) override;

    void reset() override;
    PropertyValue getProperty(Pid propertyId) const override;
    bool setProperty(Pid propertyId, const PropertyValue&) override;
    PropertyValue propertyDefault(Pid id) const override;

    staff_idx_t vStaffIdx() const override;

    Chord* chord() const { return toChord(ownershipParent()); }
    bool up() const;

    Spatium baseLength() const { return m_baseLength; }
    void setBaseLength(Spatium baseLength);

    Spatium userLength() const { return m_userLength; }
    void setUserLength(Spatium userLength) { m_userLength = userLength; }

    Spatium lineWidth() const { return m_lineWidth; }
    double lineWidthMag() const;
    void setLineWidth(Spatium lineWidth) { m_lineWidth = lineWidth; }

    PointF flagPosition() const;
    double length() const { return absoluteFromSpatium(m_baseLength + m_userLength); }


    struct LayoutData : public EngravingItem::LayoutData {
        LineF line;
        double beamCorrection = 0.0;
    };
    DECLARE_LAYOUTDATA_METHODS(Stem)

private:
    friend class Factory;
    Stem(DummyParentOr<Chord> parent);

    Spatium m_baseLength = 0.0_sp;

    Spatium m_userLength = 0.0_sp;
    Spatium m_lineWidth = 0.0_sp;
};
}
