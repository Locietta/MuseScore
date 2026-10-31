// SPDX-License-Identifier: GPL-3.0-only
#include "offlinerendersession.h"
#include "nodes/automationcontrolnode.h"
#include "nodes/signalnode.h"
using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;

OfflineRenderSession::OfflineRenderSession(IAudioFactory& factory, const OutputSpec& spec)
    : m_factory(factory), m_spec(spec), m_mixer(std::make_shared<Mixer>()),
      m_master(std::make_shared<TrackChain>(MASTER_TRACK_ID, "Master"))
{
    m_mixer->init();
    auto control = std::make_shared<AutomationControlNode>();
    control->setPlayheadPosition(nullptr);
    m_master->setSource(m_mixer);
    m_master->setFxChain(nullptr);
    m_master->setControl(control);
    m_master->setSignal(std::make_shared<SignalNode>());
    m_master->rebuild();
    m_master->setOutputSpec(spec);
    m_master->setMode(ProcessMode::Idle);
}

OfflineRenderSession::~OfflineRenderSession()
{
    m_master->setMode(ProcessMode::Idle);
    m_master->clear();
    m_master->disconnectAll();
    m_master.reset();
    for (const auto& track : m_tracks) {
        if (auto fx = track->fxChain()) fx->clear();
        track->clear();
        track->disconnectAll();
    }
    m_tracks.clear();
    m_mixer.reset();
    m_factory.clearSynthSources();
    m_factory.clearAllFx();
}

RetVal2<TrackId, TrackParams> OfflineRenderSession::addTrack(const std::string& name,
    const mpe::PlaybackData& data, const TrackParams& params)
{
    return add(name, &data, params);
}
RetVal2<TrackId, TrackParams> OfflineRenderSession::addAuxTrack(const std::string& name, const TrackParams& params)
{
    return add(name, nullptr, params);
}
RetVal2<TrackId, TrackParams> OfflineRenderSession::add(const std::string& name,
    const mpe::PlaybackData* data, const TrackParams& params)
{
    using Result = RetVal2<TrackId, TrackParams>;
    const TrackId id = ++m_lastId;
    AudioSourceNodePtr source;
    if (data) {
        auto made = m_factory.makeEventSource(id, *data, params.source);
        if (!made.ret) return Result::make_ret(made.ret);
        source = made.val;
    }
    auto control = std::make_shared<AutomationControlNode>();
    control->setPlayheadPosition(nullptr);
    auto chain = std::make_shared<TrackChain>(id, name);
    chain->setOutputSpec(m_spec);
    chain->setMode(ProcessMode::Idle);
    if (source) chain->setSource(source);
    chain->setFxChain(nullptr);
    chain->setControl(control);
    chain->setSignal(std::make_shared<SignalNode>());
    chain->rebuild();
    const Ret added = data ? m_mixer->addTrack(chain, params.auxSends) : m_mixer->addAuxTrack(chain);
    if (!added) return Result::make_ret(added);
    auto fx = m_factory.makeTrackFxChain(id, params.fxChain);
    fx->setPlayheadPosition(nullptr);
    chain->setFxChain(fx);
    chain->rebuild();
    control->setVolume(params.control.volume);
    control->setPan(params.control.balance);
    control->setMuted(params.control.muted);
    if (source) source->seek(TimePosition::zero(m_spec.sampleRate));
    m_tracks.push_back(chain);
    size_t audible = 0;
    for (const auto& track : m_tracks) if (!track->control()->muted()) ++audible;
    m_mixer->setNonMutedTrackCount(audible);
    return Result::make_ok(id, params);
}
void OfflineRenderSession::resetSources()
{
    for (const auto& track : m_tracks) {
        if (auto source = std::dynamic_pointer_cast<AudioSourceNode>(track->source()))
            source->seek(TimePosition::zero(m_spec.sampleRate));
    }
}
