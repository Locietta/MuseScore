#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <engrave/error.h>
#include <engrave/output.h>
#include <engrave/query.h>

namespace engrave {

// One open score. Every call must come from the thread that created the
// Engine, and the Document must be destroyed before the Engine; destroying it
// closes the score.
struct Document {
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // 1 after open or create; only committed edits change it. Exports do not,
    // although the writer assigns element ids.
    std::uint64_t revision() const;

    // Problems found while opening or creating the score.
    const std::vector<Diagnostic>& diagnostics() const;

    // Unsupported for a format the engine does not write (see Capabilities).
    // Throws std::runtime_error if the engine's writer fails.
    Result<std::vector<std::byte>> export_score(const ExportOptions& options);

    // The score's structure: metadata, parts and staves, measures, time, key and
    // tempo changes, and the excerpts. NotFound when nothing has the excerpt id,
    // StaleHandle when the score it names is gone, InvalidArgument when it names
    // something other than a score.
    Result<Outline> outline(const ScoreSel& score = {});

    // The elements that match, in a stable order. NotFound for a property name the
    // catalogue does not know, InvalidArgument for a type name it does not know and for
    // a filter that cannot be answered (an unknown staff or voice, a measure window out
    // of range or too wide, both forms of the window, a reversed tick range, a cursor
    // past the end, a limit of 0), ResourceLimit when more elements match than one query
    // may report.
    Result<QueryResult> query(const Query& request);

    // The properties of one element.  The default is every property the catalogue
    // lists for its type. NotFound for a property name the catalogue does not know
    // and for a reference nothing answers; InvalidArgument for a property the
    // element's type does not have, and for an OpRef, which only a batch can use.
    Result<std::vector<PropertyRead>> get(const Ref& target, const std::vector<std::string>& props = {});

private:
    friend struct Engine;
    friend struct DocumentAccess;
    struct Impl;
    explicit Document(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace engrave
