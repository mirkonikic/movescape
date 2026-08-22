#include "movescape/type_analysis.hpp"

#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"
#include "movescape/stackless_ir.hpp"

#include <algorithm>
#include <sstream>
#include <string>

namespace movescape {

namespace {

[[noreturn]] void typeError(const Module &module, std::size_t instruction, Opcode opcode, std::string message) {
  (void)module;
  std::ostringstream out;
  out << "instruction " << instruction << " (" << opcodeInfo(opcode).name << "): " << message;
  throw Error(ErrorCode::TypeMismatch, Error::UnknownOffset, out.str());
}

[[nodiscard]] Type simple(TypeKind kind) {
  Type type;
  type.kind = kind;
  return type;
}

[[nodiscard]] Type compound(TypeKind kind, Type inner) {
  Type type;
  type.kind = kind;
  type.arguments.push_back(std::move(inner));
  return type;
}

[[nodiscard]] Type materializeStruct(TableIndex handle, const Signature &arguments) {
  Type type;
  type.kind = arguments.empty() ? TypeKind::Struct : TypeKind::StructInstantiation;
  type.index = handle;
  type.arguments = arguments;
  return type;
}

[[nodiscard]] bool isInteger(const Type &type) {
  switch (type.kind) {
  case TypeKind::U8:
  case TypeKind::U16:
  case TypeKind::U32:
  case TypeKind::U64:
  case TypeKind::U128:
  case TypeKind::U256:
  case TypeKind::I8:
  case TypeKind::I16:
  case TypeKind::I32:
  case TypeKind::I64:
  case TypeKind::I128:
  case TypeKind::I256:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool isUnsignedInteger(const Type &type) {
  return type.kind == TypeKind::U8 || type.kind == TypeKind::U16 || type.kind == TypeKind::U32 || type.kind == TypeKind::U64 || type.kind == TypeKind::U128 ||
         type.kind == TypeKind::U256;
}

[[nodiscard]] bool isSignedInteger(const Type &type) { return isInteger(type) && !isUnsignedInteger(type); }

[[nodiscard]] AbilitySet abilitiesOf(const Module &module, const FunctionDefinition &function, const Type &type) {
  constexpr std::uint8_t primitive = AbilitySet::Copy | AbilitySet::Drop | AbilitySet::Store;
  switch (type.kind) {
  case TypeKind::Bool:
  case TypeKind::U8:
  case TypeKind::U16:
  case TypeKind::U32:
  case TypeKind::U64:
  case TypeKind::U128:
  case TypeKind::U256:
  case TypeKind::I8:
  case TypeKind::I16:
  case TypeKind::I32:
  case TypeKind::I64:
  case TypeKind::I128:
  case TypeKind::I256:
  case TypeKind::Address:
    return {primitive};
  case TypeKind::Signer:
    return {AbilitySet::Drop};
  case TypeKind::Reference:
  case TypeKind::MutableReference:
    return {static_cast<std::uint8_t>(AbilitySet::Copy | AbilitySet::Drop)};
  case TypeKind::Function:
    return type.abilities;
  case TypeKind::TypeParameter: {
    const auto &handle = module.function_handles.at(function.handle);
    if (type.index >= handle.type_parameters.size()) {
      return {};
    }
    return handle.type_parameters[type.index];
  }
  case TypeKind::Vector: {
    auto result = AbilitySet{primitive};
    const auto element = abilitiesOf(module, function, type.arguments.front());
    result.bits = static_cast<std::uint8_t>(result.bits & element.bits);
    return result;
  }
  case TypeKind::Struct:
  case TypeKind::StructInstantiation: {
    const auto &handle = module.struct_handles.at(type.index);
    auto result = handle.abilities;
    for (std::size_t index = 0; index < type.arguments.size() && index < handle.type_parameters.size(); ++index) {
      if (handle.type_parameters[index].is_phantom) {
        continue;
      }
      const auto argument = abilitiesOf(module, function, type.arguments[index]);
      for (const auto ability : {AbilitySet::Copy, AbilitySet::Drop, AbilitySet::Store}) {
        if (!argument.has(ability)) {
          result.bits = static_cast<std::uint8_t>(result.bits & ~ability);
        }
      }
      if (!argument.has(AbilitySet::Store)) {
        result.bits = static_cast<std::uint8_t>(result.bits & ~AbilitySet::Key);
      }
    }
    return result;
  }
  }
  return {};
}

void requireAbilityConstraints(const Module &module, const FunctionDefinition &function, const StacklessInstruction &instruction, const Signature &arguments,
                               const std::vector<AbilitySet> &constraints) {
  if (arguments.size() != constraints.size()) {
    typeError(module, instruction.bytecode_index, instruction.opcode, "generic argument arity disagrees with ability constraints");
  }
  const auto type_parameter_count = module.function_handles.at(function.handle).type_parameters.size();
  const auto typeIsInScope = [&](const auto &self, const Type &type) -> bool {
    if (type.kind == TypeKind::TypeParameter && type.index >= type_parameter_count) {
      return false;
    }
    return std::all_of(type.arguments.begin(), type.arguments.end(), [&](const Type &argument) { return self(self, argument); }) &&
           std::all_of(type.results.begin(), type.results.end(), [&](const Type &result) { return self(self, result); });
  };
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (!typeIsInScope(typeIsInScope, arguments[index])) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "type argument " + std::to_string(index) + " refers to an out-of-scope type parameter");
    }
    const auto actual = abilitiesOf(module, function, arguments[index]);
    if ((actual.bits & constraints[index].bits) != constraints[index].bits) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "type argument " + std::to_string(index) + " does not satisfy required abilities");
    }
  }
}

