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

#include "eventaudionode.h"
#include "../offlinetiming.h"


#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;
using namespace muse::audio::synth;
using namespace muse::mpe;

EventAudioNode::EventAudioNode(const IAudioFactory& audioFactory, TrackId trackId, const mpe::PlaybackData& playbackData)
    : m_audioFactory(audioFactory), m_trackId(trackId), m_playbackData(playbackData)
{
}

void EventAudioNode::onModeChanged(const ProcessMode mode)
{

    if (!m_synth) {
        return;
    }

    m_synth->setMode(mode);
    m_synth->flushSound();
}

void EventAudioNode::onOutputSpecChanged(const OutputSpec& spec)
{
    if (!m_synth) {
        return;
    }

    m_synth->setOutputSpec(spec);
}

void EventAudioNode::doSelfProcess(float* buffer, samples_t samplesPerChannel)
{
    if (!m_synth) {
        return;
    }

    OfflineTimer timer(OfflineTiming::current().synth);
    m_synth->process(buffer, samplesPerChannel);
}

void EventAudioNode::seek(const TimePosition& position, const bool flushSound)
{

    IF_ASSERT_FAILED(m_synth) {
        return;
    }

    if (m_synth->playbackPosition() == position) {
        return;
    }

    m_synth->setPlaybackPosition(position);

    if (flushSound) {
        m_synth->flushSound();
    }
}

void EventAudioNode::flush()
{

    IF_ASSERT_FAILED(m_synth) {
        return;
    }

    m_synth->flushSound();
}

const AudioInputParams& EventAudioNode::inputParams() const
{
    return m_params;
}

void EventAudioNode::applyInputParams(const AudioInputParams& requiredParams)
{
    IF_ASSERT_FAILED(m_outputSpec.isValid()) {
        return;
    }

    if (m_params.isValid() && m_params == requiredParams) {
        return;
    }

    SynthCtx ctx = currentSynthCtx();

    if (m_synth) {
        m_playbackData = m_synth->playbackData();
    }

    RetVal<synth::ISynthesizerPtr> synth = audioFactory()->makeSynth(m_trackId, requiredParams, m_playbackData.setupData);

    if (!synth.ret) {
        synth = audioFactory()->makeDefaultSynth(m_trackId);
        IF_ASSERT_FAILED(synth.val) {
            LOGE() << "Default synth not found!";
            return;
        }
    }

    m_synth = synth.val;

    setupSource();

    if (ctx.isValid()) {
        restoreSynthCtx(ctx);
    } else {
        m_synth->setMode(ProcessMode::Idle);
    }

    setName(std::string("Source[") + m_synth->name() + "]");

    m_params = m_synth->params();
}

void EventAudioNode::prepareToPlay()
{

    IF_ASSERT_FAILED(m_synth) {
        return;
    }

    m_synth->prepareToPlay();
}

bool EventAudioNode::readyToPlay() const
{

    IF_ASSERT_FAILED(m_synth) {
        return false;
    }

    return m_synth->readyToPlay();
}

bool EventAudioNode::hasPendingChunks() const
{

    IF_ASSERT_FAILED(m_synth) {
        return false;
    }

    return m_synth->hasPendingChunks();
}

void EventAudioNode::processInput()
{

    IF_ASSERT_FAILED(m_synth) {
        return;
    }

    m_synth->processInput();
}

InputProcessingProgress EventAudioNode::inputProcessingProgress() const
{

    IF_ASSERT_FAILED(m_synth) {
        return {};
    }

    return m_synth->inputProcessingProgress();
}

void EventAudioNode::clearCache()
{

    IF_ASSERT_FAILED(m_synth) {
        return;
    }

    m_synth->clearCache();
}

EventAudioNode::SynthCtx EventAudioNode::currentSynthCtx() const
{
    if (!m_synth) {
        return SynthCtx();
    }

    return { m_synth->mode(), m_synth->playbackPosition() };
}

void EventAudioNode::restoreSynthCtx(const SynthCtx& ctx)
{
    m_synth->setPlaybackPosition(ctx.playbackPosition);
    m_synth->setMode(ctx.mode);
}

void EventAudioNode::setupSource()
{

    IF_ASSERT_FAILED(m_synth) {
        return;
    }

    m_synth->setOutputSpec(m_outputSpec);
    m_synth->setup(m_playbackData);
}
