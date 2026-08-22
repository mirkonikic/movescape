#include "movescape/move_emitter.hpp"

#include "movescape/cfg.hpp"
#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"
#include "movescape/expression_ir.hpp"
#include "movescape/graph_analysis.hpp"
#include "movescape/region.hpp"
#include "movescape/source_names.hpp"
#include "movescape/stackless_ir.hpp"
#include "movescape/validator.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace movescape {

namespace {

[[nodiscard]] bool typeRequiresLanguage22(const Type &type) {
  if (type.kind == TypeKind::Function && type.abilities.bits != 0) {
    return true;
  }
  return std::ranges::any_of(type.arguments, typeRequiresLanguage22) || std::ranges::any_of(type.results, typeRequiresLanguage22);
}

[[nodiscard]] bool typeRequiresLanguage23(const Type &type) {
  const bool signed_integer = type.kind == TypeKind::I8 || type.kind == TypeKind::I16 || type.kind == TypeKind::I32 || type.kind == TypeKind::I64 ||
                              type.kind == TypeKind::I128 || type.kind == TypeKind::I256;
  return signed_integer || std::ranges::any_of(type.arguments, typeRequiresLanguage23) || std::ranges::any_of(type.results, typeRequiresLanguage23);
}

template <typename Predicate> [[nodiscard]] bool moduleContainsType(const Module &module, Predicate predicate) {
  for (const auto &signature : module.signatures) {
    if (std::ranges::any_of(signature, predicate)) {
      return true;
    }
  }
  for (const auto &constant : module.constants) {
    if (predicate(constant.type)) {
      return true;
    }
  }
  for (const auto &definition : module.struct_definitions) {
    for (const auto &field : definition.fields) {
      if (predicate(field.type)) {
        return true;
      }
    }
    for (const auto &variant : definition.variants) {
      for (const auto &field : variant.fields) {
        if (predicate(field.type)) {
          return true;
        }
      }
    }
  }
  return false;
}

[[nodiscard]] bool containsAbortMessage(const CodeUnit &unit) {
  return std::ranges::any_of(unit.code, [](const auto &instruction) { return instruction.opcode == Opcode::AbortMsg; });
}

[[nodiscard]] bool containsSignedInstruction(const CodeUnit &unit) {
  return std::ranges::any_of(unit.code, [](const auto &instruction) {
    switch (instruction.opcode) {
    case Opcode::LdI8:
    case Opcode::LdI16:
    case Opcode::LdI32:
    case Opcode::LdI64:
    case Opcode::LdI128:
    case Opcode::LdI256:
    case Opcode::CastI8:
    case Opcode::CastI16:
    case Opcode::CastI32:
    case Opcode::CastI64:
    case Opcode::CastI128:
    case Opcode::CastI256:
      return true;
    default:
      return false;
    }
  });
}

class SourceNamespace {
public:
  [[nodiscard]] std::string allocate(std::string_view bytecode_name, std::string_view category, std::size_t index) {
    const auto existing = assigned_.find(std::string(bytecode_name));
    if (existing != assigned_.end()) {
      return existing->second;
    }
    const auto base = makeMoveSourceIdentifier(bytecode_name, category, index);
    auto candidate = base;
    if (used_.contains(candidate)) {
      candidate += '_' + std::string(category) + '_' + std::to_string(index);
      std::size_t collision = 0;
      while (used_.contains(candidate)) {
        candidate = base + '_' + std::string(category) + '_' + std::to_string(index) + '_' + std::to_string(++collision);
      }
    }
    used_.insert(candidate);
    assigned_.emplace(bytecode_name, candidate);
    return candidate;
  }

private:
  std::map<std::string, std::string> assigned_;
  std::set<std::string> used_;
};

struct SourceModule {
  Module module;
  std::vector<SourceNameChange> changes;
};

[[nodiscard]] TableIndex internIdentifier(Module &module, const std::string &identifier) {
  for (std::size_t index = 0; index < module.identifiers.size(); ++index) {
    if (module.identifiers[index] == identifier) {
      return static_cast<TableIndex>(index);
    }
  }
  if (module.identifiers.size() > static_cast<std::size_t>(std::numeric_limits<TableIndex>::max())) {
    throw Error(ErrorCode::ResourceLimit, Error::UnknownOffset, "source-name rewriting exhausted the identifier index space");
  }
  module.identifiers.push_back(identifier);
  return static_cast<TableIndex>(module.identifiers.size() - 1);
}

void renameIdentifier(SourceModule &source, TableIndex &identifier_index, SourceNamespace &name_space, std::string_view category, std::size_t stable_index,
                      std::string context) {
  const auto original = source.module.identifiers.at(identifier_index);
  const auto generated = name_space.allocate(original, category, stable_index);
  if (generated == original) {
    return;
  }
  identifier_index = internIdentifier(source.module, generated);
  source.changes.push_back({
      .context = std::move(context),
      .bytecode_name = original,
      .source_name = generated,
  });
}

[[nodiscard]] SourceModule sourceModule(const Module &module) {
  SourceModule result{.module = module, .changes = {}};

  std::map<Address, SourceNamespace> module_names;
  for (std::size_t index = 0; index < result.module.module_handles.size(); ++index) {
    auto &handle = result.module.module_handles[index];
    auto &name_space = module_names[result.module.addresses.at(handle.address)];
    renameIdentifier(result, handle.name, name_space, "module", index, "module-handle#" + std::to_string(index));
  }
  for (std::size_t index = 0; index < result.module.friends.size(); ++index) {
    auto &handle = result.module.friends[index];
    auto &name_space = module_names[result.module.addresses.at(handle.address)];
    renameIdentifier(result, handle.name, name_space, "module", index, "friend#" + std::to_string(index));
  }

  std::map<std::string, SourceNamespace> struct_names;
  for (std::size_t index = 0; index < result.module.struct_handles.size(); ++index) {
    auto &handle = result.module.struct_handles[index];
    auto &name_space = struct_names[renderModuleName(result.module, handle.module)];
    renameIdentifier(result, handle.name, name_space, "struct", index, "struct-handle#" + std::to_string(index));
  }

  std::map<std::string, SourceNamespace> function_names;
  for (std::size_t index = 0; index < result.module.function_handles.size(); ++index) {
    auto &handle = result.module.function_handles[index];
    auto &name_space = function_names[renderModuleName(result.module, handle.module)];
    renameIdentifier(result, handle.name, name_space, "function", index, "function-handle#" + std::to_string(index));
  }

  for (std::size_t definition_index = 0; definition_index < result.module.struct_definitions.size(); ++definition_index) {
    auto &definition = result.module.struct_definitions[definition_index];
    SourceNamespace fields;
    for (std::size_t field_index = 0; field_index < definition.fields.size(); ++field_index) {
      renameIdentifier(result, definition.fields[field_index].name, fields, "field", field_index,
                       "struct-definition#" + std::to_string(definition_index) + " field#" + std::to_string(field_index));
    }
    SourceNamespace variants;
    for (std::size_t variant_index = 0; variant_index < definition.variants.size(); ++variant_index) {
      auto &variant = definition.variants[variant_index];
      renameIdentifier(result, variant.name, variants, "variant", variant_index,
                       "struct-definition#" + std::to_string(definition_index) + " variant#" + std::to_string(variant_index));
      SourceNamespace variant_fields;
      for (std::size_t field_index = 0; field_index < variant.fields.size(); ++field_index) {
        renameIdentifier(result, variant.fields[field_index].name, variant_fields, "field", field_index,
                         "struct-definition#" + std::to_string(definition_index) + " variant#" + std::to_string(variant_index) + " field#" +
                             std::to_string(field_index));
      }
    }
  }
  return result;
}

[[nodiscard]] std::string abilities(AbilitySet set) {
  std::vector<std::string> values;
  if (set.has(AbilitySet::Copy)) {
    values.emplace_back("copy");
  }
  if (set.has(AbilitySet::Drop)) {
    values.emplace_back("drop");
  }
  if (set.has(AbilitySet::Store)) {
    values.emplace_back("store");
  }
  if (set.has(AbilitySet::Key)) {
    values.emplace_back("key");
  }
  std::ostringstream out;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      out << ", ";
    }
    out << values[index];
  }
  return out.str();
}

