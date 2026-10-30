#pragma once

#include <cstdint>

namespace music {

// Encoded audio families and their hard cap. Non-audio exports keep the
// previous 256 MiB bound. Audio renders grow with duration: WAV Float32 stereo
// at 44100 Hz needs about 8 bytes per frame, so 1 GiB is roughly 50 minutes.
// The worker applies the projected size before rendering and the parent checks
// the finished file, so an oversized render fails before it fills the disk.
enum class AudioOutputCodec {
    Wav,
    Flac,
    Ogg,
    Mp3,
    Aac,
};

inline constexpr std::uint64_t kAudioOutputLimit = 1024ULL * 1024 * 1024;
inline constexpr std::uint64_t kNonAudioOutputLimit = 256ULL * 1024 * 1024;

// Upper bound of the file an audio export of `duration_secs` will produce with
// the encoder settings in engine_audio.cpp: stereo output at `sample_rate` (44100
// unless the engine renders another rate), FLAC Int16, and 128 kbps for
// Ogg/MP3/AAC (engine_audio.h). The bound is deliberately loose (1 MiB of
// container/header slack); a real encode is never larger.
inline std::uint64_t projected_audio_bytes(AudioOutputCodec codec, double duration_secs,
                                           int sample_rate = 44100) noexcept
{
    if (!(duration_secs > 0.0) || sample_rate <= 0) {
        return 0;
    }
    const auto frames = static_cast<std::uint64_t>(duration_secs * sample_rate);
    switch (codec) {
    case AudioOutputCodec::Wav:
        return 44 + frames * 8;                                      // Float32 stereo
    case AudioOutputCodec::Flac:
        return 1024 * 1024 + frames * 4;                             // Int16, uncompressed worst case
    case AudioOutputCodec::Ogg:
    case AudioOutputCodec::Mp3:
    case AudioOutputCodec::Aac:
        return 1024 * 1024 + static_cast<std::uint64_t>(duration_secs * 16000.0); // 128 kbps
    }
    return 0;
}

} // namespace music