void requireStructAbilityConstraints(const Module &module, const FunctionDefinition &function, const StacklessInstruction &instruction,
                                     const Signature &arguments, const std::vector<StructTypeParameter> &parameters) {
  std::vector<AbilitySet> constraints;
  constraints.reserve(parameters.size());
  for (const auto &parameter : parameters) {
    constraints.push_back(parameter.constraints);
  }
  requireAbilityConstraints(module, function, instruction, arguments, constraints);
}

struct StructUse {
  const StructDefinition *definition = nullptr;
  Signature type_arguments;
  Type type;
};

[[nodiscard]] StructUse structUse(const Module &module, const FunctionDefinition &function, const StacklessInstruction &instruction, bool generic) {
  const auto index = static_cast<std::size_t>(instruction.immediate_operands.at(0));
  const StructDefinition *definition = nullptr;
  Signature arguments;
  if (generic) {
    const auto &instantiation = module.struct_definition_instantiations.at(index);
    definition = &module.struct_definitions.at(instantiation.definition);
    arguments = module.signatures.at(instantiation.type_parameters);
    requireStructAbilityConstraints(module, function, instruction, arguments, module.struct_handles.at(definition->handle).type_parameters);
  } else {
    definition = &module.struct_definitions.at(index);
  }
  return {
      .definition = definition,
      .type_arguments = arguments,
      .type = materializeStruct(definition->handle, arguments),
  };
}

struct VariantUse {
  StructUse structure;
  std::size_t variant = 0;
};

[[nodiscard]] VariantUse variantUse(const Module &module, const FunctionDefinition &function, const StacklessInstruction &instruction, bool generic) {
  const auto index = static_cast<std::size_t>(instruction.immediate_operands.at(0));
  const StructVariantHandle *handle = nullptr;
  Signature arguments;
  if (generic) {
    const auto &instantiation = module.struct_variant_instantiations.at(index);
    handle = &module.struct_variant_handles.at(instantiation.handle);
    arguments = module.signatures.at(instantiation.type_parameters);
    const auto &definition = module.struct_definitions.at(handle->definition);
    requireStructAbilityConstraints(module, function, instruction, arguments, module.struct_handles.at(definition.handle).type_parameters);
  } else {
    handle = &module.struct_variant_handles.at(index);
  }
  const auto &definition = module.struct_definitions.at(handle->definition);
  return {
      .structure = {.definition = &definition, .type_arguments = arguments, .type = materializeStruct(definition.handle, arguments)},
      .variant = handle->variant,
  };
}

[[nodiscard]] Signature fieldsOf(const StructUse &use, std::optional<std::size_t> variant) {
  const auto &fields = variant.has_value() ? use.definition->variants.at(*variant).fields : use.definition->fields;
  Signature result;
  result.reserve(fields.size());
  for (const auto &field : fields) {
    result.push_back(substituteType(field.type, use.type_arguments));
  }
  return result;
}

