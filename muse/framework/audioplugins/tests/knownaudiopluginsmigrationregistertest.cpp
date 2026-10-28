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
#include <gtest/gtest.h>

#include "global/serialization/json.h"

#include "audioplugins/internal/knownaudiopluginsmigrationregister.h"

using namespace muse;
using namespace muse::audioplugins;

namespace muse::audioplugins {
class AudioPlugins_KnownAudioPluginsMigrationRegisterTest : public ::testing::Test
{
protected:
    KnownAudioPluginsMigrationRegister m_register;

    static JsonArray makeArray(const std::string& tag)
    {
        JsonObject obj;
        obj.set("tag", tag);
        JsonArray arr;
        arr << obj;
        return arr;
    }

    static std::string firstTag(const JsonArray& arr)
    {
        if (arr.empty()) {
            return {};
        }
        return arr.at(0).toObject().value("tag").toString().toStdString();
    }
};
}

TEST_F(AudioPlugins_KnownAudioPluginsMigrationRegisterTest, SameVersion_NoOp)
{
    JsonArray plugins = makeArray("v0");

    Ret ret = m_register.migrate(0, 0, plugins);

    EXPECT_TRUE(ret);
    EXPECT_EQ(firstTag(plugins), "v0");
}

TEST_F(AudioPlugins_KnownAudioPluginsMigrationRegisterTest, SingleStep_AppliesCallback)
{
    m_register.registerMigration(0, [](const JsonArray&) {
        return makeArray("v1");
    });

    JsonArray plugins = makeArray("v0");

    Ret ret = m_register.migrate(0, 1, plugins);

    EXPECT_TRUE(ret);
    EXPECT_EQ(firstTag(plugins), "v1");
}

TEST_F(AudioPlugins_KnownAudioPluginsMigrationRegisterTest, MultiStep_ChainsInOrder)
{
    m_register.registerMigration(0, [](const JsonArray& in) {
        // v0 -> v1: input must still be v0
        EXPECT_EQ(firstTag(in), "v0");
        return makeArray("v1");
    });
    m_register.registerMigration(1, [](const JsonArray& in) {
        // v1 -> v2: input must already be v1 (proves ordering)
        EXPECT_EQ(firstTag(in), "v1");
        return makeArray("v2");
    });

    JsonArray plugins = makeArray("v0");

    Ret ret = m_register.migrate(0, 2, plugins);

    EXPECT_TRUE(ret);
    EXPECT_EQ(firstTag(plugins), "v2");
}

TEST_F(AudioPlugins_KnownAudioPluginsMigrationRegisterTest, MissingMigration_Fails)
{
    // Slots beyond the pre-registered range keep this focused on the
    // missing-migrator path: only v5 -> v6 is registered, we request v5 -> v7.
    m_register.registerMigration(5, [](const JsonArray&) {
        return makeArray("v6");
    });

    JsonArray plugins = makeArray("v5");

    Ret ret = m_register.migrate(5, 7, plugins);

    EXPECT_FALSE(ret);
    // the error must name the missing migrator
    EXPECT_NE(ret.text().find("missing migrator"), std::string::npos);
    EXPECT_NE(ret.text().find("6 -> 7"), std::string::npos);
}

TEST_F(AudioPlugins_KnownAudioPluginsMigrationRegisterTest, BackwardMigration_Fails)
{
    JsonArray plugins = makeArray("v1");

    Ret ret = m_register.migrate(2, 1, plugins);

    EXPECT_FALSE(ret);
}

TEST_F(AudioPlugins_KnownAudioPluginsMigrationRegisterTest, FutureVersion_FailsWithNewerHint)
{
    // a newer-build cache lands on an older build: the error must say "newer"
    JsonArray plugins = makeArray("v99");

    Ret ret = m_register.migrate(99, 3, plugins);

    EXPECT_FALSE(ret);
    EXPECT_NE(ret.text().find("newer"), std::string::npos);
}

TEST_F(AudioPlugins_KnownAudioPluginsMigrationRegisterTest, V2ToV3_MovesHasNativeEditorSupportIntoAttributes)
{
    // Mirrors the v2 -> v3 migration registered MuseScore-side in
    // src/app/internal/audiopluginsappconfigmodule.cpp.
    m_register.registerMigration(2, [](const JsonArray& plugins) {
        JsonArray out;
        for (size_t i = 0; i < plugins.size(); ++i) {
            JsonObject obj = plugins.at(i).toObject();
            JsonObject meta = obj.value("meta").toObject();
            if (meta.contains("hasNativeEditorSupport")) {
                JsonObject attrs;
                if (meta.contains("attributes")) {
                    attrs = meta.value("attributes").toObject();
                }
                const bool b = meta.value("hasNativeEditorSupport").toBool();
                attrs.set("hasNativeEditorSupport", b ? std::string("true") : std::string("false"));
                meta.set("attributes", attrs);

                JsonObject metaWithoutLegacy;
                for (const std::string& k : meta.keys()) {
                    if (k == "hasNativeEditorSupport") {
                        continue;
                    }
                    metaWithoutLegacy.set(k, meta.value(k));
                }
                obj.set("meta", metaWithoutLegacy);
            }
            out << obj;
        }
        return out;
    });

    // [GIVEN] A v0-shaped plugin entry: meta.hasNativeEditorSupport = true at top level.
    JsonObject meta;
    meta.set("id", std::string("AAA"));
    meta.set("type", std::string("VstPlugin"));
    meta.set("hasNativeEditorSupport", true);

    JsonObject obj;
    obj.set("meta", meta);
    obj.set("path", std::string("/x/AAA.vst3"));

    JsonArray plugins;
    plugins << obj;

    // [WHEN] migrating from v2 to v3
    Ret ret = m_register.migrate(2, 3, plugins);

    // [THEN] hasNativeEditorSupport moved into meta.attributes as the string "true"
    ASSERT_TRUE(ret);
    ASSERT_EQ(plugins.size(), size_t(1));
    JsonObject migratedMeta = plugins.at(0).toObject().value("meta").toObject();
    EXPECT_FALSE(migratedMeta.contains("hasNativeEditorSupport"));
    EXPECT_EQ(migratedMeta.value("attributes").toObject().value("hasNativeEditorSupport").toStdString(), "true");
}

