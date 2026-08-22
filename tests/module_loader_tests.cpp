#include "test.hpp"

#include "movescape/disassembler.hpp"
#include "movescape/format.hpp"
#include "movescape/module_loader.hpp"
#include "movescape/move_emitter.hpp"
#include "movescape/opcode.hpp"
#include "movescape/validator.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void appendUleb(Bytes &bytes, std::uint64_t value) {
  do {
    auto byte = static_cast<std::uint8_t>(value & 0x7fU);
    value >>= 7U;
    if (value != 0) {
      byte = static_cast<std::uint8_t>(byte | 0x80U);
    }
    bytes.push_back(byte);
  } while (value != 0);
}

void appendFixed(Bytes &bytes, std::uint64_t value, std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    value >>= 8U;
  }
}

void appendInstruction(Bytes &bytes, movescape::Opcode opcode) {
  bytes.push_back(static_cast<std::uint8_t>(opcode));
  using movescape::OperandEncoding;
  switch (movescape::opcodeInfo(opcode).operands) {
  case OperandEncoding::None:
    break;
  case OperandEncoding::LocalIndex:
  case OperandEncoding::TableIndex:
  case OperandEncoding::CodeOffset:
    appendUleb(bytes, 0);
    break;
  case OperandEncoding::U8:
  case OperandEncoding::I8:
    appendFixed(bytes, 0x7f, 1);
    break;
  case OperandEncoding::U16:
  case OperandEncoding::I16:
    appendFixed(bytes, 0x1234, 2);
    break;
  case OperandEncoding::U32:
  case OperandEncoding::I32:
    appendFixed(bytes, 0x12345678, 4);
    break;
  case OperandEncoding::U64:
  case OperandEncoding::I64:
    appendFixed(bytes, 0x123456789abcdef0ULL, 8);
    break;
  case OperandEncoding::U128:
  case OperandEncoding::I128:
    appendFixed(bytes, 0, 16);
    break;
  case OperandEncoding::U256:
  case OperandEncoding::I256:
    appendFixed(bytes, 0, 32);
    break;
  case OperandEncoding::SignatureAndU64:
    appendUleb(bytes, 0);
    appendFixed(bytes, 3, 8);
    break;
  case OperandEncoding::TableIndexAndClosureMask:
    appendUleb(bytes, 0);
    appendUleb(bytes, 3);
    break;
  }
}

Bytes moduleWithTables(std::uint32_t version, const std::vector<std::pair<movescape::format::TableKind, Bytes>> &tables, std::uint16_t self_handle = 0) {
  Bytes bytes{
      0xa1,
      0x1c,
      0xeb,
      0x0b,
      static_cast<std::uint8_t>(version & 0xffU),
      static_cast<std::uint8_t>((version >> 8U) & 0xffU),
      static_cast<std::uint8_t>((version >> 16U) & 0xffU),
      static_cast<std::uint8_t>((version >> 24U) & 0xffU),
  };
  appendUleb(bytes, tables.size());

  std::uint64_t offset = 0;
  for (const auto &[kind, content] : tables) {
    bytes.push_back(static_cast<std::uint8_t>(kind));
    appendUleb(bytes, offset);
    appendUleb(bytes, content.size());
    offset += content.size();
  }
  for (const auto &[kind, content] : tables) {
    (void)kind;
    bytes.insert(bytes.end(), content.begin(), content.end());
  }
  appendUleb(bytes, self_handle);
  return bytes;
}

