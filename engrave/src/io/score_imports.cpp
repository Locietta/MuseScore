#include "score_imports.h"

#include <cstddef>
#include <stdexcept>

#include "global/io/byteview.h"
#include "global/io/file.h"
#include "engraving/dom/masterscore.h"
#include "engraving/editing/editstyle.h"
#include "engraving/editing/transaction/transaction.h"
#include "engraving/engravingerrors.h"

namespace mu::iex::guitarpro {
// The public importgtp.h declaration is stale; this is the signature implemented
// by importgtp.cpp and used by guitarproreader.cpp.
extern mu::engraving::Err importGTP(mu::engraving::MasterScore*, muse::io::IODevice*, bool experimental);
}

namespace music {
namespace {
constexpr std::size_t MAX_IMPORT_BYTES = 8 * 1024 * 1024;

void load_guitarpro_style(mu::engraving::MasterScore* score)
{
    muse::io::File styleFile(":/engraving/styles/gp-style.mss");
    // 5.0 loads styles through EditStyle::loadStyle on a transaction. The import
    // is not an undoable user edit, so this uses the dummy transaction, exactly
    // like GuitarProReader::read() and the MasterScore::loadStyle that 4.x had.
    if (!mu::engraving::EditStyle::loadStyle(score->transactionManager()->currentOrDummyTransaction(),
                                            score, styleFile)) {
        throw std::runtime_error("Failed to load the Guitar Pro style");
    }
}
} // namespace

void import_guitarpro_score(mu::engraving::MasterScore* score, const std::string& bytes)
{
    if (!score) {
        throw std::invalid_argument("Guitar Pro import requires a score");
    }
    if (bytes.empty() || bytes.size() > MAX_IMPORT_BYTES) {
        throw std::invalid_argument("Input must contain 1..8388608 bytes");
    }

    muse::io::ByteView buffer(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));

    mu::engraving::Err err = mu::engraving::Err::FileBadFormat;
    try {
        err = mu::iex::guitarpro::importGTP(score, &buffer, false);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Guitar Pro import failed: ") + e.what());
    }

    if (err != mu::engraving::Err::NoError) {
        throw std::runtime_error("Guitar Pro import failed with error "
                                 + std::to_string(static_cast<int>(err)));
    }
    if (!score->firstMeasure() || score->parts().empty()) {
        throw std::runtime_error("Guitar Pro import produced an empty score");
    }

    // GuitarProReader loads the same style after import. Linked tab staves are
    // deliberately not created; the no-linked-tab default is preserved.
    load_guitarpro_style(score);
}
} // namespace music
