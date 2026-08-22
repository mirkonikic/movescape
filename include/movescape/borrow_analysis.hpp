#pragma once

#include "movescape/cfg.hpp"
#include "movescape/module.hpp"
#include "movescape/stackless_ir.hpp"

namespace movescape {

void validateBorrowSafety(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph, const StacklessFunction &stackless);

} // namespace movescape
