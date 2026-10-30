// SPDX-License-Identifier: GPL-3.0-only
// Bounded structural queries over the live engraving DOM.
#include "score_query.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/spannermap.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tie.h"
#include "engraving/infrastructure/eid.h"
#include "engraving/infrastructure/eidregister.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace music {
using namespace mu::engraving;
namespace {

constexpr size_t MAX_REGION_MEASURES = 128;
constexpr size_t MAX_NOTE_RESULTS = 4096;
constexpr size_t MAX_PROJECTION_MEASURES = 1024;
constexpr size_t MAX_PROJECTION_NOTES = 16384;
constexpr size_t MAX_PROJECTION_SPANS = 4096;
constexpr size_t MAX_PROJECTION_PARTS = 256;
constexpr size_t PROJECTION_BLOCK_MEASURES = 128;

std::vector<Measure*> standard_measures(Score* score)
{
    std::vector<Measure*> measures;
    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        measures.push_back(measure);
    }
    return measures;
}

std::string stable_eid_string(const EngravingObject* object)
{
    if (!object) throw std::runtime_error("Cannot assign an EID to a null score object");
    EID eid = object->eid();
    if (!eid.isValid()) eid = object->assignNewEID();
    if (!eid.isValid()) throw std::runtime_error("Failed to assign a stable score EID");
    return eid.toStdString();
}

std::vector<staff_idx_t> part_staff_indices(Score* score, Part* part)
{
    std::vector<staff_idx_t> result;
    if (!part) throw std::invalid_argument("Score region part is null");
    for (Staff* staff : part->staves()) {
        const staff_idx_t index = score->staffIdx(staff);
        if (index == muse::nidx) throw std::runtime_error("Part staff is not present in the score");
        result.push_back(index);
    }
    if (result.empty()) throw std::runtime_error("Part has no staves");
    return result;
}

Measure* enclosed_measure(EngravingItem* item)
{
    for (EngravingObject* object = item; object; object = object->ownershipParent()) {
        if (object->isMeasure()) return toMeasure(object);
    }
    return nullptr;
}

Part* item_part(EngravingItem* item)
{
    Staff* staff = item ? item->staff() : nullptr;
    return staff ? staff->part() : nullptr;
}

// Per-score data shared by every region captured from the same DOM state. The
// measure list and the spanner map are scanned once so a whole-score snapshot
// does not repeat that work for every 128-measure block.
struct QueryCapture
{
    std::vector<Measure*> measures;
    std::unordered_map<const Measure*, size_t> measure_indices;
    std::unordered_map<const Measure*, std::string> measure_ids;
    std::unordered_map<const Part*, std::unordered_set<const Chord*>> spanned_chords;
    std::vector<Spanner*> spanners;
};

QueryCapture make_query_capture(Score* score)
{
    QueryCapture capture;
    capture.measures = standard_measures(score);
    capture.measure_indices.reserve(capture.measures.size());
    for (size_t i = 0; i < capture.measures.size(); ++i)
        capture.measure_indices.emplace(capture.measures[i], i);

    // Chord-anchored slurs and other spanners are not exposed through Note's
    // spanner lists, so collect their chords per part in the same pass.
    std::unordered_set<const Spanner*> seen;
    for (const auto& entry : score->spannerMap().map()) {
        Spanner* spanner = entry.second;
        if (!spanner || !seen.insert(spanner).second) continue;
        capture.spanners.push_back(spanner);
        if (spanner->isTie()) continue;
        Part* part = item_part(spanner);
        if (!part) continue;
        auto& chords = capture.spanned_chords[part];
        const auto add_anchor_chord = [&](EngravingItem* anchor) {
            if (!anchor) return;
            if (anchor->isChord()) chords.insert(toChord(anchor));
            else if (anchor->isNote()) chords.insert(toNote(anchor)->chord());
        };
        add_anchor_chord(spanner->startElement());
        add_anchor_chord(spanner->endElement());
    }
    return capture;
}

