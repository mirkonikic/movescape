#include "test.hpp"

#include "movescape/format.hpp"
#include "movescape/module_loader.hpp"
#include "movescape/round_trip.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

class TemporaryPackagePath {
public:
  TemporaryPackagePath() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("movescape-round-trip-test-" + std::to_string(nonce));
  }

  TemporaryPackagePath(const TemporaryPackagePath &) = delete;
  TemporaryPackagePath &operator=(const TemporaryPackagePath &) = delete;

  ~TemporaryPackagePath() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void writeText(const std::filesystem::path &path, const std::string &text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

using Bytes = std::vector<std::uint8_t>;

void appendUleb(Bytes &bytes, std::uint64_t value) {
  do {
    auto byte = static_cast<std::uint8_t>(value & 0x7fU);
    value >>= 7U;
    if (value != 0) {
      byte = static_cast<std::uint8_t>(byte | 0x80U);
    }
    bytes.push_back(byte);
  } while (value != 0);
}

Bytes serializedCompilableModule(std::uint8_t visibility = 0x01) {
  using movescape::format::TableKind;
  const Bytes identifiers{0x01, 'M', 0x01, 'f'};
  Bytes addresses(32, 0);
  addresses.back() = 1;
  const Bytes module_handles{0x00, 0x00};
  const Bytes signatures{0x00};
  const Bytes function_handles{0x00, 0x01, 0x00, 0x00, 0x00};
  const Bytes function_definitions{0x00, visibility, 0x00, 0x00, 0x00, 0x01, 0x02};
  const std::vector<std::pair<TableKind, Bytes>> tables{
      {TableKind::Identifiers, identifiers}, {TableKind::AddressIdentifiers, addresses},     {TableKind::ModuleHandles, module_handles},
      {TableKind::Signatures, signatures},   {TableKind::FunctionHandles, function_handles}, {TableKind::FunctionDefinitions, function_definitions},
  };

  Bytes bytes{0xa1, 0x1c, 0xeb, 0x0b, 0x05, 0x00, 0x00, 0x00};
  appendUleb(bytes, tables.size());
  std::uint64_t offset = 0;
  for (const auto &[kind, contents] : tables) {
    bytes.push_back(static_cast<std::uint8_t>(kind));
    appendUleb(bytes, offset);
    appendUleb(bytes, contents.size());
    offset += contents.size();
  }
  for (const auto &[kind, contents] : tables) {
    (void)kind;
    bytes.insert(bytes.end(), contents.begin(), contents.end());
  }
  appendUleb(bytes, 0);
  return bytes;
}

void writeBytes(const std::filesystem::path &path, const Bytes &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

movescape::Module compilableModule() {
  movescape::Module module;
  module.version = 10;
  module.identifiers = {"M", "f"};
  movescape::Address address{};
  address.back() = 1;
  module.addresses.push_back(address);
  module.module_handles.push_back({.address = 0, .name = 0});
  module.self_module_handle = 0;
  module.signatures.push_back({});
  module.function_handles.push_back({
      .module = 0,
      .name = 1,
      .parameters = 0,
      .returns = 0,
  });
  movescape::CodeUnit code;
  code.locals = 0;
  code.code.push_back({.opcode = movescape::Opcode::Ret});
  module.function_definitions.push_back({
      .handle = 0,
      .visibility = movescape::Visibility::Public,
      .code = code,
  });
  return module;
}

movescape::Module moduleWithDependency(bool include_dependency_handle) {
  auto module = compilableModule();
  module.identifiers[0] = "Dependency";
  module.addresses[0].back() = 2;
  if (include_dependency_handle) {
    module.identifiers.push_back("Leaf");
    movescape::Address leaf_address{};
    leaf_address.back() = 3;
    module.addresses.push_back(leaf_address);
    module.module_handles.push_back({.address = 1, .name = 2});
  }
  return module;
}

} // namespace

TEST(round_trip_preparation_writes_a_minimal_non_overwriting_move_package) {
  TemporaryPackagePath temporary;
  REQUIRE(!std::filesystem::exists(temporary.path()));

  const auto package = movescape::prepareRoundTripPackage(compilableModule(), temporary.path());
  REQUIRE_EQ(package.root, temporary.path());
  REQUIRE_EQ(package.manifest, temporary.path() / "Move.toml");
  REQUIRE_EQ(package.source, temporary.path() / "sources" / "M.move");
  REQUIRE(package.readyForCompilerAttempt());
  REQUIRE(package.external_modules.empty());
  REQUIRE(std::filesystem::is_regular_file(package.manifest));
  REQUIRE(std::filesystem::is_regular_file(package.source));
  REQUIRE_EQ(readText(package.manifest), std::string("[package]\n"
                                                     "name = \"MovescapeRoundTrip\"\n"
                                                     "version = \"0.0.0\"\n"));
  const auto source = readText(package.source);
  REQUIRE(source.find("module 0x1::M") != std::string::npos);
  REQUIRE(source.find("public fun f()") != std::string::npos);
}

TEST(round_trip_preparation_refuses_to_overwrite_an_existing_path) {
  TemporaryPackagePath temporary;
  const auto first = movescape::prepareRoundTripPackage(compilableModule(), temporary.path());
  REQUIRE(std::filesystem::exists(first.root));
  REQUIRE_ERROR(movescape::prepareRoundTripPackage(compilableModule(), temporary.path()), movescape::ErrorCode::InvalidArgument);
  REQUIRE(std::filesystem::is_regular_file(first.source));
}

TEST(round_trip_preparation_reports_external_dependency_modules) {
  auto module = compilableModule();
  module.identifiers.push_back("Dependency");
  movescape::Address dependency_address{};
  dependency_address.back() = 2;
  module.addresses.push_back(dependency_address);
  module.module_handles.push_back({.address = 1, .name = 2});

  TemporaryPackagePath temporary;
  const auto package = movescape::prepareRoundTripPackage(module, temporary.path());
  REQUIRE(!package.readyForCompilerAttempt());
  REQUIRE_EQ(package.external_modules, (std::vector<std::string>{"0x2::Dependency"}));
}

TEST(multi_module_preparation_resolves_supplied_dependencies_deterministically) {
  auto consumer = compilableModule();
  consumer.identifiers.push_back("Dependency");
  movescape::Address dependency_address{};
  dependency_address.back() = 2;
  consumer.addresses.push_back(dependency_address);
  consumer.module_handles.push_back({.address = 1, .name = 2});
  consumer.function_handles.push_back({.module = 1, .name = 1, .parameters = 0, .returns = 0});
  const auto dependency = moduleWithDependency(false);
  TemporaryPackagePath temporary;

  const auto package = movescape::prepareMultiModuleRoundTripPackage({dependency, consumer}, temporary.path());
  REQUIRE(package.readyForCompilerAttempt());
  REQUIRE(package.unresolved_external_modules.empty());
  REQUIRE_EQ(package.module_identities, (std::vector<std::string>{"0x1::M", "0x2::Dependency"}));
  REQUIRE_EQ(package.sources.size(), 2U);
  REQUIRE(std::filesystem::is_regular_file(package.sources[0]));
  REQUIRE(std::filesystem::is_regular_file(package.sources[1]));
  REQUIRE(readText(package.sources[0]).find("module 0x1::M") != std::string::npos);
  REQUIRE(readText(package.sources[1]).find("module 0x2::Dependency") != std::string::npos);
  REQUIRE(readText(package.sources[0]).find("use 0x2::Dependency;") != std::string::npos);
}

TEST(multi_module_preparation_reports_transitive_missing_dependencies) {
  const auto dependency = moduleWithDependency(true);
  TemporaryPackagePath temporary;

  const auto package = movescape::prepareMultiModuleRoundTripPackage({dependency}, temporary.path());
  REQUIRE(!package.readyForCompilerAttempt());
  REQUIRE_EQ(package.unresolved_external_modules, (std::vector<std::string>{"0x3::Leaf"}));
}

TEST(multi_module_preparation_rejects_a_supplied_dependency_abi_mismatch) {
  auto consumer = compilableModule();
  consumer.identifiers.push_back("Dependency");
  movescape::Address dependency_address{};
  dependency_address.back() = 2;
  consumer.addresses.push_back(dependency_address);
  consumer.module_handles.push_back({.address = 1, .name = 2});
  consumer.function_handles.push_back({.module = 1, .name = 1, .parameters = 0, .returns = 0});
  auto dependency = moduleWithDependency(false);
  dependency.signatures.push_back({movescape::Type{.kind = movescape::TypeKind::Bool}});
  dependency.function_handles[0].parameters = 1;
  TemporaryPackagePath temporary;

  REQUIRE_ERROR(movescape::prepareMultiModuleRoundTripPackage({consumer, dependency}, temporary.path()), movescape::ErrorCode::TypeMismatch);
  REQUIRE(!std::filesystem::exists(temporary.path()));
}

TEST(multi_module_preparation_keeps_native_provider_packages_external) {
  auto consumer = compilableModule();
  consumer.identifiers.push_back("Dependency");
  movescape::Address dependency_address{};
  dependency_address.back() = 2;
  consumer.addresses.push_back(dependency_address);
  consumer.module_handles.push_back({.address = 1, .name = 2});
  consumer.function_handles.push_back({.module = 1, .name = 1, .parameters = 0, .returns = 0});
  auto dependency = moduleWithDependency(false);
  dependency.function_definitions[0].code = std::nullopt;

  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto external = temporary.path() / "external";
  REQUIRE(std::filesystem::create_directory(external));
  writeText(external / "Move.toml", "[package]\nname = 'NativeProvider'\nversion = '0.0.0'\n");
  const auto output = temporary.path() / "output";

  const auto package = movescape::prepareMultiModuleRoundTripPackage({consumer}, {dependency}, {{.name = "NativeProvider", .root = external}}, output);
  REQUIRE(package.readyForCompilerAttempt());
  REQUIRE_EQ(package.module_identities, (std::vector<std::string>{"0x1::M"}));
  REQUIRE_EQ(package.sources.size(), 1U);
  REQUIRE_EQ(package.compiler_dependencies.size(), 1U);
  REQUIRE_EQ(package.compiler_dependencies[0].name, std::string("NativeProvider"));
  REQUIRE(readText(package.sources[0]).find("module 0x1::M") != std::string::npos);
  REQUIRE(readText(package.sources[0]).find("module 0x2::Dependency") == std::string::npos);
  const auto manifest = readText(package.manifest);
  REQUIRE(manifest.find("[dependencies]") != std::string::npos);
  REQUIRE(manifest.find("\"NativeProvider\" = { local = ") != std::string::npos);
}

TEST(multi_module_preparation_rejects_duplicates_before_writing_output) {
  const auto module = compilableModule();
  TemporaryPackagePath temporary;

  REQUIRE_ERROR(movescape::prepareMultiModuleRoundTripPackage({module, module}, temporary.path()), movescape::ErrorCode::InvalidArgument);
  REQUIRE(!std::filesystem::exists(temporary.path()));
}

TEST(round_trip_preparation_validates_before_creating_output) {
  auto module = compilableModule();
  module.self_module_handle = 99;
  TemporaryPackagePath temporary;

  REQUIRE_ERROR(movescape::prepareRoundTripPackage(module, temporary.path()), movescape::ErrorCode::InvalidIndex);
  REQUIRE(!std::filesystem::exists(temporary.path()));
}

TEST(round_trip_preparation_does_not_claim_abi_renames_are_complete) {
  auto module = compilableModule();
  module.version = 10;
  module.identifiers[0] = "$M";
  TemporaryPackagePath temporary;

  REQUIRE_ERROR(movescape::prepareRoundTripPackage(module, temporary.path()), movescape::ErrorCode::UnsupportedFeature);
  REQUIRE(!std::filesystem::exists(temporary.path()));
}

TEST(round_trip_runner_invokes_compiler_and_compares_emitted_module) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto bytes = serializedCompilableModule();
  const auto candidate = temporary.path() / "candidate.mv";
  writeBytes(candidate, bytes);

  const auto compiler = temporary.path() / "fake-aptos";
  writeText(compiler, "#!/bin/sh\n"
                      "if [ \"$1\" != move ] || [ \"$2\" != compile ] || "
                      "[ \"$3\" != --package-dir ]; then exit 64; fi\n"
                      "mkdir -p \"$4/build/Test/bytecode_modules\"\n"
                      "cp '" +
                          candidate.string() +
                          "' \"$4/build/Test/bytecode_modules/M.mv\"\n"
                          "echo fake compiler completed \"$@\"\n");
  std::filesystem::permissions(compiler, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

  const auto module = movescape::loadModule(bytes);
  const auto result = movescape::runRoundTrip(module, temporary.path() / "package", movescape::AptosCompilerOptions{.executable = compiler});
  REQUIRE(result.compiler.has_value());
  REQUIRE(result.compiler->succeeded());
  REQUIRE_EQ(result.compiler->exit_code, 0);
  REQUIRE(!result.compiler->terminated_by_signal);
  REQUIRE(!result.compiler->timed_out);
  REQUIRE(!result.compiler->output_truncated);
  REQUIRE(result.compiler->output.find("fake compiler completed") != std::string::npos);
  REQUIRE(result.compiler->output.find("--bytecode-version 5") != std::string::npos);
  REQUIRE(result.compiler->output.find("--language-version 2.0") != std::string::npos);
  REQUIRE_EQ(result.compiler->bytecode_modules.size(), 1U);
  REQUIRE(result.candidate_module.has_value());
  REQUIRE(result.interface_comparison.has_value());
  REQUIRE(result.body_comparison.has_value());
  REQUIRE(result.interfaceEquivalent());
  REQUIRE(result.bodyEquivalent());
}

TEST(round_trip_runner_rejects_compiler_versions_below_source_policy) {
  TemporaryPackagePath temporary;
  auto module = compilableModule();
  module.version = 10;
  const auto output = temporary.path();

  REQUIRE_ERROR(movescape::runRoundTrip(module, output, movescape::AptosCompilerOptions{.bytecode_version = 9, .language_version = "2.0"}),
                movescape::ErrorCode::InvalidArgument);
  REQUIRE(!std::filesystem::exists(output));

  module.function_handles[0].attributes.push_back({.kind = movescape::FunctionAttributeKind::Persistent});
  const auto language_output = temporary.path() / "language-output";
  REQUIRE_ERROR(movescape::runRoundTrip(module, language_output, movescape::AptosCompilerOptions{.bytecode_version = 10, .language_version = "2.1"}),
                movescape::ErrorCode::InvalidArgument);
  REQUIRE(!std::filesystem::exists(language_output));
}

TEST(round_trip_corpus_runs_every_module_in_deterministic_order) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto input = temporary.path() / "input";
  REQUIRE(std::filesystem::create_directory(input));
  const auto bytes = serializedCompilableModule();
  writeBytes(input / "b.mv", bytes);
  writeBytes(input / "a.mv", bytes);

  const auto candidate = temporary.path() / "candidate.mv";
  writeBytes(candidate, bytes);
  const auto compiler = temporary.path() / "fake-aptos";
  writeText(compiler, "#!/bin/sh\n"
                      "mkdir -p \"$4/build/Test/bytecode_modules\"\n"
                      "cp '" +
                          candidate.string() + "' \"$4/build/Test/bytecode_modules/M.mv\"\n");
  std::filesystem::permissions(compiler, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

  const auto corpus = movescape::runRoundTripCorpus(input, temporary.path() / "output", movescape::AptosCompilerOptions{.executable = compiler});
  REQUIRE_EQ(corpus.entries.size(), 2U);
  REQUIRE_EQ(corpus.entries[0].input.filename(), std::filesystem::path("a.mv"));
  REQUIRE_EQ(corpus.entries[1].input.filename(), std::filesystem::path("b.mv"));
  REQUIRE(corpus.entries[0].result.interfaceEquivalent());
  REQUIRE(corpus.entries[1].result.interfaceEquivalent());
  REQUIRE(corpus.allInterfaceEquivalent());
}

TEST(round_trip_corpus_rejects_an_empty_input_before_creating_output) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto input = temporary.path() / "input";
  REQUIRE(std::filesystem::create_directory(input));
  const auto output = temporary.path() / "output";

  REQUIRE_ERROR(movescape::runRoundTripCorpus(input, output), movescape::ErrorCode::InvalidArgument);
  REQUIRE(!std::filesystem::exists(output));
}

