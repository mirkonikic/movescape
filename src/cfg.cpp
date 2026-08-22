#include "movescape/cfg.hpp"

#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"

#include <algorithm>
#include <deque>
#include <set>
#include <sstream>
#include <string>

namespace movescape {

namespace {

[[noreturn]] void invalidCfg(std::string message) { throw Error(ErrorCode::InvalidIndex, Error::UnknownOffset, std::move(message)); }
[[nodiscard]] bool isAbort(Opcode opcode) noexcept { return opcode == Opcode::Abort || opcode == Opcode::AbortMsg; }
[[nodiscard]] bool isReturn(Opcode opcode) noexcept { return opcode == Opcode::Ret; }

[[nodiscard]] std::size_t branchTarget(const Instruction &instruction, std::size_t code_size) {
  if (instruction.operands.empty()) { invalidCfg("branch instruction has no target"); }
  const auto target = static_cast<std::size_t>(instruction.operands.front());
  if (target >= code_size) { invalidCfg("branch target is outside the code unit"); }
  return target;
}

void appendPredecessor(BasicBlock &block, BlockId predecessor) {
  if (std::find(block.predecessors.begin(), block.predecessors.end(), predecessor) == block.predecessors.end()) { block.predecessors.push_back(predecessor); }
}

[[nodiscard]] std::string blockName(BlockId id) { return "bb" + std::to_string(id); }

[[nodiscard]] std::string dotEscape(std::string text) {
  std::string result;
  result.reserve(text.size());
  for (const char character : text) {
    switch (character) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\l";
      break;
    default:
      result += character;
      break;
    }
  }
  return result;
}

} // namespace

const char *edgeKindName(EdgeKind kind) noexcept {
  switch (kind) {
  case EdgeKind::Fallthrough:
    return "fallthrough";
  case EdgeKind::Branch:
    return "branch";
  case EdgeKind::True:
    return "true";
  case EdgeKind::False:
    return "false";
  }
  return "unknown";
}

ControlFlowGraph buildControlFlowGraph(const CodeUnit &unit) {
  ControlFlowGraph graph;
  const auto code_size = unit.code.size();
  graph.instruction_to_block.resize(code_size);
  if (code_size == 0) { return graph; }

  // searching for leaders in cfg
  std::set<std::size_t> leaders{0};
  for (std::size_t index = 0; index < code_size; ++index) {
    const auto &instruction = unit.code[index];
    const auto &info = opcodeInfo(instruction.opcode);
    if (info.conditional_branch || info.unconditional_branch) { leaders.insert(branchTarget(instruction, code_size)); }
    if (info.terminator && index + 1 < code_size) { leaders.insert(index + 1); }
  }

  // building basic block for each leader
  const std::vector<std::size_t> starts(leaders.begin(), leaders.end());
  graph.blocks.reserve(starts.size());
  for (std::size_t index = 0; index < starts.size(); ++index) {
    const auto end = index + 1 < starts.size() ? starts[index + 1] : code_size;
    graph.blocks.push_back(BasicBlock{
        .id = index,
        .begin = starts[index],
        .end = end,
        .successors = {},
        .predecessors = {},
        .exit_kind = BlockExitKind::None,
        .reachable = false,
    });
    for (std::size_t instruction = starts[index]; instruction < end; ++instruction) {
      graph.instruction_to_block[instruction] = index;
    }
  }

  // setting lambda function or adding edges -> adding successor
  const auto addEdge = [&](BasicBlock &source, std::size_t target_instruction, EdgeKind kind) {
    const auto target = graph.instruction_to_block[target_instruction];
    source.successors.push_back({.target = target, .kind = kind});
  };

  for (std::size_t index = 0; index < graph.blocks.size(); ++index) {
    auto &block = graph.blocks[index];
    const auto last_index = block.end - 1;
    const auto &last = unit.code[last_index];
    const auto &info = opcodeInfo(last.opcode);

    if (last.opcode == Opcode::Branch) {
      addEdge(block, branchTarget(last, code_size), EdgeKind::Branch);
    } else if (last.opcode == Opcode::BrTrue) {
      addEdge(block, branchTarget(last, code_size), EdgeKind::True);
      if (last_index + 1 >= code_size) {
        invalidCfg("conditional branch has no false successor");
      }
      addEdge(block, last_index + 1, EdgeKind::False);
    } else if (last.opcode == Opcode::BrFalse) {
      addEdge(block, branchTarget(last, code_size), EdgeKind::False);
      if (last_index + 1 >= code_size) {
        invalidCfg("conditional branch has no true successor");
      }
      addEdge(block, last_index + 1, EdgeKind::True);
    } else if (isReturn(last.opcode)) {
      block.exit_kind = BlockExitKind::Return;
      graph.return_exits.push_back(block.id);
    } else if (isAbort(last.opcode)) {
      block.exit_kind = BlockExitKind::Abort;
      graph.abort_exits.push_back(block.id);
    } else if (last_index + 1 < code_size) {
      addEdge(block, last_index + 1, EdgeKind::Fallthrough);
    } else {
      block.exit_kind = BlockExitKind::FallOff;
      graph.falloff_exits.push_back(block.id);
    }

    if (info.terminator && block.successors.empty() && block.exit_kind == BlockExitKind::None) {
      invalidCfg("unclassified terminating instruction");
    }
  }

  for (const auto &block : graph.blocks) {
    for (const auto &edge : block.successors) {
      appendPredecessor(graph.blocks[edge.target], block.id);
    }
  }

  std::deque<BlockId> pending{0};
  graph.blocks[0].reachable = true;
  while (!pending.empty()) {
    const auto current = pending.front();
    pending.pop_front();
    for (const auto &edge : graph.blocks[current].successors) {
      if (!graph.blocks[edge.target].reachable) {
        graph.blocks[edge.target].reachable = true;
        pending.push_back(edge.target);
      }
    }
  }

  return graph;
}

