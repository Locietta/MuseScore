// SPDX-License-Identifier: GPL-3.0-only
//
// Headless driver for MuseScore 5.0's audio engine. See engine_audio.h for the
// lifetime/threading contract, the 5.0 call sequence and the 4.7.6 -> 5.0 API
// mapping.

#include "engine_audio.h"
#include "audio_output.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

#include <fluidsynth.h>

#include "audio/common/audiosanitizer.h"
#include "audio/common/audioutils.h"
#include "audio/common/soundfonttypes.h"
#include "audio/engine/enginesetup.h"
#include "audio/engine/internal/enginecontroller.h"
#include "audio/engine/internal/export/soundtrackwriter.h"
#include "audio/engine/internal/iaudiocontext.h"
#include "audio/engine/internal/iaudioengine.h"
#include "audio/engine/internal/nodes/automationcontrolnode.h"
#include "audio/engine/internal/nodes/signalnode.h"
#include "audio/engine/internal/nodes/trackchain.h"
#include "audio/engine/isoundfontrepository.h"

#include "engraving/dom/excerpt.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/playback/playbackmodel.h"

#include "score_audio.h"
#include "score/score_excerpts.h"

#include "global/io/ifilesystem.h"
#include "global/io/iodevice.h"
#include "global/io/ioretcodes.h"
#include "global/io/path.h"
#include "global/modularity/ioc.h"
#include "global/types/ret.h"
#include "global/types/string.h"
#include "global/types/uri.h"

#include "midi/imidioutport.h"

#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::modularity;
using namespace mu::engraving;

namespace music {
namespace {
constexpr sample_rate_t kSampleRate = 44100;
constexpr samples_t kSamplesPerChannel = 4096;
constexpr audioch_t kAudioChannelCount = 2;
constexpr double kPlaybackTailSecs = 3.0;
constexpr gain_t kDefaultAuxSend = 0.30f;
constexpr const char* kReverbTrackName = "Reverb";
constexpr const char* kAuxTrackName = "Aux 2";
constexpr const char* kMetronomeTrackName = "Metronome";
constexpr const char* kChordsTrackNamePrefix = "Chords.";
// One context per process: the host renders one audio job per worker child
// (worker_protocol.cpp), so the id only has to be non-zero and stable.
constexpr AudioCtxId kAudioCtxId = 1;

// Capturing midi::IMidiOutPort. FluidSynth sends every event it handles (after
// the staff -> channel remap), which is the event stream later golden tests
// compare. A global hook keeps the sink reusable by the probe and by tests.
// s_rendered_frames is the number of output frames already encoded: the writer
// calls rpcChannel()->process() after each chunk, so this is the start of the
// chunk currently being rendered.
EngineAudioMidiEventHook s_midi_event_hook;
std::uint64_t s_rendered_frames = 0;

class CapturingMidiOutPort final : public midi::IMidiOutPort
{
public:
    midi::MidiDeviceList availableDevices() const override { return {}; }
    async::Notification availableDevicesChanged() const override { return {}; }

    Ret connect(const midi::MidiDeviceID&) override { return make_ok(); }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    midi::MidiDeviceID deviceID() const override { return "engine-capture"; }
    async::Notification deviceChanged() const override { return {}; }

    bool supportsMIDI20Output() const override { return false; }

    Ret sendEvent(const midi::Event& event) override
    {
        if (s_midi_event_hook) {
            s_midi_event_hook(event, s_rendered_frames);
        }

        return make_ok();
    }
};

// Encoder destination. 5.0's encoders write through muse::io::IODevice
// (export/wavencoder.cpp:172,198 writes four bytes per sample), but the fork's
// io::File keeps the whole file in a ByteArray and rewrites it on every write
// (global/io/file.cpp:136-146): quadratic in the file size and with the whole
// render resident in memory. 4.7.6's encoders streamed to std::ofstream/FILE*
// instead (4.7.6 export/wavencoder.cpp:223, mp3encoder.cpp:110). This device
// restores that behaviour behind the 5.0 device API: sequential writes are
// buffered by the stream, and only the WAV header patch seeks backwards
// (export/wavencoder.cpp:134-154). Write failures make encode() return 0,
// which SoundTrackWriter reports as Err::ErrorEncode
// (export/soundtrackwriter.cpp:155,177,194).
class StreamingFileDevice final : public io::IODevice
{
public:
    explicit StreamingFileDevice(io::path_t path)
        : m_path(std::move(path)) {}

