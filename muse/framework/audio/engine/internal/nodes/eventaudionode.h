/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "audiosourcenode.h"

#include "mpe/events.h"

#include "audio/common/audiotypes.h"
#include "../iaudiofactory.h"

namespace muse::audio::engine {
class EventAudioNode : public AudioSourceNode
{
public:
    // audioFactory makes the synthesizer for the input params; it must outlive the node.
    EventAudioNode(const IAudioFactory& audioFactory, TrackId trackId, const mpe::PlaybackData& playbackData);

    void seek(const TimePosition& position, const bool flushSound = true) override;
    void flush() override;

    const AudioInputParams& inputParams() const override;
    void applyInputParams(const AudioInputParams& requiredParams) override;

    void prepareToPlay() override;
    bool readyToPlay() const override;

    bool hasPendingChunks() const override;
    void processInput() override;
    InputProcessingProgress inputProcessingProgress() const override;

    void clearCache() override;

private:
    struct SynthCtx
    {
        ProcessMode mode = ProcessMode::Undefined;
        TimePosition playbackPosition;
        bool isValid() const { return mode != ProcessMode::Undefined; }
    };

    void onModeChanged(const ProcessMode mode) override;
    void onOutputSpecChanged(const OutputSpec& spec) override;

    void doSelfProcess(float* buffer, samples_t samplesPerChannel) override;

    void setupSource();
    SynthCtx currentSynthCtx() const;
    void restoreSynthCtx(const SynthCtx& ctx);

    const IAudioFactory* audioFactory() const { return &m_audioFactory; }

    const IAudioFactory& m_audioFactory;
    TrackId m_trackId = -1;
    mpe::PlaybackData m_playbackData;
    synth::ISynthesizerPtr m_synth = nullptr;
    AudioInputParams m_params;
};

using EventAudioNodePtr = std::shared_ptr<EventAudioNode>;
}
