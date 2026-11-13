#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace uka3d {

// UTF-8 <-> UTF-16 helpers. All internal strings are UTF-8; Win32 calls use UTF-16.
std::wstring utf8ToWide(std::string_view utf8);
std::string wideToUtf8(std::wstring_view wide);

// Always returns UTF-8 regardless of the process ANSI code page.
std::string pathToUtf8(const std::filesystem::path &path);
// Interprets the argument as UTF-8 regardless of the process ANSI code page.
std::filesystem::path utf8ToPath(std::string_view utf8);

} // namespace uka3d
