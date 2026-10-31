// SPDX-License-Identifier: GPL-3.0-only
//
// Build-time link probe for the Qt-free MuseScore 5.0 audio engine and its
// encoders. It is not the export driver: it only proves that the static
// libraries resolve, that the engine services register, and that an
// AudioContext can be created against the shared no-op RPC channel (the real
// driver is engrave/src/audio/engine_audio.cpp).

#include <iostream>
#include <memory>

#include "global/modularity/ioc.h"

#include "engrave/src/audio/engine_audio.h"


#include "audio/engine/enginesetup.h"
#include "audio/engine/iaudioengineconfiguration.h"
#include "audio/engine/isynthresolver.h"
#include "audio/engine/internal/offlinerendersession.h"
#include "audio/engine/internal/audiofactory.h"

int main()
{
    muse::audio::engine::EngineGlobalSetup setup;
    setup.registerExports();
    setup.resolveImports();


    muse::audio::OutputSpec spec;
    spec.sampleRate = 44100;
    spec.samplesPerChannel = 4096;
    spec.audioChannelCount = 2;

    muse::audio::AudioEngineConfig config;
    config.useSoundFontLowPassFilter = true;
    auto configuration = muse::modularity::globalIoc()->resolve<muse::audio::engine::IAudioEngineConfiguration>("audio_engine_link_check");
    configuration->setConfig(config);
    muse::modularity::globalIoc()->resolve<muse::audio::synth::ISynthResolver>("audio_engine_link_check")->init(configuration->defaultAudioInputParams(), spec);


    auto factory = std::dynamic_pointer_cast<muse::audio::engine::AudioFactory>(
        muse::modularity::globalIoc()->resolve<muse::audio::engine::IAudioFactory>("audio_engine_link_check"));
    factory->setOutputSpec(spec);
    muse::audio::engine::OfflineRenderSession session(*factory, spec);

    std::cout << "audio engine linked\n";
    return 0;
}
