#include "movescape/round_trip.hpp"

#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"
#include "movescape/loader.hpp"
#include "movescape/module_compare.hpp"
#include "movescape/module_loader.hpp"
#include "movescape/package_loader.hpp"
#include "movescape/semantic.hpp"
#include "movescape/validator.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <compare>
#include <csignal>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace movescape {

namespace {

[[noreturn]] void fail(ErrorCode code, std::string message) { throw Error(code, Error::UnknownOffset, std::move(message)); }

void writeFile(const std::filesystem::path &path, const std::string &contents) {
  if (contents.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    fail(ErrorCode::IntegerOverflow, "round-trip output is too large to write: " + path.string());
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    fail(ErrorCode::Io, "unable to create round-trip file: " + path.string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    fail(ErrorCode::Io, "unable to write round-trip file: " + path.string());
  }
}

[[nodiscard]] std::string tomlBasicString(std::string_view value) {
  std::string result{"\""};
  result.reserve(value.size() + 2U);
  for (const auto character : value) {
    switch (character) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20U) {
        fail(ErrorCode::InvalidArgument, "compiler dependency contains a control character");
      }
      result.push_back(character);
      break;
    }
  }
  result.push_back('"');
  return result;
}

struct LanguageVersion {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;

  friend auto operator<=>(const LanguageVersion &, const LanguageVersion &) = default;
};

[[nodiscard]] LanguageVersion parseLanguageVersion(std::string_view text) {
  if (text.empty()) {
    fail(ErrorCode::InvalidArgument, "language version must not be empty");
  }
  const auto dot = text.find('.');
  const auto major_text = text.substr(0, dot);
  const auto minor_text = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1U);
  if (major_text.empty()) {
    fail(ErrorCode::InvalidArgument, "language version has an empty major component: " + std::string(text));
  }
  LanguageVersion result;
  const auto parse_part = [&](std::string_view part, std::uint32_t &value) {
    if (part.empty()) {
      value = 0;
      return;
    }
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) {
      fail(ErrorCode::InvalidArgument, "language version is not numeric: " + std::string(text));
    }
  };
  parse_part(major_text, result.major);
  if (dot != std::string_view::npos && minor_text.empty()) {
    fail(ErrorCode::InvalidArgument, "language version has an empty minor component: " + std::string(text));
  }
  parse_part(minor_text, result.minor);
  if (dot != std::string_view::npos && text.find('.', dot + 1U) != std::string_view::npos) {
    fail(ErrorCode::InvalidArgument, "language version has too many components: " + std::string(text));
  }
  return result;
}

[[nodiscard]] AptosCompilerOptions applySourcePolicies(const AptosCompilerOptions &options, const std::vector<MoveSourcePolicy> &policies) {
  if (policies.empty()) {
    fail(ErrorCode::InvalidArgument, "source policy requires at least one emitted module");
  }
  auto effective = options;
  std::uint32_t required_bytecode = 5;
  LanguageVersion required_language;
  std::string required_language_text = "2.0";
  for (const auto &policy : policies) {
    required_bytecode = std::max(required_bytecode, policy.minimum_bytecode_version);
    const auto language = parseLanguageVersion(policy.minimum_language_version);
    if (language > required_language) {
      required_language = language;
      required_language_text = policy.minimum_language_version;
    }
  }
  if (effective.bytecode_version.has_value() && *effective.bytecode_version < required_bytecode) {
    fail(ErrorCode::InvalidArgument, "requested bytecode version " + std::to_string(*effective.bytecode_version) + " is below recovered source requirement " +
                                         std::to_string(required_bytecode));
  }
  if (!effective.bytecode_version.has_value()) {
    effective.bytecode_version = required_bytecode;
  }
  if (effective.language_version.has_value() && parseLanguageVersion(*effective.language_version) < required_language) {
    fail(ErrorCode::InvalidArgument,
         "requested Move language version " + *effective.language_version + " is below recovered source requirement " + required_language_text);
  }
  if (!effective.language_version.has_value()) {
    effective.language_version = required_language_text;
  }
  return effective;
}

[[nodiscard]] std::string readBoundedFile(const std::filesystem::path &path, std::size_t maximum, bool &truncated) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail(ErrorCode::Io, "unable to read compiler log: " + path.string());
  }
  std::string result(maximum, '\0');
  input.read(result.data(), static_cast<std::streamsize>(maximum));
  result.resize(static_cast<std::size_t>(input.gcount()));
  truncated = input.peek() != std::char_traits<char>::eof();
  return result;
}

