#include "movescape/validator.hpp"

#include "movescape/binary_reader.hpp"
#include "movescape/borrow_analysis.hpp"
#include "movescape/cfg.hpp"
#include "movescape/error.hpp"
#include "movescape/local_analysis.hpp"
#include "movescape/opcode.hpp"
#include "movescape/stackless_ir.hpp"

#include <algorithm>
#include <bit>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace movescape {

namespace {

[[noreturn]] void invalid(std::string message) { throw Error(ErrorCode::InvalidIndex, Error::UnknownOffset, std::move(message)); }
[[noreturn]] void typeMismatch(std::string message) { throw Error(ErrorCode::TypeMismatch, Error::UnknownOffset, std::move(message)); }

void requireIndex(std::size_t index, std::size_t size, std::string_view description) {
  if (index >= size) {
    std::ostringstream out;
    out << description << " " << index << " is outside table of size " << size;
    invalid(out.str());
  }
}

template <typename Range, typename Projection> [[nodiscard]] bool containsDuplicateBy(const Range &range, Projection projection) {
  using Value = typename Range::value_type;
  using Key = std::remove_cvref_t<std::invoke_result_t<Projection, const Value &>>;
  std::set<Key> unique;
  for (const auto &value : range) {
    if (!unique.insert(projection(value)).second) {
      return true;
    }
  }
  return false;
}

void appendTypeKey(const Type &type, std::vector<std::uint64_t> &key) {
  key.push_back(static_cast<std::uint64_t>(type.kind));
  key.push_back(type.index);
  key.push_back(type.abilities.bits);
  key.push_back(type.arguments.size());
  for (const auto &argument : type.arguments) {
    appendTypeKey(argument, key);
  }
  key.push_back(type.results.size());
  for (const auto &result : type.results) {
    appendTypeKey(result, key);
  }
}

[[nodiscard]] std::vector<std::uint64_t> signatureKey(const Signature &signature) {
  std::vector<std::uint64_t> key;
  key.push_back(signature.size());
  for (const auto &type : signature) {
    appendTypeKey(type, key);
  }
  return key;
}

void validateAbilitySet(AbilitySet abilities) {
  if ((abilities.bits & static_cast<std::uint8_t>(~AbilitySet::All)) != 0U) {
    invalid("ability set contains unknown bits");
  }
}

[[nodiscard]] AbilitySet requiredByDeclaredAbilities(AbilitySet declared) {
  std::uint8_t result = 0;
  if (declared.has(AbilitySet::Copy)) {
    result = static_cast<std::uint8_t>(result | AbilitySet::Copy);
  }
  if (declared.has(AbilitySet::Drop)) {
    result = static_cast<std::uint8_t>(result | AbilitySet::Drop);
  }
  if (declared.has(AbilitySet::Store) || declared.has(AbilitySet::Key)) {
    result = static_cast<std::uint8_t>(result | AbilitySet::Store);
  }
  return {result};
}

[[nodiscard]] AbilitySet typeAbilities(const Module &module, const Type &type, const std::vector<AbilitySet> &type_parameter_abilities) {
  constexpr std::uint8_t primitive = AbilitySet::Copy | AbilitySet::Drop | AbilitySet::Store;
  switch (type.kind) {
  case TypeKind::Bool:
  case TypeKind::U8:
  case TypeKind::U16:
  case TypeKind::U32:
  case TypeKind::U64:
  case TypeKind::U128:
  case TypeKind::U256:
  case TypeKind::I8:
  case TypeKind::I16:
  case TypeKind::I32:
  case TypeKind::I64:
  case TypeKind::I128:
  case TypeKind::I256:
  case TypeKind::Address:
    return {primitive};
  case TypeKind::Signer:
    return {AbilitySet::Drop};
  case TypeKind::Reference:
  case TypeKind::MutableReference:
    return {static_cast<std::uint8_t>(AbilitySet::Copy | AbilitySet::Drop)};
  case TypeKind::Function:
    return type.abilities;
  case TypeKind::TypeParameter:
    if (type.index >= type_parameter_abilities.size()) {
      return {};
    }
    return type_parameter_abilities[type.index];
  case TypeKind::Vector: {
    auto result = AbilitySet{primitive};
    const auto element = typeAbilities(module, type.arguments.front(), type_parameter_abilities);
    result.bits = static_cast<std::uint8_t>(result.bits & element.bits);
    return result;
  }
  case TypeKind::Struct:
  case TypeKind::StructInstantiation: {
    const auto &handle = module.struct_handles.at(type.index);
    auto result = handle.abilities;
    for (std::size_t index = 0; index < type.arguments.size() && index < handle.type_parameters.size(); ++index) {
      if (handle.type_parameters[index].is_phantom) {
        continue;
      }
      const auto argument = typeAbilities(module, type.arguments[index], type_parameter_abilities);
      for (const auto ability : {AbilitySet::Copy, AbilitySet::Drop, AbilitySet::Store}) {
        if (!argument.has(ability)) {
          result.bits = static_cast<std::uint8_t>(result.bits & ~ability);
        }
      }
      if (!argument.has(AbilitySet::Store)) {
        result.bits = static_cast<std::uint8_t>(result.bits & ~AbilitySet::Key);
      }
    }
    return result;
  }
  }
  return {};
}

[[nodiscard]] bool hasAbilities(AbilitySet actual, AbilitySet required) { return (actual.bits & required.bits) == required.bits; }

void validateNestedTypeConstraints(const Module &module, const Type &type, const std::vector<AbilitySet> &type_parameter_abilities) {
  if (type.kind == TypeKind::StructInstantiation) {
    const auto &handle = module.struct_handles.at(type.index);
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      const auto &argument = type.arguments[index];
      validateNestedTypeConstraints(module, argument, type_parameter_abilities);
      if (!hasAbilities(typeAbilities(module, argument, type_parameter_abilities), handle.type_parameters[index].constraints)) {
        typeMismatch("struct type argument does not satisfy its ability constraint");
      }
    }
    return;
  }
  for (const auto &argument : type.arguments) {
    validateNestedTypeConstraints(module, argument, type_parameter_abilities);
  }
  for (const auto &result : type.results) {
    validateNestedTypeConstraints(module, result, type_parameter_abilities);
  }
}

