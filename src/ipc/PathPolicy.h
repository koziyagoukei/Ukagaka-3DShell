#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace uka3d::ipc {

// Resolves `candidate` (absolute, or relative to `base`) and returns the
// normalised absolute path if it lies inside one of `roots` (after resolving
// "..", symbolic links and junctions of existing path parts). Comparison is
// case-insensitive. Returns nullopt for anything outside the roots.
std::optional<std::filesystem::path> resolveWithinRoots(const std::filesystem::path &candidate,
                                                        const std::filesystem::path &base,
                                                        const std::vector<std::filesystem::path> &roots);

bool isWithin(const std::filesystem::path &path, const std::filesystem::path &root);

} // namespace uka3d::ipc