TEST(multi_module_round_trip_requires_a_primary_or_dependency_tree) {
  TemporaryPackagePath temporary;
  REQUIRE_ERROR(movescape::runRoundTripPackage(std::vector<std::filesystem::path>{}, temporary.path()), movescape::ErrorCode::InvalidArgument);
  REQUIRE(!std::filesystem::exists(temporary.path()));
}

TEST(multi_module_round_trip_deduplicates_identical_dependency_artifacts) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto primary = temporary.path() / "primary";
  const auto dependency = temporary.path() / "dependency";
  REQUIRE(std::filesystem::create_directory(primary));
  REQUIRE(std::filesystem::create_directory(dependency));
  const auto bytes = serializedCompilableModule();
  writeBytes(primary / "M.mv", bytes);
  writeBytes(dependency / "M.mv", bytes);

  const auto candidate = temporary.path() / "candidate.mv";
  writeBytes(candidate, bytes);
  const auto compiler = temporary.path() / "fake-aptos";
  writeText(compiler, "#!/bin/sh\n"
                      "mkdir -p \"$4/build/Test/bytecode_modules\"\n"
                      "cp '" +
                          candidate.string() + "' \"$4/build/Test/bytecode_modules/M.mv\"\n");
  std::filesystem::permissions(compiler, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

  const auto result = movescape::runRoundTripPackage({primary, dependency}, temporary.path() / "output", movescape::AptosCompilerOptions{.executable = compiler});
  REQUIRE_EQ(result.package.module_identities.size(), 1U);
  REQUIRE(result.allInterfacesEquivalent());
}

