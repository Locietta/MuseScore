// SPDX-License-Identifier: GPL-3.0-only
// Native runtime-font adapter for the pinned MuseScore draw interfaces:
//   src/framework/draw/internal/ifontsdatabase.h
//   src/framework/draw/internal/ifontsengine.h
#include "runtime_assets.h"
#include "draw/internal/fontprovider.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <zlib.h>

#include "draw/internal/fontsdatabase.h"
#include "draw/internal/fontsengine.h"
#include "draw/internal/ifontsdatabase.h"
#include "draw/internal/ifontsengine.h"
#include "draw/types/fontstypes.h"
#include "global/io/path.h"

namespace music {
namespace {

namespace draw = muse::draw;

constexpr std::uintmax_t kMaxFontFileBytes = 64ull * 1024ull * 1024ull;
constexpr std::uintmax_t kMaxSoundFontFileBytes = 128ull * 1024ull * 1024ull;
constexpr std::size_t kMaxRetainedFonts = 32;
constexpr std::size_t kMaxRetainedSoundFonts = 32;
constexpr std::size_t kMaxRetainedBytes = 256ull * 1024ull * 1024ull;

std::string pathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::filesystem::path pathFromUtf8(const std::string& text)
{
#if defined(__cpp_lib_char8_t)
    std::u8string utf8;
    utf8.reserve(text.size());
    for (const char ch : text) {
        utf8.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path(utf8);
#else
    return std::filesystem::u8path(text);
#endif
}

class CacheStore
{
public:
    explicit CacheStore(std::filesystem::path root) : m_root(std::move(root)) {}

    void ensureDirectory()
    {
        if (m_owned) return;
        std::error_code error;
        const std::filesystem::path base = !m_root.empty()
            ? m_root
            : std::filesystem::temp_directory_path(error);
        if (error || base.empty()) {
            throw std::runtime_error("Failed to resolve the temporary directory for runtime fonts");
        }

        std::random_device random;
        for (int attempt = 0; attempt < 64; ++attempt) {
            static std::atomic<std::uint64_t> sequence{ 0 };
            const std::string name = "musescore-runtime-fonts-"
                                     + std::to_string(random()) + "-"
                                     + std::to_string(random()) + "-"
                                     + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            const std::filesystem::path candidate = base / name;

            std::error_code createError;
            if (!std::filesystem::create_directory(candidate, createError)) {
                if (createError) {
                    throw std::runtime_error("Failed to create runtime font cache directory '"
                                             + pathToUtf8(candidate) + "': " + createError.message());
                }
                continue; // Never adopt a pre-existing path; try fresh randomness.
            }

            std::error_code absoluteError;
            std::filesystem::path absolute = std::filesystem::absolute(candidate, absoluteError);
            if (absoluteError || absolute.empty() || !absolute.is_absolute()) {
                std::error_code cleanupError;
                std::filesystem::remove(candidate, cleanupError);
                throw std::runtime_error("Failed to resolve the runtime font cache directory path");
            }

            m_dir = std::move(absolute);
            m_owned = true;
            return;
        }

        throw std::runtime_error("Failed to create a unique runtime font cache directory");
    }

    ~CacheStore()
    {
        if (!m_owned || m_dir.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(m_dir, error);
    }

    void clear()
    {
        if (!m_owned) return;
        std::error_code error;
        std::filesystem::remove_all(m_dir, error);
        if (error) throw std::system_error(error, "Failed to remove the owned runtime resource cache");
        m_owned = false;
        m_dir.clear();
    }

    void retain() { ++m_leases; }
    void release() noexcept
    {
        if (--m_leases == 0 && m_shutdownPending) {
            try { clear(); }
            catch (const std::exception& error) {
                std::fprintf(stderr, "Runtime resource cleanup: %s\n", error.what());
            }
        }
    }
    void shutdown()
    {
        m_shutdownPending = true;
        if (m_leases == 0) clear();
    }

    CacheStore(const CacheStore&) = delete;
    CacheStore& operator=(const CacheStore&) = delete;
    CacheStore(CacheStore&&) = delete;
    CacheStore& operator=(CacheStore&&) = delete;

    const std::filesystem::path& dir() { ensureDirectory(); return m_dir; }

private:
    std::filesystem::path m_root;
    std::filesystem::path m_dir;
    bool m_owned = false;
    std::size_t m_leases = 0;
    bool m_shutdownPending = false;
};

class ResourceLease
{
public:
    explicit ResourceLease(std::shared_ptr<CacheStore> cache) : m_cache(std::move(cache)) { m_cache->retain(); }
    ~ResourceLease() { m_cache->release(); }
    ResourceLease(const ResourceLease&) = delete;
    ResourceLease& operator=(const ResourceLease&) = delete;
private:
    std::shared_ptr<CacheStore> m_cache;
};

class TempFileGuard
{
public:
    explicit TempFileGuard(std::filesystem::path path)
        : m_path(std::move(path))
    {
    }

    ~TempFileGuard()
    {
        if (m_committed || m_path.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;

    void commit() noexcept { m_committed = true; }

private:
    std::filesystem::path m_path;
    bool m_committed = false;
};

struct ParsedFont {
    std::string family;
    bool bold = false;
    bool italic = false;
};

struct FreeTypeLibraryGuard {
    FT_Library library = nullptr;
    ~FreeTypeLibraryGuard()
    {
        if (library) {
            FT_Done_FreeType(library);
        }
    }
};

struct FreeTypeFaceGuard {
    FT_Face face = nullptr;
    ~FreeTypeFaceGuard()
    {
        if (face) {
            FT_Done_Face(face);
        }
    }
};

ParsedFont validateFont(const std::vector<std::uint8_t>& bytes, const std::string& sourcePath)
{
    FT_Library library = nullptr;
    const FT_Error initError = FT_Init_FreeType(&library);
    FreeTypeLibraryGuard libraryGuard{ library };
    if (initError != 0) {
        throw std::runtime_error("Failed to initialize FreeType for font validation: " + sourcePath);
    }

    FT_Face face = nullptr;
    const FT_Error faceError = FT_New_Memory_Face(
        library, reinterpret_cast<const FT_Byte*>(bytes.data()),
        static_cast<FT_Long>(bytes.size()), 0, &face);
    FreeTypeFaceGuard faceGuard{ face };
    if (faceError != 0 || !face) {
        throw std::runtime_error("FreeType could not parse the font: " + sourcePath);
    }

    if (face->num_faces != 1) {
        throw std::runtime_error("Multi-face font files are not supported: " + sourcePath);
    }
    if (!FT_IS_SCALABLE(face)) {
        throw std::runtime_error("Font is not scalable: " + sourcePath);
    }
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        throw std::runtime_error("Font has no Unicode character map: " + sourcePath);
    }
    if (!face->family_name || face->family_name[0] == '\0') {
        throw std::runtime_error("Font has no family name: " + sourcePath);
    }

    ParsedFont parsed;
    parsed.family = face->family_name;
    parsed.bold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    parsed.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    return parsed;
}

std::vector<std::uint8_t> readResourceFile(const std::string& sourcePath,
                                         std::uintmax_t limit, const std::string& kind)
{
    if (sourcePath.empty() || sourcePath.find('\0') != std::string::npos) {
        throw std::runtime_error(kind + " path is empty or contains NUL");
    }

    const std::filesystem::path path = pathFromUtf8(sourcePath);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        throw std::runtime_error(kind + " path is not a regular file: " + sourcePath);
    }

    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error("Failed to read " + kind + " file size: " + sourcePath);
    }
    if (size == 0 || size > limit) {
        throw std::runtime_error(kind + " file is empty or exceeds " + std::to_string(limit) + " bytes: " + sourcePath);
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open " + kind + " file: " + sourcePath);
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()) || input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Failed to read the complete " + kind + " file: " + sourcePath);
    }
    return bytes;
}

// SoundFont 2/3 files are RIFF containers with form type "sfbk". The host must
// not initialise the audio engine just to validate runtime_set_soundfont, so
// this is a structural check only; the worker's engine parses the font fully.
void validateSoundFontHeader(const std::vector<std::uint8_t>& bytes, const std::string& sourcePath)
{
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "sfbk", 4) != 0) {
        throw std::runtime_error("Invalid SoundFont file: " + sourcePath);
    }
}