std::string measure_id(QueryCapture& capture, Measure* measure)
{
    const auto found = capture.measure_ids.find(measure);
    if (found != capture.measure_ids.end()) return found->second;
    std::string id = stable_eid_string(measure);
    capture.measure_ids.emplace(measure, id);
    return id;
}

// Measure and segment anchors carry no meaningful voice; every other anchor
// must match the requested voice, when one was requested.
bool voice_matches(std::optional<unsigned> voice, const EngravingItem* anchor)
{
    if (!voice || !anchor || anchor->isMeasure() || anchor->isSegment()) return true;
    return static_cast<unsigned>(anchor->voice()) == *voice;
}

bool anchor_inside(const QueryCapture& capture, Part* part, size_t first, size_t last,
                   EngravingItem* anchor)
{
    if (!anchor) return false;
    // Note/chord/rest anchors must match the selected part.
    if (!anchor->isMeasure() && !anchor->isSegment()) {
        const Part* anchor_part = item_part(anchor);
        if (anchor_part && anchor_part != part) return false;
    }
    Measure* anchor_measure = enclosed_measure(anchor);
    const auto found = capture.measure_indices.find(anchor_measure);
    if (found == capture.measure_indices.end()) return false;
    return found->second >= first && found->second < last;
}

// Captures the notes of one part/measure window. note_limit caps the returned
// notes; the whole-score snapshot passes the remaining projection capacity so
// the public 4096-note region cap is not inherited by its blocks.
std::vector<NoteView> capture_region_notes(QueryCapture& capture, Score* score, Part* part,
                                           size_t part_index, size_t first, size_t last,
                                           const std::vector<unsigned>& voices,
                                           std::optional<unsigned> voice,
                                           std::optional<size_t> note_limit,
                                           bool detect_cross_region, bool& cross_region)
{
    std::vector<NoteView> notes;
    const std::vector<staff_idx_t> staff_indices = part_staff_indices(score, part);
    // An anchor in another voice is outside the requested voice window rather
    // than a reason to ignore the whole tie or spanner.
    const auto inside = [&](EngravingItem* anchor) {
        return voice_matches(voice, anchor) && anchor_inside(capture, part, first, last, anchor);
    };
    const auto spanned = capture.spanned_chords.find(part);
    const std::unordered_set<const Chord*>* spanned_chords =
        spanned == capture.spanned_chords.end() ? nullptr : &spanned->second;

    for (size_t index = first; index < last; ++index) {
        Measure* measure = capture.measures[index];
        for (Segment* segment = measure->first(SegmentType::ChordRest); segment;
             segment = segment->next(SegmentType::ChordRest)) {
            for (const staff_idx_t staff_index : staff_indices) {
                for (const unsigned current_voice : voices) {
                    EngravingItem* item = segment->element(staff2track(staff_index, current_voice));
                    if (!item || !item->isChord()) continue;
                    Chord* chord = toChord(item);
                    // Grace notes have no independent duration in this model and
                    // are explicitly outside the stable pitch-edit contract.
                    if (chord->isGrace()) continue;
                    if (voice && static_cast<unsigned>(chord->voice()) != *voice) continue;
                    for (Note* note : chord->notes()) {
                        if (!note) continue;
                        if (note_limit && notes.size() >= *note_limit) {
                            throw ScoreQueryLimit(
                                "Score query exceeds the note result bound");
                        }

                        Measure* note_measure = chord->measure();
                        size_t note_measure_index = index;
                        const auto measure_pos = capture.measure_indices.find(note_measure);
                        if (measure_pos != capture.measure_indices.end()) {
                            note_measure_index = measure_pos->second;
                        }
                        const std::string note_measure_id = measure_id(
                            capture, note_measure ? note_measure : measure);

                        const bool tied = note->tieFor() != nullptr || note->tieBack() != nullptr;
                        const bool is_spanned = !note->spannerFor().empty()
                                                || !note->spannerBack().empty()
                                                || (spanned_chords
                                                    && spanned_chords->count(chord) != 0);

                        if (detect_cross_region && tied) {
                            if (note->tieFor() && !inside(note->tieFor()->endNote())) {
                                cross_region = true;
                            }
                            if (note->tieBack() && !inside(note->tieBack()->startNote())) {
                                cross_region = true;
                            }
                        }
                        if (detect_cross_region && !cross_region && is_spanned) {
                            const auto crosses = [&](const std::vector<Spanner*>& spanners) {
                                for (Spanner* spanner : spanners) {
                                    if (!spanner) continue;
                                    EngravingItem* start = spanner->startElement();
                                    EngravingItem* end = spanner->endElement();
                                    if (!start && !end) continue;
                                    // Ignore a spanner only when neither anchor
                                    // belongs to this query; one anchor in
                                    // another voice lies outside the window.
                                    if (!voice_matches(voice, start) && !voice_matches(voice, end))
                                        continue;
                                    if (!inside(start) || !inside(end)) return true;
                                }
                                return false;
                            };
                            if (crosses(note->spannerFor()) || crosses(note->spannerBack()))
                                cross_region = true;
                        }

                        notes.push_back({ stable_eid_string(note), note_measure_id,
                                          part_index, chord->staffIdx(), note_measure_index,
                                          static_cast<unsigned>(chord->voice()), chord->tick().ticks(),
                                          chord->actualTicks().ticks(), note->pitch(), tied,
                                          is_spanned });
                    }
                }
            }
        }
    }
    return notes;
}
} // namespace