TEST(multi_module_round_trip_rejects_conflicting_dependency_artifacts) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto primary = temporary.path() / "primary";
  const auto dependency = temporary.path() / "dependency";
  REQUIRE(std::filesystem::create_directory(primary));
  REQUIRE(std::filesystem::create_directory(dependency));
  writeBytes(primary / "M.mv", serializedCompilableModule());
  writeBytes(dependency / "M.mv", serializedCompilableModule(0x00));
  const auto output = temporary.path() / "output";

  REQUIRE_ERROR(movescape::runRoundTripPackage({primary, dependency}, output), movescape::ErrorCode::InvalidArgument);
  REQUIRE(!std::filesystem::exists(output));
}

TEST(round_trip_runner_reports_a_missing_compiler_without_claiming_success) {
  TemporaryPackagePath temporary;
  const auto result =
      movescape::runRoundTrip(compilableModule(), temporary.path(), movescape::AptosCompilerOptions{.executable = temporary.path() / "compiler-does-not-exist"});

  REQUIRE(result.compiler.has_value());
  REQUIRE(!result.compiler->succeeded());
  REQUIRE_EQ(result.compiler->exit_code, 127);
  REQUIRE(!result.candidate_module.has_value());
  REQUIRE(!result.interface_comparison.has_value());
  REQUIRE(!result.interfaceEquivalent());
}

