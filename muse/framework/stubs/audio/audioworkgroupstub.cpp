/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
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

#include "audio/common/audioworkgroup.h"

#include <memory>
#include <thread>
#include <utility>

namespace muse::audio {
class AudioWorkgroupProvider
{
};

AudioWorkGroup::AudioWorkGroup() = default;

AudioWorkGroup::AudioWorkGroup(std::unique_ptr<AudioWorkgroupProvider> provider)
    : m_provider(std::move(provider)) {}

AudioWorkGroup::~AudioWorkGroup() = default;

AudioWorkGroup::AudioWorkGroup(const AudioWorkGroup& other)
    : m_provider(other.m_provider ? std::make_unique<AudioWorkgroupProvider>(*other.m_provider) : nullptr) {}

AudioWorkGroup::AudioWorkGroup(AudioWorkGroup&& other) noexcept = default;

AudioWorkGroup& AudioWorkGroup::operator=(const AudioWorkGroup& other)
{
    if (this != &other) {
        m_provider = other.m_provider ? std::make_unique<AudioWorkgroupProvider>(*other.m_provider) : nullptr;
    }
    return *this;
}

AudioWorkGroup& AudioWorkGroup::operator=(AudioWorkGroup&& other) noexcept = default;

bool AudioWorkGroup::join(AudioWorkgroupToken&)
{
    return false;
}

size_t AudioWorkGroup::getMaxParallelThreadCount() const
{
    return std::thread::hardware_concurrency();
}

AudioWorkGroup makeAudioWorkgroup(void*)
{
    return {};
}
} // namespace muse::audio
