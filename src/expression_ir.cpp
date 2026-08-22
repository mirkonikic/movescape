#include "movescape/expression_ir.hpp"

#include "movescape/binary_reader.hpp"
#include "movescape/disassembler.hpp"
#include "movescape/error.hpp"
#include "movescape/opcode.hpp"
#include "movescape/type_analysis.hpp"

#include <algorithm>
#include <bit>
#include <iomanip>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace movescape {

namespace {

[[noreturn]] void expressionError(std::size_t instruction, std::string message) {
  throw Error(ErrorCode::Malformed, Error::UnknownOffset, "expression recovery at instruction " + std::to_string(instruction) + ": " + std::move(message));
}

[[nodiscard]] ExpressionEffect effectOf(Opcode opcode) {
  switch (opcode) {
  case Opcode::LdU8:
  case Opcode::LdU16:
  case Opcode::LdU32:
  case Opcode::LdU64:
  case Opcode::LdU128:
  case Opcode::LdU256:
  case Opcode::LdI8:
  case Opcode::LdI16:
  case Opcode::LdI32:
  case Opcode::LdI64:
  case Opcode::LdI128:
  case Opcode::LdI256:
  case Opcode::LdTrue:
  case Opcode::LdFalse:
  case Opcode::CopyLoc:
  case Opcode::MoveLoc:
  case Opcode::ImmBorrowLoc:
  case Opcode::MutBorrowLoc:
  case Opcode::FreezeRef:
  case Opcode::Not:
  case Opcode::Negate:
  case Opcode::BitOr:
  case Opcode::BitAnd:
  case Opcode::Xor:
  case Opcode::Or:
  case Opcode::And:
  case Opcode::Eq:
  case Opcode::Neq:
  case Opcode::Lt:
  case Opcode::Gt:
  case Opcode::Le:
  case Opcode::Ge:
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
  case Opcode::CastI256:
    return ExpressionEffect::Pure;
  case Opcode::Add:
  case Opcode::Sub:
  case Opcode::Mul:
  case Opcode::Mod:
  case Opcode::Div:
  case Opcode::Shl:
  case Opcode::Shr:
  case Opcode::LdConst:
  case Opcode::ReadRef:
  case Opcode::Call:
  case Opcode::CallGeneric:
  case Opcode::CallClosure:
  case Opcode::Pack:
  case Opcode::PackGeneric:
  case Opcode::PackVariant:
  case Opcode::PackVariantGeneric:
  case Opcode::Unpack:
  case Opcode::UnpackGeneric:
  case Opcode::UnpackVariant:
  case Opcode::UnpackVariantGeneric:
  case Opcode::TestVariant:
  case Opcode::TestVariantGeneric:
  case Opcode::PackClosure:
  case Opcode::PackClosureGeneric:
  case Opcode::VecPack:
  case Opcode::VecLen:
  case Opcode::VecImmBorrow:
  case Opcode::VecMutBorrow:
  case Opcode::VecPopBack:
  case Opcode::VecUnpack:
  case Opcode::MutBorrowField:
  case Opcode::MutBorrowFieldGeneric:
  case Opcode::ImmBorrowField:
  case Opcode::ImmBorrowFieldGeneric:
  case Opcode::MutBorrowVariantField:
  case Opcode::MutBorrowVariantFieldGeneric:
  case Opcode::ImmBorrowVariantField:
  case Opcode::ImmBorrowVariantFieldGeneric:
  case Opcode::Exists:
  case Opcode::ExistsGeneric:
  case Opcode::MutBorrowGlobal:
  case Opcode::MutBorrowGlobalGeneric:
  case Opcode::ImmBorrowGlobal:
  case Opcode::ImmBorrowGlobalGeneric:
  case Opcode::MoveFrom:
  case Opcode::MoveFromGeneric:
    return ExpressionEffect::MayAbort;
  default:
    return ExpressionEffect::SideEffect;
  }
}

[[nodiscard]] std::string localName(LocalIndex local) { return "local" + std::to_string(static_cast<unsigned>(local)); }

[[nodiscard]] std::string localName(const FunctionDefinition &function, LocalIndex local) {
  if (static_cast<std::size_t>(local) < function.source_local_names.size()) {
    return function.source_local_names[local];
  }
  return localName(local);
}

[[nodiscard]] std::string localName(const Expression &expression) {
  if (expression.local_name.has_value()) {
    return *expression.local_name;
  }
  return localName(static_cast<LocalIndex>(expression.immediate_operands.at(0)));
}

[[nodiscard]] std::string valueName(ValueId value) { return "tmp" + std::to_string(value); }

[[nodiscard]] bool inferredScalarTransfer(const Type &type) noexcept {
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
    return true;
  case TypeKind::Signer:
  case TypeKind::Vector:
  case TypeKind::Reference:
  case TypeKind::MutableReference:
  case TypeKind::Struct:
  case TypeKind::StructInstantiation:
  case TypeKind::TypeParameter:
  case TypeKind::Function:
    return false;
  }
  return false;
}

[[nodiscard]] std::string join(const std::vector<std::string> &values, std::string_view separator) {
  std::ostringstream out;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      out << separator;
    }
    out << values[index];
  }
  return out.str();
}

[[nodiscard]] std::string typeArguments(const Module &module, const Signature &arguments);

[[nodiscard]] std::string integerLiteral(const Expression &expression, std::string_view suffix, bool signed_value = false) {
  if (!expression.wide_immediate.empty()) {
    auto bytes = expression.wide_immediate;
    const bool negative = signed_value && (bytes.back() & 0x80U) != 0;
    if (negative) {
      std::uint16_t carry = 1;
      for (auto &byte : bytes) {
        const auto magnitude = static_cast<std::uint16_t>(static_cast<std::uint8_t>(~byte)) + carry;
        byte = static_cast<std::uint8_t>(static_cast<unsigned>(magnitude) & 0xffU);
        carry = static_cast<std::uint16_t>(magnitude >> 8U);
      }
    }
    std::ostringstream out;
    out << (negative ? "-0x" : "0x");
    bool started = false;
    for (auto iterator = bytes.rbegin(); iterator != bytes.rend(); ++iterator) {
      if (!started && *iterator == 0 && iterator + 1 != bytes.rend()) {
        continue;
      }
      out << std::hex;
      if (!started) {
        out << static_cast<unsigned>(*iterator);
        started = true;
      } else {
        out << std::setw(2) << std::setfill('0') << static_cast<unsigned>(*iterator);
      }
    }
    return out.str() + std::string(suffix);
  }
  const auto bits = expression.immediate_operands.at(0);
  const auto value = signed_value ? std::to_string(std::bit_cast<std::int64_t>(bits)) : std::to_string(bits);
  return value + std::string(suffix);
}

