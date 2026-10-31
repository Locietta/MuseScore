#include "noqt_filesystem.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "io/ioretcodes.h"

using namespace muse;
using namespace muse::io;
using music::NativeFileSystem;
namespace fs = std::filesystem;

namespace {
constexpr uint64_t kMaxReadFileSize = 256ull * 1024ull * 1024ull;

std::string pathToUtf8(const fs::path& path, bool generic = true)
{
    const std::u8string utf8 = generic ? path.generic_u8string() : path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

bool hasEmbeddedNul(const std::string& value)
{
    return value.find('\0') != std::string::npos;
}

Ret failure(io::Err code, std::string text)
{
    return Ret(static_cast<int>(code), std::move(text));
}

Ret fsFailure(io::Err code, const char* operation, const fs::path& path, const std::error_code& ec)
{
    std::string text = std::string(operation) + ": " + pathToUtf8(path);
    if (ec) {
        text += ": ";
        text += ec.message();
    }
    return failure(code, std::move(text));
}

Ret readOnlyFailure(io::Err code, const char* operation, const io::path_t& path)
{
    return failure(code, std::string(operation) + ": protected by the read-only resource root: "
                   + path.toStdString());
}

char asciiLower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool sameComponent(const std::string& lhs, const std::string& rhs)
{
#ifdef _WIN32
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (asciiLower(lhs[i]) != asciiLower(rhs[i])) {
            return false;
        }
    }
    return true;
#else
    return lhs == rhs;
#endif
}

bool isPathPrefix(const fs::path& prefix, const fs::path& path)
{
    auto it = path.begin();
    for (const fs::path& component : prefix) {
        if (it == path.end() || !sameComponent(pathToUtf8(component), pathToUtf8(*it))) {
            return false;
        }
        ++it;
    }
    return true;
}

bool pathsEqual(const fs::path& lhs, const fs::path& rhs)
{
    return isPathPrefix(lhs, rhs) && isPathPrefix(rhs, lhs);
}

bool isStrictPathPrefix(const fs::path& prefix, const fs::path& path)
{
    return isPathPrefix(prefix, path) && !pathsEqual(prefix, path);
}

fs::path absoluteNormalized(const fs::path& path)
{
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    return absolute.lexically_normal();
}

io::path_t appendName(const io::path_t& directory, const std::string& name)
{
    std::string path = directory.toStdString();
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) {
        path.pop_back();
    }
    if (path.empty()) {
        path = ".";
    }
    if (path.back() != '/') {
        path.push_back('/');
    }
    path += name;
    return io::path_t(std::move(path));
}

// QDir::cleanPath equivalent: '/'-separated, redundant separators and '.' dropped,
// ".." resolved lexically (never above the root, drive or UNC share), symlinks
// untouched, no trailing separator. Resource paths keep their ":/" form.
std::string cleanPathString(std::string path)
{
#ifdef _WIN32
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
#endif
    // Resource marker (":/x") or drive ("C:/x") prefix, kept verbatim.
    std::string drive;
    if (!path.empty() && path[0] == ':') {
        drive = ":";
        path.erase(0, 1);
    } else if (path.size() >= 2 && path[1] == ':') {
        drive = path.substr(0, 2);
        path.erase(0, 2);
    }

    std::size_t index = 0;
    std::size_t leadingSlashes = 0;
    while (index < path.size() && path[index] == '/') {
        ++index;
        ++leadingSlashes;
    }
    const bool absolute = leadingSlashes > 0;
    // A UNC root ("//host/share") keeps two slashes and cannot be cancelled by "..".
    const bool unc = absolute && leadingSlashes > 1 && drive.empty();

    std::vector<std::string> components;
    for (std::size_t split = index; split <= path.size();) {
        const std::size_t slash = path.find('/', split);
        const std::string component = path.substr(split, slash == std::string::npos ? std::string::npos : slash - split);
        if (component.empty() || component == ".") {
            // Redundant separator or current directory.
        } else if (component == "..") {
            const std::size_t protectedCount = unc ? 2 : 0;
            if (components.size() > protectedCount && components.back() != "..") {
                components.pop_back();
            } else if (!absolute) {
                // A relative path cannot be collapsed further; keep the "..".
                components.push_back(component);
            }
            // Above an absolute root there is nothing left to drop.
        } else {
            components.push_back(component);
        }
        if (slash == std::string::npos) {
            break;
        }
        split = slash + 1;
    }

    std::string result = drive;
    result += unc ? "//" : (absolute ? "/" : "");
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (!result.empty() && result.back() != '/' && (absolute || i > 0)) {
            result.push_back('/');
        }
        result += components[i];
    }
    return result;
}

bool unsafeVirtualComponent(const fs::path& component)
{
    std::string name = pathToUtf8(component);
    if (name.empty()) {
        return false; // separator or trailing slash
    }
#ifdef _WIN32
    // Win32 strips trailing spaces/dots before resolving a component, so ".. " is also "..".
    const size_t last = name.find_last_not_of(" .");
    if (last == std::string::npos) {
        return true;
    }
    name.resize(last + 1);
#endif
    return name == "..";
}

