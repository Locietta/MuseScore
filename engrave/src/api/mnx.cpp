// SPDX-License-Identifier: GPL-3.0-only
#include "mnx.h"
#include "importexport/mnx/internal/import/mnximporter.h"
#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/mscore.h"
#include "engraving/infrastructure/localfileinfoprovider.h"
#include "engrave/src/runtime/engine_context.h"
namespace engrave {
music::ScoreProject open_mnx(music::EngineContext& context, const std::string& bytes) {
    auto doc = mnx::Document::create(bytes.data(), bytes.size());
    if (!mnx::validation::schemaValidate(doc) || doc.global().measures().empty() || doc.parts().empty())
        throw std::invalid_argument("Invalid MNX schema or empty score");
    mu::engraving::ScoreLoad loading;
    auto owned = mu::engraving::EngravingProject::create(context.engraving());
    owned->setFileInfoProvider(std::make_shared<mu::engraving::LocalFileInfoProvider>("Untitled.mnx"));
    mu::iex::mnxio::MnxImporter importer(owned->masterScore(), std::move(doc));
    importer.importMnx();
    auto& project = *owned;

    auto* score = project.masterScore();
    score->connectTies();
    // Match the runtime post-load MIDI wiring before layout or playback.
    for (auto* part : score->parts()) part->updateHarmonyChannels(false);
    score->rebuildMidiMapping();
    score->updateChannel();
    const auto result = project.setupMasterScore(true);
    if (!result) throw std::runtime_error(result.toString());
    // Keep imported programs and banks stable across native save/reopen.
    for (auto* part : score->parts()) {
        for (const auto& pair : part->instruments()) {
            for (auto* channel : pair.second->channel()) {
                if (auto* playback = score->playbackChannel(channel)) {
                    playback->setProgram(channel->program());
                    playback->setBank(channel->bank());
                }
            }
        }
    }
    score->setLayoutAll();
    score->update();
    return owned;
}
}
