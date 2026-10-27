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
#include "importmidi_operations.h"

#include "io/file.h"
#include "serialization/xmlstreamreader.h"

#include "log.h"

namespace mu::iex::midi {
MidiOperations::Data midiImportOperations;

namespace MidiOperations {
static int readBoolFromXml(muse::XmlStreamReader& xml)
{
    int value = -1;
    const muse::AsciiStringView name = xml.name();
    xml.readNext();

    if (xml.tokenType() == muse::XmlStreamReader::Characters) {
        if (xml.text() == u"true") {
            value = 1;
        } else if (xml.text() == u"false") {
            value = 0;
        } else {
            LOGD() << "Load MIDI import operations from file: unknown" << name << "value";
        }
    }
    return value;
}

// Terms: <name attribute="value">text</name>

static void setOperationsFromFile(const std::string& fileName, Opers& opers)
{
    if (fileName.empty()) {
        return;
    }

    muse::io::File file{muse::io::path_t(fileName)};
    if (!file.open(muse::io::IODevice::ReadOnly)) {
        LOGD("Load MIDI import operations from file: cannot open input file");
        return;
    }

    muse::XmlStreamReader xml(&file);

    while (!xml.atEnd() && !xml.isError()) {
        const auto token = xml.readNext();

        if (token == muse::XmlStreamReader::StartDocument) {
            continue;
        }
        if (token != muse::XmlStreamReader::StartElement) {          // like <elem>
            continue;
        }
        if (xml.name() == "MidiOptions") {
            continue;
        }

        if (xml.name() == "QuantValue") {
            xml.readNext();
            if (xml.tokenType() == muse::XmlStreamReader::Characters) {
                bool ok = false;
                const int index = xml.text().trimmed().toInt(&ok);
                if (ok) {
                    switch (index) {
                    case 0:
                        opers.quantValue.setDefaultValue(QuantValue::Q_4, false);
                        break;
                    case 1:
                        opers.quantValue.setDefaultValue(QuantValue::Q_8, false);
                        break;
                    case 2:
                        opers.quantValue.setDefaultValue(QuantValue::Q_16, false);
                        break;
                    case 3:
                        opers.quantValue.setDefaultValue(QuantValue::Q_32, false);
                        break;
                    case 4:
                        opers.quantValue.setDefaultValue(QuantValue::Q_64, false);
                        break;
                    case 5:
                        opers.quantValue.setDefaultValue(QuantValue::Q_128, false);
                        break;
                    case 6:
                        opers.quantValue.setDefaultValue(QuantValue::Q_256, false);
                        break;
                    case 7:
                        opers.quantValue.setDefaultValue(QuantValue::Q_512, false);
                        break;
                    case 8:
                        opers.quantValue.setDefaultValue(QuantValue::Q_1024, false);
                        break;
                    default:
                        LOGD("Load MIDI import operations from file: "
                             "unknown max quantization value");
                        break;
                    }
                }
            }
        } else if (xml.name() == "VoiceCount") {
            xml.readNext();
            if (xml.tokenType() == muse::XmlStreamReader::Characters) {
                bool ok = false;
                const int index = xml.text().trimmed().toInt(&ok);
                if (ok) {
                    switch (index) {
                    case 0:
                        opers.maxVoiceCount.setDefaultValue(VoiceCount::V_1, false);
                        break;
                    case 1:
                        opers.maxVoiceCount.setDefaultValue(VoiceCount::V_2, false);
                        break;
                    case 2:
                        opers.maxVoiceCount.setDefaultValue(VoiceCount::V_3, false);
                        break;
                    case 3:
                        opers.maxVoiceCount.setDefaultValue(VoiceCount::V_4, false);
                        break;
                    default:
                        LOGD("Load MIDI import operations from file: "
                             "unknown max voice count");
                        break;
                    }
                }
            }
        } else if (xml.name() == "Duplets") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.search2plets.setDefaultValue(value, false);
            }
        } else if (xml.name() == "Triplets") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.search3plets.setDefaultValue(value, false);
            }
        } else if (xml.name() == "Quadruplets") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.search4plets.setDefaultValue(value, false);
            }
        } else if (xml.name() == "Quintuplets") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.search5plets.setDefaultValue(value, false);
            }
        } else if (xml.name() == "Septuplets") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.search7plets.setDefaultValue(value, false);
            }
        } else if (xml.name() == "Nonuplets") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.search9plets.setDefaultValue(value, false);
            }
        } else if (xml.name() == "HumanPerformance") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.isHumanPerformance.setDefaultValue(value, false);
            }
        } else if (xml.name() == "MeasureCount2xLess") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.measureCount2xLess.setDefaultValue(value, false);
            }
        } else if (xml.name() == "SplitStaff") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.doStaffSplit.setDefaultValue(value, false);
            }
        } else if (xml.name() == "ClefChanges") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.changeClef.setDefaultValue(value, false);
            }
        } else if (xml.name() == "SimplifyDurations") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.simplifyDurations.setDefaultValue(value, false);
            }
        } else if (xml.name() == "ShowStaccato") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.showStaccato.setDefaultValue(value, false);
            }
        } else if (xml.name() == "DottedNotes") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.useDots.setDefaultValue(value, false);
            }
        } else if (xml.name() == "RecognizePickupBar") {
            const int value = readBoolFromXml(xml);
            if (value >= 0) {
                opers.searchPickupMeasure.setDefaultValue(value, false);
            }
        } else if (xml.name() == "Swing") {
            xml.readNext();
            if (xml.tokenType() == muse::XmlStreamReader::Characters) {
                bool ok = false;
                const int index = xml.text().trimmed().toInt(&ok);
                if (ok) {
                    switch (index) {
                    case 0:
                        opers.swing.setDefaultValue(Swing::NONE, false);
                        break;
                    case 1:
                        opers.swing.setDefaultValue(Swing::SHUFFLE, false);
                        break;
                    case 2:
                        opers.swing.setDefaultValue(Swing::SWING, false);
                        break;
                    default:
                        LOGD("Load MIDI import operations from file: "
                             "unknown swing");
                        break;
                    }
                }
            }
        }
    }
    if (xml.isError()) {
        LOGD("Load MIDI import operations from file: cannot parse input file");
    }
}