struct FontRecord {
    RuntimeFontInfo info;
    draw::FontDataKey key;
    std::shared_ptr<const muse::ByteArray> bytes;
};

using ActiveTable = std::vector<FontRecord>;

const FontRecord* findRecord(const ActiveTable& table, const draw::FontDataKey& key)
{
    for (const FontRecord& record : table) {
        // FontDataKey equality: case-insensitive family, exact bold/italic.
        if (record.key == key) {
            return &record;
        }
    }
    return nullptr;
}

const FontRecord* findRegularFamilyFallback(const ActiveTable& table, const draw::FontDataKey& key)
{
    // Families such as Ubuntu ship a Medium face that FreeType flags as bold,
    // so a regular query has no exact record. Upstream/desktop resolves by
    // family and the closest available style; choose an upright face of the
    // requested family when the exact regular style is absent. Bold/italic
    // queries keep exact-key semantics (no synthetic styling is invented).
    if (key.bold() || key.italic()) {
        return nullptr;
    }
    const FontRecord* italic = nullptr;
    for (const FontRecord& record : table) {
        if (record.key.family() != key.family()) {
            continue;
        }
        if (!record.key.italic()) {
            return &record;
        }
        if (!italic) {
            italic = &record;
        }
    }
    return italic;
}

struct SoundFontRecord {
    RuntimeSoundFontInfo info;
    std::size_t size = 0;
};