ScoreRegionView query_score_region(mu::engraving::MasterScore* score, const ScoreRegion& region)
{
    if (!score) throw std::invalid_argument("A master score is required");
    if (region.measure_count > MAX_REGION_MEASURES)
        throw std::invalid_argument("Score region exceeds the 128-measure query bound");
    if (region.voice && *region.voice >= VOICES)
        throw std::invalid_argument("Score region voice must be in 0..3");

    const std::vector<Part*>& parts = score->parts();
    if (region.part_index >= parts.size())
        throw std::invalid_argument("Score region part index is out of range");
    Part* part = parts[region.part_index];

    QueryCapture capture = make_query_capture(score);
    const std::vector<Measure*>& measures = capture.measures;
    if (region.first_measure > measures.size()
        || region.measure_count > measures.size() - region.first_measure) {
        throw std::invalid_argument("Score region measure range is out of bounds");
    }

    ScoreRegionView view;
    const size_t first = region.first_measure;
    const size_t last = first + region.measure_count;
    view.measures.reserve(region.measure_count);
    for (size_t index = first; index < last; ++index) {
        Measure* measure = measures[index];
        view.measures.push_back({ measure_id(capture, measure), index, measure->tick().ticks(),
                                  measure->ticks().ticks() });
    }

    std::vector<unsigned> voices;
    if (region.voice) {
        voices.push_back(*region.voice);
    } else {
        for (unsigned voice = 0; voice < static_cast<unsigned>(VOICES); ++voice)
            voices.push_back(voice);
    }

    // A tie or spanner is cross-region when either anchor lies outside the
    // requested part/measure/voice window.
    bool cross_region = false;
    if (last > first) {
        const int region_start = measures[first]->tick().ticks();
        const int region_end = measures[last - 1]->endTick().ticks();
        const auto inside = [&](EngravingItem* anchor) {
            return voice_matches(region.voice, anchor)
                && anchor_inside(capture, part, first, last, anchor);
        };
        for (Spanner* spanner : capture.spanners) {
            if (item_part(spanner) != part) continue;
            // Skip only when neither anchor is in the requested voice; an
            // anchor in another voice is outside the window, so such a spanner
            // still crosses it.
            if (!voice_matches(region.voice, spanner->startElement())
                && !voice_matches(region.voice, spanner->endElement())) {
                continue;
            }
            const bool start_inside = inside(spanner->startElement());
            const bool end_inside = inside(spanner->endElement());
            if (start_inside != end_inside) {
                cross_region = true;
            } else if (!start_inside && !end_inside && !spanner->isTie()) {
                const int spanner_start = spanner->tick().ticks();
                const int spanner_end = spanner_start + spanner->ticks().ticks();
                if (spanner_end > region_start && spanner_start < region_end)
                    cross_region = true;
            }
        }
    }

    view.notes = capture_region_notes(capture, score, part, region.part_index, first, last,
                                      voices, region.voice, MAX_NOTE_RESULTS, true, cross_region);
    view.has_cross_region_spanners = cross_region;
    return view;
}

