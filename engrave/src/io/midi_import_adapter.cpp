#include "midi_import_adapter.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "importexport/midi/internal/midiimport/importmidi.h"

namespace music {
namespace {
constexpr std::size_t MAX_IMPORT_BYTES = 8 * 1024 * 1024;

// Meta events whose payload length is read without a bounds/format check by the
// importer.  Keep these in sync with MidiFile::readTrack/readEvent users.
constexpr std::uint8_t SMF_META_END_OF_TRACK = 0x2f;
constexpr std::uint8_t SMF_META_TEMPO = 0x51;
constexpr std::uint8_t SMF_META_TIME_SIGNATURE = 0x58;
constexpr std::uint8_t SMF_META_KEY_SIGNATURE = 0x59;

std::uint8_t to_byte(char c)
{
    return static_cast<std::uint8_t>(static_cast<unsigned char>(c));
}

std::uint16_t read_be16(std::string_view data, std::size_t pos)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(to_byte(data[pos])) << 8)
                                      | to_byte(data[pos + 1]));
}

std::uint32_t read_be32(std::string_view data, std::size_t pos)
{
    return (static_cast<std::uint32_t>(to_byte(data[pos])) << 24)
           | (static_cast<std::uint32_t>(to_byte(data[pos + 1])) << 16)
           | (static_cast<std::uint32_t>(to_byte(data[pos + 2])) << 8)
           | static_cast<std::uint32_t>(to_byte(data[pos + 3]));
}

bool read_byte(std::string_view data, std::size_t end, std::size_t& pos, std::uint8_t& byte)
{
    if (pos >= end) {
        return false;
    }
    byte = to_byte(data[pos]);
    ++pos;
    return true;
}

// SMF variable-length quantities are limited to four bytes (28 payload bits).
// MidiFile::getvl accepts up to sixteen, so reject longer encodings here before
// they can overflow its signed int accumulator.
bool read_vlq(std::string_view data, std::size_t end, std::size_t& pos, std::uint32_t& value)
{
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
        std::uint8_t byte = 0;
        if (!read_byte(data, end, pos, byte)) {
            return false;
        }
        result = (result << 7) | (byte & 0x7f);
        if ((byte & 0x80) == 0) {
            value = result;
            return true;
        }
    }
    return false;
}