[[nodiscard]] std::optional<std::string> binarySymbol(Opcode opcode) {
  switch (opcode) {
  case Opcode::Add:
    return "+";
  case Opcode::Sub:
    return "-";
  case Opcode::Mul:
    return "*";
  case Opcode::Mod:
    return "%";
  case Opcode::Div:
    return "/";
  case Opcode::BitOr:
    return "|";
  case Opcode::BitAnd:
    return "&";
  case Opcode::Xor:
    return "^";
  case Opcode::Or:
    return "||";
  case Opcode::And:
    return "&&";
  case Opcode::Eq:
    return "==";
  case Opcode::Neq:
    return "!=";
  case Opcode::Lt:
    return "<";
  case Opcode::Gt:
    return ">";
  case Opcode::Le:
    return "<=";
  case Opcode::Ge:
    return ">=";
  case Opcode::Shl:
    return "<<";
  case Opcode::Shr:
    return ">>";
  default:
    return std::nullopt;
  }
}

[[nodiscard]] int binaryPrecedence(Opcode opcode) {
  switch (opcode) {
  case Opcode::Or:
    return 1;
  case Opcode::And:
    return 2;
  case Opcode::Eq:
  case Opcode::Neq:
  case Opcode::Lt:
  case Opcode::Gt:
  case Opcode::Le:
  case Opcode::Ge:
    return 3;
  case Opcode::BitOr:
    return 4;
  case Opcode::Xor:
    return 5;
  case Opcode::BitAnd:
    return 6;
  case Opcode::Shl:
  case Opcode::Shr:
    return 7;
  case Opcode::Add:
  case Opcode::Sub:
    return 8;
  case Opcode::Mul:
  case Opcode::Mod:
  case Opcode::Div:
    return 9;
  default:
    return 0;
  }
}

[[nodiscard]] bool hasObservableEvaluation(const ExpressionPtr &expression) {
  if (!expression) {
    return false;
  }
  if (expression->effect != ExpressionEffect::Pure) {
    return true;
  }
  return std::any_of(expression->operands.begin(), expression->operands.end(), hasObservableEvaluation);
}

[[nodiscard]] bool rendersAsInfixBinary(const ExpressionPtr &expression) {
  if (!expression || !binarySymbol(expression->opcode).has_value()) {
    return false;
  }
  if (expression->opcode != Opcode::And && expression->opcode != Opcode::Or) {
    return true;
  }
  return expression->short_circuit || !std::any_of(expression->operands.begin(), expression->operands.end(), hasObservableEvaluation);
}

[[nodiscard]] std::string renderEagerBoolean(const Module &module, const ExpressionPtr &expression) {
  const auto suffix = std::to_string(expression->bytecode_index);
  const auto left_name = "movescape_bool_lhs" + suffix;
  const auto right_name = "movescape_bool_rhs" + suffix;
  return "({ let " + left_name + " = " + renderExpression(module, expression->operands.at(0)) + "; let " + right_name + " = " +
         renderExpression(module, expression->operands.at(1)) + "; " + left_name + " " + *binarySymbol(expression->opcode) + " " + right_name + " })";
}

[[nodiscard]] std::string renderBinaryExpression(const Module &module, const ExpressionPtr &expression, int parent_precedence = 0, bool right_child = false) {
  const auto precedence = binaryPrecedence(expression->opcode);
  const auto render_child = [&](const ExpressionPtr &child, bool right) {
    if (rendersAsInfixBinary(child)) {
      return renderBinaryExpression(module, child, precedence, right);
    }
    return renderExpression(module, child);
  };
  auto result =
      render_child(expression->operands.at(0), false) + " " + *binarySymbol(expression->opcode) + " " + render_child(expression->operands.at(1), true);
  if (precedence < parent_precedence || (right_child && precedence == parent_precedence)) {
    result = "(" + result + ")";
  }
  return result;
}