[[nodiscard]] std::string typeParameters(const std::vector<AbilitySet> &parameters, const std::vector<std::string> &names = {}) {
  if (parameters.empty()) {
    return {};
  }
  std::ostringstream out;
  out << '<';
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      out << ", ";
    }
    out << (index < names.size() ? names[index] : "T" + std::to_string(index));
    const auto constraints = abilities(parameters[index]);
    if (!constraints.empty()) {
      out << ": " << constraints;
    }
  }
  out << '>';
  return out.str();
}

[[nodiscard]] std::string structTypeParameters(const std::vector<StructTypeParameter> &parameters, const std::vector<std::string> &names = {}) {
  if (parameters.empty()) {
    return {};
  }
  std::ostringstream out;
  out << '<';
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      out << ", ";
    }
    if (parameters[index].is_phantom) {
      out << "phantom ";
    }
    out << (index < names.size() ? names[index] : "T" + std::to_string(index));
    const auto constraints = abilities(parameters[index].constraints);
    if (!constraints.empty()) {
      out << ": " << constraints;
    }
  }
  out << '>';
  return out.str();
}

[[nodiscard]] std::string visibility(Visibility value) {
  switch (value) {
  case Visibility::Private:
    return {};
  case Visibility::Public:
    return "public ";
  case Visibility::Friend:
    return "public(friend) ";
  }
  return {};
}