int channel_data_bytes(std::uint8_t status)
{
    // Only channel messages (0x80..0xef) reach this function.
    const std::uint8_t kind = status & 0xf0;
    return (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
}

bool read_channel_data(std::string_view data, std::size_t end, std::size_t& pos, int count)
{
    for (int i = 0; i < count; ++i) {
        std::uint8_t byte = 0;
        if (!read_byte(data, end, pos, byte) || (byte & 0x80)) {
            return false;
        }
    }
    return true;
}

bool valid_meta_length(std::uint8_t meta_type, std::uint32_t length)
{
    switch (meta_type) {
    case SMF_META_END_OF_TRACK:
        return length == 0;
    case SMF_META_TEMPO:
        return length == 3;
    case SMF_META_TIME_SIGNATURE:
        return length == 4;
    case SMF_META_KEY_SIGNATURE:
        return length == 2;
    case 0x20: // MIDI channel prefix
    case 0x21: // MIDI port
        return length == 1;
    default:
        return true;
    }
}

// Division is either ticks per quarter note (nonzero) or SMPTE (fps 24/25/29/30
// with a nonzero ticks-per-frame).  The importer divides by the resulting value.
bool valid_division(std::uint16_t division)
{
    if ((division & 0x8000u) == 0) {
        return division != 0;
    }

    const int fps = 256 - static_cast<int>(division >> 8);
    const int ticks_per_frame = static_cast<int>(division & 0xffu);
    const bool known_fps = fps == 24 || fps == 25 || fps == 29 || fps == 30;
    return known_fps && ticks_per_frame != 0;
}

// Validates one MTrk chunk body in [begin, end).  The original parser reads
// events without an end boundary and stops only at End Of Track, so every byte
// read here is confined to the declared chunk and EOT is mandatory.  Bytes after
// EOT are left alone: MidiFile::readTrack also skips them.
bool validate_track(std::string_view data, std::size_t begin, std::size_t end)
{
    std::size_t pos = begin;
    int running_status = -1;
    std::int64_t ticks = 0;
    bool saw_end_of_track = false;

    while (pos < end) {
        std::uint32_t delta = 0;
        if (!read_vlq(data, end, pos, delta)) {
            return false;
        }
        if (ticks > std::numeric_limits<int>::max() - static_cast<std::int64_t>(delta)) {
            return false;
        }
        ticks += static_cast<std::int64_t>(delta);

        std::uint8_t status = 0;
        if (!read_byte(data, end, pos, status)) {
            return false;
        }

        if (status < 0x80) {
            // Running status.  MidiFile keeps sstatus across meta and sysex
            // events (it only clears `status`), so mirror that per track.
            if (running_status < 0) {
                return false;
            }
            if (!read_channel_data(data, end, pos,
                                   channel_data_bytes(static_cast<std::uint8_t>(running_status)) - 1)) {
                return false;
            }
            continue;
        }

        if (status <= 0xef) {
            running_status = status;
            if (!read_channel_data(data, end, pos, channel_data_bytes(status))) {
                return false;
            }
            continue;
        }

        if (status == 0xff) {
            std::uint8_t meta_type = 0;
            std::uint32_t length = 0;
            if (!read_byte(data, end, pos, meta_type)
                || !read_vlq(data, end, pos, length)
                || !valid_meta_length(meta_type, length)
                || length > end - pos) {
                return false;
            }
            // The importer divides by tempo and computes 2^denominator using
            // a signed int. Reject values that would make those operations invalid.
            if (meta_type == SMF_META_TEMPO
                && (to_byte(data[pos]) | to_byte(data[pos + 1]) | to_byte(data[pos + 2])) == 0) {
                return false;
            }
            if (meta_type == SMF_META_TIME_SIGNATURE
                && (to_byte(data[pos]) == 0 || to_byte(data[pos + 1]) > 30)) {
                return false;
            }
            pos += length;
            if (meta_type == SMF_META_END_OF_TRACK) {
                saw_end_of_track = true;
                break;
            }
            continue;
        }

        if (status == 0xf0 || status == 0xf7) {
            // System exclusive / escape.  MidiFile requires a nonempty payload;
            // it tolerates a missing trailing 0xf7, so do not require one.
            std::uint32_t length = 0;
            if (!read_vlq(data, end, pos, length) || length == 0 || length > end - pos) {
                return false;
            }
            pos += length;
            continue;
        }

        // System common/realtime status bytes are not valid SMF events.  The
        // original parser skips them as unknown messages; reject them as
        // malformed framing instead.
        return false;
    }

    return saw_end_of_track;
}

// Structural SMF framing validation only.  No smf events are mapped or retained
// here and no allocation is made from an untrusted length.
bool valid_smf(std::string_view data)
{
    if (data.size() < 14 || data.compare(0, 4, "MThd") != 0) {
        return false;
    }
    if (read_be32(data, 4) != 6) {
        return false;
    }

    const std::uint16_t format = read_be16(data, 8);
    const std::uint16_t track_count = read_be16(data, 10);
    const std::uint16_t division = read_be16(data, 12);

    if (format != 0 && format != 1) {
        return false;
    }
    if (track_count == 0 || (format == 0 && track_count != 1)) {
        return false;
    }
    if (!valid_division(division)) {
        return false;
    }

    std::size_t pos = 14;
    for (std::uint16_t i = 0; i < track_count; ++i) {
        if (data.size() - pos < 8 || data.compare(pos, 4, "MTrk") != 0) {
            return false;
        }
        const std::uint32_t chunk_length = read_be32(data, pos + 4);
        const std::size_t begin = pos + 8;
        if (chunk_length > data.size() - begin) {
            return false;
        }
        const std::size_t end = begin + static_cast<std::size_t>(chunk_length);
        if (!validate_track(data, begin, end)) {
            return false;
        }
        pos = end;
    }

    // Trailing bytes after the declared track chunks are ignored by the
    // original reader, so they are not treated as part of the framing.
    return true;
}
} // namespace

mu::engraving::Err import_midi_score(mu::engraving::MasterScore* score, const std::string& bytes)
{
    if (!score || bytes.empty() || bytes.size() > MAX_IMPORT_BYTES) {
        return mu::engraving::Err::FileBadFormat;
    }
    if (!valid_smf(std::string_view(bytes.data(), bytes.size()))) {
        return mu::engraving::Err::FileBadFormat;
    }

    return mu::iex::midi::importMidiFromBuffer(score, bytes);
}
}