TEST(round_trip_runner_rejects_a_successfully_compiled_interface_change) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto reference_bytes = serializedCompilableModule();
  const auto changed_candidate = temporary.path() / "changed.mv";
  writeBytes(changed_candidate, serializedCompilableModule(0x00));

  const auto compiler = temporary.path() / "fake-aptos";
  writeText(compiler, "#!/bin/sh\n"
                      "mkdir -p \"$4/build/Test/bytecode_modules\"\n"
                      "cp '" +
                          changed_candidate.string() + "' \"$4/build/Test/bytecode_modules/M.mv\"\n");
  std::filesystem::permissions(compiler, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

  const auto result =
      movescape::runRoundTrip(movescape::loadModule(reference_bytes), temporary.path() / "package", movescape::AptosCompilerOptions{.executable = compiler});
  REQUIRE(result.compiler.has_value());
  REQUIRE(result.compiler->succeeded());
  REQUIRE(result.candidate_module.has_value());
  REQUIRE(result.interface_comparison.has_value());
  REQUIRE(!result.interface_comparison->equivalent());
  REQUIRE_EQ(result.interface_comparison->differences.size(), 1U);
  REQUIRE_EQ(result.interface_comparison->differences[0].identity, std::string("function 0x1::M::f"));
  REQUIRE(!result.interfaceEquivalent());
}

