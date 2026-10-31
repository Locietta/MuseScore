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

#include "bracket.h"

#include "types/typesconv.h"

#include "bracketitem.h"
#include "instrumentname.h"
#include "measure.h"
#include "score.h"
#include "staff.h"
#include "system.h"
#include "text.h"

#include "log.h"

using namespace mu;
using namespace mu::engraving;
using namespace muse::draw;

//---------------------------------------------------------
//   Bracket
//---------------------------------------------------------

Bracket::Bracket(DummyParentOr<EngravingItem> parent)
    : EngravingItem(ElementType::BRACKET, parent)
{
    m_ay1          = 0;
    m_firstStaff  = 0;
    m_lastStaff   = 0;
    m_labelType   = InstrumentNameType::LONG;
    m_bi          = 0;
    m_braceSymbol = SymId::noSym;
    m_magx        = 1.;
    setGenerated(true);       // brackets are not saved
}

Bracket::~Bracket()
{
    if (m_text) {
        delete m_text;
    }
}

void Bracket::scanElements(std::function<void(EngravingItem*)> func)
{
    if (m_text) {
        m_text->scanElements(func);
    }

    EngravingItem::scanElements(func);
}

//---------------------------------------------------------
//   setStaffSpan
//---------------------------------------------------------

void Bracket::setStaffSpan(size_t a, size_t b)
{
    m_firstStaff = a;
    m_lastStaff = b;

    if (bracketType() == BracketType::BRACE
        && style().styleSt(Sid::musicalSymbolFont) != "Emmentaler" && style().styleSt(Sid::musicalSymbolFont) != "Gonville") {
        int v = static_cast<int>(m_lastStaff - m_firstStaff + 1);

        // if staves inner staves are hidden, decrease span
        for (size_t staffIndex = m_firstStaff; staffIndex <= m_lastStaff; ++staffIndex) {
            if (system() && !system()->staff(staffIndex)->show()) {
                --v;
            }
        }

        if (style().styleSt(Sid::musicalSymbolFont) == "Leland") {
            v = std::min(4, v);
        }

        // 1.625 is a "magic" number based on akkoladeDistance/4.0 (default value 6.5).
        m_magx = v + ((v - 1) * 1.625);

        if (v == 1) {
            m_braceSymbol = SymId::braceSmall;
        } else if (v <= 2) {
            m_braceSymbol = SymId::brace;
        } else if (v <= 3) {
            m_braceSymbol = SymId::braceLarge;
        } else {
            m_braceSymbol = SymId::braceLarger;
        }
    }
}



//---------------------------------------------------------
//   acceptDrop
//---------------------------------------------------------

bool Bracket::acceptDrop(EditData& data) const
{
    return data.dropElement->type() == ElementType::BRACKET;
}

//---------------------------------------------------------
//   drop
//---------------------------------------------------------

EngravingItem* Bracket::drop(Transaction&, EditData& data)
{
    EngravingItem* e = data.dropElement;
    Bracket* b = 0;
    if (e->isBracket()) {
        b = toBracket(e);
        undoChangeProperty(Pid::SYSTEM_BRACKET, int(b->bracketType()));
    }
    delete e;
    return this;
}

//---------------------------------------------------------
//   getProperty
//---------------------------------------------------------

PropertyValue Bracket::getProperty(Pid id) const
{
    PropertyValue v = EngravingItem::getProperty(id);
    if (!v.isValid()) {
        v = m_bi->getProperty(id);
    }
    return v;
}

//---------------------------------------------------------
//   setProperty
//---------------------------------------------------------

bool Bracket::setProperty(Pid id, const PropertyValue& v)
{
    return m_bi->setProperty(id, v);
}

//---------------------------------------------------------
//   propertyDefault
//---------------------------------------------------------

PropertyValue Bracket::propertyDefault(Pid id) const
{
    if (id == Pid::BRACKET_COLUMN) {
        return size_t(0);
    }
    PropertyValue v = EngravingItem::propertyDefault(id);
    if (!v.isValid()) {
        v = m_bi->propertyDefault(id);
    }
    return v;
}

//---------------------------------------------------------
//   subtypeUserName
//---------------------------------------------------------

muse::TranslatableString Bracket::subtypeUserName() const
{
    return TConv::userName(bracketType());
}

//---------------------------------------------------------
//   undoChangeProperty
//---------------------------------------------------------

void Bracket::undoChangeProperty(Pid id, const PropertyValue& v, PropertyFlags ps)
{
    if (id == Pid::COLOR) {
        setColor(v.value<Color>());
    }

    // brackets do not survive layout() and therefore cannot be on
    // the undo stack; delegate to BracketItem:
    BracketItem* bi = bracketItem();
    bi->undoChangeProperty(id, v, ps);
}

Fraction Bracket::tick() const
{
    if (measure()) {
        return measure()->tick();
    }
    return EngravingItem::tick();
}

//---------------------------------------------------------
//   setSelected
//---------------------------------------------------------

void Bracket::setSelected(bool f)
{
    m_bi->setSelected(f);
    if (m_text) {
        m_text->setSelected(f);
    }

    EngravingItem::setSelected(f);
}

bool Bracket::intersects(const Bracket* other) const
{
    return m_firstStaff <= other->m_lastStaff && m_lastStaff >= other->m_firstStaff;
}

bool Bracket::contains(staff_idx_t staffIdx) const
{
    return staffIdx >= m_firstStaff && staffIdx <= m_lastStaff;
}