ScoreReadProjection snapshot_score_projection(mu::engraving::MasterScore* score)
{
    if (!score) throw std::invalid_argument("A master score is required");
    ScoreReadProjection projection;
    projection.part_count = score->parts().size();
    const auto unavailable = [&](const char* reason) {
        ScoreReadProjection result;
        result.part_count = projection.part_count;
        result.unavailable_reason = reason;
        return result;
    };
    if (projection.part_count > MAX_PROJECTION_PARTS)
        return unavailable("Score exceeds 256-part snapshot limit");

    QueryCapture capture = make_query_capture(score);
    if (capture.measures.size() > MAX_PROJECTION_MEASURES)
        return unavailable("Score exceeds 1024-measure snapshot limit");
    // Check the spanner bound before capturing notes: an over-budget score is
    // unavailable regardless of how many notes it holds.
    if (capture.spanners.size() > MAX_PROJECTION_SPANS)
        return unavailable("Score exceeds 4096-spanner snapshot limit");

    projection.measures.reserve(capture.measures.size());
    for (size_t index = 0; index < capture.measures.size(); ++index) {
        Measure* measure = capture.measures[index];
        projection.measures.push_back({ measure_id(capture, measure), index,
                                        measure->tick().ticks(), measure->ticks().ticks() });
    }

    std::vector<unsigned> all_voices;
    for (unsigned voice = 0; voice < static_cast<unsigned>(VOICES); ++voice)
        all_voices.push_back(voice);

    for (size_t part = 0; part < projection.part_count; ++part) {
        for (size_t first = 0; first < capture.measures.size();
             first += PROJECTION_BLOCK_MEASURES) {
            const size_t count = std::min(PROJECTION_BLOCK_MEASURES,
                                          capture.measures.size() - first);
            const size_t remaining = MAX_PROJECTION_NOTES - projection.notes.size();
            bool unused_cross_region = false;
            std::vector<NoteView> notes;
            try {
                notes = capture_region_notes(capture, score, score->parts()[part], part, first,
                                             first + count, all_voices, std::nullopt, remaining,
                                             false, unused_cross_region);
            } catch (const ScoreQueryLimit&) {
                return unavailable("Score exceeds 16384-note snapshot limit");
            }
            projection.notes.insert(projection.notes.end(),
                                    std::make_move_iterator(notes.begin()),
                                    std::make_move_iterator(notes.end()));
        }
    }

    const auto part_index = [&](const Part* part) -> std::optional<size_t> {
        const auto& parts = score->parts();
        const auto it = std::find(parts.begin(), parts.end(), part);
        if (it == parts.end()) return {};
        return static_cast<size_t>(it-parts.begin());
    };
    const auto endpoint = [&](EngravingItem* item) {
        ScoreSpanEndpoint result;
        if (!item) return result;
        result.part_index = part_index(item_part(item));
        if (!item->isMeasure() && !item->isSegment()) result.voice = static_cast<unsigned>(item->voice());
        const auto it = capture.measure_indices.find(enclosed_measure(item));
        if (it != capture.measure_indices.end()) result.measure_index = it->second;
        return result;
    };
    std::unordered_set<const Spanner*> captured;
    const auto capture_span = [&](Spanner* span) {
        if (!span || !captured.insert(span).second) return true;
        if (projection.spans.size() == MAX_PROJECTION_SPANS) return false;
        projection.spans.push_back({part_index(item_part(span)), endpoint(span->startElement()),
                                    endpoint(span->endElement()), span->tick().ticks(), span->tick2().ticks(), span->isTie()});
        return true;
    };
    for (Spanner* span : capture.spanners) {
        if (!capture_span(span)) return unavailable("Score exceeds 4096-spanner snapshot limit");
    }
    // Ties are not guaranteed to be present in SpannerMap.
    for (auto* measure : capture.measures) {
        for (auto* segment = measure->first(SegmentType::ChordRest); segment;
             segment = segment->next(SegmentType::ChordRest)) {
            for (track_idx_t track = 0; track < score->ntracks(); ++track) {
                auto* item = segment->element(track);
                if (!item || !item->isChord()) continue;
                for (auto* note : toChord(item)->notes()) {
                    if (!capture_span(note->tieFor()) || !capture_span(note->tieBack()))
                        return unavailable("Score exceeds 4096-spanner snapshot limit");
                    for (auto* span : note->spannerFor())
                        if (!capture_span(span)) return unavailable("Score exceeds 4096-spanner snapshot limit");
                    for (auto* span : note->spannerBack())
                        if (!capture_span(span)) return unavailable("Score exceeds 4096-spanner snapshot limit");
                }
            }
        }
    }
    projection.complete = true;
    return projection;
}

