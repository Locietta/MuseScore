/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
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
#include "engravingconfiguration.h"

#include <cstdlib>


#include "global/settings.h"
#include "draw/types/color.h"
#include "dom/mscore.h"
#include "translation.h"

#include "log.h"

using namespace mu;
using namespace muse;
using namespace muse::draw;
using namespace mu::engraving;

static Val colorValue(const Color& color)
{
    return Val(color.toString());
}

static Color colorFromValue(const Val& value)
{
    return Color(value.toString().c_str());
}

static const Settings::Key DEFAULT_STYLE_FILE_PATH("engraving", "engraving/style/defaultStyleFile");
static const Settings::Key PART_STYLE_FILE_PATH("engraving", "engraving/style/partStyleFile");

static const Settings::Key DISPLAYED_DEFAULT_COLOR("engraving", "engraving/colors/displayedDefaultColor");
static const Settings::Key INVERTED_DISPLAYED_DEFAULT_COLOR("engraving", "engraving/colors/invertedDisplayedDefaultColor");
static const Settings::Key ALL_VOICES_COLOR("engraving", "engraving/colors/allVoicesColor");
static const Settings::Key FORMATTING_COLOR("engraving", "engraving/colors/formattingColor");
static const Settings::Key FRAME_COLOR("engraving", "engraving/colors/frameColor");
static const Settings::Key SCORE_GREY_COLOR("engraving", "engraving/color/scoreGreyColor");
static const Settings::Key INVISIBLE_COLOR("engraving", "engraving/colors/invisibleColor");
static const Settings::Key UNLINKED_COLOR("engraving", "engraving/colors/unlinkedColor");

static const Settings::Key DYNAMICS_APPLY_TO_ALL_VOICES("engraving", "score/dynamicsApplyToAllVoices");
static const Settings::Key FRETBOARD_DIAGRAMS_AUTO_UPDATE("engraving", "score/fretboardDiagramsAutoUpdate");

struct VoiceColor {
    Settings::Key key;
    Color color;
};

static VoiceColor VOICE_COLORS[VOICES + 1];

static const Color UNLINKED_ITEM_COLOR = "#FF9300";
static const Color DEFAULT_DISPLAYED_INVERTED_COLOR = "#CBCBCD";

EngravingConfiguration::EngravingConfiguration()
    : m_cachedDisplayedDefaultColor(Color::BLACK),
    m_cachedInvertedDisplayedDefaultColor(DEFAULT_DISPLAYED_INVERTED_COLOR)
{}

void EngravingConfiguration::init()
{
    static const Color DEFAULT_VOICE_COLORS[VOICES + 1] {
        "#0065BF",
        "#007F00",
        "#C53F00",
        "#C31989",
        "#6038FC", // "all voices"
    };

    settings()->setDefaultValue(DEFAULT_STYLE_FILE_PATH, Val(muse::io::path_t()));

    settings()->setDefaultValue(PART_STYLE_FILE_PATH, Val(muse::io::path_t()));

    settings()->setDefaultValue(DISPLAYED_DEFAULT_COLOR, colorValue(defaultColor()));
    settings()->setDescription(DISPLAYED_DEFAULT_COLOR, muse::trc("engraving", "Displayed default color"));
    settings()->setCanBeManuallyEdited(DISPLAYED_DEFAULT_COLOR, true);
    m_cachedDisplayedDefaultColor = colorFromValue(settings()->value(DISPLAYED_DEFAULT_COLOR));

    settings()->setDefaultValue(INVERTED_DISPLAYED_DEFAULT_COLOR, colorValue(Color(DEFAULT_DISPLAYED_INVERTED_COLOR)));
    settings()->setDescription(INVERTED_DISPLAYED_DEFAULT_COLOR,
                               muse::trc("engraving", "Displayed default color when score colors are inverted"));
    settings()->setCanBeManuallyEdited(INVERTED_DISPLAYED_DEFAULT_COLOR, true);
    m_cachedInvertedDisplayedDefaultColor = colorFromValue(settings()->value(INVERTED_DISPLAYED_DEFAULT_COLOR));

    for (voice_idx_t voice = 0; voice < VOICES; ++voice) {
        Settings::Key key("engraving", "engraving/colors/voice" + std::to_string(voice + 1));

        settings()->setDefaultValue(key, colorValue(DEFAULT_VOICE_COLORS[voice]));
        settings()->setDescription(key, "Voice " + std::to_string(voice + 1) + " color");
        settings()->setCanBeManuallyEdited(key, true);

        Color currentColor = colorFromValue(settings()->value(key));
        VOICE_COLORS[voice] = VoiceColor { std::move(key), currentColor };
    }

    static constexpr int ALL_VOICES_IDX = VOICES;
    settings()->setDefaultValue(ALL_VOICES_COLOR, colorValue(DEFAULT_VOICE_COLORS[ALL_VOICES_IDX]));
    settings()->setDescription(ALL_VOICES_COLOR, muse::trc("engraving", "All voices color"));
    settings()->setCanBeManuallyEdited(ALL_VOICES_COLOR, true);
    Color currentColor = colorFromValue(settings()->value(ALL_VOICES_COLOR));
    VOICE_COLORS[ALL_VOICES_IDX] = VoiceColor { std::move(ALL_VOICES_COLOR), currentColor };

    settings()->setDefaultValue(DYNAMICS_APPLY_TO_ALL_VOICES, Val(true));

    settings()->setDefaultValue(FRETBOARD_DIAGRAMS_AUTO_UPDATE, Val(true));

    settings()->setDefaultValue(FRAME_COLOR, colorValue(Color("#A0A0A4")));
    settings()->setDescription(FRAME_COLOR, muse::trc("engraving", "Frame color"));
    settings()->setCanBeManuallyEdited(FRAME_COLOR, true);

    settings()->setDefaultValue(SCORE_GREY_COLOR, colorValue(Color("#A0A0A4")));
    settings()->setDescription(SCORE_GREY_COLOR, muse::trc("engraving", "Score grey color"));
    settings()->setCanBeManuallyEdited(SCORE_GREY_COLOR, false);

    settings()->setDefaultValue(FORMATTING_COLOR, colorValue(Color("#C31989")));
    settings()->setDescription(FORMATTING_COLOR, muse::trc("engraving", "Formatting color"));
    settings()->setCanBeManuallyEdited(FORMATTING_COLOR, true);

    settings()->setDefaultValue(INVISIBLE_COLOR, colorValue(Color("#808080")));
    settings()->setDescription(INVISIBLE_COLOR, muse::trc("engraving", "Invisible color"));
    settings()->setCanBeManuallyEdited(INVISIBLE_COLOR, true);

    settings()->setDefaultValue(UNLINKED_COLOR, colorValue(Color(UNLINKED_ITEM_COLOR)));
    settings()->setDescription(UNLINKED_COLOR, muse::trc("engraving", "Desynchronized color"));
    settings()->setCanBeManuallyEdited(UNLINKED_COLOR, true);
}