[[nodiscard]] std::vector<std::filesystem::path> discoverBytecodeModules(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> result;
  const auto build = root / "build";
  std::error_code error;
  if (!std::filesystem::is_directory(build, error) || error) {
    return result;
  }
  std::filesystem::recursive_directory_iterator iterator(build, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    if (error) {
      fail(ErrorCode::Io, "unable to inspect compiler output: " + error.message());
    }
    if (iterator->is_regular_file(error) && !error && iterator->path().extension() == ".mv" &&
        iterator->path().parent_path().filename() == "bytecode_modules") {
      result.push_back(iterator->path());
    }
    iterator.increment(error);
  }
  if (error) {
    fail(ErrorCode::Io, "unable to inspect compiler output: " + error.message());
  }
  std::sort(result.begin(), result.end());
  return result;
}

[[nodiscard]] std::vector<std::filesystem::path> discoverInputModules(const std::filesystem::path &root) {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    fail(ErrorCode::InvalidArgument, "round-trip corpus input is not a readable directory: " + root.string());
  }
  std::vector<std::filesystem::path> result;
  std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    if (error) {
      fail(ErrorCode::Io, "unable to inspect round-trip corpus: " + error.message());
    }
    if (iterator->is_regular_file(error) && !error && iterator->path().extension() == ".mv") {
      result.push_back(iterator->path());
    }
    iterator.increment(error);
  }
  if (error) {
    fail(ErrorCode::Io, "unable to inspect round-trip corpus: " + error.message());
  }
  std::sort(result.begin(), result.end());
  if (result.empty()) {
    fail(ErrorCode::InvalidArgument, "round-trip corpus contains no .mv modules");
  }
  return result;
}

[[nodiscard]] CompilerAttempt invokeAptos(const std::filesystem::path &package_root, const std::filesystem::path &log_path, std::string_view operation,
                                          const AptosCompilerOptions &options, bool discover_modules) {
  if (options.executable.empty()) {
    fail(ErrorCode::InvalidArgument, "Aptos executable must not be empty");
  }
  if (options.max_output_bytes == 0 || options.max_output_bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    fail(ErrorCode::InvalidArgument, "Aptos output limit must fit in streamsize and be nonzero");
  }
  if (options.timeout <= std::chrono::milliseconds::zero()) {
    fail(ErrorCode::InvalidArgument, "Aptos timeout must be greater than zero");
  }

  CompilerAttempt result;
  result.log = log_path;

#if defined(__unix__) || defined(__APPLE__)
  const auto executable = options.executable.string();
  const auto package_path = package_root.string();
  const auto operation_text = std::string(operation);
  const auto log_path_text = result.log.string();
  const auto child = ::fork();
  if (child < 0) {
    fail(ErrorCode::Io, "unable to fork Aptos compiler: " + std::string(std::strerror(errno)));
  }
  if (child == 0) {
    if (::setpgid(0, 0) < 0) {
      ::_exit(126);
    }
    const auto descriptor = ::open(log_path_text.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
      ::_exit(126);
    }
    if (::dup2(descriptor, STDOUT_FILENO) < 0 || ::dup2(descriptor, STDERR_FILENO) < 0) {
      ::_exit(126);
    }
    ::close(descriptor);
    const auto log_limit = static_cast<rlim_t>(options.max_output_bytes + 1U);
    const rlimit limits{.rlim_cur = log_limit, .rlim_max = log_limit};
    if (::setrlimit(RLIMIT_FSIZE, &limits) < 0) {
      ::_exit(126);
    }
    std::vector<std::string> argument_storage{executable, "move", operation_text, "--package-dir", package_path};
    if (options.bytecode_version.has_value()) {
      argument_storage.emplace_back("--bytecode-version");
      argument_storage.push_back(std::to_string(*options.bytecode_version));
    }
    if (options.language_version.has_value()) {
      argument_storage.emplace_back("--language-version");
      argument_storage.push_back(*options.language_version);
    }
    if (operation == "test") {
      argument_storage.emplace_back("--skip-fetch-latest-git-deps");
      if (options.dump_storage_on_test_failure) {
        argument_storage.emplace_back("--dump");
      }
    }
    std::vector<char *> arguments;
    arguments.reserve(argument_storage.size() + 1);
    for (auto &argument : argument_storage) {
      arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);
    ::execvp(executable.c_str(), arguments.data());
    ::_exit(errno == ENOENT ? 127 : 126);
  }

  int status = 0;
  (void)::setpgid(child, child);
  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  while (true) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      break;
    }
    if (waited < 0 && errno == EINTR) {
      continue;
    }
    if (waited < 0) {
      fail(ErrorCode::Io, "unable to wait for Aptos compiler: " + std::string(std::strerror(errno)));
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      (void)::kill(-child, SIGKILL);
      auto killed = ::waitpid(child, &status, 0);
      while (killed < 0 && errno == EINTR) {
        killed = ::waitpid(child, &status, 0);
      }
      if (killed != child) {
        fail(ErrorCode::Io, "unable to reap timed-out Aptos compiler process");
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
    result.terminated_by_signal = true;
  }
#else
  (void)package_root;
  (void)log_path;
  (void)operation;
  (void)discover_modules;
  (void)options;
  fail(ErrorCode::UnsupportedFeature, "Aptos compiler execution is not implemented on this platform");
#endif

  result.output = readBoundedFile(result.log, options.max_output_bytes, result.output_truncated);
  if (result.succeeded() && discover_modules) {
    result.bytecode_modules = discoverBytecodeModules(package_root);
  }
  return result;
}