//-------------------------------------------------------------------------------------------

FileData* Data::data()
{
    const auto it = _data.find(_currentMidiFile);
    if (it != _data.end()) {
        return &it->second;
    }
    return nullptr;
}

const FileData* Data::data() const
{
    const auto it = _data.find(_currentMidiFile);
    if (it != _data.end()) {
        return &it->second;
    }
    return nullptr;
}

void Data::addNewMidiFile(const std::string& fileName)
{
    if (_data.find(fileName) != _data.end()) {
        return;
    }
    FileData fileData;
    setOperationsFromFile(_midiOperationsFile, fileData.trackOpers);
    _data.insert({ fileName, fileData });
}

const MidiFile* Data::midiFile(const std::string& fileName)
{
    const auto it = _data.find(fileName);
    if (it != _data.end()) {
        return &it->second.midiFile;
    }
    return nullptr;
}

std::vector<std::string> Data::allMidiFiles() const
{
    std::vector<std::string> list;
    for (const auto& d: _data) {
        list.push_back(d.first);
    }
    return list;
}

void Data::setMidiFileData(const std::string& fileName, const MidiFile& midiFile)
{
    _data[fileName].midiFile = midiFile;
}

void Data::excludeMidiFile(const std::string& fileName)
{
    _data.erase(fileName);
}

bool Data::hasMidiFile(const std::string& fileName)
{
    return _data.find(fileName) != _data.end();
}

int Data::currentTrack() const
{
    Q_ASSERT_X(_currentTrack >= 0,
               "Data::currentTrack", "Invalid current track index");

    return _currentTrack;
}

void Data::setOperationsFile(const std::string& fileName)
{
    if (muse::io::File::exists(muse::io::path_t(fileName))) {
        _midiOperationsFile = fileName;
    }
}
} // namespace MidiOperations
} // namespace mu::iex::midi