void emitFields(std::ostringstream &out, const Module &module, const std::vector<FieldDefinition> &fields, std::size_t depth) {
  for (const auto &field : fields) {
    out << std::string(depth * 2, ' ') << renderIdentifier(module, field.name) << ": " << renderType(module, field.type) << ",\n";
  }
}

void emitStruct(std::ostringstream &out, const Module &module, const StructDefinition &definition) {
  auto rendering = module;
  rendering.source_type_parameter_names = definition.source_type_parameter_names;
  const auto &handle = rendering.struct_handles.at(definition.handle);
  const auto name = renderIdentifier(rendering, handle.name);
  const auto parameters = structTypeParameters(handle.type_parameters, definition.source_type_parameter_names);
  const auto declared_abilities = abilities(handle.abilities);

  if (definition.field_kind == StructFieldKind::Native) {
    out << "  native struct " << name << parameters;
    if (!declared_abilities.empty()) {
      out << " has " << declared_abilities;
    }
    out << ";\n\n";
    return;
  }

  if (definition.field_kind == StructFieldKind::Variants) {
    out << "  enum " << name << parameters;
    if (!declared_abilities.empty()) {
      out << " has " << declared_abilities;
    }
    out << " {\n";
    for (const auto &variant : definition.variants) {
      out << "    " << renderIdentifier(rendering, variant.name);
      if (variant.fields.empty()) {
        out << ",\n";
      } else {
        out << " {\n";
        emitFields(out, rendering, variant.fields, 3);
        out << "    },\n";
      }
    }
    out << "  }\n\n";
    return;
  }

  out << "  struct " << name << parameters;
  if (!declared_abilities.empty()) {
    out << " has " << declared_abilities;
  }
  out << " {\n";
  emitFields(out, rendering, definition.fields, 2);
  out << "  }\n\n";
}

void emitFunctionSignature(std::ostringstream &out, const Module &module, const FunctionDefinition &definition, bool native) {
  const auto &handle = module.function_handles.at(definition.handle);
  for (const auto &attribute : handle.attributes) {
    switch (attribute.kind) {
    case FunctionAttributeKind::Persistent:
      out << "#[persistent]\n  ";
      break;
    case FunctionAttributeKind::ModuleLock:
      out << "#[module_lock]\n  ";
      break;
    default:
      break;
    }
  }
  if (native) {
    out << "native ";
  }
  out << visibility(definition.visibility);
  if (definition.is_entry) {
    out << "entry ";
  }
  out << "fun " << renderSourceFunctionIdentifier(module, handle.name) << typeParameters(handle.type_parameters, definition.source_type_parameter_names) << '(';
  const auto &parameters = module.signatures.at(handle.parameters);
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      out << ", ";
    }
    const auto name = index < definition.source_local_names.size() ? definition.source_local_names[index] : "local" + std::to_string(index);
    out << name << ": " << renderType(module, parameters[index]);
  }
  out << ')';

  const auto &returns = module.signatures.at(handle.returns);
  if (returns.size() == 1) {
    out << ": " << renderType(module, returns.front());
  } else if (!returns.empty()) {
    out << ": (";
    for (std::size_t index = 0; index < returns.size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      out << renderType(module, returns[index]);
    }
    out << ')';
  }
  if (!definition.acquires.empty()) {
    out << " acquires ";
    for (std::size_t index = 0; index < definition.acquires.size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      const auto &struct_definition = module.struct_definitions.at(definition.acquires[index]);
      out << renderStructName(module, struct_definition.handle);
    }
  }
}

