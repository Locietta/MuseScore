#include "atomic_file.h"
#include <algorithm>
#include <random>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace music {
void write_atomic(const std::filesystem::path& path, std::string_view bytes) {
    std::random_device random;
    auto temporary = path.parent_path() / (".musescore-export-" + std::to_string(random()) + "-" + std::to_string(random()));
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(), std::system_category(), "Cannot create export file");
    try {
        while (!bytes.empty()) {
            const DWORD count = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1u << 24));
            DWORD written = 0;
            if (!WriteFile(file, bytes.data(), count, &written, nullptr) || written != count)
                throw std::runtime_error("Cannot write complete export");
            bytes.remove_prefix(written);
        }
        if (!FlushFileBuffers(file)) throw std::system_error(GetLastError(), std::system_category(), "Cannot flush export");
        const auto closed = CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (!closed) throw std::system_error(GetLastError(), std::system_category(), "Cannot close export");
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "Cannot commit export");
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
#else
    int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (file < 0) throw std::system_error(errno, std::generic_category(), "Cannot create export file");
    try {
        while (!bytes.empty()) {
            const auto written = ::write(file, bytes.data(), std::min<size_t>(bytes.size(), 1u << 24));
            if (written < 0 && errno == EINTR) continue;
            if (written < 0) throw std::system_error(errno, std::generic_category(), "Cannot write export");
            if (written == 0) throw std::system_error(std::make_error_code(std::errc::io_error), "Cannot write export");
            bytes.remove_prefix(static_cast<size_t>(written));
        }
        int flushed;
        do { flushed = ::fsync(file); } while (flushed != 0 && errno == EINTR);
        if (flushed != 0) throw std::system_error(errno, std::generic_category(), "Cannot flush export");
        const int result = ::close(file);
        file = -1;
        if (result != 0) throw std::system_error(errno, std::generic_category(), "Cannot close export");
        std::filesystem::rename(temporary, path);
    } catch (...) {
        if (file >= 0) ::close(file);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
#endif
}
}
