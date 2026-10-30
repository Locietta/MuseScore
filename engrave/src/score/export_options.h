#pragma once

#include "audio_mix.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace music {

// Field-by-field audio mix override supplied with one export. Unspecified
// fields keep the document's default mix (neutral unless the source MSCZ
// carried an audiosettings.json). forceMute is never an input: it is derived
// from the solo flags after every override has been applied.
struct AudioOutputOverride {
    std::optional<double> volume_db;
    std::optional<double> balance;
    std::optional<bool> solo;
    std::optional<bool> muted;
};

struct AudioTrackMixOverride {
    std::int64_t track_id = 0;
    AudioOutputOverride output;
};

struct AudioMixOverride {
    AudioOutputOverride master;
    std::vector<AudioTrackMixOverride> tracks;
};

struct ExportOptions {
    int page = 0;
    int excerpt_id = -1;
    int png_dpi = 144;
    bool draw_page_background = false;
    bool transparent = true;
    bool midi_expand_repeats = true;
    bool midi_export_rpns = true;
    // Client export override for audio formats.
    std::optional<AudioMixOverride> mix;
    // Fully resolved mix supplied by the host to a background worker. The
    // worker's own document snapshot has no audiosettings.json, so the parent
    // sends the effective mix instead of recomputing defaults there.
    std::optional<AudioMix> explicit_mix;
};

} // namespace music
