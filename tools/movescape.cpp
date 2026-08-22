#include "movescape/behavior.hpp"
#include "movescape/body_compare.hpp"
#include "movescape/cfg.hpp"
#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"
#include "movescape/expression_ir.hpp"
#include "movescape/format.hpp"
#include "movescape/graph_analysis.hpp"
#include "movescape/loader.hpp"
#include "movescape/local_analysis.hpp"
#include "movescape/metadata.hpp"
#include "movescape/module_compare.hpp"
#include "movescape/module_loader.hpp"
#include "movescape/move_emitter.hpp"
#include "movescape/region.hpp"
#include "movescape/round_trip.hpp"
#include "movescape/semantic.hpp"
#include "movescape/source_map.hpp"
#include "movescape/stackless_ir.hpp"
#include "movescape/validator.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>

namespace {

void printUsage(std::ostream &out) {
  out << "usage:\n"
      << "  movescape inspect <bytecode.mv> [--script]\n"
      << "  movescape module <module.mv>\n"
      << "  movescape metadata <bytecode.mv> [--decode-aptos-v1] [--script]\n"
      << "  movescape symbols <module.mv>\n"
      << "  movescape compare-interface <reference.mv> <candidate.mv>\n"
      << "  movescape compare-bodies <reference.mv> <candidate.mv>\n"
      << "  movescape compare-behavior <reference-package> <candidate-package> "
         "<output-directory> [compiler-options]\n"
      << "  movescape compare-behavior-outcomes <reference-package> "
         "<candidate-package> <output-directory> [compiler-options]\n"
      << "  movescape generate-behavior-harness <module.mv> [output.move]\n"
      << "  movescape round-trip-prepare <module.mv> <output-directory>\n"
      << "  movescape round-trip <module.mv> <output-directory> "
         "[compiler-options]\n"
      << "  movescape round-trip-corpus <input-directory> <output-directory> "
         "[compiler-options]\n"
      << "  movescape round-trip-package <input-directory> <output-directory> "
         "[compiler-options]\n"
      << "  movescape disassemble <bytecode.mv> [--script]\n"
      << "  movescape decompile <bytecode.mv> [output.move] "
         "[--source-map module.mvsm] [--script]\n"
      << "  movescape source-location <module.mvsm> <function-index> "
         "<code-offset>\n"
      << "  movescape cfg <module.mv> <function-index>\n"
      << "  movescape cfg-dot <module.mv> <function-index>\n"
      << "  movescape analyze <module.mv> <function-index>\n"
      << "  movescape lift <module.mv> <function-index>\n"
      << "  movescape dataflow <module.mv> <function-index>\n"
      << "  movescape expressions <module.mv> <function-index>\n"
      << "  movescape structure <module.mv> <function-index>\n"
      << "\n"
      << "commands:\n"
      << "  inspect   validate and print the binary envelope and table "
         "directory\n"
      << "  module    decode tables and print a module summary\n"
      << "  metadata  print lossless raw metadata with optional Aptos decoding\n"
      << "  symbols   resolve and print semantic symbols and calls\n"
      << "  compare-interface compare normalized declarations, not bodies\n"
      << "  compare-bodies compare normalized opcodes and CFGs conservatively\n"
      << "  compare-behavior run identical Move tests against two packages\n"
      << "  compare-behavior-outcomes compare intentional failures and "
         "storage\n"
      << "  generate-behavior-harness emit bounded scalar and stateful VM "
         "probes\n"
      << "  round-trip-prepare emit a non-overwriting Move compiler package\n"
      << "  round-trip prepare, compile, and compare the module interface\n"
      << "  round-trip-corpus compile and compare every .mv in a tree\n"
      << "  round-trip-package compile manifest/local dependency modules "
         "together\n"
      << "  disassemble decode, validate, and print canonical bytecode\n"
      << "  decompile recover a whole Move module or transaction script\n"
      << "  source-location resolve a bytecode offset through an .mvsm map\n"
      << "  cfg       print basic blocks and typed control-flow edges\n"
      << "  cfg-dot   emit a Graphviz DOT control-flow graph\n"
      << "  analyze   print dominator, SCC, loop, and dependence analyses\n"
      << "  lift      print stackless low-level IR with explicit values\n"
      << "  dataflow  print reaching definitions, liveness, and local state\n"
      << "  expressions recover block-local expression trees\n"
      << "  structure recover if/else, loop, break, and continue regions\n";
  out << "\ncompiler-options:\n"
      << "  [aptos-executable] (backward-compatible positional form)\n"
      << "  --aptos <path> --bytecode-version <n> --language-version <name>\n"
      << "  package roots import local Move.toml dependencies; repeated\n"
      << "  --dependency <package-root-or-bytecode-tree> adds emitted roots\n"
      << "  --external-package <compiled-package-root> verifies an external\n"
      << "  source/native dependency; --framework is an equivalent alias\n";
}

movescape::AptosCompilerOptions parseCompilerOptions(int argc, char **argv, int first, std::vector<std::filesystem::path> *dependencies = nullptr,
                                                    std::vector<std::filesystem::path> *external_packages = nullptr) {
  movescape::AptosCompilerOptions options;
  bool executable_set = false;
  for (int index = first; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (!argument.starts_with("--") && !executable_set) {
      options.executable = argv[index];
      executable_set = true;
      continue;
    }
    if ((argument == "--aptos" || argument == "--language-version" || argument == "--bytecode-version" || argument == "--dependency" ||
         argument == "--external-package" || argument == "--framework") &&
        index + 1 >= argc) {
      throw movescape::Error(movescape::ErrorCode::InvalidArgument, movescape::Error::UnknownOffset, std::string(argument) + " requires a value");
    }
    if (argument == "--aptos") {
      options.executable = argv[++index];
      executable_set = true;
    } else if (argument == "--language-version") {
      options.language_version = argv[++index];
    } else if (argument == "--bytecode-version") {
      std::uint32_t value = 0;
      const std::string_view text = argv[++index];
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw movescape::Error(movescape::ErrorCode::InvalidArgument, movescape::Error::UnknownOffset, "bytecode version is not an unsigned integer");
      }
      options.bytecode_version = value;
    } else if (argument == "--dependency" && dependencies != nullptr) {
      dependencies->emplace_back(argv[++index]);
    } else if ((argument == "--external-package" || argument == "--framework") && external_packages != nullptr) {
      external_packages->emplace_back(argv[++index]);
    } else {
      throw movescape::Error(movescape::ErrorCode::InvalidArgument, movescape::Error::UnknownOffset, "unknown compiler option: " + std::string(argument));
    }
  }
  return options;
}

