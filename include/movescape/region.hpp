#pragma once

#include "movescape/expression_ir.hpp"
#include "movescape/graph_analysis.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace movescape {

enum class RegionKind {
  Sequence,
  BasicBlock,
  If,
  Assert,
  Loop,
  While,
  PostTestLoop,
  Break,
  Continue,
  Return,
  Abort,
  GotoFallback,
};

struct Region;
using RegionPtr = std::shared_ptr<Region>;

struct Region {
  RegionKind kind = RegionKind::Sequence;
  std::optional<BlockId> block;
  std::vector<BlockId> condition_blocks;
  std::vector<ExpressionStatement> statements;
  ExpressionPtr condition;
  std::vector<ExpressionPtr> values;
  std::vector<RegionPtr> children;
  std::optional<BlockId> target;
};

struct StructuredFunction {
  RegionPtr root;
  bool complete = false;
  std::vector<BlockId> missing_blocks;
  std::vector<BlockId> duplicated_blocks;
};

[[nodiscard]] StructuredFunction structureControlFlow(const ControlFlowGraph &graph, const GraphAnalysis &analysis, const ExpressionFunction &expressions);
[[nodiscard]] std::string formatStructuredFunction(const Module &module, const StructuredFunction &function);
[[nodiscard]] std::string renderStructuredBody(const Module &module, const StructuredFunction &function, std::size_t indentation_depth = 0);

} // namespace movescape
