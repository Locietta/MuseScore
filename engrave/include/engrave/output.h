#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <engrave/handle.h>

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

// What render() draws.
enum struct RenderFormat {
    Pdf, // one document holding the requested pages
    Png, // one image per page
    Svg, // one drawing per page
};

std::string_view to_string(RenderFormat format) noexcept;

// The renderer's own bounds, the same ones the A2 host enforced: PNG takes 36 to
// 1200 dpi, no page side may exceed 32767 pixels and no page more than 64e6
// pixels. A page beyond them is a ResourceLimit.
inline constexpr std::int32_t min_render_dpi = 36;
inline constexpr std::int32_t max_render_dpi = 1200;
inline constexpr std::int64_t max_render_pixels = 64'000'000;

struct RenderOptions {
    ScoreSel score;
    RenderFormat format = RenderFormat::Pdf;
    // Inclusive, 0-based page range. last_page == -1 means the score's last page.
    std::int32_t first_page = 0;
    std::int32_t last_page = -1;
    // PNG only: 36 to 1200 dpi. The other formats are drawn at 72 dpi (PDF
    // points, SVG units), so a dpi with them is InvalidArgument.
    std::int32_t dpi = 144;
    // Draw the page's own background (its colour from the score's style) in PNG
    // and SVG; a PDF always prints it. Off by default, like A2's export.
    bool background = false;
    // PNG only: leave a transparent page transparent instead of painting white
    // behind it.
    bool transparent = true;
};

// What render() produced: exactly one of the two is filled, according to the
// requested format.
struct RenderOutput {
    std::vector<std::byte> document;           // Pdf: the whole document
    std::vector<std::vector<std::byte>> pages; // Png, Svg: one per page, in page order
};

} // namespace engrave
