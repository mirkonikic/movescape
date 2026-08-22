#include "movescape/body_compare.hpp"

#include "movescape/cfg.hpp"
#include "movescape/opcode.hpp"
#include "movescape/semantic.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace movescape {

namespace {

template <typename Items, typename Render> [[nodiscard]] std::string join(const Items &items, std::string_view separator, Render render) {
  std::ostringstream out;
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index != 0) {
      out << separator;
    }
    out << render(items[index]);
  }
  return out.str();
}

[[nodiscard]] std::string abilities(AbilitySet set) {
  std::vector<std::string_view> names;
  if (set.has(AbilitySet::Copy)) {
    names.emplace_back("copy");
  }
  if (set.has(AbilitySet::Drop)) {
    names.emplace_back("drop");
  }
  if (set.has(AbilitySet::Store)) {
    names.emplace_back("store");
  }
  if (set.has(AbilitySet::Key)) {
    names.emplace_back("key");
  }
  return "[" + join(names, ",", [](std::string_view name) { return name; }) + "]";
}

[[nodiscard]] std::string typeName(const SemanticModel &model, const Type &type) {
  switch (type.kind) {
  case TypeKind::Bool:
    return "bool";
  case TypeKind::U8:
    return "u8";
  case TypeKind::U16:
    return "u16";
  case TypeKind::U32:
    return "u32";
  case TypeKind::U64:
    return "u64";
  case TypeKind::U128:
    return "u128";
  case TypeKind::U256:
    return "u256";
  case TypeKind::I8:
    return "i8";
  case TypeKind::I16:
    return "i16";
  case TypeKind::I32:
    return "i32";
  case TypeKind::I64:
    return "i64";
  case TypeKind::I128:
    return "i128";
  case TypeKind::I256:
    return "i256";
  case TypeKind::Address:
    return "address";
  case TypeKind::Signer:
    return "signer";
  case TypeKind::Vector:
    return "vector<" + typeName(model, type.arguments.at(0)) + ">";
  case TypeKind::Reference:
    return "&" + typeName(model, type.arguments.at(0));
  case TypeKind::MutableReference:
    return "&mut " + typeName(model, type.arguments.at(0));
  case TypeKind::Struct:
    return model.structures.at(type.index).qualified_name;
  case TypeKind::StructInstantiation:
    return model.structures.at(type.index).qualified_name + "<" + join(type.arguments, ",", [&](const Type &argument) { return typeName(model, argument); }) +
           ">";
  case TypeKind::TypeParameter:
    return "T" + std::to_string(type.index);
  case TypeKind::Function:
    return "fun" + abilities(type.abilities) + "(" + join(type.arguments, ",", [&](const Type &argument) { return typeName(model, argument); }) + ")->(" +
           join(type.results, ",", [&](const Type &result) { return typeName(model, result); }) + ")";
  }
  return "<type>";
}

[[nodiscard]] std::string signatureName(const SemanticModel &model, const Signature &signature) {
  return "(" + join(signature, ",", [&](const Type &type) { return typeName(model, type); }) + ")";
}

[[nodiscard]] std::string bytesHex(const std::vector<std::uint8_t> &bytes, bool little_endian = false) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setfill('0');
  if (little_endian) {
    for (auto iterator = bytes.rbegin(); iterator != bytes.rend(); ++iterator) {
      out << std::setw(2) << static_cast<unsigned>(*iterator);
    }
  } else {
    for (const auto byte : bytes) {
      out << std::setw(2) << static_cast<unsigned>(byte);
    }
  }
  return out.str();
}

[[nodiscard]] int edgeRank(EdgeKind kind) noexcept {
  switch (kind) {
  case EdgeKind::True:
    return 0;
  case EdgeKind::False:
    return 1;
  case EdgeKind::Branch:
    return 2;
  case EdgeKind::Fallthrough:
    return 3;
  }
  return 4;
}

[[nodiscard]] std::vector<BlockId> canonicalBlockOrder(const ControlFlowGraph &graph) {
  std::vector<BlockId> order;
  if (graph.blocks.empty()) {
    return order;
  }
  std::vector<bool> seen(graph.blocks.size(), false);
  std::deque<BlockId> pending{0};
  seen[0] = true;
  while (!pending.empty()) {
    const auto block = pending.front();
    pending.pop_front();
    order.push_back(block);
    auto successors = graph.blocks[block].successors;
    std::sort(successors.begin(), successors.end(), [](const auto &left, const auto &right) {
      if (edgeRank(left.kind) != edgeRank(right.kind)) {
        return edgeRank(left.kind) < edgeRank(right.kind);
      }
      return left.target < right.target;
    });
    for (const auto &edge : successors) {
      if (!seen[edge.target]) {
        seen[edge.target] = true;
        pending.push_back(edge.target);
      }
    }
  }
  for (BlockId block = 0; block < graph.blocks.size(); ++block) {
    if (!seen[block]) {
      order.push_back(block);
    }
  }
  return order;
}

