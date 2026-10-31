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

#include "mpe/events.h"

#include "../../iaudioengineconfiguration.h"

#include "audio/common/audiotypes.h"
#include "../../isynthesizer.h"

namespace muse::audio::synth {
class AbstractSynthesizer : public ISynthesizer
{
public:
    // config must outlive the synthesizer.
    AbstractSynthesizer(const audio::AudioInputParams& params, const engine::IAudioEngineConfiguration& config);
    virtual ~AbstractSynthesizer() = default;

    virtual void setMode(const ProcessMode mode) override;
    ProcessMode mode() const override;

    const audio::AudioInputParams& params() const override;

    void setup(const mpe::PlaybackData& playbackData) override;

    void prepareToPlay() override;
    bool readyToPlay() const override;

    bool hasPendingChunks() const override;
    void processInput() override;
    InputProcessingProgress inputProcessingProgress() const override;

    void clearCache() override;

protected:
    const engine::IAudioEngineConfiguration* config() const { return &m_config; }

    virtual void setupSound(const mpe::PlaybackSetupData& setupData) = 0;
    virtual void setupEvents(const mpe::PlaybackData& playbackData) = 0;

    msecs_t samplesToMsecs(const samples_t samplesPerChannel, const samples_t sampleRate) const;
    samples_t microSecsToSamples(const msecs_t msec, const samples_t sampleRate) const;

    ProcessMode m_mode = ProcessMode::Undefined;

    mpe::PlaybackSetupData m_setupData;

    audio::AudioInputParams m_params;
    const engine::IAudioEngineConfiguration& m_config;

    InputProcessingProgress m_inputProcessingProgress;
    bool m_hasPendingChunks = false;
};
}