bool globCharEqual(char lhs, char rhs)
{
#ifdef _WIN32
    return asciiLower(lhs) == asciiLower(rhs);
#else
    return lhs == rhs;
#endif
}

bool globMatch(std::string_view name, std::string_view pattern)
{
    size_t nameIndex = 0;
    size_t patternIndex = 0;
    size_t starIndex = std::string_view::npos;
    size_t starNameIndex = 0;

    while (nameIndex < name.size()) {
        if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
            starIndex = patternIndex++;
            starNameIndex = nameIndex;
        } else if (patternIndex < pattern.size()
                   && (pattern[patternIndex] == '?'
                       || globCharEqual(pattern[patternIndex], name[nameIndex]))) {
            ++patternIndex;
            ++nameIndex;
        } else if (starIndex != std::string_view::npos) {
            patternIndex = starIndex + 1;
            nameIndex = ++starNameIndex;
        } else {
            return false;
        }
    }

    while (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
        ++patternIndex;
    }
    return patternIndex == pattern.size();
}

bool matchesAnyFilter(const std::string& name, const std::vector<std::string>& filters)
{
    if (filters.empty()) {
        return true;
    }
    for (const std::string& filter : filters) {
        if (globMatch(name, filter)) {
            return true;
        }
    }
    return false;
}

DateTime dateTimeFromFileTime(const fs::file_time_type& fileTime)
{
    using namespace std::chrono;
    const auto systemTime = time_point_cast<system_clock::duration>(
        fileTime - fs::file_time_type::clock::now() + system_clock::now());
    const std::time_t seconds = system_clock::to_time_t(systemTime);

    std::tm tm = {};
#ifdef _WIN32
    if (localtime_s(&tm, &seconds) != 0) {
        return {};
    }
#else
    if (localtime_r(&seconds, &tm) == nullptr) {
        return {};
    }
#endif
    return DateTime(Date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday),
                    Time(tm.tm_hour, tm.tm_min, tm.tm_sec));
}

Ret copyEntry(const fs::path& source, const fs::path& destination)
{
    std::error_code ec;
    const fs::file_status sourceStatus = fs::symlink_status(source, ec);
    if (ec) {
        return fsFailure(io::Err::FSCopyError, "Failed to stat copy source", source, ec);
    }

    switch (sourceStatus.type()) {
    case fs::file_type::regular:
        fs::copy_file(source, destination, fs::copy_options::none, ec);
        break;
    case fs::file_type::directory: {
        fs::create_directories(destination, ec);
        if (ec) {
            return fsFailure(io::Err::FSCopyError, "Failed to create copy destination", destination, ec);
        }
        if (!fs::is_directory(destination, ec)) {
            return fsFailure(io::Err::FSCopyError, "Copy destination is not a directory", destination, ec);
        }
        fs::directory_iterator iterator(source, ec);
        const fs::directory_iterator end;
        if (ec) {
            return fsFailure(io::Err::FSCopyError, "Failed to list copy source", source, ec);
        }
        for (; iterator != end; iterator.increment(ec)) {
            if (ec) {
                break;
            }
            const Ret ret = copyEntry(iterator->path(), destination / iterator->path().filename());
            if (!ret) {
                return ret;
            }
        }
        break;
    }
    case fs::file_type::symlink:
        fs::copy_symlink(source, destination, ec);
        break;
    case fs::file_type::not_found:
        return failure(io::Err::FSNotExist, "Copy source does not exist: " + pathToUtf8(source));
    default:
        return failure(io::Err::FSCopyError, "Unsupported copy source type: " + pathToUtf8(source));
    }

    if (ec) {
        return fsFailure(io::Err::FSCopyError, "Failed to copy", source, ec);
    }
    return make_ok();
}

void scanDirectory(const fs::path& directory, const io::path_t& outputDirectory,
                   bool recursive, bool wantFiles, bool wantFolders,
                   const std::vector<std::string>& filters,
                   io::paths_t& output, Ret& result)
{
    std::error_code ec;
    fs::directory_iterator iterator(directory, ec);
    const fs::directory_iterator end;
    if (ec) {
        result = fsFailure(io::Err::FSReadError, "Failed to list directory", directory, ec);
        return;
    }

    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            result = fsFailure(io::Err::FSReadError, "Failed to list directory", directory, ec);
            return;
        }

        const fs::directory_entry& entry = *iterator;
        std::error_code statusError;
        const fs::file_status linkStatus = entry.symlink_status(statusError);
        if (statusError) {
            result = fsFailure(io::Err::FSReadError, "Failed to stat directory entry",
                               entry.path(), statusError);
            return;
        }

        const bool isLink = linkStatus.type() == fs::file_type::symlink;
        fs::file_status status = linkStatus;
        if (isLink) {
            status = entry.status(statusError);
            if (statusError) {
                continue; // dangling symlink
            }
        }

        const std::string name = pathToUtf8(entry.path().filename());
        const bool matched = matchesAnyFilter(name, filters);
        if (status.type() == fs::file_type::directory) {
            if (wantFolders && matched) {
                output.push_back(appendName(outputDirectory, name));
            }
            if (recursive && !isLink) {
                scanDirectory(entry.path(), appendName(outputDirectory, name),
                              true, wantFiles, false, filters, output, result);
                if (!result) {
                    return;
                }
            }
        } else if (status.type() == fs::file_type::regular && wantFiles && matched) {
            output.push_back(appendName(outputDirectory, name));
        }
    }

    if (ec) {
        result = fsFailure(io::Err::FSReadError, "Failed to list directory", directory, ec);
    }
}