int inspect(const std::filesystem::path &path, bool script) {
  const auto bytes = movescape::readBinaryFile(path);
  const auto kind = script ? movescape::BinaryKind::Script : movescape::BinaryKind::Module;
  const auto envelope = movescape::parseEnvelope(bytes, kind);

  std::cout << "kind: " << (script ? "script" : "module") << "-envelope\n";
  std::cout << "file: " << path.string() << '\n';
  std::cout << "size: " << bytes.size() << '\n';
  std::cout << "version: " << envelope.version << '\n';
  std::cout << "raw-version: 0x" << std::hex << std::setw(8) << std::setfill('0') << envelope.raw_version << std::dec << '\n';
  std::cout << "tables: " << envelope.tables.size() << '\n';
  std::cout << "table-content-offset: " << envelope.table_content_offset << '\n';
  std::cout << "table-content-size: " << envelope.table_content_size << '\n';
  std::cout << (script ? "script" : "module") << "-footer-offset: " << envelope.footer_offset << '\n';

  for (std::size_t index = 0; index < envelope.tables.size(); ++index) {
    const auto &table = envelope.tables[index];
    std::cout << "table[" << index << "]: " << movescape::format::tableKindName(table.kind) << " offset=" << table.offset << " size=" << table.size << '\n';
  }
  return 0;
}

