#pragma once

#include "movescape/cfg.hpp"
#include "movescape/module.hpp"
#include "movescape/stackless_ir.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace movescape {

enum class ExpressionEffect {
  Pure,
  MayAbort,
  SideEffect,
};

struct Expression;
using ExpressionPtr = std::shared_ptr<Expression>;

struct Expression {
  Opcode opcode = Opcode::Nop;
  Type type;
  std::vector<ExpressionPtr> operands;
  std::vector<std::uint64_t> immediate_operands;
  std::vector<std::uint8_t> wide_immediate;
  std::size_t bytecode_index = 0;
  std::optional<ValueId> value;
  std::size_t result_index = 0;
  ExpressionEffect effect = ExpressionEffect::Pure;
  bool short_circuit = false;
  bool conditional = false;
  std::optional<std::string> atom;
  std::optional<std::string> local_name;
};

enum class ExpressionStatementKind {
  BindTemporary,
  AssignLocal,
  Discard,
  Effect,
  Destructure,
};

struct ExpressionStatement {
  ExpressionStatementKind kind = ExpressionStatementKind::Effect;
  std::size_t bytecode_index = 0;
  std::optional<LocalIndex> local;
  std::optional<std::string> local_name;
  std::vector<ValueId> generated_values;
  ExpressionPtr expression;
};

enum class ExpressionTerminatorKind {
  None,
  Goto,
  Conditional,
  Return,
  Abort,
};

struct ExpressionTerminator {
  ExpressionTerminatorKind kind = ExpressionTerminatorKind::None;
  std::size_t bytecode_index = 0;
  std::vector<ExpressionPtr> values;
  std::optional<BlockId> true_target;
  std::optional<BlockId> false_target;
};

struct ExpressionBlock {
  BlockId id = 0;
  std::vector<ExpressionStatement> statements;
  ExpressionTerminator terminator;
};

struct ExpressionFunction {
  Signature locals;
  std::size_t parameter_count = 0;
  std::vector<ExpressionBlock> blocks;
};

[[nodiscard]] ExpressionFunction recoverExpressions(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph,
                                                    const StacklessFunction &stackless);
[[nodiscard]] std::string renderExpression(const Module &module, const ExpressionPtr &expression);
[[nodiscard]] std::string renderConstantValue(const Module &module, std::size_t index);
[[nodiscard]] bool expressionSourceSemanticsComplete(const Module &module, const ExpressionPtr &expression);
[[nodiscard]] std::string renderExpressionStatement(const Module &module, const ExpressionStatement &statement);
[[nodiscard]] bool isUnspecifiedAbortCode(const ExpressionPtr &expression) noexcept;
[[nodiscard]] std::string formatExpressionFunction(const Module &module, const ExpressionFunction &function);

} // namespace movescape