[[nodiscard]] bool isCast(Opcode opcode) {
  switch (opcode) {
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
  case Opcode::CastI256:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] std::string functionForInstruction(const Module &module, const Expression &expression) {
  const auto index = static_cast<std::size_t>(expression.immediate_operands.at(0));
  if (expression.opcode == Opcode::CallGeneric || expression.opcode == Opcode::PackClosureGeneric) {
    const auto &instantiation = module.function_instantiations.at(index);
    return renderSourceFunctionName(module, instantiation.handle) + typeArguments(module, module.signatures.at(instantiation.type_parameters));
  }
  return renderSourceFunctionName(module, static_cast<TableIndex>(index));
}

[[nodiscard]] std::string renderArguments(const Module &module, const std::vector<ExpressionPtr> &operands) {
  std::vector<std::string> values;
  values.reserve(operands.size());
  for (const auto &operand : operands) {
    values.push_back(renderExpression(module, operand));
  }
  return join(values, ", ");
}

[[noreturn]] std::string genericOperation(const Module &, const Expression &expression) {
  throw Error(ErrorCode::UnsupportedFeature, Error::UnknownOffset,
              "no exact Move source rendering for recovered opcode " + std::string(opcodeInfo(expression.opcode).name));
}

struct StructExpressionUse {
  const StructDefinition *definition = nullptr;
  Type type;
};

[[nodiscard]] StructExpressionUse structExpressionUse(const Module &module, const Expression &expression, bool generic) {
  const auto index = static_cast<std::size_t>(expression.immediate_operands.at(0));
  const StructDefinition *definition = nullptr;
  Signature arguments;
  if (generic) {
    const auto &instantiation = module.struct_definition_instantiations.at(index);
    definition = &module.struct_definitions.at(instantiation.definition);
    arguments = module.signatures.at(instantiation.type_parameters);
  } else {
    definition = &module.struct_definitions.at(index);
  }
  Type type;
  type.kind = arguments.empty() ? TypeKind::Struct : TypeKind::StructInstantiation;
  type.index = definition->handle;
  type.arguments = std::move(arguments);
  return {.definition = definition, .type = std::move(type)};
}

struct FieldExpressionUse {
  const FieldDefinition *field = nullptr;
};

[[nodiscard]] FieldExpressionUse fieldExpressionUse(const Module &module, const Expression &expression, bool generic) {
  const auto index = static_cast<std::size_t>(expression.immediate_operands.at(0));
  const FieldHandle *handle = nullptr;
  if (generic) {
    handle = &module.field_handles.at(module.field_instantiations.at(index).handle);
  } else {
    handle = &module.field_handles.at(index);
  }
  const auto &definition = module.struct_definitions.at(handle->owner);
  return {.field = &definition.fields.at(handle->field)};
}

struct VariantExpressionUse {
  const StructDefinition *definition = nullptr;
  const VariantDefinition *variant = nullptr;
  Signature arguments;
};

[[nodiscard]] VariantExpressionUse variantExpressionUse(const Module &module, const Expression &expression, bool generic) {
  const auto index = static_cast<std::size_t>(expression.immediate_operands.at(0));
  const StructVariantHandle *handle = nullptr;
  Signature arguments;
  if (generic) {
    const auto &instantiation = module.struct_variant_instantiations.at(index);
    handle = &module.struct_variant_handles.at(instantiation.handle);
    arguments = module.signatures.at(instantiation.type_parameters);
  } else {
    handle = &module.struct_variant_handles.at(index);
  }
  const auto &definition = module.struct_definitions.at(handle->definition);
  return {
      .definition = &definition,
      .variant = &definition.variants.at(handle->variant),
      .arguments = std::move(arguments),
  };
}

[[nodiscard]] std::string variantName(const Module &module, const VariantExpressionUse &use) {
  return renderStructName(module, use.definition->handle) + "::" + renderIdentifier(module, use.variant->name) + typeArguments(module, use.arguments);
}

[[nodiscard]] const FieldDefinition &variantFieldExpressionUse(const Module &module, const Expression &expression, bool generic) {
  const auto index = static_cast<std::size_t>(expression.immediate_operands.at(0));
  const VariantFieldHandle *handle = nullptr;
  if (generic) {
    const auto &instantiation = module.variant_field_instantiations.at(index);
    handle = &module.variant_field_handles.at(instantiation.handle);
  } else {
    handle = &module.variant_field_handles.at(index);
  }
  const auto &definition = module.struct_definitions.at(handle->owner);
  return definition.variants.at(handle->variants.at(0)).fields.at(handle->field);
}

[[nodiscard]] bool isFieldBorrow(Opcode opcode) {
  return opcode == Opcode::ImmBorrowField || opcode == Opcode::ImmBorrowFieldGeneric || opcode == Opcode::MutBorrowField ||
         opcode == Opcode::MutBorrowFieldGeneric || opcode == Opcode::ImmBorrowVariantField || opcode == Opcode::ImmBorrowVariantFieldGeneric ||
         opcode == Opcode::MutBorrowVariantField || opcode == Opcode::MutBorrowVariantFieldGeneric;
}

void flattenFieldBorrow(const Module &module, const ExpressionPtr &expression, ExpressionPtr &base, std::vector<std::string> &fields) {
  if (expression && isFieldBorrow(expression->opcode)) {
    flattenFieldBorrow(module, expression->operands.at(0), base, fields);
    const bool variant = expression->opcode == Opcode::ImmBorrowVariantField || expression->opcode == Opcode::ImmBorrowVariantFieldGeneric ||
                         expression->opcode == Opcode::MutBorrowVariantField || expression->opcode == Opcode::MutBorrowVariantFieldGeneric;
    const bool generic = expression->opcode == Opcode::ImmBorrowFieldGeneric || expression->opcode == Opcode::MutBorrowFieldGeneric ||
                         expression->opcode == Opcode::ImmBorrowVariantFieldGeneric || expression->opcode == Opcode::MutBorrowVariantFieldGeneric;
    const auto name = variant ? variantFieldExpressionUse(module, *expression, generic).name : fieldExpressionUse(module, *expression, generic).field->name;
    fields.push_back(renderIdentifier(module, name));
  } else {
    base = expression;
  }
}

[[nodiscard]] std::string renderFieldBase(const Module &module, const ExpressionPtr &base) {
  if (!base) {
    return "<null-field-base>";
  }
  switch (base->opcode) {
  case Opcode::CopyLoc:
  case Opcode::MoveLoc:
  case Opcode::ImmBorrowLoc:
  case Opcode::MutBorrowLoc:
    return localName(*base);
  default:
    auto rendered = renderExpression(module, base);
    if (rendersAsInfixBinary(base)) {
      rendered = "(" + rendered + ")";
    }
    return rendered;
  }
}

[[nodiscard]] std::string renderLocalLikeValue(const Module &module, const ExpressionPtr &expression) {
  if (!expression) {
    return "<null-value>";
  }
  switch (expression->opcode) {
  case Opcode::CopyLoc:
  case Opcode::MoveLoc:
    return localName(*expression);
  default:
    auto rendered = renderExpression(module, expression);
    if (rendersAsInfixBinary(expression)) {
      rendered = "(" + rendered + ")";
    }
    return rendered;
  }
}

[[nodiscard]] std::string renderFieldAccess(const Module &module, const ExpressionPtr &expression) {
  ExpressionPtr base;
  std::vector<std::string> fields;
  flattenFieldBorrow(module, expression, base, fields);
  auto result = renderFieldBase(module, base);
  if (!fields.empty()) {
    result += "." + join(fields, ".");
  }
  return result;
}

[[nodiscard]] std::string renderClosure(const Module &module, const Expression &expression) {
  const bool generic = expression.opcode == Opcode::PackClosureGeneric;
  const auto index = static_cast<std::size_t>(expression.immediate_operands.at(0));
  const FunctionHandle *handle = nullptr;
  if (generic) {
    handle = &module.function_handles.at(module.function_instantiations.at(index).handle);
  } else {
    handle = &module.function_handles.at(index);
  }
  const auto &parameters = module.signatures.at(handle->parameters);
  const auto mask = expression.immediate_operands.at(1);
  if (mask == 0U) {
    return functionForInstruction(module, expression);
  }
  std::vector<std::string> lambda_parameters;
  std::vector<std::string> call_arguments;
  std::size_t captured = 0;
  for (std::size_t parameter = 0; parameter < parameters.size(); ++parameter) {
    if (parameter < 64 && ((mask >> parameter) & 1U) != 0) {
      call_arguments.push_back(renderLocalLikeValue(module, expression.operands.at(captured++)));
    } else {
      auto name = "closure_arg" + std::to_string(parameter);
      lambda_parameters.push_back(name);
      call_arguments.push_back(std::move(name));
    }
  }
  return "(|" + join(lambda_parameters, ", ") + "| " + functionForInstruction(module, expression) + "(" + join(call_arguments, ", ") + ")" + ")";
}

[[nodiscard]] bool samePureExpression(const ExpressionPtr &left, const ExpressionPtr &right) {
  if (left == right) {
    return true;
  }
  const auto same_reference_local = [&]() {
    if (!left || !right || left->type != right->type || (left->type.kind != TypeKind::Reference && left->type.kind != TypeKind::MutableReference) ||
        left->immediate_operands.size() != 1 || right->immediate_operands.size() != 1 || left->immediate_operands != right->immediate_operands) {
      return false;
    }
    const auto local_opcode = [](Opcode opcode) { return opcode == Opcode::CopyLoc || opcode == Opcode::MoveLoc; };
    return local_opcode(left->opcode) && local_opcode(right->opcode);
  }();
  if (!left || !right || left->effect != ExpressionEffect::Pure || right->effect != ExpressionEffect::Pure ||
      (left->opcode != right->opcode && !same_reference_local) || left->type != right->type || left->operands.size() != right->operands.size() ||
      left->immediate_operands != right->immediate_operands || left->wide_immediate != right->wide_immediate || left->atom != right->atom ||
      left->local_name != right->local_name) {
    return false;
  }
  for (std::size_t index = 0; index < left->operands.size(); ++index) {
    if (!samePureExpression(left->operands[index], right->operands[index])) {
      return false;
    }
  }
  return true;
}

struct VariantTestList {
  ExpressionPtr tested;
  const StructDefinition *definition = nullptr;
  Signature arguments;
  std::vector<std::string> variants;
};

[[nodiscard]] bool collectVariantTests(const Module &module, const ExpressionPtr &expression, VariantTestList &result) {
  if (!expression) {
    return false;
  }
  if (expression->opcode == Opcode::Or && expression->short_circuit && expression->operands.size() == 2) {
    return collectVariantTests(module, expression->operands[0], result) && collectVariantTests(module, expression->operands[1], result);
  }
  if ((expression->opcode != Opcode::TestVariant && expression->opcode != Opcode::TestVariantGeneric) || expression->operands.size() != 1) {
    return false;
  }
  const auto generic = expression->opcode == Opcode::TestVariantGeneric;
  const auto use = variantExpressionUse(module, *expression, generic);
  if (!result.tested) {
    result.tested = expression->operands[0];
    result.definition = use.definition;
    result.arguments = use.arguments;
  } else if (result.definition != use.definition || result.arguments != use.arguments || !samePureExpression(result.tested, expression->operands[0])) {
    return false;
  }
  result.variants.push_back(renderIdentifier(module, use.variant->name));
  return true;
}

[[nodiscard]] std::optional<std::string> renderMultiVariantTest(const Module &module, const ExpressionPtr &expression) {
  VariantTestList tests;
  if (!collectVariantTests(module, expression, tests) || tests.variants.size() < 2) {
    return std::nullopt;
  }
  return "(" + renderExpression(module, tests.tested) + " is " + join(tests.variants, "|") + ")";
}

[[nodiscard]] std::string typeArguments(const Module &module, const Signature &arguments) {
  if (arguments.empty()) {
    return {};
  }
  std::vector<std::string> rendered;
  for (const auto &argument : arguments) {
    rendered.push_back(renderType(module, argument));
  }
  return "<" + join(rendered, ", ") + ">";
}

[[nodiscard]] std::string renderWideUnsigned(std::span<const std::uint8_t> bytes) {
  std::ostringstream out;
  out << "0x";
  bool started = false;
  for (auto iterator = bytes.rbegin(); iterator != bytes.rend(); ++iterator) {
    if (!started && *iterator == 0 && iterator + 1 != bytes.rend()) {
      continue;
    }
    out << std::hex;
    if (!started) {
      out << static_cast<unsigned>(*iterator);
      started = true;
    } else {
      out << std::setw(2) << std::setfill('0') << static_cast<unsigned>(*iterator);
    }
  }
  return started ? out.str() : "0x0";
}

[[nodiscard]] std::string renderWideSigned(std::span<const std::uint8_t> little_endian_bytes) {
  if ((little_endian_bytes.back() & 0x80U) == 0) {
    return renderWideUnsigned(little_endian_bytes);
  }
  std::vector<std::uint8_t> magnitude(little_endian_bytes.begin(), little_endian_bytes.end());
  std::uint16_t carry = 1;
  for (auto &byte : magnitude) {
    const auto value = static_cast<std::uint16_t>(static_cast<std::uint8_t>(~byte)) + carry;
    byte = static_cast<std::uint8_t>(static_cast<unsigned>(value) & 0xffU);
    carry = static_cast<std::uint16_t>(value >> 8U);
  }
  return "-" + renderWideUnsigned(magnitude);
}

[[nodiscard]] bool textLikeUtf8(std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto first = bytes[offset++];
    if (first <= 0x7fU) {
      if ((first < 0x20U && first != '\n' && first != '\r' && first != '\t') || first == 0x7fU) {
        return false;
      }
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xe0U) == 0xc0U) {
      continuation_count = 1;
      value = first & 0x1fU;
      minimum = 0x80U;
    } else if ((first & 0xf0U) == 0xe0U) {
      continuation_count = 2;
      value = first & 0x0fU;
      minimum = 0x800U;
    } else if ((first & 0xf8U) == 0xf0U) {
      continuation_count = 3;
      value = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > bytes.size() - offset) {
      return false;
    }
    for (std::size_t index = 0; index < continuation_count; ++index) {
      const auto byte = bytes[offset++];
      if ((byte & 0xc0U) != 0x80U) {
        return false;
      }
      value = (value << 6U) | (byte & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU) || (value >= 0x80U && value <= 0x9fU)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::string> renderTextByteString(std::span<const std::uint8_t> bytes) {
  if (!textLikeUtf8(bytes)) {
    return std::nullopt;
  }
  std::ostringstream literal;
  literal << "b\"";
  for (const auto byte : bytes) {
    switch (byte) {
    case '"':
      literal << "\\\"";
      break;
    case '\\':
      literal << "\\\\";
      break;
    case '\n':
      literal << "\\n";
      break;
    case '\r':
      literal << "\\r";
      break;
    case '\t':
      literal << "\\t";
      break;
    default:
      if (byte >= 0x20U && byte <= 0x7eU) {
        literal << static_cast<char>(byte);
      } else {
        literal << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
      }
      break;
    }
  }
  literal << '"';
  return literal.str();
}

[[nodiscard]] std::optional<std::string> decodeConstantValue(const Module &module, BinaryReader &reader, const Type &type, std::size_t depth = 0) {
  constexpr std::size_t maximum_depth = 256;
  constexpr std::uint64_t maximum_rendered_vector_elements = 65535;
  if (depth >= maximum_depth) {
    return std::nullopt;
  }
  switch (type.kind) {
  case TypeKind::Bool: {
    const auto value = reader.readU8("constant bool");
    if (value > 1) {
      return std::nullopt;
    }
    return value == 0 ? "false" : "true";
  }
  case TypeKind::U8:
    return std::to_string(reader.readU8("constant u8")) + "u8";
  case TypeKind::U16:
    return std::to_string(reader.readU16("constant u16")) + "u16";
  case TypeKind::U32:
    return std::to_string(reader.readU32("constant u32")) + "u32";
  case TypeKind::U64:
    return std::to_string(reader.readU64("constant u64")) + "u64";
  case TypeKind::U128: {
    const auto value = reader.readU128("constant u128");
    return renderWideUnsigned(value.little_endian_bytes) + "u128";
  }
  case TypeKind::U256: {
    const auto value = reader.readU256("constant u256");
    return renderWideUnsigned(value.little_endian_bytes) + "u256";
  }
  case TypeKind::I8:
    return std::to_string(static_cast<int>(reader.readI8("constant i8"))) + "i8";
  case TypeKind::I16:
    return std::to_string(reader.readI16("constant i16")) + "i16";
  case TypeKind::I32:
    return std::to_string(reader.readI32("constant i32")) + "i32";
  case TypeKind::I64:
    return std::to_string(reader.readI64("constant i64")) + "i64";
  case TypeKind::I128: {
    const auto value = reader.readI128("constant i128");
    return renderWideSigned(value.little_endian_bytes) + "i128";
  }
  case TypeKind::I256: {
    const auto value = reader.readI256("constant i256");
    return renderWideSigned(value.little_endian_bytes) + "i256";
  }
  case TypeKind::Address: {
    Address address{};
    const auto bytes = reader.readBytes(address.size(), "constant address");
    std::copy(bytes.begin(), bytes.end(), address.begin());
    return "@" + renderAddress(address);
  }
  case TypeKind::Vector: {
    const auto count = reader.readUleb128(std::numeric_limits<std::uint32_t>::max(), "constant vector length");
    if (count > maximum_rendered_vector_elements) {
      return std::nullopt;
    }
    if (type.arguments.size() == 1 && type.arguments.front().kind == TypeKind::U8) {
      const auto bytes = reader.readBytes(static_cast<std::size_t>(count), "constant byte string");
      if (const auto text = renderTextByteString(bytes); text.has_value()) {
        return text;
      }
      std::ostringstream literal;
      literal << "x\"";
      for (const auto byte : bytes) {
        literal << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
      }
      literal << '"';
      return literal.str();
    }
    std::vector<std::string> elements;
    elements.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
      auto element = decodeConstantValue(module, reader, type.arguments.at(0), depth + 1);
      if (!element.has_value()) {
        return std::nullopt;
      }
      elements.push_back(std::move(*element));
    }
    return "vector[" + join(elements, ", ") + "]";
  }
  case TypeKind::Struct:
  case TypeKind::StructInstantiation: {
    const auto definition = std::find_if(module.struct_definitions.begin(), module.struct_definitions.end(),
                                         [&](const StructDefinition &candidate) { return candidate.handle == type.index; });
    if (definition == module.struct_definitions.end() || definition->field_kind != StructFieldKind::Declared) {
      return std::nullopt;
    }
    std::vector<std::string> fields;
    for (const auto &field : definition->fields) {
      const auto field_type = substituteType(field.type, type.arguments);
      auto value = decodeConstantValue(module, reader, field_type, depth + 1);
      if (!value.has_value()) {
        return std::nullopt;
      }
      fields.push_back(renderIdentifier(module, field.name) + ": " + std::move(*value));
    }
    return renderType(module, type) + " { " + join(fields, ", ") + " }";
  }
  default:
    return std::nullopt;
  }
}

struct RenderedConstant {
  std::string source;
  bool exact = false;
};

[[nodiscard]] std::string rawConstantHex(const Constant &constant) {
  std::ostringstream out;
  out << "0x";
  for (const auto byte : constant.data) {
    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
  }
  return out.str();
}

[[noreturn]] RenderedConstant constantFallback(const Constant &constant, std::string_view reason) {
  throw Error(ErrorCode::UnsupportedFeature, Error::UnknownOffset, std::string(reason) + "; raw constant BCS " + rawConstantHex(constant));
}

[[nodiscard]] RenderedConstant renderConstant(const Module &module, std::size_t index) {
  const auto &constant = module.constants.at(index);
  BinaryReader reader(constant.data);
  std::optional<std::string> result;
  try {
    result = decodeConstantValue(module, reader, constant.type);
  } catch (const Error &) {
    return constantFallback(constant, "constant BCS is malformed");
  }
  if (!result.has_value()) {
    return constantFallback(constant, "constant type or value is unsupported");
  }
  if (!reader.empty()) {
    return constantFallback(constant, "constant has trailing bytes");
  }
  return {.source = std::move(*result), .exact = true};
}

} // namespace

