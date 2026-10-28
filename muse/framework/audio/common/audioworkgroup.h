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
#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

#include "global/functional/inplace_function_mv.h"

namespace muse::audio {
class AudioWorkgroupTokenProvider;

using ErasedAudioWorkgroupTokenProvider = muse::functional::MoveOnlyInplaceFunction<const AudioWorkgroupTokenProvider* (), 64>;

class AudioWorkgroupToken
{
public:
    AudioWorkgroupToken() = default;
    AudioWorkgroupToken(const AudioWorkgroupToken&) = delete;
    AudioWorkgroupToken& operator=(const AudioWorkgroupToken&) = delete;
    AudioWorkgroupToken(AudioWorkgroupToken&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    AudioWorkgroupToken& operator=(AudioWorkgroupToken&& other) noexcept
    {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }

        return *this;
    }

    ~AudioWorkgroupToken()
    {
        reset();
    }

private:
    friend class AudioWorkGroup;

    explicit AudioWorkgroupToken(ErasedAudioWorkgroupTokenProvider provider)
    {
        setProvider(std::move(provider));
    }

    const AudioWorkgroupTokenProvider* provider() const
    {
        assertCurrentThread();
        return m_provider ? m_provider() : nullptr;
    }

    void setProvider(ErasedAudioWorkgroupTokenProvider workgroup)
    {
        reset();
        m_provider = std::move(workgroup);
        if (m_provider) {
            m_threadId = std::this_thread::get_id();
        }
    }

    void reset()
    {
        assertCurrentThread();
        m_provider = nullptr;
        m_threadId = {};
    }

    void moveFrom(AudioWorkgroupToken&& other) noexcept
    {
        other.assertCurrentThread();
        m_provider = std::move(other.m_provider);
        m_threadId = other.m_threadId;
        other.m_threadId = {};
    }

    void assertCurrentThread() const
    {
        assert((!m_provider || m_threadId == std::this_thread::get_id())
               && "AudioWorkgroupToken must be used on the thread that joined the audio workgroup");
    }

    ErasedAudioWorkgroupTokenProvider m_provider;
    std::thread::id m_threadId;
};

class AudioWorkgroupProvider;

class AudioWorkGroup
{
public:
    AudioWorkGroup();
    AudioWorkGroup(const AudioWorkGroup&);
    AudioWorkGroup(AudioWorkGroup&&) noexcept;
    AudioWorkGroup& operator=(const AudioWorkGroup&);
    AudioWorkGroup& operator=(AudioWorkGroup&&) noexcept;
    ~AudioWorkGroup();

    bool join(AudioWorkgroupToken& token);

    const AudioWorkgroupProvider* getProvider() const { return m_provider.get(); }

    size_t getMaxParallelThreadCount() const;

private:
    friend class AudioWorkgroupProvider;
    friend AudioWorkGroup makeAudioWorkgroup(void* opaqueHandle);

    explicit AudioWorkGroup(std::unique_ptr<AudioWorkgroupProvider> provider);

    static const AudioWorkgroupTokenProvider* providerFor(const AudioWorkgroupToken& token) { return token.provider(); }
    static void setProviderFor(AudioWorkgroupToken& token, ErasedAudioWorkgroupTokenProvider provider)
    {
        token.setProvider(std::move(provider));
    }

    static void resetProviderFor(AudioWorkgroupToken& token) { token.reset(); }

    std::unique_ptr<AudioWorkgroupProvider> m_provider;
};

AudioWorkGroup makeAudioWorkgroup(void* opaqueHandle);
} // namespace muse::audio