TEST(round_trip_runner_skips_compiler_when_dependencies_are_unresolved) {
  auto module = compilableModule();
  module.identifiers.push_back("Dependency");
  movescape::Address dependency_address{};
  dependency_address.back() = 2;
  module.addresses.push_back(dependency_address);
  module.module_handles.push_back({.address = 1, .name = 2});

  TemporaryPackagePath temporary;
  const auto result =
      movescape::runRoundTrip(module, temporary.path(), movescape::AptosCompilerOptions{.executable = temporary.path() / "compiler-does-not-exist"});
  REQUIRE(!result.compiler.has_value());
  REQUIRE(!result.interfaceEquivalent());
}

TEST(round_trip_runner_times_out_and_kills_the_compiler_process_group) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto compiler = temporary.path() / "slow-aptos";
  writeText(compiler, "#!/bin/sh\nsleep 5\n");
  std::filesystem::permissions(compiler, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

  const auto result = movescape::runRoundTrip(compilableModule(), temporary.path() / "package",
                                             movescape::AptosCompilerOptions{
                                                 .executable = compiler,
                                                 .timeout = std::chrono::milliseconds(50),
                                             });
  REQUIRE(result.compiler.has_value());
  REQUIRE(result.compiler->timed_out);
  REQUIRE(result.compiler->terminated_by_signal);
  REQUIRE(!result.compiler->succeeded());
  REQUIRE(!result.interfaceEquivalent());
}