Bytes scriptWithTables(std::uint32_t version, const std::vector<std::pair<movescape::format::TableKind, Bytes>> &tables, const Bytes &footer) {
  Bytes bytes{
      0xa1,
      0x1c,
      0xeb,
      0x0b,
      static_cast<std::uint8_t>(version & 0xffU),
      static_cast<std::uint8_t>((version >> 8U) & 0xffU),
      static_cast<std::uint8_t>((version >> 16U) & 0xffU),
      static_cast<std::uint8_t>((version >> 24U) & 0xffU),
  };
  appendUleb(bytes, tables.size());
  std::uint64_t offset = 0;
  for (const auto &[kind, content] : tables) {
    bytes.push_back(static_cast<std::uint8_t>(kind));
    appendUleb(bytes, offset);
    appendUleb(bytes, content.size());
    offset += content.size();
  }
  for (const auto &[kind, content] : tables) {
    (void)kind;
    bytes.insert(bytes.end(), content.begin(), content.end());
  }
  bytes.insert(bytes.end(), footer.begin(), footer.end());
  return bytes;
}

} // namespace

TEST(load_minimal_function_module) {
  using movescape::format::TableKind;

  Bytes identifiers{0x01, 'M', 0x01, 'f'};
  Bytes addresses(32, 0);
  addresses.back() = 0x42;
  Bytes module_handles{0x00, 0x00};
  Bytes signatures{0x00};
  Bytes function_handles{
      0x00, // module
      0x01, // name
      0x00, // parameters
      0x00, // returns
      0x00, // type parameters
  };
  Bytes function_definitions{
      0x00, // handle
      0x01, // public
      0x00, // flags
      0x00, // acquires
      0x00, // locals signature
      0x01, // instruction count
      0x02, // Ret
  };

  const auto bytes = moduleWithTables(5, {
                                             {TableKind::Identifiers, identifiers},
                                             {TableKind::AddressIdentifiers, addresses},
                                             {TableKind::ModuleHandles, module_handles},
                                             {TableKind::Signatures, signatures},
                                             {TableKind::FunctionHandles, function_handles},
                                             {TableKind::FunctionDefinitions, function_definitions},
                                         });
  const auto module = movescape::loadModule(bytes);

  REQUIRE_EQ(module.version, 5U);
  REQUIRE_EQ(module.identifiers.size(), 2U);
  REQUIRE_EQ(module.identifiers[0], std::string("M"));
  REQUIRE_EQ(module.identifiers[1], std::string("f"));
  REQUIRE_EQ(module.addresses.size(), 1U);
  REQUIRE_EQ(module.addresses[0].back(), 0x42U);
  REQUIRE_EQ(module.function_handles.size(), 1U);
  REQUIRE_EQ(module.function_definitions.size(), 1U);
  REQUIRE(module.function_definitions[0].code.has_value());
  REQUIRE_EQ(module.function_definitions[0].code->code.size(), 1U);
  REQUIRE_EQ(module.function_definitions[0].code->code[0].opcode, movescape::Opcode::Ret);
}

TEST(load_validate_disassemble_and_emit_version_ten_script) {
  using movescape::format::TableKind;
  const Bytes signatures{
      0x01, // parameter signature length
      0x03, // u64
      0x00, // empty locals signature
  };
  const Bytes footer{
      0x00, // type-parameter count
      0x00, // parameter signature index
      0x01, // no access specifiers
      0x01, // locals signature index
      0x01, // instruction count
      0x02, // Ret
  };
  const auto script = movescape::loadScript(scriptWithTables(10, {{TableKind::Signatures, signatures}}, footer));
  REQUIRE_EQ(script.common.version, 10U);
  REQUIRE_EQ(script.parameters, 0U);
  REQUIRE_EQ(script.code.locals, 1U);
  REQUIRE_EQ(script.code.code.size(), 1U);
  movescape::validateScript(script);
  REQUIRE(movescape::disassembleScript(script).find("script bytecode-v10") != std::string::npos);
  const auto emission = movescape::emitMoveScript(script);
  REQUIRE(emission.allControlFlowComplete());
  REQUIRE(emission.allSourceSemanticsComplete());
  REQUIRE(emission.source.find("script {") != std::string::npos);
  REQUIRE(emission.source.find("fun main(local0: u64)") != std::string::npos);
}

