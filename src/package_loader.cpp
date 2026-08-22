#include "movescape/package_loader.hpp"

#include "movescape/error.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace movescape {

namespace {

[[noreturn]] void fail(ErrorCode code, std::string message) { throw Error(code, Error::UnknownOffset, std::move(message)); }

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1U);
}

[[nodiscard]] std::string readManifest(const std::filesystem::path &path, std::size_t maximum) {
  if (maximum == 0) {
    fail(ErrorCode::InvalidArgument, "manifest byte limit must be greater than zero");
  }
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    fail(ErrorCode::Io, "unable to size package manifest '" + path.string() + "': " + error.message());
  }
  if (size > maximum) {
    fail(ErrorCode::ResourceLimit, "package manifest exceeds configured byte limit: " + path.string());
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail(ErrorCode::Io, "unable to read package manifest: " + path.string());
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  if (!input && !result.empty()) {
    fail(ErrorCode::Io, "unable to read complete package manifest: " + path.string());
  }
  return result;
}

[[nodiscard]] std::string stripComment(std::string_view line) {
  bool in_basic = false;
  bool in_literal = false;
  bool escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const auto character = line[index];
    if (in_basic) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_basic = false;
      }
      continue;
    }
    if (in_literal) {
      if (character == '\'') {
        in_literal = false;
      }
      continue;
    }
    if (character == '"') {
      in_basic = true;
    } else if (character == '\'') {
      in_literal = true;
    } else if (character == '#') {
      return std::string(line.substr(0, index));
    }
  }
  return std::string(line);
}

struct Balance {
  std::size_t braces = 0;
  std::size_t brackets = 0;
  bool in_basic = false;
  bool in_literal = false;
  bool escaped = false;
};

void updateBalance(std::string_view text, Balance &balance, const std::filesystem::path &manifest) {
  for (const auto character : text) {
    if (balance.in_basic) {
      if (balance.escaped) {
        balance.escaped = false;
      } else if (character == '\\') {
        balance.escaped = true;
      } else if (character == '"') {
        balance.in_basic = false;
      }
      continue;
    }
    if (balance.in_literal) {
      if (character == '\'') {
        balance.in_literal = false;
      }
      continue;
    }
    if (character == '"') {
      balance.in_basic = true;
    } else if (character == '\'') {
      balance.in_literal = true;
    } else if (character == '{') {
      ++balance.braces;
    } else if (character == '}') {
      if (balance.braces == 0) {
        fail(ErrorCode::Malformed, "unbalanced '}' in package manifest: " + manifest.string());
      }
      --balance.braces;
    } else if (character == '[') {
      ++balance.brackets;
    } else if (character == ']') {
      if (balance.brackets == 0) {
        fail(ErrorCode::Malformed, "unbalanced ']' in package manifest: " + manifest.string());
      }
      --balance.brackets;
    }
  }
}

[[nodiscard]] std::size_t findTopLevel(std::string_view text, char needle) {
  Balance balance;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto character = text[index];
    if (balance.in_basic) {
      if (balance.escaped) {
        balance.escaped = false;
      } else if (character == '\\') {
        balance.escaped = true;
      } else if (character == '"') {
        balance.in_basic = false;
      }
      continue;
    }
    if (balance.in_literal) {
      if (character == '\'') {
        balance.in_literal = false;
      }
      continue;
    }
    if (character == '"') {
      balance.in_basic = true;
    } else if (character == '\'') {
      balance.in_literal = true;
    } else if (character == '{') {
      ++balance.braces;
    } else if (character == '}') {
      if (balance.braces != 0) {
        --balance.braces;
      }
    } else if (character == '[') {
      ++balance.brackets;
    } else if (character == ']') {
      if (balance.brackets != 0) {
        --balance.brackets;
      }
    } else if (character == needle && balance.braces == 0 && balance.brackets == 0) {
      return index;
    }
  }
  return std::string_view::npos;
}

[[nodiscard]] std::string decodeTomlString(std::string_view value, const std::filesystem::path &manifest, std::string_view subject) {
  value = trim(value);
  if (value.size() < 2U || !((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
    fail(ErrorCode::Malformed, std::string(subject) + " must be a quoted TOML string in " + manifest.string());
  }
  const auto quote = value.front();
  value.remove_prefix(1);
  value.remove_suffix(1);
  if (quote == '\'') {
    return std::string(value);
  }

  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto character = value[index];
    if (character != '\\') {
      result.push_back(character);
      continue;
    }
    if (++index == value.size()) {
      fail(ErrorCode::Malformed, "trailing escape in TOML string in " + manifest.string());
    }
    switch (value[index]) {
    case '"':
    case '\\':
    case '/':
      result.push_back(value[index]);
      break;
    case 'b':
      result.push_back('\b');
      break;
    case 'f':
      result.push_back('\f');
      break;
    case 'n':
      result.push_back('\n');
      break;
    case 'r':
      result.push_back('\r');
      break;
    case 't':
      result.push_back('\t');
      break;
    default:
      fail(ErrorCode::UnsupportedFeature, "unsupported TOML escape in " + std::string(subject) + " in " + manifest.string());
    }
  }
  return result;
}

[[nodiscard]] std::string decodeTomlKey(std::string_view key, const std::filesystem::path &manifest) {
  key = trim(key);
  if (key.empty()) {
    fail(ErrorCode::Malformed, "empty TOML key in package manifest: " + manifest.string());
  }
  if (key.front() == '"' || key.front() == '\'') {
    return decodeTomlString(key, manifest, "TOML key");
  }
  if (!std::all_of(key.begin(), key.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_' ||
               character == '-';
      })) {
    fail(ErrorCode::Malformed, "unsupported bare TOML key in package manifest: " + manifest.string());
  }
  return std::string(key);
}