void validateType(const Module &module, const Type &type, std::size_t type_parameter_count = static_cast<std::size_t>(-1), bool allow_reference = true) {
  validateAbilitySet(type.abilities);
  const auto require_no_children = [&]() {
    if (!type.arguments.empty() || !type.results.empty()) {
      invalid("non-composite type contains child types");
    }
  };
  switch (type.kind) {
  case TypeKind::Bool:
  case TypeKind::U8:
  case TypeKind::U16:
  case TypeKind::U32:
  case TypeKind::U64:
  case TypeKind::U128:
  case TypeKind::U256:
  case TypeKind::I8:
  case TypeKind::I16:
  case TypeKind::I32:
  case TypeKind::I64:
  case TypeKind::I128:
  case TypeKind::I256:
  case TypeKind::Address:
  case TypeKind::Signer:
    require_no_children();
    break;
  case TypeKind::Vector:
    if (type.arguments.size() != 1 || !type.results.empty()) {
      invalid("unary type constructor does not have exactly one argument");
    }
    break;
  case TypeKind::Reference:
  case TypeKind::MutableReference:
    if (!allow_reference) {
      invalid("reference is nested in a type position which forbids references");
    }
    if (type.arguments.size() != 1 || !type.results.empty()) {
      invalid("unary type constructor does not have exactly one argument");
    }
    break;
  case TypeKind::Struct: {
    requireIndex(type.index, module.struct_handles.size(), "struct handle index");
    require_no_children();
    if (!module.struct_handles[type.index].type_parameters.empty()) {
      invalid("generic struct used without required type arguments");
    }
    break;
  }
  case TypeKind::StructInstantiation: {
    requireIndex(type.index, module.struct_handles.size(), "struct handle index");
    if (!type.results.empty() || type.arguments.size() != module.struct_handles[type.index].type_parameters.size()) {
      invalid("struct instantiation has the wrong type argument count");
    }
    break;
  }
  case TypeKind::TypeParameter:
    require_no_children();
    if (type_parameter_count != static_cast<std::size_t>(-1) && type.index >= type_parameter_count) {
      invalid("type parameter index is outside declaration arity");
    }
    break;
  case TypeKind::Function:
    break;
  }
  if (type.kind == TypeKind::Function) {
    for (const auto &argument : type.arguments) {
      validateType(module, argument, type_parameter_count, true);
    }
    for (const auto &result : type.results) {
      validateType(module, result, type_parameter_count, true);
    }
    return;
  }
  for (const auto &argument : type.arguments) {
    validateType(module, argument, type_parameter_count, false);
  }
}

void validateSignature(const Module &module, const Signature &signature, std::size_t type_parameter_count = static_cast<std::size_t>(-1),
                       bool allow_reference = true) {
  for (const auto &type : signature) {
    validateType(module, type, type_parameter_count, allow_reference);
  }
}

[[nodiscard]] bool isValidConstantType(const Type &type) {
  switch (type.kind) {
  case TypeKind::Bool:
  case TypeKind::U8:
  case TypeKind::U16:
  case TypeKind::U32:
  case TypeKind::U64:
  case TypeKind::U128:
  case TypeKind::U256:
  case TypeKind::I8:
  case TypeKind::I16:
  case TypeKind::I32:
  case TypeKind::I64:
  case TypeKind::I128:
  case TypeKind::I256:
  case TypeKind::Address:
    return true;
  case TypeKind::Vector:
    return type.arguments.size() == 1 && isValidConstantType(type.arguments.front());
  case TypeKind::Signer:
  case TypeKind::Reference:
  case TypeKind::MutableReference:
  case TypeKind::Struct:
  case TypeKind::StructInstantiation:
  case TypeKind::TypeParameter:
  case TypeKind::Function:
    return false;
  }
  return false;
}

void consumeConstantValue(BinaryReader &reader, const Type &type, std::size_t depth = 0) {
  if (depth >= 256) {
    invalid("constant BCS nesting exceeds verifier limit");
  }
  switch (type.kind) {
  case TypeKind::Bool:
    if (reader.readU8("constant bool") > 1U) {
      invalid("constant bool is not encoded as zero or one");
    }
    return;
  case TypeKind::U8:
  case TypeKind::I8:
    (void)reader.readU8("constant integer");
    return;
  case TypeKind::U16:
  case TypeKind::I16:
    (void)reader.readU16("constant integer");
    return;
  case TypeKind::U32:
  case TypeKind::I32:
    (void)reader.readU32("constant integer");
    return;
  case TypeKind::U64:
  case TypeKind::I64:
    (void)reader.readU64("constant integer");
    return;
  case TypeKind::U128:
  case TypeKind::I128:
    (void)reader.readU128("constant integer");
    return;
  case TypeKind::U256:
  case TypeKind::I256:
    (void)reader.readU256("constant integer");
    return;
  case TypeKind::Address:
    (void)reader.readBytes(32, "constant address");
    return;
  case TypeKind::Vector: {
    const auto count = reader.readUleb128(std::numeric_limits<std::uint32_t>::max(), "constant vector length");
    for (std::uint64_t index = 0; index < count; ++index) {
      consumeConstantValue(reader, type.arguments.front(), depth + 1);
    }
    return;
  }
  default:
    invalid("constant has a type which cannot appear in the constant pool");
  }
}

void validateConstant(const Module &module, const Constant &constant) {
  validateType(module, constant.type, static_cast<std::size_t>(-1), false);
  if (!isValidConstantType(constant.type)) {
    invalid("constant has a non-primitive type");
  }
  BinaryReader reader(constant.data);
  try {
    consumeConstantValue(reader, constant.type);
  } catch (const Error &error) {
    invalid("constant contains malformed BCS data: " + std::string(error.what()));
  }
  if (!reader.empty()) {
    invalid("constant BCS data contains trailing bytes");
  }
}

void validatePhantomPositions(const Module &module, const Type &type, const std::vector<StructTypeParameter> &declaration_parameters,
                              bool is_phantom_position = false) {
  if (type.kind == TypeKind::TypeParameter) {
    if (type.index < declaration_parameters.size() && declaration_parameters[type.index].is_phantom && !is_phantom_position) {
      invalid("phantom type parameter appears in a non-phantom field position");
    }
    return;
  }
  if (type.kind == TypeKind::StructInstantiation) {
    const auto &handle = module.struct_handles.at(type.index);
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      validatePhantomPositions(module, type.arguments[index], declaration_parameters, handle.type_parameters[index].is_phantom);
    }
    return;
  }
  for (const auto &argument : type.arguments) {
    validatePhantomPositions(module, argument, declaration_parameters, false);
  }
  for (const auto &result : type.results) {
    validatePhantomPositions(module, result, declaration_parameters, false);
  }
}

