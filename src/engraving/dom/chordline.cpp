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

#include "chordline.h"

#include <functional>

#include "types/translatablestring.h"

#include "../editing/elementeditdata.h"
#include "../types/typesconv.h"

#include "chord.h"
#include "note.h"

#include "log.h"

using namespace mu;
using namespace muse::draw;
using namespace mu::engraving;

namespace mu::engraving {
//---------------------------------------------------------
//   ChordLine
//---------------------------------------------------------

ChordLine::ChordLine(DummyParentOr<Chord> parent)
    : EngravingItem(ElementType::CHORDLINE, parent, ElementFlag::MOVABLE)
{
}

ChordLine::ChordLine(const ChordLine& cl)
    : EngravingItem(cl)
{
    m_modified = cl.m_modified;
    m_chordLineType = cl.m_chordLineType;
    m_straight = cl.m_straight;
    m_wavy = cl.m_wavy;
    m_lengthX = cl.m_lengthX;
    m_lengthY = cl.m_lengthY;
    m_note = cl.m_note;

    /* The manually edited shape is "given" data which happens to live in the layout
     * data, and EngravingItem's copy constructor doesn't copy that. Without this, an edited
     * chordline is invisible in newly created parts: m_modified is true, so the layout won't
     * regenerate the path, but the path itself is empty. */
    mutldata()->path = cl.ldata()->path;
}

//---------------------------------------------------------
//   setChordLineType
//---------------------------------------------------------

void ChordLine::setChordLineType(ChordLineType st)
{
    m_chordLineType = st;
}

const TranslatableString& ChordLine::chordLineTypeName() const
{
    return TConv::userName(m_chordLineType, m_straight, m_wavy);
}

//---------------------------------------------------------
//   slideType
//---------------------------------------------------------

static Note::SlideType slideType(ChordLineType type)
{
    static const std::unordered_map<ChordLineType, Note::SlideType> chordLineToSlideTypes {
        { ChordLineType::FALL, Note::SlideType::DownFromNote },
        { ChordLineType::DOIT, Note::SlideType::UpFromNote },
        { ChordLineType::SCOOP, Note::SlideType::UpToNote },
        { ChordLineType::PLOP, Note::SlideType::DownToNote }
    };

    auto it = chordLineToSlideTypes.find(type);
    if (it != chordLineToSlideTypes.end()) {
        return it->second;
    }

    return Note::SlideType::Undefined;
}

//---------------------------------------------------------
//   setNote
//---------------------------------------------------------

void ChordLine::setNote(Note* note)
{
    m_note = note;

    if (note) {
        note->attachSlide(slideType(m_chordLineType));
    }
}

SymId ChordLine::waveSym() const
{
    if (m_chordLineType == ChordLineType::FALL || m_chordLineType == ChordLineType::PLOP) {
        return SymId::brassFallRoughShort;
    }

    return SymId::brassLiftShort;
}

//---------------------------------------------------------
//   accessibleInfo
//---------------------------------------------------------

int ChordLine::subtype() const
{
    size_t h1 = std::hash<ChordLineType> {}(m_chordLineType);
    size_t h2 = std::hash<bool> {}(m_straight);
    size_t h3 = std::hash<bool> {}(m_wavy);

    return static_cast<int>(h1 ^ (h2 << 1) ^ (h3 << 2));
}

muse::TranslatableString ChordLine::subtypeUserName() const
{
    return chordLineTypeName();
}

//---------------------------------------------------------
//   getProperty
//---------------------------------------------------------

PropertyValue ChordLine::getProperty(Pid propertyId) const
{
    switch (propertyId) {
    case Pid::PATH:
        return PropertyValue::fromValue(ldata()->path);
    case Pid::CHORD_LINE_TYPE:
        return int(m_chordLineType);
    case Pid::CHORD_LINE_STRAIGHT:
        return m_straight;
    case Pid::CHORD_LINE_WAVY:
        return m_wavy;
    case Pid::PLAY:
        return m_playChordLine;
    default:
        break;
    }
    return EngravingItem::getProperty(propertyId);
}

//---------------------------------------------------------
//   setProperty
//---------------------------------------------------------

bool ChordLine::setProperty(Pid propertyId, const PropertyValue& val)
{
    switch (propertyId) {
    case Pid::PATH:
        mutldata()->path = val.value<PainterPath>();
        setModified(true);
        break;
    case Pid::CHORD_LINE_TYPE:
        setChordLineType(ChordLineType(val.toInt()));
        break;
    case Pid::CHORD_LINE_STRAIGHT:
        setStraight(val.toBool());
        break;
    case Pid::CHORD_LINE_WAVY:
        setWavy(val.toBool());
        break;
    case Pid::PLAY:
        setPlayChordLine(val.toBool());
        break;
    default:
        return EngravingItem::setProperty(propertyId, val);
    }
    triggerLayout();
    return true;
}

//---------------------------------------------------------
//   propertyDefault
//---------------------------------------------------------

PropertyValue ChordLine::propertyDefault(Pid pid) const
{
    switch (pid) {
    case Pid::CHORD_LINE_STRAIGHT:
        return false;
    case Pid::CHORD_LINE_WAVY:
        return false;
    case Pid::PLAY:
        return true;
    default:
        break;
    }
    return EngravingItem::propertyDefault(pid);
}
}