struct Statement {
  std::string section;
  std::string text;
};

[[nodiscard]] std::vector<Statement> parseStatements(std::string_view document, const std::filesystem::path &manifest) {
  std::vector<Statement> result;
  std::string section;
  std::string pending;
  Balance balance;
  std::istringstream lines{std::string(document)};
  std::string raw_line;
  while (std::getline(lines, raw_line)) {
    const auto uncommented = stripComment(raw_line);
    const auto line = trim(uncommented);
    if (line.empty()) {
      continue;
    }
    if (pending.empty() && line.front() == '[') {
      if (line.size() < 3U || line.back() != ']' || line[1] == '[') {
        fail(ErrorCode::UnsupportedFeature, "unsupported TOML table syntax in package manifest: " + manifest.string());
      }
      section = std::string(trim(line.substr(1, line.size() - 2U)));
      continue;
    }
    if (!pending.empty()) {
      pending.push_back('\n');
    }
    pending.append(line);
    updateBalance(line, balance, manifest);
    if (!balance.in_basic && !balance.in_literal && balance.braces == 0 && balance.brackets == 0) {
      result.push_back({.section = section, .text = std::move(pending)});
      pending.clear();
      balance = {};
    }
  }
  if (!pending.empty() || balance.in_basic || balance.in_literal || balance.braces != 0 || balance.brackets != 0) {
    fail(ErrorCode::Malformed, "unterminated TOML value in package manifest: " + manifest.string());
  }
  return result;
}

[[nodiscard]] std::optional<std::string> localPathFromDependency(std::string_view value, const std::filesystem::path &manifest) {
  value = trim(value);
  if (value.empty() || value.front() != '{' || value.back() != '}') {
    return std::nullopt;
  }
  value.remove_prefix(1);
  value.remove_suffix(1);
  while (!trim(value).empty()) {
    const auto comma = findTopLevel(value, ',');
    const auto field = trim(value.substr(0, comma));
    const auto equal = findTopLevel(field, '=');
    if (equal == std::string_view::npos) {
      fail(ErrorCode::Malformed, "malformed inline dependency table in " + manifest.string());
    }
    const auto key = decodeTomlKey(field.substr(0, equal), manifest);
    if (key == "local") {
      return decodeTomlString(field.substr(equal + 1U), manifest, "local dependency path");
    }
    if (comma == std::string_view::npos) {
      break;
    }
    value.remove_prefix(comma + 1U);
  }
  return std::nullopt;
}

[[nodiscard]] std::filesystem::path canonicalDirectory(const std::filesystem::path &path, std::string_view subject) {
  std::error_code error;
  if (!std::filesystem::is_directory(path, error) || error) {
    fail(ErrorCode::InvalidArgument, std::string(subject) + " is not a readable directory: " + path.string());
  }
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error) {
    fail(ErrorCode::Io, "unable to resolve " + std::string(subject) + " '" + path.string() + "': " + error.message());
  }
  return canonical;
}

void appendModules(const std::filesystem::path &root, bool required, std::size_t maximum, std::vector<std::filesystem::path> &output) {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    if (!required) {
      error.clear();
      if (!std::filesystem::exists(root, error) && !error) {
        return;
      }
    }
    fail(required ? ErrorCode::InvalidArgument : ErrorCode::Io, "compiled package contains no readable bytecode directory '" + root.string() + "'" +
                                                                    (required ? "; run `aptos move compile` first" : ": " + error.message()));
  }
  std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    if (error) {
      fail(ErrorCode::Io, "unable to inspect package bytecode: " + error.message());
    }
    if (iterator->is_regular_file(error) && !error && iterator->path().extension() == ".mv") {
      if (output.size() >= maximum) {
        fail(ErrorCode::ResourceLimit, "package module count exceeds configured limit");
      }
      output.push_back(iterator->path());
    }
    iterator.increment(error);
  }
  if (error) {
    fail(ErrorCode::Io, "unable to inspect package bytecode: " + error.message());
  }
}

} // namespace