std::string renderConstantValue(const Module &module, std::size_t index) { return renderConstant(module, index).source; }

ExpressionFunction recoverExpressions(const Module &module, const FunctionDefinition &function, const ControlFlowGraph &graph, const StacklessFunction &stackless) {
  ExpressionFunction result;
  result.locals = functionLocalTypes(module, function);
  const auto &handle = module.function_handles.at(function.handle);
  result.parameter_count = module.signatures.at(handle.parameters).size();

  // expression pointer for all the values in the stackless ir function
  std::vector<ExpressionPtr> values(stackless.value_count);

  if (stackless.blocks.size() != graph.blocks.size()) { expressionError(0, "stackless and CFG block counts disagree"); }

  // reserving size of the blocks in the stackless ir
  result.blocks.reserve(stackless.blocks.size());
  for (const auto &block : stackless.blocks) {
    ExpressionBlock recovered{.id = block.id, .statements = {}, .terminator = {}};
    std::vector<ValueId> pending_stack;

    // going over instructions per block
    for (const auto &instruction : block.instructions) {
      // in case the intsruction is unpacking a struct, enum, vector
      const bool materialize_outputs = instruction.opcode == Opcode::Unpack || instruction.opcode == Opcode::UnpackGeneric ||
                                       instruction.opcode == Opcode::UnpackVariant || instruction.opcode == Opcode::UnpackVariantGeneric ||
                                       instruction.opcode == Opcode::VecUnpack;
      // statement boundary, displays the end of a statement, unpacking into a variable, no outputs or many outputs
      const bool statement_boundary = instruction.outputs.empty() || materialize_outputs || instruction.outputs.size() > 1;

      if (instruction.inputs.size() > pending_stack.size()) {
        expressionError(instruction.bytecode_index, "expression stack underflow");
      }

      const auto survivor_count = pending_stack.size() - instruction.inputs.size();
      for (std::size_t index = 0; index < instruction.inputs.size(); ++index) {
        if (pending_stack[survivor_count + index] != instruction.inputs[index]) {
          expressionError(instruction.bytecode_index, "stackless input order disagrees with expression stack");
        }
      }
      
      if (statement_boundary) {
        for (std::size_t index = 0; index < survivor_count; ++index) {
          const auto value = pending_stack[index];
          if (!values[value]->atom.has_value()) {
            recovered.statements.push_back({
                .kind = ExpressionStatementKind::BindTemporary,
                .bytecode_index = instruction.bytecode_index,
                .local = std::nullopt,
                .local_name = std::nullopt,
                .generated_values = {value},
                .expression = values[value],
            });
            values[value] = std::make_shared<Expression>(Expression{
                .opcode = Opcode::Nop,
                .type = stackless.value_types.at(value),
                .operands = {},
                .immediate_operands = {},
                .wide_immediate = {},
                .bytecode_index = instruction.bytecode_index,
                .value = value,
                .result_index = 0,
                .effect = ExpressionEffect::Pure,
                .short_circuit = false,
                .conditional = false,
                .atom = valueName(value),
                .local_name = std::nullopt,
            });
          }
        }
      }

      std::vector<ExpressionPtr> operands;
      operands.reserve(instruction.inputs.size());
      for (const auto value : instruction.inputs) {
        if (value >= values.size() || !values[value]) {
          expressionError(instruction.bytecode_index, "input expression is unavailable");
        }
        operands.push_back(values[value]);
      }
      pending_stack.resize(survivor_count);

      auto expression = std::make_shared<Expression>(Expression{
          .opcode = instruction.opcode,
          .type = instruction.outputs.empty() ? Type{} : stackless.value_types.at(instruction.outputs.front()),
          .operands = operands,
          .immediate_operands = instruction.immediate_operands,
          .wide_immediate = instruction.wide_immediate,
          .bytecode_index = instruction.bytecode_index,
          .value = instruction.outputs.empty() ? std::nullopt : std::optional<ValueId>{instruction.outputs.front()},
          .result_index = 0,
          .effect = effectOf(instruction.opcode),
          .short_circuit = false,
          .conditional = false,
          .atom = std::nullopt,
          .local_name = std::nullopt,
      });
      switch (instruction.opcode) {
      case Opcode::CopyLoc:
      case Opcode::MoveLoc:
      case Opcode::ImmBorrowLoc:
      case Opcode::MutBorrowLoc:
        expression->local_name = localName(function, static_cast<LocalIndex>(instruction.immediate_operands.at(0)));
        break;
      default:
        break;
      }

      if (instruction.outputs.size() == 1 && !materialize_outputs) {
        values[instruction.outputs.front()] = expression;
        pending_stack.push_back(instruction.outputs.front());
        continue;
      }
      if (!instruction.outputs.empty()) {
        ExpressionStatement statement{
            .kind = ExpressionStatementKind::Destructure,
            .bytecode_index = instruction.bytecode_index,
            .local = std::nullopt,
            .local_name = std::nullopt,
            .generated_values = instruction.outputs,
            .expression = expression,
        };
        recovered.statements.push_back(std::move(statement));
        for (std::size_t index = 0; index < instruction.outputs.size(); ++index) {
          const auto value = instruction.outputs[index];
          values[value] = std::make_shared<Expression>(Expression{
              .opcode = Opcode::Nop,
              .type = stackless.value_types.at(value),
              .operands = {},
              .immediate_operands = {},
              .wide_immediate = {},
              .bytecode_index = instruction.bytecode_index,
              .value = value,
              .result_index = index,
              .effect = ExpressionEffect::Pure,
              .short_circuit = false,
              .conditional = false,
              .atom = valueName(value),
              .local_name = std::nullopt,
          });
          pending_stack.push_back(value);
        }
        continue;
      }

      switch (instruction.opcode) {
      case Opcode::Nop:
        break;
      case Opcode::StLoc:
        recovered.statements.push_back({
            .kind = ExpressionStatementKind::AssignLocal,
            .bytecode_index = instruction.bytecode_index,
            .local = static_cast<LocalIndex>(instruction.immediate_operands.at(0)),
            .local_name = localName(function, static_cast<LocalIndex>(instruction.immediate_operands.at(0))),
            .generated_values = {},
            .expression = operands.at(0),
        });
        break;
      case Opcode::Pop:
        recovered.statements.push_back({
            .kind = ExpressionStatementKind::Discard,
            .bytecode_index = instruction.bytecode_index,
            .local = std::nullopt,
            .local_name = std::nullopt,
            .generated_values = {},
            .expression = operands.at(0),
        });
        break;
      case Opcode::Ret:
        recovered.terminator = {
            .kind = ExpressionTerminatorKind::Return,
            .bytecode_index = instruction.bytecode_index,
            .values = operands,
            .true_target = std::nullopt,
            .false_target = std::nullopt,
        };
        break;
      case Opcode::Abort:
      case Opcode::AbortMsg:
        recovered.terminator = {
            .kind = ExpressionTerminatorKind::Abort,
            .bytecode_index = instruction.bytecode_index,
            .values = operands,
            .true_target = std::nullopt,
            .false_target = std::nullopt,
        };
        break;
      case Opcode::Branch: {
        const auto target = graph.instruction_to_block.at(static_cast<std::size_t>(instruction.immediate_operands.at(0)));
        recovered.terminator = {
            .kind = ExpressionTerminatorKind::Goto,
            .bytecode_index = instruction.bytecode_index,
            .values = {},
            .true_target = target,
            .false_target = std::nullopt,
        };
        break;
      }
      case Opcode::BrTrue:
      case Opcode::BrFalse: {
        const auto target = graph.instruction_to_block.at(static_cast<std::size_t>(instruction.immediate_operands.at(0)));
        const auto &cfg_block = graph.blocks.at(block.id);
        if (cfg_block.successors.size() != 2) {
          expressionError(instruction.bytecode_index, "conditional block does not have two CFG edges");
        }
        std::optional<BlockId> true_target;
        std::optional<BlockId> false_target;
        for (const auto &edge : cfg_block.successors) {
          if (edge.kind == EdgeKind::True) {
            true_target = edge.target;
          } else if (edge.kind == EdgeKind::False) {
            false_target = edge.target;
          }
        }
        (void)target;
        recovered.terminator = {
            .kind = ExpressionTerminatorKind::Conditional,
            .bytecode_index = instruction.bytecode_index,
            .values = operands,
            .true_target = true_target,
            .false_target = false_target,
        };
        break;
      }
      default:
        recovered.statements.push_back({
            .kind = ExpressionStatementKind::Effect,
            .bytecode_index = instruction.bytecode_index,
            .local = std::nullopt,
            .local_name = std::nullopt,
            .generated_values = {},
            .expression = expression,
        });
        break;
      }
    }
    if (!pending_stack.empty()) {
      expressionError(block.instructions.empty() ? 0 : block.instructions.back().bytecode_index, "values remain on the expression stack at block end");
    }
    if (recovered.terminator.kind == ExpressionTerminatorKind::None) {
      const auto &cfg_block = graph.blocks.at(block.id);
      if (cfg_block.successors.size() == 1) {
        recovered.terminator = {
            .kind = ExpressionTerminatorKind::Goto,
            .bytecode_index = cfg_block.end - 1,
            .values = {},
            .true_target = cfg_block.successors.front().target,
            .false_target = std::nullopt,
        };
      }
    }
    result.blocks.push_back(std::move(recovered));
  }
  return result;
}