TEST(version_five_script_footer_omits_access_specifier_option) {
  using movescape::format::TableKind;
  const Bytes signatures{0x00};
  const Bytes footer{
      0x00, // type-parameter count
      0x00, // parameter signature index
      0x00, // locals signature index (no access option before v8)
      0x01, // instruction count
      0x02, // Ret
  };
  const auto script = movescape::loadScript(scriptWithTables(5, {{TableKind::Signatures, signatures}}, footer));
  REQUIRE(!script.access_specifiers.has_value());
  movescape::validateScript(script);
}

TEST(script_loader_rejects_bad_options_trailing_bytes_and_module_tables) {
  using movescape::format::TableKind;
  const Bytes signatures{0x00};
  REQUIRE_ERROR(movescape::loadScript(scriptWithTables(10, {{TableKind::Signatures, signatures}}, Bytes{0x00, 0x00, 0x00, 0x00, 0x01, 0x02})),
                movescape::ErrorCode::Malformed);

  REQUIRE_ERROR(movescape::loadScript(scriptWithTables(10, {{TableKind::Signatures, signatures}}, Bytes{0x00, 0x00, 0x01, 0x00, 0x01, 0x02, 0xff})),
                movescape::ErrorCode::Malformed);

  REQUIRE_ERROR(movescape::loadScript(scriptWithTables(10, {{TableKind::FunctionDefinitions, Bytes{0x00}}}, Bytes{0x00, 0x00, 0x01, 0x00, 0x01, 0x02})),
                movescape::ErrorCode::InvalidTableLayout);
}

TEST(script_access_specifiers_parse_and_validate_parameter_indexes) {
  using movescape::format::TableKind;
  const Bytes signatures{0x01, 0x05, 0x00}; // address parameters, empty locals
  const auto footer = [](std::uint8_t parameter) {
    return Bytes{
        0x00, // type-parameter count
        0x00, // parameter signature index
        0x02, // Some(access specifiers)
        0x01, // one access
        0x01, // reads
        0x01, // not negated
        0x01, // any resource
        0x03, // parameter address
        parameter,
        0x01, // no address-derivation function
        0x01, // locals signature index
        0x01, // instruction count
        0x02, // Ret
    };
  };
  const auto valid = movescape::loadScript(scriptWithTables(10, {{TableKind::Signatures, signatures}}, footer(0)));
  REQUIRE(valid.access_specifiers.has_value());
  REQUIRE_EQ(valid.access_specifiers->size(), 1U);
  movescape::validateScript(valid);

  const auto invalid = movescape::loadScript(scriptWithTables(10, {{TableKind::Signatures, signatures}}, footer(1)));
  REQUIRE_ERROR(movescape::validateScript(invalid), movescape::ErrorCode::InvalidIndex);
}

TEST(native_function_flag_omits_code_unit) {
  using movescape::format::TableKind;
  const Bytes function_definitions{
      0x00, // function handle
      0x00, // private
      0x02, // native flag
      0x00, // acquires
  };
  const auto bytes = moduleWithTables(5, {{TableKind::FunctionDefinitions, function_definitions}});
  const auto module = movescape::loadModule(bytes);

  REQUIRE_EQ(module.function_definitions.size(), 1U);
  REQUIRE(!module.function_definitions[0].code.has_value());
}

TEST(non_native_function_requires_a_code_unit) {
  using movescape::format::TableKind;
  const Bytes function_definitions{
      0x00, // function handle
      0x00, // private
      0x00, // non-native flags
      0x00, // acquires, followed by a missing code unit
  };
  const auto bytes = moduleWithTables(5, {{TableKind::FunctionDefinitions, function_definitions}});
  REQUIRE_ERROR(movescape::loadModule(bytes), movescape::ErrorCode::UnexpectedEof);
}

