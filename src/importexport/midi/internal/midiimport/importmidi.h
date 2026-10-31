// SPDX-License-Identifier: GPL-3.0-only
// Native buffer entry for the pinned upstream MIDI importer.
#ifndef IMPORTMIDI_H
#define IMPORTMIDI_H

#include <string>

#include "engraving/engravingerrors.h"

namespace mu::engraving {
class MasterScore;
}

namespace mu::iex::midi {
//! Import a complete Standard MIDI File / Karaoke file from memory.
//!
//! The entry is Qt-free and performs a one-shot import: it installs a temporary
//! entry in the process-global MidiImportOperations data, runs the ordinary
//! converter pipeline, and removes the entry before returning (including when
//! a conversion exception unwinds).  No filesystem access is performed.
engraving::Err importMidiFromBuffer(engraving::MasterScore* score, const std::string& bytes);
} // namespace mu::iex::midi

#endif // IMPORTMIDI_H