struct FunctionUse {
  const FunctionHandle *handle = nullptr;
  Signature arguments;
};

[[nodiscard]] FunctionUse functionUse(const Module &module, const FunctionDefinition &function, const StacklessInstruction &instruction, bool generic) {
  const auto index = static_cast<std::size_t>(instruction.immediate_operands.at(0));
  if (generic) {
    const auto &instantiation = module.function_instantiations.at(index);
    const auto &handle = module.function_handles.at(instantiation.handle);
    const auto &arguments = module.signatures.at(instantiation.type_parameters);
    requireAbilityConstraints(module, function, instruction, arguments, handle.type_parameters);
    return {.handle = &handle, .arguments = arguments};
  }
  return {.handle = &module.function_handles.at(index), .arguments = {}};
}

[[nodiscard]] const Type &onlyElement(const Module &module, const StacklessInstruction &instruction) {
  const auto &signature = module.signatures.at(static_cast<std::size_t>(instruction.immediate_operands.at(0)));
  if (signature.size() != 1) {
    typeError(module, instruction.bytecode_index, instruction.opcode, "vector opcode signature does not contain one element type");
  }
  return signature.front();
}

void requireType(const Module &module, const StacklessInstruction &instruction, const Type &actual, const Type &expected, std::string_view role,
                 bool assignable = false) {
  if ((assignable && isTypeAssignable(expected, actual)) || (!assignable && actual == expected)) {
    return;
  }
  typeError(module, instruction.bytecode_index, instruction.opcode,
            std::string(role) + " has type " + renderType(module, actual) + ", expected " + renderType(module, expected));
}

void requireInputs(const Module &module, const StacklessInstruction &instruction, const Signature &actual, const Signature &expected, bool assignable = false) {
  if (actual.size() != expected.size()) {
    typeError(module, instruction.bytecode_index, instruction.opcode, "unexpected typed operand count");
  }
  for (std::size_t index = 0; index < actual.size(); ++index) {
    requireType(module, instruction, actual[index], expected[index], "operand " + std::to_string(index), assignable);
  }
}

[[nodiscard]] Type referenced(const Module &module, const StacklessInstruction &instruction, const Type &reference, bool mutable_only) {
  if ((reference.kind != TypeKind::Reference && reference.kind != TypeKind::MutableReference) || reference.arguments.size() != 1 ||
      (mutable_only && reference.kind != TypeKind::MutableReference)) {
    typeError(module, instruction.bytecode_index, instruction.opcode, mutable_only ? "expected a mutable reference" : "expected a reference");
  }
  return reference.arguments.front();
}

[[nodiscard]] Type vectorElementFromReference(const Module &module, const StacklessInstruction &instruction, const Type &reference, bool mutable_only) {
  const auto value = referenced(module, instruction, reference, mutable_only);
  if (value.kind != TypeKind::Vector || value.arguments.size() != 1) {
    typeError(module, instruction.bytecode_index, instruction.opcode, "reference does not point to a vector");
  }
  return value.arguments.front();
}