TEST(load_nested_signature_types) {
  using movescape::TypeKind;
  using movescape::format::TableKind;

  // One signature containing:
  // vector<S<0><u8, &mut u64>>
  Bytes signatures{
      0x01, // signature length
      0x0a, // vector
      0x0b, // struct instantiation
      0x00, // struct handle
      0x02, // arity
      0x02, // u8
      0x07, // mutable reference
      0x03, // u64
  };
  const auto bytes = moduleWithTables(6, {{TableKind::Signatures, signatures}});
  const auto module = movescape::loadModule(bytes);

  REQUIRE_EQ(module.signatures.size(), 1U);
  const auto &vector = module.signatures[0][0];
  REQUIRE_EQ(vector.kind, TypeKind::Vector);
  REQUIRE_EQ(vector.arguments.size(), 1U);
  const auto &structure = vector.arguments[0];
  REQUIRE_EQ(structure.kind, TypeKind::StructInstantiation);
  REQUIRE_EQ(structure.arguments.size(), 2U);
  REQUIRE_EQ(structure.arguments[0].kind, TypeKind::U8);
  REQUIRE_EQ(structure.arguments[1].kind, TypeKind::MutableReference);
  REQUIRE_EQ(structure.arguments[1].arguments[0].kind, TypeKind::U64);
}

TEST(iterative_type_decoder_preserves_function_arguments_and_results) {
  using movescape::TypeKind;
  using movescape::format::TableKind;
  const Bytes signatures{
      0x01, // signature length
      0x10, // function
      0x00, // abilities
      0x02, // argument count
      0x01, // result count
      0x02, // u8 argument
      0x03, // u64 argument
      0x01, // bool result
  };
  const auto bytes = moduleWithTables(8, {{TableKind::Signatures, signatures}});
  const auto module = movescape::loadModule(bytes);
  const auto &function = module.signatures.at(0).at(0);

  REQUIRE_EQ(function.kind, TypeKind::Function);
  REQUIRE_EQ(function.arguments.size(), 2U);
  REQUIRE_EQ(function.arguments[0].kind, TypeKind::U8);
  REQUIRE_EQ(function.arguments[1].kind, TypeKind::U64);
  REQUIRE_EQ(function.results.size(), 1U);
  REQUIRE_EQ(function.results[0].kind, TypeKind::Bool);
}

TEST(iterative_type_decoder_accepts_maximum_nesting_depth) {
  using movescape::TypeKind;
  using movescape::format::TableKind;
  Bytes signatures{0x01};
  signatures.insert(signatures.end(), 255, 0x0a); // vector
  signatures.push_back(0x02);                     // u8 leaf at depth 255
  const auto bytes = moduleWithTables(9, {{TableKind::Signatures, signatures}});
  const auto module = movescape::loadModule(bytes);

  const auto *type = &module.signatures.at(0).at(0);
  for (std::size_t depth = 0; depth < 255; ++depth) {
    REQUIRE_EQ(type->kind, TypeKind::Vector);
    REQUIRE_EQ(type->arguments.size(), 1U);
    type = &type->arguments[0];
  }
  REQUIRE_EQ(type->kind, TypeKind::U8);
}

TEST(iterative_type_decoder_rejects_excessive_nesting_depth) {
  using movescape::format::TableKind;
  Bytes signatures{0x01};
  signatures.insert(signatures.end(), 256, 0x0a); // vector
  signatures.push_back(0x02);                     // unreachable u8 leaf
  const auto bytes = moduleWithTables(9, {{TableKind::Signatures, signatures}});
  REQUIRE_ERROR(movescape::loadModule(bytes), movescape::ErrorCode::ResourceLimit);
}