TEST_F(AudioPlugins_KnownAudioPluginsMigrationRegisterTest, V1ToV2_TranslatesEnabledIntoStateString)
{
    // Overrides the v1 -> v2 slot with the production body to isolate the
    // shape check; pre-registration itself is covered by FrameworkOwned_AutoRegisters.
    m_register.registerMigration(1, [](const JsonArray& plugins) {
        JsonArray out;
        for (size_t i = 0; i < plugins.size(); ++i) {
            JsonObject obj = plugins.at(i).toObject();
            const bool enabled = obj.value("enabled").toBool();
            obj.set("state", enabled ? std::string("Validated") : std::string("Error"));

            JsonObject rebuilt;
            for (const std::string& k : obj.keys()) {
                if (k == "enabled") {
                    continue;
                }
                rebuilt.set(k, obj.value(k));
            }
            out << rebuilt;
        }
        return out;
    });

    // [GIVEN] Two v1-shaped plugin entries: one enabled, one disabled-with-errorCode.
    JsonObject enabledObj;
    enabledObj.set("path", std::string("/x/AAA.vst3"));
    enabledObj.set("enabled", true);

    JsonObject failedObj;
    failedObj.set("path", std::string("/x/BBB.vst3"));
    failedObj.set("enabled", false);
    failedObj.set("errorCode", -42);

    JsonArray plugins;
    plugins << enabledObj;
    plugins << failedObj;

    // [WHEN] migrating from v1 to v2
    Ret ret = m_register.migrate(1, 2, plugins);

    // [THEN] enabled is gone; state reflects the boolean; errorCode preserved.
    ASSERT_TRUE(ret);
    ASSERT_EQ(plugins.size(), size_t(2));

    JsonObject migratedEnabled = plugins.at(0).toObject();
    EXPECT_FALSE(migratedEnabled.contains("enabled"));
    EXPECT_EQ(migratedEnabled.value("state").toStdString(), "Validated");

    JsonObject migratedFailed = plugins.at(1).toObject();
    EXPECT_FALSE(migratedFailed.contains("enabled"));
    EXPECT_EQ(migratedFailed.value("state").toStdString(), "Error");
    EXPECT_EQ(migratedFailed.value("errorCode").toInt(), -42);
}

TEST_F(AudioPlugins_KnownAudioPluginsMigrationRegisterTest, FrameworkOwned_AutoRegisters)
{
    // A fresh register must already carry the framework-owned migrations.
    KnownAudioPluginsMigrationRegister reg;

    // [WHEN] migrating a v0-shaped entry from v0 to v1
    JsonObject v0obj;
    v0obj.set("path", std::string("/x/AAA.vst3"));
    v0obj.set("enabled", true);
    JsonArray v0arr;
    v0arr << v0obj;

    Ret ret01 = reg.migrate(0, 1, v0arr);

    // [THEN] structural no-op: the entry passes through unchanged.
    ASSERT_TRUE(ret01);
    ASSERT_EQ(v0arr.size(), size_t(1));
    EXPECT_TRUE(v0arr.at(0).toObject().value("enabled").toBool());
    EXPECT_FALSE(v0arr.at(0).toObject().contains("state"));

    // [WHEN] migrating a v1-shaped entry from v1 to v2
    JsonObject v1enabled;
    v1enabled.set("path", std::string("/x/AAA.vst3"));
    v1enabled.set("enabled", true);
    JsonObject v1failed;
    v1failed.set("path", std::string("/x/BBB.vst3"));
    v1failed.set("enabled", false);
    v1failed.set("errorCode", -42);
    JsonArray v1arr;
    v1arr << v1enabled;
    v1arr << v1failed;

    Ret ret12 = reg.migrate(1, 2, v1arr);

    // [THEN] enabled has been translated to state; errorCode preserved.
    ASSERT_TRUE(ret12);
    ASSERT_EQ(v1arr.size(), size_t(2));
    EXPECT_FALSE(v1arr.at(0).toObject().contains("enabled"));
    EXPECT_EQ(v1arr.at(0).toObject().value("state").toStdString(), "Validated");
    EXPECT_FALSE(v1arr.at(1).toObject().contains("enabled"));
    EXPECT_EQ(v1arr.at(1).toObject().value("state").toStdString(), "Error");
    EXPECT_EQ(v1arr.at(1).toObject().value("errorCode").toInt(), -42);

    // [WHEN] migrating a v2 entry from v2 to v3
    JsonArray v2arr = makeArray("v2");

    Ret ret23 = reg.migrate(2, 3, v2arr);

    // [THEN] the default v2 -> v3 is a pass-through
    EXPECT_TRUE(ret23);
    EXPECT_EQ(firstTag(v2arr), "v2");
}