ScoreRegionView query_score_projection(const ScoreReadProjection& projection, const ScoreRegion& region)
{
    if (!projection.complete) throw ScoreQueryLimit("Score projection unavailable: " + projection.unavailable_reason);
    if (region.part_index >= projection.part_count || region.measure_count > MAX_REGION_MEASURES
        || (region.voice && *region.voice >= VOICES)
        || region.first_measure > projection.measures.size()
        || region.measure_count > projection.measures.size()-region.first_measure)
        throw std::invalid_argument("Score projection region is out of bounds");
    ScoreRegionView result;
    const size_t first = region.first_measure, end = first+region.measure_count;
    result.measures.assign(projection.measures.begin()+first, projection.measures.begin()+end);
    for (const auto& note : projection.notes) {
        if (note.part_index != region.part_index || note.measure_index < first || note.measure_index >= end
            || (region.voice && note.voice != *region.voice)) continue;
        if (result.notes.size() == MAX_NOTE_RESULTS) throw ScoreQueryLimit("Score region query exceeds 4096 notes");
        result.notes.push_back(note);
    }
    const auto inside = [&](const ScoreSpanEndpoint& point) {
        return point.measure_index && *point.measure_index >= first && *point.measure_index < end
            && (!point.part_index || *point.part_index == region.part_index)
            && (!region.voice || !point.voice || *point.voice == *region.voice);
    };
    const auto span_voice_matches = [&](const ScoreSpanEndpoint& point) {
        return !region.voice || !point.voice || *point.voice == *region.voice;
    };
    if (first != end) {
        const int first_tick = projection.measures[first].tick;
        const int last_tick = projection.measures[end-1].tick+projection.measures[end-1].duration_ticks;
        for (const auto& span : projection.spans) {
            const bool belongs = (span.part_index && *span.part_index == region.part_index)
                || (span.start.part_index && *span.start.part_index == region.part_index)
                || (span.end.part_index && *span.end.part_index == region.part_index);
            if (!belongs) continue;
            // Skip a spanner only when neither anchor belongs to the requested
            // voice; an anchor in another voice counts as outside the window.
            if (!span_voice_matches(span.start) && !span_voice_matches(span.end)) continue;
            const bool start_inside = inside(span.start), end_inside = inside(span.end);
            if (start_inside != end_inside || (!start_inside && !end_inside && !span.tie
                && span.last_tick > first_tick && span.first_tick < last_tick)) {
                result.has_cross_region_spanners = true;
                break;
            }
        }
    }
    return result;
}
} // namespace music
