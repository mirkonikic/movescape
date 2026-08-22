#include "movescape/behavior.hpp"

#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"
#include "movescape/source_names.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace movescape {

namespace {

constexpr std::uint64_t ProbeAbortBase = 0x4d4f560000000000ULL;

[[nodiscard]] std::optional<std::vector<std::string>> inputDomain(const Type &type) {
  switch (type.kind) {
  case TypeKind::Bool:
    return std::vector<std::string>{"false", "true"};
  case TypeKind::U8:
    return std::vector<std::string>{"0u8", "1u8", "255u8"};
  case TypeKind::U16:
    return std::vector<std::string>{"0u16", "1u16", "65535u16"};
  case TypeKind::U32:
    return std::vector<std::string>{"0u32", "1u32", "4294967295u32"};
  case TypeKind::U64:
    return std::vector<std::string>{"0u64", "1u64", "18446744073709551615u64"};
  case TypeKind::Address:
    return std::vector<std::string>{"@0x0", "@0x1", "@0x42"};
  case TypeKind::U128:
  case TypeKind::U256:
  case TypeKind::I8:
  case TypeKind::I16:
  case TypeKind::I32:
  case TypeKind::I64:
  case TypeKind::I128:
  case TypeKind::I256:
  case TypeKind::Signer:
  case TypeKind::Vector:
  case TypeKind::Reference:
  case TypeKind::MutableReference:
  case TypeKind::Struct:
  case TypeKind::StructInstantiation:
  case TypeKind::TypeParameter:
  case TypeKind::Function:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> unsignedReturnWidth(const Type &type) noexcept {
  switch (type.kind) {
  case TypeKind::U8:
    return 8;
  case TypeKind::U16:
    return 16;
  case TypeKind::U32:
    return 32;
  case TypeKind::U64:
    return 64;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::string unsignedSuffix(TypeKind kind) {
  switch (kind) {
  case TypeKind::U8:
    return "u8";
  case TypeKind::U16:
    return "u16";
  case TypeKind::U32:
    return "u32";
  case TypeKind::U64:
    return "u64";
  default:
    throw Error(ErrorCode::InvalidArgument, Error::UnknownOffset, "behavior result is not a supported unsigned integer");
  }
}

[[nodiscard]] std::string unsignedLiteral(TypeKind kind, std::uint64_t value) { return std::to_string(value) + unsignedSuffix(kind); }

[[nodiscard]] bool isSignerReference(const Type &type) {
  return type.kind == TypeKind::Reference && type.arguments.size() == 1 && type.arguments.front().kind == TypeKind::Signer;
}

enum class ResourceOperationKind {
  Publish,
  Exists,
  Read,
  Mutate,
  Remove,
};

struct ResourceOperation {
  ResourceOperationKind kind = ResourceOperationKind::Publish;
  TableIndex resource = 0;

  friend bool operator<(const ResourceOperation &left, const ResourceOperation &right) {
    return std::pair{left.kind, left.resource} < std::pair{right.kind, right.resource};
  }
};

[[nodiscard]] std::optional<ResourceOperation> resourceOperation(const Module &module, const Instruction &instruction) {
  if (instruction.operands.empty()) {
    return std::nullopt;
  }
  const auto direct = static_cast<TableIndex>(instruction.operands.front());
  const auto generic = [&]() { return module.struct_definition_instantiations.at(direct).definition; };
  switch (instruction.opcode) {
  case Opcode::MoveTo:
    return ResourceOperation{ResourceOperationKind::Publish, direct};
  case Opcode::MoveToGeneric:
    return ResourceOperation{ResourceOperationKind::Publish, generic()};
  case Opcode::Exists:
    return ResourceOperation{ResourceOperationKind::Exists, direct};
  case Opcode::ExistsGeneric:
    return ResourceOperation{ResourceOperationKind::Exists, generic()};
  case Opcode::ImmBorrowGlobal:
    return ResourceOperation{ResourceOperationKind::Read, direct};
  case Opcode::ImmBorrowGlobalGeneric:
    return ResourceOperation{ResourceOperationKind::Read, generic()};
  case Opcode::MutBorrowGlobal:
    return ResourceOperation{ResourceOperationKind::Mutate, direct};
  case Opcode::MutBorrowGlobalGeneric:
    return ResourceOperation{ResourceOperationKind::Mutate, generic()};
  case Opcode::MoveFrom:
    return ResourceOperation{ResourceOperationKind::Remove, direct};
  case Opcode::MoveFromGeneric:
    return ResourceOperation{ResourceOperationKind::Remove, generic()};
  default:
    return std::nullopt;
  }
}

struct LifecycleCandidate {
  std::size_t definition = 0;
  TableIndex handle = 0;
  Type value_type;
};

struct LifecycleCandidates {
  std::vector<LifecycleCandidate> publishers;
  std::vector<LifecycleCandidate> existence_checkers;
  std::vector<LifecycleCandidate> readers;
  std::vector<LifecycleCandidate> mutators;
  std::vector<LifecycleCandidate> removers;
};

void renderStatefulBooleanProbe(std::ostringstream &body, std::string_view attributes, std::string_view prelude, std::string_view expression,
                                const BehaviorProbe &probe) {
  body << "  #[test" << attributes << "]\n"
       << "  fun " << probe.test_name << "(";
  if (!attributes.empty()) {
    body << "account: signer";
  }
  body << ") {\n"
       << prelude << "    let movescape_result = " << expression << ";\n"
       << "    if (move movescape_result) {\n"
       << "      abort " << probe.true_abort_code << "u64\n"
       << "    } else {\n"
       << "      abort " << probe.false_abort_code << "u64\n"
       << "    }\n"
       << "  }\n\n";
}

void renderStatefulUnsignedProbe(std::ostringstream &body, std::string_view attributes, std::string_view parameters, std::string_view prelude,
                                 std::string_view expression, TypeKind result_kind, const BehaviorProbe &probe) {
  const auto mask = std::uint64_t{1} << *probe.result_bit;
  const auto suffix = unsignedSuffix(result_kind);
  body << "  #[test" << attributes << "]\n"
       << "  fun " << probe.test_name << '(' << parameters << ") {\n"
       << prelude << "    let movescape_result = " << expression << ";\n"
       << "    if ((move movescape_result & " << mask << suffix << ") != 0" << suffix << ") {\n"
       << "      abort " << probe.true_abort_code << "u64\n"
       << "    } else {\n"
       << "      abort " << probe.false_abort_code << "u64\n"
       << "    }\n"
       << "  }\n\n";
}

[[nodiscard]] bool exactSourceFunctionName(const Module &module, const FunctionHandle &handle) {
  const auto &name = module.identifiers.at(handle.name);
  return makeMoveSourceIdentifier(name, "function", handle.name) == name;
}

[[nodiscard]] bool exactSourceModuleName(const Module &module) {
  const auto &self = module.module_handles.at(module.self_module_handle);
  const auto &name = module.identifiers.at(self.name);
  return makeMoveSourceIdentifier(name, "module", self.name) == name;
}

[[nodiscard]] std::string renderArguments(const BehaviorProbe &probe) {
  std::ostringstream out;
  for (std::size_t index = 0; index < probe.argument_literals.size(); ++index) {
    if (index != 0) {
      out << ", ";
    }
    out << probe.argument_literals[index];
  }
  return out.str();
}

void renderProbe(std::ostringstream &body, const Module &module, TableIndex function_handle, const Type &return_type, const BehaviorProbe &probe) {
  body << "  #[test]\n"
       << "  fun " << probe.test_name << "() {\n"
       << "    let movescape_result = " << renderSourceFunctionName(module, function_handle) << '(' << renderArguments(probe) << ");\n"
       << "    if (";
  if (probe.observation == BehaviorObservationKind::BooleanReturn) {
    body << "move movescape_result";
  } else {
    const auto bit = *probe.result_bit;
    const auto mask = std::uint64_t{1} << bit;
    const auto suffix = unsignedSuffix(return_type.kind);
    body << "(move movescape_result & " << mask << suffix << ") != 0" << suffix;
  }
  body << ") {\n"
       << "      abort " << probe.true_abort_code << "u64\n"
       << "    } else {\n"
       << "      abort " << probe.false_abort_code << "u64\n"
       << "    }\n"
       << "  }\n\n";
}

} // namespace

GeneratedBehaviorHarness generateBehaviorHarness(const Module &module, const BehaviorGenerationOptions &options) {
  if (options.max_parameters == 0 || options.max_input_cases_per_function == 0 || options.max_probes == 0) {
    throw Error(ErrorCode::InvalidArgument, Error::UnknownOffset, "behavior generation limits must be nonzero");
  }

  GeneratedBehaviorHarness result;
  std::ostringstream body;
  std::size_t ordinal = 0;
  const auto allocateAbortCodes = [&]() {
    if (ordinal > (std::numeric_limits<std::uint64_t>::max() - ProbeAbortBase - 1U) / 2U) {
      throw Error(ErrorCode::ResourceLimit, Error::UnknownOffset, "behavior probe abort-code space is exhausted");
    }
    const auto false_code = ProbeAbortBase + ordinal * 2U;
    ++ordinal;
    return std::pair{false_code, false_code + 1U};
  };
  const bool source_module_name_is_exact = exactSourceModuleName(module);
  for (std::size_t definition_index = 0; definition_index < module.function_definitions.size(); ++definition_index) {
    const auto &definition = module.function_definitions[definition_index];
    const auto &handle = module.function_handles.at(definition.handle);
    const auto function_name = renderFunctionName(module, definition.handle);
    const auto &parameters = module.signatures.at(handle.parameters);
    const auto &returns = module.signatures.at(handle.returns);
    const bool common_eligibility = definition.visibility == Visibility::Public && definition.code.has_value() && handle.type_parameters.empty() &&
                                    definition.acquires.empty() && parameters.size() <= options.max_parameters && returns.size() == 1 &&
                                    source_module_name_is_exact && exactSourceFunctionName(module, handle);
    if (!common_eligibility) {
      result.skipped_functions.push_back(function_name);
      continue;
    }

    std::vector<std::vector<std::string>> domains;
    std::size_t input_case_count = 1;
    bool inputs_supported = true;
    for (const auto &parameter : parameters) {
      auto domain = inputDomain(parameter);
      if (!domain.has_value() || input_case_count > options.max_input_cases_per_function / domain->size()) {
        inputs_supported = false;
        break;
      }
      input_case_count *= domain->size();
      domains.push_back(std::move(*domain));
    }

    const bool boolean_return = returns[0].kind == TypeKind::Bool;
    const auto integer_width = unsignedReturnWidth(returns[0]);
    if (!inputs_supported || input_case_count > options.max_input_cases_per_function || (!boolean_return && !integer_width.has_value())) {
      result.skipped_functions.push_back(function_name + " (unsupported domain)");
      continue;
    }
    const auto observations_per_input = boolean_return ? std::size_t{1} : *integer_width;
    if (input_case_count > (options.max_probes - result.probes.size()) / observations_per_input) {
      result.skipped_functions.push_back(function_name + " (probe limit)");
      continue;
    }

    for (std::size_t input_case = 0; input_case < input_case_count; ++input_case) {
      std::vector<std::string> arguments;
      std::size_t remaining = input_case;
      for (const auto &domain : domains) {
        arguments.push_back(domain[remaining % domain.size()]);
        remaining /= domain.size();
      }
      for (std::size_t observation = 0; observation < observations_per_input; ++observation) {
        BehaviorProbe probe;
        probe.function_definition = definition_index;
        probe.function_name = function_name;
        probe.test_name = "probe_" + std::to_string(definition_index) + "_" + std::to_string(input_case);
        probe.argument_literals = arguments;
        if (boolean_return) {
          probe.observation = BehaviorObservationKind::BooleanReturn;
        } else {
          probe.observation = BehaviorObservationKind::UnsignedReturnBit;
          probe.result_bit = observation;
          probe.test_name += "_bit_" + std::to_string(observation);
        }
        const auto [false_code, true_code] = allocateAbortCodes();
        probe.false_abort_code = false_code;
        probe.true_abort_code = true_code;
        renderProbe(body, module, definition.handle, returns[0], probe);
        result.probes.push_back(std::move(probe));
      }
    }
  }

  std::map<TableIndex, LifecycleCandidates> lifecycle_candidates;
  if (source_module_name_is_exact) {
    for (std::size_t definition_index = 0; definition_index < module.function_definitions.size(); ++definition_index) {
      const auto &definition = module.function_definitions[definition_index];
      const auto &handle = module.function_handles.at(definition.handle);
      if (definition.visibility != Visibility::Public || !definition.code.has_value() || !handle.type_parameters.empty() ||
          !exactSourceFunctionName(module, handle)) {
        continue;
      }
      std::set<ResourceOperation> operations;
      for (const auto &instruction : definition.code->code) {
        const auto operation = resourceOperation(module, instruction);
        if (operation.has_value()) {
          operations.insert(*operation);
        }
      }
      if (operations.size() != 1) {
        continue;
      }

      const auto operation = *operations.begin();
      const auto &parameters = module.signatures.at(handle.parameters);
      const auto &returns = module.signatures.at(handle.returns);
      LifecycleCandidate candidate{
          .definition = definition_index,
          .handle = definition.handle,
          .value_type = {},
      };
      auto &candidates = lifecycle_candidates[operation.resource];
      switch (operation.kind) {
      case ResourceOperationKind::Publish:
        if (parameters.size() == 2 && isSignerReference(parameters[0]) && unsignedReturnWidth(parameters[1]).has_value() && returns.empty()) {
          candidate.value_type = parameters[1];
          candidates.publishers.push_back(std::move(candidate));
        }
        break;
      case ResourceOperationKind::Exists:
        if (parameters.size() == 1 && parameters[0].kind == TypeKind::Address && returns.size() == 1 && returns[0].kind == TypeKind::Bool) {
          candidates.existence_checkers.push_back(std::move(candidate));
        }
        break;
      case ResourceOperationKind::Read:
        if (parameters.size() == 1 && parameters[0].kind == TypeKind::Address && returns.size() == 1 && unsignedReturnWidth(returns[0]).has_value()) {
          candidate.value_type = returns[0];
          candidates.readers.push_back(std::move(candidate));
        }
        break;
      case ResourceOperationKind::Mutate:
        if (parameters.size() == 1 && parameters[0].kind == TypeKind::Address && returns.empty()) {
          candidates.mutators.push_back(std::move(candidate));
        }
        break;
      case ResourceOperationKind::Remove:
        if (parameters.size() == 1 && parameters[0].kind == TypeKind::Address && returns.size() == 1 && unsignedReturnWidth(returns[0]).has_value()) {
          candidate.value_type = returns[0];
          candidates.removers.push_back(std::move(candidate));
        }
        break;
      }
    }
  }

  for (const auto &[resource_index, candidates] : lifecycle_candidates) {
    const auto &resource_definition = module.struct_definitions.at(resource_index);
    const auto &resource_handle = module.struct_handles.at(resource_definition.handle);
    const auto &resource_identifier = module.identifiers.at(resource_handle.name);
    const auto resource_name = renderStructName(module, resource_definition.handle);
    const bool exact_resource_name = makeMoveSourceIdentifier(resource_identifier, "resource", resource_handle.name) == resource_identifier;
    const bool unique_roles = candidates.publishers.size() == 1 && candidates.existence_checkers.size() == 1 && candidates.readers.size() == 1 &&
                              candidates.mutators.size() == 1 && candidates.removers.size() == 1;
    if (!exact_resource_name || !unique_roles) {
      result.skipped_stateful_resources.push_back(resource_name + " (incomplete lifecycle)");
      continue;
    }
    const auto &publisher = candidates.publishers.front();
    const auto &existence = candidates.existence_checkers.front();
    const auto &reader = candidates.readers.front();
    const auto &mutator = candidates.mutators.front();
    const auto &remover = candidates.removers.front();
    if (publisher.value_type != reader.value_type || publisher.value_type != remover.value_type) {
      result.skipped_stateful_resources.push_back(resource_name + " (value type mismatch)");
      continue;
    }
    const auto width = *unsignedReturnWidth(publisher.value_type);
    const auto required_probes = width * 5U + 4U;
    if (result.stateful_scenarios.size() >= options.max_stateful_scenarios || required_probes > options.max_probes - result.probes.size()) {
      result.skipped_stateful_resources.push_back(resource_name + " (probe limit)");
      continue;
    }

    const auto publisher_name = renderSourceFunctionName(module, publisher.handle);
    const auto existence_name = renderSourceFunctionName(module, existence.handle);
    const auto reader_name = renderSourceFunctionName(module, reader.handle);
    const auto mutator_name = renderSourceFunctionName(module, mutator.handle);
    const auto remover_name = renderSourceFunctionName(module, remover.handle);
    const auto prefix = "stateful_" + std::to_string(resource_index) + "_";
    const auto account_attribute = "(account = @0xcafe)";
    const auto initial = unsignedLiteral(publisher.value_type.kind, 41);
    const auto publish = "    " + publisher_name + "(&account, " + initial + ");\n";
    const auto mutate = "    " + mutator_name + "(@0xcafe);\n";
    const auto remove = "    let _movescape_removed = " + remover_name + "(@0xcafe);\n";

    const auto makeProbe = [&](std::size_t definition, std::string test_name, BehaviorObservationKind observation, std::optional<std::size_t> bit) {
      const auto [false_code, true_code] = allocateAbortCodes();
      return BehaviorProbe{
          .function_definition = definition,
          .function_name = renderFunctionName(module, module.function_definitions[definition].handle),
          .test_name = std::move(test_name),
          .argument_literals = {},
          .observation = observation,
          .result_bit = bit,
          .false_abort_code = false_code,
          .true_abort_code = true_code,
      };
    };

    auto before = makeProbe(existence.definition, prefix + "contains_before", BehaviorObservationKind::StatefulBooleanReturn, std::nullopt);
    renderStatefulBooleanProbe(body, {}, {}, existence_name + "(@0xcafe)", before);
    result.probes.push_back(std::move(before));

    auto after_publish = makeProbe(existence.definition, prefix + "contains_after_publish", BehaviorObservationKind::StatefulBooleanReturn, std::nullopt);
    renderStatefulBooleanProbe(body, account_attribute, publish, existence_name + "(@0xcafe)", after_publish);
    result.probes.push_back(std::move(after_publish));

    const auto renderBitSeries = [&](std::string_view label, std::size_t definition, std::string_view attributes, std::string_view parameters,
                                     const std::string &prelude, const std::string &expression) {
      for (std::size_t bit = 0; bit < width; ++bit) {
        auto probe =
            makeProbe(definition, prefix + std::string(label) + "_bit_" + std::to_string(bit), BehaviorObservationKind::StatefulUnsignedReturnBit, bit);
        renderStatefulUnsignedProbe(body, attributes, parameters, prelude, expression, publisher.value_type.kind, probe);
        result.probes.push_back(std::move(probe));
      }
    };
    renderBitSeries("value_after_publish", reader.definition, account_attribute, "account: signer", publish, reader_name + "(@0xcafe)");
    renderBitSeries("value_after_mutation", reader.definition, account_attribute, "account: signer", publish + mutate, reader_name + "(@0xcafe)");
    renderBitSeries("remove_after_mutation", remover.definition, account_attribute, "account: signer", publish + mutate, remover_name + "(@0xcafe)");

    auto after_remove = makeProbe(existence.definition, prefix + "contains_after_remove", BehaviorObservationKind::StatefulBooleanReturn, std::nullopt);
    renderStatefulBooleanProbe(body, account_attribute, publish + remove, existence_name + "(@0xcafe)", after_remove);
    result.probes.push_back(std::move(after_remove));

    const auto isolation_attribute = "(alice = @0xa11ce, bob = @0xb0b)";
    const auto isolation_prelude = "    " + publisher_name + "(&alice, " + unsignedLiteral(publisher.value_type.kind, 5) + ");\n" + "    " + publisher_name +
                                   "(&bob, " + unsignedLiteral(publisher.value_type.kind, 9) + ");\n" + "    " + mutator_name + "(@0xa11ce);\n";
    renderBitSeries("isolated_alice", reader.definition, isolation_attribute, "alice: signer, bob: signer", isolation_prelude, reader_name + "(@0xa11ce)");
    renderBitSeries("isolated_bob", reader.definition, isolation_attribute, "alice: signer, bob: signer", isolation_prelude, reader_name + "(@0xb0b)");

    auto snapshot = makeProbe(mutator.definition, prefix + "storage_after_mutation", BehaviorObservationKind::StatefulStorageSnapshot, std::nullopt);
    body << "  #[test" << account_attribute << "]\n"
         << "  fun " << snapshot.test_name << "(account: signer) {\n"
         << publish << mutate << "    abort " << snapshot.false_abort_code << "u64\n"
         << "  }\n\n";
    result.probes.push_back(std::move(snapshot));

    result.stateful_scenarios.push_back({
        .resource_definition = resource_index,
        .resource_name = resource_name,
        .publisher = publisher_name,
        .existence_checker = existence_name,
        .reader = reader_name,
        .mutator = mutator_name,
        .remover = remover_name,
        .probe_count = required_probes,
    });

    const std::set<std::string> participating{
        renderFunctionName(module, publisher.handle), renderFunctionName(module, existence.handle), renderFunctionName(module, reader.handle),
        renderFunctionName(module, mutator.handle),   renderFunctionName(module, remover.handle),
    };
    result.skipped_functions.erase(
        std::remove_if(result.skipped_functions.begin(), result.skipped_functions.end(), [&](const std::string &name) { return participating.contains(name); }),
        result.skipped_functions.end());
  }

  std::string harness_name = "MovescapeGeneratedBehavior";
  const auto fixed_identity = "0xc0de::" + harness_name;
  if (renderModuleName(module, module.self_module_handle) == fixed_identity) {
    harness_name += "_1";
  }
  std::ostringstream source;
  source << "// Generated by movescape. Every test intentionally aborts so "
            "the VM outcome can be compared.\n"
         << "#[test_only]\n"
         << "module 0xc0de::" << harness_name << " {\n\n"
         << body.str() << "}\n";
  result.source = source.str();
  return result;
}

} // namespace movescape