fs::path resolvedPath(const fs::path& path)
{
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    return ec ? absoluteNormalized(path) : canonical.lexically_normal();
}

bool pathsOverlap(const fs::path& lhs, const fs::path& rhs)
{
    const fs::path first = resolvedPath(lhs);
    const fs::path second = resolvedPath(rhs);
    return isPathPrefix(first, second) || isPathPrefix(second, first);
}

bool removeOnceWithRetry(const fs::path& path, std::error_code& ec)
{
    ec.clear();
    fs::remove(path, ec);
    if (!ec) {
        return true;
    }
    if (ec != std::errc::permission_denied) {
        return false;
    }

    // Windows read-only attribute (and some POSIX ACLs) prevent deletion; match upstream retry.
    std::error_code permissionError;
    fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, permissionError);
    if (permissionError) {
        return false;
    }
    ec.clear();
    fs::remove(path, ec);
    return !ec;
}

bool removeTree(const fs::path& path, std::error_code& ec)
{
    fs::directory_iterator iterator(path, ec);
    const fs::directory_iterator end;
    if (ec) {
        return false;
    }
    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            return false;
        }

        std::error_code entryError;
        const fs::file_status status = iterator->symlink_status(entryError);
        if (entryError) {
            ec = entryError;
            return false;
        }
        if (status.type() == fs::file_type::directory) {
            if (!removeTree(iterator->path(), ec)) {
                return false;
            }
        } else if (!removeOnceWithRetry(iterator->path(), ec)) {
            return false;
        }
    }
    if (ec) {
        return false;
    }
    return removeOnceWithRetry(path, ec);
}
} // namespace

struct NativeFileSystem::Stream {
    std::ofstream file;
};

NativeFileSystem::NativeFileSystem(fs::path resourceRoot)
{
    if (resourceRoot.empty()) {
        return;
    }

    std::error_code ec;
    fs::path absolute = fs::absolute(resourceRoot, ec);
    if (ec) {
        absolute = resourceRoot;
    }
    const fs::path canonical = fs::weakly_canonical(absolute, ec);
    if (!ec) {
        absolute = canonical;
    }
    m_resourceRoot = absolute.lexically_normal();
    // A trailing separator would survive lexically_normal and break component-wise containment.
    while (m_resourceRoot.has_relative_path() && m_resourceRoot.filename().empty()) {
        m_resourceRoot = m_resourceRoot.parent_path();
    }
    if (!m_resourceRoot.is_absolute()) {
        m_resourceRoot.clear();
        return;
    }
    m_hasResourceRoot = true;
}

NativeFileSystem::~NativeFileSystem()
{
    std::lock_guard<std::mutex> lock(m_streamsMutex);
    for (auto& item : m_streams) {
        if (item.second && item.second->file.is_open()) {
            item.second->file.flush();
            item.second->file.close();
        }
    }
    m_streams.clear();
}

NativeFileSystem::Target NativeFileSystem::resolve(const io::path_t& path) const
{
    try {
        Target target;
        const std::string& raw = path.toStdString();
        if (hasEmbeddedNul(raw)) {
            return target;
        }

        if (raw.size() >= 2 && raw[0] == ':' && raw[1] == '/') {
            if (!m_hasResourceRoot) {
                return target;
            }

            // Qt cleans resource paths, so ":/" and "://" name the same root (fret.cpp uses "://data/...").
            const std::size_t start = raw.find_first_not_of('/', 1);
            fs::path relative = fs::u8path(start == std::string::npos ? std::string() : raw.substr(start));
            if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
                return target;
            }
            for (const fs::path& component : relative) {
                if (unsafeVirtualComponent(component) || component.has_root_name()
                    || component.has_root_directory()) {
                    return target;
                }
            }

            relative = relative.lexically_normal();
            if (relative == fs::path(".")) {
                relative.clear();
            }
            // Lexical containment: every component is below resourceRoot and ".." is rejected above.
            target.path = (m_resourceRoot / relative).lexically_normal();
            if (!isPathPrefix(m_resourceRoot, target.path)) {
                return target;
            }
            target.virtualRel = pathToUtf8(relative);
            target.valid = true;
            target.resource = true;
            target.virtualPath = true;
            return target;
        }

        target.path = fs::u8path(raw);
        target.valid = true;
        target.resource = isProtectedPath(target.path);
        return target;
    } catch (const std::exception&) {
        return Target{};
    }
}

bool NativeFileSystem::isInsideResourceRoot(const fs::path& path) const
{
    if (!m_hasResourceRoot) {
        return false;
    }

    if (isPathPrefix(m_resourceRoot, absoluteNormalized(path))) {
        return true;
    }

    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    return !ec && isPathPrefix(m_resourceRoot, canonical.lexically_normal());
}