std::string contentFingerprint(const std::vector<std::uint8_t>& bytes)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uInt>(bytes.size()));
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "crc32:%08x:%zu", static_cast<unsigned int>(crc), bytes.size());
    return buffer;
}

// The owner thread and the installed/shut-down state, shared by the asset store
// and the fonts-engine proxy it hands to the draw layer.
struct OwnerState {
    std::thread::id owner = std::this_thread::get_id();
    bool installed = true;

    void check() const
    {
        if (!installed) {
            throw std::runtime_error("Runtime font services are not installed");
        }
        if (std::this_thread::get_id() != owner) {
            throw std::runtime_error("Runtime font services called from a non-owner thread");
        }
    }
};

std::unique_ptr<draw::FontsEngine> createEngine(draw::IFontsDatabase& database)
{
    auto engine = std::make_unique<draw::FontsEngine>(database);
    engine->init();
    return engine;
}

class RuntimeFontsDatabase final : public draw::FontsDatabase
{
public:
    RuntimeFontsDatabase(std::shared_ptr<CacheStore> cacheStore,
                         std::shared_ptr<const ActiveTable> active)
        : m_cacheStore(std::move(cacheStore)),
          m_active(std::move(active))
    {
    }

    ~RuntimeFontsDatabase() override = default;

    void publish(std::shared_ptr<const ActiveTable> active) noexcept
    {
        m_active.swap(active);
    }

    std::shared_ptr<const ActiveTable> activeTable() const { return m_active; }

    const std::shared_ptr<CacheStore>& cacheStore() const { return m_cacheStore; }

    draw::FontDataKey actualFont(const draw::FontDataKey& requireKey,
                                 draw::Font::Type type) const override
    {
        if (const FontRecord* exact = findRecord(*m_active, requireKey)) {
            return exact->key;
        }

        const draw::FontDataKey baseKey = draw::FontsDatabase::actualFont(requireKey, type);
        if (const FontRecord* overridden = findRecord(*m_active, baseKey)) {
            return overridden->key;
        }

        // Built-in families keep priority; only a family the upstream database
        // cannot provide at all falls back to its registered runtime face.
        // FontsEngine normalises Undefined/Unknown to Text before querying, so
        // this fallback must not apply to symbol/music faces.
        if (type == draw::Font::Type::Text && baseKey.family() != requireKey.family()) {
            if (const FontRecord* fallback = findRegularFamilyFallback(*m_active, requireKey)) {
                return fallback->key;
            }
        }

        // Only the upstream database's own default fallback is accepted here
        // when no registered face can represent the requested family.
        return baseKey;
    }

    std::vector<draw::FontDataKey> substitutionFonts(const draw::FontDataKey& requireKey) const override
    {
        std::vector<draw::FontDataKey> result = draw::FontsDatabase::substitutionFonts(requireKey);
        // 5.0 keys the substitution list by family instead of by font type, so
        // the 4.7.6 rule "every registered runtime face is a text fallback" has
        // no type to gate on. Keep the registered faces as extra glyph-fallback
        // candidates: the primary face is always tried first, so this only
        // changes codepoints it cannot render (for example CJK text in a score
        // that names a Latin-only family).
        result.reserve(result.size() + m_active->size());
        for (const FontRecord& record : *m_active) {
            if (std::find(result.begin(), result.end(), record.key) == result.end()) {
                result.push_back(record.key);
            }
        }
        return result;
    }