void collectDefinedStructDependencies(const Type &type, const std::map<TableIndex, std::size_t> &definitions, std::set<std::size_t> &dependencies) {
  if (type.kind == TypeKind::Struct || type.kind == TypeKind::StructInstantiation) {
    const auto definition = definitions.find(type.index);
    if (definition != definitions.end()) {
      dependencies.insert(definition->second);
    }
  }
  for (const auto &argument : type.arguments) {
    collectDefinedStructDependencies(argument, definitions, dependencies);
  }
  for (const auto &result : type.results) {
    collectDefinedStructDependencies(result, definitions, dependencies);
  }
}

void validateNonRecursiveStructDefinitions(const Module &module) {
  std::map<TableIndex, std::size_t> definitions;
  for (std::size_t index = 0; index < module.struct_definitions.size(); ++index) {
    definitions.emplace(module.struct_definitions[index].handle, index);
  }
  std::vector<std::set<std::size_t>> edges(module.struct_definitions.size());
  for (std::size_t index = 0; index < module.struct_definitions.size(); ++index) {
    const auto &definition = module.struct_definitions[index];
    for (const auto &field : definition.fields) {
      collectDefinedStructDependencies(field.type, definitions, edges[index]);
    }
    for (const auto &variant : definition.variants) {
      for (const auto &field : variant.fields) {
        collectDefinedStructDependencies(field.type, definitions, edges[index]);
      }
    }
  }

  std::vector<std::size_t> indegrees(edges.size(), 0);
  for (const auto &dependencies : edges) {
    for (const auto dependency : dependencies) {
      ++indegrees[dependency];
    }
  }
  std::deque<std::size_t> ready;
  for (std::size_t index = 0; index < indegrees.size(); ++index) {
    if (indegrees[index] == 0) {
      ready.push_back(index);
    }
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto node = ready.front();
    ready.pop_front();
    ++visited;
    for (const auto dependency : edges[node]) {
      --indegrees[dependency];
      if (indegrees[dependency] == 0) {
        ready.push_back(dependency);
      }
    }
  }
  if (visited != edges.size()) {
    invalid("module contains a recursive struct definition");
  }
}

[[nodiscard]] bool containsTypeParameter(const Type &type, TableIndex parameter) {
  if (type.kind == TypeKind::TypeParameter && type.index == parameter) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(), [&](const Type &argument) { return containsTypeParameter(argument, parameter); }) ||
         std::any_of(type.results.begin(), type.results.end(), [&](const Type &result) { return containsTypeParameter(result, parameter); });
}

void validateInstantiationLoops(const Module &module) {
  std::map<TableIndex, std::size_t> definitions;
  std::vector<std::size_t> node_bases(module.function_definitions.size() + 1, 0);
  for (std::size_t index = 0; index < module.function_definitions.size(); ++index) {
    definitions.emplace(module.function_definitions[index].handle, index);
    const auto &handle = module.function_handles.at(module.function_definitions[index].handle);
    node_bases[index + 1] = node_bases[index] + handle.type_parameters.size();
  }
  struct Edge {
    std::size_t from = 0;
    std::size_t to = 0;
    bool constructed = false;
  };
  std::vector<Edge> edges;
  for (std::size_t caller_index = 0; caller_index < module.function_definitions.size(); ++caller_index) {
    const auto &definition = module.function_definitions[caller_index];
    if (!definition.code.has_value()) {
      continue;
    }
    const auto &caller = module.function_handles.at(definition.handle);
    for (const auto &instruction : definition.code->code) {
      if (instruction.opcode != Opcode::CallGeneric && instruction.opcode != Opcode::PackClosureGeneric) {
        continue;
      }
      const auto &instantiation = module.function_instantiations.at(static_cast<std::size_t>(instruction.operands.at(0)));
      const auto callee = definitions.find(instantiation.handle);
      if (callee == definitions.end()) {
        continue;
      }
      const auto &arguments = module.signatures.at(instantiation.type_parameters);
      for (std::size_t formal = 0; formal < arguments.size(); ++formal) {
        for (std::size_t actual = 0; actual < caller.type_parameters.size(); ++actual) {
          if (!containsTypeParameter(arguments[formal], static_cast<TableIndex>(actual))) {
            continue;
          }
          const auto identity = arguments[formal].kind == TypeKind::TypeParameter && arguments[formal].index == actual;
          edges.push_back({
              .from = node_bases[caller_index] + actual,
              .to = node_bases[callee->second] + formal,
              .constructed = !identity,
          });
        }
      }
    }
  }

  std::vector<std::vector<std::size_t>> adjacency(node_bases.back());
  std::vector<std::vector<std::size_t>> reverse_adjacency(node_bases.back());
  for (const auto &edge : edges) {
    adjacency[edge.from].push_back(edge.to);
    reverse_adjacency[edge.to].push_back(edge.from);
  }

  std::vector<bool> visited(adjacency.size(), false);
  std::vector<std::size_t> finish_order;
  finish_order.reserve(adjacency.size());
  for (std::size_t root = 0; root < adjacency.size(); ++root) {
    if (visited[root]) {
      continue;
    }
    visited[root] = true;
    std::vector<std::pair<std::size_t, std::size_t>> pending{{root, 0}};
    while (!pending.empty()) {
      auto &[node, next_index] = pending.back();
      if (next_index < adjacency[node].size()) {
        const auto next = adjacency[node][next_index++];
        if (!visited[next]) {
          visited[next] = true;
          pending.emplace_back(next, 0);
        }
      } else {
        finish_order.push_back(node);
        pending.pop_back();
      }
    }
  }

  const auto no_component = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> components(adjacency.size(), no_component);
  std::size_t component_count = 0;
  for (auto iterator = finish_order.rbegin(); iterator != finish_order.rend(); ++iterator) {
    if (components[*iterator] != no_component) {
      continue;
    }
    std::deque<std::size_t> pending{*iterator};
    components[*iterator] = component_count;
    while (!pending.empty()) {
      const auto node = pending.front();
      pending.pop_front();
      for (const auto next : reverse_adjacency[node]) {
        if (components[next] == no_component) {
          components[next] = component_count;
          pending.push_back(next);
        }
      }
    }
    ++component_count;
  }

  for (const auto &edge : edges) {
    if (edge.constructed && components[edge.from] == components[edge.to]) {
      invalid("generic calls form an expanding instantiation loop");
    }
  }
}

