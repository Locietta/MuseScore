#include "runtime/noqt_runtime.h"
#include "score/score_edit.h"
#include "score/score_excerpts.h"
#include "score/score_query.h"

#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/part.h"
#include "engraving/editing/transaction/undostack.h"
#include "engraving/engravingproject.h"

#include <cstddef>
#include <iostream>
#include <future>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using music::NoteView;
using music::ScoreRegion;
using music::ScoreRegionView;

void expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

ScoreRegion make_region(size_t part, size_t first, size_t count,
                        std::optional<unsigned> voice = std::nullopt)
{
    ScoreRegion region;
    region.part_index = part;
    region.first_measure = first;
    region.measure_count = count;
    region.voice = voice;
    return region;
}

const NoteView* find_note(const ScoreRegionView& view, size_t measure, int pitch)
{
    for (const NoteView& note : view.notes) {
        if (note.measure_index == measure && note.pitch == pitch) return &note;
    }
    return nullptr;
}

std::map<std::string, int> pitch_map(const ScoreRegionView& view)
{
    std::map<std::string, int> result;
    for (const NoteView& note : view.notes) result.emplace(note.id, note.pitch);
    return result;
}

bool has_scope(const std::vector<music::ScoreEditScope>& scopes, size_t part, size_t measure,
               bool whole_part)
{
    for (const music::ScoreEditScope& scope : scopes) {
        if (scope.part_index != part || scope.whole_part != whole_part) continue;
        if (whole_part) return true;
        if (measure >= scope.first_measure
            && measure < scope.first_measure + scope.measure_count) {
            return true;
        }
    }
    return false;
}

bool same_scopes(const std::vector<music::ScoreEditScope>& left,
                 const std::vector<music::ScoreEditScope>& right)
{
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].part_index != right[i].part_index
            || left[i].first_measure != right[i].first_measure
            || left[i].measure_count != right[i].measure_count
            || left[i].whole_part != right[i].whole_part) {
            return false;
        }
    }
    return true;
}

void expect_same_ids(const ScoreRegionView& before, const ScoreRegionView& after)
{
    expect(before.notes.size() == after.notes.size(), "Query note count changed");
    expect(before.measures.size() == after.measures.size(), "Query measure count changed");
    for (size_t i = 0; i < before.measures.size(); ++i) {
        expect(before.measures[i].id == after.measures[i].id, "Measure id did not stay stable");
    }
    for (size_t i = 0; i < before.notes.size(); ++i) {
        expect(before.notes[i].id == after.notes[i].id, "Note id did not stay stable");
        expect(before.notes[i].measure_id == after.notes[i].measure_id, "Note measure id changed");
    }
}

// A dense score whose first (and only) 128-measure projection block holds
// more than the public 4096-note region cap but fewer than the 16384-note
// whole-projection bound: 128 measures x 33-note chords = 4224 notes.
std::string dense_projection_score()
{
    static const char* kSteps[] = { "C", "D", "E", "F", "G", "A", "B" };
    std::string xml = R"(<score-partwise version="4.0">
  <part-list>
    <score-part id="P1"><part-name>Dense</part-name></score-part>
  </part-list>
  <part id="P1">
)";
    for (size_t measure = 0; measure < 128; ++measure) {
        xml += "    <measure number=\"" + std::to_string(measure + 1) + "\">\n";
        if (measure == 0) {
            xml += "      <attributes><divisions>1</divisions>"
                   "<time><beats>4</beats><beat-type>4</beat-type></time>"
                   "<clef><sign>G</sign><line>2</line></clef></attributes>\n";
        }
        for (size_t note = 0; note < 33; ++note) {
            xml += "      <note>";
            if (note != 0) xml += "<chord/>";
            xml += "<pitch><step>";
            xml += kSteps[note % 7];
            xml += "</step><octave>";
            xml += std::to_string(2 + note / 7);
            xml += "</octave></pitch><duration>4</duration><type>whole</type></note>\n";
        }
        xml += "    </measure>\n";
    }
    xml += "  </part>\n</score-partwise>";
    return xml;
}