    draw::FontData fontData(const draw::FontDataKey& requireKey,
                            draw::Font::Type type) const override
    {
        const draw::FontDataKey actualKey = actualFont(requireKey, type);
        if (const FontRecord* record = findRecord(*m_active, actualKey)) {
            draw::FontData data;
            data.key = record->key;
            data.data = *record->bytes;
            return data;
        }
        return draw::FontsDatabase::fontData(requireKey, type);
    }

private:
    std::shared_ptr<CacheStore> m_cacheStore;
    std::shared_ptr<const ActiveTable> m_active;
};

class FontsEngineProxy final : public draw::IFontsEngine
{
public:
    FontsEngineProxy(const OwnerState& owner, std::shared_ptr<CacheStore> cacheStore,
                     std::unique_ptr<draw::FontsEngine> engine)
        : m_owner(owner),
          m_cacheStore(std::move(cacheStore)),
          m_engine(std::move(engine))
    {
    }

    ~FontsEngineProxy() override
    {
        m_engine.reset();
        m_cacheStore.reset();
    }

    // The caller has already checked ownership and prepared the new engine.
    // Replacing the engine drops every upstream cached face, so replacements
    // and reactivations cannot keep using the old face.
    void setEngine(std::unique_ptr<draw::FontsEngine> engine) noexcept
    {
        m_engine = std::move(engine);
    }

    // The render cache is optional (FontRenderCache is a no-op without a
    // directory); forward it so a caller can opt in.
    void setRenderCacheDirPath(const muse::io::path_t& path, const std::string& revision = std::string()) override
    {
        m_owner.check();
        if (!m_engine) {
            throw std::runtime_error("Runtime font engine is not initialized");
        }
        m_engine->setRenderCacheDirPath(path, revision);
    }

    void clearLoadedFaces() override
    {
        m_owner.check();
        if (m_engine) {
            m_engine->clearLoadedFaces();
        }
    }

    double lineSpacing(const draw::Font& f) const override
    {
        return engine()->lineSpacing(f);
    }

    double xHeight(const draw::Font& f) const override
    {
        return engine()->xHeight(f);
    }

    double height(const draw::Font& f) const override
    {
        return engine()->height(f);
    }

    double capHeight(const draw::Font& f) const override
    {
        return engine()->capHeight(f);
    }

    double ascent(const draw::Font& f) const override
    {
        return engine()->ascent(f);
    }

    double descent(const draw::Font& f) const override
    {
        return engine()->descent(f);
    }

    double underlinePos(const draw::Font& f) const override
    {
        return engine()->underlinePos(f);
    }

    double lineWidth(const draw::Font& f) const override
    {
        return engine()->lineWidth(f);
    }

    double strikeOutPos(const draw::Font& f) const override
    {
        return engine()->strikeOutPos(f);
    }

    bool inFont(const draw::Font& f, char32_t ucs4) const override
    {
        return engine()->inFont(f, ucs4);
    }

    double horizontalAdvance(const draw::Font& f, const char32_t& ch) const override
    {
        return engine()->horizontalAdvance(f, ch);
    }

    double horizontalAdvance(const draw::Font& f, const std::u32string& text) const override
    {
        return engine()->horizontalAdvance(f, text);
    }

    muse::RectF boundingRect(const draw::Font& f, const char32_t& ch) const override
    {
        return engine()->boundingRect(f, ch);
    }

    muse::RectF boundingRect(const draw::Font& f, const std::u32string& text) const override
    {
        return engine()->boundingRect(f, text);
    }

    muse::RectF tightBoundingRect(const draw::Font& f, const std::u32string& text) const override
    {
        return engine()->tightBoundingRect(f, text);
    }

    std::vector<draw::GlyphImage> render(const draw::Font& f, const std::u32string& text) const override
    {
        return engine()->render(f, text);
    }

private:
    const draw::FontsEngine* engine() const
    {
        m_owner.check();
        if (!m_engine) {
            throw std::runtime_error("Runtime font engine is not initialized");
        }
        return m_engine.get();
    }

    const OwnerState& m_owner;
    std::shared_ptr<CacheStore> m_cacheStore;
    std::unique_ptr<draw::FontsEngine> m_engine;
};

} // namespace

struct RuntimeAssets::Impl {
    OwnerState owner;
    std::shared_ptr<CacheStore> cacheStore;
    std::unique_ptr<RuntimeFontsDatabase> database;
    std::unique_ptr<FontsEngineProxy> proxyEngine;
    std::unique_ptr<draw::FontProvider> provider;
    int nextFontId = 1;
    std::vector<FontRecord> retainedFonts;
    std::vector<SoundFontRecord> retainedSoundFonts;

