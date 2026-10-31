// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "iaudiofactory.h"
#include "mixer.h"
#include "nodes/trackchain.h"

namespace muse::audio::engine {
// Owns one render graph. Synth and effect resolver caches are released with it.
// The former offline ContextPlayer stayed stopped: control/FX time is zero.
class OfflineRenderSession {
public:
    OfflineRenderSession(IAudioFactory& factory, const OutputSpec& spec);
    ~OfflineRenderSession();
    OfflineRenderSession(const OfflineRenderSession&) = delete;
    OfflineRenderSession& operator=(const OfflineRenderSession&) = delete;
    RetVal2<TrackId, TrackParams> addTrack(const std::string& name,
        const mpe::PlaybackData& data, const TrackParams& params);
    RetVal2<TrackId, TrackParams> addAuxTrack(const std::string& name, const TrackParams& params);
    IAudioNodePtr output() const { return m_master; }
    void resetSources();
private:
    RetVal2<TrackId, TrackParams> add(const std::string& name,
        const mpe::PlaybackData* data, const TrackParams& params);
    IAudioFactory& m_factory;
    OutputSpec m_spec;
    std::shared_ptr<Mixer> m_mixer;
    TrackChainPtr m_master;
    std::vector<TrackChainPtr> m_tracks;
    TrackId m_lastId = MASTER_TRACK_ID;
};
}
