#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace movescape {

struct PackageDiscoveryLimits {
  std::size_t max_manifest_bytes = 1024U * 1024U;
  std::size_t max_packages = 256;
  std::size_t max_dependency_depth = 64;
  std::size_t max_modules = 100'000;
};

struct LocalPackageDependency {
  std::string name;
  std::filesystem::path root;
};

struct MovePackageManifest {
  std::filesystem::path root;
  std::filesystem::path path;
  std::string package_name;
  std::vector<LocalPackageDependency> local_dependencies;
};

// Reads the package name and local dependency entries from Move.toml. Only
// dependency declarations containing an explicit `local` path are returned;
// this function never fetches Git, REST, or on-chain dependencies.
[[nodiscard]] MovePackageManifest loadMovePackageManifest(const std::filesystem::path &package_root, const PackageDiscoveryLimits &limits = {});

// Each input can be either a compiled Move package root (a directory with a
// Move.toml) or a raw bytecode tree. Local manifest dependencies are traversed
// deterministically. Package artifacts are read from
// build/<package>/bytecode_modules; raw trees are searched recursively.
[[nodiscard]] std::vector<std::filesystem::path> discoverPackageInputModules(const std::vector<std::filesystem::path> &inputs,
                                                                             const PackageDiscoveryLimits &limits = {});

} // namespace movescape
