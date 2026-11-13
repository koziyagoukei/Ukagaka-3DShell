#include "platform/Utf.h"

#include <windows.h>

namespace uka3d {

std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), length);
    return result;
}

std::string wideToUtf8(std::wstring_view wide) {
    if (wide.empty())
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), length, nullptr,
                        nullptr);
    return result;
}

std::string pathToUtf8(const std::filesystem::path &path) {
    return wideToUtf8(path.native());
}

std::filesystem::path utf8ToPath(std::string_view utf8) {
    return std::filesystem::path(utf8ToWide(utf8));
}

} // namespace uka3d
