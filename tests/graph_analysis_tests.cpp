#include "test.hpp"

#include "movescape/cfg.hpp"
#include "movescape/graph_analysis.hpp"

namespace {

movescape::Instruction op(movescape::Opcode opcode, std::vector<std::uint64_t> operands = {}) { return {.opcode = opcode, .operands = std::move(operands)}; }

movescape::ControlFlowGraph graph(std::vector<movescape::Instruction> instructions) {
  movescape::CodeUnit unit;
  unit.code = std::move(instructions);
  return movescape::buildControlFlowGraph(unit);
}

} // namespace

TEST(diamond_dominators_postdominators_and_control_dependence) {
  const auto cfg = graph({
      op(movescape::Opcode::LdTrue),
      op(movescape::Opcode::BrTrue, {4}),
      op(movescape::Opcode::LdU64, {1}),
      op(movescape::Opcode::Branch, {5}),
      op(movescape::Opcode::LdU64, {2}),
      op(movescape::Opcode::Ret),
  });
  const auto result = movescape::analyzeControlFlowGraph(cfg);
  REQUIRE_EQ(result.dominators.immediate_dominator[1], std::optional<movescape::BlockId>{0});
  REQUIRE_EQ(result.dominators.immediate_dominator[2], std::optional<movescape::BlockId>{0});
  REQUIRE_EQ(result.dominators.immediate_dominator[3], std::optional<movescape::BlockId>{0});
  REQUIRE_EQ(result.postdominators.immediate_postdominator[0], std::optional<movescape::BlockId>{3});
  REQUIRE_EQ(result.control_dependents[0], (std::vector<movescape::BlockId>{1, 2}));
  REQUIRE(result.reducible());
}

TEST(loop_scc_back_edge_and_natural_loop) {
  const auto cfg = graph({
      op(movescape::Opcode::LdTrue),
      op(movescape::Opcode::BrFalse, {4}),
      op(movescape::Opcode::Nop),
      op(movescape::Opcode::Branch, {0}),
      op(movescape::Opcode::Ret),
  });
  const auto result = movescape::analyzeControlFlowGraph(cfg);
  REQUIRE_EQ(result.back_edges, (std::vector<movescape::BackEdge>{
                                    {.source = 1, .header = 0},
                                }));
  REQUIRE_EQ(result.natural_loops.size(), 1U);
  REQUIRE_EQ(result.natural_loops[0].members, (std::vector<movescape::BlockId>{0, 1}));
  REQUIRE_EQ(result.sccs.components[0], (std::vector<movescape::BlockId>{0, 1}));
  REQUIRE(result.reducible());
}

TEST(multiple_entry_cycle_is_irreducible) {
  const auto cfg = graph({
      op(movescape::Opcode::BrTrue, {2}),
      op(movescape::Opcode::Branch, {3}),
      op(movescape::Opcode::Branch, {3}),
      op(movescape::Opcode::BrTrue, {1}),
      op(movescape::Opcode::Ret),
  });
  const auto result = movescape::analyzeControlFlowGraph(cfg);
  REQUIRE(!result.reducible());
  REQUIRE_EQ(result.irreducible_regions.size(), 1U);
  REQUIRE_EQ(result.irreducible_regions[0], (std::vector<movescape::BlockId>{1, 3}));
}

TEST(infinite_loop_has_no_exit_path_or_postdominator) {
  const auto cfg = graph({op(movescape::Opcode::Branch, {0})});
  const auto result = movescape::analyzeControlFlowGraph(cfg);
  REQUIRE(!result.postdominators.can_reach_exit[0]);
  REQUIRE(!result.postdominators.immediate_postdominator[0].has_value());
  REQUIRE_EQ(result.back_edges.size(), 1U);
}
