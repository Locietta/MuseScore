#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <engrave/handle.h>
#include <engrave/value.h>

namespace engrave {

// One property of one element: what it holds, whether that is the default the
// engine would use, and whether the value comes from a style rather than from
// the element itself.
struct PropertyRead {
    std::string name; // the catalogue's name, as in describe()
    Value value;      // monostate when the engine has no value for the property
    // The engine knows a default for the property and the element's value is
    // that default. A property without a default (pitch, track, ...) is never
    // default.
    bool is_default = false;
    // The engine marks the value as its style's (PropertyFlags::STYLED); an edit that
    // overrides the property clears the mark.
    bool styled = false;
};

// Where an element is on the page, in the engine's layout units (1/1200 inch).
struct LayoutBox {
    std::int32_t page = 0; // page index in the score
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

// One element of a score.  A query reports the elements that have an id, which is
// every element an edit or get() can name; a segment, a page or a system has none and
// is reached through an Address instead, and an element layout builds (a stem, a
// system) is not part of the score's content at all.
struct ElementInfo {
    ElementId id;
    std::string type;                  // the MSCX type name, e.g. "Note"
    ScoreSel score;                    // the master, or the excerpt it belongs to
    std::optional<ElementId> measure;  // the measure it sits in
    std::int32_t measure_index = -1;   // ... and that measure's index in the score
    std::int32_t staff = -1;           // 0-based staff index; -1 when it is on none
    std::int32_t voice = -1;           // 0-based voice; -1 when it has none
    Fraction tick;                     // position in the score, in whole notes
    Fraction duration;                 // its length; zero for a point event
    std::optional<ElementId> parent;   // the element it hangs off, when that has an id
    std::vector<ElementId> links;      // its link group (linked parts), master first
    std::optional<LayoutBox> box;      // where it is, when the query asked for layout_boxes
    std::vector<PropertyRead> props;   // the properties the query asked for
};

// The score's structure, without its notes: what a caller needs to plan an edit
// or a query.
struct StaffInfo {
    ElementId id;
    std::int32_t index = 0;  // the score's staff index
    std::int32_t part = 0;   // index of the part in Outline::parts
    std::string group;       // "pitched", "percussion" or "tablature"
    std::int32_t lines = 5;
    bool invisible = false;
};

struct PartInfo {
    ElementId id;
    std::string name;           // the long name, plain text
    std::string instrument_id;  // e.g. "keyboard.piano"
    std::vector<StaffInfo> staves;
};

struct MeasureInfo {
    ElementId id;
    std::int32_t index = 0; // index in score order, 0-based
    Fraction tick;
    Fraction duration;
};

// A time signature, key signature or tempo marking.  Which of the value fields
// apply is decided by type.
struct SignatureInfo {
    std::string type; // "TimeSig", "KeySig" or "Tempo"
    Fraction tick;
    std::int32_t staff = -1;           // -1 for a tempo, which is the score's
    std::optional<Fraction> timesig;   // a TimeSig's signature
    std::int32_t key = 0;              // a KeySig's concert key, in fifths
    double bpm = 0;                    // a Tempo's beats per minute
    std::string text;                  // a Tempo's own text
};

struct ExcerptInfo {
    ElementId id;
    std::string name;
    std::int32_t pages = 0;
};

struct Outline {
    // The metadata of the score, as the engine's own metadata export derives it.
    std::string title;
    std::string subtitle;
    std::string composer;
    std::string poet;
    std::string timesig;        // "4/4"; empty when the score has no time signature
    std::string mscore_version;
    std::int32_t file_version = 0;
    std::int32_t pages = 0;
    std::int32_t measure_count = 0;
    std::int32_t key = 0;       // the key signature number of the first measure
    double duration = 0;        // seconds

    std::vector<PartInfo> parts;         // in score order
    std::vector<MeasureInfo> measures;   // every measure, in score order
    std::vector<SignatureInfo> signatures; // in score order, then staff, then type
    std::vector<ExcerptInfo> excerpts;   // the master's linked parts
};

// Which elements a query reports, and how many.
struct Query {
    ScoreSel score; // the master, or one excerpt

    // Empty: every type. A type name is the catalogue's ("Note", "Chord").
    std::vector<std::string> types;

    // The measures to report, by index range or by the ids of the first and last
    // measure. An unset index or count means the score's first measure or all of
    // them; both forms together are invalid. A window is at most
    // max_query_measures wide.
    std::optional<std::int32_t> first_measure;
    std::optional<std::int32_t> measure_count;
    std::optional<ElementId> from_measure;
    std::optional<ElementId> to_measure;

    // Empty: every staff or voice. Staff indices are the score's, voices 0..3.
    std::vector<std::int32_t> staves;
    std::vector<std::int32_t> voices;

    // The half-open tick range [from_tick, to_tick) in whole notes; an element is
    // reported when its tick is in it.
    std::optional<Fraction> from_tick;
    std::optional<Fraction> to_tick;

    // The properties to read for every element, by catalogue name. Empty: none. A
    // property the element's type does not have is left out, because one query can
    // cover several types; get() reports it instead.
    std::vector<std::string> props;

    // Fill ElementInfo::box. Layout must have run, which open and a commit do.
    bool layout_boxes = false;

    // Paging. The result order is stable: by score, measure, tick, staff, voice and
    // id, so the pages of one query concatenate to what one call with a larger
    // limit returns. A query that matches more than max_query_results elements is
    // a ResourceLimit: narrow it with types or a measure window.
    std::size_t limit = 4096;
    std::size_t cursor = 0;
};

// The elements a query matched, and how to ask for the rest.
struct QueryResult {
    std::vector<ElementInfo> elements;
    std::size_t total = 0;                  // how many elements the filter matched
    std::optional<std::size_t> next_cursor; // unset when this page is the last
};

// A measure window wider than this is rejected with InvalidArgument.
inline constexpr std::size_t max_query_measures = 128;
// The most elements one query may match; larger is ResourceLimit.
inline constexpr std::size_t max_query_results = 4096;

} // namespace engrave
