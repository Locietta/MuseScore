#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <engrave/output.h>

namespace engrave {

// The format the content identifies itself as, in this order: a zip whose
// META-INF/container.xml lists an .mscx (MSCZ) or a MusicXML rootfile (MXL),
// or that holds Content/score.gpif (GP7); a MIDI header; an XML document whose
// root is <museScore> (MSCX) or <score-partwise>/<score-timewise> (MusicXML);
// the Guitar Pro and Power Tab magics.
std::optional<Format> sniff_format(const std::string& data);

std::optional<Format> format_from_extension(const std::filesystem::path& name);

} // namespace engrave
