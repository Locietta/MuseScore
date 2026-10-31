// SPDX-License-Identifier: GPL-3.0-only
// Native adapter for the pinned MuseScore draw interfaces:
//   muse/framework/draw/internal/ifontsdatabase.h
//   muse/framework/draw/internal/ifontsengine.h
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace muse::draw {
class IFontsDatabase;
class IFontsEngine;
class IFontProvider;
}

namespace music {

struct RuntimeFontInfo {
    int id = 0;
    std::string family;
    bool bold = false;
    bool italic = false;
    std::string path;
    // crc32:<8 lowercase hex>:<byte size>; identity only, not a security hash.
    std::string content_hash;
};

struct RuntimeSoundFontInfo {
    std::string path;
    // Same non-cryptographic identity format as RuntimeFontInfo.
    std::string content_hash;
};

// The engine's font database/engine/provider and its retained runtime fonts and
// SoundFonts. EngineContext owns the only instance. Construction binds it to the
// calling thread; every later call, including the draw-engine calls, must come
// from that owner thread.
class RuntimeAssets
{
public:
    // cacheRoot is the parent directory of the runtime cache directory; empty
    // selects the system temporary directory.
    explicit RuntimeAssets(const std::filesystem::path& cacheRoot = {});
    ~RuntimeAssets();

    RuntimeAssets(const RuntimeAssets&) = delete;
    RuntimeAssets& operator=(const RuntimeAssets&) = delete;

    muse::draw::IFontsDatabase& fontsDatabase();
    muse::draw::IFontsEngine& fontsEngine();
    muse::draw::IFontProvider& fontProvider();

    // Owner-thread final shutdown, after scores and background jobs have been
    // destroyed. Releases cached faces; retained files survive outstanding leases.
    // The destructor shuts down when this was not called.
    void shutdown();

    // Keeps immutable files alive for an audio session that outlives the core.
    // Acquire and release on the owner thread; the last lease performs deferred
    // cleanup after shutdown. The token contains no engraving pointers.
    std::shared_ptr<const void> retain();

    // Adds a font file and selects it for its exact family/bold/italic key.
    // A family whose only faces are not regular (e.g. a Medium face that FreeType
    // flags bold) also satisfies regular lookups when upstream has no such family.
    // The score layer owns relayout.
    RuntimeFontInfo addFont(const std::string& source_path);

    // Returns an immutable snapshot of the currently selected fonts.
    std::vector<RuntimeFontInfo> fonts() const;

    // Copies and validates a SoundFont. The immutable retained path remains valid
    // until shutdown and the last lease. Owner-thread only.
    RuntimeSoundFontInfo freezeSoundFont(const std::string& source_path);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace music