TEST(behavioral_runner_executes_identical_harnesses_against_both_packages) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto reference = temporary.path() / "reference";
  const auto candidate = temporary.path() / "candidate";
  REQUIRE(std::filesystem::create_directories(reference / "tests"));
  REQUIRE(std::filesystem::create_directories(candidate / "tests"));
  writeText(reference / "Move.toml", "[package]\nname='Reference'\n");
  writeText(candidate / "Move.toml", "[package]\nname='Candidate'\n");
  constexpr auto harness = "#[test] fun same_case() {}\n";
  writeText(reference / "tests" / "Behavior.move", harness);
  writeText(candidate / "tests" / "Behavior.move", harness);

  const auto aptos = temporary.path() / "fake-aptos";
  writeText(aptos, "#!/bin/sh\n"
                   "if [ \"$1\" != move ] || [ \"$2\" != test ] || "
                   "[ \"$3\" != --package-dir ]; then exit 64; fi\n"
                   "echo '[ PASS    ] 0x1::Behavior::same_case'\n"
                   "echo 'Test result: OK. Total tests: 1; passed: 1; failed: 0'\n");
  std::filesystem::permissions(aptos, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

  const auto comparison = movescape::runBehavioralTests(reference, candidate, temporary.path() / "results", movescape::AptosCompilerOptions{.executable = aptos});
  REQUIRE(comparison.allCasesPassed());
  REQUIRE_EQ(comparison.shared_harnesses, (std::vector<std::filesystem::path>{"Behavior.move"}));
  REQUIRE(comparison.reference.output.find("Behavior::same_case") != std::string::npos);
  REQUIRE(comparison.candidate.output.find("Behavior::same_case") != std::string::npos);
  REQUIRE(comparison.observationsEquivalent());
  REQUIRE_EQ(comparison.reference_trace.cases.size(), 1U);
  REQUIRE(std::filesystem::is_regular_file(comparison.reference.log));
  REQUIRE(std::filesystem::is_regular_file(comparison.candidate.log));
}

