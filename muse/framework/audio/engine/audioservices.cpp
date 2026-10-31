// SPDX-License-Identifier: GPL-3.0-only
#include "audioservices.h"
#include "internal/synthesizers/fluidsynth/fluidresolver.h"
#include "internal/fx/musefxresolver.h"
using namespace muse::audio;
using namespace muse::audio::engine;
AudioServices::AudioServices(midi::IMidiOutPort* midiOutPort)
    : m_factory(m_synthResolver, m_fxResolver)
{
    m_fxResolver.registerResolver(AudioFxType::MuseFx, std::make_shared<fx::MuseFxResolver>());
    auto fluid = std::make_shared<synth::FluidResolver>(m_soundFontRepository, m_configuration, midiOutPort);
    m_synthResolver.registerResolver(AudioSourceType::Fluid, fluid);
    m_soundFontRepository.setOnSoundFontsChanged([fluid]() { fluid->refresh(); });
}
AudioServices::~AudioServices()
{
    m_soundFontRepository.setOnSoundFontsChanged({});
}
