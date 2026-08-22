#include "movescape/region.hpp"

#include "movescape/error.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace movescape {

namespace {

[[nodiscard]] RegionPtr region(RegionKind kind) {
  auto result = std::make_shared<Region>();
  result->kind = kind;
  return result;
}

[[nodiscard]] RegionPtr sequence() { return region(RegionKind::Sequence); }

void append(RegionPtr &parent, RegionPtr child) {
  if (child && (child->kind != RegionKind::Sequence || !child->children.empty())) {
    parent->children.push_back(std::move(child));
  }
}

[[nodiscard]] bool contains(const NaturalLoop &loop, BlockId block) { return std::binary_search(loop.members.begin(), loop.members.end(), block); }

struct LoopContext {
  const NaturalLoop *loop = nullptr;
  std::optional<BlockId> follow;
};

class Structurer {
public:
  Structurer(const ControlFlowGraph &graph, const GraphAnalysis &analysis, const ExpressionFunction &expressions) : graph_(graph), analysis_(analysis), expressions_(expressions) {
    for (std::size_t index = 0; index < analysis_.natural_loops.size(); ++index) { loops_by_header_[analysis_.natural_loops[index].header] = index; }
  }

  [[nodiscard]] StructuredFunction run() {
    StructuredFunction result;
    if (graph_.blocks.empty()) {
      result.root = sequence();
      result.complete = true;
      return result;
    }
    result.root = build(0, std::nullopt, std::nullopt, {});
    foldEnumConditionTemporaries(result.root);
    foldCompoundConditions(result.root);
    foldAssertions(result.root);
    foldConditionalExpressions(result.root);

    std::vector<std::size_t> coverage(graph_.blocks.size(), 0);
    collectCoverage(result.root, coverage);
    for (const auto &block : graph_.blocks) {
      if (!block.reachable) {
        continue;
      }
      if (coverage[block.id] == 0) {
        result.missing_blocks.push_back(block.id);
      } else if (coverage[block.id] > 1) {
        result.duplicated_blocks.push_back(block.id);
      }
    }
    result.complete = analysis_.reducible() && result.missing_blocks.empty() && !used_fallback_;
    return result;
  }

private:
  struct NestedConditional {
    RegionPtr value;
    std::vector<BlockId> condition_blocks;
  };

  struct TerminalAbort {
    RegionPtr value;
    std::vector<BlockId> covered_blocks;
  };

  struct TerminalReturn {
    RegionPtr value;
    std::vector<BlockId> covered_blocks;
  };

  struct BranchAssignment {
    ExpressionStatement statement;
    std::vector<BlockId> covered_blocks;
  };