muse::io::path_t EngravingConfiguration::appDataPath() const
{
    // The engine has no application data directory.
    return muse::io::path_t();
}

muse::io::path_t EngravingConfiguration::defaultStyleFilePath() const
{
    return settings()->value(DEFAULT_STYLE_FILE_PATH).toPath();
}

void EngravingConfiguration::setDefaultStyleFilePath(const muse::io::path_t& path)
{
    settings()->setSharedValue(DEFAULT_STYLE_FILE_PATH, Val(path.toStdString()));
}

muse::io::path_t EngravingConfiguration::partStyleFilePath() const
{
    return settings()->value(PART_STYLE_FILE_PATH).toPath();
}

void EngravingConfiguration::setPartStyleFilePath(const muse::io::path_t& path)
{
    settings()->setSharedValue(PART_STYLE_FILE_PATH, Val(path.toStdString()));
}

static bool defaultPageSizeIsLetter()
{
    // try PAPERSIZE environment variable
    const char* papersize = getenv("PAPERSIZE");
    if (papersize) {
        return strcmp(papersize, "letter") == 0;
    }
    return false;
}

SizeF EngravingConfiguration::defaultPageSize() const
{
    // Needs to be determined only once, therefore static
    static SizeF size = defaultPageSizeIsLetter() ? SizeF(8.5, 11.0) : SizeF(210.0 / 25.4, 297.0 / 25.4);

    return size;
}

bool EngravingConfiguration::canLayoutIcons() const
{
    return false;
}

muse::String EngravingConfiguration::iconsFontFamily() const
{
    return String();
}

Color EngravingConfiguration::defaultColor() const
{
    return Color::BLACK;
}

Color EngravingConfiguration::displayedDefaultColor(bool inverted) const
{
    if (inverted) {
        return m_cachedInvertedDisplayedDefaultColor;
    }
    return m_cachedDisplayedDefaultColor;
}

void EngravingConfiguration::setDisplayedDefaultColor(Color color, bool inverted)
{
    if (inverted) {
        settings()->setSharedValue(INVERTED_DISPLAYED_DEFAULT_COLOR, colorValue(color));
        m_cachedInvertedDisplayedDefaultColor = color;
    } else {
        settings()->setSharedValue(DISPLAYED_DEFAULT_COLOR, colorValue(color));
        m_cachedDisplayedDefaultColor = color;
    }
}

void EngravingConfiguration::resetDisplayedDefaultColors()
{
    settings()->setSharedValue(DISPLAYED_DEFAULT_COLOR, settings()->defaultValue(DISPLAYED_DEFAULT_COLOR));
    settings()->setSharedValue(INVERTED_DISPLAYED_DEFAULT_COLOR, settings()->defaultValue(INVERTED_DISPLAYED_DEFAULT_COLOR));
    m_cachedDisplayedDefaultColor = colorFromValue(settings()->value(DISPLAYED_DEFAULT_COLOR));
    m_cachedInvertedDisplayedDefaultColor = colorFromValue(settings()->value(INVERTED_DISPLAYED_DEFAULT_COLOR));
}