std::string formatControlFlowGraph(const Module &module, const CodeUnit &unit, const ControlFlowGraph &graph) {
  std::ostringstream out;
  for (const auto &block : graph.blocks) {
    out << blockName(block.id) << " [" << block.begin << ", " << block.end << ")" << (block.reachable ? "" : " unreachable") << '\n';
    out << "  predecessors:";
    for (const auto predecessor : block.predecessors) {
      out << ' ' << blockName(predecessor);
    }
    out << '\n';
    for (std::size_t index = block.begin; index < block.end; ++index) {
      out << "  " << renderInstruction(module, unit.code[index], index) << '\n';
    }
    out << "  successors:";
    for (const auto &edge : block.successors) {
      out << ' ' << edgeKindName(edge.kind) << "->" << blockName(edge.target);
    }
    if (block.exit_kind == BlockExitKind::Return) {
      out << " return-exit";
    } else if (block.exit_kind == BlockExitKind::Abort) {
      out << " abort-exit";
    } else if (block.exit_kind == BlockExitKind::FallOff) {
      out << " falloff-exit";
    }
    out << "\n\n";
  }
  return out.str();
}

std::string controlFlowGraphDot(const Module &module, const CodeUnit &unit, const ControlFlowGraph &graph) {
  std::ostringstream out;
  out << "digraph cfg {\n"
      << "  node [shape=box, fontname=\"monospace\"];\n";
  for (const auto &block : graph.blocks) {
    std::ostringstream label;
    label << blockName(block.id);
    if (!block.reachable) {
      label << " (unreachable)";
    }
    label << '\n';
    for (std::size_t index = block.begin; index < block.end; ++index) {
      label << renderInstruction(module, unit.code[index], index) << '\n';
    }
    out << "  " << blockName(block.id) << " [label=\"" << dotEscape(label.str()) << "\"";
    if (!block.reachable) {
      out << ", style=dashed, color=gray";
    }
    out << "];\n";
  }
  for (const auto &block : graph.blocks) {
    for (const auto &edge : block.successors) {
      out << "  " << blockName(block.id) << " -> " << blockName(edge.target) << " [label=\"" << edgeKindName(edge.kind) << "\"];\n";
    }
  }
  out << "}\n";
  return out.str();
}

} // namespace movescape