[[nodiscard]] CompilerAttempt invokeAptosCompiler(const RoundTripPackage &package, const AptosCompilerOptions &options) {
  return invokeAptos(package.root, package.root / "movescape-compiler.log", "compile", options, true);
}

[[nodiscard]] std::vector<std::filesystem::path> discoverTestHarnesses(const std::filesystem::path &package) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(package / "Move.toml", error) || error) {
    fail(ErrorCode::InvalidArgument, "behavioral test package has no readable Move.toml: " + package.string());
  }
  const auto root = package / "tests";
  if (!std::filesystem::is_directory(root, error) || error) {
    fail(ErrorCode::InvalidArgument, "behavioral test package has no readable tests directory: " + package.string());
  }
  std::vector<std::filesystem::path> result;
  std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    if (error) {
      fail(ErrorCode::Io, "unable to inspect behavioral test harness: " + error.message());
    }
    if (iterator->is_regular_file(error) && !error && iterator->path().extension() == ".move") {
      result.push_back(iterator->path().lexically_relative(root));
    }
    iterator.increment(error);
  }
  if (error) {
    fail(ErrorCode::Io, "unable to inspect behavioral test harness: " + error.message());
  }
  std::sort(result.begin(), result.end());
  if (result.empty()) {
    fail(ErrorCode::InvalidArgument, "behavioral test package contains no tests/*.move harnesses: " + package.string());
  }
  return result;
}

void verifySharedHarnesses(const std::filesystem::path &reference, const std::filesystem::path &candidate, const std::vector<std::filesystem::path> &harnesses,
                           std::size_t maximum) {
  if (maximum == 0 || maximum > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    fail(ErrorCode::InvalidArgument, "behavioral harness limit must fit in streamsize and be nonzero");
  }
  std::size_t consumed = 0;
  for (const auto &relative : harnesses) {
    const auto reference_path = reference / "tests" / relative;
    const auto candidate_path = candidate / "tests" / relative;
    std::error_code error;
    const auto reference_size = std::filesystem::file_size(reference_path, error);
    if (error) {
      fail(ErrorCode::Io, "unable to size behavioral harness: " + error.message());
    }
    const auto candidate_size = std::filesystem::file_size(candidate_path, error);
    if (error) {
      fail(ErrorCode::Io, "unable to size behavioral harness: " + error.message());
    }
    if (reference_size != candidate_size || reference_size > maximum - consumed) {
      fail(reference_size != candidate_size ? ErrorCode::InvalidArgument : ErrorCode::ResourceLimit,
           reference_size != candidate_size ? "behavioral test harnesses differ: " + relative.string()
                                            : "behavioral test harnesses exceed the configured byte limit");
    }
    bool reference_truncated = false;
    bool candidate_truncated = false;
    const auto reference_text = readBoundedFile(reference_path, static_cast<std::size_t>(reference_size), reference_truncated);
    const auto candidate_text = readBoundedFile(candidate_path, static_cast<std::size_t>(candidate_size), candidate_truncated);
    if (reference_truncated || candidate_truncated || reference_text != candidate_text) {
      fail(ErrorCode::InvalidArgument, "behavioral test harnesses differ: " + relative.string());
    }
    consumed += static_cast<std::size_t>(reference_size);
  }
}

} // namespace

