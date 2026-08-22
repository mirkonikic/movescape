#include "test.hpp"

#include "movescape/package_loader.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

class TemporaryTree {
public:
  TemporaryTree() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() / ("movescape-package-loader-test-" + std::to_string(nonce));
    REQUIRE(std::filesystem::create_directory(root_));
  }

  TemporaryTree(const TemporaryTree &) = delete;
  TemporaryTree &operator=(const TemporaryTree &) = delete;

  ~TemporaryTree() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &root() const noexcept { return root_; }

private:
  std::filesystem::path root_;
};

void writeText(const std::filesystem::path &path, const std::string &text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void createArtifact(const std::filesystem::path &package, std::string_view name, std::string_view module) {
  const auto directory = package / "build" / name / "bytecode_modules";
  REQUIRE(std::filesystem::create_directories(directory));
  writeText(directory / (std::string(module) + ".mv"), std::string(module));
}

} // namespace

TEST(package_manifest_parses_local_dependencies_deterministically) {
  TemporaryTree tree;
  const auto root = tree.root() / "root";
  const auto alpha = tree.root() / "alpha";
  const auto beta = tree.root() / "beta";
  REQUIRE(std::filesystem::create_directories(root));
  REQUIRE(std::filesystem::create_directories(alpha));
  REQUIRE(std::filesystem::create_directories(beta));
  writeText(root / "Move.toml", "[package]\n"
                                "name = \"Root\" # retained package name\n"
                                "[dependencies]\n"
                                "Beta = { local = '../beta', addr_subst = { X = '0x1' } }\n"
                                "Remote = { git = 'https://invalid.example/repo' }\n"
                                "[dev-dependencies]\n"
                                "Alpha = {\n  local = \"../alpha\"\n}\n");

  const auto manifest = movescape::loadMovePackageManifest(root);
  REQUIRE_EQ(manifest.package_name, std::string("Root"));
  REQUIRE_EQ(manifest.local_dependencies.size(), 2U);
  REQUIRE_EQ(manifest.local_dependencies[0].name, std::string("Alpha"));
  REQUIRE_EQ(manifest.local_dependencies[0].root, std::filesystem::canonical(alpha));
  REQUIRE_EQ(manifest.local_dependencies[1].name, std::string("Beta"));
  REQUIRE_EQ(manifest.local_dependencies[1].root, std::filesystem::canonical(beta));
}

TEST(package_discovery_walks_local_manifest_graph_and_bytecode_trees) {
  TemporaryTree tree;
  const auto root = tree.root() / "root";
  const auto dependency = tree.root() / "dependency";
  REQUIRE(std::filesystem::create_directories(root));
  REQUIRE(std::filesystem::create_directories(dependency));
  writeText(root / "Move.toml", "[package]\nname = 'Root'\n"
                                "[dependencies]\nDep = { local = '../dependency' }\n");
  writeText(dependency / "Move.toml", "[package]\nname = 'Dep'\n");
  createArtifact(root, "Root", "root");
  createArtifact(dependency, "Dep", "dep");

  const auto modules = movescape::discoverPackageInputModules({root});
  REQUIRE_EQ(modules.size(), 2U);
  REQUIRE_EQ(modules[0].filename(), std::filesystem::path("dep.mv"));
  REQUIRE_EQ(modules[1].filename(), std::filesystem::path("root.mv"));
}

TEST(package_discovery_uses_dependency_artifacts_embedded_in_primary_build) {
  TemporaryTree tree;
  const auto root = tree.root() / "root";
  const auto dependency = tree.root() / "dependency";
  REQUIRE(std::filesystem::create_directories(root));
  REQUIRE(std::filesystem::create_directories(dependency));
  writeText(root / "Move.toml", "[package]\nname = 'Root'\n"
                                "[dependencies]\nDep = { local = '../dependency' }\n");
  writeText(dependency / "Move.toml", "[package]\nname = 'Dep'\n");
  createArtifact(root, "Root", "root");
  const auto embedded = root / "build" / "Root" / "bytecode_modules" / "dependencies";
  REQUIRE(std::filesystem::create_directory(embedded));
  writeText(embedded / "dep.mv", "dep");

  const auto modules = movescape::discoverPackageInputModules({root});
  REQUIRE_EQ(modules.size(), 2U);
  REQUIRE_EQ(modules[0].filename(), std::filesystem::path("dep.mv"));
  REQUIRE_EQ(modules[1].filename(), std::filesystem::path("root.mv"));
}

TEST(package_discovery_rejects_a_local_manifest_cycle) {
  TemporaryTree tree;
  const auto alpha = tree.root() / "alpha";
  const auto beta = tree.root() / "beta";
  REQUIRE(std::filesystem::create_directories(alpha));
  REQUIRE(std::filesystem::create_directories(beta));
  writeText(alpha / "Move.toml", "[package]\nname='Alpha'\n"
                                 "[dependencies]\nBeta={local='../beta'}\n");
  writeText(beta / "Move.toml", "[package]\nname='Beta'\n"
                                "[dependencies]\nAlpha={local='../alpha'}\n");
  createArtifact(alpha, "Alpha", "alpha");

  REQUIRE_ERROR(movescape::discoverPackageInputModules({alpha}), movescape::ErrorCode::InvalidArgument);
}

TEST(package_discovery_requires_primary_compiled_artifacts) {
  TemporaryTree tree;
  const auto root = tree.root() / "root";
  REQUIRE(std::filesystem::create_directory(root));
  writeText(root / "Move.toml", "[package]\nname='Root'\n");

  REQUIRE_ERROR(movescape::discoverPackageInputModules({root}), movescape::ErrorCode::InvalidArgument);
}

TEST(package_discovery_enforces_manifest_and_graph_limits) {
  TemporaryTree tree;
  const auto root = tree.root() / "root";
  REQUIRE(std::filesystem::create_directory(root));
  writeText(root / "Move.toml", "[package]\nname='Root'\n");
  createArtifact(root, "Root", "root");

  REQUIRE_ERROR(movescape::discoverPackageInputModules({root},
                                                      movescape::PackageDiscoveryLimits{
                                                          .max_manifest_bytes = 4,
                                                          .max_packages = 1,
                                                          .max_dependency_depth = 1,
                                                          .max_modules = 1,
                                                      }),
                movescape::ErrorCode::ResourceLimit);
  REQUIRE_ERROR(movescape::discoverPackageInputModules({root},
                                                      movescape::PackageDiscoveryLimits{
                                                          .max_manifest_bytes = 1024,
                                                          .max_packages = 1,
                                                          .max_dependency_depth = 1,
                                                          .max_modules = 0,
                                                      }),
                movescape::ErrorCode::InvalidArgument);
}
