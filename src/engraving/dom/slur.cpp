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
#include "slur.h"

#include "../editing/editspanner.h"
#include "../editing/navigation.h"
#include "../editing/transaction/transaction.h"
#include "../rendering/iscorerenderer.h"

#include "arpeggio.h"
#include "beam.h"
#include "chord.h"
#include "masterscore.h"
#include "measure.h"
#include "note.h"
#include "part.h"
#include "score.h"
#include "staff.h"
#include "stafftype.h"
#include "stem.h"
#include "system.h"
#include "tremolotwochord.h"

#include "log.h"

using namespace mu;
using namespace mu::engraving;
using namespace muse::draw;

namespace mu::engraving {
SlurSegment::SlurSegment(Slur* sp, ElementType type)
    : SlurTieSegment(type, sp)
{
}

SlurSegment::SlurSegment(const SlurSegment& ss)
    : SlurTieSegment(ss)
{
}

//---------------------------------------------------------
//   searchCR
//---------------------------------------------------------

static ChordRest* searchCR(Segment* segment, track_idx_t startTrack, track_idx_t endTrack)
{
    for (Segment* s = segment; s; s = s->next(SegmentType::ChordRest)) {       // restrict search to measure
        if (startTrack > endTrack) {
            for (int t = static_cast<int>(startTrack) - 1; t >= static_cast<int>(endTrack); --t) {
                if (s->element(t)) {
                    return toChordRest(s->element(t));
                }
            }
        } else {
            for (track_idx_t t = startTrack; t < endTrack; ++t) {
                if (s->element(t)) {
                    return toChordRest(s->element(t));
                }
            }
        }
    }
    return 0;
}

//---------------------------------------------------------
//   isEdited
//---------------------------------------------------------

bool SlurSegment::isEdited() const
{
    for (int i = 0; i < int(Grip::GRIPS); ++i) {
        if (!m_ups[i].off.isNull()) {
            return true;
        }
    }
    return false;
}

bool SlurSegment::isEndPointsEdited() const
{
    return !(m_ups[int(Grip::START)].off.isNull() && m_ups[int(Grip::END)].off.isNull());
}

double SlurSegment::endWidth() const
{
    return style().styleAbsolute(Sid::slurEndWidth);
}

double SlurSegment::midWidth() const
{
    return style().styleAbsolute(Sid::slurMidWidth);
}

double SlurSegment::dottedWidth() const
{
    return style().styleAbsolute(Sid::slurDottedWidth);
}

Color SlurSegment::curColor(const rendering::PaintOptions& opt) const
{
    return EngravingItem::curColor(getProperty(Pid::VISIBLE).toBool(), getProperty(Pid::COLOR).value<Color>(), opt);
}

Slur::Slur(const Slur& s)
    : SlurTie(s)
{
    _connectedElement = s._connectedElement;
    _partialSpannerDirection = s._partialSpannerDirection;
}

//---------------------------------------------------------
//   Slur
//---------------------------------------------------------

Slur::Slur(DummyParentOr<EngravingItem> parent, ElementType type)
    : SlurTie(type, parent)
{
}

double Slur::scalingFactor() const
{
    Chord* startC = startElement() && startElement()->isChord() ? toChord(startElement()) : nullptr;
    Chord* endC = endElement() && endElement()->isChord() ? toChord(endElement()) : nullptr;

    if (!startC || !endC) {
        return 1.0;
    }

    if ((startC->isGraceBefore() && startC->parent() == endC)
        || (endC->isGraceAfter() && endC->parent() == startC)) {
        return style().styleD(Sid::graceNoteMag);
    }

    if (startC->isGrace()) {
        startC = toChord(startC->parent());
    }
    if (endC->isGrace()) {
        endC = toChord(endC->parent());
    }

    return 0.5 * (startC->intrinsicMag() + endC->intrinsicMag());
}

void Slur::undoSetIncoming(bool incoming)
{
    if (incoming == isIncoming()) {
        return;
    }

    undoChangeProperty(Pid::PARTIAL_SPANNER_DIRECTION, calcIncomingDirection(incoming), PropertyFlags::UNSTYLED);
}

void Slur::undoSetOutgoing(bool outgoing)
{
    if (outgoing == isOutgoing()) {
        return;
    }

    undoChangeProperty(Pid::PARTIAL_SPANNER_DIRECTION, calcOutgoingDirection(outgoing), PropertyFlags::UNSTYLED);
}

void Slur::setIncoming(bool incoming)
{
    if (incoming == isIncoming()) {
        return;
    }

    _partialSpannerDirection = calcIncomingDirection(incoming);
}

void Slur::setOutgoing(bool outgoing)
{
    if (outgoing == isOutgoing()) {
        return;
    }

    _partialSpannerDirection = calcOutgoingDirection(outgoing);
}

PartialSpannerDirection Slur::calcIncomingDirection(bool incoming)
{
    PartialSpannerDirection dir = PartialSpannerDirection::INCOMING;
    if (incoming) {
        SlurSegment* firstSeg = nsegments() > 0 ? frontSegment() : nullptr;
        if (firstSeg) {
            firstSeg->setSlurOffset(Grip::START, PointF(0, 0));
        }
        dir = _partialSpannerDirection
              == PartialSpannerDirection::OUTGOING ? PartialSpannerDirection::BOTH : PartialSpannerDirection::INCOMING;
    } else {
        dir = _partialSpannerDirection == PartialSpannerDirection::BOTH ? PartialSpannerDirection::OUTGOING : PartialSpannerDirection::NONE;
    }
    return dir;
}

PartialSpannerDirection Slur::calcOutgoingDirection(bool outgoing)
{
    PartialSpannerDirection dir = PartialSpannerDirection::OUTGOING;
    if (outgoing) {
        SlurSegment* lastSeg = nsegments() > 0 ? backSegment() : nullptr;
        if (lastSeg) {
            lastSeg->setSlurOffset(Grip::END, PointF(0, 0));
        }
        dir = _partialSpannerDirection
              == PartialSpannerDirection::INCOMING ? PartialSpannerDirection::BOTH : PartialSpannerDirection::OUTGOING;
    } else {
        dir = _partialSpannerDirection == PartialSpannerDirection::BOTH ? PartialSpannerDirection::INCOMING : PartialSpannerDirection::NONE;
    }
    return dir;
}

bool Slur::isIncoming() const
{
    return _partialSpannerDirection == PartialSpannerDirection::BOTH || _partialSpannerDirection == PartialSpannerDirection::INCOMING;
}

bool Slur::isOutgoing() const
{
    return _partialSpannerDirection == PartialSpannerDirection::BOTH || _partialSpannerDirection == PartialSpannerDirection::OUTGOING;
}

void Slur::undoChangeStartEndElements(ChordRest* scr, ChordRest* ecr)
{
    for (EngravingObject* lsp : linkList()) {
        Spanner* sp = toSpanner(lsp);
        if (sp == this) {
            score()->undo(new ChangeSpannerElements(this, scr, ecr));
        } else {
            EngravingItem* se = 0;
            EngravingItem* ee = 0;
            if (scr) {
                std::list<EngravingObject*> sel = scr->linkList();
                for (EngravingObject* lcr : sel) {
                    EngravingItem* le = toEngravingItem(lcr);
                    if (le->score() == sp->score() && le->track() == sp->track()) {
                        se = le;
                        break;
                    }
                }
            }
            if (ecr) {
                std::list<EngravingObject*> sel = ecr->linkList();
                for (EngravingObject* lcr : sel) {
                    EngravingItem* le = toEngravingItem(lcr);
                    if (le->score() == sp->score() && le->track() == sp->track2()) {
                        ee = le;
                        break;
                    }
                }
            }
            if (se && ee) {
                score()->undo(new ChangeStartEndSpanner(sp, se, ee));
                renderer()->layoutItem(sp);
            }
        }
    }
}

//---------------------------------------------------------
//   setTrack
//---------------------------------------------------------

void Slur::setTrack(track_idx_t n)
{
    EngravingItem::setTrack(n);
    for (SpannerSegment* ss : spannerSegments()) {
        ss->setTrack(n);
    }
}

bool Slur::isCrossStaff()
{
    return startCR() && endCR()
           && (startCR()->staffMove() != 0 || endCR()->staffMove() != 0
               || startCR()->vStaffIdx() != endCR()->vStaffIdx());
}

bool Slur::hasCrossBeams()
{
    return (startCR() && startCR()->beam() && startCR()->beam()->cross()) || (endCR() && endCR()->beam() && endCR()->beam()->cross());
}

PropertyValue Slur::getProperty(Pid propertyId) const
{
    switch (propertyId) {
    case Pid::PARTIAL_SPANNER_DIRECTION:
        return partialSpannerDirection();
    default:
        return SlurTie::getProperty(propertyId);
    }
}

PropertyValue Slur::propertyDefault(Pid id) const
{
    switch (id) {
    case Pid::PARTIAL_SPANNER_DIRECTION:
        return PartialSpannerDirection::NONE;
    default:
        return SlurTie::propertyDefault(id);
    }
}

bool Slur::setProperty(Pid propertyId, const PropertyValue& v)
{
    switch (propertyId) {
    case Pid::PARTIAL_SPANNER_DIRECTION:
        setPartialSpannerDirection(v.value<PartialSpannerDirection>());
        break;
    default:
        return SlurTie::setProperty(propertyId, v);
    }
    triggerLayout();
    return true;
}
}