BehavioralTrace parseBehavioralTrace(std::string_view output) {
  BehavioralTrace result;
  std::istringstream lines{std::string(output)};
  std::string line;
  bool in_storage = false;
  std::ostringstream storage;
  auto finish_storage = [&]() {
    auto snapshot = storage.str();
    while (!snapshot.empty() && (snapshot.back() == '\n' || snapshot.back() == '\r')) {
      snapshot.pop_back();
    }
    if (!snapshot.empty()) {
      result.storage_snapshots.push_back(std::move(snapshot));
    }
    storage.str({});
    storage.clear();
  };
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.find("Storage state at point of failure") != std::string::npos) {
      if (in_storage) {
        finish_storage();
      }
      in_storage = true;
      continue;
    }
    if (in_storage) {
      if (line.starts_with("└") || line.starts_with("Test result:") || line.starts_with("[ PASS") || line.starts_with("[ FAIL")) {
        finish_storage();
        in_storage = false;
      } else {
        if (line.starts_with("│ ")) {
          line.erase(0, std::string("│ ").size());
        }
        storage << line << '\n';
        continue;
      }
    }

    constexpr std::string_view abort_prefix = "aborted with code ";
    constexpr std::string_view module_prefix = "originating in the module ";
    const auto abort_start = line.find(abort_prefix);
    if (abort_start != std::string::npos) {
      const auto code_start = abort_start + abort_prefix.size();
      const auto code_end = line.find(' ', code_start);
      const auto module_marker = line.find(module_prefix, code_end);
      if (code_end != std::string::npos && module_marker != std::string::npos) {
        std::uint64_t code = 0;
        const auto *first = line.data() + code_start;
        const auto *last = line.data() + code_end;
        const auto parsed = std::from_chars(first, last, code);
        const auto module_start = module_marker + module_prefix.size();
        const auto module_end = line.find(' ', module_start);
        if (parsed.ec == std::errc{} && parsed.ptr == last && module_start < line.size()) {
          result.aborts.push_back({
              .code = code,
              .module = line.substr(module_start, module_end - module_start),
          });
        }
      }
    }
    const bool passed = line.starts_with("[ PASS");
    const bool failed = line.starts_with("[ FAIL");
    if (passed || failed) {
      const auto bracket = line.find(']');
      if (bracket != std::string::npos) {
        auto name = line.substr(bracket + 1U);
        const auto first = name.find_first_not_of(" \t");
        if (first != std::string::npos) {
          name.erase(0, first);
          result.cases.push_back({.name = std::move(name), .passed = passed});
        }
      }
    }
    if (line.starts_with("Test result:")) {
      result.summary_seen = true;
    }
  }
  if (in_storage) {
    finish_storage();
  }
  return result;
}

RoundTripPackage prepareRoundTripPackage(const Module &module, const std::filesystem::path &output_directory) {
  if (output_directory.empty()) {
    fail(ErrorCode::InvalidArgument, "round-trip output directory must not be empty");
  }

  const auto semantic = buildSemanticModel(module);
  const auto &self = semantic.modules[module.self_module_handle];
  RoundTripPackage result{
      .root = output_directory,
      .manifest = output_directory / "Move.toml",
      .source = output_directory / "sources" / (self.source_name + ".move"),
      .emission = emitMoveModule(module),
      .external_modules = {},
  };
  for (const auto &dependency : semantic.modules) {
    if (!dependency.is_self) {
      result.external_modules.push_back(dependency.qualified_name);
    }
  }
  std::sort(result.external_modules.begin(), result.external_modules.end());
  result.external_modules.erase(std::unique(result.external_modules.begin(), result.external_modules.end()), result.external_modules.end());

  std::error_code error;
  const bool created = std::filesystem::create_directory(output_directory, error);
  if (error) {
    fail(ErrorCode::Io, "unable to create round-trip directory '" + output_directory.string() + "': " + error.message());
  }
  if (!created) {
    fail(ErrorCode::InvalidArgument, "round-trip output path already exists: " + output_directory.string());
  }

  const auto sources = output_directory / "sources";
  if (!std::filesystem::create_directory(sources, error) || error) {
    fail(ErrorCode::Io, "unable to create round-trip sources directory '" + sources.string() + "': " + error.message());
  }

  constexpr std::string_view manifest = "[package]\n"
                                        "name = \"MovescapeRoundTrip\"\n"
                                        "version = \"0.0.0\"\n";
  writeFile(result.manifest, std::string(manifest));
  writeFile(result.source, result.emission.source);
  return result;
}

RoundTripResult runRoundTrip(const Module &module, const std::filesystem::path &output_directory, const AptosCompilerOptions &options) {
  const auto effective_options = applySourcePolicies(options, {sourcePolicy(module)});
  RoundTripResult result{
      .package = prepareRoundTripPackage(module, output_directory),
      .compiler = std::nullopt,
      .candidate_module = std::nullopt,
      .interface_comparison = std::nullopt,
      .body_comparison = std::nullopt,
  };
  if (!result.package.readyForCompilerAttempt()) {
    return result;
  }

  result.compiler = invokeAptosCompiler(result.package, effective_options);
  if (!result.compiler->succeeded()) {
    return result;
  }

  const auto reference = normalizeModuleInterface(module);
  for (const auto &candidate_path : result.compiler->bytecode_modules) {
    const auto bytes = readBinaryFile(candidate_path);
    const auto candidate = loadModule(bytes);
    if (normalizeModuleInterface(candidate).module_name != reference.module_name) {
      continue;
    }
    if (result.candidate_module.has_value()) {
      fail(ErrorCode::Malformed, "compiler emitted multiple modules with identity " + reference.module_name);
    }
    result.candidate_module = candidate_path;
    result.interface_comparison = compareModuleInterfaces(module, candidate);
    result.body_comparison = compareModuleBodies(module, candidate);
  }
  return result;
}

