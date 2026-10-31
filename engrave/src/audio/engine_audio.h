// SPDX-License-Identifier: GPL-3.0-only
//
// Headless driver for MuseScore 5.0's audio engine (roadmap-engrave A2).
//
// 4.7.6 drove `audio::engine::IEnginePlayback` with one TrackSequence per job.
// 5.0 removed that interface and the sequence concept: the engine is a node
// graph owned by an `engine::AudioContext` (one per context id), fed by
// `engine::AudioEngine`, and rendered by `soundtrack::SoundTrackWriter`. This
// driver builds that graph in-process, in one thread, and follows the 5.0 call
// sequence (paths relative to the 5.0 tree):
//   - muse/framework/audio/engine/enginesetup.cpp:46-74
//       (EngineGlobalSetup::registerExports/resolveImports: the 5.0 replacement
//        for 4.7.6 EngineController::registerExports)
//   - muse/framework/audio/engine/internal/enginecontroller.cpp:84-98
//       (EngineController::init: config, synth resolver, audio engine,
//        transport event dispatcher)
//   - muse/framework/audio/engine/internal/audiocontext.cpp:44-79
//       (the context's own master chain: TrackChain + AutomationControlNode +
//        SignalNode over the context mixer)
//   - muse/framework/audio/engine/internal/audiocontext.cpp:147-240, 241-281
//       (AudioContext::addTrack/addAuxTrack: one TrackChain per track)
//   - muse/framework/audio/engine/internal/audiocontext.cpp:777-812, 909-953
//       (AudioContext::saveSoundTrack/doSaveSoundTrack: the offline render)
//   - muse/framework/audio/engine/internal/export/soundtrackwriter.cpp:59-121,
//     133-207 (SoundTrackWriter: the render loop and the encoders)
//   - src/playback/internal/playbackcontroller.cpp:63-73, 1188-1360,
//     1717-1719, 1811-1824 (desktop mix setup: reverb aux, per-track params,
//     solo folded into mute; only used as a reference - the desktop side is an
//     RPC client)
//
// The engine keeps process-wide state (global IoC exports, the global FluidSynth
// Vorbis decoder, the lock-free SoundFont cache, static track ids) and we run it
// single-threaded: MUSE_THREADS_SUPPORT stays undefined, so the mixer never
// starts worker threads. Therefore:
//   - only one engine may exist per process;
//   - initialize_engine_audio() must be called once, before any export;
//   - every engine call (bootstrap, export, track listing) must happen on the
//     same OS thread. It is not safe to export from two threads or to run two
//     exports at the same time;
//   - a process may render any number of audio jobs: export_score_audio() drops
//     what the previous job left in the shared context (its tracks and sources,
//     the synthesizer instances behind them, and the fx chains, Muse Reverb
//     included) before it builds the graph. A second render therefore starts
//     from the same state as the first, and the caller gets the same bytes it
//     would get from a fresh process (engrave_output_test's D4 test).
//
// initialize_engine_audio() needs the native runtime already set up
// (music::initialize_native_runtime): Uri::fromLocalFile resolves through
// IFileSystem and PlaybackModel resolves the MPE articulation profile
// repository from the global IoC.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "audio/common/rpc/irpcchannel.h"
#include "engraving/types/types.h"
#include "midi/miditypes.h"

namespace mu::engraving {
class MasterScore;
}

namespace music {

// No-op rpc::IRpcChannel for the in-process engine.
//
// 5.0 changed the interface: `onMethod`/`listenAll` are gone, requests and
// notifications are registered with `onRequest`/`onNotification`, and handlers
// return a response message that the caller sends back (irpcchannel.h:400-407).
// The host never invokes a handler: EngineController installs two request
// handlers in its constructor (enginecontroller.cpp:46-64) and
// EngineRpcController installs ~50 more when the engine process starts
// (enginerpccontroller.cpp:68 onwards), but this driver talks to the engine
// through IAudioEngine/IAudioContext directly. Sending stays a no-op.
//
// SoundTrackWriter still calls process() after every rendered chunk
// (soundtrackwriter.cpp:160,182,199); the export driver uses that to count
// rendered output frames for the MIDI event hook.
class NullRpcChannel final : public muse::audio::rpc::IRpcChannel
{
public:
    // Called once after every rendered chunk (see above).
    std::function<void()> onProcess;

