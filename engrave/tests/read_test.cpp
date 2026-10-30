// The read API: outline(), query() and get(), against the A2 score code, which walks
// the same score independently.
//
//   engrave_read_test --assets=<resources> [--corpus=<file or dir>]...
//
// The corpus test compares a query with the semantic dump (engrave/src/score/
// score_semantic.cpp).  The comparison is exact and covers the element kinds both
// walks report: for every score of a document, master and excerpts,
//   - each chord, rest and measure repeat of every staff and voice, with its tick and
//     length in ticks;
//   - each note of those chords, with its pitch;
//   - each time signature, key signature and clef, with its tick, staff and value.
// A query over the same types must give one row per dump row with the same fields and
// no row of its own.  What the dump reports and a query does not is out of scope:
// annotations, spanners, frame elements, and everything layout builds.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <engrave/engrave.h>

#include "engrave/src/api/handles.h"
#include "engrave/src/api/state.h"
#include "engrave/src/score/score_metadata.h"
#include "engrave/src/score/score_positions.h"
#include "engrave/src/score/score_semantic.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/property.h"
#include "engraving/dom/score.h"
#include "engraving/dom/textbase.h"
#include "engraving/editing/transaction/undostack.h"
#include "engraving/types/constants.h"

namespace {

using engrave::ElementId;
using engrave::ErrorCode;
using engrave::PropertyRead;
using mu::engraving::MasterScore;

// A whole note in the engine's ticks, which is what a Fraction in the API counts.
constexpr std::int64_t whole_note_ticks = 4 * mu::engraving::Constants::DIVISION;

std::filesystem::path g_assets;
std::vector<std::filesystem::path> g_corpus;
std::unique_ptr<engrave::Engine> g_engine;

// --- text, bytes and documents -------------------------------------------

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::byte> as_bytes(std::string_view text) {
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

std::unique_ptr<engrave::Document> open_path(const std::filesystem::path& path) {
    auto document = g_engine->open(path);
    EXPECT_TRUE(document) << path.string() << ": " << document.error().message;
    return document ? std::move(*document) : nullptr;
}

MasterScore& master_of(engrave::Document& document) {
    MasterScore* master = engrave::DocumentAccess::master_score(document);
    EXPECT_NE(master, nullptr);
    return *master;
}

std::int64_t ticks_of(const engrave::Fraction& value) {
    return value.num * whole_note_ticks / value.den;
}

/// Checks the error a call must fail with, without reading .error() of a success.
template <typename T>
void expect_code(const engrave::Result<T>& result, ErrorCode expected, std::string_view what) {
    if (result) {
        ADD_FAILURE() << what << ": the call succeeded";
        return;
    }
    EXPECT_EQ(result.error().code, expected) << what << ": " << result.error().message;
}

// --- transactions --------------------------------------------------------

/// One transaction on the score, so that an engine command may run (the batch API is
/// S6's; a read test drives the primitive the same way the handle tests do).
class Command {
public:
    explicit Command(MasterScore& score) : m_score(score) {
        mu::engraving::MScore::setError(mu::engraving::MsError::MS_NO_ERROR);
        m_score.startCmd(muse::TranslatableString::untranslatable("engrave read test"));
    }
    ~Command() {
        if (m_open) m_score.endCmd(false, true);
    }
    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;

    void commit() {
        m_score.endCmd(false, true);
        m_open = false;
        engrave::assign_missing_ids(m_score);
        m_score.undoStack()->clearHistory();
    }

private:
    MasterScore& m_score;
    bool m_open = true;
};

// --- rows of the oracle comparison ---------------------------------------

// One element of the comparison, as a line so that a difference prints readably.
using Row = std::string;

const char* type_name(const nlohmann::json& kind) {
    const std::string value = kind.get<std::string>();
    if (value == "chord") return "Chord";
    if (value == "rest") return "Rest";
    if (value == "measureRepeat") return "MeasureRepeat";
    return "unknown";
}

Row event_row(std::string_view type, int tick, std::size_t staff, std::size_t voice, int ticks) {
    return std::format("{} tick={} staff={} voice={} ticks={}", type, tick, staff, voice, ticks);
}

Row note_row(int tick, std::size_t staff, std::size_t voice, int pitch) {
    return std::format("Note tick={} staff={} voice={} pitch={}", tick, staff, voice, pitch);
}

std::string value_text(const nlohmann::json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

// The rows the semantic dump reports for one score body.
void oracle_rows(const nlohmann::json& body, std::multiset<Row>& rows) {
    const nlohmann::json& events = body.at("events");
    for (std::size_t staff = 0; staff < events.size(); ++staff) {
        for (std::size_t voice = 0; voice < events[staff].size(); ++voice) {
            for (const nlohmann::json& item : events[staff][voice]) {
                const int tick = item.at("tick").get<int>();
                const int ticks = item.at("ticks").get<int>();
                rows.insert(event_row(type_name(item.at("kind")), tick, staff, voice, ticks));
                if (!item.contains("notes")) continue;
                for (const nlohmann::json& note : item.at("notes")) {
                    rows.insert(note_row(tick, staff, voice, note.at("pitch").get<int>()));
                }
            }
        }
    }
    for (const nlohmann::json& signature : body.at("signatures")) {
        const std::string type = signature.at("type").get<std::string>();
        const int tick = signature.at("tick").get<int>();
        const std::size_t staff = signature.at("staff").get<std::size_t>();
        if (type == "Clef") {
            // The clef token depends on the score's concert pitch setting; the tick and
            // the staff are what the walk has to agree on.
            rows.insert(std::format("Clef tick={} staff={}", tick, staff));
            continue;
        }
        rows.insert(std::format("{} tick={} staff={} value={}", type, tick, staff, value_text(signature.at("value"))));
    }
}

const PropertyRead* property(const engrave::ElementInfo& element, std::string_view name) {
    for (const PropertyRead& read : element.props) {
        if (read.name == name) return &read;
    }
    return nullptr;
}

std::int64_t int_property(const engrave::ElementInfo& element, std::string_view name) {
    const PropertyRead* read = property(element, name);
    EXPECT_NE(read, nullptr) << element.type << " " << engrave::to_string(element.id) << " has no " << name;
    if (!read || !std::holds_alternative<std::int64_t>(read->value)) return -1;
    return std::get<std::int64_t>(read->value);
}

// The rows a query reports, in the same form as oracle_rows.
void query_rows(const std::vector<engrave::ElementInfo>& elements, std::multiset<Row>& rows) {
    for (const engrave::ElementInfo& element : elements) {
        const auto tick = static_cast<int>(ticks_of(element.tick));
        const auto staff = static_cast<std::size_t>(element.staff);
        const auto voice = static_cast<std::size_t>(element.voice);
        if (element.type == "Note") {
            rows.insert(note_row(tick, staff, voice, static_cast<int>(int_property(element, "pitch"))));
        } else if (element.type == "Chord" || element.type == "Rest" || element.type == "MeasureRepeat") {
            rows.insert(event_row(element.type, tick, staff, voice, static_cast<int>(ticks_of(element.duration))));
        } else if (element.type == "Clef") {
            rows.insert(std::format("Clef tick={} staff={}", tick, staff));
        } else if (element.type == "KeySig") {
            rows.insert(std::format("KeySig tick={} staff={} value={}", tick, staff,
                                    int_property(element, "concertKey")));
        } else if (element.type == "TimeSig") {
            const PropertyRead* read = property(element, "timesig");
            EXPECT_NE(read, nullptr) << "a TimeSig has no timesig";
            const auto& sig = std::get<engrave::Fraction>(read->value);
            rows.insert(std::format("TimeSig tick={} staff={} value={}/{}", tick, staff, sig.num, sig.den));
        } else if (element.type == "Clef") {
            rows.insert(std::format("Clef tick={} staff={}", tick, staff));
        } else {
            ADD_FAILURE() << "the comparison covers no " << element.type;
        }
    }
}

/// Compares two row sets, reporting the rows whose counts differ rather than the whole
/// set.
void expect_same_rows(const std::multiset<Row>& expected, const std::multiset<Row>& actual, std::string_view what) {
    std::map<Row, int> wanted;
    for (const Row& row : expected) ++wanted[row];
    std::map<Row, int> got;
    for (const Row& row : actual) ++got[row];
    int reported = 0;
    for (const auto& [row, count] : wanted) {
        const auto found = got.find(row);
        const int mine = found == got.end() ? 0 : found->second;
        if (mine == count) continue;
        if (reported++ < 20) {
            ADD_FAILURE() << what << ": " << row << " appears " << mine << " time(s) in the query, " << count
                          << " in the oracle";
        }
    }
    for (const auto& [row, count] : got) {
        if (wanted.contains(row)) continue;
        if (reported++ < 20) ADD_FAILURE() << what << ": the query has " << row << " " << count << " time(s), the oracle none";
    }
    if (reported > 20) ADD_FAILURE() << what << ": " << reported - 20 << " more row(s) differ";
}

// --- reading a whole score ----------------------------------------------

constexpr std::string_view compared_types[] = {"Chord", "Rest", "MeasureRepeat", "Note", "Clef", "KeySig", "TimeSig"};
constexpr std::string_view compared_props[] = {"pitch", "timesig", "concertKey"};

std::vector<std::string> names(std::span<const std::string_view> values) {
    std::vector<std::string> result;
    for (std::string_view value : values) result.emplace_back(value);
    return result;
}

/// Every element of the comparison's types in one score, asked for measure window by
/// measure window so that a large score stays under the result cap, and page by page
/// so that paging is part of every corpus run.
std::vector<engrave::ElementInfo> read_all(engrave::Document& document, const engrave::ScoreSel& score,
                                           std::size_t measures) {
    std::vector<engrave::ElementInfo> elements;
    for (std::size_t first = 0; first < measures; first += engrave::max_query_measures) {
        engrave::Query request;
        request.score = score;
        request.types = names(compared_types);
        request.props = names(compared_props);
        request.first_measure = static_cast<std::int32_t>(first);
        request.measure_count = static_cast<std::int32_t>(std::min(engrave::max_query_measures, measures - first));
        for (std::size_t cursor = 0;;) {
            request.cursor = cursor;
            auto page = document.query(request);
            if (!page) {
                ADD_FAILURE() << "query: " << page.error().message;
                return elements;
            }
            elements.insert(elements.end(), std::make_move_iterator(page->elements.begin()),
                            std::make_move_iterator(page->elements.end()));
            if (!page->next_cursor) break;
            cursor = *page->next_cursor;
        }
    }
    return elements;
}

// --- corpus choice -------------------------------------------------------

std::filesystem::path corpus_file(std::initializer_list<std::string_view> extensions) {
    for (const auto& path : g_corpus) {
        if (extensions.size() == 0) return path;
        for (std::string_view extension : extensions) {
            if (path.extension() == extension) return path;
        }
    }
    return {};
}

/// The first corpus score that has an element of a type.
std::unique_ptr<engrave::Document> open_with(std::string_view type) {
    for (const auto& path : g_corpus) {
        auto document = g_engine->open(path);
        if (!document) continue;
        engrave::Query request;
        request.types = {std::string(type)};
        const auto found = (*document)->query(request);
        if (found && !found->elements.empty()) return std::move(*document);
    }
    return nullptr;
}

// --- the tests -----------------------------------------------------------

TEST(Read, MatchesTheSemanticOracle) {
    ASSERT_FALSE(g_corpus.empty());
    int scores = 0;
    std::size_t rows = 0;
    for (const auto& path : g_corpus) {
        auto document = open_path(path);
        if (!document) continue;
        SCOPED_TRACE(path.string());
        const nlohmann::json dump = music::semantic_dump(&master_of(*document));

        const auto outline = document->outline();
        ASSERT_TRUE(outline) << outline.error().message;
        const std::size_t measures = outline->measures.size();

        std::multiset<Row> expected;
        oracle_rows(dump, expected);
        std::multiset<Row> actual;
        query_rows(read_all(*document, {}, measures), actual);
        expect_same_rows(expected, actual, "the master score");
        rows += expected.size();
        ++scores;

        // The excerpts are separate scores with ids of their own; the dump lists each
        // one's body under its name, in master order.  Two excerpts of one part name are
        // told apart by their order, which both lists keep.
        std::size_t next = 0;
        for (const nlohmann::json& listed : dump.at("excerpts")) {
            if (!listed.contains("score")) continue;
            const std::string name = listed.at("name").get<std::string>();
            while (next < outline->excerpts.size() && outline->excerpts[next].name != name) ++next;
            if (next == outline->excerpts.size()) {
                ADD_FAILURE() << "the outline has no excerpt named '" << name << "'";
                continue;
            }
            const engrave::ExcerptInfo& excerpt = outline->excerpts[next++];
            const auto excerpt_outline = document->outline(engrave::ScoreSel{excerpt.id});
            ASSERT_TRUE(excerpt_outline) << excerpt_outline.error().message;
            std::multiset<Row> excerpt_expected;
            oracle_rows(listed.at("score"), excerpt_expected);
            std::multiset<Row> excerpt_actual;
            query_rows(read_all(*document, engrave::ScoreSel{excerpt.id}, excerpt_outline->measures.size()),
                       excerpt_actual);
            expect_same_rows(excerpt_expected, excerpt_actual, "the excerpt '" + excerpt.name + "'");
            rows += excerpt_expected.size();
        }
    }
    ASSERT_GT(scores, 0) << "no corpus score opens";
    std::printf("corpus: %d score(s), %zu compared row(s)\n", scores, rows);
    ASSERT_GT(rows, 0u) << "the corpus comparison saw no element";
}

TEST(Read, OutlineDescribesTheScore) {
    ASSERT_FALSE(g_corpus.empty());
    for (const auto& path : g_corpus) {
        auto document = open_path(path);
        if (!document) continue;
        SCOPED_TRACE(path.string());
        MasterScore& master = master_of(*document);
        const nlohmann::json metadata = music::score_metadata(&master, std::vector<music::ScoreMetadataTrack>{});
        const auto outline = document->outline();
        ASSERT_TRUE(outline) << outline.error().message;

        // The metadata fields are the engine's own derivation.
        EXPECT_EQ(outline->title, metadata.at("title").get<std::string>());
        EXPECT_EQ(outline->subtitle, metadata.at("subtitle").get<std::string>());
        EXPECT_EQ(outline->composer, metadata.at("composer").get<std::string>());
        EXPECT_EQ(outline->poet, metadata.at("poet").get<std::string>());
        EXPECT_EQ(outline->timesig, metadata.at("timesig").get<std::string>());
        EXPECT_EQ(outline->mscore_version, metadata.at("mscoreVersion").get<std::string>());
        EXPECT_EQ(outline->file_version, metadata.at("fileVersion").get<int>());
        EXPECT_EQ(outline->pages, metadata.at("pages").get<int>());
        EXPECT_EQ(outline->measure_count, metadata.at("measures").get<int>());
        EXPECT_EQ(outline->key, metadata.at("keysig").get<int>());
        EXPECT_EQ(outline->duration, metadata.at("duration").get<double>());

        // Parts and staves are named as the metadata names them, and every id it hands
        // out resolves.
        const nlohmann::json& parts = metadata.at("parts");
        ASSERT_EQ(outline->parts.size(), parts.size());
        for (std::size_t i = 0; i < outline->parts.size(); ++i) {
            const engrave::PartInfo& part = outline->parts[i];
            EXPECT_EQ(part.name, parts[i].at("name").get<std::string>());
            EXPECT_EQ(part.instrument_id, parts[i].at("instrumentId").get<std::string>());
            EXPECT_TRUE(document->get(part.id).has_value()) << "the part id does not resolve";
            ASSERT_FALSE(part.staves.empty());
            for (const engrave::StaffInfo& staff : part.staves) {
                EXPECT_TRUE(document->get(staff.id).has_value()) << "the staff id does not resolve";
                EXPECT_EQ(staff.part, static_cast<std::int32_t>(i));
                EXPECT_GE(staff.lines, 0);
            }
        }

        // The measures are the score's own list, in order, and each id names the
        // measure at that index.
        ASSERT_EQ(outline->measures.size(), master.nmeasures());
        for (std::size_t i = 0; i < outline->measures.size(); ++i) {
            const engrave::MeasureInfo& measure = outline->measures[i];
            EXPECT_EQ(measure.index, static_cast<std::int32_t>(i));
            EXPECT_GT(measure.duration.num, 0);
            if (i > 0) EXPECT_LE(ticks_of(outline->measures[i - 1].tick), ticks_of(measure.tick));
            const auto read = document->get(measure.id);
            ASSERT_TRUE(read) << "measure " << i << ": " << read.error().message;
        }

        // The signatures are the dump's, which walks them independently: same type,
        // tick and staff.  The dump's clefs are left out: a clef change is an element
        // of its own that a query reports, and the outline covers time, key and tempo.
        std::multiset<std::string> expected;
        for (const nlohmann::json& signature : music::semantic_dump(&master).at("signatures")) {
            const std::string type = signature.at("type").get<std::string>();
            if (type == "Clef") continue;
            expected.insert(std::format("{} tick={} staff={}", type, signature.at("tick").get<int>(),
                                        signature.at("staff").get<std::size_t>()));
        }
        std::multiset<std::string> actual;
        for (const engrave::SignatureInfo& signature : outline->signatures) {
            if (signature.type == "Tempo") continue;
            actual.insert(std::format("{} tick={} staff={}", signature.type, static_cast<int>(ticks_of(signature.tick)),
                                      signature.staff));
        }
        EXPECT_EQ(expected, actual) << "the outline's signatures are not the dump's";

        // The tempo markings are the dump's annotations of that type.
        std::multiset<std::string> expected_tempi;
        for (const nlohmann::json& annotation : music::semantic_dump(&master).at("annotations")) {
            if (annotation.at("type").get<std::string>() != "Tempo") continue;
            expected_tempi.insert(std::format("tick={} bpm={} text={}", annotation.at("tick").get<int>(),
                                              annotation.at("bpm").get<double>(),
                                              annotation.at("text").get<std::string>()));
        }
        std::multiset<std::string> actual_tempi;
        for (const engrave::SignatureInfo& signature : outline->signatures) {
            if (signature.type != "Tempo") continue;
            actual_tempi.insert(std::format("tick={} bpm={} text={}", static_cast<int>(ticks_of(signature.tick)),
                                            signature.bpm, signature.text));
        }
        EXPECT_EQ(expected_tempi, actual_tempi) << "the outline's tempo markings are not the dump's";
    }
}

TEST(Read, FiltersAndPagingAgree) {
    auto document = open_path(corpus_file({".mscx"}));
    ASSERT_TRUE(document);
    const auto outline = document->outline();
    ASSERT_TRUE(outline) << outline.error().message;
    const auto measures = static_cast<std::size_t>(outline->measures.size());
    ASSERT_GT(measures, 1u);

    // Every element of the score, in one call.
    engrave::Query all;
    all.limit = engrave::max_query_results;
    const auto full = document->query(all);
    ASSERT_TRUE(full) << full.error().message;
    ASSERT_EQ(full->total, full->elements.size());
    ASSERT_GT(full->total, 20u) << "the corpus score is too small to page";
    EXPECT_FALSE(full->next_cursor);

    const auto ids = [](const std::vector<engrave::ElementInfo>& elements) {
        std::vector<std::string> result;
        for (const auto& element : elements) result.push_back(engrave::to_string(element.id));
        return result;
    };
    const std::vector<std::string> order = ids(full->elements);

    // The same query twice is the same answer, in the same order.
    const auto again = document->query(all);
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(ids(again->elements), order) << "a repeated query reordered its results";

    // Page by page, the results concatenate to the one call's, in the same order.
    engrave::Query paged;
    paged.limit = 7;
    std::vector<engrave::ElementInfo> pages;
    std::size_t calls = 0;
    for (std::size_t cursor = 0;;) {
        paged.cursor = cursor;
        const auto page = document->query(paged);
        ASSERT_TRUE(page) << page.error().message;
        EXPECT_EQ(page->total, full->total) << "paging changed the total";
        pages.insert(pages.end(), page->elements.begin(), page->elements.end());
        if (!page->next_cursor) break;
        EXPECT_EQ(*page->next_cursor, pages.size()) << "the next cursor is not where the page ended";
        cursor = *page->next_cursor;
        if (++calls > full->total) ADD_FAILURE() << "paging does not end";
    }
    EXPECT_EQ(ids(pages), order) << "the pages are not the whole query";

    // A type filter selects exactly the rows of that type.
    engrave::Query typed;
    typed.types = {"Note"};
    typed.limit = engrave::max_query_results;
    const auto notes = document->query(typed);
    ASSERT_TRUE(notes) << notes.error().message;
    std::size_t notes_in_full = 0;
    for (const auto& element : full->elements) notes_in_full += element.type == "Note";
    EXPECT_EQ(notes->total, notes_in_full);
    for (const auto& element : notes->elements) EXPECT_EQ(element.type, "Note");

    // Adjacent measure windows tile the score.
    std::vector<std::string> tiled;
    for (std::size_t first = 0; first < measures; ++first) {
        engrave::Query window;
        window.first_measure = static_cast<std::int32_t>(first);
        window.measure_count = 1;
        window.limit = engrave::max_query_results;
        const auto part = document->query(window);
        ASSERT_TRUE(part) << part.error().message;
        for (const auto& element : part->elements) {
            EXPECT_EQ(element.measure_index, static_cast<std::int32_t>(first));
            tiled.push_back(engrave::to_string(element.id));
        }
    }
    // Every element the whole-score query reported for a measure is in exactly one
    // window; the score and its parts and staves are in none.
    std::vector<std::string> in_measures;
    for (const auto& element : full->elements) {
        if (element.measure_index >= 0) in_measures.push_back(engrave::to_string(element.id));
    }
    std::ranges::sort(in_measures);
    std::ranges::sort(tiled);
    EXPECT_EQ(tiled, in_measures) << "the measure windows do not tile the score";

    // A staff or a voice filter keeps the elements of that staff or voice and drops
    // the rest.
    if (master_of(*document).nstaves() > 1) {
        engrave::Query staff;
        staff.staves = {1};
        staff.limit = engrave::max_query_results;
        const auto on_staff = document->query(staff);
        ASSERT_TRUE(on_staff) << on_staff.error().message;
        std::size_t expected = 0;
        for (const auto& element : full->elements) expected += element.staff == 1;
        EXPECT_EQ(on_staff->total, expected);
        for (const auto& element : on_staff->elements) EXPECT_EQ(element.staff, 1);
    }
    engrave::Query voice;
    voice.voices = {2};
    voice.limit = engrave::max_query_results;
    const auto in_voice = document->query(voice);
    ASSERT_TRUE(in_voice) << in_voice.error().message;
    for (const auto& element : in_voice->elements) EXPECT_EQ(element.voice, 2);

    // A tick range keeps the elements that start inside it.
    engrave::Query ticks;
    ticks.from_tick = engrave::Fraction{1, 4};
    ticks.to_tick = engrave::Fraction{1, 2};
    ticks.limit = engrave::max_query_results;
    const auto in_ticks = document->query(ticks);
    ASSERT_TRUE(in_ticks) << in_ticks.error().message;
    std::size_t expected = 0;
    for (const auto& element : full->elements) {
        const double start = static_cast<double>(element.tick.num) / static_cast<double>(element.tick.den);
        expected += start >= 0.25 && start < 0.5;
    }
    EXPECT_EQ(in_ticks->total, expected);
}

TEST(Read, LimitsAreEnforced) {
    auto document = open_path(corpus_file({".mscx"}));
    ASSERT_TRUE(document);
    MasterScore& master = master_of(*document);
    const auto outline = document->outline();
    ASSERT_TRUE(outline) << outline.error().message;
    const auto measures = static_cast<std::int32_t>(outline->measures.size());

    engrave::Query request;
    request.limit = 0;
    expect_code(document->query(request), ErrorCode::InvalidArgument, "a limit of 0");
    request.limit = engrave::max_query_results + 1;
    expect_code(document->query(request), ErrorCode::ResourceLimit, "a limit over the cap");
    request.limit = engrave::max_query_results;
    request.types = {"NoSuchType"};
    expect_code(document->query(request), ErrorCode::InvalidArgument, "an unknown type");
    request.types = {};
    request.props = {"noSuchProperty"};
    expect_code(document->query(request), ErrorCode::NotFound, "an unknown property name");
    request.props = {};
    request.staves = {99};
    expect_code(document->query(request), ErrorCode::InvalidArgument, "a staff the score has not");
    request.staves = {};
    request.voices = {4};
    expect_code(document->query(request), ErrorCode::InvalidArgument, "a voice over 3");
    request.voices = {};
    request.first_measure = 0;
    request.measure_count = static_cast<std::int32_t>(engrave::max_query_measures) + 1;
    expect_code(document->query(request), ErrorCode::InvalidArgument, "a window over the measure cap");
    request.measure_count = measures + 1;
    expect_code(document->query(request), ErrorCode::InvalidArgument, "a window past the last measure");
    request.measure_count.reset();
    request.first_measure = measures + 1;
    expect_code(document->query(request), ErrorCode::InvalidArgument, "a window that starts past the end");
    request.first_measure.reset();
    request.from_tick = engrave::Fraction{1, 2};
    request.to_tick = engrave::Fraction{1, 4};
    expect_code(document->query(request), ErrorCode::InvalidArgument, "a reversed tick range");
    request.from_tick.reset();
    request.to_tick.reset();
    request.cursor = 100000;
    expect_code(document->query(request), ErrorCode::InvalidArgument, "a cursor past the end");
    request.cursor = 0;

    // A score with more elements than one query may report is a ResourceLimit, not a
    // partial answer.  Measures are appended through the engine's own command; the
    // batch API that would do this for a client is S6's.
    {
        Command command(master);
        for (std::size_t i = 0; i <= engrave::max_query_results; ++i) {
            if (!master.Score::insertMeasure(mu::engraving::ElementType::MEASURE, nullptr)) {
                GTEST_SKIP() << "the engine did not append a measure";
            }
        }
        command.commit();
    }
    engrave::Query everything;
    everything.types = {"Measure"};
    const auto tooMany = document->query(everything);
    ASSERT_FALSE(tooMany) << "a query over " << everything.types.front() << " elements succeeded";
    EXPECT_EQ(tooMany.error().code, ErrorCode::ResourceLimit);
    EXPECT_FALSE(tooMany.error().message.empty());

    // The same score narrowed to a measure window fits again.
    everything.first_measure = 0;
    everything.measure_count = 16;
    const auto window = document->query(everything);
    ASSERT_TRUE(window) << window.error().message;
    EXPECT_LE(window->total, 16u);
}

TEST(Read, GetReadsProperties) {
    auto document = open_with("Dynamic");
    ASSERT_TRUE(document) << "no corpus score has a dynamic";
    MasterScore& master = master_of(*document);

    // A note: the properties the catalogue lists for its type, and pitch among them.
    engrave::Query notes;
    notes.types = {"Note"};
    const auto found = document->query(notes);
    ASSERT_TRUE(found) << found.error().message;
    ASSERT_FALSE(found->elements.empty()) << "no note in the corpus score";
    const ElementId note_id = found->elements.front().id;

    const auto all = document->get(note_id);
    ASSERT_TRUE(all) << all.error().message;
    std::set<std::string> names;
    for (const PropertyRead& read : *all) names.insert(read.name);
    EXPECT_EQ(names.size(), all->size()) << "a property is reported twice";
    EXPECT_TRUE(names.contains("pitch"));
    const auto pitch = document->get(note_id, {"pitch"});
    ASSERT_TRUE(pitch) << pitch.error().message;
    ASSERT_EQ(pitch->size(), 1u);
    EXPECT_EQ((*pitch)[0].name, "pitch");
    EXPECT_TRUE(std::holds_alternative<std::int64_t>((*pitch)[0].value));
    EXPECT_FALSE((*pitch)[0].styled);
    EXPECT_FALSE((*pitch)[0].is_default) << "pitch has no default to be";

    // A dynamic: minDistance is a style's value until the element sets one.
    engrave::Query dynamics;
    dynamics.types = {"Dynamic"};
    const auto dynamic_found = document->query(dynamics);
    ASSERT_TRUE(dynamic_found) << dynamic_found.error().message;
    ASSERT_FALSE(dynamic_found->elements.empty()) << "no dynamic in the corpus score";
    const ElementId dynamic_id = dynamic_found->elements.front().id;
    {
        // Whatever the file holds, start from the style's value.
        Command command(master);
        const auto dynamic = engrave::resolve(master, dynamic_id);
        ASSERT_TRUE(dynamic) << dynamic.error().message;
        dynamic->object->undoResetProperty(mu::engraving::Pid::MIN_DISTANCE);
        command.commit();
    }
    auto styled = document->get(dynamic_id, {"minDistance"});
    ASSERT_TRUE(styled) << styled.error().message;
    ASSERT_EQ(styled->size(), 1u);
    EXPECT_TRUE((*styled)[0].styled) << "minDistance is not reported as styled";
    EXPECT_TRUE((*styled)[0].is_default) << "an untouched style value is not the default";

    {
        Command command(master);
        const auto dynamic = engrave::resolve(master, dynamic_id);
        ASSERT_TRUE(dynamic) << dynamic.error().message;
        dynamic->object->undoChangeProperty(mu::engraving::Pid::MIN_DISTANCE,
                                            mu::engraving::PropertyValue::fromValue(2.5),
                                            mu::engraving::PropertyFlags::UNSTYLED);
        command.commit();
    }
    auto changed = document->get(dynamic_id, {"minDistance"});
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_FALSE((*changed)[0].styled) << "a value the element sets is still reported as styled";
    EXPECT_FALSE((*changed)[0].is_default) << "a value the element sets is reported as the default";
    EXPECT_EQ(std::get<double>((*changed)[0].value), 2.5);

    {
        Command command(master);
        const auto dynamic = engrave::resolve(master, dynamic_id);
        ASSERT_TRUE(dynamic) << dynamic.error().message;
        dynamic->object->undoResetProperty(mu::engraving::Pid::MIN_DISTANCE);
        command.commit();
    }
    auto reset = document->get(dynamic_id, {"minDistance"});
    ASSERT_TRUE(reset) << reset.error().message;
    EXPECT_TRUE((*reset)[0].styled) << "a reset property did not go back to its style";
    EXPECT_TRUE((*reset)[0].is_default) << "a reset property is not the default again";

    // A property name the catalogue does not know, and one the type does not have.
    const auto unknown = document->get(note_id, {"noSuchProperty"});
    expect_code(unknown, ErrorCode::NotFound, "an unknown property name");
    const auto foreign = document->get(dynamic_id, {"concertClefType"});
    expect_code(foreign, ErrorCode::InvalidArgument, "a property of another type");
    // A reference only a batch can use.
    expect_code(document->get(engrave::OpRef{}), ErrorCode::InvalidArgument, "an OpRef");
}

TEST(Read, LayoutBoxesComeFromTheLayout) {
    auto document = open_path(corpus_file({".mscx"}));
    ASSERT_TRUE(document);
    const auto outline = document->outline();
    ASSERT_TRUE(outline) << outline.error().message;
    ASSERT_GT(outline->pages, 0);

    engrave::Query request;
    request.types = {"Measure"};
    request.layout_boxes = true;
    const auto measures = document->query(request);
    ASSERT_TRUE(measures) << measures.error().message;
    ASSERT_EQ(measures->total, outline->measures.size());

    std::int32_t page = 0;
    for (const auto& measure : measures->elements) {
        ASSERT_TRUE(measure.box) << "a laid-out measure has no box";
        EXPECT_GE(measure.box->page, page) << "the pages of the measures go backwards";
        page = measure.box->page;
        EXPECT_LT(measure.box->page, outline->pages);
        EXPECT_GT(measure.box->width, 0.0);
        EXPECT_GE(measure.box->height, 0.0);
        EXPECT_GE(measure.box->x, 0.0);
        EXPECT_GE(measure.box->y, 0.0);
    }

    // Without the flag, no box is filled in.
    engrave::Query plain;
    plain.types = {"Measure"};
    const auto bare = document->query(plain);
    ASSERT_TRUE(bare) << bare.error().message;
    for (const auto& measure : bare->elements) EXPECT_FALSE(measure.box);
}

TEST(Read, ErrorsForStaleAndMissingReferences) {
    const auto path = corpus_file({".mscx"});
    ASSERT_FALSE(path.empty());
    auto document = open_path(path);
    ASSERT_TRUE(document);
    const auto outline = document->outline();
    ASSERT_TRUE(outline) << outline.error().message;

    engrave::Query notes;
    notes.types = {"Note"};
    const auto found = document->query(notes);
    ASSERT_TRUE(found) << found.error().message;
    ASSERT_FALSE(found->elements.empty());
    const ElementId note_id = found->elements.front().id;

    // An id that names nothing is NotFound, in get() and in a measure window alike.
    const ElementId missing{0x1234, 0x5678};
    expect_code(document->get(missing), ErrorCode::NotFound, "an id nothing has");
    engrave::Query window;
    window.from_measure = missing;
    expect_code(document->query(window), ErrorCode::NotFound, "a measure id nothing has");
    // A selection has to name a score.
    engrave::Query wrong_score;
    wrong_score.score = engrave::ScoreSel{note_id};
    expect_code(document->query(wrong_score), ErrorCode::InvalidArgument, "a selection that names a note");
    wrong_score.score = engrave::ScoreSel{missing};
    expect_code(document->query(wrong_score), ErrorCode::NotFound, "a selection nothing has");
    expect_code(document->outline(engrave::ScoreSel{missing}), ErrorCode::NotFound, "an outline of nothing");
    expect_code(document->outline(engrave::ScoreSel{note_id}), ErrorCode::InvalidArgument, "an outline of a note");
    // A measure window names a measure of this score.
    engrave::Query not_a_measure;
    not_a_measure.from_measure = note_id;
    expect_code(document->query(not_a_measure), ErrorCode::InvalidArgument, "a window that names a note");
    engrave::Query both_forms;
    both_forms.first_measure = 0;
    both_forms.from_measure = outline->measures.front().id;
    expect_code(document->query(both_forms), ErrorCode::InvalidArgument, "a window named twice");

    // A page, a system and a segment that the layout does not have are stale.
    expect_code(document->get(engrave::Address{engrave::PageAddress{.score = {}, .index = 9999}}),
                ErrorCode::StaleHandle, "a page past the end");
    expect_code(document->get(engrave::Address{engrave::SystemAddress{.score = {}, .page = 9999, .index = 0}}),
                ErrorCode::StaleHandle, "a system on no page");
    expect_code(document->get(engrave::Address{engrave::SegmentAddress{
                    .measure = outline->measures.front().id, .tick = engrave::Fraction{1000, 1}, .kind = "ChordRest"}}),
                ErrorCode::StaleHandle, "a segment at no tick");
    // The first page and the first measure's own segment do resolve.
    const auto first_page = document->get(engrave::Address{engrave::PageAddress{.score = {}, .index = 0}});
    ASSERT_TRUE(first_page) << first_page.error().message;
    EXPECT_FALSE(first_page->empty());
    const auto first_segment = document->get(engrave::Address{engrave::SegmentAddress{
        .measure = outline->measures.front().id, .tick = engrave::Fraction{0, 1}, .kind = "ChordRest"}});
    ASSERT_TRUE(first_segment) << first_segment.error().message;

    // An id of a document that is gone names nothing in another one.
    const auto other = corpus_file({".musicxml"});
    ASSERT_FALSE(other.empty());
    auto second = open_path(other);
    ASSERT_TRUE(second);
    expect_code(second->get(note_id), ErrorCode::NotFound, "an id of another document");
}

// --- corpus discovery ----------------------------------------------------

bool looks_like_score(const std::filesystem::path& path) {
    static const std::set<std::string> extensions = {".mscx", ".mscz", ".musicxml", ".mxl", ".mid", ".midi",
                                                     ".gp", ".gpx", ".gp3", ".gp4", ".gp5", ".ptb"};
    return extensions.contains(path.extension().string());
}

void collect(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) files.push_back(entry.path());
        std::ranges::sort(files);
        for (const auto& file : files) collect(file);
        return;
    }
    if (std::filesystem::is_regular_file(path, ec) && looks_like_score(path)) g_corpus.push_back(path);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument.starts_with("--assets=")) {
            g_assets = std::filesystem::path(argument.substr(9));
        } else if (argument.starts_with("--corpus=")) {
            collect(std::filesystem::path(argument.substr(9)));
        }
    }
    if (g_corpus.empty()) {
        for (const auto& candidate : {std::filesystem::path("fixtures/golden"), std::filesystem::path("../fixtures/golden"),
                                      std::filesystem::path("../../fixtures/golden")}) {
            if (std::filesystem::is_directory(candidate)) {
                collect(candidate);
                break;
            }
        }
    }
    if (g_corpus.empty()) {
        std::fprintf(stderr, "no corpus: pass --corpus=<file or directory>\n");
        return 1;
    }

    auto engine = engrave::Engine::create({.resources = g_assets});
    if (!engine) {
        std::fprintf(stderr, "Engine::create: %s (pass --assets=<resources>)\n", engine.error().message.c_str());
        return 1;
    }
    g_engine = std::move(*engine);
    const int result = RUN_ALL_TESTS();
    g_engine.reset();
    return result;
}
