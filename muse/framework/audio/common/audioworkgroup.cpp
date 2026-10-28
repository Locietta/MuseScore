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

#include "audioworkgroup.h"

#include <memory>
#include <utility>

#ifdef __APPLE__
#include <os/object.h>
#include <os/workgroup.h>
#else
#include <thread>
#endif

namespace muse::audio {
#ifdef __APPLE__
class AudioWorkgroupTokenProvider
{
public:
    AudioWorkgroupTokenProvider(os_workgroup_t workgroup)
        : m_workgroup(workgroup)
    {
        if (!m_workgroup) {
            return;
        }

        if (__builtin_available(macOS 11.0, *)) {
            os_retain(m_workgroup);
            auto status = os_workgroup_join(m_workgroup, &m_joinToken);
            if (status == 0) {
                return;
            }

            os_release(m_workgroup);
            m_workgroup = nullptr;
            m_joinToken = {};
            return;
        }

        m_workgroup = nullptr;
    }

    bool isAttachedTo(os_workgroup_t wg) const { return m_workgroup == wg; }
    bool isValid() const { return m_workgroup != nullptr; }

    AudioWorkgroupTokenProvider(const AudioWorkgroupTokenProvider& other) = delete;

    AudioWorkgroupTokenProvider& operator=(const AudioWorkgroupTokenProvider&) = delete;

    AudioWorkgroupTokenProvider(AudioWorkgroupTokenProvider&& other) noexcept
        : m_workgroup(other.m_workgroup), m_joinToken(other.m_joinToken)
    {
        other.m_workgroup = nullptr;
        other.m_joinToken = {};
    }

    ~AudioWorkgroupTokenProvider()
    {
        leave();
    }

private:
    void leave() noexcept
    {
        if (!m_workgroup) {
            return;
        }

        if (__builtin_available(macOS 11.0, *)) {
            os_workgroup_leave(m_workgroup, &m_joinToken);
        }
        os_release(m_workgroup);
        m_workgroup = nullptr;
        m_joinToken = {};
    }

    os_workgroup_t m_workgroup;
    os_workgroup_join_token_s m_joinToken;
};

class AudioWorkgroupProvider
{
public:
    explicit AudioWorkgroupProvider(os_workgroup_t wg)
        : m_workgroup(wg)
    {
        os_retain(m_workgroup);
    }

    ~AudioWorkgroupProvider()
    {
        if (m_workgroup) {
            os_release(m_workgroup);
        }
    }

    AudioWorkgroupProvider(const AudioWorkgroupProvider& other)
        : m_workgroup(other.m_workgroup)
    {
        os_retain(m_workgroup);
    }

    AudioWorkgroupProvider& operator=(const AudioWorkgroupProvider& other)
    {
        if (this != &other) {
            os_retain(other.m_workgroup);
            if (m_workgroup) {
                os_release(m_workgroup);
            }
            m_workgroup = other.m_workgroup;
        }
        return *this;
    }

    AudioWorkgroupProvider(AudioWorkgroupProvider&& other) noexcept
        : m_workgroup(other.m_workgroup)
    {
        other.m_workgroup = nullptr;
    }

    bool join(AudioWorkgroupToken& tokenProvider) const
    {
        if (auto existingProvider = AudioWorkGroup::providerFor(tokenProvider);
            existingProvider != nullptr && existingProvider->isAttachedTo(m_workgroup)) {
            return true;
        }
        AudioWorkGroup::resetProviderFor(tokenProvider);
        AudioWorkgroupTokenProvider provider(m_workgroup);
        if (!provider.isValid()) {
            return false;
        }

        AudioWorkGroup::setProviderFor(tokenProvider, [provider = std::move(provider)]() {
            return &provider;
        });
        return true;
    }

    size_t getMaxParallelThreadCount() const
    {
        if (__builtin_available(macOS 11.0, *)) {
            return (size_t)os_workgroup_max_parallel_threads(m_workgroup, nullptr);
        }
        return 0;
    }

private:

    os_workgroup_t m_workgroup;
};

bool AudioWorkGroup::join(AudioWorkgroupToken& token)
{
    auto provider = getProvider();
    if (!provider) {
        return false;
    }
    return provider->join(token);
}

AudioWorkGroup makeAudioWorkgroup(void* opaqueHandle)
{
    if (opaqueHandle == nullptr) {
        return {};
    }

    os_workgroup_t handle = reinterpret_cast<os_workgroup_t>(opaqueHandle);

    return AudioWorkGroup { std::make_unique<AudioWorkgroupProvider>(handle) };
}

size_t AudioWorkGroup::getMaxParallelThreadCount() const
{
    if (auto provider = getProvider(); provider) {
        return provider->getMaxParallelThreadCount();
    }
    return 0;
}

#else

class AudioWorkgroupProvider
{
};

size_t AudioWorkGroup::getMaxParallelThreadCount() const
{
    return std::thread::hardware_concurrency();
}

bool AudioWorkGroup::join(AudioWorkgroupToken&)
{
    return false;
}

AudioWorkGroup makeAudioWorkgroup(void*)
{
    return {};
}

#endif

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
} // namespace muse::audio