TEST(custom_limits_accept_values_exactly_at_the_boundary) {
  using movescape::format::TableKind;
  const Bytes identifiers{0x01, 'A'};
  const auto bytes = moduleWithTables(5, {{TableKind::Identifiers, identifiers}});
  movescape::ParserLimits limits;
  limits.max_file_bytes = bytes.size();
  limits.max_table_count = 1;
  limits.max_table_content_bytes = identifiers.size();
  limits.max_table_entries = 1;
  limits.max_identifier_bytes = 1;

  const auto module = movescape::loadModule(bytes, limits);
  REQUIRE_EQ(module.identifiers.size(), 1U);
  REQUIRE_EQ(module.identifiers[0], std::string("A"));
}

TEST(custom_limits_reject_table_entry_count) {
  using movescape::format::TableKind;
  const Bytes identifiers{0x01, 'A', 0x01, 'B'};
  const auto bytes = moduleWithTables(5, {{TableKind::Identifiers, identifiers}});
  movescape::ParserLimits limits;
  limits.max_table_entries = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, limits), movescape::ErrorCode::ResourceLimit);
}

TEST(custom_limits_reject_identifier_size) {
  using movescape::format::TableKind;
  const Bytes identifiers{0x02, 'A', 'b'};
  const auto bytes = moduleWithTables(5, {{TableKind::Identifiers, identifiers}});
  movescape::ParserLimits limits;
  limits.max_identifier_bytes = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, limits), movescape::ErrorCode::ResourceLimit);
}

TEST(custom_limits_reject_constant_blob_size) {
  using movescape::format::TableKind;
  const Bytes constants{
      0x02, // u8 type
      0x02, // data length
      0x11,
      0x22, // data
  };
  const auto bytes = moduleWithTables(5, {{TableKind::Constants, constants}});
  movescape::ParserLimits limits;
  limits.max_constant_bytes = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, limits), movescape::ErrorCode::ResourceLimit);
}

TEST(custom_limits_reject_metadata_key_and_value_sizes) {
  using movescape::format::TableKind;
  const Bytes metadata{
      0x02, 'k', '1', // key
      0x02, 'v', '1', // value
  };
  const auto bytes = moduleWithTables(5, {{TableKind::Metadata, metadata}});

  movescape::ParserLimits key_limits;
  key_limits.max_metadata_key_bytes = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, key_limits), movescape::ErrorCode::ResourceLimit);

  movescape::ParserLimits value_limits;
  value_limits.max_metadata_value_bytes = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, value_limits), movescape::ErrorCode::ResourceLimit);
}

TEST(custom_limits_reject_signature_length_and_type_depth) {
  using movescape::format::TableKind;
  const Bytes long_signature{
      0x02, // signature length
      0x02, // u8
      0x03, // u64
  };
  auto bytes = moduleWithTables(5, {{TableKind::Signatures, long_signature}});
  movescape::ParserLimits length_limits;
  length_limits.max_signature_length = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, length_limits), movescape::ErrorCode::ResourceLimit);

  const Bytes nested_signature{
      0x01, // signature length
      0x0a, // vector, depth 1
      0x0a, // vector, depth 2
      0x02, // u8, depth 3
  };
  bytes = moduleWithTables(5, {{TableKind::Signatures, nested_signature}});
  movescape::ParserLimits depth_limits;
  depth_limits.max_type_nesting = 2;
  REQUIRE_ERROR(movescape::loadModule(bytes, depth_limits), movescape::ErrorCode::ResourceLimit);
}

TEST(custom_limits_reject_aggregate_type_node_count) {
  using movescape::format::TableKind;
  const Bytes signatures{
      0x02, // signature length
      0x02, // u8
      0x03, // u64
  };
  const auto bytes = moduleWithTables(5, {{TableKind::Signatures, signatures}});
  movescape::ParserLimits limits;
  limits.max_total_type_nodes = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, limits), movescape::ErrorCode::ResourceLimit);
}