bool NativeFileSystem::isProtectedPath(const fs::path& path) const
{
    if (isInsideResourceRoot(path)) {
        return true;
    }

    // weakly_canonical cannot resolve a dangling final symlink, so follow it explicitly.
    fs::path current = path;
    for (int depth = 0; depth < 16; ++depth) {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec || status.type() != fs::file_type::symlink) {
            return false;
        }
        fs::path target = fs::read_symlink(current, ec);
        if (ec) {
            return false;
        }
        if (target.is_relative()) {
            target = current.parent_path() / target;
        }
        current = target.lexically_normal();
        if (isInsideResourceRoot(current)) {
            return true;
        }
    }
    return false;
}

bool NativeFileSystem::isResourceAncestor(const fs::path& path) const
{
    if (!m_hasResourceRoot) {
        return false;
    }

    if (isStrictPathPrefix(absoluteNormalized(path), m_resourceRoot)) {
        return true;
    }

    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    return !ec && isStrictPathPrefix(canonical.lexically_normal(), m_resourceRoot);
}

Ret NativeFileSystem::exists(const io::path_t& path) const
{
    const Target target = resolve(path);
    if (!target.valid) {
        return failure(io::Err::FSNotExist, "Invalid path: " + path.toStdString());
    }

    std::error_code ec;
    if (fs::exists(target.path, ec)) {
        return make_ok();
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return fsFailure(io::Err::FSNotExist, "Failed to stat", target.path, ec);
    }
    return make_ret(io::Err::FSNotExist);
}

Ret NativeFileSystem::remove(const io::path_t& path, bool onlyIfEmpty)
{
    const Target target = resolve(path);
    if (!target.valid) {
        return failure(io::Err::FSRemoveError, "Invalid path: " + path.toStdString());
    }
    if (target.resource || isResourceAncestor(target.path)) {
        return readOnlyFailure(io::Err::FSRemoveError, "Cannot remove", path);
    }

    std::error_code ec;
    const fs::file_status status = fs::symlink_status(target.path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return make_ok();
        }
        return fsFailure(io::Err::FSRemoveError, "Failed to stat", target.path, ec);
    }
    if (status.type() == fs::file_type::not_found) {
        return make_ok();
    }

    bool removed = false;
    if (status.type() == fs::file_type::directory) {
        if (onlyIfEmpty) {
            if (!fs::is_empty(target.path, ec)) {
                if (ec) {
                    return fsFailure(io::Err::FSRemoveError, "Failed to inspect directory",
                                     target.path, ec);
                }
                return make_ret(io::Err::FSDirNotEmptyError);
            }
            removed = removeOnceWithRetry(target.path, ec);
        } else {
            removed = removeTree(target.path, ec);
        }
    } else {
        removed = removeOnceWithRetry(target.path, ec);
    }

    if (!removed) {
        return fsFailure(io::Err::FSRemoveError, "Failed to remove", target.path, ec);
    }
    return make_ok();
}

Ret NativeFileSystem::clear(const io::path_t& path)
{
    const Target target = resolve(path);
    if (!target.valid) {
        return failure(io::Err::FSClearError, "Invalid path: " + path.toStdString());
    }
    if (target.resource || isResourceAncestor(target.path)) {
        return readOnlyFailure(io::Err::FSClearError, "Cannot clear", path);
    }

    std::error_code ec;
    const fs::file_status status = fs::symlink_status(target.path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return make_ok();
        }
        return fsFailure(io::Err::FSClearError, "Failed to stat", target.path, ec);
    }
    if (status.type() == fs::file_type::not_found) {
        return make_ok();
    }
    if (status.type() == fs::file_type::symlink) {
        return failure(io::Err::FSClearError,
                       "Refusing to clear a symlinked directory: " + pathToUtf8(target.path));
    }
    if (status.type() != fs::file_type::directory) {
        return failure(io::Err::FSClearError, "Not a directory: " + pathToUtf8(target.path));
    }

    fs::directory_iterator iterator(target.path, ec);
    const fs::directory_iterator end;
    if (ec) {
        return fsFailure(io::Err::FSClearError, "Failed to list directory", target.path, ec);
    }

    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            return fsFailure(io::Err::FSClearError, "Failed to list directory", target.path, ec);
        }

        const fs::directory_entry& entry = *iterator;
        std::error_code entryError;
        const fs::file_status entryStatus = entry.symlink_status(entryError);
        if (entryError) {
            return fsFailure(io::Err::FSClearError, "Failed to stat directory entry",
                             entry.path(), entryError);
        }

        bool removed = false;
        if (entryStatus.type() == fs::file_type::directory) {
            removed = removeTree(entry.path(), entryError);
        } else {
            removed = removeOnceWithRetry(entry.path(), entryError);
        }
        if (!removed) {
            return fsFailure(io::Err::FSClearError, "Failed to remove directory entry",
                             entry.path(), entryError);
        }
    }
    if (ec) {
        return fsFailure(io::Err::FSClearError, "Failed to list directory", target.path, ec);
    }

    return make_ok();
}