std::string renderExpression(const Module &module, const ExpressionPtr &expression) {
  if (!expression) {
    return "<null-expression>";
  }
  if (expression->atom.has_value()) {
    return *expression->atom;
  }
  if (expression->conditional) {
    return "if (" + renderExpression(module, expression->operands.at(0)) + ") { " + renderExpression(module, expression->operands.at(1)) + " } else { " +
           renderExpression(module, expression->operands.at(2)) + " }";
  }
  if (const auto variants = renderMultiVariantTest(module, expression); variants.has_value()) {
    return *variants;
  }
  const auto args = [&]() { return renderArguments(module, expression->operands); };
  if (rendersAsInfixBinary(expression)) {
    return renderBinaryExpression(module, expression);
  }
  if (expression->opcode == Opcode::And || expression->opcode == Opcode::Or) {
    return renderEagerBoolean(module, expression);
  }
  switch (expression->opcode) {
  case Opcode::LdU8:
    return integerLiteral(*expression, "u8");
  case Opcode::LdU16:
    return integerLiteral(*expression, "u16");
  case Opcode::LdU32:
    return integerLiteral(*expression, "u32");
  case Opcode::LdU64:
    return integerLiteral(*expression, "u64");
  case Opcode::LdU128:
    return integerLiteral(*expression, "u128");
  case Opcode::LdU256:
    return integerLiteral(*expression, "u256");
  case Opcode::LdI8:
    return integerLiteral(*expression, "i8", true);
  case Opcode::LdI16:
    return integerLiteral(*expression, "i16", true);
  case Opcode::LdI32:
    return integerLiteral(*expression, "i32", true);
  case Opcode::LdI64:
    return integerLiteral(*expression, "i64", true);
  case Opcode::LdI128:
    return integerLiteral(*expression, "i128", true);
  case Opcode::LdI256:
    return integerLiteral(*expression, "i256", true);
  case Opcode::LdTrue:
    return "true";
  case Opcode::LdFalse:
    return "false";
  case Opcode::LdConst:
    if (const auto index = static_cast<std::size_t>(expression->immediate_operands.at(0));
        index < module.source_constant_names.size() && module.source_constant_names[index].has_value()) {
      return *module.source_constant_names[index];
    } else {
      return renderConstant(module, index).source;
    }
  case Opcode::CopyLoc:
    return (inferredScalarTransfer(expression->type) ? "" : "copy ") + localName(*expression);
  case Opcode::MoveLoc:
    return (inferredScalarTransfer(expression->type) ? "" : "move ") + localName(*expression);
  case Opcode::ImmBorrowLoc:
    return "&" + localName(*expression);
  case Opcode::MutBorrowLoc:
    return "&mut " + localName(*expression);
  case Opcode::Not:
  case Opcode::Negate: {
    const auto &operand = expression->operands.at(0);
    auto rendered = renderExpression(module, operand);
    if (binarySymbol(operand->opcode).has_value() || (expression->opcode == Opcode::Negate && operand->opcode == Opcode::Negate)) {
      rendered = "(" + rendered + ")";
    }
    return std::string(expression->opcode == Opcode::Not ? "!" : "-") + rendered;
  }
  case Opcode::ReadRef:
    if (isFieldBorrow(expression->operands.at(0)->opcode)) {
      return renderFieldAccess(module, expression->operands.at(0));
    }
    return "(*" + renderLocalLikeValue(module, expression->operands.at(0)) + ")";
  case Opcode::FreezeRef:
    return renderExpression(module, expression->operands.at(0));
  case Opcode::Call:
  case Opcode::CallGeneric:
    return functionForInstruction(module, *expression) + "(" + args() + ")";
  case Opcode::CallClosure: {
    if (expression->operands.empty()) {
      return "call_closure()";
    }
    std::vector<ExpressionPtr> call_arguments(expression->operands.begin(), std::prev(expression->operands.end()));
    return renderLocalLikeValue(module, expression->operands.back()) + "(" + renderArguments(module, call_arguments) + ")";
  }
  case Opcode::PackClosure:
  case Opcode::PackClosureGeneric:
    return renderClosure(module, *expression);
  case Opcode::VecPack:
    return "vector[" + args() + "]";
  case Opcode::VecLen:
    return "0x1::vector::length(" + args() + ")";
  case Opcode::VecImmBorrow:
  case Opcode::VecMutBorrow:
    return "0x1::vector::borrow" + std::string(expression->opcode == Opcode::VecMutBorrow ? "_mut" : "") + "(" + args() + ")";
  case Opcode::VecPopBack:
    return "0x1::vector::pop_back(" + args() + ")";
  case Opcode::VecPushBack:
    return "0x1::vector::push_back(" + args() + ")";
  case Opcode::VecSwap:
    return "0x1::vector::swap(" + args() + ")";
  case Opcode::WriteRef:
    if (isFieldBorrow(expression->operands.at(1)->opcode)) {
      return renderFieldAccess(module, expression->operands.at(1)) + " = " + renderExpression(module, expression->operands.at(0));
    }
    return "*" + renderLocalLikeValue(module, expression->operands.at(1)) + " = " + renderExpression(module, expression->operands.at(0));
  case Opcode::Pack:
  case Opcode::PackGeneric: {
    const auto use = structExpressionUse(module, *expression, expression->opcode == Opcode::PackGeneric);
    std::vector<std::string> fields;
    for (std::size_t index = 0; index < use.definition->fields.size(); ++index) {
      fields.push_back(renderIdentifier(module, use.definition->fields[index].name) + ": " + renderExpression(module, expression->operands.at(index)));
    }
    return renderType(module, use.type) + " { " + join(fields, ", ") + " }";
  }
  case Opcode::PackVariant:
  case Opcode::PackVariantGeneric: {
    const auto use = variantExpressionUse(module, *expression, expression->opcode == Opcode::PackVariantGeneric);
    std::vector<std::string> fields;
    for (std::size_t field = 0; field < use.variant->fields.size(); ++field) {
      fields.push_back(renderIdentifier(module, use.variant->fields[field].name) + ": " + renderExpression(module, expression->operands.at(field)));
    }
    return variantName(module, use) + " { " + join(fields, ", ") + " }";
  }
  case Opcode::ImmBorrowField:
  case Opcode::ImmBorrowFieldGeneric:
  case Opcode::MutBorrowField:
  case Opcode::MutBorrowFieldGeneric:
  case Opcode::ImmBorrowVariantField:
  case Opcode::ImmBorrowVariantFieldGeneric:
  case Opcode::MutBorrowVariantField:
  case Opcode::MutBorrowVariantFieldGeneric: {
    const bool mutable_borrow = expression->opcode == Opcode::MutBorrowField || expression->opcode == Opcode::MutBorrowFieldGeneric ||
                                expression->opcode == Opcode::MutBorrowVariantField || expression->opcode == Opcode::MutBorrowVariantFieldGeneric;
    return std::string(mutable_borrow ? "&mut " : "&") + renderFieldAccess(module, expression);
  }
  case Opcode::TestVariant:
  case Opcode::TestVariantGeneric: {
    const auto use = variantExpressionUse(module, *expression, expression->opcode == Opcode::TestVariantGeneric);
    return "(" + renderExpression(module, expression->operands.at(0)) + " is " + renderIdentifier(module, use.variant->name) + ")";
  }
  case Opcode::Exists:
  case Opcode::ExistsGeneric: {
    const auto use = structExpressionUse(module, *expression, expression->opcode == Opcode::ExistsGeneric);
    return "exists<" + renderType(module, use.type) + ">(" + args() + ")";
  }
  case Opcode::MoveFrom:
  case Opcode::MoveFromGeneric: {
    const auto use = structExpressionUse(module, *expression, expression->opcode == Opcode::MoveFromGeneric);
    return "move_from<" + renderType(module, use.type) + ">(" + args() + ")";
  }
  case Opcode::ImmBorrowGlobal:
  case Opcode::ImmBorrowGlobalGeneric:
  case Opcode::MutBorrowGlobal:
  case Opcode::MutBorrowGlobalGeneric: {
    const bool generic = expression->opcode == Opcode::ImmBorrowGlobalGeneric || expression->opcode == Opcode::MutBorrowGlobalGeneric;
    const auto use = structExpressionUse(module, *expression, generic);
    const bool mutable_borrow = expression->opcode == Opcode::MutBorrowGlobal || expression->opcode == Opcode::MutBorrowGlobalGeneric;
    return std::string(mutable_borrow ? "borrow_global_mut<" : "borrow_global<") + renderType(module, use.type) + ">(" + args() + ")";
  }
  case Opcode::MoveTo:
  case Opcode::MoveToGeneric: {
    const auto use = structExpressionUse(module, *expression, expression->opcode == Opcode::MoveToGeneric);
    return "move_to<" + renderType(module, use.type) + ">(" + args() + ")";
  }
  default:
    if (isCast(expression->opcode)) {
      return "(" + args() + " as " + renderType(module, expression->type) + ")";
    }
    return genericOperation(module, *expression);
  }
}