[[nodiscard]] Signature inferOperation(const Module &module, const FunctionDefinition &function, const Signature &locals,
                                       const StacklessInstruction &instruction, const Signature &inputs) {
  const auto boolean = simple(TypeKind::Bool);
  const auto u8 = simple(TypeKind::U8);
  const auto u64 = simple(TypeKind::U64);
  const auto address = simple(TypeKind::Address);

  const auto noResult = [&]() -> Signature { return {}; };
  const auto one = [&](Type type) -> Signature { return {std::move(type)}; };
  const auto local = [&]() -> const Type & {
    const auto index = static_cast<std::size_t>(instruction.immediate_operands.at(0));
    if (index >= locals.size()) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "local index is outside typed local vector");
    }
    return locals[index];
  };

  switch (instruction.opcode) {
  case Opcode::Pop:
    if (!abilitiesOf(module, function, inputs.at(0)).has(AbilitySet::Drop)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "popped value lacks the drop ability");
    }
    return noResult();
  case Opcode::BrTrue:
  case Opcode::BrFalse:
    requireInputs(module, instruction, inputs, {boolean});
    return noResult();
  case Opcode::StLoc:
    requireInputs(module, instruction, inputs, {local()}, true);
    return noResult();
  case Opcode::Abort:
    requireInputs(module, instruction, inputs, {u64});
    return noResult();
  case Opcode::AbortMsg:
    requireInputs(module, instruction, inputs, {u64, compound(TypeKind::Vector, u8)});
    return noResult();
  case Opcode::Ret: {
    const auto &handle = module.function_handles.at(function.handle);
    requireInputs(module, instruction, inputs, module.signatures.at(handle.returns), true);
    return noResult();
  }
  case Opcode::Branch:
  case Opcode::Nop:
    return noResult();

  case Opcode::LdU8:
    return one(simple(TypeKind::U8));
  case Opcode::LdU16:
    return one(simple(TypeKind::U16));
  case Opcode::LdU32:
    return one(simple(TypeKind::U32));
  case Opcode::LdU64:
    return one(u64);
  case Opcode::LdU128:
    return one(simple(TypeKind::U128));
  case Opcode::LdU256:
    return one(simple(TypeKind::U256));
  case Opcode::LdI8:
    return one(simple(TypeKind::I8));
  case Opcode::LdI16:
    return one(simple(TypeKind::I16));
  case Opcode::LdI32:
    return one(simple(TypeKind::I32));
  case Opcode::LdI64:
    return one(simple(TypeKind::I64));
  case Opcode::LdI128:
    return one(simple(TypeKind::I128));
  case Opcode::LdI256:
    return one(simple(TypeKind::I256));
  case Opcode::LdTrue:
  case Opcode::LdFalse:
    return one(boolean);
  case Opcode::LdConst:
    return one(module.constants.at(static_cast<std::size_t>(instruction.immediate_operands.at(0))).type);
  case Opcode::CopyLoc:
    if (!abilitiesOf(module, function, local()).has(AbilitySet::Copy)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "copied local lacks the copy ability");
    }
    return one(local());
  case Opcode::MoveLoc:
    return one(local());
  case Opcode::MutBorrowLoc:
    return one(compound(TypeKind::MutableReference, local()));
  case Opcode::ImmBorrowLoc:
    return one(compound(TypeKind::Reference, local()));

  case Opcode::FreezeRef: {
    const auto inner = referenced(module, instruction, inputs.at(0), true);
    return one(compound(TypeKind::Reference, inner));
  }
  case Opcode::ReadRef: {
    const auto inner = referenced(module, instruction, inputs.at(0), false);
    if (!abilitiesOf(module, function, inner).has(AbilitySet::Copy)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "referenced value lacks the copy ability");
    }
    return one(inner);
  }
  case Opcode::WriteRef: {
    const auto inner = referenced(module, instruction, inputs.at(1), true);
    requireType(module, instruction, inputs.at(0), inner, "written value", true);
    if (!abilitiesOf(module, function, inner).has(AbilitySet::Drop)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "overwritten value lacks the drop ability");
    }
    return noResult();
  }

  case Opcode::CastU8:
  case Opcode::CastU16:
  case Opcode::CastU32:
  case Opcode::CastU64:
  case Opcode::CastU128:
  case Opcode::CastU256:
  case Opcode::CastI8:
  case Opcode::CastI16:
  case Opcode::CastI32:
  case Opcode::CastI64:
  case Opcode::CastI128:
  case Opcode::CastI256: {
    if (!isInteger(inputs.at(0))) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "cast operand is not an integer");
    }
    const auto target = [&]() {
      switch (instruction.opcode) {
      case Opcode::CastU8:
        return TypeKind::U8;
      case Opcode::CastU16:
        return TypeKind::U16;
      case Opcode::CastU32:
        return TypeKind::U32;
      case Opcode::CastU64:
        return TypeKind::U64;
      case Opcode::CastU128:
        return TypeKind::U128;
      case Opcode::CastU256:
        return TypeKind::U256;
      case Opcode::CastI8:
        return TypeKind::I8;
      case Opcode::CastI16:
        return TypeKind::I16;
      case Opcode::CastI32:
        return TypeKind::I32;
      case Opcode::CastI64:
        return TypeKind::I64;
      case Opcode::CastI128:
        return TypeKind::I128;
      case Opcode::CastI256:
        return TypeKind::I256;
      default:
        return TypeKind::U8;
      }
    }();
    return one(simple(target));
  }

  case Opcode::Add:
  case Opcode::Sub:
  case Opcode::Mul:
  case Opcode::Mod:
  case Opcode::Div:
    if (!isInteger(inputs.at(0)) || inputs.at(0) != inputs.at(1)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "arithmetic operands are not equal integer types");
    }
    return one(inputs.at(0));
  case Opcode::BitOr:
  case Opcode::BitAnd:
  case Opcode::Xor:
    if (!isUnsignedInteger(inputs.at(0)) || inputs.at(0) != inputs.at(1)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "bitwise operands are not equal unsigned integer types");
    }
    return one(inputs.at(0));
  case Opcode::Negate:
    if (!isSignedInteger(inputs.at(0))) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "negation operand is not a signed integer");
    }
    return one(inputs.at(0));
  case Opcode::Shl:
  case Opcode::Shr:
    if (!isUnsignedInteger(inputs.at(0)) || inputs.at(1).kind != TypeKind::U8) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "shift requires an unsigned integer and a u8 count");
    }
    return one(inputs.at(0));
  case Opcode::Or:
  case Opcode::And:
    requireInputs(module, instruction, inputs, {boolean, boolean});
    return one(boolean);
  case Opcode::Not:
    requireInputs(module, instruction, inputs, {boolean});
    return one(boolean);
  case Opcode::Eq:
  case Opcode::Neq:
    if (inputs.at(0) != inputs.at(1)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "equality operands have different types");
    }
    if (!abilitiesOf(module, function, inputs.at(0)).has(AbilitySet::Drop)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "equality operand lacks the drop ability");
    }
    return one(boolean);
  case Opcode::Lt:
  case Opcode::Gt:
  case Opcode::Le:
  case Opcode::Ge:
    if (!isInteger(inputs.at(0)) || inputs.at(0) != inputs.at(1)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "comparison operands are not equal integer types");
    }
    return one(boolean);

  case Opcode::Call:
  case Opcode::CallGeneric: {
    const auto use = functionUse(module, function, instruction, instruction.opcode == Opcode::CallGeneric);
    const auto parameters = substituteSignature(module.signatures.at(use.handle->parameters), use.arguments);
    requireInputs(module, instruction, inputs, parameters, true);
    return substituteSignature(module.signatures.at(use.handle->returns), use.arguments);
  }
  case Opcode::CallClosure: {
    const auto &signature = module.signatures.at(static_cast<std::size_t>(instruction.immediate_operands.at(0)));
    if (signature.size() != 1 || signature.front().kind != TypeKind::Function) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "closure signature is not one function type");
    }
    const auto &function_type = signature.front();
    Signature expected = function_type.arguments;
    expected.push_back(function_type);
    requireInputs(module, instruction, inputs, expected, true);
    return function_type.results;
  }
  case Opcode::PackClosure:
  case Opcode::PackClosureGeneric: {
    const bool generic = instruction.opcode == Opcode::PackClosureGeneric;
    const auto use = functionUse(module, function, instruction, generic);
    const auto parameters = substituteSignature(module.signatures.at(use.handle->parameters), use.arguments);
    const auto returns = substituteSignature(module.signatures.at(use.handle->returns), use.arguments);
    const auto mask = instruction.immediate_operands.at(1);
    Signature captured;
    Signature remaining;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (index < 64 && ((mask >> index) & 1U) != 0) {
        captured.push_back(parameters[index]);
      } else {
        remaining.push_back(parameters[index]);
      }
    }
    requireInputs(module, instruction, inputs, captured, true);
    AbilitySet abilities{static_cast<std::uint8_t>(AbilitySet::Copy | AbilitySet::Drop)};
    const bool persistent = std::any_of(use.handle->attributes.begin(), use.handle->attributes.end(),
                                        [](const FunctionAttribute &attribute) { return attribute.kind == FunctionAttributeKind::Persistent; });
    if (persistent) {
      abilities.bits = static_cast<std::uint8_t>(abilities.bits | AbilitySet::Store);
    }
    for (const auto &captured_type : inputs) {
      abilities.bits = static_cast<std::uint8_t>(abilities.bits & abilitiesOf(module, function, captured_type).bits);
    }
    Type result;
    result.kind = TypeKind::Function;
    result.abilities = abilities;
    result.arguments = std::move(remaining);
    result.results = returns;
    return one(std::move(result));
  }

  case Opcode::Pack:
  case Opcode::PackGeneric:
  case Opcode::Unpack:
  case Opcode::UnpackGeneric: {
    const bool generic = instruction.opcode == Opcode::PackGeneric || instruction.opcode == Opcode::UnpackGeneric;
    const auto use = structUse(module, function, instruction, generic);
    const auto fields = fieldsOf(use, std::nullopt);
    if (instruction.opcode == Opcode::Pack || instruction.opcode == Opcode::PackGeneric) {
      requireInputs(module, instruction, inputs, fields, true);
      return one(use.type);
    }
    requireInputs(module, instruction, inputs, {use.type});
    return fields;
  }
  case Opcode::PackVariant:
  case Opcode::PackVariantGeneric:
  case Opcode::UnpackVariant:
  case Opcode::UnpackVariantGeneric: {
    const bool generic = instruction.opcode == Opcode::PackVariantGeneric || instruction.opcode == Opcode::UnpackVariantGeneric;
    const auto use = variantUse(module, function, instruction, generic);
    const auto fields = fieldsOf(use.structure, use.variant);
    if (instruction.opcode == Opcode::PackVariant || instruction.opcode == Opcode::PackVariantGeneric) {
      requireInputs(module, instruction, inputs, fields, true);
      return one(use.structure.type);
    }
    requireInputs(module, instruction, inputs, {use.structure.type});
    return fields;
  }
  case Opcode::TestVariant:
  case Opcode::TestVariantGeneric: {
    const auto use = variantUse(module, function, instruction, instruction.opcode == Opcode::TestVariantGeneric);
    const auto inner = referenced(module, instruction, inputs.at(0), false);
    requireType(module, instruction, inner, use.structure.type, "variant reference");
    return one(boolean);
  }

  case Opcode::Exists:
  case Opcode::ExistsGeneric:
  case Opcode::MoveFrom:
  case Opcode::MoveFromGeneric:
  case Opcode::MutBorrowGlobal:
  case Opcode::MutBorrowGlobalGeneric:
  case Opcode::ImmBorrowGlobal:
  case Opcode::ImmBorrowGlobalGeneric: {
    const bool generic = instruction.opcode == Opcode::ExistsGeneric || instruction.opcode == Opcode::MoveFromGeneric ||
                         instruction.opcode == Opcode::MutBorrowGlobalGeneric || instruction.opcode == Opcode::ImmBorrowGlobalGeneric;
    const auto use = structUse(module, function, instruction, generic);
    if (!abilitiesOf(module, function, use.type).has(AbilitySet::Key)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "global resource operation requires a type with key");
    }
    requireInputs(module, instruction, inputs, {address});
    if (instruction.opcode == Opcode::Exists || instruction.opcode == Opcode::ExistsGeneric) {
      return one(boolean);
    }
    if (instruction.opcode == Opcode::MoveFrom || instruction.opcode == Opcode::MoveFromGeneric) {
      return one(use.type);
    }
    const auto reference = instruction.opcode == Opcode::MutBorrowGlobal || instruction.opcode == Opcode::MutBorrowGlobalGeneric ? TypeKind::MutableReference
                                                                                                                                 : TypeKind::Reference;
    return one(compound(reference, use.type));
  }
  case Opcode::MoveTo:
  case Opcode::MoveToGeneric: {
    const auto use = structUse(module, function, instruction, instruction.opcode == Opcode::MoveToGeneric);
    if (!abilitiesOf(module, function, use.type).has(AbilitySet::Key)) {
      typeError(module, instruction.bytecode_index, instruction.opcode, "move_to requires a resource type with key");
    }
    requireInputs(module, instruction, inputs, {compound(TypeKind::Reference, simple(TypeKind::Signer)), use.type}, true);
    return noResult();
  }

  case Opcode::MutBorrowField:
  case Opcode::MutBorrowFieldGeneric:
  case Opcode::ImmBorrowField:
  case Opcode::ImmBorrowFieldGeneric: {
    const bool generic = instruction.opcode == Opcode::MutBorrowFieldGeneric || instruction.opcode == Opcode::ImmBorrowFieldGeneric;
    const auto index = static_cast<std::size_t>(instruction.immediate_operands.at(0));
    const FieldHandle *handle = nullptr;
    Signature arguments;
    if (generic) {
      const auto &instantiation = module.field_instantiations.at(index);
      handle = &module.field_handles.at(instantiation.handle);
      arguments = module.signatures.at(instantiation.type_parameters);
    } else {
      handle = &module.field_handles.at(index);
    }
    const auto &definition = module.struct_definitions.at(handle->owner);
    const auto owner = materializeStruct(definition.handle, arguments);
    const bool mutable_borrow = instruction.opcode == Opcode::MutBorrowField || instruction.opcode == Opcode::MutBorrowFieldGeneric;
    const auto inner = referenced(module, instruction, inputs.at(0), mutable_borrow);
    requireType(module, instruction, inner, owner, "field owner");
    const auto field = substituteType(definition.fields.at(handle->field).type, arguments);
    return one(compound(mutable_borrow ? TypeKind::MutableReference : TypeKind::Reference, field));
  }

  case Opcode::MutBorrowVariantField:
  case Opcode::MutBorrowVariantFieldGeneric:
  case Opcode::ImmBorrowVariantField:
  case Opcode::ImmBorrowVariantFieldGeneric: {
    const bool generic = instruction.opcode == Opcode::MutBorrowVariantFieldGeneric || instruction.opcode == Opcode::ImmBorrowVariantFieldGeneric;
    const auto index = static_cast<std::size_t>(instruction.immediate_operands.at(0));
    const VariantFieldHandle *handle = nullptr;
    Signature arguments;
    if (generic) {
      const auto &instantiation = module.variant_field_instantiations.at(index);
      handle = &module.variant_field_handles.at(instantiation.handle);
      arguments = module.signatures.at(instantiation.type_parameters);
    } else {
      handle = &module.variant_field_handles.at(index);
    }
    const auto &definition = module.struct_definitions.at(handle->owner);
    const auto owner = materializeStruct(definition.handle, arguments);
    const bool mutable_borrow = instruction.opcode == Opcode::MutBorrowVariantField || instruction.opcode == Opcode::MutBorrowVariantFieldGeneric;
    const auto inner = referenced(module, instruction, inputs.at(0), mutable_borrow);
    requireType(module, instruction, inner, owner, "variant owner");
    const auto first_variant = static_cast<std::size_t>(handle->variants.at(0));
    const auto field = substituteType(definition.variants.at(first_variant).fields.at(handle->field).type, arguments);
    return one(compound(mutable_borrow ? TypeKind::MutableReference : TypeKind::Reference, field));
  }

  case Opcode::VecPack: {
    const auto element = onlyElement(module, instruction);
    for (const auto &input : inputs) {
      requireType(module, instruction, input, element, "vector element", true);
    }
    return one(compound(TypeKind::Vector, element));
  }
  case Opcode::VecUnpack: {
    const auto element = onlyElement(module, instruction);
    requireInputs(module, instruction, inputs, {compound(TypeKind::Vector, element)});
    return Signature(instruction.outputs.size(), element);
  }
  case Opcode::VecLen: {
    const auto element = onlyElement(module, instruction);
    const auto actual = vectorElementFromReference(module, instruction, inputs.at(0), false);
    requireType(module, instruction, actual, element, "vector element");
    return one(u64);
  }
  case Opcode::VecImmBorrow:
  case Opcode::VecMutBorrow: {
    const auto element = onlyElement(module, instruction);
    const bool mutable_borrow = instruction.opcode == Opcode::VecMutBorrow;
    const auto actual = vectorElementFromReference(module, instruction, inputs.at(0), mutable_borrow);
    requireType(module, instruction, actual, element, "vector element");
    requireType(module, instruction, inputs.at(1), u64, "vector index");
    return one(compound(mutable_borrow ? TypeKind::MutableReference : TypeKind::Reference, element));
  }
  case Opcode::VecPushBack: {
    const auto element = onlyElement(module, instruction);
    const auto actual = vectorElementFromReference(module, instruction, inputs.at(0), true);
    requireType(module, instruction, actual, element, "vector element");
    requireType(module, instruction, inputs.at(1), element, "pushed vector element", true);
    return noResult();
  }
  case Opcode::VecPopBack: {
    const auto element = onlyElement(module, instruction);
    const auto actual = vectorElementFromReference(module, instruction, inputs.at(0), true);
    requireType(module, instruction, actual, element, "vector element");
    return one(element);
  }
  case Opcode::VecSwap: {
    const auto element = onlyElement(module, instruction);
    const auto actual = vectorElementFromReference(module, instruction, inputs.at(0), true);
    requireType(module, instruction, actual, element, "vector element");
    requireType(module, instruction, inputs.at(1), u64, "first vector index");
    requireType(module, instruction, inputs.at(2), u64, "second vector index");
    return noResult();
  }
  }
  typeError(module, instruction.bytecode_index, instruction.opcode, "typed semantics are not implemented");
}

} // namespace

