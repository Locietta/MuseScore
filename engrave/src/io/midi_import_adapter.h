#pragma once

#include <string>

#include "engraving/engravingerrors.h"

namespace mu::engraving {
class MasterScore;
}

namespace music {
//! Qt-free typed entry for the MIDI/KAR importer.
//!
//! `bytes` is the complete Standard MIDI File (SMF 0/1) image, including
//! karaoke lyric/text meta events.  No filesystem path or Qt type crosses this
//! boundary.  On success the score has parts, measures, notes, lyrics, keys,
//! tempo and chord symbols populated by the ordinary MuseScore MIDI-import
//! pipeline; the caller remains responsible for layout/setup.
//!
//! The importer keeps process-global options/cache state only for the duration
//! of the call and removes its temporary entry before returning.  A non-NoError
//! result must be treated as a failed import (the score may be partially
//! populated).
mu::engraving::Err import_midi_score(mu::engraving::MasterScore* score, const std::string& bytes);
}
