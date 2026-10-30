// SPDX-License-Identifier: GPL-3.0-only
// Native structural score queries for the persistent engraving DOM.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

namespace mu::engraving {
class MasterScore;
}

namespace music {

// Region measure indices use the standard score order (Score::firstMeasure),
// never the multi-measure-rest display order. part_index refers to
// MasterScore::parts() and staff_index is the score-global staff index.
//
// Durations are integer engraving ticks: MusicXML divisions are normalised by
// the engine to Constants::DIVISION == 480 ticks per quarter note. All staves
// of the selected part are included; voice, when present, is 0..3.
struct ScoreRegion
{
    size_t part_index = 0;
    size_t first_measure = 0;
    size_t measure_count = 0;
    std::optional<unsigned> voice;
};

// id is a stable EID assigned once on the owner thread. tied reports a tie
// anchor on either side. spanned reports a non-tie spanner anchored to the
// note or its chord. Grace notes are deliberately excluded from query results.
struct NoteView
{
    std::string id;
    std::string measure_id;
    size_t part_index = 0;
    size_t staff_index = 0;
    size_t measure_index = 0;
    unsigned voice = 0;
    int tick = 0;
    int duration_ticks = 0;
    int pitch = 0;
    bool tied = false;
    bool spanned = false;
};

struct MeasureView
{
    std::string id;
    size_t index = 0;
    int tick = 0;
    int duration_ticks = 0;
};

struct ScoreRegionView
{
    std::vector<NoteView> notes;
    std::vector<MeasureView> measures;
    // True when a tie or spanner crosses the requested part/measure window.
    bool has_cross_region_spanners = false;
};

class ScoreQueryLimit : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ScoreSpanEndpoint {
    std::optional<size_t> part_index;
    std::optional<size_t> measure_index;
    std::optional<unsigned> voice;
};
struct ScoreSpanView {
    std::optional<size_t> part_index;
    ScoreSpanEndpoint start, end;
    int first_tick = 0;
    int last_tick = 0;
    bool tie = false;
};
struct ScoreReadProjection {
    bool complete = false;
    std::string unavailable_reason;
    size_t part_count = 0;
    std::vector<MeasureView> measures;
    std::vector<NoteView> notes;
    std::vector<ScoreSpanView> spans;
};

// Owner-thread capture, with no borrowed pointers in the result. Bounds are
// 1024 measures, 16384 notes, 4096 spanners and 256 parts for the entire score;
// the 4096-note per-region query cap does not apply to this capture. An
// over-limit score returns complete=false, an explicit reason and no partial
// musical data. Once captured, querying this value never accesses the engine.
ScoreReadProjection snapshot_score_projection(mu::engraving::MasterScore*);
ScoreRegionView query_score_projection(const ScoreReadProjection&, const ScoreRegion&);

// Requires a valid MasterScore and execution on its owner thread. The query
// assigns EIDs to returned notes and measures when they do not have one yet,
// so it mutates the master score's EID registry even though musical content is
// left untouched. Bounded to 128 measures and 4096 notes; a larger request or
// result throws. Bad regions throw std::invalid_argument.
ScoreRegionView query_score_region(mu::engraving::MasterScore* score, const ScoreRegion& region);

} // namespace music
