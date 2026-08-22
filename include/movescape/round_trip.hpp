#pragma once

#include "movescape/body_compare.hpp"
#include "movescape/module.hpp"
#include "movescape/module_compare.hpp"
#include "movescape/move_emitter.hpp"
#include "movescape/package_loader.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace movescape {

struct RoundTripPackage {
  std::filesystem::path root;
  std::filesystem::path manifest;
  std::filesystem::path source;
  MoveEmission emission;
  std::vector<std::string> external_modules;

  [[nodiscard]] bool readyForCompilerAttempt() const noexcept {
    return emission.allControlFlowComplete() && emission.allSourceSemanticsComplete() && external_modules.empty();
  }
};

struct MultiModuleRoundTripPackage {
  std::filesystem::path root;
  std::filesystem::path manifest;
  std::vector<std::filesystem::path> sources;
  std::vector<std::string> module_identities;
  std::vector<MoveEmission> emissions;
  std::vector<LocalPackageDependency> compiler_dependencies;
  std::vector<std::string> unresolved_external_modules;

  [[nodiscard]] bool readyForCompilerAttempt() const noexcept {
    return !module_identities.empty() && module_identities.size() == emissions.size() && unresolved_external_modules.empty() &&
           std::all_of(emissions.begin(), emissions.end(),
                       [](const auto &emission) { return emission.allControlFlowComplete() && emission.allSourceSemanticsComplete(); });
  }
};

// Creates a new minimal Move package containing decompiled source. The output
// directory must not already exist; this function never overwrites an existing
// file or directory. External dependencies are reported but are not invented in
// Move.toml.
[[nodiscard]] RoundTripPackage prepareRoundTripPackage(const Module &module, const std::filesystem::path &output_directory);

struct AptosCompilerOptions {
  std::filesystem::path executable = "aptos";
  std::size_t max_output_bytes = 1024U * 1024U;
  std::size_t max_harness_bytes = 4U * 1024U * 1024U;
  std::chrono::milliseconds timeout = std::chrono::minutes(2);
  // Aptos CLI compiler compatibility controls. When omitted, the installed
  // CLI defaults are used.
  std::optional<std::uint32_t> bytecode_version;
  std::optional<std::string> language_version;
  bool dump_storage_on_test_failure = true;
};

struct BehaviorCaseObservation {
  std::string name;
  bool passed = false;

  friend bool operator==(const BehaviorCaseObservation &, const BehaviorCaseObservation &) = default;
};

struct BehaviorAbortObservation {
  std::uint64_t code = 0;
  std::string module;

  friend bool operator==(const BehaviorAbortObservation &, const BehaviorAbortObservation &) = default;
};

struct BehavioralTrace {
  std::vector<BehaviorCaseObservation> cases;
  std::vector<BehaviorAbortObservation> aborts;
  std::vector<std::string> storage_snapshots;
  bool summary_seen = false;

  [[nodiscard]] bool allPassed() const noexcept {
    return summary_seen && !cases.empty() && std::all_of(cases.begin(), cases.end(), [](const auto &test) { return test.passed; });
  }

  friend bool operator==(const BehavioralTrace &, const BehavioralTrace &) = default;
};

// Parses the stable, human-readable Move unit-test result lines and any
// `--dump` storage sections. Build chatter and package paths are ignored.
[[nodiscard]] BehavioralTrace parseBehavioralTrace(std::string_view output);

struct CompilerAttempt {
  int exit_code = -1;
  bool terminated_by_signal = false;
  bool timed_out = false;
  std::filesystem::path log;
  std::string output;
  bool output_truncated = false;
  std::vector<std::filesystem::path> bytecode_modules;

  [[nodiscard]] bool succeeded() const noexcept { return exit_code == 0 && !terminated_by_signal && !timed_out; }

  [[nodiscard]] bool completed() const noexcept { return exit_code >= 0 && !terminated_by_signal && !timed_out; }
};

struct RoundTripResult {
  RoundTripPackage package;
  std::optional<CompilerAttempt> compiler;
  std::optional<std::filesystem::path> candidate_module;
  std::optional<ModuleInterfaceComparison> interface_comparison;
  std::optional<ModuleBodyComparison> body_comparison;

  [[nodiscard]] bool interfaceEquivalent() const noexcept {
    return compiler.has_value() && compiler->succeeded() && candidate_module.has_value() && interface_comparison.has_value() &&
           interface_comparison->equivalent();
  }

  [[nodiscard]] bool bodyEquivalent() const noexcept { return interfaceEquivalent() && body_comparison.has_value() && body_comparison->equivalent(); }
};

struct RoundTripCorpusEntry {
  std::filesystem::path input;
  RoundTripResult result;
};

struct RoundTripCorpusResult {
  std::filesystem::path root;
  std::vector<RoundTripCorpusEntry> entries;

  [[nodiscard]] bool allInterfaceEquivalent() const noexcept {
    return !entries.empty() && std::all_of(entries.begin(), entries.end(), [](const auto &entry) { return entry.result.interfaceEquivalent(); });
  }
};

struct MultiModuleRoundTripEntry {
  std::string identity;
  std::optional<std::filesystem::path> candidate_module;
  std::optional<ModuleInterfaceComparison> interface_comparison;
  std::optional<ModuleBodyComparison> body_comparison;