    std::size_t retainedResourceBytes() const
    {
        std::size_t result = 0;
        for (const auto& font : retainedFonts) result += font.bytes ? font.bytes->size() : 0;
        for (const auto& font : retainedSoundFonts) result += font.size;
        return result;
    }

    RuntimeFontInfo reactivateRetained(const FontRecord& retained);
};

RuntimeFontInfo RuntimeAssets::Impl::reactivateRetained(const FontRecord& retained)
{
    RuntimeFontInfo result = retained.info;

    const std::shared_ptr<const ActiveTable> current = database->activeTable();
    std::size_t index = current->size();
    bool alreadyActive = false;
    for (std::size_t i = 0; i < current->size(); ++i) {
        if ((*current)[i].key == retained.key) {
            index = i;
            alreadyActive = (*current)[i].info.id == retained.info.id;
            break;
        }
    }

    if (alreadyActive) {
        return result;
    }

    auto next = std::make_shared<ActiveTable>(*current);
    if (index < next->size()) {
        (*next)[index] = retained;
    } else {
        next->push_back(retained);
    }

    auto newEngine = createEngine(*database);

    std::shared_ptr<const ActiveTable> published = std::move(next);
    database->publish(published);
    proxyEngine->setEngine(std::move(newEngine));
    return result;
}


RuntimeAssets::RuntimeAssets(const std::filesystem::path& cacheRoot)
    : m_impl(std::make_unique<Impl>())
{
    if (!cacheRoot.empty()) {
        std::error_code error;
        std::filesystem::create_directories(cacheRoot, error);
    }
    m_impl->cacheStore = std::make_shared<CacheStore>(cacheRoot);
    m_impl->database = std::make_unique<RuntimeFontsDatabase>(m_impl->cacheStore, std::make_shared<ActiveTable>());
    m_impl->proxyEngine = std::make_unique<FontsEngineProxy>(m_impl->owner, m_impl->cacheStore,
                                                             createEngine(*m_impl->database));
    m_impl->provider = std::make_unique<draw::FontProvider>(*m_impl->database, *m_impl->proxyEngine);
}

RuntimeAssets::~RuntimeAssets()
{
    if (!m_impl->owner.installed) return;
    try {
        shutdown();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Runtime resource cleanup: %s\n", error.what());
    }
}

draw::IFontsDatabase& RuntimeAssets::fontsDatabase() { return *m_impl->database; }
draw::IFontsEngine& RuntimeAssets::fontsEngine() { return *m_impl->proxyEngine; }
draw::IFontProvider& RuntimeAssets::fontProvider() { return *m_impl->provider; }

void RuntimeAssets::shutdown()
{
    if (!m_impl->owner.installed) return;
    m_impl->owner.check();
    // Release the face cache and retained files explicitly; leases keep the
    // cache directory until the last one is released.
    m_impl->proxyEngine->setEngine(nullptr);
    m_impl->database->cacheStore()->shutdown();
    m_impl->retainedFonts.clear();
    m_impl->retainedSoundFonts.clear();
    m_impl->owner.installed = false;
}