int printModule(const std::filesystem::path &path) {
  const auto bytes = movescape::readBinaryFile(path);
  const auto module = movescape::loadModule(bytes);
  movescape::validateModule(module);
  std::size_t instruction_count = 0;
  for (const auto &function : module.function_definitions) {
    if (function.code.has_value()) {
      instruction_count += function.code->code.size();
    }
  }

  std::cout << "kind: module\n";
  std::cout << "file: " << path.string() << '\n';
  std::cout << "version: " << module.version << '\n';
  std::cout << "self-module-handle: " << module.self_module_handle << '\n';
  std::cout << "module-handles: " << module.module_handles.size() << '\n';
  std::cout << "struct-handles: " << module.struct_handles.size() << '\n';
  std::cout << "function-handles: " << module.function_handles.size() << '\n';
  std::cout << "signatures: " << module.signatures.size() << '\n';
  std::cout << "constants: " << module.constants.size() << '\n';
  std::cout << "identifiers: " << module.identifiers.size() << '\n';
  std::cout << "addresses: " << module.addresses.size() << '\n';
  std::cout << "struct-definitions: " << module.struct_definitions.size() << '\n';
  std::cout << "function-definitions: " << module.function_definitions.size() << '\n';
  std::cout << "instructions: " << instruction_count << '\n';
  return 0;
}

int printMetadata(const std::filesystem::path &path, movescape::MetadataDecoderVersion decoder, bool script) {
  const auto bytes = movescape::readBinaryFile(path);
  std::vector<movescape::DecodedMetadata> metadata;
  if (script) {
    const auto decoded = movescape::loadScript(bytes);
    movescape::validateScript(decoded);
    metadata = movescape::decodeMetadata(decoded.common.metadata, decoder);
  } else {
    const auto decoded = movescape::loadModule(bytes);
    movescape::validateModule(decoded);
    metadata = movescape::decodeMetadata(decoded.metadata, decoder);
  }
  std::cout << "kind: " << (script ? "script" : "module") << "-metadata\n"
            << "file: " << path.string() << '\n'
            << movescape::formatMetadata(metadata, decoder);
  return 0;
}

int disassemble(const std::filesystem::path &path, bool script) {
  const auto bytes = movescape::readBinaryFile(path);
  if (script) {
    const auto decoded = movescape::loadScript(bytes);
    movescape::validateScript(decoded);
    std::cout << movescape::disassembleScript(decoded);
    return 0;
  }
  const auto module = movescape::loadModule(bytes);
  movescape::validateModule(module);
  std::cout << movescape::disassembleModule(module);
  return 0;
}

int printSymbols(const std::filesystem::path &path) {
  const auto bytes = movescape::readBinaryFile(path);
  const auto module = movescape::loadModule(bytes);
  std::cout << movescape::formatSemanticModel(movescape::buildSemanticModel(module));
  return 0;
}

int compareInterfaces(const std::filesystem::path &reference_path, const std::filesystem::path &candidate_path) {
  const auto reference_bytes = movescape::readBinaryFile(reference_path);
  const auto candidate_bytes = movescape::readBinaryFile(candidate_path);
  const auto reference = movescape::loadModule(reference_bytes);
  const auto candidate = movescape::loadModule(candidate_bytes);
  const auto comparison = movescape::compareModuleInterfaces(reference, candidate);
  std::cout << movescape::formatModuleInterfaceComparison(comparison);
  return comparison.equivalent() ? 0 : 1;
}

void printRoundTripPackage(const movescape::RoundTripPackage &package) {
  std::cout << "kind: round-trip-package\n"
            << "root: " << package.root.string() << '\n'
            << "manifest: " << package.manifest.string() << '\n'
            << "source: " << package.source.string() << '\n'
            << "minimum-bytecode-version: " << package.emission.policy.minimum_bytecode_version << '\n'
            << "minimum-language-version: " << package.emission.policy.minimum_language_version << '\n'
            << "control-flow-complete: " << (package.emission.allControlFlowComplete() ? "yes" : "no") << '\n'
            << "source-semantics-complete: " << (package.emission.allSourceSemanticsComplete() ? "yes" : "no") << '\n'
            << "external-modules: " << package.external_modules.size() << '\n';
  for (const auto &dependency : package.external_modules) {
    std::cout << "  " << dependency << '\n';
  }
  std::cout << "ready-for-compiler-attempt: " << (package.readyForCompilerAttempt() ? "yes" : "no") << '\n';
}

int prepareRoundTrip(const std::filesystem::path &module_path, const std::filesystem::path &output_directory) {
  const auto bytes = movescape::readBinaryFile(module_path);
  const auto module = movescape::loadModule(bytes);
  const auto package = movescape::prepareRoundTripPackage(module, output_directory);
  printRoundTripPackage(package);
  return package.readyForCompilerAttempt() ? 0 : 1;
}

