// SPDX-License-Identifier: GPL-3.0-only
//
// Offline driver for the retained audio graph, synthesizers and encoders.
// Services are initialized directly; no RPC channel or controller is installed.
//
// Process-wide audio services and SoundFont resources require one engine per
// process and calls on its owner thread. Each export owns a fresh render graph
// and releases its tracks, synth sources and effects before returning. The
// native runtime must be initialized before initialize_engine_audio().

#pragma once

#include <memory>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "engraving/types/types.h"
#include "midi/miditypes.h"

namespace muse::mpe {
class IArticulationProfilesRepository;
}

namespace mu::engraving {
class MasterScore;
}

namespace music {

// The engine's audio services, SoundFonts and render state. EngineContext owns
// the only instance and creates it idle; initialize_engine_audio() builds the
// services on first use. profiles must outlive it.
class EngineAudio
{
public:
    explicit EngineAudio(const muse::mpe::IArticulationProfilesRepository& profiles);
    ~EngineAudio();

    EngineAudio(const EngineAudio&) = delete;
    EngineAudio& operator=(const EngineAudio&) = delete;

    struct State;
    State& state() const { return *m_state; }

private:
    std::unique_ptr<State> m_state;
};

// Hook for MIDI event goldens. The engine resolves a capturing IMidiOutPort and
// FluidSynth sends every event it actually handles to it (fluidsynth.cpp:249-262,
// after the staff % 16 channel remap), together with `frame`: the 44100 Hz
// output-frame offset of the render chunk in which the event was handled.
//
// The engine does not expose an event's offset inside a chunk to an
// IMidiOutPort, so frame is the start of the 4096-frame render chunk
// (SoundTrackWriter's render step), not the exact sample. Consecutive events
// handled in one chunk therefore share a frame value.
//
// The engine_audio_probe --midi-events JSON schema per event is
//   { "frame": <uint64>,
//     "opcode": <string>,
//     "channel": <0..15>,                 // channel-voice events only
//     "note": <0..127>, "velocity": <0..127>,   // NoteOn/NoteOff
//     "index": <0..127>, "data": <0..127>,      // ControlChange
//     "pitchBend": <int>,                       // PitchBend
//     "program": <0..127> }                     // ProgramChange
// and the probe additionally reports midiEventTotal (all captured events),
// midiEventsTruncated and at most the first 1,000,000 entries in midiEvents.
//
// Events applied by FluidSynth outside handleEvent are not captured: setup-time
// program/bank/CC7/CC11 from setupSound() (fluidsynth.cpp:338-370), and the
// expression/pitch-bend changes from updateExpressionLevels()/
// setPlaybackPosition() (fluidsynth.cpp:393-407, 470-505). doFlushSound()
// (fluidsynth.cpp:166-219) does send all-notes-off/sustain/pitch-bend to the
// port, but only for the last played staff and it returns early while no event
// has been handled yet (lastStaff() == -1, fluidsequencer.h:52,83), so the
// pre-render flush adds no events. An empty function disables capture. Events
// are sent on the engine thread; the hook must not re-enter the engine.
using EngineAudioMidiEventHook = std::function<void(const muse::midi::Event&, std::uint64_t frame)>;
void set_engine_audio_midi_event_hook(EngineAudio& audio, EngineAudioMidiEventHook hook);

// Initialises the engine's audio services and loads its SoundFonts:
//   Capturing MIDI output, owned audio services, explicit
//   configuration/synth/engine initialization, then SoundFont loading.
// The first font of the list is the default: every track plays it unless its
// request names another index. A SoundFont's resource id is its complete basename
// ("FluidR3Mono_GM" for ".../FluidR3Mono_GM.sf3"), matching
// FluidSoundFontParser::parseSoundFont (fluidsoundfontparser.cpp:70) and
// FluidResolver::refresh (fluidresolver.cpp:102-145).
//
// The sample rate is fixed here for the life of the process, like the context
// output spec it feeds; every render the engine writes is at that rate.
//
// Returns false and fills `error` (when not null) if the engine was already
// initialised with a different list or rate, a path is not .sf2/.sf3, the
// native runtime/IFileSystem is missing, a font cannot be parsed, or a parsed
// font does not show up as an engine input resource. A failed call after the
// engine services were constructed leaves the process without a usable
// engine; exit instead of retrying.
struct EngineAudioSetup {
    std::vector<std::string> soundfonts; // in order; the first is the default
    int sample_rate = 44100;
};
bool initialize_engine_audio(EngineAudio& audio, const EngineAudioSetup& setup, std::string* error = nullptr);
// One SoundFont at 44100 Hz.
bool initialize_engine_audio(EngineAudio& audio, const std::string& soundfont_path, std::string* error = nullptr);
bool engine_audio_initialized(const EngineAudio& audio);

// SoundFont identity of the successfully initialised engine's default (first)
// font (empty when not initialised). The URI is the encoded "file://" form used
// as the repository key; it lets callers verify the Windows drive-letter/UTF-8
// round trip.
std::string engine_soundfont_resource_id(const EngineAudio& audio);
std::string engine_soundfont_uri(const EngineAudio& audio);

enum class EngineAudioFormat {
    Wav,
    Flac,
    Ogg,
    Mp3,
    Aac,
};

// Per-track mix override. Entries are indexed by the public trackId returned
// by engine_audio_tracks() (and score_audio_tracks / metadata.tracks), i.e. by
// position in the selected score's track list. Tracks without an entry use
// neutral values. `muted` is folded with the derived forceMute (any solo and
// this track is not solo) and with the engine's chord-symbol/metronome mute
// rules; see export_score_audio().
struct EngineAudioTrackParams {
    double volume_db = 0.0;
    double balance = 0.0;
    bool solo = false;
    bool muted = false;
    // Index into EngineAudioSetup::soundfonts, or -1 for the default (the
    // first). Every track resolves its own Fluid source from it.
    int soundfont = -1;
};

struct AudioRenderRequest {
    EngineAudioFormat format = EngineAudioFormat::Wav;
    std::string output_path;
    // -1 renders the master score. A non-negative value renders the master
    // score with every part that is absent from, or invisible in, that excerpt
    // muted, like MasterNotation::initNotationSoloMuteState
    // (masternotation.cpp:748-770).
    int excerpt_id = -1;
    bool use_repeats = true;
    double master_volume_db = 0.0;
    double master_balance = 0.0;
    // Indexed by engine_audio_tracks(master, excerpt_id) order.
    std::vector<EngineAudioTrackParams> tracks;
    // Encoder settings. bit_rate is in kbps and applies to MP3, Ogg and AAC;
    // bit_depth is 16 or 24 for WAV and FLAC, and 32 for a Float32 WAV. 0 keeps
    // the format's default (128 kbps; Float32 WAV, Int16 FLAC).
    int bit_rate = 0;
    int bit_depth = 0;
    // The Muse Reverb aux send: on by default at 0.30, as desktop mixes; off
    // sends nothing to the reverb return and renders the dry mix.
    bool reverb = true;
    double reverb_send = 0.30;
    // Kept for source compatibility. 4.7.6 waited for saveSoundTrack()'s
    // promise and could at least catch a promise that never ran; 5.0's
    // SoundTrackWriter renders synchronously inside export_score_audio()
    // (soundtrackwriter.cpp:96-121 -> 133-207), so this driver no longer pumps
    // an event loop and cannot honour a timeout. A caller that needs a deadline
    // must enforce it out of process: for the synchronous export the parent's
    // duration-derived deadline is the real limit (noqt_engine.cpp).
    std::int64_t timeout_ms = 0;
};

struct AudioRenderResult {
    // Why a render failed. EngineFailure is the only kind the caller cannot act
    // on: the engine's graph, synthesizer or encoder failed.
    enum class Failure {
        None,
        InvalidScore,  // nothing to play, or no length to play it
        ResourceLimit, // the projected output exceeds the 1 GiB cap
        EngineFailure,
    };

