#pragma once

#include <string_view>

namespace engrave {

// Score file formats. Capabilities lists which of them the engine reads and
// which it writes.
enum struct Format {
    Mscz,      // MuseScore archive
    Mscx,      // uncompressed MuseScore XML
    MusicXml,  // uncompressed MusicXML
    Mxl,       // compressed MusicXML
    Midi,      // Standard MIDI File, including karaoke (.kar)
    GuitarPro, // Guitar Pro 1-7 and Power Tab
};

std::string_view to_string(Format format) noexcept;

struct ExportOptions {
    Format format = Format::Mscz;
    bool midi_expand_repeats = true;
    bool midi_rpns = true;
};

} // namespace engrave