int executeRoundTrip(const std::filesystem::path &module_path, const std::filesystem::path &output_directory, const movescape::AptosCompilerOptions &options) {
  const auto bytes = movescape::readBinaryFile(module_path);
  const auto module = movescape::loadModule(bytes);
  const auto result = movescape::runRoundTrip(module, output_directory, options);
  printRoundTripPackage(result.package);
  if (!result.compiler.has_value()) {
    std::cout << "compiler-attempt: skipped\n";
    return 1;
  }

  std::cout << "compiler-attempt: completed\n"
            << "compiler-exit-code: " << result.compiler->exit_code << '\n'
            << "compiler-terminated-by-signal: " << (result.compiler->terminated_by_signal ? "yes" : "no") << '\n'
            << "compiler-timed-out: " << (result.compiler->timed_out ? "yes" : "no") << '\n'
            << "compiler-log: " << result.compiler->log.string() << '\n'
            << "compiler-output-modules: " << result.compiler->bytecode_modules.size() << '\n';
  if (!result.compiler->output.empty()) {
    std::cout << "compiler-output:\n" << result.compiler->output;
    if (result.compiler->output.back() != '\n') {
      std::cout << '\n';
    }
    if (result.compiler->output_truncated) {
      std::cout << "[compiler output truncated]\n";
    }
  }
  if (!result.compiler->succeeded()) {
    return 1;
  }
  if (!result.candidate_module.has_value() || !result.interface_comparison.has_value()) {
    std::cout << "candidate-module: <not found>\n";
    return 1;
  }

  std::cout << "candidate-module: " << result.candidate_module->string() << '\n' << movescape::formatModuleInterfaceComparison(*result.interface_comparison);
  if (result.body_comparison.has_value()) {
    std::cout << "normalized-body-equivalent: " << (result.body_comparison->equivalent() ? "yes" : "no") << '\n'
              << "normalized-body-differences: " << result.body_comparison->differences.size() << '\n';
  }
  return result.interfaceEquivalent() ? 0 : 1;
}

int executeRoundTripCorpus(const std::filesystem::path &input_directory, const std::filesystem::path &output_directory,
                           const movescape::AptosCompilerOptions &options) {
  const auto corpus = movescape::runRoundTripCorpus(input_directory, output_directory, options);
  std::cout << "kind: round-trip-corpus\n"
            << "root: " << corpus.root.string() << '\n'
            << "modules: " << corpus.entries.size() << '\n';
  for (std::size_t index = 0; index < corpus.entries.size(); ++index) {
    const auto &entry = corpus.entries[index];
    std::cout << "module[" << index << "]: " << entry.input.string() << '\n'
              << "  compiler-accepted: " << (entry.result.compiler.has_value() && entry.result.compiler->succeeded() ? "yes" : "no") << '\n'
              << "  interface-equivalent: " << (entry.result.interfaceEquivalent() ? "yes" : "no") << '\n'
              << "  normalized-body-equivalent: " << (entry.result.bodyEquivalent() ? "yes" : "no") << '\n';
  }
  std::cout << "all-interface-equivalent: " << (corpus.allInterfaceEquivalent() ? "yes" : "no") << '\n';
  return corpus.allInterfaceEquivalent() ? 0 : 1;
}