  [[nodiscard]] static bool sameExpression(const ExpressionPtr &left, const ExpressionPtr &right) {
    if (left == right) {
      return true;
    }
    if (!left || !right || left->opcode != right->opcode || left->type != right->type || left->immediate_operands != right->immediate_operands ||
        left->wide_immediate != right->wide_immediate || left->result_index != right->result_index || left->short_circuit != right->short_circuit ||
        left->conditional != right->conditional || left->atom != right->atom || left->local_name != right->local_name ||
        left->operands.size() != right->operands.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left->operands.size(); ++index) {
      if (!sameExpression(left->operands[index], right->operands[index])) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static bool sameStatement(const ExpressionStatement &left, const ExpressionStatement &right) {
    return left.kind == right.kind && left.local == right.local && left.local_name == right.local_name && left.generated_values == right.generated_values &&
           sameExpression(left.expression, right.expression);
  }

  [[nodiscard]] static bool sameRegion(const RegionPtr &left, const RegionPtr &right) {
    if (left == right) {
      return true;
    }
    if (!left || !right || left->kind != right->kind || left->block != right->block || left->target != right->target ||
        left->condition_blocks != right->condition_blocks || left->statements.size() != right->statements.size() ||
        !sameExpression(left->condition, right->condition) || left->values.size() != right->values.size() || left->children.size() != right->children.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left->statements.size(); ++index) {
      if (!sameStatement(left->statements[index], right->statements[index])) {
        return false;
      }
    }
    for (std::size_t index = 0; index < left->values.size(); ++index) {
      if (!sameExpression(left->values[index], right->values[index])) {
        return false;
      }
    }
    for (std::size_t index = 0; index < left->children.size(); ++index) {
      if (!sameRegion(left->children[index], right->children[index])) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static std::optional<NestedConditional> nestedConditional(const RegionPtr &candidate) {
    if (!candidate) {
      return std::nullopt;
    }
    if (candidate->kind == RegionKind::If && candidate->children.size() == 2) {
      return NestedConditional{.value = candidate, .condition_blocks = {}};
    }
    if (candidate->kind != RegionKind::Sequence) {
      return std::nullopt;
    }

    NestedConditional result;
    for (const auto &child : candidate->children) {
      if (child->kind == RegionKind::BasicBlock && child->statements.empty() && child->block.has_value()) {
        result.condition_blocks.push_back(*child->block);
        continue;
      }
      if (!result.value && child->kind == RegionKind::If && child->children.size() == 2) {
        result.value = child;
        continue;
      }
      return std::nullopt;
    }
    if (!result.value) {
      return std::nullopt;
    }
    return result;
  }

  [[nodiscard]] static ExpressionPtr compoundCondition(Opcode opcode, const ExpressionPtr &left, const ExpressionPtr &right) {
    return std::make_shared<Expression>(Expression{
        .opcode = opcode,
        .type =
            Type{
                .kind = TypeKind::Bool,
                .index = 0,
                .abilities = {},
                .arguments = {},
                .results = {},
            },
        .operands = {left, right},
        .immediate_operands = {},
        .wide_immediate = {},
        .bytecode_index = left ? left->bytecode_index : 0,
        .value = std::nullopt,
        .result_index = 0,
        .effect = ExpressionEffect::Pure,
        .short_circuit = true,
        .conditional = false,
        .atom = std::nullopt,
        .local_name = std::nullopt,
    });
  }

  [[nodiscard]] static bool localValue(const ExpressionPtr &expression, LocalIndex local) {
    return expression && (expression->opcode == Opcode::CopyLoc || expression->opcode == Opcode::MoveLoc) && expression->immediate_operands.size() == 1 &&
           expression->immediate_operands[0] == local;
  }

  [[nodiscard]] static ExpressionPtr booleanLiteral(bool value, std::size_t index) {
    return std::make_shared<Expression>(Expression{
        .opcode = value ? Opcode::LdTrue : Opcode::LdFalse,
        .type = Type{.kind = TypeKind::Bool, .index = 0, .abilities = {}, .arguments = {}, .results = {}},
        .operands = {},
        .immediate_operands = {},
        .wide_immediate = {},
        .bytecode_index = index,
        .value = std::nullopt,
        .result_index = 0,
        .effect = ExpressionEffect::Pure,
        .short_circuit = false,
        .conditional = false,
        .atom = std::nullopt,
        .local_name = std::nullopt,
    });
  }

  [[nodiscard]] static std::optional<LocalIndex> testedLocal(const ExpressionPtr &condition) {
    if (!condition || (condition->opcode != Opcode::TestVariant && condition->opcode != Opcode::TestVariantGeneric) || condition->operands.size() != 1) {
      return std::nullopt;
    }
    const auto &tested = condition->operands[0];
    if (!tested || (tested->opcode != Opcode::CopyLoc && tested->opcode != Opcode::MoveLoc) || tested->immediate_operands.size() != 1 ||
        (tested->type.kind != TypeKind::Reference && tested->type.kind != TypeKind::MutableReference)) {
      return std::nullopt;
    }
    return static_cast<LocalIndex>(tested->immediate_operands[0]);
  }

  [[nodiscard]] static bool enumCleanup(const ExpressionStatement &statement, const ExpressionPtr &condition) {
    const auto tested = testedLocal(condition);
    return tested.has_value() && statement.kind == ExpressionStatementKind::Discard && localValue(statement.expression, *tested);
  }

  [[nodiscard]] static bool rewriteReturnedConditionLocal(const RegionPtr &branch, LocalIndex local, bool value, const ExpressionPtr &enum_condition) {
    if (!branch) {
      return false;
    }
    RegionPtr returned;
    std::vector<RegionPtr> cleanup_blocks;
    if (branch->kind == RegionKind::Return && branch->values.size() == 1 && localValue(branch->values[0], local)) {
      returned = branch;
    } else if (branch->kind == RegionKind::Sequence) {
      for (const auto &child : branch->children) {
        if (child->kind == RegionKind::BasicBlock) {
          if (child->statements.empty()) {
            continue;
          }
          if (child->statements.size() == 1 && enumCleanup(child->statements[0], enum_condition)) {
            cleanup_blocks.push_back(child);
            continue;
          }
          return false;
        }
        if (!returned && child->kind == RegionKind::Return && child->values.size() == 1 && localValue(child->values[0], local)) {
          returned = child;
          continue;
        }
        return false;
      }
    }
    if (!returned) {
      return false;
    }
    returned->values[0] = booleanLiteral(value, returned->values[0] ? returned->values[0]->bytecode_index : 0);
    for (const auto &block : cleanup_blocks) {
      block->statements.clear();
    }
    return true;
  }

  [[nodiscard]] static bool expressionUsesLocal(const ExpressionPtr &value, LocalIndex local) {
    if (!value) {
      return false;
    }
    if ((value->opcode == Opcode::CopyLoc || value->opcode == Opcode::MoveLoc || value->opcode == Opcode::ImmBorrowLoc ||
         value->opcode == Opcode::MutBorrowLoc) &&
        value->immediate_operands.size() == 1 && value->immediate_operands[0] == local) {
      return true;
    }
    return std::any_of(value->operands.begin(), value->operands.end(), [&](const auto &operand) { return expressionUsesLocal(operand, local); });
  }

  [[nodiscard]] static bool regionUsesLocal(const RegionPtr &value, LocalIndex local) {
    if (!value) {
      return false;
    }
    if (expressionUsesLocal(value->condition, local)) {
      return true;
    }
    for (const auto &statement : value->statements) {
      if ((statement.local.has_value() && *statement.local == local) || expressionUsesLocal(statement.expression, local)) {
        return true;
      }
    }
    for (const auto &expression : value->values) {
      if (expressionUsesLocal(expression, local)) {
        return true;
      }
    }
    return std::any_of(value->children.begin(), value->children.end(), [&](const auto &child) { return regionUsesLocal(child, local); });
  }

  static void foldEnumConditionTemporaries(const RegionPtr &value) {
    if (!value) {
      return;
    }
    for (const auto &child : value->children) {
      foldEnumConditionTemporaries(child);
    }
    if (value->kind != RegionKind::Sequence || value->children.size() < 2) {
      return;
    }
    for (std::size_t index = 0; index + 1 < value->children.size(); ++index) {
      const auto &header = value->children[index];
      const auto &conditional = value->children[index + 1];
      if (header->kind != RegionKind::BasicBlock || header->statements.size() != 1 || header->statements[0].kind != ExpressionStatementKind::AssignLocal ||
          !header->statements[0].local.has_value() || conditional->kind != RegionKind::If || conditional->children.size() != 2) {
        continue;
      }
      const auto local = *header->statements[0].local;
      const auto expression = header->statements[0].expression;
      if (!testedLocal(expression).has_value() || !localValue(conditional->condition, local)) {
        continue;
      }
      (void)rewriteReturnedConditionLocal(conditional->children[0], local, true, expression);
      (void)rewriteReturnedConditionLocal(conditional->children[1], local, false, expression);
      if (regionUsesLocal(conditional->children[0], local) || regionUsesLocal(conditional->children[1], local)) {
        continue;
      }
      conditional->condition = expression;
      header->statements.clear();
    }
  }

  [[nodiscard]] static ExpressionPtr negatedCondition(const ExpressionPtr &condition, std::size_t bytecode_index) {
    return std::make_shared<Expression>(Expression{
        .opcode = Opcode::Not,
        .type =
            Type{
                .kind = TypeKind::Bool,
                .index = 0,
                .abilities = {},
                .arguments = {},
                .results = {},
            },
        .operands = {condition},
        .immediate_operands = {},
        .wide_immediate = {},
        .bytecode_index = bytecode_index,
        .value = std::nullopt,
        .result_index = 0,
        .effect = ExpressionEffect::Pure,
        .short_circuit = false,
        .conditional = false,
        .atom = std::nullopt,
        .local_name = std::nullopt,
    });
  }

  static void foldCompoundConditions(const RegionPtr &value) {
    if (!value) {
      return;
    }
    for (const auto &child : value->children) {
      foldCompoundConditions(child);
    }
    if (value->kind != RegionKind::If || value->children.size() != 2) {
      return;
    }

    while (true) {
      if (const auto nested = nestedConditional(value->children[0]); nested.has_value() && sameRegion(value->children[1], nested->value->children[1])) {
        value->condition = compoundCondition(Opcode::And, value->condition, nested->value->condition);
        value->condition_blocks.insert(value->condition_blocks.end(), nested->condition_blocks.begin(), nested->condition_blocks.end());
        value->condition_blocks.insert(value->condition_blocks.end(), nested->value->condition_blocks.begin(), nested->value->condition_blocks.end());
        value->children = {nested->value->children[0], value->children[1]};
        continue;
      }
      if (const auto nested = nestedConditional(value->children[1]); nested.has_value() && sameRegion(value->children[0], nested->value->children[0])) {
        value->condition = compoundCondition(Opcode::Or, value->condition, nested->value->condition);
        value->condition_blocks.insert(value->condition_blocks.end(), nested->condition_blocks.begin(), nested->condition_blocks.end());
        value->condition_blocks.insert(value->condition_blocks.end(), nested->value->condition_blocks.begin(), nested->value->condition_blocks.end());
        value->children = {value->children[0], nested->value->children[1]};
        continue;
      }
      return;
    }
  }

  [[nodiscard]] static std::optional<TerminalAbort> terminalAbort(const RegionPtr &candidate) {
    if (!candidate) {
      return std::nullopt;
    }
    if (candidate->kind == RegionKind::Abort && candidate->values.size() == 1) {
      return TerminalAbort{.value = candidate, .covered_blocks = {}};
    }
    if (candidate->kind != RegionKind::Sequence) {
      return std::nullopt;
    }

    TerminalAbort result;
    for (const auto &child : candidate->children) {
      if (child->kind == RegionKind::BasicBlock && child->statements.empty() && child->block.has_value()) {
        result.covered_blocks.push_back(*child->block);
        continue;
      }
      if (!result.value && child->kind == RegionKind::Abort && child->values.size() == 1) {
        result.value = child;
        continue;
      }
      return std::nullopt;
    }
    if (!result.value) {
      return std::nullopt;
    }
    return result;
  }

  static void foldAssertions(const RegionPtr &value) {
    if (!value) {
      return;
    }
    for (const auto &child : value->children) {
      foldAssertions(child);
    }
    if (value->kind != RegionKind::If || value->children.size() != 2 || !value->condition) {
      return;
    }

    const auto true_abort = terminalAbort(value->children[0]);
    const auto false_abort = terminalAbort(value->children[1]);
    if (true_abort.has_value() == false_abort.has_value()) {
      return;
    }

    const auto &aborting = true_abort.has_value() ? *true_abort : *false_abort;
    auto assertion = region(RegionKind::Assert);
    assertion->condition = true_abort.has_value() ? negatedCondition(value->condition, value->condition->bytecode_index) : value->condition;
    assertion->values = aborting.value->values;
    assertion->condition_blocks = value->condition_blocks;
    assertion->condition_blocks.insert(assertion->condition_blocks.end(), aborting.covered_blocks.begin(), aborting.covered_blocks.end());

    auto continuation = true_abort.has_value() ? value->children[1] : value->children[0];
    value->kind = RegionKind::Sequence;
    value->block = std::nullopt;
    value->condition_blocks.clear();
    value->statements.clear();
    value->condition.reset();
    value->values.clear();
    value->children = {std::move(assertion), std::move(continuation)};
    value->target = std::nullopt;
  }

  static void appendCoveredBlocks(std::vector<BlockId> &destination, const std::vector<BlockId> &source) {
    for (const auto block : source) {
      if (std::find(destination.begin(), destination.end(), block) == destination.end()) {
        destination.push_back(block);
      }
    }
  }

  [[nodiscard]] static std::optional<TerminalReturn> terminalReturn(const RegionPtr &candidate) {
    if (!candidate) {
      return std::nullopt;
    }
    if (candidate->kind == RegionKind::Return && !candidate->values.empty()) {
      return TerminalReturn{.value = candidate, .covered_blocks = candidate->condition_blocks};
    }
    if (candidate->kind != RegionKind::Sequence) {
      return std::nullopt;
    }

    TerminalReturn result;
    for (const auto &child : candidate->children) {
      if (child->kind == RegionKind::BasicBlock && child->statements.empty() && child->block.has_value()) {
        result.covered_blocks.push_back(*child->block);
        continue;
      }
      if (!result.value && child->kind == RegionKind::Return && !child->values.empty()) {
        result.value = child;
        appendCoveredBlocks(result.covered_blocks, child->condition_blocks);
        continue;
      }
      return std::nullopt;
    }
    if (!result.value) {
      return std::nullopt;
    }
    return result;
  }

  [[nodiscard]] static std::optional<BranchAssignment> branchAssignment(const RegionPtr &candidate) {
    if (!candidate) {
      return std::nullopt;
    }
    std::vector<RegionPtr> blocks;
    if (candidate->kind == RegionKind::BasicBlock) {
      blocks.push_back(candidate);
    } else if (candidate->kind == RegionKind::Sequence) {
      blocks = candidate->children;
    } else {
      return std::nullopt;
    }

    std::optional<ExpressionStatement> assignment;
    std::vector<BlockId> covered_blocks;
    for (const auto &block : blocks) {
      if (block->kind != RegionKind::BasicBlock || !block->block.has_value()) {
        return std::nullopt;
      }
      covered_blocks.push_back(*block->block);
      if (block->statements.empty()) {
        continue;
      }
      if (assignment.has_value() || block->statements.size() != 1 || block->statements.front().kind != ExpressionStatementKind::AssignLocal ||
          !block->statements.front().local.has_value()) {
        return std::nullopt;
      }
      assignment = block->statements.front();
    }
    if (!assignment.has_value()) {
      return std::nullopt;
    }
    return BranchAssignment{
        .statement = std::move(*assignment),
        .covered_blocks = std::move(covered_blocks),
    };
  }

  [[nodiscard]] static ExpressionPtr conditionalExpression(const ExpressionPtr &condition, const ExpressionPtr &true_value, const ExpressionPtr &false_value) {
    if (sameExpression(true_value, false_value)) {
      return true_value;
    }
    if (true_value && false_value && true_value->opcode == Opcode::LdTrue && false_value->opcode == Opcode::LdFalse) {
      return condition;
    }
    if (true_value && false_value && true_value->opcode == Opcode::LdFalse && false_value->opcode == Opcode::LdTrue) {
      return negatedCondition(condition, condition ? condition->bytecode_index : 0);
    }
    if (true_value && true_value->opcode == Opcode::LdTrue) {
      return compoundCondition(Opcode::Or, condition, false_value);
    }
    if (false_value && false_value->opcode == Opcode::LdFalse) {
      return compoundCondition(Opcode::And, condition, true_value);
    }
    return std::make_shared<Expression>(Expression{
        .opcode = Opcode::Nop,
        .type = true_value->type,
        .operands = {condition, true_value, false_value},
        .immediate_operands = {},
        .wide_immediate = {},
        .bytecode_index = condition ? condition->bytecode_index : 0,
        .value = std::nullopt,
        .result_index = 0,
        .effect = ExpressionEffect::MayAbort,
        .short_circuit = false,
        .conditional = true,
        .atom = std::nullopt,
        .local_name = std::nullopt,
    });
  }

  static void foldConditionalExpressions(const RegionPtr &value) {
    if (!value) {
      return;
    }
    for (const auto &child : value->children) {
      foldConditionalExpressions(child);
    }
    if (value->kind != RegionKind::If || value->children.size() != 2 || !value->condition) {
      return;
    }

    const auto true_return = terminalReturn(value->children[0]);
    const auto false_return = terminalReturn(value->children[1]);
    if (true_return.has_value() && false_return.has_value() && true_return->value->values.size() == false_return->value->values.size()) {
      std::vector<ExpressionPtr> results;
      results.reserve(true_return->value->values.size());
      for (std::size_t index = 0; index < true_return->value->values.size(); ++index) {
        results.push_back(conditionalExpression(value->condition, true_return->value->values[index], false_return->value->values[index]));
      }
      appendCoveredBlocks(value->condition_blocks, true_return->covered_blocks);
      appendCoveredBlocks(value->condition_blocks, false_return->covered_blocks);
      value->kind = RegionKind::Return;
      value->block = std::nullopt;
      value->statements.clear();
      value->condition.reset();
      value->values = std::move(results);
      value->children.clear();
      value->target = std::nullopt;
      return;
    }

    const auto true_assignment = branchAssignment(value->children[0]);
    const auto false_assignment = branchAssignment(value->children[1]);
    if (!true_assignment.has_value() || !false_assignment.has_value() || true_assignment->statement.local != false_assignment->statement.local) {
      return;
    }

    auto statement = true_assignment->statement;
    statement.expression = conditionalExpression(value->condition, true_assignment->statement.expression, false_assignment->statement.expression);
    appendCoveredBlocks(value->condition_blocks, true_assignment->covered_blocks);
    appendCoveredBlocks(value->condition_blocks, false_assignment->covered_blocks);
    value->kind = RegionKind::BasicBlock;
    value->block = std::nullopt;
    value->statements = {std::move(statement)};
    value->condition.reset();
    value->values.clear();
    value->children.clear();
    value->target = std::nullopt;
  }

  [[nodiscard]] std::optional<BlockId> loopFollow(const NaturalLoop &loop) const {
    auto current = loop.header;
    std::size_t steps = 0;
    while (steps++ <= graph_.blocks.size()) {
      const auto next = analysis_.postdominators.immediate_postdominator[current];
      if (!next.has_value() || *next == analysis_.postdominators.synthetic_exit) {
        break;
      }
      if (!contains(loop, *next)) {
        return *next;
      }
      current = *next;
    }

    std::vector<BlockId> exits;
    for (const auto member : loop.members) {
      for (const auto &edge : graph_.blocks[member].successors) {
        if (contains(loop, edge.target)) {
          continue;
        }
        const auto exit_kind = graph_.blocks[edge.target].exit_kind;
        if (exit_kind == BlockExitKind::Return || exit_kind == BlockExitKind::Abort) {
          continue;
        }
        exits.push_back(edge.target);
      }
    }
    std::sort(exits.begin(), exits.end());
    exits.erase(std::unique(exits.begin(), exits.end()), exits.end());
    if (exits.empty()) {
      return std::nullopt;
    }

    std::optional<BlockId> best;
    std::size_t best_depth = 0;
    for (BlockId candidate = 0; candidate < graph_.blocks.size(); ++candidate) {
      if (contains(loop, candidate)) {
        continue;
      }
      const bool common = std::all_of(exits.begin(), exits.end(), [&](BlockId exit) {
        const auto &set = analysis_.postdominators.postdominators[exit];
        return std::find(set.begin(), set.end(), candidate) != set.end();
      });
      if (!common) {
        continue;
      }
      const auto depth = analysis_.postdominators.postdominators[candidate].size();
      if (!best.has_value() || depth > best_depth) {
        best = candidate;
        best_depth = depth;
      }
    }
    return best;
  }

  [[nodiscard]] RegionPtr basicBlock(BlockId block) const {
    auto result = region(RegionKind::BasicBlock);
    result->block = block;
    result->statements = expressions_.blocks.at(block).statements;
    return result;
  }

  static void removeTrailingContinue(const RegionPtr &value) {
    if (value && value->kind == RegionKind::Sequence && !value->children.empty() && value->children.back()->kind == RegionKind::Continue) {
      value->children.pop_back();
    }
  }

  [[nodiscard]] RegionPtr preTestedLoop(const NaturalLoop &loop, std::optional<BlockId> follow) {
    if (!follow.has_value()) {
      return {};
    }
    const auto &header = expressions_.blocks.at(loop.header);
    const auto &terminator = header.terminator;
    if (!header.statements.empty() || terminator.kind != ExpressionTerminatorKind::Conditional || !terminator.true_target.has_value() ||
        !terminator.false_target.has_value() || terminator.values.size() != 1) {
      return {};
    }

    const bool true_inside = contains(loop, *terminator.true_target);
    const bool false_inside = contains(loop, *terminator.false_target);
    if (true_inside == false_inside) {
      return {};
    }
    const auto body_target = true_inside ? *terminator.true_target : *terminator.false_target;
    const auto exit_target = true_inside ? *terminator.false_target : *terminator.true_target;
    if (exit_target != *follow) {
      return {};
    }

    auto result = region(RegionKind::While);
    result->block = loop.header;
    if (true_inside) {
      result->condition = terminator.values.front();
    } else {
      result->condition = negatedCondition(terminator.values.front(), terminator.bytecode_index);
    }
    append(result, basicBlock(loop.header));
    auto body = build(body_target, follow, LoopContext{&loop, follow}, {loop.header});
    removeTrailingContinue(body);
    append(result, std::move(body));
    return result;
  }

  [[nodiscard]] RegionPtr postTestedLoop(const NaturalLoop &loop, std::optional<BlockId> follow) {
    if (!follow.has_value() || loop.latches.size() != 1) {
      return {};
    }
    const auto latch = loop.latches.front();
    const auto &terminator = expressions_.blocks.at(latch).terminator;
    if (terminator.kind != ExpressionTerminatorKind::Conditional || !terminator.true_target.has_value() || !terminator.false_target.has_value() ||
        terminator.values.size() != 1) {
      return {};
    }

    const bool true_repeats = *terminator.true_target == loop.header;
    const bool false_repeats = *terminator.false_target == loop.header;
    if (true_repeats == false_repeats) {
      return {};
    }
    const auto exit_target = true_repeats ? *terminator.false_target : *terminator.true_target;
    if (exit_target != *follow) {
      return {};
    }

    auto result = region(RegionKind::PostTestLoop);
    result->block = loop.header;
    result->condition = true_repeats ? negatedCondition(terminator.values.front(), terminator.bytecode_index) : terminator.values.front();
    append(result, build(loop.header, latch, LoopContext{&loop, follow}, {}));
    append(result, basicBlock(latch));
    return result;
  }

  [[nodiscard]] std::vector<std::optional<std::size_t>> distancesInside(BlockId start, const NaturalLoop &loop) const {
    std::vector<std::optional<std::size_t>> distance(graph_.blocks.size());
    if (!contains(loop, start) || start == loop.header) {
      return distance;
    }
    std::deque<BlockId> pending{start};
    distance[start] = 0;
    while (!pending.empty()) {
      const auto current = pending.front();
      pending.pop_front();
      for (const auto &edge : graph_.blocks[current].successors) {
        if (!contains(loop, edge.target) || edge.target == loop.header || distance[edge.target].has_value()) {
          continue;
        }
        distance[edge.target] = *distance[current] + 1;
        pending.push_back(edge.target);
      }
    }
    return distance;
  }

  [[nodiscard]] std::optional<BlockId> partialJoin(BlockId left, BlockId right, const NaturalLoop &loop) const {
    const auto left_distance = distancesInside(left, loop);
    const auto right_distance = distancesInside(right, loop);
    std::optional<BlockId> best;
    std::size_t best_cost = 0;
    for (const auto candidate : loop.members) {
      if (candidate == loop.header || !left_distance[candidate].has_value() || !right_distance[candidate].has_value()) {
        continue;
      }
      const auto cost = *left_distance[candidate] + *right_distance[candidate];
      if (!best.has_value() || cost < best_cost || (cost == best_cost && candidate < *best)) {
        best = candidate;
        best_cost = cost;
      }
    }
    return best;
  }

  [[nodiscard]] RegionPtr control(RegionKind kind, std::optional<BlockId> target = std::nullopt) {
    auto result = region(kind);
    result->target = target;
    if (kind == RegionKind::GotoFallback) {
      used_fallback_ = true;
    }
    return result;
  }

  [[nodiscard]] RegionPtr branch(BlockId target, std::optional<BlockId> stop, std::optional<LoopContext> active_loop, std::set<BlockId> path) {
    if (active_loop.has_value()) {
      if (target == active_loop->loop->header) {
        return control(RegionKind::Continue);
      }
      if (!contains(*active_loop->loop, target)) {
        auto result = sequence();
        if (!active_loop->follow.has_value()) {
          append(result, build(target, std::nullopt, std::nullopt, std::move(path)));
          return result;
        }
        if (target != *active_loop->follow) {
          append(result, build(target, active_loop->follow, std::nullopt, std::move(path)));
        }
        if (!endsControl(result)) {
          append(result, control(RegionKind::Break));
        }
        return result;
      }
    }
    if (stop.has_value() && target == *stop) {
      return sequence();
    }
    return build(target, stop, active_loop, std::move(path));
  }

  [[nodiscard]] RegionPtr build(BlockId start, std::optional<BlockId> stop, std::optional<LoopContext> active_loop, std::set<BlockId> path) {
    auto result = sequence();
    auto current = start;

    while (!stop.has_value() || current != *stop) {
      if (current >= graph_.blocks.size() || !graph_.blocks[current].reachable) {
        append(result, control(RegionKind::GotoFallback, current));
        return result;
      }
      if (active_loop.has_value() && current == active_loop->loop->header && path.contains(current)) {
        append(result, control(RegionKind::Continue));
        return result;
      }
      if (!path.insert(current).second) {
        append(result, control(RegionKind::GotoFallback, current));
        return result;
      }

      const auto loop_iterator = loops_by_header_.find(current);
      const bool is_active_header = active_loop.has_value() && active_loop->loop->header == current;
      if (loop_iterator != loops_by_header_.end() && !is_active_header) {
        const auto &loop = analysis_.natural_loops[loop_iterator->second];
        const auto follow = loopFollow(loop);
        auto loop_region = preTestedLoop(loop, follow);
        if (!loop_region) {
          loop_region = postTestedLoop(loop, follow);
        }
        if (!loop_region) {
          loop_region = region(RegionKind::Loop);
          loop_region->block = loop.header;
          append(loop_region, build(loop.header, follow, LoopContext{&loop, follow}, {}));
        }
        append(result, std::move(loop_region));
        if (!follow.has_value()) {
          return result;
        }
        current = *follow;
        continue;
      }

      append(result, basicBlock(current));
      const auto &terminator = expressions_.blocks.at(current).terminator;
      switch (terminator.kind) {
      case ExpressionTerminatorKind::Return: {
        auto return_region = region(RegionKind::Return);
        return_region->values = terminator.values;
        append(result, std::move(return_region));
        return result;
      }
      case ExpressionTerminatorKind::Abort: {
        auto abort_region = region(RegionKind::Abort);
        abort_region->values = terminator.values;
        append(result, std::move(abort_region));
        return result;
      }
      case ExpressionTerminatorKind::Goto: {
        if (!terminator.true_target.has_value()) {
          append(result, control(RegionKind::GotoFallback));
          return result;
        }
        const auto target = *terminator.true_target;
        if (active_loop.has_value() && target == active_loop->loop->header) {
          append(result, control(RegionKind::Continue));
          return result;
        }
        if (active_loop.has_value() && !contains(*active_loop->loop, target)) {
          append(result, branch(target, stop, active_loop, path));
          return result;
        }
        if (stop.has_value() && target == *stop) {
          return result;
        }
        current = target;
        break;
      }
      case ExpressionTerminatorKind::Conditional: {
        if (!terminator.true_target.has_value() || !terminator.false_target.has_value() || terminator.values.empty()) {
          append(result, control(RegionKind::GotoFallback));
          return result;
        }
        std::optional<BlockId> join = analysis_.postdominators.immediate_postdominator[current];
        if (join.has_value() && *join == analysis_.postdominators.synthetic_exit) {
          join = std::nullopt;
        }
        if (active_loop.has_value()) {
          if (stop.has_value() && contains(*active_loop->loop, *stop)) {
            const auto true_distance = distancesInside(*terminator.true_target, *active_loop->loop);
            const auto false_distance = distancesInside(*terminator.false_target, *active_loop->loop);
            if (true_distance[*stop].has_value() || false_distance[*stop].has_value()) {
              join = stop;
            }
          }
          if (join.has_value() && !contains(*active_loop->loop, *join)) {
            const auto partial = partialJoin(*terminator.true_target, *terminator.false_target, *active_loop->loop);
            join = partial.has_value() ? partial : active_loop->follow;
          }
        }

        auto if_region = region(RegionKind::If);
        if_region->condition = terminator.values.front();
        append(if_region, branch(*terminator.true_target, join, active_loop, path));
        append(if_region, branch(*terminator.false_target, join, active_loop, path));
        append(result, std::move(if_region));
        if (!join.has_value()) {
          return result;
        }
        if (active_loop.has_value() && !contains(*active_loop->loop, *join)) {
          return result;
        }
        if (stop.has_value() && *join == *stop) {
          return result;
        }
        current = *join;
        break;
      }
      case ExpressionTerminatorKind::None:
        append(result, control(RegionKind::GotoFallback));
        return result;
      }
    }
    return result;
  }

  static void collectCoverage(const RegionPtr &region, std::vector<std::size_t> &coverage) {
    if (!region) {
      return;
    }
    if (region->kind == RegionKind::BasicBlock && region->block.has_value() && *region->block < coverage.size()) {
      ++coverage[*region->block];
    }
    for (const auto block : region->condition_blocks) {
      if (block < coverage.size()) {
        ++coverage[block];
      }
    }
    for (const auto &child : region->children) {
      collectCoverage(child, coverage);
    }
  }

  static bool endsControl(const RegionPtr &value) {
    if (!value) {
      return false;
    }
    switch (value->kind) {
    case RegionKind::Break:
    case RegionKind::Continue:
    case RegionKind::Return:
    case RegionKind::Abort:
    case RegionKind::GotoFallback:
      return true;
    case RegionKind::Sequence:
    case RegionKind::Loop:
      return !value->children.empty() && endsControl(value->children.back());
    case RegionKind::While:
    case RegionKind::PostTestLoop:
    case RegionKind::Assert:
      return false;
    case RegionKind::If:
      return value->children.size() == 2 && endsControl(value->children[0]) && endsControl(value->children[1]);
    case RegionKind::BasicBlock:
      return false;
    }
    return false;
  }

  const ControlFlowGraph &graph_;
  const GraphAnalysis &analysis_;
  const ExpressionFunction &expressions_;
  std::map<BlockId, std::size_t> loops_by_header_;
  bool used_fallback_ = false;
};

void indent(std::ostringstream &out, std::size_t depth) { out << std::string(depth * 2, ' '); }

[[nodiscard]] const ExpressionStatement *singleAssignment(const RegionPtr &region) {
  if (!region) {
    return nullptr;
  }
  if (region->kind == RegionKind::BasicBlock) {
    if (region->statements.size() == 1 && region->statements[0].kind == ExpressionStatementKind::AssignLocal) {
      return &region->statements[0];
    }
    return nullptr;
  }
  if (region->kind != RegionKind::Sequence) {
    return nullptr;
  }
  const ExpressionStatement *result = nullptr;
  for (const auto &child : region->children) {
    if (child->kind == RegionKind::BasicBlock && child->statements.empty()) {
      continue;
    }
    const auto *candidate = singleAssignment(child);
    if (!candidate || result) {
      return nullptr;
    }
    result = candidate;
  }
  return result;
}

[[nodiscard]] std::string renderAssignmentExpression(const Module &module, const ExpressionStatement &statement) {
  const auto name = statement.local_name.value_or("local" + std::to_string(static_cast<unsigned>(*statement.local)));
  return name + " = " + renderExpression(module, statement.expression);
}

void renderRegion(std::ostringstream &out, const Module &module, const RegionPtr &region, std::size_t depth) {
  if (!region) {
    return;
  }
  switch (region->kind) {
  case RegionKind::Sequence:
    for (const auto &child : region->children) {
      renderRegion(out, module, child, depth);
    }
    break;
  case RegionKind::BasicBlock:
    for (const auto &statement : region->statements) {
      indent(out, depth);
      out << renderExpressionStatement(module, statement) << '\n';
    }
    break;
  case RegionKind::If:
    if (region->children.size() == 2) {
      const auto *true_assignment = singleAssignment(region->children[0]);
      const auto *false_assignment = singleAssignment(region->children[1]);
      if (true_assignment && false_assignment) {
        indent(out, depth);
        out << "if (" << renderExpression(module, region->condition) << ") " << renderAssignmentExpression(module, *true_assignment) << " else "
            << renderAssignmentExpression(module, *false_assignment) << ";\n";
        break;
      }
    }
    indent(out, depth);
    out << "if (" << renderExpression(module, region->condition) << ") {\n";
    if (!region->children.empty()) {
      renderRegion(out, module, region->children[0], depth + 1);
    }
    indent(out, depth);
    out << '}';
    if (region->children.size() >= 2 && (!region->children[1]->children.empty() || region->children[1]->kind != RegionKind::Sequence)) {
      out << " else {\n";
      renderRegion(out, module, region->children[1], depth + 1);
      indent(out, depth);
      out << '}';
    }
    out << '\n';
    break;
  case RegionKind::Assert:
    indent(out, depth);
    out << "assert!(" << renderExpression(module, region->condition) << ", " << renderExpression(module, region->values.at(0)) << ");\n";
    break;
  case RegionKind::Loop:
    indent(out, depth);
    out << "loop {\n";
    for (const auto &child : region->children) {
      renderRegion(out, module, child, depth + 1);
    }
    indent(out, depth);
    out << "}\n";
    break;
  case RegionKind::While:
    indent(out, depth);
    out << "while (" << renderExpression(module, region->condition) << ") {\n";
    for (const auto &child : region->children) {
      renderRegion(out, module, child, depth + 1);
    }
    indent(out, depth);
    out << "}\n";
    break;
  case RegionKind::PostTestLoop:
    indent(out, depth);
    out << "loop {\n";
    for (const auto &child : region->children) {
      renderRegion(out, module, child, depth + 1);
    }
    indent(out, depth + 1);
    out << "if (" << renderExpression(module, region->condition) << ") {\n";
    indent(out, depth + 2);
    out << "break;\n";
    indent(out, depth + 1);
    out << "}\n";
    indent(out, depth);
    out << "}\n";
    break;
  case RegionKind::Break:
    indent(out, depth);
    out << "break;\n";
    break;
  case RegionKind::Continue:
    indent(out, depth);
    out << "continue;\n";
    break;
  case RegionKind::Return: {
    indent(out, depth);
    out << "return";
    for (std::size_t index = 0; index < region->values.size(); ++index) {
      out << (index == 0 ? " " : ", ") << renderExpression(module, region->values[index]);
    }
    out << (region->values.empty() ? ";\n" : "\n");
    break;
  }
  case RegionKind::Abort:
    indent(out, depth);
    out << "abort ";
    if (region->values.size() == 1) {
      out << renderExpression(module, region->values.front());
    } else if (region->values.size() == 2 && isUnspecifiedAbortCode(region->values[0])) {
      out << renderExpression(module, region->values[1]);
    } else if (region->values.size() == 2) {
      out << renderExpression(module, region->values[0]) << " /* WARNING: bytecode also carries abort message " << renderExpression(module, region->values[1])
          << " */";
    } else {
      out << '(';
      for (std::size_t index = 0; index < region->values.size(); ++index) {
        if (index != 0) {
          out << ", ";
        }
        out << renderExpression(module, region->values[index]);
      }
      out << ')';
    }
    out << '\n';
    break;
  case RegionKind::GotoFallback:
    throw Error(ErrorCode::UnsupportedFeature, Error::UnknownOffset,
                "cannot render unresolved control transfer" + (region->target.has_value() ? " to bb" + std::to_string(*region->target) : std::string{}));
  }
}

} // namespace

StructuredFunction structureControlFlow(const ControlFlowGraph &graph, const GraphAnalysis &analysis, const ExpressionFunction &expressions) {
  return Structurer(graph, analysis, expressions).run();
}

std::string formatStructuredFunction(const Module &module, const StructuredFunction &function) {
  std::ostringstream out;
  out << "complete: " << (function.complete ? "yes" : "no") << '\n';
  if (!function.missing_blocks.empty()) {
    out << "missing:";
    for (const auto block : function.missing_blocks) {
      out << " bb" << block;
    }
    out << '\n';
  }
  if (!function.duplicated_blocks.empty()) {
    out << "duplicated:";
    for (const auto block : function.duplicated_blocks) {
      out << " bb" << block;
    }
    out << '\n';
  }
  out << "{\n";
  renderRegion(out, module, function.root, 1);
  out << "}\n";
  return out.str();
}

std::string renderStructuredBody(const Module &module, const StructuredFunction &function, std::size_t indentation_depth) {
  std::ostringstream out;
  renderRegion(out, module, function.root, indentation_depth);
  return out.str();
}

} // namespace movescape