    bool ok = false;
    Failure failure = Failure::None;
    std::string error;
    // Total sample frames the writer emits: (repeat-expanded play time + 3 s,
    // truncated to milliseconds) at 44100 Hz. Computed from the same duration
    // handed to SoundTrackWriter (engine_audio.cpp); the writer has no
    // frames-written return value.
    std::uint64_t frames = 0;
    // Repeat-expanded score play time, without the 3 s tail.
    double play_time_secs = 0.0;
    // Audio span of the written file: play_time_secs + 3 s, in milliseconds.
    double duration_secs = 0.0;
    // File size after a successful export; 0 when the file does not exist.
    std::uint64_t bytes = 0;
};

// Renders `master` with the engine and writes one audio file. The request
// format selects the upstream encoder defaults, which bit_rate/bit_depth can
// override (engine_audio.cpp validates neither: the caller does).
//   WAV  Float32, FLAC Int16, MP3 128 kbps, OGG 128 kbps (Opus in Ogg),
//   AAC  128 kbps (ADTS; desktop AacWriter reuses exportMp3Bitrate).
// The engine renders at the rate initialize_engine_audio() fixed, in 4096-frame
// chunks. Ogg/Opus is resampled to 48 kHz inside libopusenc (upstream OggWriter
// behavior too), so the decoded Ogg stream is 48 kHz while its duration follows
// the engine render.
//
// A PlaybackModel supplies repeat-expanded MPE events for the duration of the
// owned OfflineRenderSession. Tracks use explicit SoundFont resources and two
// auxiliary returns (Muse Reverb and an empty return). The retained nested
// master chains preserve the existing gain initialization and mix behavior.
// The writer clears its additive mix buffer before each 4096-frame block,
// finalizes the encoder, and returns through the existing public ByteSink
// adapter (which uses a seekable temporary file for header finalization).
//
// The metronome and chord-symbol tracks are added when the PlaybackModel
// reports them, but are always muted (the metronome/chord playback switches
// are off, like PlaybackController::trackOutputParams). request.tracks is
// indexed by the track list for the same excerpt (engine_audio_tracks);
// tracks outside the excerpt keep neutral values but are muted as described
// above.
//
// Never throws for a bad score/format/engine state; inspect AudioRenderResult.
AudioRenderResult export_score_audio(EngineAudio& audio, mu::engraving::MasterScore* master,
                                     const AudioRenderRequest& request);

// One playable track for score_audio_tracks. `id` is the raw score identity
// (muse::ID hex string / muse::String); the public trackId is this entry's
// zero-based index in the list. The chord-symbol pseudo track has
// chord_symbols == true. The metronome pseudo track is not listed because it
// is not addressable through a part/instrument identity.
struct EngineAudioTrackInfo {
    mu::engraving::InstrumentTrackId id;
    std::string part_id;
    std::string instrument_id;
    std::string part_name;
    std::string instrument_name;
    bool chord_symbols = false;
    // Zero-based index in MasterScore::parts().
    std::size_t part_index = 0;
};

// Lists the selected score's parts, their instrument tracks (one per
// InstrumentTrackId, including instrument changes) and chord-symbol
// pseudo-tracks in deterministic score order; the list index is the public
// trackId. excerpt_id == -1 lists the master score; a non-negative value lists
// only the excerpt's parts, matching what export_score_audio() renders.
// Throws std::invalid_argument for a null master or an invalid excerpt id.
std::vector<EngineAudioTrackInfo> engine_audio_tracks(mu::engraving::MasterScore* master,
                                                      int excerpt_id = -1);

} // namespace music