int executeRoundTripPackage(const std::vector<std::filesystem::path> &input_directories, const std::vector<std::filesystem::path> &external_packages,
                            const std::filesystem::path &output_directory, const movescape::AptosCompilerOptions &options) {
  const auto result = movescape::runRoundTripPackage(input_directories, external_packages, output_directory, options);
  std::uint32_t minimum_bytecode_version = 5;
  std::string minimum_language_version = "2.0";
  for (const auto &emission : result.package.emissions) {
    minimum_bytecode_version = std::max(minimum_bytecode_version, emission.policy.minimum_bytecode_version);
    if (emission.policy.minimum_language_version > minimum_language_version) {
      minimum_language_version = emission.policy.minimum_language_version;
    }
  }
  std::cout << "kind: multi-module-round-trip\n"
            << "root: " << result.package.root.string() << '\n'
            << "modules: " << result.package.module_identities.size() << '\n'
            << "minimum-bytecode-version: " << minimum_bytecode_version << '\n'
            << "minimum-language-version: " << minimum_language_version << '\n'
            << "external-compiler-packages: " << result.package.compiler_dependencies.size() << '\n'
            << "unresolved-external-modules: " << result.package.unresolved_external_modules.size() << '\n';
  for (const auto &dependency : result.package.unresolved_external_modules) {
    std::cout << "  " << dependency << '\n';
  }
  for (const auto &dependency : result.package.compiler_dependencies) {
    std::cout << "external-package: " << dependency.name << " at " << dependency.root.string() << '\n';
  }
  std::cout << "ready-for-compiler-attempt: " << (result.package.readyForCompilerAttempt() ? "yes" : "no") << '\n';
  if (!result.compiler.has_value()) {
    std::cout << "compiler-attempt: skipped\n";
    return 1;
  }
  std::cout << "compiler-exit-code: " << result.compiler->exit_code << '\n'
            << "compiler-timed-out: " << (result.compiler->timed_out ? "yes" : "no") << '\n'
            << "compiler-log: " << result.compiler->log.string() << '\n';
  for (const auto &entry : result.modules) {
    std::cout << "module: " << entry.identity << '\n'
              << "  candidate: " << (entry.candidate_module.has_value() ? entry.candidate_module->string() : "<not found>") << '\n'
              << "  interface-equivalent: " << (entry.interfaceEquivalent() ? "yes" : "no") << '\n'
              << "  normalized-body-equivalent: " << (entry.body_comparison.has_value() && entry.body_comparison->equivalent() ? "yes" : "no") << '\n';
  }
  std::cout << "all-interfaces-equivalent: " << (result.allInterfacesEquivalent() ? "yes" : "no") << '\n';
  return result.allInterfacesEquivalent() ? 0 : 1;
}

int compareBodies(const std::filesystem::path &reference_path, const std::filesystem::path &candidate_path) {
  const auto reference = movescape::loadModule(movescape::readBinaryFile(reference_path));
  const auto candidate = movescape::loadModule(movescape::readBinaryFile(candidate_path));
  const auto comparison = movescape::compareModuleBodies(reference, candidate);
  std::cout << movescape::formatModuleBodyComparison(comparison);
  return comparison.equivalent() ? 0 : 1;
}

void printTestAttempt(std::string_view label, const movescape::CompilerAttempt &attempt) {
  std::cout << label << "-exit-code: " << attempt.exit_code << '\n'
            << label << "-timed-out: " << (attempt.timed_out ? "yes" : "no") << '\n'
            << label << "-log: " << attempt.log.string() << '\n';
  if (!attempt.output.empty()) {
    std::cout << label << "-output:\n" << attempt.output;
    if (attempt.output.back() != '\n') {
      std::cout << '\n';
    }
    if (attempt.output_truncated) {
      std::cout << '[' << label << " output truncated]\n";
    }
  }
}

int compareBehavior(const std::filesystem::path &reference_package, const std::filesystem::path &candidate_package,
                    const std::filesystem::path &output_directory, const movescape::AptosCompilerOptions &options, bool require_passing_cases) {
  const auto comparison = movescape::runBehavioralTests(reference_package, candidate_package, output_directory, options);
  std::cout << "kind: behavioral-test-comparison\n"
            << "shared-harnesses: " << comparison.shared_harnesses.size() << '\n';
  for (const auto &harness : comparison.shared_harnesses) {
    std::cout << "  " << harness.string() << '\n';
  }
  printTestAttempt("reference", comparison.reference);
  printTestAttempt("candidate", comparison.candidate);
  std::cout << "all-shared-cases-passed: " << (comparison.allCasesPassed() ? "yes" : "no") << '\n';
  std::cout << "all-observed-outcomes-equivalent: " << (comparison.allObservedOutcomesEquivalent() ? "yes" : "no") << '\n';
  std::cout << "observations-equivalent: " << (comparison.observationsEquivalent() ? "yes" : "no") << '\n'
            << "observed-cases: " << comparison.reference_trace.cases.size() << '\n'
            << "abort-observations: " << comparison.reference_trace.aborts.size() << '\n'
            << "storage-snapshots: " << comparison.reference_trace.storage_snapshots.size() << '\n';
  return (require_passing_cases ? comparison.allCasesPassed() : comparison.allObservedOutcomesEquivalent()) ? 0 : 1;
}