RoundTripCorpusResult runRoundTripCorpus(const std::filesystem::path &input_directory, const std::filesystem::path &output_directory,
                                         const AptosCompilerOptions &options) {
  if (output_directory.empty()) {
    fail(ErrorCode::InvalidArgument, "round-trip corpus output directory must not be empty");
  }
  const auto inputs = discoverInputModules(input_directory);
  std::vector<Module> modules;
  modules.reserve(inputs.size());
  for (const auto &input : inputs) {
    auto module = loadModule(readBinaryFile(input));
    validateModule(module);
    modules.push_back(std::move(module));
  }

  std::error_code error;
  if (!std::filesystem::create_directory(output_directory, error) || error) {
    if (!error && std::filesystem::exists(output_directory)) {
      fail(ErrorCode::InvalidArgument, "round-trip corpus output path already exists: " + output_directory.string());
    }
    fail(ErrorCode::Io, "unable to create round-trip corpus directory: " + error.message());
  }

  RoundTripCorpusResult result{.root = output_directory, .entries = {}};
  result.entries.reserve(inputs.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    auto package = output_directory / ("module-" + std::to_string(index));
    result.entries.push_back({
        .input = inputs[index],
        .result = runRoundTrip(modules[index], package, options),
    });
  }
  return result;
}

MultiModuleRoundTripPackage prepareMultiModuleRoundTripPackage(const std::vector<Module> &modules, const std::filesystem::path &output_directory) {
  return prepareMultiModuleRoundTripPackage(modules, {}, {}, output_directory);
}