std::shared_ptr<const void> RuntimeAssets::retain()
{
    m_impl->owner.check();
    return std::make_shared<ResourceLease>(m_impl->database->cacheStore());
}
RuntimeFontInfo RuntimeAssets::addFont(const std::string& source_path)
{
    Impl& d = *m_impl;
    d.owner.check();

    std::vector<std::uint8_t> bytes = readResourceFile(source_path, kMaxFontFileBytes, "Font");

    for (const FontRecord& retained : d.retainedFonts) {
        if (retained.bytes && retained.bytes->size() == bytes.size()
            && std::memcmp(retained.bytes->constData(), bytes.data(), bytes.size()) == 0) {
            return d.reactivateRetained(retained);
        }
    }

    const ParsedFont parsed = validateFont(bytes, source_path);

    const std::size_t retainedBytes = d.retainedResourceBytes();
    if (d.retainedFonts.size() >= kMaxRetainedFonts) {
        throw std::runtime_error("Maximum number of retained runtime fonts (32) exceeded");
    }
    if (bytes.size() > kMaxRetainedBytes || retainedBytes > kMaxRetainedBytes - bytes.size()) {
        throw std::runtime_error("Retained runtime resource bytes exceed the 256 MiB limit");
    }

    const muse::draw::FontDataKey key(
        muse::draw::Font::FontFamily(parsed.family), parsed.bold, parsed.italic);

    // Non-cryptographic content identity only; this is not a security hash.
    const std::string contentHash = contentFingerprint(bytes);

    const int fontId = d.nextFontId;
    static std::atomic<std::uint64_t> fileSequence{ 0 };
    const std::string fileName = "font_" + std::to_string(fontId) + "_"
                                 + std::to_string(fileSequence.fetch_add(1, std::memory_order_relaxed))
                                 + ".ttf";
    const std::filesystem::path destination = d.database->cacheStore()->dir() / fileName;
    TempFileGuard cacheFileGuard(destination);

    {
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Failed to create cached runtime font: "
                                     + pathToUtf8(destination));
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("Failed to write cached runtime font: "
                                     + pathToUtf8(destination));
        }
    }

    std::error_code absoluteError;
    const std::filesystem::path absoluteDestination = std::filesystem::absolute(destination, absoluteError);
    if (absoluteError || absoluteDestination.empty() || !absoluteDestination.is_absolute()) {
        throw std::runtime_error("Failed to resolve the cached runtime font path");
    }
    const std::string retainedPath = pathToUtf8(absoluteDestination);

    std::shared_ptr<const muse::ByteArray> sharedBytes =
        std::make_shared<muse::ByteArray>(bytes.data(), bytes.size());

    FontRecord record;
    record.info.id = fontId;
    record.info.family = parsed.family;
    record.info.bold = parsed.bold;
    record.info.italic = parsed.italic;
    record.info.path = retainedPath;
    record.info.content_hash = contentHash;
    record.key = key;
    record.bytes = std::move(sharedBytes);

    const std::shared_ptr<const ActiveTable> current = d.database->activeTable();
    auto nextActive = std::make_shared<ActiveTable>(*current);
    bool replaced = false;
    for (FontRecord& existing : *nextActive) {
        if (existing.key == record.key) {
            existing = record;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        nextActive->push_back(record);
    }

    std::vector<FontRecord> nextRetained = d.retainedFonts;
    nextRetained.push_back(record);

    RuntimeFontInfo result = record.info;
    auto newEngine = createEngine(*d.database);

    std::shared_ptr<const ActiveTable> published = std::move(nextActive);
    d.database->publish(published);
    d.retainedFonts.swap(nextRetained);
    d.proxyEngine->setEngine(std::move(newEngine));
    d.nextFontId = fontId + 1;
    cacheFileGuard.commit();
    return result;
}

std::vector<RuntimeFontInfo> RuntimeAssets::fonts() const
{
    const Impl& d = *m_impl;
    d.owner.check();

    const std::shared_ptr<const ActiveTable> active = d.database->activeTable();
    std::vector<RuntimeFontInfo> result;
    result.reserve(active->size());
    for (const FontRecord& record : *active) {
        result.push_back(record.info);
    }
    return result;
}

RuntimeSoundFontInfo RuntimeAssets::freezeSoundFont(const std::string& source_path)
{
    Impl& d = *m_impl;
    d.owner.check();
    const auto bytes = readResourceFile(source_path, kMaxSoundFontFileBytes, "SoundFont");
    validateSoundFontHeader(bytes, source_path);
    const auto fingerprint = contentFingerprint(bytes);
    for (const auto& retained : d.retainedSoundFonts) {
        if (retained.size == bytes.size() && retained.info.content_hash == fingerprint
            && readResourceFile(retained.info.path, kMaxSoundFontFileBytes, "Cached SoundFont") == bytes) {
            return retained.info; // Exact byte comparison; the fingerprint is only a prefilter.
        }
    }
    if (d.retainedSoundFonts.size() >= kMaxRetainedSoundFonts) {
        throw std::runtime_error("Maximum number of retained SoundFonts (32) exceeded");
    }
    if (d.retainedResourceBytes() > kMaxRetainedBytes - bytes.size()) {
        throw std::runtime_error("Retained runtime resource bytes exceed the 256 MiB limit");
    }
    const auto destination = d.database->cacheStore()->dir()
        / ("soundfont_" + std::to_string(d.retainedSoundFonts.size()) + ".sf2");
    TempFileGuard guard(destination);
    {
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) throw std::runtime_error("Failed to write retained SoundFont");
    }
    RuntimeSoundFontInfo result{pathToUtf8(destination), fingerprint};
    auto next = d.retainedSoundFonts;
    next.push_back({result, bytes.size()});
    d.retainedSoundFonts.swap(next);
    guard.commit();
    return result;
}

} // namespace music