int generateBehaviorHarness(const std::filesystem::path &module_path, const std::optional<std::filesystem::path> &output_path) {
  const auto module = movescape::loadModule(movescape::readBinaryFile(module_path));
  movescape::validateModule(module);
  const auto harness = movescape::generateBehaviorHarness(module);
  if (harness.probes.empty()) {
    throw movescape::Error(movescape::ErrorCode::UnsupportedFeature, movescape::Error::UnknownOffset, "module has no eligible bounded Boolean functions");
  }
  if (!output_path.has_value()) {
    std::cout << harness.source;
    return 0;
  }
  std::ofstream output(*output_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw movescape::Error(movescape::ErrorCode::Io, movescape::Error::UnknownOffset, "unable to open output file: " + output_path->string());
  }
  output.write(harness.source.data(), static_cast<std::streamsize>(harness.source.size()));
  if (!output) {
    throw movescape::Error(movescape::ErrorCode::Io, movescape::Error::UnknownOffset, "unable to write output file: " + output_path->string());
  }
  std::cout << "generated-probes: " << harness.probes.size() << '\n'
            << "stateful-scenarios: " << harness.stateful_scenarios.size() << '\n'
            << "skipped-functions: " << harness.skipped_functions.size() << '\n'
            << "skipped-stateful-resources: " << harness.skipped_stateful_resources.size() << '\n';
  return 0;
}

int decompile(const std::filesystem::path &path, const std::optional<std::filesystem::path> &output_path, const std::optional<std::filesystem::path> &source_map_path, bool script) {
  const auto bytes = movescape::readBinaryFile(path);
  movescape::MoveEmission result;
  if (script) {
    auto decoded = movescape::loadScript(bytes);
    if (source_map_path.has_value()) {
      decoded = movescape::withSourceMapNames(decoded, movescape::loadSourceMap(movescape::readBinaryFile(*source_map_path)));
    }
    result = movescape::emitMoveScript(decoded);
  } else {
    auto module = movescape::loadModule(bytes);
    movescape::validateModule(module);
    if (source_map_path.has_value()) {
      const auto source_map_bytes = movescape::readBinaryFile(*source_map_path);
      module = movescape::withSourceMapNames(module, movescape::loadSourceMap(source_map_bytes));
    }
    result = movescape::emitMoveModule(module);
  }
  if (output_path.has_value()) {
    std::ofstream output(*output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw movescape::Error(movescape::ErrorCode::Io, movescape::Error::UnknownOffset, "unable to open output file: " + output_path->string());
    }
    output.write(result.source.data(), static_cast<std::streamsize>(result.source.size()));
    if (!output) {
      throw movescape::Error(movescape::ErrorCode::Io, movescape::Error::UnknownOffset, "unable to write output file: " + output_path->string());
    }
  } else {
    std::cout << result.source;
  }
  if (!result.allControlFlowComplete()) {
    std::cerr << "warning: one or more functions used a control-flow fallback\n";
  }
  if (!result.allSourceSemanticsComplete()) {
    std::cerr << "warning: one or more bytecode operations were not exactly "
                 "representable in Move source\n";
  }
  return 0;
}

std::size_t parseIndex(std::string_view text) {
  std::size_t result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw movescape::Error(movescape::ErrorCode::InvalidIndex, movescape::Error::UnknownOffset, "function index is not a decimal integer");
  }
  return result;
}

int printSourceLocation(const std::filesystem::path &path, std::string_view function_text, std::string_view offset_text) {
  const auto source_map = movescape::loadSourceMap(movescape::readBinaryFile(path));
  const auto function_index = parseIndex(function_text);
  const auto offset = parseIndex(offset_text);
  if (function_index >= source_map.functions.size() || !source_map.functions[function_index].has_value()) {
    throw movescape::Error(movescape::ErrorCode::InvalidIndex, movescape::Error::UnknownOffset, "source map has no such function definition");
  }
  if (offset > std::numeric_limits<movescape::CodeOffset>::max()) {
    throw movescape::Error(movescape::ErrorCode::InvalidIndex, movescape::Error::UnknownOffset, "code offset exceeds the bytecode offset range");
  }
  const auto location = movescape::sourceLocationAt(*source_map.functions[function_index], static_cast<movescape::CodeOffset>(offset));
  if (!location.has_value()) {
    throw movescape::Error(movescape::ErrorCode::InvalidIndex, movescape::Error::UnknownOffset, "no source location precedes this code offset");
  }
  std::cout << "kind: source-location\n"
            << "function-definition: " << function_index << '\n'
            << "requested-code-offset: " << offset << '\n'
            << "file-hash: ";
  for (const auto byte : location->file_hash) {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
  }
  std::cout << std::dec << '\n' << "source-start: " << location->start << '\n' << "source-end: " << location->end << '\n';
  return 0;
}