  [[nodiscard]] bool interfaceEquivalent() const noexcept {
    return candidate_module.has_value() && interface_comparison.has_value() && interface_comparison->equivalent();
  }
};

struct MultiModuleRoundTripResult {
  MultiModuleRoundTripPackage package;
  std::optional<CompilerAttempt> compiler;
  std::vector<MultiModuleRoundTripEntry> modules;

  [[nodiscard]] bool allInterfacesEquivalent() const noexcept {
    return compiler.has_value() && compiler->succeeded() && !modules.empty() &&
           std::all_of(modules.begin(), modules.end(), [](const auto &module) { return module.interfaceEquivalent(); });
  }
};

struct BehavioralTestComparison {
  std::filesystem::path root;
  std::vector<std::filesystem::path> shared_harnesses;
  CompilerAttempt reference;
  CompilerAttempt candidate;
  BehavioralTrace reference_trace;
  BehavioralTrace candidate_trace;

  [[nodiscard]] bool observationsEquivalent() const noexcept {
    return reference_trace.summary_seen && candidate_trace.summary_seen && reference_trace == candidate_trace;
  }

  // This is finite evidence for the shared test cases, not a proof over all
  // possible inputs or states.
  [[nodiscard]] bool allCasesPassed() const noexcept {
    return !shared_harnesses.empty() && reference.succeeded() && candidate.succeeded() && observationsEquivalent() && reference_trace.allPassed();
  }

  // Generated probes intentionally fail to expose return bits, abort origins,
  // and failure storage. This accepts either process exit status as long as
  // both VM runs completed and produced identical nonempty structured traces.
  [[nodiscard]] bool allObservedOutcomesEquivalent() const noexcept {
    return !shared_harnesses.empty() && reference.completed() && candidate.completed() && observationsEquivalent() && !reference_trace.cases.empty();
  }
};

// Prepares a package, invokes the Aptos CLI directly (without a shell), finds
// the compiler output with the reference module identity, and compares its
// normalized declaration interface. If preparation reports blockers, the
// compiler is not invoked and `compiler` remains empty.
[[nodiscard]] RoundTripResult runRoundTrip(const Module &module, const std::filesystem::path &output_directory, const AptosCompilerOptions &options = {});

// Recursively discovers .mv files, validates all inputs before creating any
// output, and runs a separate real compiler package for each module. Inputs and
// result packages are ordered deterministically.
[[nodiscard]] RoundTripCorpusResult runRoundTripCorpus(const std::filesystem::path &input_directory, const std::filesystem::path &output_directory,
                                                       const AptosCompilerOptions &options = {});

// Emits all supplied modules into one deterministic package. External module
// handles are considered resolved only when a supplied module has the exact
// qualified identity. Validation and emission finish before output is created.
[[nodiscard]] MultiModuleRoundTripPackage prepareMultiModuleRoundTripPackage(const std::vector<Module> &modules, const std::filesystem::path &output_directory);

// External provider modules participate in exact identity/ABI resolution but
// are not emitted as source. Their authoritative local packages are retained
// as Move.toml compiler dependencies, which permits framework/native modules
// to remain external to the recovered package.
[[nodiscard]] MultiModuleRoundTripPackage prepareMultiModuleRoundTripPackage(const std::vector<Module> &modules,
                                                                             const std::vector<Module> &external_provider_modules,
                                                                             const std::vector<LocalPackageDependency> &compiler_dependencies,
                                                                             const std::filesystem::path &output_directory);

// Loads a raw .mv tree or a compiled package root. Package roots recursively
// import local Move.toml dependencies. One dependency-aware package is compiled
// and every supplied module is compared with its matching compiler output.
[[nodiscard]] MultiModuleRoundTripResult runRoundTripPackage(const std::filesystem::path &input_directory, const std::filesystem::path &output_directory,
                                                             const AptosCompilerOptions &options = {});

// The first root is the primary input and the remaining roots are explicit,
// local dependency search paths. Each may be a raw bytecode tree or compiled
// package root. All discovered modules participate in the same identity/ABI
// validation and compiler package; no network access occurs.
[[nodiscard]] MultiModuleRoundTripResult runRoundTripPackage(const std::vector<std::filesystem::path> &input_directories,
                                                             const std::filesystem::path &output_directory, const AptosCompilerOptions &options = {});

// External package roots must contain a Move.toml and compiled bytecode. Their
// modules verify primary references, while the package itself is passed to the
// official compiler instead of being decompiled into local source.
[[nodiscard]] MultiModuleRoundTripResult runRoundTripPackage(const std::vector<std::filesystem::path> &input_directories,
                                                             const std::vector<std::filesystem::path> &external_package_directories,
                                                             const std::filesystem::path &output_directory, const AptosCompilerOptions &options = {});

// Requires byte-for-byte identical nonempty `tests/*.move` harnesses in both
// packages, then runs each package through the official Aptos Move unit-test
// VM. The output directory must not exist and stores bounded logs.
[[nodiscard]] BehavioralTestComparison runBehavioralTests(const std::filesystem::path &reference_package, const std::filesystem::path &candidate_package,
                                                          const std::filesystem::path &output_directory, const AptosCompilerOptions &options = {});

} // namespace movescape