Ret NativeFileSystem::copy(const io::path_t& src, const io::path_t& dst, bool replace)
{
    const Target source = resolve(src);
    const Target destination = resolve(dst);
    if (!source.valid) {
        return failure(io::Err::FSNotExist, "Invalid copy source: " + src.toStdString());
    }
    if (!destination.valid) {
        return failure(io::Err::FSCopyError, "Invalid copy destination: " + dst.toStdString());
    }
    if (destination.resource
        || (replace && isResourceAncestor(destination.path))) {
        return readOnlyFailure(io::Err::FSCopyError, "Cannot copy to", dst);
    }

    std::error_code ec;
    if (!fs::exists(source.path, ec)) {
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return fsFailure(io::Err::FSCopyError, "Failed to stat copy source", source.path, ec);
        }
        return make_ret(io::Err::FSNotExist);
    }

    ec.clear();
    const bool destinationExists = fs::exists(destination.path, ec);
    if (destinationExists && !replace) {
        return make_ret(io::Err::FSAlreadyExists);
    }
    if (pathsOverlap(source.path, destination.path)) {
        return failure(io::Err::FSCopyError,
                       "Copy source and destination overlap: " + src.toStdString()
                       + " -> " + dst.toStdString());
    }
    if (destinationExists) {
        const Ret removed = remove(dst);
        if (!removed) {
            return removed;
        }
    }

    return copyEntry(source.path, destination.path);
}

Ret NativeFileSystem::move(const io::path_t& src, const io::path_t& dst, bool replace)
{
    const Target source = resolve(src);
    const Target destination = resolve(dst);
    if (!source.valid) {
        return failure(io::Err::FSNotExist, "Invalid move source: " + src.toStdString());
    }
    if (!destination.valid) {
        return failure(io::Err::FSMoveErrors, "Invalid move destination: " + dst.toStdString());
    }
    if (source.resource || isResourceAncestor(source.path)) {
        return readOnlyFailure(io::Err::FSMoveErrors, "Cannot move", src);
    }
    if (destination.resource || (replace && isResourceAncestor(destination.path))) {
        return readOnlyFailure(io::Err::FSMoveErrors, "Cannot move to", dst);
    }

    std::error_code ec;
    if (!fs::exists(source.path, ec)) {
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return fsFailure(io::Err::FSMoveErrors, "Failed to stat move source", source.path, ec);
        }
        return make_ret(io::Err::FSNotExist);
    }

    ec.clear();
    const bool destinationExists = fs::exists(destination.path, ec);
    if (destinationExists && !replace) {
        return make_ret(io::Err::FSAlreadyExists);
    }
    if (pathsOverlap(source.path, destination.path)) {
        return failure(io::Err::FSMoveErrors,
                       "Move source and destination overlap: " + src.toStdString()
                       + " -> " + dst.toStdString());
    }
    if (destinationExists) {
        const Ret removed = remove(dst);
        if (!removed) {
            return removed;
        }
    }

    fs::rename(source.path, destination.path, ec);
    if (ec) {
        return fsFailure(io::Err::FSMoveErrors, "Failed to move", source.path, ec);
    }
    return make_ok();
}

Ret NativeFileSystem::makePath(const io::path_t& path) const
{
    const Target target = resolve(path);
    if (!target.valid || target.path.empty()) {
        return failure(io::Err::FSMakingError, "Invalid path: " + path.toStdString());
    }
    if (target.resource) {
        return readOnlyFailure(io::Err::FSMakingError, "Cannot create resource path", path);
    }

    std::error_code ec;
    fs::create_directories(target.path, ec);
    if (ec) {
        return fsFailure(io::Err::FSMakingError, "Failed to create directory", target.path, ec);
    }
    if (!fs::is_directory(target.path, ec)) {
        return fsFailure(io::Err::FSMakingError, "Not a directory", target.path, ec);
    }
    return make_ok();
}

Ret NativeFileSystem::makeLink(const io::path_t& targetPath, const io::path_t& linkPath) const
{
    const Target target = resolve(targetPath);
    const Target link = resolve(linkPath);
    if (!target.valid) {
        return failure(io::Err::FSMakingError, "Invalid link target: " + targetPath.toStdString());
    }
    if (!link.valid || link.path.empty()) {
        return failure(io::Err::FSMakingError, "Invalid link path: " + linkPath.toStdString());
    }
    if (link.resource) {
        return readOnlyFailure(io::Err::FSMakingError, "Cannot create link in", linkPath);
    }

    const fs::path linkTarget = target.path.lexically_normal();
    std::error_code ec;
    const bool targetIsDirectory = fs::is_directory(linkTarget, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return fsFailure(io::Err::FSMakingError, "Failed to inspect link target", linkTarget, ec);
    }
    ec.clear();
    if (targetIsDirectory) {
        fs::create_directory_symlink(linkTarget, link.path, ec);
    } else {
        fs::create_symlink(linkTarget, link.path, ec);
    }
    if (ec) {
        return fsFailure(io::Err::FSMakingError, "Failed to create link", link.path, ec);
    }
    return make_ok();
}

