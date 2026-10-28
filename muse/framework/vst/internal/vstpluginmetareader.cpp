/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2022 MuseScore Limited and others
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

#include "vstpluginmetareader.h"

#include "audio/common/audiotypes.h"

#include "vsttypes.h"
#include "vstpluginattrs.h"
#include "vsterrors.h"

#include "log.h"

using namespace muse;
using namespace muse::audioplugins;
using namespace muse::vst;

audioplugins::PluginType VstPluginMetaReader::metaType() const
{
    return std::string(AUDIO_RESOURCE_TYPE_NAME);
}

bool VstPluginMetaReader::canReadMeta(const io::path_t& pluginPath) const
{
    return io::suffix(pluginPath) == VST3_PACKAGE_EXTENSION;
}

RetVal<PluginMetaList> VstPluginMetaReader::readMeta(const io::path_t& pluginPath) const
{
    PluginModulePtr module = createModule(pluginPath);
    if (!module) {
        return make_ret(Err::NoPluginModule);
    }

    const auto& factory = module->getFactory();
    PluginMetaList result;

    for (const ClassInfo& classInfo : factory.classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass) {
            continue;
        }

        PluginMeta meta;
        meta.id = io::completeBasename(pluginPath).toStdString();
        meta.type = AUDIO_RESOURCE_TYPE_NAME;
        meta.attributes.emplace(CATEGORIES_ATTRIBUTE, String::fromStdString(classInfo.subCategoriesString()));
        meta.attributes.emplace(muse::audio::HAS_NATIVE_EDITOR_SUPPORT_ATTRIBUTE, u"true");
        meta.vendor = classInfo.vendor();

        result.emplace_back(std::move(meta));
        break;
    }

    if (result.empty()) {
        return make_ret(Err::NoAudioEffect);
    }

    return RetVal<PluginMetaList>::make_ok(result);
}
