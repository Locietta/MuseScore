/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
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

#include "articulationprofilesrepository.h"

#include <string>
#include <utility>

#include "serialization/json.h"

#include "log.h"

#include "internal/articulationstringutils.h"

using namespace muse;
using namespace muse::mpe;
using namespace muse::async;

static const std::map<ArticulationFamily, io::path_t> DEFAULT_ARTICULATION_PROFILES =
{
    { ArticulationFamily::Keyboards, io::path_t(":/mpe/resources/general_keyboard_articulations_profile.json") },
    { ArticulationFamily::Strings, io::path_t(":/mpe/resources/general_strings_articulations_profile.json") },
    { ArticulationFamily::Winds, io::path_t(":/mpe/resources/general_winds_articulations_profile.json") },
    { ArticulationFamily::Percussions, io::path_t(":/mpe/resources/general_percussion_articulations_profile.json") },
    { ArticulationFamily::Voices, io::path_t(":/mpe/resources/general_voice_articulations_profile.json") }
};

static const std::string SUPPORTED_FAMILIES = "supportedFamilies";
static const std::string PATTERNS_KEY = "patterns";
static const std::string PATTERN_POS_KEY = "patternPosition";
static const std::string OFFSET_POS_KEY = "offsetPosition";
static const std::string OFFSET_VAL_KEY = "offsetValue";

// Arrangement
static const std::string ARRANGEMENT_PATTERN_KEY = "arrangementPattern";
static const std::string DURATION_FACTOR_KEY = "durationFactor";
static const std::string TIMESTAMP_OFFSET_KEY = "timestampOffset";

// Pitch
static const std::string PITCH_PATTERN_KEY = "pitchPattern";
static const std::string PITCH_OFFSETS_KEY = "pitchOffsets";

// Expression
static const std::string EXPRESSION_PATTERN = "expressionPattern";
static const std::string MAX_AMPLITUDE_LEVEL_KEY = "maxAmplitudeLevel";
static const std::string AMPLITUDE_TIME_SHIFT = "amplitudeTimeShift";
static const std::string DYNAMIC_OFFSETS_KEY = "dynamicOffsets";

static JsonArray jsonArray(const JsonValue& value)
{
    return value.isArray() ? value.toArray() : JsonArray();
}

static JsonObject jsonObject(const JsonValue& value)
{
    return value.isObject() ? value.toObject() : JsonObject();
}

ArticulationsProfilePtr ArticulationProfilesRepository::createNew() const
{
    return std::make_shared<ArticulationsProfile>();
}

ArticulationsProfilePtr ArticulationProfilesRepository::defaultProfile(const ArticulationFamily family) const
{
    auto search = m_defaultProfiles.find(family);

    if (search != m_defaultProfiles.cend()) {
        return search->second;
    }

    auto pathSearch = DEFAULT_ARTICULATION_PROFILES.find(family);

    if (pathSearch == DEFAULT_ARTICULATION_PROFILES.cend()) {
        LOGE() << "Unable to find path for undefined articulations family";
        return nullptr;
    }

    ArticulationsProfilePtr result = loadProfile(pathSearch->second);
    m_defaultProfiles.emplace(family, result);

    return result;
}

ArticulationsProfilePtr ArticulationProfilesRepository::loadProfile(const io::path_t& path) const
{
    RetVal<ByteArray> fileReading = fileSystem()->readFile(path);

    if (!fileReading.ret) {
        LOGE() << "Unable to read profile, path: " << path;
        return nullptr;
    }

    std::string err;

    JsonDocument file = JsonDocument::fromJson(fileReading.val, &err);
    if (!err.empty()) {
        LOGE() << err;
        return nullptr;
    }

    ArticulationsProfilePtr result = std::make_shared<ArticulationsProfile>();

    JsonObject rootObj = file.rootObject();

    result->supportedFamilies = supportedFamiliesFromJson(jsonArray(rootObj.value(SUPPORTED_FAMILIES)));

    JsonObject articulationPatterns = jsonObject(rootObj.value(PATTERNS_KEY));

    for (const std::string& key : articulationPatterns.keys()) {
        result->setPattern(articulationTypeFromString(key),
                           patternsScopeFromJson(jsonArray(articulationPatterns.value(key))));
    }

    return result;
}

void ArticulationProfilesRepository::saveProfile(const io::path_t& path, const ArticulationsProfilePtr profilePtr)
{
    IF_ASSERT_FAILED(profilePtr) {
        return;
    }

    JsonObject rootObj;

    rootObj.set(SUPPORTED_FAMILIES, supportedFamiliesToJson(profilePtr->supportedFamilies));

    JsonObject articulationPatterns;

    for (const auto& pair : profilePtr->data()) {
        articulationPatterns.set(articulationTypeToString(pair.first).toStdString(),
                                 patternsScopeToJson(pair.second));
    }

    rootObj.set(PATTERNS_KEY, articulationPatterns);

    ByteArray json = JsonDocument(rootObj).toJson();
    Ret fileWriting = fileSystem()->writeFile(path, json);

    if (!fileWriting) {
        LOGE() << "Unable to write MPE Articulation Profile, err: " << fileWriting.toString();
        return;
    }

    m_profileChanged.send(path);
}

Channel<io::path_t> ArticulationProfilesRepository::profileChanged() const
{
    return m_profileChanged;
}