// The slur lives entirely in MusicXML voice 2 (MuseScore voice 1), while the
// outer measures carry voice 1 (MuseScore voice 0) notes.
const char* kVoiceSpannerScore = R"xml(<score-partwise version="4.0">
  <part-list>
    <score-part id="P1"><part-name>Voices</part-name></score-part>
  </part-list>
  <part id="P1">
    <measure number="1">
      <attributes>
        <divisions>1</divisions>
        <time><beats>4</beats><beat-type>4</beat-type></time>
        <clef><sign>G</sign><line>2</line></clef>
      </attributes>
      <note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><type>whole</type></note>
    </measure>
    <measure number="2">
      <note><pitch><step>E</step><octave>4</octave></pitch><duration>4</duration>
        <voice>2</voice><type>whole</type>
        <notations><slur type="start" number="1"/></notations></note>
    </measure>
    <measure number="3">
      <note><pitch><step>E</step><octave>4</octave></pitch><duration>4</duration>
        <voice>2</voice><type>whole</type>
        <notations><slur type="stop" number="1"/></notations></note>
    </measure>
    <measure number="4">
      <note><pitch><step>G</step><octave>4</octave></pitch><duration>4</duration><type>whole</type></note>
    </measure>
  </part>
</score-partwise>)xml";

// Measure 1 carries voice 1 (MuseScore voice 0) notes plus a slur fully
// contained in it in MusicXML voice 2 (MuseScore voice 1). A second voice-2
// slur crosses into measure 2, so a measure-1 voice-1 window cuts it.
const char* kVoiceFilterScore = R"xml(<score-partwise version="4.0">
  <part-list>
    <score-part id="P1"><part-name>Voice filter</part-name></score-part>
  </part-list>
  <part id="P1">
    <measure number="1">
      <attributes>
        <divisions>1</divisions>
        <time><beats>4</beats><beat-type>4</beat-type></time>
        <clef><sign>G</sign><line>2</line></clef>
      </attributes>
      <note><pitch><step>C</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>
      <note><pitch><step>D</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>
      <note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>
      <note><pitch><step>F</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>
      <backup><duration>4</duration></backup>
      <note><pitch><step>G</step><octave>4</octave></pitch><duration>2</duration>
        <voice>2</voice><type>half</type>
        <notations><slur type="start" number="1"/></notations></note>
      <note><pitch><step>A</step><octave>4</octave></pitch><duration>2</duration>
        <voice>2</voice><type>half</type>
        <notations><slur type="stop" number="1"/><slur type="start" number="2"/></notations></note>
    </measure>
    <measure number="2">
      <note><pitch><step>C</step><octave>5</octave></pitch><duration>4</duration><type>whole</type></note>
      <backup><duration>4</duration></backup>
      <note><pitch><step>B</step><octave>4</octave></pitch><duration>4</duration>
        <voice>2</voice><type>whole</type>
        <notations><slur type="stop" number="2"/></notations></note>
    </measure>
  </part>
</score-partwise>)xml";

// A slur starts on a voice-1 (MuseScore voice 0) note in measure 1 and stops
// on a voice-2 (MuseScore voice 1) note in measure 2. One anchor belongs to the
// queried voice, so a voice-0 window that cuts it must report a crossing.
const char* kCrossVoiceSpanScore = R"xml(<score-partwise version="4.0">
  <part-list>
    <score-part id="P1"><part-name>Cross voice</part-name></score-part>
  </part-list>
  <part id="P1">
    <measure number="1">
      <attributes>
        <divisions>1</divisions>
        <time><beats>4</beats><beat-type>4</beat-type></time>
        <clef><sign>G</sign><line>2</line></clef>
      </attributes>
      <note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><type>whole</type>
        <notations><slur type="start" number="1"/></notations></note>
    </measure>
    <measure number="2">
      <note><pitch><step>E</step><octave>4</octave></pitch><duration>4</duration>
        <voice>2</voice><type>whole</type>
        <notations><slur type="stop" number="1"/></notations></note>
    </measure>
  </part>
</score-partwise>)xml";