[[nodiscard]] std::string exitName(BlockExitKind kind) {
  switch (kind) {
  case BlockExitKind::None:
    return {};
  case BlockExitKind::Return:
    return "return";
  case BlockExitKind::Abort:
    return "abort";
  case BlockExitKind::FallOff:
    return "falloff";
  }
  return "unknown";
}

class InstructionNormalizer {
public:
  InstructionNormalizer(const Module &module, const SemanticModel &model, const FunctionDefinition &definition, const std::vector<BlockId> &canonical_by_old)
      : module_(module), model_(model), definition_(definition), canonical_by_old_(canonical_by_old) {
    const auto &handle = module_.function_handles.at(definition_.handle);
    parameter_types_ = module_.signatures.at(handle.parameters);
    if (definition_.code.has_value()) {
      local_types_ = module_.signatures.at(definition_.code->locals);
    }
  }

  [[nodiscard]] std::string operator()(const Instruction &instruction, const ControlFlowGraph &graph) {
    const auto opcode = instruction.opcode;
    const auto name = std::string(opcodeInfo(opcode).name);
    const auto first = instruction.operands.empty() ? std::size_t{0} : static_cast<std::size_t>(instruction.operands[0]);
    const auto with = [&](const std::string &operand) { return name + " " + operand; };
    switch (opcode) {
    case Opcode::CopyLoc:
    case Opcode::MoveLoc:
    case Opcode::StLoc:
    case Opcode::MutBorrowLoc:
    case Opcode::ImmBorrowLoc:
      return with(localName(first));
    case Opcode::LdConst: {
      const auto &constant = module_.constants.at(first);
      return with(typeName(model_, constant.type) + ":" + bytesHex(constant.data));
    }
    case Opcode::Call:
      return with(model_.functions.at(first).qualified_name);
    case Opcode::PackClosure:
      return with(model_.functions.at(first).qualified_name + " mask=" + std::to_string(instruction.operands.at(1)));
    case Opcode::CallGeneric:
    case Opcode::PackClosureGeneric: {
      const auto &instantiation = model_.function_instantiations.at(first);
      auto operand = model_.functions.at(instantiation.function).qualified_name + signatureName(model_, instantiation.type_arguments);
      if (opcode == Opcode::PackClosureGeneric) {
        operand += " mask=" + std::to_string(instruction.operands.at(1));
      }
      return with(operand);
    }
    case Opcode::CallClosure:
      return with(signatureName(model_, module_.signatures.at(first)));
    case Opcode::Pack:
    case Opcode::Unpack:
    case Opcode::Exists:
    case Opcode::MutBorrowGlobal:
    case Opcode::ImmBorrowGlobal:
    case Opcode::MoveFrom:
    case Opcode::MoveTo: {
      const auto structure = module_.struct_definitions.at(first).handle;
      return with(model_.structures.at(structure).qualified_name);
    }
    case Opcode::PackGeneric:
    case Opcode::UnpackGeneric:
    case Opcode::ExistsGeneric:
    case Opcode::MutBorrowGlobalGeneric:
    case Opcode::ImmBorrowGlobalGeneric:
    case Opcode::MoveFromGeneric:
    case Opcode::MoveToGeneric: {
      const auto &instantiation = model_.struct_instantiations.at(first);
      return with(model_.structures.at(instantiation.structure).qualified_name + signatureName(model_, instantiation.type_arguments));
    }
    case Opcode::MutBorrowField:
    case Opcode::ImmBorrowField:
      return with(model_.fields.at(model_.field_handle_targets.at(first)).qualified_name);
    case Opcode::MutBorrowFieldGeneric:
    case Opcode::ImmBorrowFieldGeneric: {
      const auto &instantiation = model_.field_instantiations.at(first);
      return with(model_.fields.at(instantiation.field).qualified_name + signatureName(model_, instantiation.type_arguments));
    }
    case Opcode::ImmBorrowVariantField:
    case Opcode::MutBorrowVariantField:
      return with(variantFields(model_.variant_field_handle_targets.at(first)));
    case Opcode::ImmBorrowVariantFieldGeneric:
    case Opcode::MutBorrowVariantFieldGeneric: {
      const auto &instantiation = model_.variant_field_instantiations.at(first);
      return with(variantFields(instantiation.fields) + signatureName(model_, instantiation.type_arguments));
    }
    case Opcode::PackVariant:
    case Opcode::UnpackVariant:
    case Opcode::TestVariant:
      return with(model_.variants.at(model_.struct_variant_handle_targets.at(first)).qualified_name);
    case Opcode::PackVariantGeneric:
    case Opcode::UnpackVariantGeneric:
    case Opcode::TestVariantGeneric: {
      const auto &instantiation = model_.struct_variant_instantiations.at(first);
      return with(model_.variants.at(instantiation.variant).qualified_name + signatureName(model_, instantiation.type_arguments));
    }
    case Opcode::VecPack:
    case Opcode::VecUnpack:
      return with(signatureName(model_, module_.signatures.at(first)) + " count=" + std::to_string(instruction.operands.at(1)));
    case Opcode::VecLen:
    case Opcode::VecImmBorrow:
    case Opcode::VecMutBorrow:
    case Opcode::VecPushBack:
    case Opcode::VecPopBack:
    case Opcode::VecSwap:
      return with(signatureName(model_, module_.signatures.at(first)));
    case Opcode::LdU128:
    case Opcode::LdU256:
    case Opcode::LdI128:
    case Opcode::LdI256:
      return with(bytesHex(instruction.wide_operand, true));
    case Opcode::LdI8:
    case Opcode::LdI16:
    case Opcode::LdI32:
    case Opcode::LdI64:
      return with(std::to_string(std::bit_cast<std::int64_t>(instruction.operands.at(0))));
    case Opcode::BrTrue:
    case Opcode::BrFalse:
    case Opcode::Branch: {
      const auto target = graph.instruction_to_block.at(first);
      return with("bb" + std::to_string(canonical_by_old_.at(target)));
    }
    default:
      break;
    }
    if (!instruction.operands.empty()) {
      return with(join(instruction.operands, ",", [](std::uint64_t value) { return std::to_string(value); }));
    }
    return name;
  }

private:
  [[nodiscard]] std::string localName(std::size_t index) {
    if (index < parameter_types_.size()) {
      return "arg#" + std::to_string(index) + ":" + typeName(model_, parameter_types_[index]);
    }
    const auto raw_local = index - parameter_types_.size();
    auto [position, inserted] = local_names_.try_emplace(raw_local, 0);
    if (inserted) {
      position->second = local_names_.size() - 1;
    }
    return "local#" + std::to_string(position->second) + ":" + typeName(model_, local_types_.at(raw_local));
  }