std::vector<ArticulationFamily> ArticulationProfilesRepository::supportedFamiliesFromJson(const JsonArray& array) const
{
    std::vector<ArticulationFamily> result;
    result.reserve(array.size());

    for (size_t i = 0; i < array.size(); ++i) {
        result.push_back(articulationFamilyFromString(array.at(i).toString()));
    }

    return result;
}

JsonArray ArticulationProfilesRepository::supportedFamiliesToJson(const std::vector<ArticulationFamily>& families) const
{
    JsonArray result;

    for (const auto& family : families) {
        result.append(articulationFamilyToString(family));
    }

    return result;
}

ArticulationPattern ArticulationProfilesRepository::patternsScopeFromJson(const JsonArray& array) const
{
    ArticulationPattern result;

    for (size_t i = 0; i < array.size(); ++i) {
        JsonObject patternObj = jsonObject(array.at(i));

        duration_percentage_t position = patternObj.value(PATTERN_POS_KEY).toInt();

        ArrangementPattern arrangementPattern = arrangementPatternFromJson(jsonObject(patternObj.value(ARRANGEMENT_PATTERN_KEY)));
        PitchPattern pitchPattern = pitchPatternFromJson(jsonObject(patternObj.value(PITCH_PATTERN_KEY)));
        ExpressionPattern expressionPattern = expressionPatternFromJson(jsonObject(patternObj.value(EXPRESSION_PATTERN)));

        ArticulationPatternSegment articulation;
        articulation.arrangementPattern = std::move(arrangementPattern);
        articulation.pitchPattern = std::move(pitchPattern);
        articulation.expressionPattern = std::move(expressionPattern);

        result.emplace(position, std::move(articulation));
    }

    return result;
}

JsonArray ArticulationProfilesRepository::patternsScopeToJson(const ArticulationPattern& scope) const
{
    JsonArray result;

    for (const auto& pair : scope) {
        JsonObject pattern;
        pattern.set(PATTERN_POS_KEY, static_cast<int>(pair.first));
        pattern.set(ARRANGEMENT_PATTERN_KEY, arrangementPatternToJson(pair.second.arrangementPattern));
        pattern.set(PITCH_PATTERN_KEY, pitchPatternToJson(pair.second.pitchPattern));
        pattern.set(EXPRESSION_PATTERN, expressionPatternToJson(pair.second.expressionPattern));

        result.append(std::move(pattern));
    }

    return result;
}

ArrangementPattern ArticulationProfilesRepository::arrangementPatternFromJson(const JsonObject& obj) const
{
    ArrangementPattern result;

    result.durationFactor = obj.value(DURATION_FACTOR_KEY).toInt();
    result.timestampOffset = obj.value(TIMESTAMP_OFFSET_KEY).toInt();

    return result;
}

JsonObject ArticulationProfilesRepository::arrangementPatternToJson(const ArrangementPattern& pattern) const
{
    JsonObject result;

    result.set(DURATION_FACTOR_KEY, static_cast<int>(pattern.durationFactor));
    result.set(TIMESTAMP_OFFSET_KEY, static_cast<int>(pattern.timestampOffset));

    return result;
}

PitchPattern ArticulationProfilesRepository::pitchPatternFromJson(const JsonObject& obj) const
{
    PitchPattern result;

    JsonArray offsets = jsonArray(obj.value(PITCH_OFFSETS_KEY));

    for (size_t i = 0; i < offsets.size(); ++i) {
        JsonObject offsetObj = jsonObject(offsets.at(i));

        result.pitchOffsetMap.emplace(offsetObj.value(OFFSET_POS_KEY).toInt(),
                                      offsetObj.value(OFFSET_VAL_KEY).toInt());
    }

    return result;
}

JsonObject ArticulationProfilesRepository::pitchPatternToJson(const PitchPattern& pattern) const
{
    JsonObject result;

    JsonArray pitchOffsets;

    for (const auto& pair : pattern.pitchOffsetMap) {
        JsonObject offsetObj;
        offsetObj.set(OFFSET_POS_KEY, static_cast<int>(pair.first));
        offsetObj.set(OFFSET_VAL_KEY, static_cast<int>(pair.second));

        pitchOffsets.append(std::move(offsetObj));
    }

    result.set(PITCH_OFFSETS_KEY, pitchOffsets);

    return result;
}

ExpressionPattern ArticulationProfilesRepository::expressionPatternFromJson(const JsonObject& obj) const
{
    ExpressionPattern result;

    JsonArray offsets = jsonArray(obj.value(DYNAMIC_OFFSETS_KEY));

    for (size_t i = 0; i < offsets.size(); ++i) {
        JsonObject offsetObj = jsonObject(offsets.at(i));
        result.dynamicOffsetMap.emplace(offsetObj.value(OFFSET_POS_KEY).toInt(),
                                        offsetObj.value(OFFSET_VAL_KEY).toInt());
    }

    return result;
}

JsonObject ArticulationProfilesRepository::expressionPatternToJson(const ExpressionPattern& pattern) const
{
    JsonObject result;

    JsonArray dynamicOffsets;

    for (const auto& pair : pattern.dynamicOffsetMap) {
        JsonObject offsetObj;
        offsetObj.set(OFFSET_POS_KEY, static_cast<int>(pair.first));
        offsetObj.set(OFFSET_VAL_KEY, static_cast<int>(pair.second));

        dynamicOffsets.append(std::move(offsetObj));
    }

    result.set(DYNAMIC_OFFSETS_KEY, dynamicOffsets);

    return result;
}
