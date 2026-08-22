#pragma once

#include "movescape/module.hpp"

#include <vector>

namespace movescape {

struct StacklessFunction;

[[nodiscard]] Type substituteType(const Type &type, const Signature &type_arguments);
[[nodiscard]] Signature substituteSignature(const Signature &signature, const Signature &type_arguments);
[[nodiscard]] Signature functionLocalTypes(const Module &module, const FunctionDefinition &function);
[[nodiscard]] bool isTypeAssignable(const Type &expected, const Type &actual) noexcept;
void inferAndValidateStacklessTypes(const Module &module, const FunctionDefinition &function, StacklessFunction &stackless);

} // namespace movescape