TEST(custom_limits_reject_general_list_and_instruction_counts) {
  using movescape::format::TableKind;
  const Bytes struct_definitions{
      0x00,       // struct handle
      0x02,       // declared fields
      0x02,       // field count
      0x00, 0x02, // field name, u8
      0x00, 0x02, // field name, u8
  };
  auto bytes = moduleWithTables(5, {{TableKind::StructDefinitions, struct_definitions}});
  movescape::ParserLimits list_limits;
  list_limits.max_list_elements = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, list_limits), movescape::ErrorCode::ResourceLimit);

  const Bytes function_definitions{
      0x00, // function handle
      0x00, // private
      0x00, // flags
      0x00, // acquires
      0x00, // locals signature
      0x02, // instruction count
      static_cast<std::uint8_t>(movescape::Opcode::Nop),
      static_cast<std::uint8_t>(movescape::Opcode::Ret),
  };
  bytes = moduleWithTables(5, {
                                  {TableKind::Signatures, Bytes{0x00}},
                                  {TableKind::FunctionDefinitions, function_definitions},
                              });
  movescape::ParserLimits instruction_limits;
  instruction_limits.max_instructions_per_function = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, instruction_limits), movescape::ErrorCode::ResourceLimit);
}

TEST(custom_limits_reject_aggregate_instruction_count) {
  using movescape::format::TableKind;
  const Bytes function_definitions{
      0x00,
      0x00,
      0x00,
      0x00, // handle, visibility, flags, acquires
      0x00,
      0x01, // locals signature, instruction count
      static_cast<std::uint8_t>(movescape::Opcode::Nop),
      0x00,
      0x00,
      0x00,
      0x00, // second function header
      0x00,
      0x01, // locals signature, instruction count
      static_cast<std::uint8_t>(movescape::Opcode::Ret),
  };
  const auto bytes = moduleWithTables(5, {
                                             {TableKind::Signatures, Bytes{0x00}},
                                             {TableKind::FunctionDefinitions, function_definitions},
                                         });
  movescape::ParserLimits limits;
  limits.max_instructions_per_function = 1;
  limits.max_total_instructions = 1;
  REQUIRE_ERROR(movescape::loadModule(bytes, limits), movescape::ErrorCode::ResourceLimit);
}

TEST(load_all_valid_bytecode_identifier_forms) {
  using movescape::format::TableKind;
  Bytes identifiers{
      0x01, 'A',  0x06, '_', 'l', 'o', 'c', 'a', 'l',  0x09, '$', 'g', 'e', 'n', 'e', 'r', 'a', 't',
      'e',  0x06, '<',  'S', 'E', 'L', 'F', '>', 0x09, '<',  'S', 'E', 'L', 'F', '>', '_', '1', '2',
  };
  const auto bytes = moduleWithTables(0x0a000009U, {{TableKind::Identifiers, identifiers}});
  const auto module = movescape::loadModule(bytes);

  REQUIRE_EQ(module.identifiers.size(), 5U);
  REQUIRE_EQ(module.identifiers[2], std::string("$generate"));
  REQUIRE_EQ(module.identifiers[4], std::string("<SELF>_12"));
}

TEST(reject_invalid_move_bytecode_identifiers) {
  using movescape::format::TableKind;
  const std::vector<Bytes> invalid_identifiers{
      Bytes{0x00},
      Bytes{0x01, '_'},
      Bytes{0x01, '$'},
      Bytes{0x04, '9', 'b', 'a', 'd'},
      Bytes{0x05, 'a', '-', 'b', 'a', 'd'},
      Bytes{0x08, '<', 'S', 'E', 'L', 'F', '>', '_', 'x'},
      Bytes{0x02, 0xc3, 0xa9}, // Valid UTF-8, but identifiers are ASCII-only.
  };

  for (const auto &identifiers : invalid_identifiers) {
    const auto bytes = moduleWithTables(9, {{TableKind::Identifiers, identifiers}});
    REQUIRE_ERROR(movescape::loadModule(bytes), movescape::ErrorCode::Malformed);
  }
}