void validateInstantiationArguments(const Module &module, const Signature &arguments, const std::vector<AbilitySet> &constraints) {
  if (arguments.size() != constraints.size()) {
    invalid("generic instantiation has the wrong type argument count");
  }
  static const std::vector<AbilitySet> maximally_capable_type_parameters(static_cast<std::size_t>(std::numeric_limits<TableIndex>::max()) + 1U,
                                                                         AbilitySet{AbilitySet::All});
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    validateType(module, arguments[index], static_cast<std::size_t>(-1), false);
    const auto abilities = typeAbilities(module, arguments[index], maximally_capable_type_parameters);
    validateNestedTypeConstraints(module, arguments[index], maximally_capable_type_parameters);
    if (!hasAbilities(abilities, constraints[index])) {
      typeMismatch("generic type argument cannot satisfy its ability constraint");
    }
  }
}

void validateCanonicalPools(const Module &module) {
  if (containsDuplicateBy(module.identifiers, [](const std::string &value) { return value; })) {
    invalid("identifier pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.addresses, [](const Address &value) { return value; })) {
    invalid("address pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.signatures, signatureKey)) {
    invalid("signature pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.constants, [](const Constant &constant) {
        std::vector<std::uint64_t> type_key;
        appendTypeKey(constant.type, type_key);
        return std::pair{std::move(type_key), constant.data};
      })) {
    invalid("constant pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.module_handles, [](const ModuleHandle &handle) { return std::pair{handle.address, handle.name}; })) {
    invalid("module handle pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.struct_handles, [](const StructHandle &handle) { return std::pair{handle.module, handle.name}; })) {
    invalid("struct handle pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.function_handles, [](const FunctionHandle &handle) { return std::pair{handle.module, handle.name}; })) {
    invalid("function handle pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.function_instantiations,
                          [](const FunctionInstantiation &instantiation) { return std::pair{instantiation.handle, instantiation.type_parameters}; })) {
    invalid("function instantiation pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.struct_definition_instantiations, [](const StructDefinitionInstantiation &instantiation) {
        return std::pair{instantiation.definition, instantiation.type_parameters};
      })) {
    invalid("struct instantiation pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.field_handles, [](const FieldHandle &handle) { return std::pair{handle.owner, handle.field}; })) {
    invalid("field handle pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.field_instantiations,
                          [](const FieldInstantiation &instantiation) { return std::pair{instantiation.handle, instantiation.type_parameters}; })) {
    invalid("field instantiation pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.variant_field_handles,
                          [](const VariantFieldHandle &handle) { return std::tuple{handle.owner, handle.variants, handle.field}; })) {
    invalid("variant field handle pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.variant_field_instantiations,
                          [](const VariantFieldInstantiation &instantiation) { return std::pair{instantiation.handle, instantiation.type_parameters}; })) {
    invalid("variant field instantiation pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.struct_variant_handles, [](const StructVariantHandle &handle) { return std::pair{handle.definition, handle.variant}; })) {
    invalid("struct variant handle pool contains a duplicate entry");
  }
  if (containsDuplicateBy(module.struct_variant_instantiations,
                          [](const StructVariantInstantiation &instantiation) { return std::pair{instantiation.handle, instantiation.type_parameters}; })) {
    invalid("struct variant instantiation pool contains a duplicate entry");
  }
}

void validateModuleHandle(const Module &module, const ModuleHandle &handle) {
  requireIndex(handle.address, module.addresses.size(), "address identifier index");
  requireIndex(handle.name, module.identifiers.size(), "module name index");
}

void validateAccessSpecifier(const Module &module, const AccessSpecifier &specifier, std::size_t parameter_count) {
  switch (specifier.resource.kind) {
  case ResourceSpecifierKind::Any:
    break;
  case ResourceSpecifierKind::DeclaredAtAddress:
    requireIndex(specifier.resource.primary, module.addresses.size(), "access resource address index");
    break;
  case ResourceSpecifierKind::DeclaredInModule:
    requireIndex(specifier.resource.primary, module.module_handles.size(), "access resource module index");
    break;
  case ResourceSpecifierKind::Resource:
    requireIndex(specifier.resource.primary, module.struct_handles.size(), "access resource struct index");
    break;
  case ResourceSpecifierKind::ResourceInstantiation:
    requireIndex(specifier.resource.primary, module.struct_handles.size(), "access resource struct index");
    requireIndex(specifier.resource.signature, module.signatures.size(), "access resource signature index");
    break;
  }

  switch (specifier.address.kind) {
  case AddressSpecifierKind::Any:
    break;
  case AddressSpecifierKind::Parameter:
    requireIndex(specifier.address.value, parameter_count, "access address parameter local");
    break;
  case AddressSpecifierKind::Literal:
    requireIndex(specifier.address.value, module.addresses.size(), "access literal address index");
    break;
  }
  if (specifier.address.function_instantiation.has_value()) {
    requireIndex(*specifier.address.function_instantiation, module.function_instantiations.size(), "address derivation function instantiation index");
  }
}

void validateFieldDefinition(const Module &module, const FieldDefinition &field, std::size_t type_parameters) {
  requireIndex(field.name, module.identifiers.size(), "field name index");
  validateType(module, field.type, type_parameters, false);
}

void validateInstruction(const Module &module, const FunctionDefinition &function, const CodeUnit &unit, const Instruction &instruction,
                         std::size_t instruction_index) {
  const auto operand = [&]() -> std::size_t {
    if (instruction.operands.empty()) {
      invalid("instruction descriptor expected an operand");
    }
    return static_cast<std::size_t>(instruction.operands[0]);
  };
  const auto local = [&]() {
    const auto &handle = module.function_handles[function.handle];
    const auto local_count = module.signatures[handle.parameters].size() + module.signatures[unit.locals].size();
    requireIndex(operand(), local_count, "local index");
  };
  const auto target = [&]() { requireIndex(operand(), unit.code.size(), "branch target"); };

  switch (instruction.opcode) {
  case Opcode::BrTrue:
  case Opcode::BrFalse:
  case Opcode::Branch:
    target();
    break;

  case Opcode::CopyLoc:
  case Opcode::MoveLoc:
  case Opcode::StLoc:
  case Opcode::MutBorrowLoc:
  case Opcode::ImmBorrowLoc:
    local();
    break;

  case Opcode::LdConst:
    requireIndex(operand(), module.constants.size(), "constant index");
    break;

  case Opcode::MutBorrowField:
  case Opcode::ImmBorrowField:
    requireIndex(operand(), module.field_handles.size(), "field handle index");
    break;

  case Opcode::MutBorrowFieldGeneric:
  case Opcode::ImmBorrowFieldGeneric:
    requireIndex(operand(), module.field_instantiations.size(), "field instantiation index");
    break;

  case Opcode::ImmBorrowVariantField:
  case Opcode::MutBorrowVariantField:
    requireIndex(operand(), module.variant_field_handles.size(), "variant field handle index");
    break;

  case Opcode::ImmBorrowVariantFieldGeneric:
  case Opcode::MutBorrowVariantFieldGeneric:
    requireIndex(operand(), module.variant_field_instantiations.size(), "variant field instantiation index");
    break;

  case Opcode::Call:
  case Opcode::PackClosure:
    requireIndex(operand(), module.function_handles.size(), "function handle index");
    break;

  case Opcode::CallGeneric:
  case Opcode::PackClosureGeneric:
    requireIndex(operand(), module.function_instantiations.size(), "function instantiation index");
    break;

  case Opcode::Pack:
  case Opcode::Unpack:
  case Opcode::Exists:
  case Opcode::MutBorrowGlobal:
  case Opcode::ImmBorrowGlobal:
  case Opcode::MoveFrom:
  case Opcode::MoveTo:
    requireIndex(operand(), module.struct_definitions.size(), "struct definition index");
    break;

  case Opcode::PackGeneric:
  case Opcode::UnpackGeneric:
  case Opcode::ExistsGeneric:
  case Opcode::MutBorrowGlobalGeneric:
  case Opcode::ImmBorrowGlobalGeneric:
  case Opcode::MoveFromGeneric:
  case Opcode::MoveToGeneric:
    requireIndex(operand(), module.struct_definition_instantiations.size(), "struct definition instantiation index");
    break;

  case Opcode::PackVariant:
  case Opcode::UnpackVariant:
  case Opcode::TestVariant:
    requireIndex(operand(), module.struct_variant_handles.size(), "struct variant handle index");
    break;

  case Opcode::PackVariantGeneric:
  case Opcode::UnpackVariantGeneric:
  case Opcode::TestVariantGeneric:
    requireIndex(operand(), module.struct_variant_instantiations.size(), "struct variant instantiation index");
    break;

  case Opcode::VecPack:
  case Opcode::VecLen:
  case Opcode::VecImmBorrow:
  case Opcode::VecMutBorrow:
  case Opcode::VecPushBack:
  case Opcode::VecPopBack:
  case Opcode::VecUnpack:
  case Opcode::VecSwap:
  case Opcode::CallClosure:
    requireIndex(operand(), module.signatures.size(), "signature index");
    break;

  default:
    break;
  }

  const auto require_function_form = [&](TableIndex handle_index, bool generic) {
    const auto actually_generic = !module.function_handles[handle_index].type_parameters.empty();
    if (actually_generic != generic) {
      invalid("function opcode generic form disagrees with its handle");
    }
  };
  const auto require_struct_form = [&](TableIndex definition_index, bool generic) {
    const auto &definition = module.struct_definitions[definition_index];
    const auto actually_generic = !module.struct_handles[definition.handle].type_parameters.empty();
    if (actually_generic != generic) {
      invalid("struct opcode generic form disagrees with its definition");
    }
  };
  switch (instruction.opcode) {
  case Opcode::Call:
  case Opcode::PackClosure:
    require_function_form(static_cast<TableIndex>(operand()), false);
    break;
  case Opcode::CallGeneric:
  case Opcode::PackClosureGeneric: {
    const auto &instantiation = module.function_instantiations[operand()];
    require_function_form(instantiation.handle, true);
    break;
  }
  case Opcode::Pack:
  case Opcode::Unpack:
  case Opcode::Exists:
  case Opcode::MutBorrowGlobal:
  case Opcode::ImmBorrowGlobal:
  case Opcode::MoveFrom:
  case Opcode::MoveTo:
    require_struct_form(static_cast<TableIndex>(operand()), false);
    break;
  case Opcode::PackGeneric:
  case Opcode::UnpackGeneric:
  case Opcode::ExistsGeneric:
  case Opcode::MutBorrowGlobalGeneric:
  case Opcode::ImmBorrowGlobalGeneric:
  case Opcode::MoveFromGeneric:
  case Opcode::MoveToGeneric: {
    const auto &instantiation = module.struct_definition_instantiations[operand()];
    require_struct_form(instantiation.definition, true);
    break;
  }
  case Opcode::MutBorrowField:
  case Opcode::ImmBorrowField:
    require_struct_form(module.field_handles[operand()].owner, false);
    break;
  case Opcode::MutBorrowFieldGeneric:
  case Opcode::ImmBorrowFieldGeneric: {
    const auto &instantiation = module.field_instantiations[operand()];
    require_struct_form(module.field_handles[instantiation.handle].owner, true);
    break;
  }
  case Opcode::ImmBorrowVariantField:
  case Opcode::MutBorrowVariantField:
    require_struct_form(module.variant_field_handles[operand()].owner, false);
    break;
  case Opcode::ImmBorrowVariantFieldGeneric:
  case Opcode::MutBorrowVariantFieldGeneric: {
    const auto &instantiation = module.variant_field_instantiations[operand()];
    require_struct_form(module.variant_field_handles[instantiation.handle].owner, true);
    break;
  }
  case Opcode::PackVariant:
  case Opcode::UnpackVariant:
  case Opcode::TestVariant:
    require_struct_form(module.struct_variant_handles[operand()].definition, false);
    break;
  case Opcode::PackVariantGeneric:
  case Opcode::UnpackVariantGeneric:
  case Opcode::TestVariantGeneric: {
    const auto &instantiation = module.struct_variant_instantiations[operand()];
    require_struct_form(module.struct_variant_handles[instantiation.handle].definition, true);
    break;
  }
  case Opcode::VecPack:
  case Opcode::VecLen:
  case Opcode::VecImmBorrow:
  case Opcode::VecMutBorrow:
  case Opcode::VecPushBack:
  case Opcode::VecPopBack:
  case Opcode::VecUnpack:
  case Opcode::VecSwap: {
    const auto &signature = module.signatures[operand()];
    if (signature.size() != 1) {
      invalid("vector opcode signature does not contain exactly one type");
    }
    const auto &handle = module.function_handles[function.handle];
    validateSignature(module, signature, handle.type_parameters.size(), false);
    if ((instruction.opcode == Opcode::VecPack || instruction.opcode == Opcode::VecUnpack) &&
        (instruction.operands.size() < 2 || instruction.operands[1] > static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max()))) {
      invalid("vector pack/unpack element count exceeds u16");
    }
    break;
  }
  case Opcode::CallClosure: {
    const auto &signature = module.signatures[operand()];
    if (signature.size() != 1 || signature.front().kind != TypeKind::Function) {
      invalid("call-closure signature is not one function type");
    }
    break;
  }
  default:
    break;
  }

  if (instruction.opcode == Opcode::PackClosure || instruction.opcode == Opcode::PackClosureGeneric) {
    if (instruction.operands.size() < 2) {
      invalid("pack-closure instruction has no capture mask");
    }
    const auto handle_index = instruction.opcode == Opcode::PackClosure ? static_cast<TableIndex>(operand()) : module.function_instantiations[operand()].handle;
    const auto parameter_count = module.signatures[module.function_handles[handle_index].parameters].size();
    const auto mask = instruction.operands[1];
    if (mask != 0U && static_cast<std::size_t>(std::bit_width(mask)) > parameter_count) {
      invalid("pack-closure capture mask selects a nonexistent parameter");
    }
  }

  if (opcodeInfo(instruction.opcode).conditional_branch && instruction_index + 1 >= unit.code.size()) {
    invalid("conditional branch has no fallthrough instruction");
  }

  (void)function;
}

} // namespace

void validateModule(const Module &module) {
  requireIndex(module.self_module_handle, module.module_handles.size(), "self module handle index");
  validateCanonicalPools(module);

  for (const auto &handle : module.module_handles) {
    validateModuleHandle(module, handle);
  }
  for (const auto &friend_handle : module.friends) {
    validateModuleHandle(module, friend_handle);
  }
  std::set<std::pair<TableIndex, TableIndex>> unique_friends;
  const auto &self_handle = module.module_handles[module.self_module_handle];
  const auto &self_address = module.addresses[self_handle.address];
  for (const auto &friend_handle : module.friends) {
    if (!unique_friends.emplace(friend_handle.address, friend_handle.name).second) {
      invalid("duplicate friend declaration");
    }
    if (module.addresses[friend_handle.address] != self_address) {
      invalid("friend declaration uses a different account address");
    }
    if (friend_handle.name == self_handle.name) {
      invalid("module cannot declare itself as a friend");
    }
  }

  for (const auto &handle : module.struct_handles) {
    requireIndex(handle.module, module.module_handles.size(), "struct module handle index");
    requireIndex(handle.name, module.identifiers.size(), "struct name index");
    validateAbilitySet(handle.abilities);
    for (const auto &parameter : handle.type_parameters) {
      validateAbilitySet(parameter.constraints);
    }
  }

  for (const auto &signature : module.signatures) {
    validateSignature(module, signature);
  }
  for (const auto &constant : module.constants) {
    validateConstant(module, constant);
  }

  for (const auto &handle : module.function_handles) {
    requireIndex(handle.module, module.module_handles.size(), "function module handle index");
    requireIndex(handle.name, module.identifiers.size(), "function name index");
    requireIndex(handle.parameters, module.signatures.size(), "function parameter signature index");
    requireIndex(handle.returns, module.signatures.size(), "function return signature index");
    validateSignature(module, module.signatures[handle.parameters], handle.type_parameters.size());
    validateSignature(module, module.signatures[handle.returns], handle.type_parameters.size());
    for (const auto abilities : handle.type_parameters) {
      validateAbilitySet(abilities);
    }
    for (const auto &type : module.signatures[handle.parameters]) {
      validateNestedTypeConstraints(module, type, handle.type_parameters);
    }
    for (const auto &type : module.signatures[handle.returns]) {
      validateNestedTypeConstraints(module, type, handle.type_parameters);
    }
    if (handle.access_specifiers.has_value()) {
      for (const auto &specifier : *handle.access_specifiers) {
        validateAccessSpecifier(module, specifier, module.signatures[handle.parameters].size());
      }
    }
  }

  for (const auto &instantiation : module.function_instantiations) {
    requireIndex(instantiation.handle, module.function_handles.size(), "function instantiation handle index");
    requireIndex(instantiation.type_parameters, module.signatures.size(), "function instantiation signature index");
    const auto &handle = module.function_handles[instantiation.handle];
    validateInstantiationArguments(module, module.signatures[instantiation.type_parameters], handle.type_parameters);
  }

  std::set<TableIndex> defined_struct_handles;
  for (const auto &definition : module.struct_definitions) {
    requireIndex(definition.handle, module.struct_handles.size(), "struct definition handle index");
    if (!defined_struct_handles.insert(definition.handle).second) {
      invalid("duplicate struct definition for one handle");
    }
    const auto &handle = module.struct_handles[definition.handle];
    if (handle.module != module.self_module_handle) {
      invalid("struct definition references a non-self struct handle");
    }
    if (definition.field_kind == StructFieldKind::Declared && definition.fields.empty()) {
      invalid("declared struct definition has no fields");
    }
    std::set<TableIndex> field_names;
    for (const auto &field : definition.fields) {
      validateFieldDefinition(module, field, handle.type_parameters.size());
      std::vector<AbilitySet> field_context;
      field_context.reserve(handle.type_parameters.size());
      for (const auto &parameter : handle.type_parameters) {
        field_context.push_back(parameter.constraints);
      }
      validateNestedTypeConstraints(module, field.type, field_context);
      if (!field_names.insert(field.name).second) {
        invalid("struct contains duplicate field names");
      }
      const std::vector<AbilitySet> all_abilities(handle.type_parameters.size(), AbilitySet{AbilitySet::All});
      if (!hasAbilities(typeAbilities(module, field.type, all_abilities), requiredByDeclaredAbilities(handle.abilities))) {
        typeMismatch("struct field is missing an ability required by its owner");
      }
      validatePhantomPositions(module, field.type, handle.type_parameters);
    }
    std::set<TableIndex> variant_names;
    for (const auto &variant : definition.variants) {
      requireIndex(variant.name, module.identifiers.size(), "variant name index");
      if (!variant_names.insert(variant.name).second) {
        invalid("enum contains duplicate variant names");
      }
      std::set<TableIndex> variant_field_names;
      for (const auto &field : variant.fields) {
        validateFieldDefinition(module, field, handle.type_parameters.size());
        std::vector<AbilitySet> field_context;
        field_context.reserve(handle.type_parameters.size());
        for (const auto &parameter : handle.type_parameters) {
          field_context.push_back(parameter.constraints);
        }
        validateNestedTypeConstraints(module, field.type, field_context);
        if (!variant_field_names.insert(field.name).second) {
          invalid("enum variant contains duplicate field names");
        }
        const std::vector<AbilitySet> all_abilities(handle.type_parameters.size(), AbilitySet{AbilitySet::All});
        if (!hasAbilities(typeAbilities(module, field.type, all_abilities), requiredByDeclaredAbilities(handle.abilities))) {
          typeMismatch("enum field is missing an ability required by its owner");
        }
        validatePhantomPositions(module, field.type, handle.type_parameters);
      }
    }
    if (definition.field_kind == StructFieldKind::Variants && definition.variants.empty()) {
      invalid("enum definition has no variants");
    }
  }

  for (const auto &instantiation : module.struct_definition_instantiations) {
    requireIndex(instantiation.definition, module.struct_definitions.size(), "struct definition instantiation definition index");
    requireIndex(instantiation.type_parameters, module.signatures.size(), "struct definition instantiation signature index");
    const auto &definition = module.struct_definitions[instantiation.definition];
    const auto &handle = module.struct_handles[definition.handle];
    std::vector<AbilitySet> constraints;
    constraints.reserve(handle.type_parameters.size());
    for (const auto &parameter : handle.type_parameters) {
      constraints.push_back(parameter.constraints);
    }
    validateInstantiationArguments(module, module.signatures[instantiation.type_parameters], constraints);
  }

  for (const auto &handle : module.field_handles) {
    requireIndex(handle.owner, module.struct_definitions.size(), "field handle owner index");
    const auto &definition = module.struct_definitions[handle.owner];
    requireIndex(handle.field, definition.fields.size(), "field offset");
  }
  for (const auto &instantiation : module.field_instantiations) {
    requireIndex(instantiation.handle, module.field_handles.size(), "field instantiation handle index");
    requireIndex(instantiation.type_parameters, module.signatures.size(), "field instantiation signature index");
    const auto &field = module.field_handles[instantiation.handle];
    const auto &definition = module.struct_definitions[field.owner];
    const auto &handle = module.struct_handles[definition.handle];
    std::vector<AbilitySet> constraints;
    constraints.reserve(handle.type_parameters.size());
    for (const auto &parameter : handle.type_parameters) {
      constraints.push_back(parameter.constraints);
    }
    validateInstantiationArguments(module, module.signatures[instantiation.type_parameters], constraints);
  }

  for (const auto &handle : module.variant_field_handles) {
    requireIndex(handle.owner, module.struct_definitions.size(), "variant field owner index");
    const auto &definition = module.struct_definitions[handle.owner];
    if (definition.field_kind != StructFieldKind::Variants) {
      invalid("variant field handle owner is not an enum");
    }
    if (handle.variants.empty()) {
      invalid("variant field handle has no variants");
    }
    std::set<std::uint16_t> unique_variants;
    std::optional<Type> common_field_type;
    for (const auto variant : handle.variants) {
      if (!unique_variants.insert(variant).second) {
        invalid("variant field handle contains a duplicate variant");
      }
      requireIndex(variant, definition.variants.size(), "variant offset");
      requireIndex(handle.field, definition.variants[variant].fields.size(), "variant field offset");
      const auto &field_type = definition.variants[variant].fields[handle.field].type;
      if (!common_field_type.has_value()) {
        common_field_type = field_type;
      } else if (*common_field_type != field_type) {
        invalid("variant field handle selects fields with different types");
      }
    }
  }
  for (const auto &instantiation : module.variant_field_instantiations) {
    requireIndex(instantiation.handle, module.variant_field_handles.size(), "variant field instantiation handle index");
    requireIndex(instantiation.type_parameters, module.signatures.size(), "variant field instantiation signature index");
    const auto &field = module.variant_field_handles[instantiation.handle];
    const auto &definition = module.struct_definitions[field.owner];
    const auto &handle = module.struct_handles[definition.handle];
    std::vector<AbilitySet> constraints;
    constraints.reserve(handle.type_parameters.size());
    for (const auto &parameter : handle.type_parameters) {
      constraints.push_back(parameter.constraints);
    }
    validateInstantiationArguments(module, module.signatures[instantiation.type_parameters], constraints);
  }

  for (const auto &handle : module.struct_variant_handles) {
    requireIndex(handle.definition, module.struct_definitions.size(), "struct variant definition index");
    const auto &definition = module.struct_definitions[handle.definition];
    if (definition.field_kind != StructFieldKind::Variants) {
      invalid("struct variant handle definition is not an enum");
    }
    requireIndex(handle.variant, definition.variants.size(), "variant offset");
  }
  for (const auto &instantiation : module.struct_variant_instantiations) {
    requireIndex(instantiation.handle, module.struct_variant_handles.size(), "struct variant instantiation handle index");
    requireIndex(instantiation.type_parameters, module.signatures.size(), "struct variant instantiation signature index");
    const auto &variant = module.struct_variant_handles[instantiation.handle];
    const auto &definition = module.struct_definitions[variant.definition];
    const auto &handle = module.struct_handles[definition.handle];
    std::vector<AbilitySet> constraints;
    constraints.reserve(handle.type_parameters.size());
    for (const auto &parameter : handle.type_parameters) {
      constraints.push_back(parameter.constraints);
    }
    validateInstantiationArguments(module, module.signatures[instantiation.type_parameters], constraints);
  }

  for (std::size_t index = 0; index < module.struct_handles.size(); ++index) {
    if (module.struct_handles[index].module == module.self_module_handle && !defined_struct_handles.contains(static_cast<TableIndex>(index))) {
      invalid("self struct handle has no definition");
    }
  }

  std::set<TableIndex> defined_function_handles;
  for (const auto &function : module.function_definitions) {
    requireIndex(function.handle, module.function_handles.size(), "function definition handle index");
    if (!defined_function_handles.insert(function.handle).second) {
      invalid("duplicate function definition for one handle");
    }
    const auto &handle = module.function_handles[function.handle];
    if (handle.module != module.self_module_handle) {
      invalid("function definition references a non-self function handle");
    }
    std::set<TableIndex> unique_acquires;
    for (const auto acquired : function.acquires) {
      requireIndex(acquired, module.struct_definitions.size(), "acquired struct definition index");
      if (!unique_acquires.insert(acquired).second) {
        invalid("function contains a duplicate acquires annotation");
      }
      const auto &resource = module.struct_definitions[acquired];
      const auto &resource_handle = module.struct_handles[resource.handle];
      if (!resource_handle.abilities.has(AbilitySet::Key)) {
        invalid("function acquires a struct without the key ability");
      }
    }
    if (!function.code.has_value()) {
      continue;
    }
    const auto &unit = *function.code;
    if (unit.code.empty()) {
      invalid("non-native function has an empty code unit");
    }
    requireIndex(unit.locals, module.signatures.size(), "locals signature index");
    const auto &locals = module.signatures[unit.locals];
    const auto parameter_count = module.signatures[handle.parameters].size();
    if (parameter_count > 255 || locals.size() > 255 - parameter_count) {
      invalid("function has more than 255 parameters and locals");
    }
    validateSignature(module, locals, handle.type_parameters.size());
    for (const auto &type : locals) {
      validateNestedTypeConstraints(module, type, handle.type_parameters);
    }
    for (std::size_t index = 0; index < unit.code.size(); ++index) {
      validateInstruction(module, function, unit, unit.code[index], index);
    }
    const auto graph = buildControlFlowGraph(unit);
    if (!graph.falloff_exits.empty()) {
      invalid("function has a path which falls off the end of its code unit");
    }
    const auto stackless = liftToStackless(module, function, graph);
    (void)analyzeLocals(module, function, graph);
    validateBorrowSafety(module, function, graph, stackless);
  }

  for (std::size_t index = 0; index < module.function_handles.size(); ++index) {
    if (module.function_handles[index].module == module.self_module_handle && !defined_function_handles.contains(static_cast<TableIndex>(index))) {
      invalid("self function handle has no definition");
    }
  }

  validateNonRecursiveStructDefinitions(module);
  validateInstantiationLoops(module);

  std::map<TableIndex, const FunctionDefinition *> definitions_by_handle;
  for (const auto &function : module.function_definitions) {
    definitions_by_handle.emplace(function.handle, &function);
  }
  for (const auto &function : module.function_definitions) {
    if (!function.code.has_value()) {
      continue;
    }
    const std::set<TableIndex> declared(function.acquires.begin(), function.acquires.end());
    const auto require_acquired = [&](TableIndex resource) {
      if (!declared.contains(resource)) {
        invalid("function uses a global resource missing from acquires");
      }
    };
    for (const auto &instruction : function.code->code) {
      switch (instruction.opcode) {
      case Opcode::MoveFrom:
      case Opcode::MutBorrowGlobal:
      case Opcode::ImmBorrowGlobal:
        require_acquired(static_cast<TableIndex>(instruction.operands.at(0)));
        break;
      case Opcode::MoveFromGeneric:
      case Opcode::MutBorrowGlobalGeneric:
      case Opcode::ImmBorrowGlobalGeneric: {
        const auto &instantiation = module.struct_definition_instantiations.at(static_cast<std::size_t>(instruction.operands.at(0)));
        require_acquired(instantiation.definition);
        break;
      }
      case Opcode::Call:
      case Opcode::CallGeneric: {
        const auto called_handle = instruction.opcode == Opcode::Call
                                       ? static_cast<TableIndex>(instruction.operands.at(0))
                                       : module.function_instantiations.at(static_cast<std::size_t>(instruction.operands.at(0))).handle;
        const auto callee = definitions_by_handle.find(called_handle);
        if (callee != definitions_by_handle.end()) {
          for (const auto resource : callee->second->acquires) {
            require_acquired(resource);
          }
        }
        break;
      }
      default:
        break;
      }
    }
  }
}

Module scriptValidationModule(const Script &script) {
  Module result = script.common;
  const auto index_limit = static_cast<std::size_t>(std::numeric_limits<TableIndex>::max());
  if (result.identifiers.size() > index_limit - 1U || result.addresses.size() > index_limit || result.module_handles.size() > index_limit ||
      result.function_handles.size() > index_limit || result.signatures.size() > index_limit) {
    throw Error(ErrorCode::ResourceLimit, Error::UnknownOffset, "script common pools have no room for validation symbols");
  }

  const auto unique_identifier = [&](std::string base) {
    for (std::size_t suffix = 0;; ++suffix) {
      auto candidate = suffix == 0 ? base : base + "_" + std::to_string(suffix);
      if (std::find(result.identifiers.begin(), result.identifiers.end(), candidate) == result.identifiers.end()) {
        return candidate;
      }
    }
  };
  const auto module_name = static_cast<TableIndex>(result.identifiers.size());
  result.identifiers.push_back(unique_identifier("movescape_script"));
  const auto existing_main = std::find(result.identifiers.begin(), result.identifiers.end(), "main");
  TableIndex main_name = 0;
  if (existing_main == result.identifiers.end()) {
    main_name = static_cast<TableIndex>(result.identifiers.size());
    result.identifiers.emplace_back("main");
  } else {
    main_name = static_cast<TableIndex>(std::distance(result.identifiers.begin(), existing_main));
  }
  const auto self_address = static_cast<TableIndex>(result.addresses.size());
  Address validation_address{};
  while (std::find(result.addresses.begin(), result.addresses.end(), validation_address) != result.addresses.end()) {
    for (auto iterator = validation_address.rbegin(); iterator != validation_address.rend(); ++iterator) {
      ++*iterator;
      if (*iterator != 0U) {
        break;
      }
    }
  }
  result.addresses.push_back(validation_address);
  result.self_module_handle = static_cast<TableIndex>(result.module_handles.size());
  result.module_handles.push_back({.address = self_address, .name = module_name});

  auto empty_signature = std::find(result.signatures.begin(), result.signatures.end(), Signature{});
  TableIndex empty_signature_index = 0;
  if (empty_signature == result.signatures.end()) {
    empty_signature_index = static_cast<TableIndex>(result.signatures.size());
    result.signatures.push_back({});
  } else {
    empty_signature_index = static_cast<TableIndex>(std::distance(result.signatures.begin(), empty_signature));
  }

  const auto main_handle = static_cast<TableIndex>(result.function_handles.size());
  result.function_handles.push_back({
      .module = result.self_module_handle,
      .name = main_name,
      .parameters = script.parameters,
      .returns = empty_signature_index,
      .type_parameters = script.type_parameters,
      .access_specifiers = script.access_specifiers,
      .attributes = {},
  });
  result.function_definitions.push_back({
      .handle = main_handle,
      .visibility = Visibility::Public,
      .is_entry = true,
      .acquires = {},
      .code = script.code,
      .source_local_names = script.source_local_names,
      .source_type_parameter_names = script.source_type_parameter_names,
      .source_definition_location = script.source_definition_location,
      .source_code_locations = script.source_code_locations,
  });
  return result;
}

void validateScript(const Script &script) {
  const auto &common = script.common;
  if (!common.struct_definitions.empty() || !common.struct_definition_instantiations.empty() || !common.function_definitions.empty() ||
      !common.field_handles.empty() || !common.field_instantiations.empty() || !common.friends.empty() || !common.variant_field_handles.empty() ||
      !common.variant_field_instantiations.empty() || !common.struct_variant_handles.empty() || !common.struct_variant_instantiations.empty()) {
    invalid("script contains module-only definition state");
  }
  validateModule(scriptValidationModule(script));
}

} // namespace movescape