    ~StreamingFileDevice() override { close(); }

protected:
    bool doOpen(OpenMode mode) override
    {
        if (mode != WriteOnly && mode != ReadWrite && mode != Append) {
            setError(static_cast<int>(io::Err::FSWriteError), "audio output is write-only");
            return false;
        }

        // The path is UTF-8; std::filesystem::path keeps it away from the
        // process ANSI code page on Windows.
        m_stream.open(std::filesystem::u8path(m_path.toStdString()), std::ios::binary | std::ios::trunc);
        if (!m_stream.is_open()) {
            setError(static_cast<int>(io::Err::FSWriteError), "cannot open audio output for writing");
            return false;
        }

        m_size = 0;
        m_streamPos = 0;
        return true;
    }

    void doClose() override
    {
        if (m_stream.is_open()) {
            m_stream.flush();
            m_stream.close();
        }
    }

    size_t dataSize() const override { return m_size; }
    const uint8_t* rawData() const override { return nullptr; }

    bool resizeData(size_t size) override
    {
        m_size = std::max(m_size, size);
        return true;
    }

    size_t writeData(const uint8_t* data, size_t len) override
    {
        if (!m_stream.is_open()) {
            return 0;
        }

        // Only seek for the non-sequential writes (the WAV header patch);
        // std::ofstream::seekp flushes the put buffer, so doing it per write
        // would turn a buffered stream into one syscall per sample.
        if (m_streamPos != pos()) {
            m_stream.seekp(static_cast<std::streamoff>(pos()));
            if (!m_stream) {
                return 0;
            }
        }

        m_stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
        if (!m_stream) {
            return 0;
        }

        m_streamPos = pos() + len;
        return len;
    }

private:
    io::path_t m_path;
    std::ofstream m_stream;
    size_t m_size = 0;
    size_t m_streamPos = 0;
};

// Process-wide state. The services and the engine controller are intentionally
// leaked: the engine owns global IoC exports and static synthesizer state, and
// must outlive every static destructor (FluidResolver's Vorbis decoder is a
// process-wide pointer, fluidresolver.cpp:44-53).
struct EngineState {
    std::shared_ptr<NullRpcChannel> rpcChannel;
    std::shared_ptr<CapturingMidiOutPort> midiOutPort;
    engine::EngineGlobalSetup* engineSetup = nullptr;
    engine::EngineController* controller = nullptr;
    std::shared_ptr<engine::IAudioContext> context;
    AudioInputParams inputParams;
    std::string soundfontPath;
    std::string soundfontUri;
    std::string soundfontId;
};

EngineState& engine_state()
{
    static EngineState state;
    return state;
}

OutputSpec output_spec()
{
    OutputSpec spec;
    spec.sampleRate = kSampleRate;
    spec.samplesPerChannel = kSamplesPerChannel;
    spec.audioChannelCount = kAudioChannelCount;
    return spec;
}

// Mirrors playbackcontroller.cpp:63-73 (makeReverbOutputParams). 4.7.6 wrapped
// the chain in an AudioOutputParams because the engine took output params;
// 5.0's TrackParams carries the fx chain directly.
AudioFxChain make_reverb_fx_chain()
{
    AudioFxParams reverbParams;
    reverbParams.resourceMeta = makeReverbMeta();
    reverbParams.categories.insert(AudioFxCategory::FxReverb);
    reverbParams.chainOrder = 0;
    reverbParams.active = true;

    AudioFxChain chain;
    chain.emplace(reverbParams.chainOrder, std::move(reverbParams));
    return chain;
}

SoundTrackFormat make_soundtrack_format(EngineAudioFormat format)
{
    SoundTrackFormat result;
    result.outputSpec = output_spec();

    switch (format) {
    case EngineAudioFormat::Wav:
        result.type = SoundTrackType::WAV;
        result.sampleFormat = AudioSampleFormat::Float32;
        result.bitRate = 0;
        break;
    case EngineAudioFormat::Flac:
        result.type = SoundTrackType::FLAC;
        result.sampleFormat = AudioSampleFormat::Int16;
        result.bitRate = 0;
        break;
    case EngineAudioFormat::Ogg:
        // Desktop OggWriter hard-codes 128 kbps (oggwriter.cpp:42).
        result.type = SoundTrackType::OGG;
        result.sampleFormat = AudioSampleFormat::Undefined;
        result.bitRate = 128;
        break;
    case EngineAudioFormat::Mp3:
        // Desktop default exportMp3Bitrate is 128
        // (audioexportconfiguration.cpp:44).
        result.type = SoundTrackType::MP3;
        result.sampleFormat = AudioSampleFormat::Undefined;
        result.bitRate = 128;
        break;
    case EngineAudioFormat::Aac:
        // Desktop AacWriter reuses exportMp3Bitrate for the AAC bitrate.
        result.type = SoundTrackType::AAC;
        result.sampleFormat = AudioSampleFormat::Undefined;
        result.bitRate = 128;
        break;
    }

    return result;
}

const char* audio_format_name(EngineAudioFormat format)
{
    switch (format) {
    case EngineAudioFormat::Wav: return "WAV";
    case EngineAudioFormat::Flac: return "FLAC";
    case EngineAudioFormat::Ogg: return "Ogg";
    case EngineAudioFormat::Mp3: return "MP3";
    case EngineAudioFormat::Aac: return "AAC";
    }
    return "audio";
}

AudioOutputCodec audio_output_codec_of(EngineAudioFormat format)
{
    switch (format) {
    case EngineAudioFormat::Wav: return AudioOutputCodec::Wav;
    case EngineAudioFormat::Flac: return AudioOutputCodec::Flac;
    case EngineAudioFormat::Ogg: return AudioOutputCodec::Ogg;
    case EngineAudioFormat::Mp3: return AudioOutputCodec::Mp3;
    case EngineAudioFormat::Aac: return AudioOutputCodec::Aac;
    }
    return AudioOutputCodec::Wav;
}

// Mirrors the desktop mixer title rules in playbackcontroller.cpp:1155-1186
// (addTrack) and resolveAuxTrackTitle() (:77-89). Translation is not available
// in the native host, so the English labels are used verbatim.
std::string track_name(const MasterScore* master, const PlaybackModel& model, const InstrumentTrackId& id)
{
    if (id == model.metronomeTrackId()) {
        return kMetronomeTrackName;
    }

    const Part* part = master->partById(id.partId);
    if (!part) {
        return id.instrumentId.toStdString();
    }

    if (model.isChordSymbolsTrack(id)) {
        return std::string(kChordsTrackNamePrefix) + part->partName().toStdString();
    }

    const Instrument* primary = part->instrument();
    if (primary && primary->id() == id.instrumentId) {
        return part->partName().toStdString();
    }

    if (const Instrument* instrument = part->instrumentById(id.instrumentId)) {
        return "(" + instrument->trackName().toStdString() + ")";
    }

    return id.instrumentId.toStdString();
}

// 4.7.6 filled AudioOutputParams. 5.0 splits it: ControlParams carries only
// volume/balance/mute (audiotypes.h:435-447); solo is a desktop UI concept
// that PlaybackController folds into `muted` before it reaches the engine
// (playbackcontroller.cpp:1811-1824) and `forceMute` does not exist at all.
void set_track_control(ControlParams& control, const EngineAudioTrackParams& params)
{
    control.volume = volume_db_t(static_cast<float>(params.volume_db));
    control.balance = balance_t(static_cast<float>(params.balance));
    control.muted = params.muted;
}

// The explicit master chain this driver renders through.
//
// AudioContext::saveSoundTrack() hands the writer the context mixer
// (audiocontext.cpp:915), i.e. the node *below* the context's own master chain
// (audiocontext.cpp:65-77), so master volume/balance/mute - which 5.0 applies
// with setControlParams(MASTER_TRACK_ID, ...) on that chain
// (audiocontext.cpp:557-566) - never reach the exported file. 4.7.6 applied
// the master params inside Mixer::process (4.7.6 mixer.cpp:583-589). To keep
// that behavior this builds the same chain AudioContext::init() builds and
// makes it the writer's source; the context's internal master chain then runs
// with neutral values and is a no-op.
engine::TrackChainPtr make_master_chain(const std::shared_ptr<engine::IAudioContext>& context,
                                        const AudioRenderRequest& request, std::string& error)
{
    IAudioNodePtr contextNode = std::dynamic_pointer_cast<IAudioNode>(context);
    if (!contextNode) {
        error = "audio context is not an audio node";
        return nullptr;
    }

    engine::AutomationControlNodePtr control = std::make_shared<engine::AutomationControlNode>();
    engine::TrackChainPtr chain = std::make_shared<engine::TrackChain>(MASTER_TRACK_ID, "Master");

    // Output spec and mode first: TrackChain::rebuild() pushes both into every
    // node it adds (nodes/chainnode.h:70-80), so the control node computes its
    // channel gains once, and only the explicit set* calls below can retarget
    // them.
    chain->setOutputSpec(output_spec());
    chain->setMode(ProcessMode::Idle);

    chain->setSource(contextNode);
    chain->setFxChain(nullptr); // 5.0 has no master fx chain (audiocontext.cpp:72)
    chain->setControl(control);
    chain->setSignal(std::make_shared<engine::SignalNode>());
    chain->rebuild();

    // Mirrors AudioContext::onControlParamsChanged() (audiocontext.cpp:310-331)
    // but latches the result instead of smoothing to it. 4.7.6 applied the
    // master params to every chunk without any smoothing (4.7.6
    // mixer.cpp:583-607); 5.0's AutomationControlNode re-targets its channel
    // gains over SmoothLinearValue's 1024 steps (dsp/smoothlinearvalue.h:37-46,
    // ~23 ms at 44.1 kHz), which would fade the master pan in from centre and
    // so leak the panned signal into the silenced channel.
    // updateChannelGains() initializes the gains directly when the automation
    // marker is unset (nodes/automationcontrolnode.cpp:74-96, initWithValue at
    // dsp/smoothlinearvalue.h:108-114) and only the first set* call after a
    // marker reset does so, so the pan is stored first and the volume written
    // once more to re-initialize both channel gains from (volume, pan).
    const volume_db_t masterVolume(static_cast<float>(request.master_volume_db));
    control->setPan(balance_t(static_cast<float>(request.master_balance)));
    control->setMuted(false);
    control->setPlayheadPosition(nullptr);
    control->setVolume(masterVolume);

    return chain;
}

std::uint64_t file_size(const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

void fluid_log_to_logger(int level, const char* message, void*)
{
    switch (level) {
    case FLUID_PANIC:
    case FLUID_ERR:
        LOGE() << message;
        break;
    case FLUID_WARN:
        LOGW() << message;
        break;
    default:
        LOGI() << message;
        break;
    }
}
} // namespace

void set_engine_audio_midi_event_hook(EngineAudioMidiEventHook hook)
{
    s_midi_event_hook = std::move(hook);
}

bool initialize_engine_audio(const std::string& soundfont_path, std::string* error)
{
    EngineState& state = engine_state();
    const auto fail = [error](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return false;
    };

    if (state.controller) {
        if (!state.soundfontId.empty() && state.soundfontPath == soundfont_path) {
            return true;
        }
        if (state.soundfontId.empty()) {
            return fail("audio engine initialisation failed earlier; restart the process");
        }
        return fail("audio engine is already initialised with SoundFont '" + state.soundfontPath
                    + "' (only one engine and one SoundFont per process)");
    }

    if (soundfont_path.empty() || !synth::isSoundFont(io::path_t(soundfont_path))) {
        return fail("SoundFont must be an existing .sf2 or .sf3 file: " + soundfont_path);
    }

    // Uri::fromLocalFile() resolves the absolute path through global IFileSystem.
    if (!globalIoc()->resolve<io::IFileSystem>("engine_audio")) {
        return fail("native runtime (IFileSystem) is not initialised; call initialize_native_runtime() first");
    }

    // 1. Thread identity. Everything below (and every later engine call) runs
    // on this thread; the mixer stays single-threaded because
    // MUSE_THREADS_SUPPORT is undefined in the generated config.
    AudioSanitizer::setupMainThread();
    AudioSanitizer::setupEngineThread();

    // 2. Null rpc channel + capturing MIDI out. Replace pre-registered stubs
    // (e.g. a MIDI module) so the engine and FluidSynth resolve our exports.
    state.rpcChannel = std::make_shared<NullRpcChannel>();
    state.rpcChannel->onProcess = [] { s_rendered_frames += kSamplesPerChannel; };
    globalIoc()->unregister<rpc::IRpcChannel>("engine_audio");
    globalIoc()->registerExport<rpc::IRpcChannel>("engine_audio", state.rpcChannel);

    state.midiOutPort = std::make_shared<CapturingMidiOutPort>();
    globalIoc()->unregister<midi::IMidiOutPort>("engine_audio");
    globalIoc()->registerExport<midi::IMidiOutPort>("engine_audio", state.midiOutPort);

    // 3. Engine services. EngineGlobalSetup is the 5.0 replacement for
    // EngineController::registerExports() (enginesetup.cpp:46-74): it registers
    // AudioEngine, AudioFactory, AudioEngineConfiguration, SynthResolver,
    // FxResolver, SoundFontRepository and TransportEventsDispatcher, and
    // resolveImports() wires the Fluid synthesizer and MuseFX resolvers.
    state.engineSetup = new engine::EngineGlobalSetup();
    state.engineSetup->registerExports();
    state.engineSetup->resolveImports();

    // EngineController keeps the engine-side rpc handlers and owns the engine
    // configuration/initialisation sequence (enginecontroller.cpp:46-98). The
    // host never starts the rpc side, so onStartRunning() is not called.
    auto* controller = new engine::EngineController(state.rpcChannel);
    state.controller = controller;

    AudioEngineConfig config;
    config.useSoundFontLowPassFilter = true; // desktop parity
    controller->init(output_spec(), config);

    // 4. SoundFont. addSoundFont() is synchronous for file: URIs
    // (soundfontrepository.cpp:68-77, 117-168) and refreshes FluidResolver
    // through soundFontsChanged (fluidresolver.cpp:44-50, 102-145).
    auto repository = globalIoc()->resolve<synth::ISoundFontRepository>("engine_audio");
    if (!repository) {
        return fail("audio engine SoundFont repository is not registered");
    }

    // The pinned FluidSynth default sink writes parser errors to stdout, which
    // corrupts a JSON CLI's output. FluidSynth::init() installs its own
    // per-synth callbacks later, so this only covers parsing here.
    fluid_set_log_function(FLUID_PANIC, fluid_log_to_logger, nullptr);
    fluid_set_log_function(FLUID_ERR, fluid_log_to_logger, nullptr);
    fluid_set_log_function(FLUID_WARN, fluid_log_to_logger, nullptr);
    fluid_set_log_function(FLUID_INFO, fluid_log_to_logger, nullptr);

    const Uri uri = Uri::fromLocalFile(io::path_t(soundfont_path));
    repository->addSoundFont(uri);

    const synth::SoundFontsMap& soundfonts = repository->soundFonts();
    auto it = soundfonts.find(uri);
    if (it == soundfonts.end()) {
        return fail("failed to load SoundFont '" + soundfont_path + "'");
    }

    // 5. One audio context for the process. 4.7.6 created a TrackSequence per
    // export instead; in 5.0 the context owns the graph and the master chain
    // (audiocontext.cpp:44-86), and a worker process renders one job.
    auto audioEngine = globalIoc()->resolve<engine::IAudioEngine>("engine_audio");
    if (!audioEngine) {
        return fail("audio engine is not registered");
    }
    RetVal<std::shared_ptr<engine::IAudioContext> > context = audioEngine->addAudioContext(kAudioCtxId);
    if (!context.ret || !context.val) {
        return fail("failed to create an audio context: " + context.ret.toString());
    }

    // 6. Explicit source resource for every track, taken from the engine's own
    // resource list (FluidResolver::refresh, fluidresolver.cpp:102-145). Without
    // it a track would resolve to the engine default ("MS Basic",
    // audioengineconfiguration.cpp:35-41, 103-109), which is absent from this
    // process, and the track would be silent.
    const AudioResourceMetaList resources = context.val->availableInputResources();
    for (const AudioResourceMeta& meta : resources) {
        if (meta.id != it->second.name || !isResourceType(meta, AudioResourceType::FluidSoundfont)) {
            continue;
        }

        state.inputParams = AudioInputParams();
        state.inputParams.resourceMeta = meta;

        state.context = context.val;
        state.soundfontPath = soundfont_path;
        state.soundfontUri = uri.toString();
        state.soundfontId = it->second.name;
        return true;
    }

    return fail("SoundFont '" + soundfont_path + "' is not available as an audio input resource");
}

bool engine_audio_initialized()
{
    const EngineState& state = engine_state();
    return state.controller != nullptr && !state.soundfontId.empty();
}

std::string engine_soundfont_resource_id()
{
    return engine_state().soundfontId;
}

std::string engine_soundfont_uri()
{
    return engine_state().soundfontUri;
}

AudioRenderResult export_score_audio(MasterScore* master, const AudioRenderRequest& request)
{
    AudioRenderResult result;
    const auto fail = [&result](std::string message) {
        result.ok = false;
        result.error = std::move(message);
        return result;
    };

    const EngineState& state = engine_state();
    if (!state.controller || !state.context || state.soundfontId.empty()) {
        return fail("audio engine is not initialised");
    }
    if (!master) {
        return fail("score is null");
    }
    if (request.output_path.empty()) {
        return fail("output path is empty");
    }

    MasterScore* masterScore = master;
    engine::IAudioContext& context = *state.context;

    // Excerpt selection mutes every master part that is absent from, or
    // invisible in, the excerpt (masternotation.cpp:748-770).
    std::set<ID> mutedParts;
    bool excerptSelected = false;
    if (request.excerpt_id >= 0) {
        const std::vector<Excerpt*>& excerpts = masterScore->excerpts();
        if (static_cast<std::size_t>(request.excerpt_id) >= excerpts.size()) {
            return fail("excerpt id is out of range");
        }

        const Excerpt* excerpt = excerpts[static_cast<std::size_t>(request.excerpt_id)];
        if (!excerpt) {
            return fail("excerpt entry is null");
        }

        const Score* excerptScore = excerpt->excerptScore();
        if (!excerptScore || excerptScore->masterScore() != masterScore) {
            return fail("excerpt has no score owned by this master score");
        }

        for (const Part* part : masterScore->parts()) {
            if (!part) {
                continue;
            }
            const Part* excerptPart = excerptScore->partById(part->id());
            if (!excerptPart || !excerptPart->isVisible()) {
                mutedParts.insert(part->id());
            }
        }
        excerptSelected = true;
    }

    // PlaybackModel stays alive until the render has consumed the playback data
    // streams; the audio tracks reference them.
    PlaybackModel model(modularity::globalCtx());
    model.setPlayRepeats(request.use_repeats);
    model.setPlayChordSymbols(false);
    model.setIsMetronomeEnabled(false);
    model.load(masterScore);

    const InstrumentTrackIdSet existingTrackIds = model.existingTrackIdSet();
    if (existingTrackIds.empty()) {
        return fail("score has no playable tracks");
    }

    // Repeat-expanded play time plus the upstream 3 s tail
    // (notationplayback.cpp:235-252). The tail contains reverb/note decay; the
    // writer truncates everything after it (soundtrackwriter.cpp:149-204).
    //
    // 4.7.6 derived the duration with secsToMilisecs(), which truncates the
    // double towards zero (4.7.6 audio/common/audiotypes.h:49). 5.0 replaced it
    // with muse::secs_to_msecs(), which rounds (global/types/secs.h:33), so the
    // conversion is spelled out here: the truncation is what fixes the render
    // length in whole milliseconds, and therefore the exact frame count.
    const int lastUtick = masterScore->repeatList(request.use_repeats).ticks();
    const double playTime = masterScore->utick2utime(lastUtick);
    const msecs_t totalDurationMs = msecs_t(static_cast<int64_t>((playTime + kPlaybackTailSecs) * 1000.0));
    // Compare the raw value: msecs_t is a number_t<int64_t> since 5.0 and its
    // operator<=(T) / operator<=(number_t<T>) overloads are equally viable for an
    // int literal (C2666). The meaning is unchanged - a non-positive millisecond
    // duration means the score has nothing to play.
    if (totalDurationMs.raw() <= 0) {
        return fail("score has no audio duration");
    }

    // 4.7.6 wrote (msecs * 1000 * rate) / 1000000 output frames. The writer
    // derives its frame count from a secs_t duration with llround()
    // (soundtrackwriter.cpp:66-72), so hand it the frame count converted back
    // to time: the render then has exactly the same length as before.
    const std::uint64_t totalFrames = static_cast<std::uint64_t>(totalDurationMs) * 1000ULL
                                      * kSampleRate / 1000000ULL;
    const secs_t renderSecs = secs_t(static_cast<double>(totalFrames) / static_cast<double>(kSampleRate));

    // Refuse an oversized render before the encoder creates the file. The
    // parent applies the same projection before spawning a worker; this guard
    // also covers direct callers (engine_audio_probe, tests).
    const double durationSecs = static_cast<double>(totalDurationMs) / 1000.0;
    const std::uint64_t projectedBytes = projected_audio_bytes(audio_output_codec_of(request.format), durationSecs);
    if (projectedBytes > kAudioOutputLimit) {
        return fail(std::string("projected ") + audio_format_name(request.format) + " output of "
                    + std::to_string(static_cast<int>(durationSecs / 60.0)) + " min (~"
                    + std::to_string(projectedBytes / (1024 * 1024))
                    + " MiB) exceeds the 1 GiB audio export limit");
    }

    std::string chainError;
    engine::TrackChainPtr masterChain = make_master_chain(state.context, request, chainError);
    if (!masterChain) {
        return fail(std::move(chainError));
    }

    // The public mix is indexed by the same deterministic track list that
    // score_audio_tracks reports. Keep the list for the selected excerpt: for
    // a part export it contains only the excerpt's parts, while master tracks
    // outside it keep neutral values and are muted below.
    const std::vector<EngineAudioTrackInfo> mixTracks = engine_audio_tracks(masterScore, request.excerpt_id);
    std::map<InstrumentTrackId, std::size_t> mixIndex;
    for (std::size_t index = 0; index < mixTracks.size(); ++index) {
        mixIndex.emplace(mixTracks[index].id, index);
    }
    const auto trackParams = [&request](std::size_t index) {
        return index < request.tracks.size() ? request.tracks[index] : EngineAudioTrackParams{};
    };

    // updateSoloMuteStates() derives forceMute from any non-metronome solo.
    bool hasSolo = false;
    for (std::size_t index = 0; index < mixTracks.size(); ++index) {
        if (mixTracks[index].id == model.metronomeTrackId()) {
            continue;
        }
        if (trackParams(index).solo) {
            hasSolo = true;
            break;
        }
    }

    // Deterministic track order: upstream iterates the unordered track set, but
    // deterministic ids/mix order are needed for byte-identical exports.
    std::vector<InstrumentTrackId> orderedTrackIds(existingTrackIds.cbegin(), existingTrackIds.cend());
    std::sort(orderedTrackIds.begin(), orderedTrackIds.end());

    for (const InstrumentTrackId& trackId : orderedTrackIds) {
        const mpe::PlaybackData& playbackData = model.resolveTrackPlaybackData(trackId);
        if (!playbackData.isValid()) {
            continue;
        }

        const bool isMetronome = trackId == model.metronomeTrackId();
        const bool isChordSymbols = model.isChordSymbolsTrack(trackId);

        const auto index = mixIndex.find(trackId);
        EngineAudioTrackParams mix;
        if (index != mixIndex.end()) {
            mix = trackParams(index->second);
        }

        // Fold forceMute as PlaybackController::updateSoloMuteStates does:
        // params.muted = mute || forceMute (the engine only honours
        // ControlParams::muted, nodes/automationcontrolnode.cpp:49-52).
        // Chord-symbol and metronome tracks are force-muted because chord
        // playback and the metronome are off.
        const bool forceMute = !isMetronome && hasSolo && !mix.solo;

        TrackParams params;
        params.source = state.inputParams;
        set_track_control(params.control, mix);
        params.control.muted = mix.muted || forceMute || isChordSymbols || isMetronome
                               || (excerptSelected && mutedParts.count(trackId.partId) > 0);
        if (!isMetronome) {
            // Desktop adds the two Fluid default sends only for non-metronome
            // tracks (playbackcontroller.cpp:1220-1227; defaultAuxSendValue is
            // 0.30 for both, playbackconfiguration.cpp:272-289). Mixer index 0
            // feeds the first aux track (muse/framework/audio/engine/internal/
            // mixer.cpp:283-309), which is the reverb added below.
            //
            // The second send must stay at zero: 5.0 always gives an aux track
            // an FxChain (AudioContext::onFxChainParamsChanged builds one even
            // for an empty AudioFxChain, audiocontext.cpp:355-369) and the
            // mixer only skips an aux whose chain *pointer* is null
            // (mixer.cpp:275-277, 291-293), so the empty "Aux 2" track is a
            // pass-through that would add 0.30 x the track signal - i.e. 1.3x
            // dry, +2.28 dB - to the master. 4.7.6 skipped an aux whose fx
            // chain *params* were empty (4.7.6 mixer.cpp:507-509, 527-529), so
            // that send was discarded there; keeping it zero reproduces the
            // 4.7.6 mix bit-exactly.
            params.auxSends = { AuxSendParams { kDefaultAuxSend, true }, AuxSendParams { 0.0f, true } };
        }

        const std::string name = track_name(masterScore, model, trackId);
        RetVal2<TrackId, TrackParams> added = context.addTrack(name, playbackData, params);
        if (!added.ret) {
            return fail("addTrack failed: " + added.ret.toString());
        }
    }

    // Aux 0 is the Muse Reverb send target; aux 1 is empty (desktop adds
    // AUX_CHANNEL_NUM aux tracks, playbackcontroller.cpp:1717-1719, in
    // addAuxTrack (:1304-1360)).
    TrackParams reverbParams;
    reverbParams.fxChain = make_reverb_fx_chain();
    RetVal2<TrackId, TrackParams> reverb = context.addAuxTrack(kReverbTrackName, reverbParams);
    if (!reverb.ret) {
        return fail("addAuxTrack(reverb) failed: " + reverb.ret.toString());
    }
    RetVal2<TrackId, TrackParams> aux2 = context.addAuxTrack(kAuxTrackName, TrackParams());
    if (!aux2.ret) {
        return fail("addAuxTrack(aux 2) failed: " + aux2.ret.toString());
    }

    context.setDuration(renderSecs);

    // Mirrors the pre-render reset in AudioContext::saveSoundTrack()
    // (audiocontext.cpp:785-793): stop and seek to zero so every source starts
    // from a known position. A fresh synth is already at zero, so this is a
    // no-op for the one-job-per-process case and only matters for a caller that
    // renders after playback.
    context.stop();
    context.seek(0.0);

    // libopusenc dithers with rand(). 4.7.6 also relied on this for the Ogg
    // stream serial; 5.0 pins the serial itself (oggencoder.cpp:88-91), so the
    // seed only keeps the encoder deterministic. Seed after all engine setup
    // (synth/sequence creation) so a given request renders identically across
    // processes.
    std::srand(0);

    // The writer increments this once per encoded chunk (NullRpcChannel
    // onProcess), giving captured MIDI events their render-frame offset.
    s_rendered_frames = 0;

    StreamingFileDevice device(io::path_t(request.output_path));
    if (!device.open(io::IODevice::WriteOnly)) {
        return fail(std::string("cannot write ") + audio_format_name(request.format) + " output: '"
                    + request.output_path + "'");
    }

    // The writer owns the render loop (soundtrackwriter.cpp:133-207): it sets
    // the source to PlayingOffline, renders in 4096-frame chunks, encodes them
    // and calls rpcChannel()->process() after each one.
    soundtrack::SoundTrackWriter writer(device, make_soundtrack_format(request.format), renderSecs, masterChain);
    const Ret writeRet = writer.write();

    // Flush before the FileSystem size check below.
    device.close();

    // Leave the graph idle, like AudioContext::doSaveSoundTrack()
    // (audiocontext.cpp:934-940).
    masterChain->setMode(ProcessMode::Idle);

    if (!writeRet) {
        // SoundTrackWriter returns Err::ErrorEncode when an encoder could not
        // be created or stopped accepting data, e.g. the destination is not
        // writable, and the bare Ret has no context. Name the format and path
        // instead of reporting the raw code.
        const std::string detail = writeRet.toString();
        return fail(std::string(audio_format_name(request.format)) + " encoder failed (" + detail
                    + "): '" + request.output_path + "'");
    }

    result.ok = true;
    result.frames = totalFrames;
    result.play_time_secs = playTime;
    result.duration_secs = durationSecs;
    result.bytes = file_size(request.output_path);
    if (result.bytes == 0) {
        result.ok = false;
        result.error = "audio export reported success but wrote no data";
    }

    return result;
}

std::vector<EngineAudioTrackInfo> engine_audio_tracks(MasterScore* master, int excerpt_id)
{
    if (!master) {
        throw std::invalid_argument("engine_audio_tracks: master score is null");
    }

    mu::engraving::Score* selected = nullptr;
    try {
        selected = select_score(master, excerpt_id);
    } catch (const std::out_of_range& error) {
        throw std::invalid_argument(error.what());
    }

    std::vector<EngineAudioTrackInfo> result;
    for (const ScoreAudioTrackInfo& description : score_audio_track_info(selected)) {
        EngineAudioTrackInfo info;
        info.id = description.id;
        info.part_id = description.part_id;
        info.instrument_id = description.instrument_id;
        info.part_name = description.part_name;
        info.instrument_name = description.instrument_name;
        info.chord_symbols = description.chord_symbols;
        info.part_index = description.part_index;
        result.push_back(std::move(info));
    }

    return result;
}
} // namespace music
