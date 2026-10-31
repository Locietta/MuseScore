// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "internal/audioengineconfiguration.h"
#include "internal/synthesizers/soundfontrepository.h"
#include "internal/synthesizers/synthresolver.h"
#include "internal/fx/fxresolver.h"
#include "internal/audiofactory.h"
namespace muse::midi { class IMidiOutPort; }
namespace muse::audio::engine {
class AudioServices {
public:
    explicit AudioServices(midi::IMidiOutPort* midiOutPort);
    ~AudioServices();
    AudioServices(const AudioServices&) = delete;
    AudioServices& operator=(const AudioServices&) = delete;
    AudioEngineConfiguration& configuration() { return m_configuration; }
    synth::SoundFontRepository& soundFontRepository() { return m_soundFontRepository; }
    synth::SynthResolver& synthResolver() { return m_synthResolver; }
    AudioFactory& factory() { return m_factory; }
private:
    AudioEngineConfiguration m_configuration;
    synth::SoundFontRepository m_soundFontRepository;
    synth::SynthResolver m_synthResolver;
    fx::FxResolver m_fxResolver;
    AudioFactory m_factory;
};
}