MultiModuleRoundTripPackage prepareMultiModuleRoundTripPackage(const std::vector<Module> &modules, const std::vector<Module> &external_provider_modules,
                                                               const std::vector<LocalPackageDependency> &compiler_dependencies,
                                                               const std::filesystem::path &output_directory) {
  if (modules.empty()) {
    fail(ErrorCode::InvalidArgument, "multi-module round trip requires at least one module");
  }
  if (output_directory.empty()) {
    fail(ErrorCode::InvalidArgument, "multi-module round-trip output directory must not be empty");
  }
  std::vector<LocalPackageDependency> normalized_compiler_dependencies;
  normalized_compiler_dependencies.reserve(compiler_dependencies.size());
  std::set<std::string> compiler_dependency_names;
  std::set<std::filesystem::path> compiler_dependency_roots;
  for (const auto &dependency : compiler_dependencies) {
    if (dependency.name.empty()) {
      fail(ErrorCode::InvalidArgument, "compiler dependency package name must not be empty");
    }
    const auto manifest = loadMovePackageManifest(dependency.root);
    if (manifest.package_name != dependency.name) {
      fail(ErrorCode::InvalidArgument, "compiler dependency name does not match Move.toml package name: " + dependency.name + " != " + manifest.package_name);
    }
    if (!compiler_dependency_names.insert(dependency.name).second) {
      fail(ErrorCode::InvalidArgument, "duplicate compiler dependency package name: " + dependency.name);
    }
    if (!compiler_dependency_roots.insert(manifest.root).second) {
      fail(ErrorCode::InvalidArgument, "duplicate compiler dependency package root: " + manifest.root.string());
    }
    normalized_compiler_dependencies.push_back({.name = dependency.name, .root = manifest.root});
  }
  std::sort(normalized_compiler_dependencies.begin(), normalized_compiler_dependencies.end(), [](const auto &left, const auto &right) {
    if (left.name != right.name) {
      return left.name < right.name;
    }
    return left.root < right.root;
  });

  struct PreparedModule {
    const Module *module = nullptr;
    SemanticModel semantic;
    std::string identity;
    std::string source_name;
    std::optional<MoveEmission> emission;
    std::vector<std::string> external_modules;

    [[nodiscard]] bool emitted() const noexcept { return emission.has_value(); }
  };
  std::vector<PreparedModule> prepared;
  prepared.reserve(modules.size() + external_provider_modules.size());
  const auto prepare_module = [&](const Module &module, bool emit) {
    validateModule(module);
    const auto semantic = buildSemanticModel(module);
    const auto &self = semantic.modules.at(module.self_module_handle);
    PreparedModule item{
        .module = &module,
        .semantic = semantic,
        .identity = self.qualified_name,
        .source_name = self.source_name,
        .emission = emit ? std::optional<MoveEmission>{emitMoveModule(module)} : std::nullopt,
        .external_modules = {},
    };
    for (const auto &dependency : semantic.modules) {
      if (!dependency.is_self) {
        item.external_modules.push_back(dependency.qualified_name);
      }
    }
    std::sort(item.external_modules.begin(), item.external_modules.end());
    item.external_modules.erase(std::unique(item.external_modules.begin(), item.external_modules.end()), item.external_modules.end());
    prepared.push_back(std::move(item));
  };
  for (const auto &module : modules) {
    prepare_module(module, true);
  }
  for (const auto &module : external_provider_modules) {
    prepare_module(module, false);
  }
  std::sort(prepared.begin(), prepared.end(), [](const auto &left, const auto &right) { return left.identity < right.identity; });
  for (std::size_t index = 1; index < prepared.size(); ++index) {
    if (prepared[index - 1].identity == prepared[index].identity) {
      fail(ErrorCode::InvalidArgument, "duplicate module identity in multi-module package: " + prepared[index].identity);
    }
  }

  std::set<std::string> supplied;
  std::map<std::string, const PreparedModule *> provider_by_identity;
  for (const auto &module : prepared) {
    supplied.insert(module.identity);
    provider_by_identity.emplace(module.identity, &module);
  }
  std::set<std::string> unresolved;
  for (const auto &module : prepared) {
    if (!module.emitted()) {
      continue;
    }
    for (const auto &dependency : module.external_modules) {
      if (!supplied.contains(dependency)) {
        unresolved.insert(dependency);
      }
    }
  }

  const auto signature_text = [](const Module &module, const Signature &signature) {
    std::ostringstream out;
    out << '(';
    for (std::size_t index = 0; index < signature.size(); ++index) {
      if (index != 0) {
        out << ',';
      }
      out << renderType(module, signature[index]);
    }
    out << ')';
    return out.str();
  };
  const auto same_struct_parameters = [](const auto &left, const auto &right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
      if (left[index].constraints != right[index].constraints || left[index].is_phantom != right[index].is_phantom) {
        return false;
      }
    }
    return true;
  };
  const auto same_attributes = [](const auto &left, const auto &right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
      if (left[index].kind != right[index].kind || left[index].value != right[index].value) {
        return false;
      }
    }
    return true;
  };
  for (const auto &consumer : prepared) {
    if (!consumer.emitted()) {
      continue;
    }
    for (std::size_t index = 0; index < consumer.semantic.structures.size(); ++index) {
      const auto &symbol = consumer.semantic.structures[index];
      const auto &owner = consumer.semantic.modules[symbol.module];
      if (owner.is_self || !supplied.contains(owner.qualified_name)) {
        continue;
      }
      const auto &provider = *provider_by_identity.at(owner.qualified_name);
      const auto match = std::find_if(provider.semantic.structures.begin(), provider.semantic.structures.end(), [&](const auto &candidate) {
        return provider.semantic.modules[candidate.module].is_self && candidate.name == symbol.name && candidate.definition.has_value();
      });
      if (match == provider.semantic.structures.end()) {
        fail(ErrorCode::InvalidArgument, "dependency " + owner.qualified_name + " does not define referenced struct " + symbol.name);
      }
      const auto &expected = consumer.module->struct_handles[index];
      const auto &actual = provider.module->struct_handles[match->handle];
      if (expected.abilities != actual.abilities || !same_struct_parameters(expected.type_parameters, actual.type_parameters)) {
        fail(ErrorCode::TypeMismatch, "dependency struct ABI mismatch for " + symbol.qualified_name);
      }
    }
    for (std::size_t index = 0; index < consumer.semantic.functions.size(); ++index) {
      const auto &symbol = consumer.semantic.functions[index];
      const auto &owner = consumer.semantic.modules[symbol.module];
      if (owner.is_self || !supplied.contains(owner.qualified_name)) {
        continue;
      }
      const auto &provider = *provider_by_identity.at(owner.qualified_name);
      const auto match = std::find_if(provider.semantic.functions.begin(), provider.semantic.functions.end(), [&](const auto &candidate) {
        return provider.semantic.modules[candidate.module].is_self && candidate.name == symbol.name && candidate.definition.has_value();
      });
      if (match == provider.semantic.functions.end()) {
        fail(ErrorCode::InvalidArgument, "dependency " + owner.qualified_name + " does not define referenced function " + symbol.name);
      }
      const auto &expected = consumer.module->function_handles[index];
      const auto &actual = provider.module->function_handles[match->handle];
      const auto actual_definition = provider.module->function_definitions.at(*match->definition);
      if (actual_definition.visibility == Visibility::Private || expected.type_parameters != actual.type_parameters ||
          !same_attributes(expected.attributes, actual.attributes) ||
          signature_text(*consumer.module, consumer.module->signatures[expected.parameters]) !=
              signature_text(*provider.module, provider.module->signatures[actual.parameters]) ||
          signature_text(*consumer.module, consumer.module->signatures[expected.returns]) !=
              signature_text(*provider.module, provider.module->signatures[actual.returns])) {
        fail(ErrorCode::TypeMismatch, "dependency function ABI mismatch for " + symbol.qualified_name);
      }
    }
  }

  MultiModuleRoundTripPackage result{
      .root = output_directory,
      .manifest = output_directory / "Move.toml",
      .sources = {},
      .module_identities = {},
      .emissions = {},
      .compiler_dependencies = normalized_compiler_dependencies,
      .unresolved_external_modules = {unresolved.begin(), unresolved.end()},
  };
  result.sources.reserve(modules.size());
  result.module_identities.reserve(modules.size());
  result.emissions.reserve(modules.size());
  std::size_t source_index = 0;
  for (auto &module : prepared) {
    if (!module.emitted()) {
      continue;
    }
    result.sources.push_back(output_directory / "sources" / ("module-" + std::to_string(source_index) + "-" + module.source_name + ".move"));
    result.module_identities.push_back(module.identity);
    result.emissions.push_back(std::move(*module.emission));
    ++source_index;
  }

  std::error_code error;
  if (!std::filesystem::create_directory(output_directory, error) || error) {
    if (!error && std::filesystem::exists(output_directory)) {
      fail(ErrorCode::InvalidArgument, "multi-module round-trip output path already exists: " + output_directory.string());
    }
    fail(ErrorCode::Io, "unable to create multi-module round-trip directory: " + error.message());
  }
  const auto sources = output_directory / "sources";
  if (!std::filesystem::create_directory(sources, error) || error) {
    fail(ErrorCode::Io, "unable to create multi-module sources directory: " + error.message());
  }
  std::ostringstream manifest;
  manifest << "[package]\n"
              "name = \"MovescapeMultiRoundTrip\"\n"
              "version = \"0.0.0\"\n";
  if (!result.compiler_dependencies.empty()) {
    manifest << "\n[dependencies]\n";
    for (const auto &dependency : result.compiler_dependencies) {
      manifest << tomlBasicString(dependency.name) << " = { local = " << tomlBasicString(dependency.root.string()) << " }\n";
    }
  }
  writeFile(result.manifest, manifest.str());
  for (std::size_t index = 0; index < result.sources.size(); ++index) {
    writeFile(result.sources[index], result.emissions[index].source);
  }
  return result;
}