bool expressionSourceSemanticsComplete(const Module &module, const ExpressionPtr &expression) {
  if (!expression) {
    return false;
  }
  if (expression->opcode == Opcode::LdConst && !renderConstant(module, static_cast<std::size_t>(expression->immediate_operands.at(0))).exact) {
    return false;
  }
  return std::all_of(expression->operands.begin(), expression->operands.end(),
                     [&](const ExpressionPtr &operand) { return expressionSourceSemanticsComplete(module, operand); });
}

std::string renderExpressionStatement(const Module &module, const ExpressionStatement &statement) {
  std::ostringstream out;
  switch (statement.kind) {
  case ExpressionStatementKind::BindTemporary:
    out << "let " << valueName(statement.generated_values.at(0)) << " = " << renderExpression(module, statement.expression) << ';';
    break;
  case ExpressionStatementKind::AssignLocal:
    out << statement.local_name.value_or(localName(*statement.local)) << " = " << renderExpression(module, statement.expression) << ';';
    break;
  case ExpressionStatementKind::Discard:
  case ExpressionStatementKind::Effect:
    out << renderExpression(module, statement.expression) << ';';
    break;
  case ExpressionStatementKind::Destructure: {
    if (statement.expression->opcode == Opcode::Unpack || statement.expression->opcode == Opcode::UnpackGeneric) {
      const auto use = structExpressionUse(module, *statement.expression, statement.expression->opcode == Opcode::UnpackGeneric);
      std::vector<std::string> fields;
      for (std::size_t index = 0; index < statement.generated_values.size(); ++index) {
        fields.push_back(renderIdentifier(module, use.definition->fields.at(index).name) + ": " + valueName(statement.generated_values[index]));
      }
      out << "let " << renderType(module, use.type) << " { " << join(fields, ", ") << " } = " << renderExpression(module, statement.expression->operands.at(0))
          << ';';
      break;
    }
    if (statement.expression->opcode == Opcode::UnpackVariant || statement.expression->opcode == Opcode::UnpackVariantGeneric) {
      const auto use = variantExpressionUse(module, *statement.expression, statement.expression->opcode == Opcode::UnpackVariantGeneric);
      std::vector<std::string> fields;
      for (std::size_t index = 0; index < statement.generated_values.size(); ++index) {
        fields.push_back(renderIdentifier(module, use.variant->fields.at(index).name) + ": " + valueName(statement.generated_values[index]));
      }
      out << "let " << variantName(module, use) << " { " << join(fields, ", ") << " } = " << renderExpression(module, statement.expression->operands.at(0))
          << ';';
      break;
    }
    if (statement.expression->opcode == Opcode::VecUnpack) {
      std::vector<std::string> names;
      for (const auto value : statement.generated_values) {
        names.push_back(valueName(value));
      }
      out << "let vector[" << join(names, ", ") << "] = " << renderExpression(module, statement.expression->operands.at(0)) << ';';
      break;
    }
    std::vector<std::string> names;
    for (const auto value : statement.generated_values) {
      names.push_back(valueName(value));
    }
    out << "let (" << join(names, ", ") << ") = " << renderExpression(module, statement.expression) << ';';
    break;
  }
  }
  return out.str();
}

