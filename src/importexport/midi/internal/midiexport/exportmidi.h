/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef EXPORTMIDI_H
#define EXPORTMIDI_H

#ifdef NO_QT_SUPPORT
#include "io/path.h"
#else
#include <QFile>
#endif

#include "../midishared/midifile.h"
#include "engraving/compat/midi/pausemap.h"
#include "engraving/compat/midi/compatmidirenderinternal.h"

namespace mu::engraving {
class Score;
class SynthesizerState;
}

namespace mu::iex::midi {
#ifdef NO_QT_SUPPORT
using MidiPath = muse::io::path_t;
#else
using MidiPath = QString;
#endif

//---------------------------------------------------------
//   ExportMidi
//---------------------------------------------------------

class ExportMidi
{
public:
    ExportMidi(engraving::Score* s) { m_score = s; }
    bool write(const MidiPath& name, bool midiExpandRepeats, bool exportRPNs);
    bool write(MidiIODevice* device, bool midiExpandRepeats, bool exportRPNs);
    bool write(const MidiPath& name, bool midiExpandRepeats, bool exportRPNs, const engraving::SynthesizerState& synthState);
    bool write(MidiIODevice* device, bool midiExpandRepeats, bool exportRPNs, const engraving::SynthesizerState& synthState);

private:
    void writeHeader(const engraving::CompatMidiRendererInternal::Context& context);

#ifndef NO_QT_SUPPORT
    QFile m_file;
#endif
    MidiFile m_midiFile;
    engraving::Score* m_score = nullptr;
};
}
#endif // EXPORTMIDI_H
