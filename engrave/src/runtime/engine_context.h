#pragma once

#include <memory>
#include <string>

namespace muse::io {
class IFileSystem;
}
namespace muse::draw {
class IFontsDatabase;
}

namespace music {
class RuntimeAssets;

// The engine's services, owned in a fixed initialisation order and destroyed in
// the reverse order (see notes/cleanup/c5/DESIGN.md in the superproject). One
// context per process, created and destroyed on the owner thread; every
// document of the engine must be destroyed first.
class EngineContext
{
public:
    // assets is the UTF-8 path of the resource directory; cache_root the UTF-8
    // parent of the runtime cache directory (empty: the system temporary
    // directory). Throws std::invalid_argument when the resources are missing,
    // before any process state changes; std::runtime_error for a second context
    // in the process or a failed initialisation, which cannot be retried.
    explicit EngineContext(const std::string& assets, const std::string& cache_root = {});
    ~EngineContext();

    EngineContext(const EngineContext&) = delete;
    EngineContext& operator=(const EngineContext&) = delete;

    muse::io::IFileSystem& fileSystem() const;
    RuntimeAssets& assets() const;
    muse::draw::IFontsDatabase& fontsDatabase() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
