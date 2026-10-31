// SPDX-License-Identifier: GPL-3.0-only
//
// Link and lifecycle probe for the offline audio services and render session.
// Musical output, encoder finalization and parity are checked by the public
// output tests and engine_audio_probe.

#include <iostream>
#include <memory>



#include "engrave/src/audio/engine_audio.h"


#include "audio/engine/audioservices.h"
#include "audio/engine/iaudioengineconfiguration.h"
#include "audio/engine/isynthresolver.h"
#include "audio/engine/internal/offlinerendersession.h"
#include "audio/engine/internal/audiofactory.h"

int main()
{
    muse::audio::engine::AudioServices setup(nullptr);


    muse::audio::OutputSpec spec;
    spec.sampleRate = 44100;
    spec.samplesPerChannel = 4096;
    spec.audioChannelCount = 2;

    muse::audio::AudioEngineConfig config;
    config.useSoundFontLowPassFilter = true;
    auto configuration = &setup.configuration();
    configuration->setConfig(config);
    setup.synthResolver().init(configuration->defaultAudioInputParams(), spec);


    auto factory = &setup.factory();
    factory->setOutputSpec(spec);
    muse::audio::engine::OfflineRenderSession session(*factory, spec);

    std::cout << "audio engine linked\n";
    return 0;
}
