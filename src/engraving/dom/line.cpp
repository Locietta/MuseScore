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

#include "line.h"

#include <vector>

#include "containers.h"

#include "iengravingconfiguration.h" // IWYU pragma: keep

#include "../editing/navigation.h"
#include "../rendering/iscorerenderer.h"

#include "anchors.h"
#include "barline.h"
#include "chord.h"
#include "dynamic.h"
#include "measure.h"
#include "note.h"
#include "page.h"
#include "part.h"
#include "score.h"
#include "segment.h"
#include "staff.h"
#include "system.h"
#include "utils.h"

#include "log.h"

using namespace mu;
using namespace mu::engraving;

namespace mu::engraving {
LineSegment::LineSegment(const ElementType& type, SLine* sp, ElementFlags f)
    : SpannerSegment(type, sp, f)
{
}

//---------------------------------------------------------
//   LineSegment
//---------------------------------------------------------

LineSegment::LineSegment(const LineSegment& s)
    : SpannerSegment(s)
{
}

//---------------------------------------------------------
//   leftAnchorPosition
//---------------------------------------------------------

PointF LineSegment::leftAnchorPosition(const double& systemPositionY) const
{
    if (isMiddleType() || isEndType()) {
        return PointF(system()->firstMeasure()->pageBoundingRect().left(), systemPositionY);
    }

    PointF result;

    Segment* startSeg = line()->startSegment();
    if (!startSeg) {
        return result;
    }

    System* s = startSeg->measure()->system();
    result = startSeg->pos() + startSeg->measure()->pos();
    result.ry() += systemPositionY - system()->pos().y();

    if (s) {
        result += s->pos();      // to page coordinates
    }
    return result;
}

//---------------------------------------------------------
//   rightAnchorPosition
//---------------------------------------------------------

PointF LineSegment::rightAnchorPosition(const double& systemPositionY) const
{
    const System* sys = system();
    IF_ASSERT_FAILED(sys) {
        return PointF();
    }

    bool endsAtSystemEnd = isSingleEndType() && line()->tick2() == sys->endTick();
    if (isMiddleType() || isBeginType() || endsAtSystemEnd) {
        return PointF(sys->endingXForOpenEndedLines() + sys->x(), systemPositionY);
    }

    PointF result;

    Segment* endSeg = line()->endSegment();
    if (!endSeg) {
        return result;
    }

    System* s = endSeg->measure()->system();
    result = endSeg->pos() + endSeg->measure()->pos();
    result.ry() += systemPositionY - system()->pos().y();

    if (s) {
        result += s->pos();    // to page coordinates
    }
    return result;
}

//---------------------------------------------------------
//   findSegmentForGrip
//---------------------------------------------------------

Segment* LineSegment::findSegmentForGrip(Grip grip, PointF pos) const
{
    if (grip != Grip::START && grip != Grip::END) {
        return nullptr;
    }

    SLine* l = line();
    const bool left = (grip == Grip::START);

    const staff_idx_t oldStaffIndex = left ? staffIdx() : track2staff(l->effectiveTrack2());

    const double spacingFactor = 0.5;   // defines the point where canvas is divided between segments, systems etc.

    System* sys = system();
    const std::vector<System*> foundSystems = score()->searchSystem(pos, /*includeMeasurelessSystems*/ false, sys, spacingFactor);

    if (!foundSystems.empty() && !muse::contains(foundSystems, sys) && foundSystems[0]->staves().size()) {
        sys = foundSystems[0];
    }

    if (!sys) {
        return nullptr;
    }

    // Restrict searching segment to the correct staff
    pos.setY(sys->staffCanvasYpage(oldStaffIndex));

    Segment* seg = nullptr;   // don't prefer any segment while searching line position
    staff_idx_t staffIndex = oldStaffIndex;
    score()->dragPosition(pos, &staffIndex, &seg, spacingFactor, allowTimeAnchor());

    // Lines that don't allow time anchors can anchor to the last segment in the score (see also LineSegment::findNewAnchorSegment)
    if (seg && !allowTimeAnchor() && grip == Grip::END) {
        Segment* last = score()->lastSegment();
        const bool lastSegIsClosest = std::abs(last->canvasX() - pos.x()) < std::abs(seg->canvasX() - pos.x());
        if (lastSegIsClosest) {
            return last;
        }
    }

    return seg;
}

//---------------------------------------------------------
//   deltaRebaseLeft
///   Helper function for anchors rebasing when dragging.
//---------------------------------------------------------

PointF LineSegment::deltaRebaseLeft(const Segment* oldSeg, const Segment* newSeg)
{
    if (oldSeg == newSeg) {
        return PointF();
    }
    return oldSeg->canvasPos() - newSeg->canvasPos();
}

//---------------------------------------------------------
//   deltaRebaseRight
///   Helper function for anchors rebasing when dragging.
//---------------------------------------------------------

PointF LineSegment::deltaRebaseRight(const Segment* oldSeg, const Segment* newSeg)
{
    if (oldSeg == newSeg) {
        return PointF();
    }

    const PointF oldBase = oldSeg->canvasPos();
    const PointF newBase = newSeg->canvasPos();
    return oldBase - newBase;
}

//---------------------------------------------------------
//   lastSegmentEndTick
///   Helper function for anchors rebasing when dragging.
//---------------------------------------------------------

Fraction LineSegment::lastSegmentEndTick(const Segment* seg, const Spanner* s)
{
    return seg->tick() + seg->ticksInStaff(track2staff(s->effectiveTrack2()));
}

//---------------------------------------------------------
//   rebaseAnchor
///   If system has changed, returns the new line segment
///   (may appear to be reused \c this segment), otherwise
///   returns nullptr.
//---------------------------------------------------------

LineSegment* LineSegment::rebaseAnchor(Grip grip, Segment* newSeg)
{
    switch (grip) {
    case Grip::START:
        if (!isSingleBeginType()) {
            return nullptr;
        }
        break;
    case Grip::END:
        if (!isSingleEndType()) {
            return nullptr;
        }
        break;
    default:
        return nullptr;
    }

    SLine* l = line();
    const bool left = (grip == Grip::START);
    Segment* const oldSeg = left ? l->startSegment() : l->endSegment();
    System* const oldLineSegSystem = system();
    System* const oldSpannerSystem = oldSeg->measure()->system();

    if (!newSeg || oldSeg == newSeg) {
        return nullptr;
    }

    Fraction newSegTick = newSeg->tick();
    Fraction startTick = left ? newSegTick : l->tick();
    Fraction endTick = left ? l->tick2() : (newSegTick == startTick ? newSegTick + newSeg->ticks() : newSegTick);

    if (endTick <= startTick) {
        if (left) {
            endTick = lastSegmentEndTick(newSeg, l);
        } else {
            startTick = newSeg->tick();
        }
    }

    bool anchorChanged = false;

    System* lineSegSys = system();
    PointF oldPos = line()->linePos(grip, &lineSegSys);

    if (l->tick() != startTick) {
        l->undoChangeProperty(Pid::SPANNER_TICK, startTick);
        anchorChanged = true;
    }

    const Fraction ticksLength = endTick - startTick;
    if (ticksLength != l->ticks()) {
        l->undoChangeProperty(Pid::SPANNER_TICKS, ticksLength);
        anchorChanged = true;
    }

    const Segment* newTickSeg = left ? l->startSegment() : l->endSegment();
    const System* newSpannerSystem = newTickSeg->measure()->system();

    if ((oldLineSegSystem && newSeg->system() != oldLineSegSystem) || oldSpannerSystem != newSpannerSystem) {
        renderer()->layoutItem(l);
        return l->segmentsEmpty() ? nullptr : left ? l->frontSegment() : l->backSegment();
    } else if (anchorChanged) {
        rebaseOffsetsOnAnchorChanged(grip, oldPos, oldLineSegSystem);
    }

    return nullptr;
}

void LineSegment::rebaseOffsetsOnAnchorChanged(Grip grip, const PointF& oldPos, System* sys)
{
    PointF newPos = line()->linePos(grip, &sys);
    PointF delta = oldPos - newPos;
    if (grip == Grip::START) {
        setOffset(offset() + delta);
        m_offset2 -= delta;
    } else {
        m_offset2 += delta;
    }
}

//---------------------------------------------------------
//   spatiumChanged
//---------------------------------------------------------

void LineSegment::spatiumChanged(double ov, double nv)
{
    EngravingItem::spatiumChanged(ov, nv);
    double scale = nv / ov;
    m_offset2 *= scale;
}

//---------------------------------------------------------
//   localSpatiumChanged
//---------------------------------------------------------

void LineSegment::localSpatiumChanged(double ov, double nv)
{
    EngravingItem::localSpatiumChanged(ov, nv);
    m_offset2 *= nv / ov;
}

//---------------------------------------------------------
//   propertyDelegate
//---------------------------------------------------------

EngravingObject* LineSegment::propertyDelegate(Pid pid) const
{
    if (pid == Pid::DIAGONAL
        || pid == Pid::COLOR
        || pid == Pid::LINE_WIDTH
        || pid == Pid::LINE_STYLE
        || pid == Pid::DASH_LINE_LEN
        || pid == Pid::DASH_GAP_LEN) {
        return spanner();
    }
    return SpannerSegment::propertyDelegate(pid);
}

//---------------------------------------------------------
//   dragAnchorLines
//---------------------------------------------------------

Spatium LineSegment::lineWidth() const
{
    if (!line()) {
        return 0.0_sp;
    }

    return line()->lineWidth();
}

double LineSegment::absoluteFromSpatium(const Spatium& sp) const
{
    if (!line()) {
        return EngravingItem::absoluteFromSpatium(sp);
    }

    return line()->absoluteFromSpatium(sp);
}

//---------------------------------------------------------
//   SLine
//---------------------------------------------------------

SLine::SLine(const ElementType& type, EngravingObject* parent, ElementFlags f)
    : Spanner(type, parent, f)
{
    setTrack(0);
    m_lineColor = configuration()->defaultColor();
    m_lineWidth = 0.15_sp;
}

SLine::SLine(const SLine& s)
    : Spanner(s)
{
    m_diagonal    = s.m_diagonal;
    m_lineWidth   = s.m_lineWidth;
    m_lineColor   = s.m_lineColor;
    m_lineStyle   = s.m_lineStyle;
    m_dashLineLen = s.m_dashLineLen;
    m_dashGapLen  = s.m_dashGapLen;
}

PointF SLine::linePos(Grip grip, System** system) const
{
    bool start = grip == Grip::START;
    bool mmRest = style().styleB(Sid::createMultiMeasureRests);

    switch (anchor()) {
    case Spanner::Anchor::NOTE:
    {
        EngravingItem* item = start ? startElement() : endElement();
        Note* note = item && item->isNote() ? toNote(item) : nullptr;
        if (!note) {
            return PointF();
        }
        *system = note->chord()->segment()->measure()->system();
        if (!(*system)) {
            return PointF();
        }
        return note->pagePos() - (*system)->pagePos();
    }
    case Spanner::Anchor::CHORDREST:
    case Spanner::Anchor::SEGMENT:
    {
        Segment* segment = start ? startSegment() : endSegment();
        if (!segment) {
            return PointF();
        }
        if (anchor() == Spanner::Anchor::CHORDREST && !segment->isChordRestType()) {
            return PointF();
        }
        if (!start) {
            Fraction curTick = segment->tick();
            while (true) {
                Segment* prevSeg = mmRest ? segment->prev1MM() : segment->prev1();
                if (prevSeg && prevSeg->tick() == curTick) {
                    segment = prevSeg;
                } else {
                    break;
                }
            }
        }
        *system = segment->measure()->system();
        double x = segment->x() + segment->measure()->x() - (start ? 0.0 : spatium());
        return PointF(x, 0.0);
    }
    case Spanner::Anchor::MEASURE:
    {
        Measure* measure = score()->tick2measureMM(start ? tick() : tick2());
        if (!measure) {
            return PointF();
        }
        *system = measure->system();
        double x = measure->x();
        return PointF(x, 0.0);
    }
    default:
        return PointF();
    }
}

//---------------------------------------------------------
//   setLen
//    used to create an element suitable for palette
//---------------------------------------------------------

void SLine::setLen(double l)
{
    if (spannerSegments().empty()) {
        add(createLineSegment());
    }
    LineSegment* s = frontSegment();
    s->setPos(PointF());
    s->setPos2(PointF(l, 0));
}

//---------------------------------------------------------
//   getProperty
//---------------------------------------------------------

PropertyValue SLine::getProperty(Pid id) const
{
    switch (id) {
    case Pid::DIAGONAL:
        return m_diagonal;
    case Pid::COLOR:
        return PropertyValue::fromValue(m_lineColor);
    case Pid::LINE_WIDTH:
        return m_lineWidth;
    case Pid::LINE_STYLE:
        return m_lineStyle;
    case Pid::DASH_LINE_LEN:
        return dashLineLen();
    case Pid::DASH_GAP_LEN:
        return dashGapLen();
    default:
        return Spanner::getProperty(id);
    }
}

//---------------------------------------------------------
//   setProperty
//---------------------------------------------------------

bool SLine::setProperty(Pid id, const PropertyValue& v)
{
    switch (id) {
    case Pid::DIAGONAL:
        m_diagonal = v.toBool();
        break;
    case Pid::COLOR:
        setLineColor(v.value<Color>());
        break;
    case Pid::LINE_WIDTH:
        if (v.type() == P_TYPE::ABSOLUTE) {
            m_lineWidth = Spatium::fromAbsolute(v.value<double>(), spatium());
        } else if (v.type() == P_TYPE::SPATIUM) {
            m_lineWidth = v.value<Spatium>();
        }
        break;
    case Pid::LINE_STYLE:
        m_lineStyle = v.value<LineType>();
        break;
    case Pid::DASH_LINE_LEN:
        setDashLineLen(v.toDouble());
        break;
    case Pid::DASH_GAP_LEN:
        setDashGapLen(v.toDouble());
        break;
    default:
        return Spanner::setProperty(id, v);
    }
    triggerLayout();
    return true;
}

//---------------------------------------------------------
//   propertyDefault
//---------------------------------------------------------

PropertyValue SLine::propertyDefault(Pid pid) const
{
    switch (pid) {
    case Pid::DIAGONAL:
        return false;
    case Pid::COLOR:
        return PropertyValue::fromValue(configuration()->defaultColor());
    case Pid::LINE_WIDTH:
        if (propertyFlags(pid) != PropertyFlags::NOSTYLE) {
            return Spanner::propertyDefault(pid);
        }
        return 0.15_sp;
    case Pid::LINE_STYLE:
        if (propertyFlags(pid) != PropertyFlags::NOSTYLE) {
            return Spanner::propertyDefault(pid);
        }
        return LineType::SOLID;
    case Pid::DASH_LINE_LEN:
    case Pid::DASH_GAP_LEN:
        if (propertyFlags(pid) != PropertyFlags::NOSTYLE) {
            return Spanner::propertyDefault(pid);
        }
        return 5.0;
    default:
        return Spanner::propertyDefault(pid);
    }
}

void SLine::undoMoveStart(Fraction tickDiff)
{
    Fraction newTick = tick() + tickDiff;
    if (newTick >= Fraction(0, 1)) {
        undoChangeProperty(Pid::SPANNER_TICK, newTick);
    }
    Fraction newDuration = ticks() - tickDiff;
    if (newDuration > Fraction(0, 1)) {
        undoChangeProperty(Pid::SPANNER_TICKS, newDuration);
    }
}

void SLine::undoMoveEnd(Fraction tickDiff)
{
    Fraction newDuration = ticks() + tickDiff;
    if (newDuration > Fraction(0, 1)) {
        undoChangeProperty(Pid::SPANNER_TICKS, newDuration);
    } else {
        Fraction newTick = tick() + tickDiff;
        if (newTick >= Fraction(0, 1)) {
            undoChangeProperty(Pid::SPANNER_TICK, tick() + tickDiff);
        }
    }
}

Note* SLine::guessFinalNote(Note* startNote)
{
    Chord* chord = startNote->chord();
    if (chord->isGraceBefore()) {
        Chord* parentChord = toChord(chord->parent());
        GraceNotesGroup& gracesBefore = parentChord->graceNotesBefore();
        auto positionOfThis = std::find(gracesBefore.begin(), gracesBefore.end(), chord);
        if (positionOfThis != gracesBefore.end()) {
            auto nextPosition = ++positionOfThis;
            if (nextPosition != gracesBefore.end()) {
                return (*nextPosition)->upNote();
            }
        }
        return parentChord->upNote();
    } else if (chord->isGraceAfter()) {
        Chord* parentChord = toChord(chord->parent());
        GraceNotesGroup& gracesAfter = parentChord->graceNotesAfter();
        auto positionOfThis = std::find(gracesAfter.begin(), gracesAfter.end(), chord);
        if (positionOfThis != gracesAfter.end()) {
            auto nextPosition = ++positionOfThis;
            if (nextPosition != gracesAfter.end()) {
                return (*nextPosition)->upNote();
            }
        }
        chord = toChord(chord->parent());
    } else {
        std::vector<Chord*> graces = chord->graceNotesAfter();
        if (graces.size() > 0) {
            return graces.front()->upNote();
        }
    }

    if (!chord->ownershipParent()->isSegment()) {
        return 0;
    }

    Segment* segm = chord->score()->tick2rightSegment(chord->endTick());
    while (segm && !segm->isChordRestType()) {
        segm = segm->next1();
    }

    if (!segm) {
        return nullptr;
    }

    track_idx_t chordTrack = chord->track();
    Part* part = chord->part();

    Chord* target = nullptr;
    if (segm->element(chordTrack) && segm->element(chordTrack)->isChord()) {
        target = toChord(segm->element(chordTrack));
    } else {
        for (EngravingItem* currChord : segm->elist()) {
            if (currChord && currChord->isChord() && toChord(currChord)->part() == part) {
                target = toChord(currChord);
                break;
            }
        }
    }

    if (target && target->notes().size() > 0) {
        const std::vector<Chord*>& graces = target->graceNotesBefore();
        if (graces.size() > 0) {
            return graces.front()->upNote();
        }
        // normal case: try to return the note in the next chord that is in the
        // same position as the start note relative to the end chord
        size_t startNoteIdx = muse::indexOf(chord->notes(), startNote);
        size_t endNoteIdx = std::min(startNoteIdx, target->notes().size() - 1);
        return target->notes().at(endNoteIdx);
    }

    LOGD("no second note for note anchored line found");
    return nullptr;
}
}