MultiModuleRoundTripResult runRoundTripPackage(const std::filesystem::path &input_directory, const std::filesystem::path &output_directory,
                                               const AptosCompilerOptions &options) {
  return runRoundTripPackage(std::vector<std::filesystem::path>{input_directory}, output_directory, options);
}

MultiModuleRoundTripResult runRoundTripPackage(const std::vector<std::filesystem::path> &input_directories, const std::filesystem::path &output_directory,
                                               const AptosCompilerOptions &options) {
  return runRoundTripPackage(input_directories, {}, output_directory, options);
}

MultiModuleRoundTripResult runRoundTripPackage(const std::vector<std::filesystem::path> &input_directories,
                                               const std::vector<std::filesystem::path> &external_package_directories,
                                               const std::filesystem::path &output_directory, const AptosCompilerOptions &options) {
  if (input_directories.empty()) {
    fail(ErrorCode::InvalidArgument, "multi-module round trip requires at least one input tree");
  }
  std::vector<LocalPackageDependency> compiler_dependencies;
  compiler_dependencies.reserve(external_package_directories.size());
  for (const auto &root : external_package_directories) {
    const auto manifest = loadMovePackageManifest(root);
    compiler_dependencies.push_back({.name = manifest.package_name, .root = manifest.root});
  }

  std::vector<Module> external_providers;
  std::map<std::string, std::vector<std::uint8_t>> external_bytes_by_identity;
  if (!external_package_directories.empty()) {
    const auto provider_inputs = discoverPackageInputModules(external_package_directories);
    external_providers.reserve(provider_inputs.size());
    for (const auto &input : provider_inputs) {
      auto bytes = readBinaryFile(input);
      auto module = loadModule(bytes);
      const auto identity = normalizeModuleInterface(module).module_name;
      const auto existing = external_bytes_by_identity.find(identity);
      if (existing != external_bytes_by_identity.end()) {
        if (existing->second == bytes) {
          continue;
        }
        fail(ErrorCode::InvalidArgument, "conflicting module identity across external packages: " + identity);
      }
      external_bytes_by_identity.emplace(identity, std::move(bytes));
      external_providers.push_back(std::move(module));
    }
  }

  const auto inputs = discoverPackageInputModules(input_directories);
  std::vector<Module> references;
  references.reserve(inputs.size());
  std::map<std::string, std::size_t> reference_by_identity;
  std::map<std::string, std::vector<std::uint8_t>> bytes_by_identity;
  for (const auto &input : inputs) {
    auto bytes = readBinaryFile(input);
    auto module = loadModule(bytes);
    const auto identity = normalizeModuleInterface(module).module_name;
    const auto external = external_bytes_by_identity.find(identity);
    if (external != external_bytes_by_identity.end()) {
      if (external->second == bytes) {
        continue;
      }
      fail(ErrorCode::InvalidArgument, "primary module conflicts with external package identity: " + identity);
    }
    const auto existing = bytes_by_identity.find(identity);
    if (existing != bytes_by_identity.end()) {
      if (existing->second == bytes) {
        continue;
      }
      fail(ErrorCode::InvalidArgument, "conflicting module identity across input/dependency trees: " + identity);
    }
    bytes_by_identity.emplace(identity, std::move(bytes));
    reference_by_identity.emplace(identity, references.size());
    references.push_back(std::move(module));
  }
  if (references.empty()) {
    fail(ErrorCode::InvalidArgument, "external packages consume every discovered primary module");
  }
  std::vector<MoveSourcePolicy> source_policies;
  source_policies.reserve(references.size());
  for (const auto &reference : references) {
    source_policies.push_back(sourcePolicy(reference));
  }
  const auto effective_options = applySourcePolicies(options, source_policies);

  MultiModuleRoundTripResult result{
      .package = prepareMultiModuleRoundTripPackage(references, external_providers, compiler_dependencies, output_directory),
      .compiler = std::nullopt,
      .modules = {},
  };
  result.modules.reserve(result.package.module_identities.size());
  for (const auto &identity : result.package.module_identities) {
    result.modules.push_back({
        .identity = identity,
        .candidate_module = std::nullopt,
        .interface_comparison = std::nullopt,
        .body_comparison = std::nullopt,
    });
  }
  if (!result.package.readyForCompilerAttempt()) {
    return result;
  }

  result.compiler = invokeAptos(result.package.root, result.package.root / "movescape-multi-compiler.log", "compile", effective_options, true);
  if (!result.compiler->succeeded()) {
    return result;
  }

  std::map<std::string, std::filesystem::path> candidates;
  for (const auto &candidate_path : result.compiler->bytecode_modules) {
    const auto candidate = loadModule(readBinaryFile(candidate_path));
    const auto identity = normalizeModuleInterface(candidate).module_name;
    if (!reference_by_identity.contains(identity)) {
      continue;
    }
    if (!candidates.emplace(identity, candidate_path).second) {
      fail(ErrorCode::Malformed, "compiler emitted multiple modules with identity " + identity);
    }
  }

  for (auto &entry : result.modules) {
    const auto candidate_path = candidates.find(entry.identity);
    if (candidate_path == candidates.end()) {
      continue;
    }
    const auto reference_index = reference_by_identity.at(entry.identity);
    const auto candidate = loadModule(readBinaryFile(candidate_path->second));
    entry.candidate_module = candidate_path->second;
    entry.interface_comparison = compareModuleInterfaces(references[reference_index], candidate);
    entry.body_comparison = compareModuleBodies(references[reference_index], candidate);
  }
  return result;
}

