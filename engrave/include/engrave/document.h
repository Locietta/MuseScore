#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <engrave/error.h>
#include <engrave/output.h>

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

private:
    friend struct Engine;
    struct Impl;
    explicit Document(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace engrave
