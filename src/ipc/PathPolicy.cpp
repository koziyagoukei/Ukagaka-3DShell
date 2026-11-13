#include "ipc/PathPolicy.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>

namespace uka3d::ipc {
namespace {

std::filesystem::path canonicalish(const std::filesystem::path &path) {
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        result = path.lexically_normal();
    return result;
}

std::wstring folded(const std::filesystem::path &path) {
    std::wstring text = path.native();
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/'))
        text.pop_back();
    return text;
}

} // namespace

bool isWithin(const std::filesystem::path &path, const std::filesystem::path &root) {
    const std::wstring p = folded(canonicalish(path));
    std::wstring r = folded(canonicalish(root));
    if (r.empty() || p.size() < r.size() || p.compare(0, r.size(), r) != 0)
        return false;
    if (p.size() == r.size())
        return true;
    return r.back() == L'\\' || p[r.size()] == L'\\';
}

std::optional<std::filesystem::path> resolveWithinRoots(const std::filesystem::path &candidate,
                                                        const std::filesystem::path &base,
                                                        const std::vector<std::filesystem::path> &roots) {
    if (candidate.empty())
        return std::nullopt;
    // Reject device/UNC style paths outright (\\?\, \\server\share, \\.\pipe ...).
    const std::wstring &text = candidate.native();
    if (text.size() >= 2 && (text[0] == L'\\' || text[0] == L'/') && (text[1] == L'\\' || text[1] == L'/'))
        return std::nullopt;
    if (text.find(L':', candidate.has_root_name() ? 2 : 0) != std::wstring::npos)
        return std::nullopt; // alternate data streams
    const std::filesystem::path absolute =
        candidate.is_absolute() ? candidate : (base / candidate);
    const std::filesystem::path resolved = canonicalish(absolute);
    for (const auto &root : roots) {
        if (!root.empty() && isWithin(resolved, root))
            return resolved;
    }
    return std::nullopt;
}

} // namespace uka3d::ipc
