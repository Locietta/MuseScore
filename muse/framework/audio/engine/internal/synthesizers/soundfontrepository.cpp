/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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
#include "soundfontrepository.h"


#include "fluidsynth/fluidsoundfontparser.h"


#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::synth;

bool SoundFontRepository::isSoundFontLoaded(const std::string& name) const
{
    for (const auto& p : m_soundFonts) {
        if (p.second.name == name) {
            return true;
        }
    }
    return false;
}

bool SoundFontRepository::isLoadingSoundFonts() const
{
    return m_loadingSoundFontCount > 0;
}

const SoundFontsMap& SoundFontRepository::soundFonts() const
{
    return m_soundFonts;
}

void SoundFontRepository::setOnSoundFontsChanged(std::function<void()> onSoundFontsChanged)
{
    m_onSoundFontsChanged = std::move(onSoundFontsChanged);
}

void SoundFontRepository::notifySoundFontsChanged()
{
    if (m_onSoundFontsChanged) {
        m_onSoundFontsChanged();
    }
}

void SoundFontRepository::addSoundFont(const SoundFontUri& uri)
{

    m_loadingSoundFontCount++;
    doAddSoundFont(uri, nullptr, [this]() {
        --m_loadingSoundFontCount;
        notifySoundFontsChanged();
    });
}

static io::path_t fileNameFromUri(const SoundFontUri& uri)
{
    io::path_t path = uri.toLocalFile();
    return io::filename(path);
}

void SoundFontRepository::addSoundFontData(const SoundFontUri& uri, const ByteArray& data)
{

    LOGI() << "try add sound font uri: " << uri << ", size: " << data.size();

    const io::path_t fileName = fileNameFromUri(uri);

    {
        FILE* file = fopen(fileName.c_str(), "wb");
        IF_ASSERT_FAILED(file) {
            return;
        }
        size_t wsize = fwrite(data.constChar(), sizeof(char), data.size(), file);
        IF_ASSERT_FAILED(wsize == data.size()) {
            return;
        }
        fclose(file);
    }

    RetVal<SoundFontMeta> meta = FluidSoundFontParser::parseSoundFont(fileName);

    if (meta.ret) {
        m_soundFonts.insert_or_assign(uri, std::move(meta.val));
        LOGI() << "added sound font, uri: " << uri;
    } else {
        LOGE() << "Failed parse SoundFont presets for " << fileName << ": " << meta.ret.toString();
    }

    notifySoundFontsChanged();
}

void SoundFontRepository::doAddSoundFont(const SoundFontUri& uri, const SoundFontsMap* cache, std::function<void()> onFinished)
{
    if (cache) {
        auto it = cache->find(uri);
        if (it != cache->cend()) {
            m_soundFonts.insert(*it);
            if (onFinished) {
                onFinished();
            }
            return;
        }
    }

    auto parseAndAdd = [this, onFinished](const SoundFontUri& uri, const SoundFontPath& path) {
        RetVal<SoundFontMeta> meta = FluidSoundFontParser::parseSoundFont(path);

        if (meta.ret) {
            m_soundFonts.insert_or_assign(uri, std::move(meta.val));
            LOGI() << "added sound font, uri: " << uri;
        } else {
            LOGE() << "Failed parse SoundFont presets for " << path << ": " << meta.ret.toString();
        }

        if (onFinished) {
            onFinished();
        }
    };

    if (uri.scheme() == "file") {
        SoundFontPath path = uri.toLocalFile();
        parseAndAdd(uri, path);
    } else {
        NOT_SUPPORTED;
        if (onFinished) {
            onFinished();
        }
    }
}

void SoundFontRepository::loadSoundFonts(const std::vector<SoundFontUri>& uris)
{

    SoundFontsMap* cache = new SoundFontsMap();
    m_soundFonts.swap(*cache);

    m_loadingSoundFontCount = m_loadingSoundFontCount + uris.size();
    const size_t total = m_loadingSoundFontCount;

    for (const SoundFontUri& uri : uris) {
        LOGI() << "try add sound font: " << uri.toString();
        doAddSoundFont(uri, cache, [this, total, cache]() {
            --m_loadingSoundFontCount;
            LOGI() << "remaining: " << m_loadingSoundFontCount << ", total: " << total;
            if (m_loadingSoundFontCount == 0) {
                delete cache;
                notifySoundFontsChanged();
                LOGI() << "all added notify about sound fonts changed";
            }
        });
    }
}