bool isTypeAssignable(const Type &expected, const Type &actual) noexcept {
  if (expected.kind == TypeKind::Function && actual.kind == TypeKind::Function) {
    const auto expected_is_subset = (expected.abilities.bits & actual.abilities.bits) == expected.abilities.bits;
    return expected.arguments == actual.arguments && expected.results == actual.results && expected_is_subset;
  }
  if (expected.kind == TypeKind::Reference && actual.kind == TypeKind::Reference && expected.arguments.size() == 1 && actual.arguments.size() == 1) {
    return isTypeAssignable(expected.arguments.front(), actual.arguments.front());
  }
  return expected == actual;
}

Type substituteType(const Type &type, const Signature &type_arguments) {
  if (type.kind == TypeKind::TypeParameter && !type_arguments.empty()) {
    if (type.index >= type_arguments.size()) {
      throw Error(ErrorCode::TypeMismatch, Error::UnknownOffset, "type substitution parameter is out of range");
    }
    return type_arguments[type.index];
  }
  Type result = type;
  for (auto &argument : result.arguments) {
    argument = substituteType(argument, type_arguments);
  }
  for (auto &return_type : result.results) {
    return_type = substituteType(return_type, type_arguments);
  }
  return result;
}

Signature substituteSignature(const Signature &signature, const Signature &type_arguments) {
  Signature result;
  result.reserve(signature.size());
  for (const auto &type : signature) {
    result.push_back(substituteType(type, type_arguments));
  }
  return result;
}

