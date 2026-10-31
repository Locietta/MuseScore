// SPDX-License-Identifier: GPL-3.0-only
#include "score_semantic.h"

#include "engraving/dom/arpeggio.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/tremolosinglechord.h"
#include "engraving/dom/tremolotwochord.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/clef.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/repeatlist.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftype.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/tempotimeline.h"
#include "engraving/dom/textbase.h"
#include "engraving/dom/textlinebase.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/volta.h"
#include "engraving/types/typesconv.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>

using namespace mu::engraving;
using json = nlohmann::ordered_json;

namespace music {
namespace {

std::string str(const muse::String& value) { return value.toStdString(); }
std::string str(muse::AsciiStringView value) { return std::string(value.ascii(), value.size()); }
// Instrument names are stored as rich text; formatting tags are appearance.
std::string plain(const muse::String& rich)
{
    muse::String text;
    bool tag = false;
    for (size_t i = 0; i < rich.size(); ++i) {
        const char16_t c = rich.at(i).unicode();
        if (c == u'<') tag = true;
        else if (c == u'>') tag = false;
        else if (!tag) text.append(rich.at(i));
    }
    return TextBase::unEscape(text).toStdString();
}

std::string fraction(const Fraction& f) { return std::to_string(f.numerator()) + "/" + std::to_string(f.denominator()); }

// Same names as the MSCX grace tags parsed by scripts/test_roundtrip.py.
const char* grace_kind(NoteType type)
{
    switch (type) {
    case NoteType::NORMAL: return "none";
    case NoteType::ACCIACCATURA: return "acciaccatura";
    case NoteType::APPOGGIATURA: return "appoggiatura";
    case NoteType::GRACE4: return "grace4";
    case NoteType::GRACE16: return "grace16";
    case NoteType::GRACE32: return "grace32";
    case NoteType::GRACE8_AFTER: return "grace8after";
    case NoteType::GRACE16_AFTER: return "grace16after";
    case NoteType::GRACE32_AFTER: return "grace32after";
    default: return "invalid";
    }
}

const char* staff_group(StaffGroup group)
{
    switch (group) {
    case StaffGroup::STANDARD: return "pitched";
    case StaffGroup::PERCUSSION: return "percussion";
    case StaffGroup::TAB: return "tablature";
    }
    return "unknown";
}

// Untranslated subtype label ("Line break", "D.C. al Fine", "8va alta", ...).
std::string subtype(const EngravingItem* item) { return str(item->subtypeUserName().str); }

json text_item(const EngravingItem* item)
{
    json out;
    out["type"] = str(TConv::toXml(item->type()));
    out["tick"] = item->tick().ticks();
    out["track"] = static_cast<int>(item->track());
    // Dynamics omit the UI tooltip subtype label ("f (Forte)" vs "f (forte)");
    // the dynamic field below is the stable identity.
    if (!item->isDynamic()) {
        const std::string sub = subtype(item);
        if (!sub.empty()) {
            out["subtype"] = sub;
        }
    }
    // Dynamics render as SMuFL glyphs; their kind is recorded below instead.
    if (item->isTextBase() && !item->isDynamic()) {
        out["text"] = str(toTextBase(item)->plainText());
    }
    if (item->isTempoText()) {
        // Quarter-note BPM rounded like test_roundtrip.py rounds <tempo> (beats per second there).
        out["bpm"] = std::round(toTempoText(item)->tempo().val * 60.0 * 1e4) / 1e4;
    } else if (item->isDynamic()) {
        out["dynamic"] = str(TConv::toXml(toDynamic(item)->dynamicType()));
    } else if (item->isHarmony()) {
        out["harmony"] = str(toHarmony(item)->harmonyName());
    }
    return out;
}

// Measure-level elements (measure->el()): system-level items omit track when not
// meaningful (e.g. LayoutBreak track is unassigned -1 in 4.7.6 vs 0 in 5.0);
// staff-level items retain track to identify the staff.
json measure_element_json(const EngravingItem* item)
{
    json out = text_item(item);
    switch (item->type()) {
    case ElementType::LAYOUT_BREAK:
        // LayoutBreak is a system-level break (line/page/section); track is not
        // meaningful (-1 in 4.7.6 vs 0 in 5.0).
        out.erase("track");
        break;
    case ElementType::STAFFTYPE_CHANGE:
        // StaffTypeChange is staff-level; track identifies the affected staff.
        break;
    case ElementType::SPACER:
        // Spacer is staff-level; track identifies the affected staff.
        break;
    case ElementType::JUMP:
    case ElementType::MARKER:
        // Repeat jumps and markers are anchored to a staff (typically track 0).
        break;
    default:
        // Other system-level elements without a meaningful staff omit track.
        if (item->systemFlag() && !item->flag(ElementFlag::ON_STAFF)) {
            out.erase("track");
        }
        break;
    }
    return out;
}

json note_json(const Note* note)
{
    json out;
    out["pitch"] = note->pitch();
    out["tpc1"] = note->tpc1();
    out["tpc2"] = note->tpc2();
    out["head"] = str(TConv::toXml(note->headGroup()));
    out["headType"] = str(TConv::toXml(note->headType()));
    if (note->fret() >= 0 || note->string() >= 0) {
        out["fret"] = note->fret();
        out["string"] = note->string();
    }
    if (note->ghost()) {
        out["ghost"] = true;
    }
    if (!note->play()) {
        out["play"] = false;
    }
    if (const Tie* tie = note->tieFor()) {
        if (tie->type() == ElementType::LAISSEZ_VIB) {
            out["laissezVib"] = true;
        } else if (const Note* end = tie->endNote()) {
            out["tieTo"] = { end->tick().ticks(), static_cast<int>(end->track()), end->pitch() };
        } else {
            out["tieTo"] = nullptr; // partial tie (e.g. into a volta)
        }
    }
    json fingerings = json::array();
    json elements = json::array(); // symbols, bends, note-attached text, ...
    for (const EngravingItem* e : const_cast<Note*>(note)->el()) {
        if (e->isFingering()) {
            fingerings.push_back(str(toTextBase(e)->plainText()));
        } else if (!e->generated()) {
            json item = text_item(e);
            item.erase("tick");
            item.erase("track");
            elements.push_back(item);
        }
    }
    if (!fingerings.empty()) {
        out["fingerings"] = fingerings;
    }
    if (!elements.empty()) {
        out["elements"] = elements;
    }
    return out;
}

json chord_rest_json(const ChordRest* cr)
{
    json out;
    out["kind"] = cr->isChord() ? "chord" : cr->isMeasureRepeat() ? "measureRepeat" : "rest";
    out["tick"] = cr->tick().ticks();
    out["ticks"] = cr->isChord() && toChord(cr)->isGrace() ? 0 : cr->actualTicks().ticks();
    out["duration"] = str(TConv::toXml(cr->durationType().type()));
    if (cr->dots()) {
        out["dots"] = cr->dots();
    }
    if (const Tuplet* tuplet = cr->tuplet()) {
        out["tuplet"] = fraction(tuplet->ratio());
    }
    if (cr->isChord()) {
        const Chord* chord = toChord(cr);
        out["grace"] = grace_kind(chord->noteType());
        json notes = json::array();
        for (const Note* note : chord->notes()) {
            notes.push_back(note_json(note));
        }
        out["notes"] = notes;
        json articulations = json::array();
        for (const Articulation* a : chord->articulations()) {
            articulations.push_back(str(TConv::toXml(a->symId())));
        }
        if (!articulations.empty()) {
            out["articulations"] = articulations;
        }
        // Chord lines, arpeggios, tremolos, stems set by the user.
        json elements = json::array();
        for (const EngravingItem* e : const_cast<Chord*>(chord)->el()) {
            if (!e->generated()) {
                elements.push_back(str(TConv::toXml(e->type())) + ":" + subtype(e));
            }
        }
        if (chord->arpeggio()) {
            elements.push_back("Arpeggio:" + subtype(chord->arpeggio()));
        }
        if (chord->tremoloSingleChord()) {
            elements.push_back("TremoloSingleChord:" + subtype(chord->tremoloSingleChord()));
        }
        if (chord->tremoloTwoChord()) {
            elements.push_back("TremoloTwoChord:" + subtype(chord->tremoloTwoChord()));
        }
        if (!elements.empty()) {
            out["elements"] = elements;
        }
        if (chord->stemDirection() != DirectionV::AUTO) {
            out["stemDirection"] = str(TConv::toXml(chord->stemDirection()));
        }
    } else if (!toRest(cr)->visible()) {
        out["visible"] = false;
    }
    json lyrics = json::array();
    for (const Lyrics* l : cr->lyrics()) {
        lyrics.push_back({ l->verse(), str(l->plainText()) });
    }
    if (!lyrics.empty()) {
        out["lyrics"] = lyrics;
    }
    return out;
}

json spanner_json(const Spanner* sp)
{
    json out;
    out["type"] = str(TConv::toXml(sp->type()));
    out["tick"] = sp->tick().ticks();
    out["tick2"] = sp->tick2().ticks();
    out["track"] = static_cast<int>(sp->track());
    out["track2"] = static_cast<int>(sp->track2());
    const std::string sub = subtype(sp);
    if (!sub.empty()) {
        out["subtype"] = sub;
    }
    out["anchor"] = static_cast<int>(sp->anchor());
    if (sp->isVolta()) {
        out["endings"] = toVolta(sp)->endings();
    }
    if (sp->isTextLineBase()) {
        const std::string begin = str(toTextLineBase(sp)->beginText());
        if (!begin.empty()) {
            out["beginText"] = begin;
        }
    }
    return out;
}

json part_json(const Part* part)
{
    json out;
    out["id"] = part->id().toStdString();
    out["partName"] = plain(part->partName());
    out["longName"] = plain(part->longName());
    out["shortName"] = plain(part->shortName());
    out["instrumentId"] = str(part->instrumentId());
    out["show"] = part->show();
    const Interval transpose = part->instrument()->transpose();
    out["transpose"] = { transpose.chromatic, transpose.diatonic };
    json staves = json::array();
    for (const Staff* staff : part->staves()) {
        const StaffType* type = staff->staffType(Fraction(0, 1));
        staves.push_back({
            { "index", static_cast<int>(staff->idx()) },
            { "group", staff_group(type->group()) },
            { "lines", type->lines() },
            { "invisible", type->invisible() },
        });
    }
    out["staves"] = staves;
    return out;
}

// Annotation and spanner containers keep insertion order, which a save and
// reload does not preserve; order by position, then by content.
void sort_items(json& items)
{
    std::sort(items.begin(), items.end(), [](const json& a, const json& b) {
        const auto key = [](const json& v) { return std::make_tuple(v.value("tick", 0), v.value("track", 0), v.dump()); };
        return key(a) < key(b);
    });
}

// Musical content of one score (master or excerpt).
json score_body(Score* score)
{
    json out;
    json parts = json::array();
    for (const Part* part : score->parts()) {
        parts.push_back(part_json(part));
    }
    out["parts"] = parts;

    json measures = json::array();
    json signatures = json::array(); // clefs, key and time signatures per staff
    json events = json::array();     // [staff][voice] -> chord/rest list
    json annotations = json::array();
    json segment_elements = json::array();
    const size_t nstaves = score->nstaves();
    for (size_t s = 0; s < nstaves; ++s) {
        events.push_back(json::array({ json::array(), json::array(), json::array(), json::array() }));
    }

    for (const Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        json measure;
        measure["tick"] = m->tick().ticks();
        measure["ticks"] = m->ticks().ticks();
        measure["timesig"] = fraction(m->timesig());
        measure["startRepeat"] = m->repeatStart();
        measure["endRepeat"] = m->repeatEnd() ? m->repeatCount() : 0;
        if (m->flag(ElementFlag::EXCLUDE_FROM_NUMBERING)) {
            // 5.0 renamed the "irregular" measure flag (Pid::IRREGULAR) to this.
            measure["irregular"] = true;
        }
        json elements = json::array();
        for (const EngravingItem* e : const_cast<Measure*>(m)->el()) {
            if (!e->generated()) {
                elements.push_back(measure_element_json(e));
            }
        }
        if (!elements.empty()) {
            sort_items(elements);
            measure["elements"] = elements;
        }
        measures.push_back(measure);

        for (const Segment* seg = m->first(); seg; seg = seg->next()) {
            for (const EngravingItem* a : seg->annotations()) {
                if (!a->generated()) {
                    annotations.push_back(text_item(a));
                }
            }
            for (size_t track = 0; track < nstaves * VOICES; ++track) {
                const EngravingItem* e = seg->element(static_cast<track_idx_t>(track));
                if (!e || e->generated()) {
                    continue;
                }
                const size_t staff = track / VOICES;
                if (e->isChordRest()) {
                    const ChordRest* cr = toChordRest(e);
                    json& voice = events[staff][track % VOICES];
                    if (cr->isChord()) {
                        for (const Chord* g : toChord(cr)->graceNotesBefore()) {
                            voice.push_back(chord_rest_json(g));
                        }
                    }
                    voice.push_back(chord_rest_json(cr));
                    if (cr->isChord()) {
                        for (const Chord* g : toChord(cr)->graceNotesAfter()) {
                            voice.push_back(chord_rest_json(g));
                        }
                    }
                } else if (e->isClef()) {
                    signatures.push_back({ { "type", "Clef" }, { "tick", seg->tick().ticks() }, { "staff", staff },
                                           { "value", str(TConv::toXml(toClef(e)->clefType())) } });
                } else if (e->isKeySig()) {
                    const KeySig* k = toKeySig(e);
                    signatures.push_back({ { "type", "KeySig" }, { "tick", seg->tick().ticks() }, { "staff", staff },
                                           { "value", static_cast<int>(k->concertKey()) },
                                           { "key", static_cast<int>(k->key()) } });
                } else if (e->isTimeSig()) {
                    signatures.push_back({ { "type", "TimeSig" }, { "tick", seg->tick().ticks() }, { "staff", staff },
                                           { "value", fraction(toTimeSig(e)->sig()) } });
                } else {
                    // Breaths, user barlines, ambitus, ...
                    segment_elements.push_back(text_item(e));
                }
            }
        }
    }
    out["measures"] = measures;
    out["signatures"] = signatures;
    out["events"] = events;
    sort_items(annotations);
    out["annotations"] = annotations;
    out["segmentElements"] = segment_elements;

    json spanners = json::array();
    for (const auto& entry : score->spannerMap().map()) {
        const Spanner* sp = entry.second;
        if (!sp->generated()) {
            spanners.push_back(spanner_json(sp));
        }
    }
    sort_items(spanners);
    out["spanners"] = spanners;
    return out;
}

} // namespace

json semantic_dump(MasterScore* score)
{
    json out;
    out["schema"] = 1;
    json meta = json::object();
    for (const auto& [name, value] : score->metaTags()) {
        if (name != u"creationDate" && name != u"platform") {
            meta[str(name)] = str(value);
        }
    }
    out["metadata"] = meta;

    // 5.0 derives tempo from an automation curve; the flattened timeline is the
    // replacement for the 4.x TempoMap it replaced: one point per tempo marking,
    // per resampled gradual-change step and per pause, keyed by score tick.
    json tempo = json::array();
    for (const TempoTimePoint& point : score->tempoTimeline(false).points()) {
        tempo.push_back({ point.utick, std::round(point.bps * 60.0 * 1e4) / 1e4 });
    }
    out["tempoMap"] = tempo;

    score->setExpandRepeats(true);
    json playback = json::array();
    for (const RepeatSegment* rs : score->repeatList()) {
        playback.push_back({ rs->tick, rs->len(), rs->utick });
    }
    out["playbackOrder"] = playback;

    out.update(score_body(score));

    json excerpts = json::array();
    for (const Excerpt* excerpt : score->excerpts()) {
        json e;
        e["name"] = str(excerpt->name());
        json part_ids = json::array();
        for (const Part* p : excerpt->parts()) {
            part_ids.push_back(p->id().toStdString());
        }
        e["partIds"] = part_ids;
        if (Score* es = excerpt->excerptScore()) {
            e["score"] = score_body(es);
        }
        excerpts.push_back(e);
    }
    out["excerpts"] = excerpts;
    return out;
}

} // namespace music
