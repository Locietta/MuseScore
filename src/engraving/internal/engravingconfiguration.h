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

#pragma once


#include "../iengravingconfiguration.h"

namespace mu::engraving {
class EngravingConfiguration : public IEngravingConfiguration
{
public:
    EngravingConfiguration();

    void init();

    muse::io::path_t appDataPath() const override;

    muse::io::path_t defaultStyleFilePath() const override;
    void setDefaultStyleFilePath(const muse::io::path_t& path) override;

    muse::io::path_t partStyleFilePath() const override;
    void setPartStyleFilePath(const muse::io::path_t& path) override;

    SizeF defaultPageSize() const override;

    bool canLayoutIcons() const override;
    String iconsFontFamily() const override;

    Color defaultColor() const override;

    Color displayedDefaultColor(bool inverted) const override;
    void setDisplayedDefaultColor(Color color, bool inverted) override;
    void resetDisplayedDefaultColors() override;

    Color indicatorIconInvertedSelectionColor() const override;
    Color lassoColor() const override;
    Color warningColor() const override;
    Color warningSelectedColor() const override;
    Color criticalColor() const override;
    Color criticalBackgroundColor() const override;
    Color criticalSelectedColor() const override;
    Color thumbnailBackgroundColor() const override;
    Color noteBackgroundColor() const override;
    Color fontPrimaryColor() const override;
    Color voiceColor(voice_idx_t voiceIdx) const override;

    Color selectionColor(voice_idx_t voiceIndex = 0, bool itemVisible = true, bool itemIsUnlinkedFromScore = false) const override;
    void setSelectionColor(voice_idx_t voiceIndex, Color color) override;

    Color highlightSelectionColor(voice_idx_t voice = 0) const override;

    bool dynamicsApplyToAllVoices() const override;
    void setDynamicsApplyToAllVoices(bool v) override;

    bool autoUpdateFretboardDiagrams() const override;
    void setAutoUpdateFretboardDiagrams(bool v) override;

    Color formattingColor() const override;

    Color frameColor() const override;

    Color scoreGreyColor() const override;

    Color invisibleColor() const override;

    Color unlinkedColor() const override;

    const DebuggingOptions& debuggingOptions() const override;
    void setDebuggingOptions(const DebuggingOptions& options) override;

    bool allowReadingImagesFromOutsideMscz() const override;

    bool guitarProImportExperimental() const override;
    bool negativeFretsAllowed() const override;
    void setGuitarProMultivoiceEnabled(bool multiVoice) override;
    bool guitarProMultivoiceEnabled() const override;
    bool minDistanceForPartialSkylineCalculated() const override;
    bool specificSlursLayoutWorkaround() const override;
    bool preferSameStringForTranspose() const override;
    void setPreferSameStringForTranspose(bool preferSameString) override;
    bool keepDeadNotesUnchangedOnTranspose() const override;

private:

    DebuggingOptions m_debuggingOptions;

    Color m_cachedDisplayedDefaultColor;
    Color m_cachedInvertedDisplayedDefaultColor;

    bool m_multiVoice = false;
};
}
