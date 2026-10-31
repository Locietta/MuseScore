// SPDX-License-Identifier: GPL-3.0-only
// Native adapter for the pinned MuseScore draw interfaces:
//   muse/framework/draw/internal/ifontsdatabase.h
//   muse/framework/draw/internal/ifontsengine.h
#pragma once

#include <string>
#include <memory>
#include <vector>

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

// Installs the process-wide draw font database/engine on the calling thread.
// Later install/add/list and draw-engine calls must come from that owner thread.
void install_runtime_font_services();

// Sets a custom parent directory where the runtime cache directory will be created.
// Must be called before font/soundfont caches are created. Empty resets to default.
void set_runtime_cache_root(const std::string& path);

// Owner-thread final shutdown, after scores and background jobs have been
// destroyed. Releases cached faces; retained files survive outstanding leases.
// Upstream references may keep the registered service objects alive afterwards.
void shutdown_runtime_assets();

// Keeps immutable files alive for an audio session that outlives the core.
// Acquire and release on the runtime owner thread; the last lease performs
// deferred cleanup after shutdown. The token contains no engraving pointers.
std::shared_ptr<const void> retain_runtime_assets();

// Adds a font file and selects it for its exact family/bold/italic key.
// A family whose only faces are not regular (e.g. a Medium face that FreeType
// flags bold) also satisfies regular lookups when upstream has no such family.
// The parent/score layer owns relayout; background jobs must receive immutable
// RuntimeFontInfo copies rather than using this adapter directly.
RuntimeFontInfo add_runtime_font(const std::string& source_path);

// Returns an immutable snapshot of the currently selected fonts.
std::vector<RuntimeFontInfo> runtime_fonts();

struct RuntimeSoundFontInfo {
    std::string path;
    // Same non-cryptographic identity format as RuntimeFontInfo.
    std::string content_hash;
};

// Copies and validates through the actual synthesizer before publication.
// The immutable retained path remains valid until process shutdown; stop all
// background jobs before tearing down runtime services. Owner-thread only.
RuntimeSoundFontInfo freeze_runtime_soundfont(const std::string& source_path);

} // namespace music
