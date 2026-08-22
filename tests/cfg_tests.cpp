#include "test.hpp"

#include "movescape/cfg.hpp"

namespace {

movescape::Instruction op(movescape::Opcode opcode, std::vector<std::uint64_t> operands = {}) { return {.opcode = opcode, .operands = std::move(operands)}; }

movescape::CodeUnit code(std::vector<movescape::Instruction> instructions) {
  movescape::CodeUnit unit;
  unit.code = std::move(instructions);
  return unit;
}

} // namespace

TEST(cfg_straight_line) {
  const auto unit = code({op(movescape::Opcode::LdTrue), op(movescape::Opcode::Pop), op(movescape::Opcode::Ret)});
  const auto graph = movescape::buildControlFlowGraph(unit);
  REQUIRE_EQ(graph.blocks.size(), 1U);
  REQUIRE_EQ(graph.blocks[0].begin, 0U);
  REQUIRE_EQ(graph.blocks[0].end, 3U);
  REQUIRE(graph.blocks[0].reachable);
  REQUIRE_EQ(graph.return_exits, std::vector<movescape::BlockId>{0});
}

TEST(cfg_diamond_labels_true_and_false_edges) {
  const auto unit = code({
      op(movescape::Opcode::LdTrue),
      op(movescape::Opcode::BrTrue, {4}),
      op(movescape::Opcode::LdU64, {1}),
      op(movescape::Opcode::Branch, {5}),
      op(movescape::Opcode::LdU64, {2}),
      op(movescape::Opcode::Ret),
  });
  const auto graph = movescape::buildControlFlowGraph(unit);
  REQUIRE_EQ(graph.blocks.size(), 4U);
  REQUIRE_EQ(graph.blocks[0].successors.size(), 2U);
  REQUIRE_EQ(graph.blocks[0].successors[0], (movescape::ControlFlowEdge{.target = 2, .kind = movescape::EdgeKind::True}));
  REQUIRE_EQ(graph.blocks[0].successors[1], (movescape::ControlFlowEdge{.target = 1, .kind = movescape::EdgeKind::False}));
  REQUIRE_EQ(graph.blocks[3].predecessors.size(), 2U);
}

TEST(cfg_loop_has_back_edge) {
  const auto unit = code({
      op(movescape::Opcode::LdTrue),
      op(movescape::Opcode::BrFalse, {4}),
      op(movescape::Opcode::Nop),
      op(movescape::Opcode::Branch, {0}),
      op(movescape::Opcode::Ret),
  });
  const auto graph = movescape::buildControlFlowGraph(unit);
  REQUIRE_EQ(graph.blocks.size(), 3U);
  REQUIRE_EQ(graph.blocks[1].successors[0].target, 0U);
  REQUIRE_EQ(graph.blocks[0].predecessors, std::vector<movescape::BlockId>{1});
}

TEST(cfg_preserves_and_marks_dead_code) {
  const auto unit = code({
      op(movescape::Opcode::Branch, {2}),
      op(movescape::Opcode::Ret),
      op(movescape::Opcode::Ret),
  });
  const auto graph = movescape::buildControlFlowGraph(unit);
  REQUIRE_EQ(graph.blocks.size(), 3U);
  REQUIRE(graph.blocks[0].reachable);
  REQUIRE(!graph.blocks[1].reachable);
  REQUIRE(graph.blocks[2].reachable);
  REQUIRE_EQ(graph.return_exits.size(), 2U);
}

TEST(cfg_rejects_out_of_range_branch) {
  const auto unit = code({op(movescape::Opcode::Branch, {1})});
  REQUIRE_ERROR(movescape::buildControlFlowGraph(unit), movescape::ErrorCode::InvalidIndex);
}
