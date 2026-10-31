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

#include "audio/common/rpc/irpcchannel.h"
#include "audio/engine/enginesetup.h"
#include "audio/engine/internal/enginecontroller.h"
#include "audio/engine/internal/iaudiocontext.h"
#include "audio/engine/internal/iaudioengine.h"

int main()
{
    const auto rpc = std::make_shared<music::NullRpcChannel>();

    // 5.0 registers the engine services in EngineGlobalSetup (the replacement
    // for 4.7.6 EngineController::registerExports); EngineController keeps the
    // engine-side RPC handlers and the init sequence
    // (muse/framework/audio/engine/enginesetup.cpp:46-74,
    //  muse/framework/audio/engine/internal/enginecontroller.cpp:46-98).
    muse::audio::engine::EngineGlobalSetup setup;
    setup.registerExports();
    setup.resolveImports();

    muse::audio::engine::EngineController controller(rpc);

    muse::audio::OutputSpec spec;
    spec.sampleRate = 44100;
    spec.samplesPerChannel = 4096;
    spec.audioChannelCount = 2;

    muse::audio::AudioEngineConfig config;
    config.useSoundFontLowPassFilter = true;
    controller.init(spec, config);

    auto engine = muse::modularity::globalIoc()->resolve<muse::audio::engine::IAudioEngine>("audio_engine_link_check");
    if (!engine) {
        std::cout << "audio engine is not registered\n";
        return 1;
    }

    muse::RetVal<std::shared_ptr<muse::audio::engine::IAudioContext> > context = engine->addAudioContext(1);
    if (!context.ret || !context.val) {
        std::cout << "audio context could not be created\n";
        return 1;
    }

    std::cout << "audio engine linked\n";
    return 0;
}
