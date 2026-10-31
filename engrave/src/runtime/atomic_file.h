#pragma once
#include <filesystem>
#include <string_view>

namespace music { void write_atomic(const std::filesystem::path& path, std::string_view bytes); }