bool isUnspecifiedAbortCode(const ExpressionPtr &expression) noexcept {
  constexpr std::uint64_t UnspecifiedAbortCode = 0xCA26CBD9BE0B0000ULL;
  return expression && expression->opcode == Opcode::LdU64 && expression->immediate_operands.size() == 1 &&
         expression->immediate_operands[0] == UnspecifiedAbortCode;
}

std::string formatExpressionFunction(const Module &module, const ExpressionFunction &function) {
  std::ostringstream out;
  out << "parameters: " << function.parameter_count << "\nlocals: " << function.locals.size() << '\n';
  for (const auto &block : function.blocks) {
    out << "\nbb" << block.id << ":\n";
    for (const auto &statement : block.statements) {
      out << "  " << renderExpressionStatement(module, statement) << "  // @" << statement.bytecode_index << '\n';
    }
    const auto &terminator = block.terminator;
    out << "  ";
    switch (terminator.kind) {
    case ExpressionTerminatorKind::None:
      out << "<no terminator>";
      break;
    case ExpressionTerminatorKind::Goto:
      out << "goto bb" << *terminator.true_target;
      break;
    case ExpressionTerminatorKind::Conditional:
      out << "if " << renderExpression(module, terminator.values.at(0)) << " goto bb" << *terminator.true_target << " else goto bb" << *terminator.false_target;
      break;
    case ExpressionTerminatorKind::Return: {
      std::vector<std::string> values;
      for (const auto &value : terminator.values) {
        values.push_back(renderExpression(module, value));
      }
      out << "return";
      if (!values.empty()) {
        out << ' ' << join(values, ", ");
      }
      break;
    }
    case ExpressionTerminatorKind::Abort:
      out << "abort ";
      if (terminator.values.size() == 1) {
        out << renderExpression(module, terminator.values.front());
      } else {
        out << '(' << renderArguments(module, terminator.values) << ')';
      }
      break;
    }
    out << ";  // @" << terminator.bytecode_index << '\n';
  }
  return out.str();
}

} // namespace movescape