  [[nodiscard]] std::string variantFields(const std::vector<SemanticIndex> &fields) const {
    return "[" + join(fields, ",", [&](SemanticIndex field) { return model_.fields.at(field).qualified_name; }) + "]";
  }

  const Module &module_;
  const SemanticModel &model_;
  const FunctionDefinition &definition_;
  const std::vector<BlockId> &canonical_by_old_;
  Signature parameter_types_;
  Signature local_types_;
  std::map<std::size_t, std::size_t> local_names_;
};

[[nodiscard]] std::string formatFunction(const NormalizedFunctionBody &function) {
  std::ostringstream out;
  out << "function " << function.identity;
  if (function.native) {
    out << " native\n";
    return out.str();
  }
  out << '\n';
  for (const auto &block : function.blocks) {
    out << "  " << block.label << ":\n";
    for (const auto &instruction : block.instructions) {
      out << "    " << instruction << '\n';
    }
    for (const auto &successor : block.successors) {
      out << "    -> " << successor << '\n';
    }
    if (!block.exit.empty()) {
      out << "    exit " << block.exit << '\n';
    }
  }
  return out.str();
}

[[nodiscard]] std::map<std::string, std::string> bodyMap(const NormalizedModuleBodies &module) {
  std::map<std::string, std::string> result;
  for (const auto &function : module.functions) {
    result.emplace(function.identity, formatFunction(function));
  }
  return result;
}

} // namespace

