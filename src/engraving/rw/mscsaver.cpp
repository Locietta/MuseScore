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
#include "mscsaver.h"

#include "global/io/buffer.h"

#include "dom/masterscore.h"
#include "dom/excerpt.h"
#include "dom/imageStore.h"

#include "engraving/automation/internal/automationrw.h"

#include "rwregister.h"
#include "inoutdata.h"

#include "log.h"

using namespace mu;
using namespace muse;
using namespace muse::io;
using namespace mu::engraving;
using namespace mu::engraving::rw;

bool MscSaver::writeMscz(MasterScore* score, MscWriter& mscWriter,
                        const write::WriteContext* ctx)
{
    TRACEFUNC;

    IF_ASSERT_FAILED(mscWriter.isOpened()) {
        return false;
    }

    // Write style of MasterScore
    {
        //! NOTE The style is writing to a separate file only for the master score.
        //! At the moment, the style for the parts is still writing to the score file.
        ByteArray styleData;
        auto styleBuf = Buffer::opened(IODevice::WriteOnly, &styleData);
        score->style().write(&styleBuf);
        mscWriter.writeStyleFile(styleData);
    }

    WriteInOutData masterWriteOutData(score);

    if (ctx) {
        masterWriteOutData.ctx = *ctx;
    }

    // Write MasterScore
    {
        ByteArray scoreData;
        auto scoreBuf = Buffer::opened(IODevice::ReadWrite, &scoreData);

        RWRegister::writer()->writeScore(score, &scoreBuf, &masterWriteOutData);

        mscWriter.writeScoreFile(scoreData);
    }

    // Write Excerpts
    {
        if (!ctx || !ctx->shouldWriteRange()) {
            const std::vector<Excerpt*>& excerpts = score->excerpts();

            struct ExcerptData {
                String fileName;
                ByteArray styleData;
                ByteArray scoreData;
            };

            auto serializeExcerpt = [masterWriteOutData, score](Excerpt* excerpt, size_t excerptIndex) -> ExcerptData {
                Score* partScore = excerpt->excerptScore();
                IF_ASSERT_FAILED(partScore && partScore != score) {
                    return ExcerptData();
                }

                excerpt->updateFileName(excerptIndex);

                ExcerptData data;
                data.fileName = excerpt->fileName();

                auto styleBuf = Buffer::opened(IODevice::WriteOnly, &data.styleData);
                partScore->style().write(&styleBuf);

                WriteInOutData writeOutData = masterWriteOutData;
                auto scoreBuf = Buffer::opened(IODevice::ReadWrite, &data.scoreData);
                RWRegister::writer()->writeScore(partScore, &scoreBuf, &writeOutData);

                return data;
            };

            for (size_t excerptIndex = 0; excerptIndex < excerpts.size(); ++excerptIndex) {
                Excerpt* excerpt = excerpts.at(excerptIndex);

                ExcerptData data = serializeExcerpt(excerpt, excerptIndex);
                mscWriter.addExcerptStyleFile(data.fileName, data.styleData);
                mscWriter.addExcerptFile(data.fileName, data.scoreData);
            }
        }
    }

    // Write ChordList
    {
        ChordList* chordList = score->chordList();
        if (chordList->customChordList() && !chordList->empty()) {
            ByteArray chlData;
            auto chlBuf = Buffer::opened(IODevice::WriteOnly, &chlData);
            chordList->write(&chlBuf);
            mscWriter.writeChordListFile(chlData);
        }
    }

    // Write images
    {
        for (ImageStoreItem* ip : imageStore) {
            if (!ip->isUsed(score)) {
                continue;
            }
            ByteArray data = ip->buffer();
            mscWriter.addImageFile(String::fromStdString(ip->hashName()), data);
        }
    }

    // Write automation
    {
        if (score->automationData()) {
            ByteArray data = AutomationRW::write(*score->automationData(), false /*writeGenerated*/);
            if (!data.empty()) {
                mscWriter.writeAutomationJsonFile(data);
            }
        }
    }

    return true;
}