TEST(behavioral_trace_retains_case_outcomes_and_failure_storage) {
  const auto trace = movescape::parseBehavioralTrace("[ PASS    ] 0x1::Tests::ok\n"
                                                    "[ FAIL    ] 0x1::Tests::snapshot\n"
                                                    "Test was not expected to error, but it aborted with code 99 "
                                                    "originating in the module "
                                                    "0000000000000000000000000000000000000000000000000000000000000001::Tests "
                                                    "rooted here\n"
                                                    "│ ────── Storage state at point of failure ──────\n"
                                                    "│ 0xcafe::M::R { value: 42 }\n"
                                                    "└──────────────────\n"
                                                    "Test result: FAILED. Total tests: 2; passed: 1; failed: 1\n");
  REQUIRE(trace.summary_seen);
  REQUIRE_EQ(trace.cases.size(), 2U);
  REQUIRE(trace.cases[0].passed);
  REQUIRE(!trace.cases[1].passed);
  REQUIRE_EQ(trace.aborts.size(), 1U);
  REQUIRE_EQ(trace.aborts[0].code, 99U);
  REQUIRE_EQ(trace.aborts[0].module, std::string("000000000000000000000000000000000000000000000000"
                                                 "0000000000000001::Tests"));
  REQUIRE_EQ(trace.storage_snapshots, (std::vector<std::string>{"0xcafe::M::R { value: 42 }"}));
  REQUIRE(!trace.allPassed());
}

TEST(behavioral_runner_compares_intentional_abort_and_storage_outcomes) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto reference = temporary.path() / "reference";
  const auto candidate = temporary.path() / "candidate";
  REQUIRE(std::filesystem::create_directories(reference / "tests"));
  REQUIRE(std::filesystem::create_directories(candidate / "tests"));
  writeText(reference / "Move.toml", "[package]\nname='Reference'\n");
  writeText(candidate / "Move.toml", "[package]\nname='Candidate'\n");
  constexpr auto harness = "#[test] fun observed_failure() { abort 99 }\n";
  writeText(reference / "tests" / "Behavior.move", harness);
  writeText(candidate / "tests" / "Behavior.move", harness);

  const auto aptos = temporary.path() / "fake-aptos";
  writeText(aptos, "#!/bin/sh\n"
                   "echo '[ FAIL    ] 0x1::Tests::observed_failure'\n"
                   "echo 'aborted with code 99 originating in the module "
                   "0000000000000000000000000000000000000000000000000000000000000001::Tests "
                   "rooted here'\n"
                   "echo '│ ────── Storage state at point of failure ──────'\n"
                   "echo '│ 0xcafe::M::R { value: 42 }'\n"
                   "echo '└──────────────────'\n"
                   "echo 'Test result: FAILED. Total tests: 1; passed: 0; failed: 1'\n"
                   "exit 1\n");
  std::filesystem::permissions(aptos, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

  const auto comparison = movescape::runBehavioralTests(reference, candidate, temporary.path() / "results", movescape::AptosCompilerOptions{.executable = aptos});
  REQUIRE(!comparison.allCasesPassed());
  REQUIRE(comparison.allObservedOutcomesEquivalent());
  REQUIRE(comparison.observationsEquivalent());
  REQUIRE_EQ(comparison.reference_trace.aborts.size(), 1U);
  REQUIRE_EQ(comparison.reference_trace.storage_snapshots.size(), 1U);
}

TEST(behavioral_runner_rejects_different_harnesses_before_creating_output) {
  TemporaryPackagePath temporary;
  REQUIRE(std::filesystem::create_directory(temporary.path()));
  const auto reference = temporary.path() / "reference";
  const auto candidate = temporary.path() / "candidate";
  REQUIRE(std::filesystem::create_directories(reference / "tests"));
  REQUIRE(std::filesystem::create_directories(candidate / "tests"));
  writeText(reference / "Move.toml", "[package]\nname='Reference'\n");
  writeText(candidate / "Move.toml", "[package]\nname='Candidate'\n");
  writeText(reference / "tests" / "Behavior.move", "reference\n");
  writeText(candidate / "tests" / "Behavior.move", "candidate\n");
  const auto output = temporary.path() / "results";

  REQUIRE_ERROR(movescape::runBehavioralTests(reference, candidate, output), movescape::ErrorCode::InvalidArgument);
  REQUIRE(!std::filesystem::exists(output));
}