BehavioralTestComparison runBehavioralTests(const std::filesystem::path &reference_package, const std::filesystem::path &candidate_package,
                                            const std::filesystem::path &output_directory, const AptosCompilerOptions &options) {
  if (output_directory.empty()) {
    fail(ErrorCode::InvalidArgument, "behavioral test output directory must not be empty");
  }
  const auto reference_harnesses = discoverTestHarnesses(reference_package);
  const auto candidate_harnesses = discoverTestHarnesses(candidate_package);
  if (reference_harnesses != candidate_harnesses) {
    fail(ErrorCode::InvalidArgument, "behavioral test packages do not contain the same harness paths");
  }
  verifySharedHarnesses(reference_package, candidate_package, reference_harnesses, options.max_harness_bytes);

  std::error_code error;
  if (!std::filesystem::create_directory(output_directory, error) || error) {
    if (!error && std::filesystem::exists(output_directory)) {
      fail(ErrorCode::InvalidArgument, "behavioral test output path already exists: " + output_directory.string());
    }
    fail(ErrorCode::Io, "unable to create behavioral test output directory: " + error.message());
  }

  BehavioralTestComparison result{
      .root = output_directory,
      .shared_harnesses = reference_harnesses,
      .reference = invokeAptos(reference_package, output_directory / "reference-tests.log", "test", options, false),
      .candidate = invokeAptos(candidate_package, output_directory / "candidate-tests.log", "test", options, false),
      .reference_trace = {},
      .candidate_trace = {},
  };
  result.reference_trace = parseBehavioralTrace(result.reference.output);
  result.candidate_trace = parseBehavioralTrace(result.candidate.output);
  return result;
}

} // namespace movescape