NormalizedModuleBodies normalizeModuleBodies(const Module &module) {
  const auto model = buildSemanticModel(module);
  NormalizedModuleBodies result{
      .module_name = model.modules.at(module.self_module_handle).qualified_name,
      .functions = {},
  };
  result.functions.reserve(module.function_definitions.size());
  for (std::size_t definition_index = 0; definition_index < module.function_definitions.size(); ++definition_index) {
    const auto &definition = module.function_definitions[definition_index];
    const auto handle = model.function_handle_by_definition[definition_index];
    NormalizedFunctionBody function{
        .identity = model.functions.at(handle).qualified_name,
        .native = !definition.code.has_value(),
        .blocks = {},
    };
    if (definition.code.has_value()) {
      const auto &unit = *definition.code;
      const auto graph = buildControlFlowGraph(unit);
      const auto order = canonicalBlockOrder(graph);
      std::vector<BlockId> canonical_by_old(graph.blocks.size());
      for (std::size_t canonical = 0; canonical < order.size(); ++canonical) {
        canonical_by_old[order[canonical]] = canonical;
      }
      InstructionNormalizer normalize(module, model, definition, canonical_by_old);
      function.blocks.reserve(order.size());
      for (std::size_t canonical = 0; canonical < order.size(); ++canonical) {
        const auto old = order[canonical];
        const auto &block = graph.blocks[old];
        NormalizedBasicBlock normalized{
            .label = "bb" + std::to_string(canonical),
            .instructions = {},
            .successors = {},
            .exit = exitName(block.exit_kind),
        };
        for (std::size_t pc = block.begin; pc < block.end; ++pc) {
          const auto &instruction = unit.code[pc];
          if (instruction.opcode == Opcode::Nop || instruction.opcode == Opcode::BrTrue || instruction.opcode == Opcode::BrFalse ||
              instruction.opcode == Opcode::Branch) {
            continue;
          }
          normalized.instructions.push_back(normalize(instruction, graph));
        }
        auto successors = block.successors;
        std::sort(successors.begin(), successors.end(), [](const auto &left, const auto &right) {
          if (edgeRank(left.kind) != edgeRank(right.kind)) {
            return edgeRank(left.kind) < edgeRank(right.kind);
          }
          return left.target < right.target;
        });
        for (const auto &edge : successors) {
          normalized.successors.push_back(std::string(edgeKindName(edge.kind)) + "->bb" + std::to_string(canonical_by_old.at(edge.target)));
        }
        function.blocks.push_back(std::move(normalized));
      }
    }
    result.functions.push_back(std::move(function));
  }
  std::sort(result.functions.begin(), result.functions.end(), [](const auto &left, const auto &right) { return left.identity < right.identity; });
  return result;
}

ModuleBodyComparison compareModuleBodies(const Module &reference, const Module &candidate) {
  const auto reference_bodies = bodyMap(normalizeModuleBodies(reference));
  const auto candidate_bodies = bodyMap(normalizeModuleBodies(candidate));
  ModuleBodyComparison result;
  for (const auto &[identity, body] : reference_bodies) {
    const auto found = candidate_bodies.find(identity);
    if (found == candidate_bodies.end()) {
      result.differences.push_back({
          .identity = identity,
          .reference = body,
          .candidate = std::nullopt,
      });
    } else if (found->second != body) {
      result.differences.push_back({
          .identity = identity,
          .reference = body,
          .candidate = found->second,
      });
    }
  }
  for (const auto &[identity, body] : candidate_bodies) {
    if (!reference_bodies.contains(identity)) {
      result.differences.push_back({
          .identity = identity,
          .reference = std::nullopt,
          .candidate = body,
      });
    }
  }
  std::sort(result.differences.begin(), result.differences.end(), [](const auto &left, const auto &right) { return left.identity < right.identity; });
  return result;
}

std::string formatNormalizedModuleBodies(const NormalizedModuleBodies &bodies) {
  std::ostringstream out;
  out << "module " << bodies.module_name << '\n';
  for (const auto &function : bodies.functions) {
    out << formatFunction(function);
  }
  return out.str();
}

std::string formatModuleBodyComparison(const ModuleBodyComparison &comparison) {
  if (comparison.equivalent()) {
    return "normalized function bodies and CFGs are equivalent\n";
  }
  std::ostringstream out;
  out << "normalized function bodies and CFGs differ (" << comparison.differences.size() << ")\n";
  for (const auto &difference : comparison.differences) {
    out << "  function " << difference.identity << '\n' << "    reference:\n";
    if (difference.reference.has_value()) {
      out << *difference.reference;
    } else {
      out << "      <missing>\n";
    }
    out << "    candidate:\n";
    if (difference.candidate.has_value()) {
      out << *difference.candidate;
    } else {
      out << "      <missing>\n";
    }
  }
  return out.str();
}

} // namespace movescape