EntryType NativeFileSystem::entryType(const io::path_t& path) const
{
    const Target target = resolve(path);
    if (!target.valid) {
        return EntryType::Undefined;
    }

    std::error_code ec;
    const fs::file_status status = fs::status(target.path, ec);
    if (ec) {
        return EntryType::Undefined;
    }
    if (fs::is_regular_file(status)) {
        return EntryType::File;
    }
    if (fs::is_directory(status)) {
        return EntryType::Dir;
    }
    return EntryType::Undefined;
}

RetVal<uint64_t> NativeFileSystem::fileSize(const io::path_t& path) const
{
    RetVal<uint64_t> result;
    const Target target = resolve(path);
    if (!target.valid) {
        result.ret = failure(io::Err::FSNotExist, "Invalid path: " + path.toStdString());
        return result;
    }

    const Ret existsRet = exists(path);
    if (!existsRet) {
        result.ret = existsRet;
        return result;
    }

    std::error_code ec;
    const uint64_t size = fs::file_size(target.path, ec);
    if (ec) {
        result.ret = fsFailure(io::Err::FSReadError, "Failed to get file size", target.path, ec);
        return result;
    }
    result.ret = make_ok();
    result.val = size;
    return result;
}

RetVal<uint64_t> NativeFileSystem::availableSpace(const io::path_t& path) const
{
    RetVal<uint64_t> result;
    const Target target = resolve(path);
    if (!target.valid) {
        result.ret = failure(io::Err::FSNotExist, "Invalid path: " + path.toStdString());
        return result;
    }

    // QStorageInfo treats a missing path as an invalid storage, so an error here
    // means "not there" as much as "cannot query".
    std::error_code ec;
    const fs::space_info space = fs::space(target.path, ec);
    if (ec) {
        result.ret = fsFailure(io::Err::FSNotExist, "Failed to query available space", target.path, ec);
        return result;
    }
    result.ret = make_ok();
    result.val = static_cast<uint64_t>(space.available);
    return result;
}

RetVal<io::paths_t> NativeFileSystem::scanFiles(const io::path_t& rootDir,
                                                const std::vector<std::string>& filters,
                                                ScanMode mode) const
{
    RetVal<io::paths_t> result;
    const Target root = resolve(rootDir);
    if (!root.valid) {
        result.ret = failure(io::Err::FSNotExist, "Invalid path: " + rootDir.toStdString());
        return result;
    }

    const Ret existsRet = exists(rootDir);
    if (!existsRet) {
        result.ret = existsRet;
        return result;
    }

    std::error_code ec;
    if (!fs::is_directory(root.path, ec)) {
        if (ec && ec != std::errc::no_such_file_or_directory) {
            result.ret = fsFailure(io::Err::FSReadError, "Failed to stat directory", root.path, ec);
            return result;
        }
        result.ret = make_ok();
        return result;
    }

    bool wantFiles = false;
    bool wantFolders = false;
    bool recursive = false;
    switch (mode) {
    case ScanMode::FilesInCurrentDir:
        wantFiles = true;
        break;
    case ScanMode::FoldersInCurrentDir:
        wantFolders = true;
        break;
    case ScanMode::FilesAndFoldersInCurrentDir:
        wantFiles = true;
        wantFolders = true;
        break;
    case ScanMode::FilesInCurrentDirAndSubdirs:
        wantFiles = true;
        recursive = true;
        break;
    }

    const io::path_t outputRoot = root.virtualPath
                                  ? io::path_t(root.virtualRel.empty()
                                               ? ":/" : ":/" + root.virtualRel)
                                  : io::path_t(pathToUtf8(root.path));
    result.ret = make_ok();
    scanDirectory(root.path, outputRoot, recursive, wantFiles, wantFolders, filters, result.val, result.ret);
    if (!result.ret) {
        result.val.clear();
        return result;
    }
    std::sort(result.val.begin(), result.val.end());
    return result;
}

void NativeFileSystem::setAttribute(const io::path_t&, io::IFileSystem::Attribute) const
{
    // Hidden attributes have no portable std::filesystem equivalent, so this is a no-op.
}

bool NativeFileSystem::setPermissionsAllowedForAll(const io::path_t& path) const
{
    const Target target = resolve(path);
    if (!target.valid || target.resource) {
        return false;
    }

    const fs::perms all = fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec
                          | fs::perms::group_read | fs::perms::group_write | fs::perms::group_exec
                          | fs::perms::others_read | fs::perms::others_write | fs::perms::others_exec;
    std::error_code ec;
    fs::permissions(target.path, all, fs::perm_options::replace, ec);
    return !ec;
}