[[nodiscard]] bool sourceSemanticsComplete(const Module &module, const RegionPtr &region) {
  if (!region) {
    return true;
  }
  if (region->kind == RegionKind::GotoFallback) {
    return false;
  }
  if (region->kind == RegionKind::Abort && region->values.size() == 2 && !isUnspecifiedAbortCode(region->values[0])) {
    return false;
  }
  if (region->condition && !expressionSourceSemanticsComplete(module, region->condition)) {
    return false;
  }
  for (const auto &statement : region->statements) {
    if (!expressionSourceSemanticsComplete(module, statement.expression)) {
      return false;
    }
  }
  for (const auto &value : region->values) {
    if (!expressionSourceSemanticsComplete(module, value)) {
      return false;
    }
  }
  for (const auto &child : region->children) {
    if (!sourceSemanticsComplete(module, child)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool dispatcherScalar(const Type &type) noexcept {
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
  case TypeKind::Signer:
  case TypeKind::Vector:
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

[[nodiscard]] bool dispatcherExpressionSafe(const Module &module, const ExpressionPtr &expression) {
  if (!expression || !expressionSourceSemanticsComplete(module, expression) || expression->opcode == Opcode::MoveLoc) {
    return false;
  }
  return std::ranges::all_of(expression->operands, [&](const auto &operand) { return dispatcherExpressionSafe(module, operand); });
}

[[nodiscard]] bool exactAbortTerminator(const ExpressionTerminator &terminator) noexcept {
  return terminator.values.size() == 1 || (terminator.values.size() == 2 && isUnspecifiedAbortCode(terminator.values[0]));
}

[[nodiscard]] bool canRenderTypedDispatcher(const Module &module, const ControlFlowGraph &graph, const ExpressionFunction &expressions) {
  if (expressions.locals.size() != expressions.parameter_count || !std::ranges::all_of(expressions.locals, dispatcherScalar)) {
    return false;
  }

  for (const auto &block : expressions.blocks) {
    if (block.id >= graph.blocks.size() || !graph.blocks[block.id].reachable) {
      continue;
    }
    for (const auto &statement : block.statements) {
      if (!dispatcherExpressionSafe(module, statement.expression) ||
          (statement.kind == ExpressionStatementKind::AssignLocal && (!statement.local.has_value() || *statement.local >= expressions.parameter_count))) {
        return false;
      }
    }
    for (const auto &value : block.terminator.values) {
      if (!dispatcherExpressionSafe(module, value)) {
        return false;
      }
    }

    const auto valid_target = [&](std::optional<BlockId> target) {
      return target.has_value() && *target < graph.blocks.size() && graph.blocks[*target].reachable;
    };
    switch (block.terminator.kind) {
    case ExpressionTerminatorKind::Goto:
      if (!valid_target(block.terminator.true_target)) {
        return false;
      }
      break;
    case ExpressionTerminatorKind::Conditional:
      if (block.terminator.values.size() != 1 || !valid_target(block.terminator.true_target) || !valid_target(block.terminator.false_target)) {
        return false;
      }
      break;
    case ExpressionTerminatorKind::Return:
      break;
    case ExpressionTerminatorKind::Abort:
      if (!exactAbortTerminator(block.terminator)) {
        return false;
      }
      break;
    case ExpressionTerminatorKind::None:
      return false;
    }
  }
  return true;
}

void emitDispatcherTransition(std::ostringstream &out, BlockId target, std::string_view state_name, std::size_t depth) {
  out << std::string(depth * 2, ' ') << state_name << " = " << target << "u64;\n";
}

void emitDispatcherTerminator(std::ostringstream &out, const Module &module, const ExpressionTerminator &terminator, std::string_view state_name,
                              std::size_t depth) {
  const auto indentation = std::string(depth * 2, ' ');
  switch (terminator.kind) {
  case ExpressionTerminatorKind::Goto:
    emitDispatcherTransition(out, *terminator.true_target, state_name, depth);
    break;
  case ExpressionTerminatorKind::Conditional:
    out << indentation << "if (" << renderExpression(module, terminator.values.at(0)) << ") {\n";
    emitDispatcherTransition(out, *terminator.true_target, state_name, depth + 1);
    out << indentation << "} else {\n";
    emitDispatcherTransition(out, *terminator.false_target, state_name, depth + 1);
    out << indentation << "}\n";
    break;
  case ExpressionTerminatorKind::Return:
    out << indentation << "return";
    for (std::size_t index = 0; index < terminator.values.size(); ++index) {
      out << (index == 0 ? " " : ", ") << renderExpression(module, terminator.values[index]);
    }
    out << (terminator.values.empty() ? ";\n" : "\n");
    break;
  case ExpressionTerminatorKind::Abort:
    out << indentation << "abort ";
    if (terminator.values.size() == 1) {
      out << renderExpression(module, terminator.values[0]);
    } else {
      out << renderExpression(module, terminator.values[1]);
    }
    out << '\n';
    break;
  case ExpressionTerminatorKind::None:
    throw Error(ErrorCode::UnsupportedFeature, Error::UnknownOffset, "typed dispatcher encountered a block without a terminator");
  }
}

[[nodiscard]] std::string renderTypedDispatcher(const Module &module, const ControlFlowGraph &graph, const ExpressionFunction &expressions,
                                                std::string_view state_name, std::size_t depth) {
  std::vector<const ExpressionBlock *> reachable;
  for (const auto &block : expressions.blocks) {
    if (block.id < graph.blocks.size() && graph.blocks[block.id].reachable) {
      reachable.push_back(&block);
    }
  }
  if (reachable.empty()) {
    throw Error(ErrorCode::UnsupportedFeature, Error::UnknownOffset, "typed dispatcher has no reachable blocks");
  }

  std::ostringstream out;
  const auto indentation = std::string(depth * 2, ' ');
  out << indentation << "let " << state_name << ": u64;\n";
  out << indentation << state_name << " = 0u64;\n";
  out << indentation << "loop {\n";
  for (std::size_t index = 0; index < reachable.size(); ++index) {
    const auto &block = *reachable[index];
    if (index == 0) {
      out << std::string((depth + 1) * 2, ' ');
      out << "if (copy " << state_name << " == " << block.id << "u64) {\n";
    } else if (index + 1 == reachable.size()) {
      out << "else {\n";
    } else {
      out << "else if (copy " << state_name << " == " << block.id << "u64) {\n";
    }
    for (const auto &statement : block.statements) {
      out << std::string((depth + 2) * 2, ' ') << renderExpressionStatement(module, statement) << '\n';
    }
    emitDispatcherTerminator(out, module, block.terminator, state_name, depth + 2);
    out << std::string((depth + 1) * 2, ' ') << '}';
    if (index + 1 != reachable.size()) {
      out << ' ';
    } else {
      out << '\n';
    }
  }
  out << indentation << "}\n";
  return out.str();
}

[[nodiscard]] std::string dispatcherName(const FunctionDefinition &definition, const ExpressionFunction &expressions) {
  std::set<std::string> local_names;
  for (std::size_t local = 0; local < expressions.locals.size(); ++local) {
    local_names.insert(local < definition.source_local_names.size() ? definition.source_local_names[local] : "local" + std::to_string(local));
  }
  std::string candidate = "movescape_dispatch_state";
  for (std::size_t collision = 0; local_names.contains(candidate); ++collision) {
    candidate = "movescape_dispatch_state_" + std::to_string(collision + 1);
  }
  return candidate;
}

[[nodiscard]] bool expressionUsesLocal(const ExpressionPtr &expression, LocalIndex local) {
  if (!expression) {
    return false;
  }
  if ((expression->opcode == Opcode::CopyLoc || expression->opcode == Opcode::MoveLoc || expression->opcode == Opcode::ImmBorrowLoc ||
       expression->opcode == Opcode::MutBorrowLoc) &&
      expression->immediate_operands.size() == 1 && expression->immediate_operands[0] == local) {
    return true;
  }
  return std::any_of(expression->operands.begin(), expression->operands.end(), [&](const auto &operand) { return expressionUsesLocal(operand, local); });
}

[[nodiscard]] bool structuredUsesLocal(const RegionPtr &region, LocalIndex local) {
  if (!region) {
    return false;
  }
  if (expressionUsesLocal(region->condition, local)) {
    return true;
  }
  for (const auto &statement : region->statements) {
    if ((statement.local.has_value() && *statement.local == local) || expressionUsesLocal(statement.expression, local)) {
      return true;
    }
  }
  for (const auto &expression : region->values) {
    if (expressionUsesLocal(expression, local)) {
      return true;
    }
  }
  return std::any_of(region->children.begin(), region->children.end(), [&](const auto &child) { return structuredUsesLocal(child, local); });
}

struct ModuleAlias {
  std::string qualified;
  std::string local;
};

void replaceAll(std::string &source, std::string_view from, std::string_view to) {
  for (std::size_t position = 0; (position = source.find(from, position)) != std::string::npos;) {
    source.replace(position, from.size(), to);
    position += to.size();
  }
}

[[nodiscard]] std::string applyModuleAliases(const Module &module, std::string source) {
  std::set<std::string> used;
  const auto &self = module.module_handles.at(module.self_module_handle);
  used.insert(renderIdentifier(module, self.name));
  for (const auto &definition : module.struct_definitions) {
    used.insert(renderIdentifier(module, module.struct_handles.at(definition.handle).name));
  }

  std::map<std::string, std::string> aliases_by_identity;
  for (std::size_t index = 0; index < module.module_handles.size(); ++index) {
    if (index == module.self_module_handle) {
      continue;
    }
    const auto qualified = renderModuleName(module, static_cast<TableIndex>(index));
    if (aliases_by_identity.contains(qualified)) {
      continue;
    }
    const auto &handle = module.module_handles[index];
    const auto base = renderIdentifier(module, handle.name);
    auto alias = base;
    for (std::size_t collision = 1; used.contains(alias); ++collision) {
      alias = base + "_module_" + std::to_string(collision);
    }
    used.insert(alias);
    aliases_by_identity.emplace(qualified, std::move(alias));
  }

  const auto self_prefix = renderModuleName(module, module.self_module_handle) + "::";
  replaceAll(source, self_prefix, "");
  for (const auto &[qualified, alias] : aliases_by_identity) {
    replaceAll(source, qualified + "::", alias + "::");
  }
  if (aliases_by_identity.empty()) {
    return source;
  }

  std::ostringstream uses;
  for (const auto &[qualified, alias] : aliases_by_identity) {
    const auto simple = qualified.substr(qualified.rfind("::") + 2U);
    uses << "  use " << qualified;
    if (alias != simple) {
      uses << " as " << alias;
    }
    uses << ";\n";
  }
  uses << '\n';
  const auto module_start = source.find("module ");
  const auto insertion = module_start == std::string::npos ? std::string::npos : source.find("{\n\n", module_start);
  if (insertion == std::string::npos) {
    throw Error(ErrorCode::Malformed, Error::UnknownOffset, "emitted module header is missing");
  }
  source.insert(insertion + 3U, uses.str());
  return source;
}

} // namespace

MoveSourcePolicy sourcePolicy(const Module &module) {
  MoveSourcePolicy result{
      .minimum_bytecode_version = module.version,
      .minimum_language_version = "2.0",
      .reasons = {"preserve input bytecode version " + std::to_string(module.version)},
  };
  if (moduleContainsType(module, typeRequiresLanguage22)) {
    result.minimum_language_version = "2.2";
    result.reasons.emplace_back("function-type ability constraints");
  }
  if (std::ranges::any_of(module.function_handles, [](const auto &function) {
        return std::ranges::any_of(function.attributes, [](const auto &attribute) { return attribute.kind == FunctionAttributeKind::Persistent; });
      })) {
    result.minimum_language_version = "2.2";
    result.reasons.emplace_back("persistent function attributes");
  }
  if (moduleContainsType(module, typeRequiresLanguage23) || std::ranges::any_of(module.function_definitions, [](const auto &function) {
        return function.code.has_value() && containsSignedInstruction(*function.code);
      })) {
    result.minimum_language_version = "2.3";
    result.reasons.emplace_back("signed integer syntax");
  }
  if (std::ranges::any_of(module.function_definitions,
                          [](const auto &function) { return function.code.has_value() && containsAbortMessage(*function.code); })) {
    result.minimum_language_version = "2.4";
    result.reasons.emplace_back("abort-message syntax");
  }
  return result;
}

MoveSourcePolicy sourcePolicy(const Script &script) {
  auto result = sourcePolicy(script.common);
  if (containsSignedInstruction(script.code)) {
    result.minimum_language_version = "2.3";
    result.reasons.emplace_back("signed integer syntax");
  }
  if (containsAbortMessage(script.code)) {
    result.minimum_language_version = "2.4";
    result.reasons.emplace_back("abort-message syntax");
  }
  return result;
}

bool MoveEmission::allControlFlowComplete() const noexcept {
  for (const auto &function : functions) {
    if (!function.native && !function.control_flow_complete) {
      return false;
    }
  }
  return true;
}

bool MoveEmission::allSourceSemanticsComplete() const noexcept {
  if (!renamed_identifiers.empty()) {
    return false;
  }
  for (const auto &function : functions) {
    if (!function.native && !function.source_semantics_complete) {
      return false;
    }
  }
  return true;
}

MoveEmission emitMoveModule(const Module &decoded_module) {
  auto normalized = sourceModule(decoded_module);
  const auto &module = normalized.module;
  const auto has_source_map_names =
      std::ranges::any_of(module.struct_definitions, [](const auto &definition) { return !definition.source_type_parameter_names.empty(); }) ||
      std::ranges::any_of(module.function_definitions,
                          [](const auto &definition) { return !definition.source_local_names.empty() || !definition.source_type_parameter_names.empty(); });
  MoveEmission result;
  result.policy = sourcePolicy(module);
  result.renamed_identifiers = std::move(normalized.changes);
  if (!result.renamed_identifiers.empty()) {
    throw Error(ErrorCode::UnsupportedFeature, Error::UnknownOffset, "exact Move source emission would rename " + std::to_string(result.renamed_identifiers.size()) + " bytecode identifier(s)");
  }
  std::ostringstream out;
  out << "// Decompiled by movescape from Aptos Move bytecode v" << module.version << ".\n";
  out << "// Compiler policy: bytecode >= v" << result.policy.minimum_bytecode_version << ", Move language >= " << result.policy.minimum_language_version << ".\n";
  if (has_source_map_names) { out << "// Source-map names were used where available; other names are generated.\n"; } 
  else { out << "// Generated names were not present in the original source.\n"; }
  for (const auto &change : result.renamed_identifiers) { out << "// Renamed " << change.context << " `" << change.bytecode_name << "` to `" << change.source_name << "` for Move source syntax.\n"; }
  if (!module.metadata.empty()) { out << "// " << module.metadata.size() << " metadata entries are preserved in the decoded module.\n"; }

  // emit friend modules
  out << "module " << renderModuleName(module, module.self_module_handle) << " {\n\n";
  for (const auto &friend_module : module.friends) {
    if (friend_module.address < module.addresses.size()) {
      out << "  friend " << renderAddress(module.addresses[friend_module.address]) << "::" << renderIdentifier(module, friend_module.name) << ";\n";
    }
  }
  if (!module.friends.empty()) {
    out << '\n';
  }

  // emit constants
  for (std::size_t index = 0; index < module.source_constant_names.size(); ++index) {
    if (!module.source_constant_names[index].has_value()) {
      continue;
    }
    out << "  const " << *module.source_constant_names[index] << ": " << renderType(module, module.constants.at(index).type) << " = "
        << renderConstantValue(module, index) << ";\n";
  }
  if (std::ranges::any_of(module.source_constant_names, [](const auto &name) { return name.has_value(); })) {
    out << '\n';
  }

  // emit struct definitions
  for (const auto &definition : module.struct_definitions) {
    emitStruct(out, module, definition);
  }

  // 
  for (std::size_t index = 0; index < module.function_definitions.size(); ++index) {
    auto rendering = module;
    rendering.source_type_parameter_names = module.function_definitions[index].source_type_parameter_names;
    const auto &definition = rendering.function_definitions[index];
    const bool native = !definition.code.has_value();
    out << "  ";
    emitFunctionSignature(out, rendering, definition, native);
    if (native) {
      out << ";\n\n";
      result.functions.push_back({
          .definition = index,
          .native = true,
          .control_flow_complete = true,
          .source_semantics_complete = true,
      });
      continue;
    }

    const auto &unit = *definition.code;
    const auto graph = buildControlFlowGraph(unit);
    const auto graph_analysis = analyzeControlFlowGraph(graph);
    const auto stackless = liftToStackless(rendering, definition, graph);
    const auto expressions = recoverExpressions(rendering, definition, graph, stackless);
    const auto structured = structureControlFlow(graph, graph_analysis, expressions);
    const auto use_typed_dispatcher = !graph_analysis.reducible() && canRenderTypedDispatcher(rendering, graph, expressions);
    const auto semantics_complete = use_typed_dispatcher || sourceSemanticsComplete(rendering, structured.root);
    const auto function_name = renderFunctionName(rendering, definition.handle);
    if (!structured.complete && !use_typed_dispatcher) {
      throw Error(ErrorCode::UnsupportedFeature, Error::UnknownOffset, "cannot emit " + function_name + ": control-flow recovery is incomplete");
    }
    if (!semantics_complete) {
      throw Error(ErrorCode::UnsupportedFeature, Error::UnknownOffset, "cannot emit " + function_name + ": recovered source would change bytecode semantics");
    }
    result.functions.push_back({
        .definition = index,
        .native = false,
        .control_flow_complete = structured.complete || use_typed_dispatcher,
        .source_semantics_complete = semantics_complete,
    });

    out << " {\n";
    if (use_typed_dispatcher) {
      out << renderTypedDispatcher(rendering, graph, expressions, dispatcherName(definition, expressions), 2);
      out << "  }\n\n";
      continue;
    }
    const auto &handle = rendering.function_handles.at(definition.handle);
    const auto parameter_count = rendering.signatures.at(handle.parameters).size();
    const auto &declared_locals = rendering.signatures.at(unit.locals);
    bool emitted_local = false;
    for (std::size_t local = 0; local < declared_locals.size(); ++local) {
      const auto local_index = parameter_count + local;
      if (!structuredUsesLocal(structured.root, static_cast<LocalIndex>(local_index))) {
        continue;
      }
      const auto name = local_index < definition.source_local_names.size() ? definition.source_local_names[local_index] : "local" + std::to_string(local_index);
      out << "    let " << name << ": " << renderType(rendering, declared_locals[local]) << ";\n";
      emitted_local = true;
    }
    if (emitted_local) {
      out << '\n';
    }
    out << renderStructuredBody(rendering, structured, 2);
    out << "  }\n\n";
  }
  out << "}\n";
  result.source = applyModuleAliases(module, out.str());
  return result;
}

MoveEmission emitMoveScript(const Script &script) {
  validateScript(script);
  auto result = emitMoveModule(scriptValidationModule(script));
  result.policy = sourcePolicy(script);
  constexpr std::string_view synthetic_header = "module 0x0::movescape_script {";
  const auto header = result.source.find(synthetic_header);
  if (header == std::string::npos) {
    throw Error(ErrorCode::Malformed, Error::UnknownOffset, "synthetic script module header is missing");
  }
  result.source.replace(header, synthetic_header.size(), "script {");
  replaceAll(result.source, "preserved in the decoded module", "preserved in the decoded script");
  constexpr std::string_view synthetic_main = "public entry fun main";
  const auto main = result.source.find(synthetic_main, header);
  if (main == std::string::npos) {
    throw Error(ErrorCode::Malformed, Error::UnknownOffset, "synthetic script main function is missing");
  }
  result.source.replace(main, synthetic_main.size(), "fun main");
  return result;
}

} // namespace movescape