enum class FunctionOutput {
  Cfg,
  Dot,
  Analysis,
  Lift,
  Dataflow,
  Expressions,
  Structure,
};

int printFunction(const std::filesystem::path &path, std::string_view index_text, FunctionOutput output) {
  const auto bytes = movescape::readBinaryFile(path);
  const auto module = movescape::loadModule(bytes);
  movescape::validateModule(module);
  const auto index = parseIndex(index_text);
  if (index >= module.function_definitions.size()) {
    throw movescape::Error(movescape::ErrorCode::InvalidIndex, movescape::Error::UnknownOffset, "function definition index is outside the module");
  }
  const auto &definition = module.function_definitions[index];
  if (!definition.code.has_value()) {
    throw movescape::Error(movescape::ErrorCode::InvalidIndex, movescape::Error::UnknownOffset, "selected function is native and has no code");
  }
  const auto &unit = *definition.code;
  const auto graph = movescape::buildControlFlowGraph(unit);
  if (output == FunctionOutput::Analysis) {
    std::cout << "function-definition #" << index << '\n' << movescape::formatGraphAnalysis(movescape::analyzeControlFlowGraph(graph));
  } else if (output == FunctionOutput::Lift) {
    std::cout << "function-definition #" << index << '\n' << movescape::formatStacklessFunction(module, movescape::liftToStackless(module, definition, graph));
  } else if (output == FunctionOutput::Dataflow) {
    std::cout << "function-definition #" << index << '\n'
              << movescape::formatLocalAnalysis(module, definition, movescape::analyzeLocals(module, definition, graph));
  } else if (output == FunctionOutput::Expressions) {
    const auto stackless = movescape::liftToStackless(module, definition, graph);
    std::cout << "function-definition #" << index << '\n'
              << movescape::formatExpressionFunction(module, movescape::recoverExpressions(module, definition, graph, stackless));
  } else if (output == FunctionOutput::Structure) {
    const auto stackless = movescape::liftToStackless(module, definition, graph);
    const auto expressions = movescape::recoverExpressions(module, definition, graph, stackless);
    std::cout << "function-definition #" << index << '\n'
              << movescape::formatStructuredFunction(module, movescape::structureControlFlow(graph, movescape::analyzeControlFlowGraph(graph), expressions));
  } else if (output == FunctionOutput::Dot) {
    std::cout << movescape::controlFlowGraphDot(module, unit, graph);
  } else {
    std::cout << "function-definition #" << index << '\n' << movescape::formatControlFlowGraph(module, unit, graph);
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      printUsage(std::cout);
      return 0;
    }
    if (argc < 3) {
      printUsage(std::cerr);
      return 2;
    }

    const std::string_view command(argv[1]);
    if (command == "inspect") {
      if (argc != 3 && !(argc == 4 && std::string_view(argv[3]) == "--script")) {
        printUsage(std::cerr);
        return 2;
      }
      return inspect(argv[2], argc == 4);
    }
    if (command == "module") {
      if (argc != 3) {
        printUsage(std::cerr);
        return 2;
      }
      return printModule(argv[2]);
    }
    if (command == "metadata") {
      bool script = false;
      auto decoder = movescape::MetadataDecoderVersion::RawOnly;
      for (int index = 3; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--script") {
          script = true;
        } else if (argument == "--decode-aptos-v1") {
          decoder = movescape::MetadataDecoderVersion::AptosV1;
        } else {
          printUsage(std::cerr);
          return 2;
        }
      }
      return printMetadata(argv[2], decoder, script);
    }
    if (command == "symbols") {
      if (argc != 3) {
        printUsage(std::cerr);
        return 2;
      }
      return printSymbols(argv[2]);
    }
    if (command == "compare-interface") {
      if (argc != 4) {
        printUsage(std::cerr);
        return 2;
      }
      return compareInterfaces(argv[2], argv[3]);
    }
    if (command == "compare-bodies") {
      if (argc != 4) {
        printUsage(std::cerr);
        return 2;
      }
      return compareBodies(argv[2], argv[3]);
    }
    if (command == "compare-behavior") {
      if (argc < 5) {
        printUsage(std::cerr);
        return 2;
      }
      return compareBehavior(argv[2], argv[3], argv[4], parseCompilerOptions(argc, argv, 5), true);
    }
    if (command == "compare-behavior-outcomes") {
      if (argc < 5) {
        printUsage(std::cerr);
        return 2;
      }
      return compareBehavior(argv[2], argv[3], argv[4], parseCompilerOptions(argc, argv, 5), false);
    }
    if (command == "generate-behavior-harness") {
      if (argc != 3 && argc != 4) {
        printUsage(std::cerr);
        return 2;
      }
      return generateBehaviorHarness(argv[2], argc == 4 ? std::optional<std::filesystem::path>{argv[3]} : std::nullopt);
    }
    if (command == "round-trip-prepare") {
      if (argc != 4) {
        printUsage(std::cerr);
        return 2;
      }
      return prepareRoundTrip(argv[2], argv[3]);
    }
    if (command == "round-trip") {
      if (argc < 4) {
        printUsage(std::cerr);
        return 2;
      }
      return executeRoundTrip(argv[2], argv[3], parseCompilerOptions(argc, argv, 4));
    }
    if (command == "round-trip-corpus") {
      if (argc < 4) {
        printUsage(std::cerr);
        return 2;
      }
      return executeRoundTripCorpus(argv[2], argv[3], parseCompilerOptions(argc, argv, 4));
    }
    if (command == "round-trip-package") {
      if (argc < 4) {
        printUsage(std::cerr);
        return 2;
      }
      std::vector<std::filesystem::path> inputs{argv[2]};
      std::vector<std::filesystem::path> dependencies;
      std::vector<std::filesystem::path> external_packages;
      const auto options = parseCompilerOptions(argc, argv, 4, &dependencies, &external_packages);
      inputs.insert(inputs.end(), dependencies.begin(), dependencies.end());
      return executeRoundTripPackage(inputs, external_packages, argv[3], options);
    }
    if (command == "disassemble") {
      if (argc != 3 && !(argc == 4 && std::string_view(argv[3]) == "--script")) {
        printUsage(std::cerr);
        return 2;
      }
      return disassemble(argv[2], argc == 4);
    }
    if (command == "decompile") {
      std::optional<std::filesystem::path> output;
      std::optional<std::filesystem::path> source_map;
      bool script = false;
      for (int index = 3; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--script") {
          script = true;
        } else if (argument == "--source-map" && index + 1 < argc) {
          source_map = argv[++index];
        } else if (!argument.starts_with("--") && !output.has_value()) {
          output = argv[index];
        } else {
          printUsage(std::cerr);
          return 2;
        }
      }
      return decompile(argv[2], output, source_map, script);
    }
    if (command == "source-location") {
      if (argc != 5) {
        printUsage(std::cerr);
        return 2;
      }
      return printSourceLocation(argv[2], argv[3], argv[4]);
    }
    if (command == "cfg" || command == "cfg-dot" || command == "analyze" || command == "lift" || command == "dataflow" || command == "expressions" ||
        command == "structure") {
      if (argc != 4) {
        printUsage(std::cerr);
        return 2;
      }
      auto output = FunctionOutput::Cfg;
      if (command == "cfg-dot") {
        output = FunctionOutput::Dot;
      } else if (command == "analyze") {
        output = FunctionOutput::Analysis;
      } else if (command == "lift") {
        output = FunctionOutput::Lift;
      } else if (command == "dataflow") {
        output = FunctionOutput::Dataflow;
      } else if (command == "expressions") {
        output = FunctionOutput::Expressions;
      } else if (command == "structure") {
        output = FunctionOutput::Structure;
      }
      return printFunction(argv[2], argv[3], output);
    }

    std::cerr << "error: unknown command '" << command << "'\n";
    printUsage(std::cerr);
    return 2;
  } catch (const movescape::Error &error) {
    std::cerr << "error[" << movescape::errorCodeName(error.code()) << ']';
    if (error.hasOffset()) {
      std::cerr << " at byte " << error.offset();
    }
    std::cerr << ": " << error.what() << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "internal error: " << error.what() << '\n';
    return 1;
  }
}