const char* kTwoPartScore = R"xml(<score-partwise version="4.0">
  <part-list>
    <score-part id="P1"><part-name>Piano</part-name></score-part>
    <score-part id="P2"><part-name>Clarinet</part-name></score-part>
  </part-list>
  <part id="P1">
    <measure number="1">
      <attributes>
        <divisions>1</divisions>
        <time><beats>4</beats><beat-type>4</beat-type></time>
        <clef><sign>G</sign><line>2</line></clef>
      </attributes>
      <note><pitch><step>C</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>
      <note><pitch><step>D</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>
      <note><pitch><step>E</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>
      <note><pitch><step>F</step><octave>4</octave></pitch><duration>1</duration><type>quarter</type></note>
    </measure>
    <measure number="2">
      <note><pitch><step>G</step><octave>4</octave></pitch><duration>4</duration><type>whole</type></note>
    </measure>
    <measure number="3">
      <note><pitch><step>A</step><octave>4</octave></pitch><duration>2</duration><type>half</type></note>
      <note><pitch><step>B</step><octave>4</octave></pitch><duration>2</duration><type>half</type></note>
    </measure>
    <measure number="4">
      <note><pitch><step>C</step><octave>5</octave></pitch><duration>4</duration><type>whole</type></note>
    </measure>
  </part>
  <part id="P2">
    <measure number="1">
      <attributes>
        <divisions>1</divisions>
        <time><beats>4</beats><beat-type>4</beat-type></time>
        <clef><sign>G</sign><line>2</line></clef>
        <transpose><diatonic>-1</diatonic><chromatic>-2</chromatic></transpose>
      </attributes>
      <note><pitch><step>C</step><octave>5</octave></pitch><duration>4</duration><type>whole</type></note>
    </measure>
    <measure number="2">
      <note><pitch><step>D</step><octave>5</octave></pitch><duration>4</duration><type>whole</type></note>
    </measure>
    <measure number="3">
      <note><pitch><step>E</step><octave>5</octave></pitch><duration>4</duration>
        <tie type="start"/><type>whole</type>
        <notations><tied type="start"/></notations></note>
    </measure>
    <measure number="4">
      <note><pitch><step>E</step><octave>5</octave></pitch><duration>4</duration>
        <tie type="stop"/><type>whole</type>
        <notations><tied type="stop"/></notations></note>
    </measure>
  </part>