Color EngravingConfiguration::indicatorIconInvertedSelectionColor() const
{
    Color color = voiceColor(0);
    color.applyTint(0.35f);
    return color;
}

Color EngravingConfiguration::lassoColor() const
{
    return "#00323200";
}

Color EngravingConfiguration::warningColor() const
{
    return "#808000";
}

Color EngravingConfiguration::warningSelectedColor() const
{
    return "#565600";
}

Color EngravingConfiguration::criticalColor() const
{
    return Color::RED;
}

Color EngravingConfiguration::criticalBackgroundColor() const
{
    return Color::RED;
}

Color EngravingConfiguration::criticalSelectedColor() const
{
    return "#8B0000";
}

Color EngravingConfiguration::thumbnailBackgroundColor() const
{
    return Color::WHITE;
}

Color EngravingConfiguration::noteBackgroundColor() const
{
    return Color::WHITE;
}

Color EngravingConfiguration::fontPrimaryColor() const
{
    return Color::BLACK;
}

Color EngravingConfiguration::voiceColor(voice_idx_t voiceIdx) const
{
    return VOICE_COLORS[voiceIdx].color;
}

Color EngravingConfiguration::selectionColor(voice_idx_t voice, bool itemVisible, bool itemIsUnlinkedFromScore) const
{
    Color color = itemIsUnlinkedFromScore ? unlinkedColor() : VOICE_COLORS[voice].color;

    if (itemVisible) {
        return color;
    }

    constexpr float tint = .6f; // Between 0 and 1. Higher means lighter, lower means darker

    color.applyTint(tint);

    return color;
}

void EngravingConfiguration::setSelectionColor(voice_idx_t voiceIndex, Color color)
{
    settings()->setSharedValue(VOICE_COLORS[voiceIndex].key, colorValue(color));
    VOICE_COLORS[voiceIndex].color = color;
}

Color EngravingConfiguration::highlightSelectionColor(voice_idx_t voice) const
{
    Color color = selectionColor(voice);
    color.applyTint(0.35f);
    return color;
}

bool EngravingConfiguration::dynamicsApplyToAllVoices() const
{
    return settings()->value(DYNAMICS_APPLY_TO_ALL_VOICES).toBool();
}

void EngravingConfiguration::setDynamicsApplyToAllVoices(bool v)
{
    settings()->setSharedValue(DYNAMICS_APPLY_TO_ALL_VOICES, Val(v));
}

bool EngravingConfiguration::autoUpdateFretboardDiagrams() const
{
    return settings()->value(FRETBOARD_DIAGRAMS_AUTO_UPDATE).toBool();
}

void EngravingConfiguration::setAutoUpdateFretboardDiagrams(bool v)
{
    settings()->setSharedValue(FRETBOARD_DIAGRAMS_AUTO_UPDATE, Val(v));
}

Color EngravingConfiguration::formattingColor() const
{
    return colorFromValue(settings()->value(FORMATTING_COLOR));
}

Color EngravingConfiguration::frameColor() const
{
    return colorFromValue(settings()->value(FRAME_COLOR));
}

Color EngravingConfiguration::scoreGreyColor() const
{
    return colorFromValue(settings()->value(SCORE_GREY_COLOR));
}

Color EngravingConfiguration::invisibleColor() const
{
    return colorFromValue(settings()->value(INVISIBLE_COLOR));
}

Color EngravingConfiguration::unlinkedColor() const
{
    return colorFromValue(settings()->value(UNLINKED_COLOR));
}

const IEngravingConfiguration::DebuggingOptions& EngravingConfiguration::debuggingOptions() const
{
    return m_debuggingOptions;
}

void EngravingConfiguration::setDebuggingOptions(const DebuggingOptions& options)
{
    m_debuggingOptions = options;
}

bool EngravingConfiguration::allowReadingImagesFromOutsideMscz() const
{
    return false;
}

bool EngravingConfiguration::guitarProImportExperimental() const
{
    // The experimental Guitar Pro import is a desktop preference the engine does not offer.
    return false;
}

bool EngravingConfiguration::negativeFretsAllowed() const
{
    return guitarProImportExperimental();
}

void EngravingConfiguration::setGuitarProMultivoiceEnabled(bool multiVoice)
{
    m_multiVoice = multiVoice;
}

bool EngravingConfiguration::guitarProMultivoiceEnabled() const
{
    return m_multiVoice;
}

bool EngravingConfiguration::minDistanceForPartialSkylineCalculated() const
{
    return guitarProImportExperimental();
}

bool EngravingConfiguration::specificSlursLayoutWorkaround() const
{
    return guitarProImportExperimental();
}

bool EngravingConfiguration::preferSameStringForTranspose() const
{
    return false;
}

void EngravingConfiguration::setPreferSameStringForTranspose(bool /*preferSameString*/)
{
}

bool EngravingConfiguration::keepDeadNotesUnchangedOnTranspose() const
{
    return false;
}
