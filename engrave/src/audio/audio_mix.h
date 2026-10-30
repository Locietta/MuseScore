#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace music {

// Per-track output values used by both the score-facing mix API and the
// MuseScore audio-engine request. Values are the score/audiosettings.json
// representation: volume in decibels and balance in [-1, 1].
struct AudioOutputParams {
    double volume_db = 0;
    double balance = 0;
    bool solo = false;
    bool muted = false;
    bool force_mute = false;
    bool operator==(const AudioOutputParams&) const = default;
};
struct AudioTrackParams {
    std::int64_t track_id = 0;
    AudioOutputParams output;
    bool operator==(const AudioTrackParams&) const = default;
};
struct AudioMix {
    AudioOutputParams master;
    std::vector<AudioTrackParams> tracks;
    bool operator==(const AudioMix&) const = default;
};

// Recomputes forceMute as playback does: when any track is soloed, every
// non-solo track is force-muted. The stored mute flags are left untouched.
inline void derive_force_mute(AudioMix& mix) noexcept
{
    bool has_solo = false;
    for (const AudioTrackParams& track : mix.tracks) {
        if (track.output.solo) {
            has_solo = true;
            break;
        }
    }
    for (AudioTrackParams& track : mix.tracks) {
        track.output.force_mute = has_solo && !track.output.solo;
    }
}

// True when master and every track use neutral output parameters.
inline bool is_neutral_mix(const AudioMix& mix) noexcept
{
    const auto neutral = [](const AudioOutputParams& output) {
        return output.volume_db == 0.0 && output.balance == 0.0 && !output.solo && !output.muted
               && !output.force_mute;
    };
    if (!neutral(mix.master)) return false;
    for (const AudioTrackParams& track : mix.tracks) {
        if (!neutral(track.output)) return false;
    }
    return true;
}

} // namespace music
