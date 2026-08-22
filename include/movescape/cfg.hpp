#pragma once

#include "movescape/module.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace movescape {

using BlockId = std::size_t;

enum class EdgeKind {
  Fallthrough,
  Branch,
  True,
  False,
};

struct ControlFlowEdge {
  BlockId target = 0;
  EdgeKind kind = EdgeKind::Fallthrough;

  friend bool operator==(const ControlFlowEdge &, const ControlFlowEdge &) = default;
};

enum class BlockExitKind {
  None,
  Return,
  Abort,
  FallOff,
};

struct BasicBlock {
  BlockId id = 0;
  std::size_t begin = 0;
  std::size_t end = 0;
  std::vector<ControlFlowEdge> successors;
  std::vector<BlockId> predecessors;
  BlockExitKind exit_kind = BlockExitKind::None;
  bool reachable = false;
};

struct ControlFlowGraph {
  std::vector<BasicBlock> blocks;
  std::vector<BlockId> instruction_to_block;
  std::vector<BlockId> return_exits;
  std::vector<BlockId> abort_exits;
  std::vector<BlockId> falloff_exits;
};

[[nodiscard]] ControlFlowGraph buildControlFlowGraph(const CodeUnit &unit);
[[nodiscard]] std::string formatControlFlowGraph(const Module &module, const CodeUnit &unit, const ControlFlowGraph &graph);
[[nodiscard]] std::string controlFlowGraphDot(const Module &module, const CodeUnit &unit, const ControlFlowGraph &graph);
[[nodiscard]] const char *edgeKindName(EdgeKind kind) noexcept;

} // namespace movescape