    void setupOnMain() override {}
    void setupOnEngine() override {}

    void process() override { if (onProcess) onProcess(); }

    void send(const muse::audio::rpc::Msg&, const muse::audio::rpc::ResponseHandler& = nullptr) override {}
    void onRequest(muse::audio::rpc::CtxId, muse::audio::rpc::MsgCode, muse::audio::rpc::RequestHandler) override {}
    void onNotification(muse::audio::rpc::CtxId, muse::audio::rpc::MsgCode,
                        muse::audio::rpc::NotificationHandler) override {}

    void addStream(std::shared_ptr<muse::audio::rpc::IRpcStream>) override {}
    void removeStream(muse::audio::rpc::StreamId) override {}
    void sendStream(const muse::audio::rpc::StreamMsg&) override {}
    void onStream(muse::audio::rpc::StreamId, muse::audio::rpc::StreamHandler) override {}
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
void set_engine_audio_midi_event_hook(EngineAudioMidiEventHook hook);

// Initialises the process-wide engine and loads its SoundFonts:
//   AudioSanitizer setup, NullRpcChannel registered as rpc::IRpcChannel,
//   capturing IMidiOutPort registered, EngineGlobalSetup::{registerExports,
//   resolveImports} (AudioEngine, AudioFactory, AudioEngineConfiguration, SynthResolver
//   with the Fluid resolver, FxResolver with MuseFX, SoundFontRepository,
//   TransportEventsDispatcher), EngineController::init(<setup.sample_rate, 4096, 2>,
//   {useSoundFontLowPassFilter=true}), addSoundFont for every font, one AudioContext
//   (id 1) whose availableInputResources() must report each of them.
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
bool initialize_engine_audio(const EngineAudioSetup& setup, std::string* error = nullptr);
// One SoundFont at 44100 Hz.
bool initialize_engine_audio(const std::string& soundfont_path, std::string* error = nullptr);
bool engine_audio_initialized();

// SoundFont identity of the successfully initialised engine's default (first)
// font (empty when not initialised). The URI is the encoded "file://" form used
// as the repository key; it lets callers verify the Windows drive-letter/UTF-8
// round trip.
std::string engine_soundfont_resource_id();
std::string engine_soundfont_uri();

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
// Setup mirrors desktop playback/export:
//   the context's previous tracks, sources and fx are dropped first, so every
//   render starts from the state a fresh process has;
//   PlaybackModel on `master` (repeats as requested, chord symbols off,
//   metronome off) kept alive for the whole export;
//   the context is stopped and seeked to zero (audiocontext.cpp:785-793);
//   addTrack per playable InstrumentTrackId with resolveTrackPlaybackData() and
//   an explicit Fluid source resource from the track's SoundFont (so a track
//   never falls back to the nonexistent default "MS Basic" resource),
//   TrackParams::auxSends = {{request.reverb_send,true}, {0,true}}; aux 0 is
//   Muse Reverb (fx chain copied from playbackcontroller.cpp:63-73), aux 1 is
//   an empty output;
//   setDuration(total render time); SoundTrackWriter over the context.
//
// Master volume/balance/mute are applied by an explicit master TrackChain that
// wraps the context (the same TrackChain/AutomationControlNode/SignalNode trio
// AudioContext::init() builds at audiocontext.cpp:44-79), because
// AudioContext::saveSoundTrack() renders from the context mixer, i.e. from
// below the context's own master chain (audiocontext.cpp:915): the 5.0 export
// path drops master control params. See engine_audio.cpp for the details.
//
// The metronome and chord-symbol tracks are added when the PlaybackModel
// reports them, but are always muted (the metronome/chord playback switches
// are off, like PlaybackController::trackOutputParams). request.tracks is
// indexed by the track list for the same excerpt (engine_audio_tracks);
// tracks outside the excerpt keep neutral values but are muted as described
// above.
//
// Never throws for a bad score/format/engine state; inspect AudioRenderResult.
AudioRenderResult export_score_audio(mu::engraving::MasterScore* master, const AudioRenderRequest& request);

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
