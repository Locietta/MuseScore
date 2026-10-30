#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <engrave/edit.h>
#include <engrave/error.h>
#include <engrave/output.h>

namespace engrave {

namespace api {
class BatchGuard;
}

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

    // Runs a batch as one transaction. It fails and leaves the document as it was
    // with RevisionConflict when expected_revision is set and is not the current
    // revision, with NotFound, StaleHandle, InvalidArgument or Unsupported for an
    // operation the caller got wrong, and with EditRejected when the engine
    // refused one; Error::op_index names the operation. A committed batch that
    // changed the score advances the revision by one and gives what it created
    // its ids. Throws when the engine failed and the document could not be
    // restored: later edits and exports then return Poisoned.
    Result<EditResult> edit(const Batch& batch, std::optional<std::uint64_t> expected_revision = {});

private:
    friend struct Engine;
    friend struct DocumentAccess;
    friend class api::BatchGuard;
    struct Impl;
    explicit Document(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace engrave
