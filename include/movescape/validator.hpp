#pragma once

#include "movescape/module.hpp"

namespace movescape {

// Validates the cross-table and per-instruction invariants required by later
// analyses. This is intentionally separate from parsing: table order does not
// constrain references, so complete bounds checks happen after every table has
// been decoded.
//
// This is not yet a replacement for Aptos's full bytecode verifier. Typed
// stack interpretation, local availability, ability constraints, and reference
// safety are enforced; the remaining verifier-parity audit is documented in
// ROADMAP.md.
void validateModule(const Module &module);

// Builds an analysis-only module containing a synthetic self module and
// `main` definition around a script's common pools and code. The synthetic
// declarations are never serialized or compared as script identity.
[[nodiscard]] Module scriptValidationModule(const Script &script);
void validateScript(const Script &script);

} // namespace movescape