</score-partwise>)xml";
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) throw std::invalid_argument("Usage: score_edit_test ASSETS");
        music::initialize_native_runtime(argv[1]);
        const auto project = music::load_native_score(kTwoPartScore, "musicxml");
        auto* score = project->masterScore();
        expect(score->parts().size() == 2, "Fixture did not import two parts");

        size_t total_measures = 0;
        for (mu::engraving::Measure* measure = score->firstMeasure(); measure;
             measure = measure->nextMeasure()) {
            ++total_measures;
        }
        expect(total_measures == 4, "Fixture did not import four measures");

        const ScoreRegionView piano_before = music::query_score_region(score, make_region(0, 0, total_measures));
        const ScoreRegionView clarinet_before = music::query_score_region(score, make_region(1, 0, total_measures));
        expect(piano_before.measures.size() == 4 && clarinet_before.measures.size() == 4,
               "Region measure views are incomplete");
        expect(piano_before.notes.size() == 8, "Unexpected piano note count");
        expect(clarinet_before.notes.size() == 4, "Unexpected clarinet note count");
        expect(piano_before.measures[0].duration_ticks == 1920, "Measure ticks are not 480/quarter");
        const NoteView* piano_first = find_note(piano_before, 0, 60);
        expect(piano_first && piano_first->duration_ticks == 480 && piano_first->tick == 0,
               "Quarter-note tick units are wrong or note C4 is missing");

        const auto initial_projection = music::snapshot_score_projection(score);
        expect(initial_projection.complete, "Small score projection was unavailable");
        const auto detached_region = std::async(std::launch::async, [&] {
            return music::query_score_projection(initial_projection, make_region(0, 0, total_measures));
        }).get();
        expect_same_ids(piano_before, detached_region);
        expect(pitch_map(piano_before) == pitch_map(detached_region), "Detached projection changed pitches");
        expect(music::query_score_projection(initial_projection, make_region(1, 2, 1)).has_cross_region_spanners,
               "Detached projection lost cross-region tie");
        expect(!music::query_score_projection(initial_projection, make_region(1, 0, total_measures)).has_cross_region_spanners,
               "Detached projection misclassified a contained tie");
        const ScoreRegionView piano_repeat = music::query_score_region(score, make_region(0, 0, total_measures));
        const ScoreRegionView clarinet_repeat = music::query_score_region(score, make_region(1, 0, total_measures));
        expect_same_ids(piano_before, piano_repeat);
        expect_same_ids(clarinet_before, clarinet_repeat);

        // The clarinet is written a major second above sounding pitch, so the
        // written D5 in measure 2 must be stored as sounding C5.
        const NoteView* clarinet_target = find_note(clarinet_before, 1, 72);
        expect(clarinet_target != nullptr, "Transposing instrument pitch was not converted");

        const NoteView* tie_start = find_note(clarinet_before, 2, 74);
        const NoteView* tie_stop = find_note(clarinet_before, 3, 74);
        expect(tie_start && tie_start->tied, "Explicit tie start was not reported");
        expect(tie_stop && tie_stop->tied, "Explicit tie stop was not reported");
        const ScoreRegionView tie_window = music::query_score_region(score, make_region(1, 2, 1));
        expect(tie_window.has_cross_region_spanners, "Cross-region tie was not detected");
        expect(!clarinet_before.has_cross_region_spanners, "Contained tie was reported as cross-region");

        bool rejected = false;
        try { music::query_score_region(nullptr, make_region(0, 0, 1)); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Null score query was accepted");

        rejected = false;
        try { music::query_score_region(score, make_region(2, 0, 1)); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Out-of-range part query was accepted");

        rejected = false;
        try { music::query_score_region(score, make_region(0, 0, 129)); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Over-128-measure query was accepted");

        rejected = false;
        try { music::query_score_region(score, make_region(0, total_measures + 1, 0)); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Out-of-range measure query was accepted");

        rejected = false;
        try { music::query_score_region(score, make_region(0, 3, 2)); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Measure range past the score was accepted");

        rejected = false;
        try { music::query_score_region(score, make_region(0, 0, 1, 4)); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Out-of-range voice query was accepted");

        expect(piano_first != nullptr, "Piano target C4 is missing");
        const std::string piano_target_id = piano_first->id;
        const std::string clarinet_target_id = clarinet_target->id;

        music::NativePitchEdit edit;
        edit.changes.push_back({ piano_target_id, 61 });
        edit.changes.push_back({ clarinet_target_id, 73 });
        const auto prepared = music::inspect_score_pitch_edit(score, edit);
        expect(prepared.changed_notes == 2, "Preflight did not predict changes");
        expect(pitch_map(music::query_score_region(score, make_region(0, 0, total_measures)))
               == pitch_map(piano_before), "Preflight mutated pitches");
        const music::NativePitchResult result = music::apply_score_pitch_edit(score, edit);
        expect(result.changed_notes == 2, "Disjoint batch did not change two notes");
        expect(pitch_map(music::query_score_projection(initial_projection, make_region(0, 0, total_measures)))
               == pitch_map(piano_before), "Live edit changed an earlier immutable projection");
        expect(result.requires_stable_structure, "Pitch result did not require stable structure");
        expect(has_scope(result.write_scopes, 0, 0, false), "Piano write scope is missing");
        expect(has_scope(result.write_scopes, 1, 1, false), "Clarinet write scope is missing");
        expect(same_scopes(result.read_scopes, result.write_scopes),
               "Read and write scopes diverged for a plain pitch edit");

        const ScoreRegionView piano_after = music::query_score_region(score, make_region(0, 0, total_measures));
        const ScoreRegionView clarinet_after = music::query_score_region(score, make_region(1, 0, total_measures));
        const NoteView* piano_changed = find_note(piano_after, 0, 61);
        const NoteView* clarinet_changed = find_note(clarinet_after, 1, 73);
        expect(piano_changed && piano_changed->id == piano_target_id, "Piano pitch edit or id failed");
        expect(clarinet_changed && clarinet_changed->id == clarinet_target_id,
               "Clarinet pitch edit or id failed");
        expect_same_ids(piano_before, piano_after);
        expect_same_ids(clarinet_before, clarinet_after);

        const std::map<std::string, int> piano_original = pitch_map(piano_before);
        const std::map<std::string, int> clarinet_original = pitch_map(clarinet_before);
        for (const auto& [id, pitch] : pitch_map(piano_after)) {
            if (id == piano_target_id) continue;
            const auto original = piano_original.find(id);
            expect(original != piano_original.end() && original->second == pitch,
                   "A non-target piano note changed");
        }
        for (const auto& [id, pitch] : pitch_map(clarinet_after)) {
            if (id == clarinet_target_id) continue;
            const auto original = clarinet_original.find(id);
            expect(original != clarinet_original.end() && original->second == pitch,
                   "A non-target clarinet note changed");
        }

        expect(score->undoStack()->canUndo(), "Pitch edit did not create an undo step");
        score->undoRedo(true, nullptr);
        expect(find_note(music::query_score_region(score, make_region(0, 0, total_measures)), 0, 60)
               && find_note(music::query_score_region(score, make_region(1, 0, total_measures)), 1, 72),
               "Undo did not restore the original pitches");
        score->undoRedo(false, nullptr);
        expect(find_note(music::query_score_region(score, make_region(0, 0, total_measures)), 0, 61)
               && find_note(music::query_score_region(score, make_region(1, 0, total_measures)), 1, 73),
               "Redo did not restore the edited pitches");

        const NoteView* piano_spare = find_note(piano_before, 0, 64);
        expect(piano_spare != nullptr, "Spare piano note E4 is missing");
        const std::string spare_id = piano_spare->id;

        rejected = false;
        try {
            music::NativePitchEdit invalid_second;
            invalid_second.changes.push_back({ spare_id, 65 });
            invalid_second.changes.push_back({ "not-a-valid-note-id", 66 });
            music::apply_score_pitch_edit(score, invalid_second);
        } catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Malformed second note id was accepted");
        expect(find_note(music::query_score_region(score, make_region(0, 0, total_measures)), 0, 64)
               != nullptr, "Invalid second change modified the first note");

        rejected = false;
        try {
            music::NativePitchEdit bad_pitch;
            bad_pitch.changes.push_back({ spare_id, 128 });
            music::apply_score_pitch_edit(score, bad_pitch);
        } catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Out-of-range pitch was accepted");

        rejected = false;
        try {
            music::NativePitchEdit duplicate;
            duplicate.changes.push_back({ spare_id, 65 });
            duplicate.changes.push_back({ spare_id, 66 });
            music::apply_score_pitch_edit(score, duplicate);
        } catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Duplicate note id was accepted");

        rejected = false;
        try {
            music::NativePitchEdit too_many;
            for (size_t i = 0; i < 257; ++i) too_many.changes.push_back({ spare_id, 65 });
            music::apply_score_pitch_edit(score, too_many);
        } catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Over-256-change batch was accepted");

        rejected = false;
        try { music::apply_score_pitch_edit(score, music::NativePitchEdit{}); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Empty pitch edit was accepted");

        score->undoStack()->setLocked(true);
        rejected = false;
        try {
            music::NativePitchEdit locked;
            locked.changes.push_back({ spare_id, 65 });
            music::apply_score_pitch_edit(score, locked);
        } catch (const std::runtime_error&) { rejected = true; }
        score->undoStack()->setLocked(false);
        expect(rejected, "Locked undo stack was accepted");

        score->setReadOnly(true);
        rejected = false;
        try {
            music::NativePitchEdit read_only;
            read_only.changes.push_back({ spare_id, 65 });
            music::apply_score_pitch_edit(score, read_only);
        } catch (const std::runtime_error&) { rejected = true; }
        score->setReadOnly(false);
        expect(rejected, "Read-only score was accepted");

        rejected = false;
        try {
            music::NativePitchEdit mixed;
            mixed.changes.push_back({ spare_id, 65 });
            mixed.changes.push_back({ tie_start->id, 75 });
            music::apply_score_pitch_edit(score, mixed);
        } catch (const std::runtime_error&) { rejected = true; }
        expect(rejected, "Tied note pitch edit was accepted");
        expect(find_note(music::query_score_region(score, make_region(0, 0, total_measures)), 0, 64)
               != nullptr, "Tie rejection modified the valid first change");
        expect(find_note(music::query_score_region(score, make_region(1, 0, total_measures)), 2, 74)
               && find_note(music::query_score_region(score, make_region(1, 0, total_measures)), 3, 74),
               "Tie rejection changed tied notes");

        // Round-trip the edited master score through MusicXML: the persisted
        // sounding pitches and ties must match the live DOM semantics.
        const std::string exported = music::export_native_score(project, "musicxml", 0, "");
        expect(exported.find("<score-partwise") != std::string::npos, "MusicXML export was empty");
        const auto reimported = music::load_native_score(exported, "musicxml");
        auto* reimported_score = reimported->masterScore();
        const ScoreRegionView piano_export = music::query_score_region(
            reimported_score, make_region(0, 0, total_measures));
        const ScoreRegionView clarinet_export = music::query_score_region(
            reimported_score, make_region(1, 0, total_measures));
        expect(find_note(piano_export, 0, 61) != nullptr,
               "Exported MusicXML lost the edited piano pitch");
        expect(find_note(clarinet_export, 1, 73) != nullptr,
               "Exported MusicXML lost the edited transposing-instrument pitch");
        const NoteView* reimported_tie = find_note(clarinet_export, 2, 74);
        expect(reimported_tie && reimported_tie->tied,
               "Exported MusicXML lost the tie semantics");

        const size_t no_op_index = score->undoStack()->currentIndex();
        const auto no_op = music::apply_score_pitch_edit(score, {{{piano_target_id, 61}}});
        expect(no_op.changed_notes == 0 && score->undoStack()->currentIndex() == no_op_index,
               "No-op pitch request changed undo history");

#ifdef MUSIC_CORE_TESTING
        // Failure while a redo branch exists must preserve both score and history.
        score->undoRedo(true, nullptr);
        const auto before_failure = music::export_native_score(project, "mscx", 0, "");
        const auto failure_index = score->undoStack()->currentIndex();
        const auto failure_size = score->undoStack()->size();
        expect(score->undoStack()->canRedo(), "Failure fixture has no redo branch");
        music::set_native_edit_test_fault(music::NativeEditTestFault::engine_error_after_first);
        bool engine_failure = false;
        try { music::apply_score_pitch_edit(score, {{{spare_id, 65}, {piano_target_id, 62}}}); }
        catch (const music::NativeEditFailure& e) { engine_failure = !e.poisoned(); }
        expect(engine_failure, "Injected engine failure was not cleanly rolled back");
        expect(!score->undoStack()->hasActiveTransaction()
               && score->undoStack()->currentIndex() == failure_index
               && score->undoStack()->size() == failure_size && score->undoStack()->canRedo(),
               "Engine rollback damaged the previous redo branch");
        expect(music::export_native_score(project, "mscx", 0, "") == before_failure,
               "Injected engine failure changed serialized score");
        mu::engraving::MScore::setError(mu::engraving::MsError::MS_NO_ERROR);
        score->undoRedo(false, nullptr);

        const auto poison_project = music::load_native_score(kTwoPartScore, "musicxml");
        auto* poison_score = poison_project->masterScore();
        const auto poison_view = music::query_score_region(poison_score, make_region(0, 0, 1));
        music::set_native_edit_test_fault(music::NativeEditTestFault::exception_after_first);
        bool poisoned = false;
        try { music::apply_score_pitch_edit(poison_score, {{{poison_view.notes[0].id, 63}}}); }
        catch (const music::NativeEditFailure& e) { poisoned = e.poisoned(); }
        expect(poisoned, "Unexpected transaction exception was not marked poisoned");
        // Do not issue another operation on this document: core must retire it.
#endif

        const auto before_cap = music::export_native_score(project, "mscx", 0, "");
        const bool was_clean = score->undoStack()->isClean();
        expect(music::cap_native_edit_history(score, 0), "Undo history cap failed");
        expect(score->undoStack()->size() == 0 && score->undoStack()->currentIndex() == 0
               && !score->undoStack()->canUndo() && !score->undoStack()->canRedo(),
               "Undo history was not retired");
        expect(score->undoStack()->isClean() == was_clean, "History retirement changed dirty state");
        expect(music::export_native_score(project, "mscx", 0, "") == before_cap,
               "Undo history retirement modified score contents");

        // An unopened headless excerpt must follow linked master edits and be
        // fully updated by endCmd. Exercise its exported written transposition,
        // rather than only checking the master note's in-memory pitch.
        const auto excerpt_project = music::load_native_score(kTwoPartScore, "musicxml");
        auto* excerpt_master = excerpt_project->masterScore();
        expect(music::generate_score_excerpts(excerpt_master), "Excerpt fixture generation failed");
        expect(music::list_score_excerpts(excerpt_master).size() == 2,
               "Excerpt fixture did not produce two parts");
        auto* clarinet_excerpt = music::select_score(excerpt_master, 1);
        expect(!clarinet_excerpt->isOpen(), "Excerpt fixture unexpectedly has an open view");
        const auto excerpt_region = music::query_score_region(excerpt_master, make_region(1, 1, 1));
        expect(excerpt_region.notes.size() == 1 && excerpt_region.notes[0].pitch == 72,
               "Excerpt fixture target pitch is wrong");
        const auto excerpt_change = music::apply_score_pitch_edit(
            excerpt_master, {{{excerpt_region.notes[0].id, 73}}});
        expect(has_scope(excerpt_change.write_scopes, 1, 1, true),
               "Linked excerpt edit did not expand its dependency to the whole part");
        music::ExportOptions excerpt_options;
        excerpt_options.excerpt_id = 1;
        const auto excerpt_xml = music::export_native_score(
            excerpt_project, "musicxml", excerpt_options, "");
        const auto excerpt_reopened = music::load_native_score(excerpt_xml, "musicxml");
        expect(excerpt_reopened->masterScore()->parts().size() == 1,
               "Excerpt export contained other parts");
        const auto excerpt_export_region = music::query_score_region(
            excerpt_reopened->masterScore(), make_region(0, 1, 1));
        expect(excerpt_export_region.notes.size() == 1 && excerpt_export_region.notes[0].pitch == 73,
               "Unopened excerpt export lost the linked pitch edit or written transposition");

        // A whole-score projection must not inherit the public 4096-note
        // per-region cap: 4224 notes in one 128-measure block are inside the
        // documented 16384-note projection bound.
        const auto dense_project = music::load_native_score(dense_projection_score(), "musicxml");
        auto* dense_score = dense_project->masterScore();
        const auto dense_projection = music::snapshot_score_projection(dense_score);
        expect(dense_projection.complete, "Dense score projection was unavailable");
        expect(dense_projection.notes.size() == 128 * 33,
               "Dense projection did not capture every note");
        expect(dense_projection.notes.size() > 4096,
               "Dense fixture does not exceed the public region cap");
        bool dense_region_limited = false;
        try {
            music::query_score_region(dense_score, make_region(0, 0, 128));
        } catch (const music::ScoreQueryLimit&) { dense_region_limited = true; }
        expect(dense_region_limited, "Public region query no longer enforces the 4096-note bound");
        bool dense_projection_limited = false;
        try {
            music::query_score_projection(dense_projection, make_region(0, 0, 128));
        } catch (const music::ScoreQueryLimit&) { dense_projection_limited = true; }
        expect(dense_projection_limited,
               "Projected region query no longer enforces the 4096-note bound");
        expect(music::query_score_projection(dense_projection, make_region(0, 0, 1)).notes.size() == 33,
               "Dense projection region did not return its notes");

        // A spanner anchored only to another voice is outside the requested
        // voice window and must not set has_cross_region_spanners.
        const auto voice_project = music::load_native_score(kVoiceSpannerScore, "musicxml");
        auto* voice_score = voice_project->masterScore();
        const ScoreRegionView other_voice = music::query_score_region(
            voice_score, make_region(0, 0, 4, 0));
        expect(!other_voice.has_cross_region_spanners,
               "Another-voice spanner was reported as cross-region");
        const ScoreRegionView matching_voice = music::query_score_region(
            voice_score, make_region(0, 2, 1, 1));
        expect(matching_voice.has_cross_region_spanners,
               "Matching-voice cross-region slur was not detected");
        const auto voice_projection = music::snapshot_score_projection(voice_score);
        expect(voice_projection.complete, "Voice spanner projection was unavailable");
        expect(!music::query_score_projection(voice_projection, make_region(0, 0, 4, 0))
                    .has_cross_region_spanners,
               "Projection reported another-voice spanner as cross-region");
        expect(music::query_score_projection(voice_projection, make_region(0, 2, 1, 1))
                   .has_cross_region_spanners,
               "Projection lost the matching-voice cross-region slur");

        // A contained slur in another voice is outside a voice-0 window, while
        // a voice-1 window that cuts the voice-1 slur across the barline must
        // report it. Both live and projected region queries share this filter.
        const auto filter_project = music::load_native_score(kVoiceFilterScore, "musicxml");
        auto* filter_score = filter_project->masterScore();
        expect(!music::query_score_region(filter_score, make_region(0, 0, 1, 0))
                    .has_cross_region_spanners,
               "Voice-0 region reported a voice-1 spanner as cross-region");
        const auto filter_projection = music::snapshot_score_projection(filter_score);
        expect(filter_projection.complete, "Voice filter projection was unavailable");
        size_t voice_filter_slurs = 0;
        for (const auto& span : filter_projection.spans) {
            if (!span.tie && span.start.voice && *span.start.voice == 1
                && span.end.voice && *span.end.voice == 1) {
                ++voice_filter_slurs;
            }
        }
        expect(voice_filter_slurs == 2, "Voice filter fixture did not import both slurs");
        expect(!music::query_score_projection(filter_projection, make_region(0, 0, 1, 0))
                    .has_cross_region_spanners,
               "Voice-0 projection reported a voice-1 spanner as cross-region");
        expect(music::query_score_region(filter_score, make_region(0, 0, 1, 1))
                   .has_cross_region_spanners,
               "Voice-1 region lost the cross-measure slur");
        expect(music::query_score_projection(filter_projection, make_region(0, 0, 1, 1))
                   .has_cross_region_spanners,
               "Voice-1 projection lost the cross-measure slur");

        // A slur with one anchor in the queried voice and the other in another
        // voice is a voice-window crossing, not an ignorable other-voice span.
        const auto cross_project = music::load_native_score(kCrossVoiceSpanScore, "musicxml");
        auto* cross_score = cross_project->masterScore();
        expect(music::query_score_region(cross_score, make_region(0, 0, 1, 0))
                   .has_cross_region_spanners,
               "Voice-0 region lost a cross-voice slur");
        const auto cross_projection = music::snapshot_score_projection(cross_score);
        expect(cross_projection.complete, "Cross-voice projection was unavailable");
        expect(music::query_score_projection(cross_projection, make_region(0, 0, 1, 0))
                   .has_cross_region_spanners,
               "Voice-0 projection lost a cross-voice slur");
        expect(music::query_score_region(cross_score, make_region(0, 1, 1, 1))
                   .has_cross_region_spanners,
               "Voice-1 region lost a cross-voice slur");
        expect(music::query_score_projection(cross_projection, make_region(0, 1, 1, 1))
                   .has_cross_region_spanners,
               "Voice-1 projection lost a cross-voice slur");

        std::cout << "Native pitch query/edit tests passed: stable EIDs, transposing "
                     "spelling, undo/redo, all-or-nothing validation and MusicXML round-trip\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