Ret NativeFileSystem::readFile(const io::path_t& filePath, ByteArray& data) const
{
    const Target target = resolve(filePath);
    if (!target.valid) {
        return failure(io::Err::FSReadError, "Invalid path: " + filePath.toStdString());
    }

    std::ifstream stream(target.path, std::ios::binary);
    if (!stream.is_open()) {
        return failure(io::Err::FSReadError, "Failed to open file: " + pathToUtf8(target.path));
    }

    std::error_code ec;
    const uint64_t size = fs::file_size(target.path, ec);
    if (ec) {
        return fsFailure(io::Err::FSReadError, "Failed to get file size", target.path, ec);
    }
    if (size > kMaxReadFileSize) {
        return failure(io::Err::FSReadError,
                       "File exceeds the 256 MiB read limit: " + pathToUtf8(target.path));
    }

    ByteArray buffer;
    try {
        buffer = ByteArray(static_cast<size_t>(size));
    } catch (const std::bad_alloc&) {
        return failure(io::Err::FSReadError, "Out of memory reading file: " + pathToUtf8(target.path));
    }
    if (size > 0) {
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
        if (!stream || static_cast<uint64_t>(stream.gcount()) != size) {
            return failure(io::Err::FSReadError, "Unexpected end of file: " + pathToUtf8(target.path));
        }
    }
    data = std::move(buffer);
    return make_ok();
}

RetVal<ByteArray> NativeFileSystem::readFile(const io::path_t& filePath) const
{
    RetVal<ByteArray> result;
    const Ret existsRet = exists(filePath);
    if (!existsRet) {
        result.ret = existsRet;
        return result;
    }

    ByteArray data;
    const Ret readRet = readFile(filePath, data);
    result.ret = readRet;
    if (readRet) {
        result.val = std::move(data);
    }
    return result;
}

Ret NativeFileSystem::writeFile(const io::path_t& filePath, const ByteArray& data)
{
    const Target target = resolve(filePath);
    if (!target.valid) {
        return failure(io::Err::FSWriteError, "Invalid path: " + filePath.toStdString());
    }
    if (target.resource) {
        return readOnlyFailure(io::Err::FSWriteError, "Cannot write", filePath);
    }
    if (data.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        return failure(io::Err::FSWriteError, "Data is too large to write: " + filePath.toStdString());
    }

    std::ofstream stream(target.path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return failure(io::Err::FSWriteError, "Failed to open file for writing: "
                       + pathToUtf8(target.path));
    }

    if (!data.empty()) {
        stream.write(data.constChar(), static_cast<std::streamsize>(data.size()));
    }
    stream.flush();
    if (!stream) {
        return failure(io::Err::FSWriteError, "Failed to write file: " + pathToUtf8(target.path));
    }
    stream.close();
    if (stream.fail()) {
        return failure(io::Err::FSWriteError, "Failed to close file: " + pathToUtf8(target.path));
    }
    return make_ok();
}

RetVal<StreamId> NativeFileSystem::openStream(const io::path_t& filePath, OpenMode mode)
{
    RetVal<StreamId> result;
    const Target target = resolve(filePath);
    if (!target.valid) {
        result.ret = failure(io::Err::FSWriteError, "Invalid path: " + filePath.toStdString());
        return result;
    }
    if (target.resource) {
        result.ret = readOnlyFailure(io::Err::FSWriteError, "Cannot write", filePath);
        return result;
    }

    std::ios::openmode flags = std::ios::binary | std::ios::out;
    flags |= (mode == OpenMode::Append) ? std::ios::app : std::ios::trunc;
    auto stream = std::make_unique<Stream>();
    stream->file.open(target.path, flags);
    if (!stream->file.is_open()) {
        result.ret = failure(io::Err::FSWriteError,
                             "Failed to open stream: " + pathToUtf8(target.path));
        return result;
    }

    std::lock_guard<std::mutex> lock(m_streamsMutex);
    while (m_streams.find(m_nextStreamId) != m_streams.end()) {
        if (m_nextStreamId == std::numeric_limits<StreamId>::max()) {
            m_nextStreamId = 0;
        } else {
            ++m_nextStreamId;
        }
    }

    const StreamId id = m_nextStreamId;
    if (m_nextStreamId == std::numeric_limits<StreamId>::max()) {
        m_nextStreamId = 0;
    } else {
        ++m_nextStreamId;
    }
    m_streams.emplace(id, std::move(stream));

    result.ret = make_ok();
    result.val = id;
    return result;
}

Ret NativeFileSystem::writeToStream(StreamId fileId, const ByteArray& data, uint64_t offset)
{
    std::lock_guard<std::mutex> lock(m_streamsMutex);
    auto it = m_streams.find(fileId);
    if (it == m_streams.end() || !it->second) {
        return failure(io::Err::FSWriteError, "Invalid stream id");
    }

    std::ofstream& stream = it->second->file;
    if (offset != STREAM_POS_CURRENT) {
        if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return failure(io::Err::FSWriteError, "Stream offset is out of range");
        }
        stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream) {
            return failure(io::Err::FSWriteError, "Failed to seek stream");
        }
    }

    if (data.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        return failure(io::Err::FSWriteError, "Stream write is too large");
    }
    if (!data.empty()) {
        stream.write(data.constChar(), static_cast<std::streamsize>(data.size()));
    }
    if (!stream) {
        return failure(io::Err::FSWriteError, "Failed to write stream");
    }
    return make_ok();
}

