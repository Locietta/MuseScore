#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "io/ifilesystem.h"

namespace music {
// Native, Qt-free IFileSystem. Virtual ":/..." paths resolve under resourceRoot and
// are read-only. The hidden attribute cannot be set portably, so setAttribute is a no-op.
class NativeFileSystem final : public muse::io::IFileSystem
{
public:
    explicit NativeFileSystem(std::filesystem::path resourceRoot);
    ~NativeFileSystem() override;

    muse::Ret exists(const muse::io::path_t& path) const override;
    muse::Ret remove(const muse::io::path_t& path, bool onlyIfEmpty = false) override;
    muse::Ret clear(const muse::io::path_t& path) override;
    muse::Ret copy(const muse::io::path_t& src, const muse::io::path_t& dst, bool replace = false) override;
    muse::Ret move(const muse::io::path_t& src, const muse::io::path_t& dst, bool replace = false) override;

    muse::Ret makePath(const muse::io::path_t& path) const override;
    muse::Ret makeLink(const muse::io::path_t& targetPath, const muse::io::path_t& linkPath) const override;

    muse::io::EntryType entryType(const muse::io::path_t& path) const override;

    muse::RetVal<uint64_t> fileSize(const muse::io::path_t& path) const override;
    muse::RetVal<uint64_t> availableSpace(const muse::io::path_t& path) const override;

    muse::RetVal<muse::io::paths_t> scanFiles(const muse::io::path_t& rootDir,
                                              const std::vector<std::string>& filters,
                                              muse::io::ScanMode mode
                                              = muse::io::ScanMode::FilesInCurrentDirAndSubdirs) const override;

    void setAttribute(const muse::io::path_t& path,
                      muse::io::IFileSystem::Attribute attribute) const override;
    bool setPermissionsAllowedForAll(const muse::io::path_t& path) const override;

    muse::RetVal<muse::ByteArray> readFile(const muse::io::path_t& filePath) const override;
    muse::Ret readFile(const muse::io::path_t& filePath, muse::ByteArray& data) const override;
    muse::Ret writeFile(const muse::io::path_t& filePath, const muse::ByteArray& data) override;

    muse::RetVal<muse::io::StreamId> openStream(const muse::io::path_t& filePath,
                                                muse::io::OpenMode mode) override;
    muse::Ret writeToStream(muse::io::StreamId fileId, const muse::ByteArray& data,
                            uint64_t offset = muse::io::STREAM_POS_CURRENT) override;
    muse::Ret closeStream(muse::io::StreamId fileId) override;

    muse::io::path_t canonicalFilePath(const muse::io::path_t& filePath) const override;
    muse::io::path_t absolutePath(const muse::io::path_t& filePath) const override;
    muse::io::path_t absoluteFilePath(const muse::io::path_t& filePath) const override;
    muse::io::path_t cleanPath(const muse::io::path_t& filePath) const override;
    bool isSymLink(const muse::io::path_t& filePath) const override;
    muse::io::path_t symLinkTarget(const muse::io::path_t& filePath) const override;
    muse::io::path_t temporaryDirectoryPath() const override;
    muse::DateTime birthTime(const muse::io::path_t& filePath) const override;
    muse::DateTime lastModified(const muse::io::path_t& filePath) const override;
    muse::Ret isWritable(const muse::io::path_t& filePath) const override;

private:
    struct Stream;
    struct Target {
        std::filesystem::path path;
        std::string virtualRel;
        bool valid = false;
        bool resource = false; // read-only: a ":/" path or resolves inside the resource root
        bool virtualPath = false;
    };

    Target resolve(const muse::io::path_t& path) const;
    bool isInsideResourceRoot(const std::filesystem::path& path) const;
    bool isProtectedPath(const std::filesystem::path& path) const;
    bool isResourceAncestor(const std::filesystem::path& path) const;

    std::filesystem::path m_resourceRoot;
    bool m_hasResourceRoot = false;
    mutable std::mutex m_streamsMutex;
    std::unordered_map<muse::io::StreamId, std::unique_ptr<Stream> > m_streams;
    muse::io::StreamId m_nextStreamId = 0;
};
}