TEST(reject_dollar_identifier_before_bytecode_version_nine) {
  using movescape::format::TableKind;
  const Bytes identifiers{0x02, '$', 'x'};
  const auto bytes = moduleWithTables(8, {{TableKind::Identifiers, identifiers}});
  REQUIRE_ERROR(movescape::loadModule(bytes), movescape::ErrorCode::Malformed);
}

TEST(reject_version_gated_signature_type) {
  using movescape::format::TableKind;
  Bytes signatures{
      0x01, // signature length
      0x0d, // u16, introduced in version 6
  };
  const auto bytes = moduleWithTables(5, {{TableKind::Signatures, signatures}});
  REQUIRE_ERROR(movescape::loadModule(bytes), movescape::ErrorCode::UnsupportedFeature);
}

TEST(reject_trailing_module_footer_bytes) {
  auto bytes = moduleWithTables(5, {});
  bytes.push_back(0);
  REQUIRE_ERROR(movescape::loadModule(bytes), movescape::ErrorCode::Malformed);
}

TEST(decode_every_version_ten_opcode) {
  using movescape::format::TableKind;

  Bytes definitions{
      0x00, // function handle
      0x00, // private
      0x00, // flags
      0x00, // acquires count
      0x00, // locals signature
  };
  appendUleb(definitions, 104);
  for (std::uint16_t value = 1; value <= 0x68; ++value) {
    appendInstruction(definitions, static_cast<movescape::Opcode>(value));
  }

  const auto bytes = moduleWithTables(0x0a00000aU, {
                                                       {TableKind::Signatures, Bytes{0x00}},
                                                       {TableKind::FunctionDefinitions, definitions},
                                                   });
  const auto module = movescape::loadModule(bytes);
  REQUIRE_EQ(module.function_definitions.size(), 1U);
  REQUIRE(module.function_definitions[0].code.has_value());
  const auto &code = module.function_definitions[0].code->code;
  REQUIRE_EQ(code.size(), 104U);
  for (std::size_t index = 0; index < code.size(); ++index) {
    REQUIRE_EQ(static_cast<std::uint8_t>(code[index].opcode), index + 1U);
  }
  REQUIRE_EQ(code.back().opcode, movescape::Opcode::AbortMsg);
}

TEST(decode_and_render_negative_signed_immediate) {
  using movescape::format::TableKind;

  Bytes definitions{
      0x00, // function handle
      0x00, // private
      0x00, // flags
      0x00, // acquires count
      0x00, // locals signature
      0x02, // instruction count
      static_cast<std::uint8_t>(movescape::Opcode::LdI8),
      0xff, // -1i8
      static_cast<std::uint8_t>(movescape::Opcode::Ret),
  };
  const auto bytes = moduleWithTables(0x0a000009U, {
                                                       {TableKind::Signatures, Bytes{0x00}},
                                                       {TableKind::FunctionDefinitions, definitions},
                                                   });
  const auto module = movescape::loadModule(bytes);
  const auto &instruction = module.function_definitions[0].code->code.front();

  REQUIRE_EQ(std::bit_cast<std::int64_t>(instruction.operands.at(0)), -1);
  REQUIRE_EQ(movescape::renderInstruction(module, instruction, 0), std::string("0000: LdI8 -1"));
}

TEST(reject_opcode_before_its_introduction_version) {
  using movescape::format::TableKind;
  Bytes definitions{
      0x00, // function handle
      0x00, // private
      0x00, // flags
      0x00, // acquires count
      0x00, // locals signature
      0x01, // instruction count
      0x68, // AbortMsg, version 10
  };
  const auto bytes = moduleWithTables(0x0a000009U, {
                                                       {TableKind::Signatures, Bytes{0x00}},
                                                       {TableKind::FunctionDefinitions, definitions},
                                                   });
  REQUIRE_ERROR(movescape::loadModule(bytes), movescape::ErrorCode::UnsupportedFeature);
}