Ret NativeFileSystem::closeStream(StreamId fileId)
{
    std::unique_ptr<Stream> stream;
    {
        std::lock_guard<std::mutex> lock(m_streamsMutex);
        auto it = m_streams.find(fileId);
        if (it == m_streams.end() || !it->second) {
            return failure(io::Err::FSWriteError, "Invalid stream id");
        }
        stream = std::move(it->second);
        m_streams.erase(it);
    }

    if (!stream) {
        return failure(io::Err::FSWriteError, "Invalid stream id");
    }
    stream->file.flush();
    const bool flushed = stream->file.good();
    stream->file.close();
    if (!flushed || stream->file.fail()) {
        return failure(io::Err::FSWriteError, "Failed to flush or close stream");
    }
    return make_ok();
}

io::path_t NativeFileSystem::canonicalFilePath(const io::path_t& filePath) const
{
    const Target target = resolve(filePath);
    if (!target.valid) {
        return {};
    }

    std::error_code ec;
    const fs::path canonical = fs::canonical(target.path, ec);
    if (ec) {
        return {};
    }
    return io::path_t(pathToUtf8(canonical));
}

io::path_t NativeFileSystem::absolutePath(const io::path_t& filePath) const
{
    const Target target = resolve(filePath);
    if (!target.valid) {
        return {};
    }

    std::error_code ec;
    const fs::path absolute = fs::absolute(target.path, ec);
    if (ec) {
        return {};
    }
    return io::path_t(pathToUtf8(absolute.lexically_normal().parent_path()));
}

io::path_t NativeFileSystem::absoluteFilePath(const io::path_t& filePath) const
{
    const Target target = resolve(filePath);
    if (!target.valid) {
        return {};
    }

    std::error_code ec;
    const fs::path absolute = fs::absolute(target.path, ec);
    if (ec) {
        return {};
    }
    return io::path_t(pathToUtf8(absolute.lexically_normal()));
}

io::path_t NativeFileSystem::cleanPath(const io::path_t& filePath) const
{
    // Lexical only: QDir::cleanPath never touches the disk, so an invalid or
    // missing path is still cleaned instead of rejected.
    return io::path_t(cleanPathString(filePath.toStdString()));
}

bool NativeFileSystem::isSymLink(const io::path_t& filePath) const
{
    const Target target = resolve(filePath);
    if (!target.valid) {
        return false;
    }

    std::error_code ec;
    const fs::file_status status = fs::symlink_status(target.path, ec);
    return !ec && fs::is_symlink(status);
}

io::path_t NativeFileSystem::symLinkTarget(const io::path_t& filePath) const
{
    const Target target = resolve(filePath);
    if (!target.valid) {
        return {};
    }

    std::error_code ec;
    fs::path link = fs::read_symlink(target.path, ec);
    if (ec) {
        return {};
    }
    // QFileInfo::symLinkTarget reports an absolute path; read_symlink returns the
    // stored target, which is relative to the link's own directory.
    if (link.is_relative()) {
        link = target.path.parent_path() / link;
    }
    return io::path_t(pathToUtf8(link.lexically_normal()));
}

io::path_t NativeFileSystem::temporaryDirectoryPath() const
{
    std::error_code ec;
    const fs::path directory = fs::temp_directory_path(ec);
    if (ec) {
        return {};
    }
    return io::path_t(pathToUtf8(directory));
}

DateTime NativeFileSystem::birthTime(const io::path_t& filePath) const
{
    // std::filesystem has no portable creation time; do not fake one.
    (void)filePath;
    return {};
}

DateTime NativeFileSystem::lastModified(const io::path_t& filePath) const
{
    const Target target = resolve(filePath);
    if (!target.valid) {
        return {};
    }

    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(target.path, ec);
    if (ec) {
        return {};
    }
    return dateTimeFromFileTime(time);
}

Ret NativeFileSystem::isWritable(const io::path_t& filePath) const
{
    const Target target = resolve(filePath);
    if (!target.valid) {
        return failure(io::Err::FSWriteError, "Invalid path: " + filePath.toStdString());
    }
    if (target.resource) {
        return readOnlyFailure(io::Err::FSWriteError, "Cannot write", filePath);
    }

    std::error_code ec;
    const bool existed = fs::exists(target.path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return fsFailure(io::Err::FSWriteError, "Failed to stat", target.path, ec);
    }

    // A directory cannot be opened as a stream; match the upstream permission check.
    ec.clear();
    const fs::file_status status = fs::status(target.path, ec);
    if (!ec && fs::is_directory(status)) {
        const fs::perms writeBits = fs::perms::owner_write | fs::perms::group_write
                                    | fs::perms::others_write;
        if ((status.permissions() & writeBits) != fs::perms::none) {
            return make_ok();
        }
        return failure(io::Err::FSWriteError, "Directory is not writable: " + pathToUtf8(target.path));
    }

    std::ofstream stream(target.path, std::ios::binary | std::ios::app);
    if (!stream.is_open()) {
        return failure(io::Err::FSWriteError, "Path is not writable: " + pathToUtf8(target.path));
    }
    stream.close();
    const bool closed = !stream.fail();

    if (!existed) {
        std::error_code removeError;
        fs::remove(target.path, removeError);
    }
    if (!closed) {
        return failure(io::Err::FSWriteError, "Failed to close file: " + pathToUtf8(target.path));
    }
    return make_ok();
}
