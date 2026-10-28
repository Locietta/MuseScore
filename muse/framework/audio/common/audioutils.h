/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2023 MuseScore Limited and others
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

#include "audiotypes.h"
#include "soundfonttypes.h"

namespace muse::audio {
inline AudioResourceMeta makeReverbMeta()
{
    AudioResourceMeta meta;
    meta.id = MUSE_REVERB_ID;
    meta.type = NATIVE_EFFECT_TYPE_NAME;
    meta.vendor = "Muse";
    meta.attributes.emplace(HAS_NATIVE_EDITOR_SUPPORT_ATTRIBUTE, u"true");

    return meta;
}

inline String audioResourceTypeToString(const muse::audioplugins::PluginType& metaType)
{
    AudioResourceType type = resourceTypeFromString(metaType);
    auto search = RESOURCE_TYPE_MAP.find(type);

    if (search != RESOURCE_TYPE_MAP.end()) {
        return search->second;
    }

    return RESOURCE_TYPE_MAP.at(AudioResourceType::Undefined);
}

inline String audioSourceName(const AudioInputParams& params)
{
    if (params.type() == AudioSourceType::MuseSampler) {
        return params.resourceMeta.attributeVal(u"museName");
    }

    if (isResourceType(params.resourceMeta, AudioResourceType::FluidSoundfont)) {
        const String& presetName = params.resourceMeta.attributeVal(synth::PRESET_NAME_ATTRIBUTE);
        if (!presetName.empty()) {
            return presetName;
        }

        const String& soundFontName = params.resourceMeta.attributeVal(synth::SOUNDFONT_NAME_ATTRIBUTE);
        if (!soundFontName.empty()) {
            return soundFontName;
        }
    }

    return String::fromStdString(params.resourceMeta.id);
}

inline String audioSourcePackName(const AudioInputParams& params)
{
    if (params.type() == AudioSourceType::MuseSampler) {
        return params.resourceMeta.attributeVal(u"musePack");
    }

    return String();
}

inline String audioSourceCategoryName(const AudioInputParams& params)
{
    if (params.type() == AudioSourceType::MuseSampler) {
        return params.resourceMeta.attributeVal(u"museCategory");
    }

    if (isResourceType(params.resourceMeta, AudioResourceType::FluidSoundfont)) {
        return params.resourceMeta.attributeVal(synth::SOUNDFONT_NAME_ATTRIBUTE);
    }

    return String::fromStdString(params.resourceMeta.id);
}

inline AudioFxCategories audioFxCategoriesFromString(const String& str)
{
    if (str.empty()) {
        return {};
    }

    StringList list = str.split('|');

    AudioFxCategories result;
    for (const String& name : list) {
        result.insert(muse::key(AUDIO_FX_CATEGORY_TO_STRING_MAP, name, AudioFxCategory::FxOther));
    }

    return result;
}

inline bool isOnlineAudioResource(const AudioResourceMeta& meta)
{
    return boolAttribute(meta, u"isOnline");
}
}
