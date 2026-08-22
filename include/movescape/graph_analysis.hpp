#pragma once

#include "movescape/cfg.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace movescape {

struct DominatorAnalysis {
  std::vector<std::vector<BlockId>> dominators;
  std::vector<std::optional<BlockId>> immediate_dominator;
  std::vector<std::vector<BlockId>> tree_children;
  std::vector<std::vector<BlockId>> dominance_frontier;
};

struct PostDominatorAnalysis {
  BlockId synthetic_exit = 0;
  std::vector<bool> can_reach_exit;
  std::vector<std::vector<BlockId>> postdominators;
  std::vector<std::optional<BlockId>> immediate_postdominator;
};

struct StronglyConnectedComponents {
  std::vector<std::vector<BlockId>> components;
  std::vector<std::size_t> component_of;
};

struct BackEdge {
  BlockId source = 0;
  BlockId header = 0;

  friend bool operator==(const BackEdge &, const BackEdge &) = default;
};

struct NaturalLoop {
  BlockId header = 0;
  std::vector<BlockId> latches;
  std::vector<BlockId> members;
  std::optional<std::size_t> parent;
};

struct GraphAnalysis {
  std::vector<BlockId> reverse_postorder;
  StronglyConnectedComponents sccs;
  DominatorAnalysis dominators;
  PostDominatorAnalysis postdominators;
  std::vector<BackEdge> back_edges;
  std::vector<NaturalLoop> natural_loops;
  std::vector<std::vector<BlockId>> control_dependents;
  std::vector<std::vector<BlockId>> irreducible_regions;

  [[nodiscard]] bool reducible() const noexcept { return irreducible_regions.empty(); }
};

[[nodiscard]] GraphAnalysis analyzeControlFlowGraph(const ControlFlowGraph &graph);
[[nodiscard]] std::string formatGraphAnalysis(const GraphAnalysis &analysis);

} // namespace movescape
