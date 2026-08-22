#include "movescape/local_analysis.hpp"

#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"
#include "movescape/type_analysis.hpp"

#include <algorithm>
#include <sstream>
#include <string>

namespace movescape {

namespace {

using Bits = std::vector<bool>;

[[noreturn]] void invalidLocal(std::size_t instruction, LocalIndex local, Opcode opcode) {
  std::ostringstream out;
  out << "instruction " << instruction << " (" << opcodeInfo(opcode).name << ") may read unavailable local#" << static_cast<unsigned>(local);
  throw Error(ErrorCode::InvalidLocalState, Error::UnknownOffset, out.str());
}

[[nodiscard]] bool localRead(Opcode opcode) {
  return opcode == Opcode::CopyLoc || opcode == Opcode::MoveLoc || opcode == Opcode::ImmBorrowLoc || opcode == Opcode::MutBorrowLoc;
}

[[nodiscard]] bool localWrite(Opcode opcode) { return opcode == Opcode::StLoc; }

[[nodiscard]] std::size_t localIndex(const Instruction &instruction) { return static_cast<std::size_t>(instruction.operands.at(0)); }

[[nodiscard]] Availability availability(bool must, bool may) {
  if (must) {
    return Availability::Available;
  }
  return may ? Availability::MaybeAvailable : Availability::Unavailable;
}

[[nodiscard]] const char *availabilityName(Availability value) {
  switch (value) {
  case Availability::Unavailable:
    return "unavailable";
  case Availability::MaybeAvailable:
    return "maybe";
  case Availability::Available:
    return "available";
  }
  return "unknown";
}

} // namespace

LocalAnalysis analyzeLocals(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph) {
  LocalAnalysis result;
  if (!function.code.has_value()) {
    return result;
  }
  const auto &unit = *function.code;
  const auto locals = functionLocalTypes(module, function);
  const auto local_count = locals.size();
  const auto block_count = graph.blocks.size();
  const auto &handle = module.function_handles.at(function.handle);
  const auto parameter_count = module.signatures.at(handle.parameters).size();

  std::vector<std::optional<DefinitionId>> instruction_definition(unit.code.size());
  std::vector<std::vector<DefinitionId>> definitions_by_local(local_count);
  for (std::size_t local = 0; local < parameter_count; ++local) {
    const auto id = result.definitions.size();
    result.definitions.push_back({
        .id = id,
        .local = static_cast<LocalIndex>(local),
        .instruction = std::nullopt,
        .parameter = true,
    });
    definitions_by_local[local].push_back(id);
  }
  for (std::size_t instruction = 0; instruction < unit.code.size(); ++instruction) {
    if (!localWrite(unit.code[instruction].opcode)) {
      continue;
    }
    const auto local = localIndex(unit.code[instruction]);
    const auto id = result.definitions.size();
    result.definitions.push_back({
        .id = id,
        .local = static_cast<LocalIndex>(local),
        .instruction = instruction,
        .parameter = false,
    });
    definitions_by_local[local].push_back(id);
    instruction_definition[instruction] = id;
  }
  result.definition_uses.resize(result.definitions.size());

  const auto definition_count = result.definitions.size();
  std::vector<Bits> reaching_in(block_count, Bits(definition_count, false));
  std::vector<Bits> reaching_out(block_count, Bits(definition_count, false));
  Bits entry_definitions(definition_count, false);
  for (DefinitionId id = 0; id < parameter_count; ++id) {
    entry_definitions[id] = true;
  }

  auto transferDefinitions = [&](const BasicBlock &block, Bits state) {
    for (std::size_t instruction = block.begin; instruction < block.end; ++instruction) {
      const auto &opcode = unit.code[instruction];
      if (localWrite(opcode.opcode) || opcode.opcode == Opcode::MoveLoc) {
        const auto local = localIndex(opcode);
        for (const auto definition : definitions_by_local[local]) {
          state[definition] = false;
        }
      }
      if (localWrite(opcode.opcode)) {
        state[*instruction_definition[instruction]] = true;
      }
    }
    return state;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    ++result.fixed_point_iterations;
    for (const auto &block : graph.blocks) {
      if (!block.reachable) {
        continue;
      }
      Bits incoming(definition_count, false);
      if (block.id == 0) {
        incoming = entry_definitions;
      }
      for (const auto predecessor : block.predecessors) {
        if (!graph.blocks[predecessor].reachable) {
          continue;
        }
        for (DefinitionId id = 0; id < definition_count; ++id) {
          incoming[id] = incoming[id] || reaching_out[predecessor][id];
        }
      }
      const auto outgoing = transferDefinitions(block, incoming);
      if (incoming != reaching_in[block.id] || outgoing != reaching_out[block.id]) {
        reaching_in[block.id] = incoming;
        reaching_out[block.id] = outgoing;
        changed = true;
      }
    }
  }

  std::vector<std::vector<bool>> must_in(block_count, std::vector<bool>(local_count, true));
  std::vector<std::vector<bool>> must_out(block_count, std::vector<bool>(local_count, true));
  std::vector<std::vector<bool>> may_in(block_count, std::vector<bool>(local_count, false));
  std::vector<std::vector<bool>> may_out(block_count, std::vector<bool>(local_count, false));
  std::vector<bool> entry_available(local_count, false);
  std::fill(entry_available.begin(), entry_available.begin() + static_cast<std::ptrdiff_t>(parameter_count), true);

  const auto transferAvailability = [&](const BasicBlock &block, std::vector<bool> state) {
    for (std::size_t instruction = block.begin; instruction < block.end; ++instruction) {
      const auto &operation = unit.code[instruction];
      if (operation.opcode == Opcode::MoveLoc) {
        state[localIndex(operation)] = false;
      } else if (localWrite(operation.opcode)) {
        state[localIndex(operation)] = true;
      }
    }
    return state;
  };

  changed = true;
  while (changed) {
    changed = false;
    ++result.fixed_point_iterations;
    for (const auto &block : graph.blocks) {
      if (!block.reachable) {
        continue;
      }
      std::vector<bool> must(local_count, true);
      std::vector<bool> may(local_count, false);
      bool has_input = false;
      if (block.id == 0) {
        must = entry_available;
        may = entry_available;
        has_input = true;
      }
      for (const auto predecessor : block.predecessors) {
        if (!graph.blocks[predecessor].reachable) {
          continue;
        }
        if (!has_input) {
          must = must_out[predecessor];
          may = may_out[predecessor];
          has_input = true;
        } else {
          for (std::size_t local = 0; local < local_count; ++local) {
            must[local] = must[local] && must_out[predecessor][local];
            may[local] = may[local] || may_out[predecessor][local];
          }
        }
      }
      if (!has_input) {
        std::fill(must.begin(), must.end(), false);
      }
      const auto next_must = transferAvailability(block, must);
      const auto next_may = transferAvailability(block, may);
      if (must != must_in[block.id] || may != may_in[block.id] || next_must != must_out[block.id] || next_may != may_out[block.id]) {
        must_in[block.id] = must;
        may_in[block.id] = may;
        must_out[block.id] = next_must;
        may_out[block.id] = next_may;
        changed = true;
      }
    }
  }

  result.availability_in.assign(block_count, std::vector<Availability>(local_count));
  result.availability_out.assign(block_count, std::vector<Availability>(local_count));
  for (const auto &block : graph.blocks) {
    for (std::size_t local = 0; local < local_count; ++local) {
      result.availability_in[block.id][local] = availability(must_in[block.id][local], may_in[block.id][local]);
      result.availability_out[block.id][local] = availability(must_out[block.id][local], may_out[block.id][local]);
    }
  }

  for (const auto &block : graph.blocks) {
    if (!block.reachable) {
      continue;
    }
    auto definitions = reaching_in[block.id];
    auto available = must_in[block.id];
    for (std::size_t instruction = block.begin; instruction < block.end; ++instruction) {
      const auto &operation = unit.code[instruction];
      if (localRead(operation.opcode)) {
        const auto local = localIndex(operation);
        if (!available[local]) {
          invalidLocal(instruction, static_cast<LocalIndex>(local), operation.opcode);
        }
        LocalUse use{
            .instruction = instruction,
            .local = static_cast<LocalIndex>(local),
            .opcode = operation.opcode,
            .reaching_definitions = {},
        };
        for (const auto definition : definitions_by_local[local]) {
          if (definitions[definition]) {
            use.reaching_definitions.push_back(definition);
          }
        }
        const auto use_id = result.uses.size();
        for (const auto definition : use.reaching_definitions) {
          result.definition_uses[definition].push_back(use_id);
        }
        result.uses.push_back(std::move(use));
      }
      if (localWrite(operation.opcode) || operation.opcode == Opcode::MoveLoc) {
        const auto local = localIndex(operation);
        for (const auto definition : definitions_by_local[local]) {
          definitions[definition] = false;
        }
      }
      if (operation.opcode == Opcode::MoveLoc) {
        available[localIndex(operation)] = false;
      } else if (localWrite(operation.opcode)) {
        const auto local = localIndex(operation);
        definitions[*instruction_definition[instruction]] = true;
        available[local] = true;
      }
    }
  }

  result.live_in.assign(block_count, std::vector<bool>(local_count, false));
  result.live_out.assign(block_count, std::vector<bool>(local_count, false));
  changed = true;
  while (changed) {
    changed = false;
    ++result.fixed_point_iterations;
    for (auto iterator = graph.blocks.rbegin(); iterator != graph.blocks.rend(); ++iterator) {
      const auto &block = *iterator;
      if (!block.reachable) {
        continue;
      }
      std::vector<bool> outgoing(local_count, false);
      for (const auto &edge : block.successors) {
        if (!graph.blocks[edge.target].reachable) {
          continue;
        }
        for (std::size_t local = 0; local < local_count; ++local) {
          outgoing[local] = outgoing[local] || result.live_in[edge.target][local];
        }
      }
      auto incoming = outgoing;
      for (std::size_t instruction = block.end; instruction-- > block.begin;) {
        const auto &operation = unit.code[instruction];
        if (localWrite(operation.opcode)) {
          incoming[localIndex(operation)] = false;
        }
        if (localRead(operation.opcode)) {
          incoming[localIndex(operation)] = true;
        }
      }
      if (incoming != result.live_in[block.id] || outgoing != result.live_out[block.id]) {
        result.live_in[block.id] = incoming;
        result.live_out[block.id] = outgoing;
        changed = true;
      }
    }
  }

  result.reaching_in.assign(block_count, std::vector<std::vector<DefinitionId>>(local_count));
  result.reaching_out.assign(block_count, std::vector<std::vector<DefinitionId>>(local_count));
  for (const auto &block : graph.blocks) {
    for (std::size_t local = 0; local < local_count; ++local) {
      for (const auto definition : definitions_by_local[local]) {
        if (reaching_in[block.id][definition]) {
          result.reaching_in[block.id][local].push_back(definition);
        }
        if (reaching_out[block.id][definition]) {
          result.reaching_out[block.id][local].push_back(definition);
        }
      }
    }
  }
  return result;
}

std::string formatLocalAnalysis(const Module &module, const FunctionDefinition &function, const LocalAnalysis &analysis) {
  const auto locals = functionLocalTypes(module, function);
  std::ostringstream out;
  out << "locals: " << locals.size() << "\ndefinitions: " << analysis.definitions.size() << "\nuses: " << analysis.uses.size()
      << "\nfixed-point-iterations: " << analysis.fixed_point_iterations << '\n';
  for (const auto &definition : analysis.definitions) {
    out << "  d" << definition.id << " local#" << static_cast<unsigned>(definition.local) << " @";
    if (definition.parameter) {
      out << "parameter";
    } else {
      out << *definition.instruction;
    }
    out << " uses={";
    for (std::size_t index = 0; index < analysis.definition_uses[definition.id].size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      out << 'u' << analysis.definition_uses[definition.id][index];
    }
    out << "}\n";
  }
  out << "\nlocal-uses:\n";
  for (std::size_t index = 0; index < analysis.uses.size(); ++index) {
    const auto &use = analysis.uses[index];
    out << "  u" << index << " @" << use.instruction << ' ' << opcodeInfo(use.opcode).name << " local#" << static_cast<unsigned>(use.local) << " <- {";
    for (std::size_t definition = 0; definition < use.reaching_definitions.size(); ++definition) {
      if (definition != 0) {
        out << ", ";
      }
      out << 'd' << use.reaching_definitions[definition];
    }
    out << "}\n";
  }

  out << "\nblocks:\n";
  for (std::size_t block = 0; block < analysis.live_in.size(); ++block) {
    out << "  bb" << block << '\n';
    for (std::size_t local = 0; local < locals.size(); ++local) {
      out << "    local#" << local << ": " << renderType(module, locals[local]) << " availability=" << availabilityName(analysis.availability_in[block][local])
          << "->" << availabilityName(analysis.availability_out[block][local]) << " live=" << (analysis.live_in[block][local] ? "in" : "-") << '/'
          << (analysis.live_out[block][local] ? "out" : "-") << " reaching-in={";
      for (std::size_t index = 0; index < analysis.reaching_in[block][local].size(); ++index) {
        if (index != 0) {
          out << ", ";
        }
        out << 'd' << analysis.reaching_in[block][local][index];
      }
      out << "} reaching-out={";
      for (std::size_t index = 0; index < analysis.reaching_out[block][local].size(); ++index) {
        if (index != 0) {
          out << ", ";
        }
        out << 'd' << analysis.reaching_out[block][local][index];
      }
      out << "}\n";
    }
  }
  return out.str();
}

} // namespace movescape