MovePackageManifest loadMovePackageManifest(const std::filesystem::path &package_root, const PackageDiscoveryLimits &limits) {
  const auto root = canonicalDirectory(package_root, "Move package root");
  const auto manifest = root / "Move.toml";
  std::error_code error;
  if (!std::filesystem::is_regular_file(manifest, error) || error) {
    fail(ErrorCode::InvalidArgument, "Move package has no readable Move.toml: " + root.string());
  }
  const auto document = readManifest(manifest, limits.max_manifest_bytes);
  MovePackageManifest result{
      .root = root,
      .path = manifest,
      .package_name = {},
      .local_dependencies = {},
  };
  std::set<std::string> dependency_names;
  for (const auto &statement : parseStatements(document, manifest)) {
    const auto equal = findTopLevel(statement.text, '=');
    if (equal == std::string::npos) {
      fail(ErrorCode::Malformed, "manifest entry has no assignment in " + manifest.string());
    }
    const auto key = decodeTomlKey(std::string_view(statement.text).substr(0, equal), manifest);
    const auto value = trim(std::string_view(statement.text).substr(equal + 1U));
    if (statement.section == "package" && key == "name") {
      if (!result.package_name.empty()) {
        fail(ErrorCode::Malformed, "duplicate package name in " + manifest.string());
      }
      result.package_name = decodeTomlString(value, manifest, "package name");
      continue;
    }
    if (statement.section != "dependencies" && statement.section != "dev-dependencies") {
      continue;
    }
    const auto local = localPathFromDependency(value, manifest);
    if (!local.has_value()) {
      continue;
    }
    if (local->empty()) {
      fail(ErrorCode::Malformed, "local dependency path is empty in " + manifest.string());
    }
    if (!dependency_names.insert(key).second) {
      fail(ErrorCode::Malformed, "duplicate local dependency '" + key + "' in " + manifest.string());
    }
    result.local_dependencies.push_back({
        .name = key,
        .root = canonicalDirectory(root / *local, "local dependency root"),
    });
  }
  if (result.package_name.empty()) {
    fail(ErrorCode::Malformed, "Move.toml has no [package] name: " + manifest.string());
  }
  if (result.package_name == "." || result.package_name == ".." || result.package_name.find('/') != std::string::npos ||
      result.package_name.find('\\') != std::string::npos) {
    fail(ErrorCode::Malformed, "package name cannot identify a build directory safely: " + result.package_name);
  }
  std::sort(result.local_dependencies.begin(), result.local_dependencies.end(), [](const auto &left, const auto &right) {
    if (left.name != right.name) {
      return left.name < right.name;
    }
    return left.root < right.root;
  });
  return result;
}

std::vector<std::filesystem::path> discoverPackageInputModules(const std::vector<std::filesystem::path> &inputs, const PackageDiscoveryLimits &limits) {
  if (inputs.empty()) {
    fail(ErrorCode::InvalidArgument, "package discovery requires at least one input directory");
  }
  if (limits.max_packages == 0 || limits.max_dependency_depth == 0 || limits.max_modules == 0) {
    fail(ErrorCode::InvalidArgument, "package discovery limits must be greater than zero");
  }

  enum class VisitState { Active, Complete };
  std::map<std::filesystem::path, VisitState> states;
  std::vector<std::filesystem::path> modules;
  const auto visit = [&](const auto &self, const std::filesystem::path &root, std::size_t depth, bool primary) -> void {
    if (depth > limits.max_dependency_depth) {
      fail(ErrorCode::ResourceLimit, "local package dependency depth exceeds configured limit");
    }
    const auto canonical = canonicalDirectory(root, "package input");
    const auto known = states.find(canonical);
    if (known != states.end()) {
      if (known->second == VisitState::Active) {
        fail(ErrorCode::InvalidArgument, "cycle in local Move.toml dependency graph at " + canonical.string());
      }
      return;
    }
    if (states.size() >= limits.max_packages) {
      fail(ErrorCode::ResourceLimit, "local package dependency count exceeds configured limit");
    }
    states.emplace(canonical, VisitState::Active);
    const auto manifest = loadMovePackageManifest(canonical, limits);
    for (const auto &dependency : manifest.local_dependencies) {
      self(self, dependency.root, depth + 1U, false);
    }
    appendModules(canonical / "build" / manifest.package_name / "bytecode_modules", primary, limits.max_modules, modules);
    states.at(canonical) = VisitState::Complete;
  };

  for (const auto &input : inputs) {
    const auto canonical = canonicalDirectory(input, "package input");
    std::error_code error;
    if (std::filesystem::is_regular_file(canonical / "Move.toml", error) && !error) {
      visit(visit, canonical, 1U, true);
    } else {
      appendModules(canonical, true, limits.max_modules, modules);
    }
  }
  std::sort(modules.begin(), modules.end());
  modules.erase(std::unique(modules.begin(), modules.end()), modules.end());
  if (modules.empty()) {
    fail(ErrorCode::InvalidArgument, "package inputs contain no compiled .mv modules");
  }
  return modules;
}

} // namespace movescape