Signature functionLocalTypes(const Module &module, const FunctionDefinition &function) {
  const auto &handle = module.function_handles.at(function.handle);
  Signature result = module.signatures.at(handle.parameters);
  if (function.code.has_value()) {
    const auto &declared = module.signatures.at(function.code->locals);
    result.insert(result.end(), declared.begin(), declared.end());
  }
  return result;
}

void inferAndValidateStacklessTypes(const Module &module, const FunctionDefinition &function, StacklessFunction &stackless) {
  const auto locals = functionLocalTypes(module, function);
  stackless.value_types.assign(stackless.value_count, Type{});
  std::vector<bool> assigned(stackless.value_count, false);

  for (const auto &block : stackless.blocks) {
    for (const auto &instruction : block.instructions) {
      Signature inputs;
      inputs.reserve(instruction.inputs.size());
      for (const auto value : instruction.inputs) {
        if (value >= assigned.size() || !assigned[value]) {
          typeError(module, instruction.bytecode_index, instruction.opcode, "input value has no inferred type");
        }
        inputs.push_back(stackless.value_types[value]);
      }
      const auto outputs = inferOperation(module, function, locals, instruction, inputs);
      if (outputs.size() != instruction.outputs.size()) {
        typeError(module, instruction.bytecode_index, instruction.opcode, "typed output arity disagrees with stack effect");
      }
      for (std::size_t index = 0; index < outputs.size(); ++index) {
        const auto value = instruction.outputs[index];
        if (value >= assigned.size() || assigned[value]) {
          typeError(module, instruction.bytecode_index, instruction.opcode, "output value identity is invalid or duplicated");
        }
        stackless.value_types[value] = outputs[index];
        assigned[value] = true;
      }
    }
  }
}

} // namespace movescape
