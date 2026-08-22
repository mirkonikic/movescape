#include "movescape/module_loader.hpp"

#include "movescape/binary_reader.hpp"
#include "movescape/error.hpp"
#include "movescape/format.hpp"
#include "movescape/loader.hpp"
#include "movescape/opcode.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace movescape {

namespace {

constexpr std::uint64_t kMaximumTableIndex = 65535;
constexpr std::uint64_t kMaximumLocalIndex = 255;
constexpr std::uint64_t kMaximumSignatureLength = 255;
constexpr std::uint64_t kMaximumTypeParameters = 255;
constexpr std::uint64_t kMaximumFields = 255;
constexpr std::uint64_t kMaximumVariants = 128;
constexpr std::uint64_t kMaximumVariantOffset = 127;
constexpr std::uint64_t kMaximumAcquires = 255;
constexpr std::uint64_t kMaximumInstructions = 65535;
constexpr std::uint64_t kMaximumConstantBytes = 65535;
constexpr std::uint64_t kMaximumMetadataKeyBytes = 1023;
constexpr std::uint64_t kMaximumMetadataValueBytes = 65535;
constexpr std::uint64_t kMaximumIdentifierBytes = 255;
constexpr std::uint64_t kMaximumAccessSpecifiers = 64;
constexpr std::uint64_t kMaximumFunctionAttributes = 16;
constexpr std::size_t kMaximumTypeDepth = 256;

[[noreturn]] void malformed(const BinaryReader &reader, std::string message) { throw Error(ErrorCode::Malformed, reader.absolutePosition(), std::move(message));}
[[nodiscard]] TableIndex readTableIndex(BinaryReader &reader, std::string_view field) { return static_cast<TableIndex>(reader.readUleb128(kMaximumTableIndex, field));}
[[nodiscard]] std::uint16_t readBoundedU16(BinaryReader &reader, std::uint64_t maximum, std::string_view field) { return static_cast<std::uint16_t>(reader.readUleb128(maximum, field));}
[[nodiscard]] std::size_t readCount(BinaryReader &reader, std::uint64_t maximum, std::string_view field) { return static_cast<std::size_t>(reader.readUleb128(maximum, field));}

[[nodiscard]] bool readSerializedBool(BinaryReader &reader, std::string_view field) {
  const auto value = reader.readU8(field);
  if (value == 0x01U) { return false; }
  if (value == 0x02U) { return true; }
  malformed(reader, "invalid serialized boolean marker");
}

[[nodiscard]] bool readOptionMarker(BinaryReader &reader, std::string_view field) {
  const auto value = reader.readU8(field);
  if (value == 0x01U) { return false; }
  if (value == 0x02U) { return true; }
  malformed(reader, "invalid serialized option marker");
}

[[nodiscard]] bool validUtf8(std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto first = bytes[offset++];
    if (first <= 0x7fU) { continue; }

    std::size_t continuation_count = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xe0U) == 0xc0U) {
      continuation_count = 1;
      value = first & 0x1fU;
      minimum = 0x80;
    } else if ((first & 0xf0U) == 0xe0U) {
      continuation_count = 2;
      value = first & 0x0fU;
      minimum = 0x800;
    } else if ((first & 0xf8U) == 0xf0U) {
      continuation_count = 3;
      value = first & 0x07U;
      minimum = 0x10000;
    } else { return false; }

    if (continuation_count > bytes.size() - offset) { return false; }
    for (std::size_t index = 0; index < continuation_count; ++index) {
      const auto byte = bytes[offset++];
      if ((byte & 0xc0U) != 0x80U) {
        return false;
      }
      value = (value << 6U) | (byte & 0x3fU);
    }

    if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) { return false; }
  }
  return true;
}

[[nodiscard]] constexpr bool asciiLetter(char value) noexcept { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); }
[[nodiscard]] constexpr bool identifierContinuation(char value) noexcept { return asciiLetter(value) || (value >= '0' && value <= '9') || value == '_' || value == '$'; }

[[nodiscard]] bool validMoveIdentifier(std::string_view value) noexcept {
  if (value == "<SELF>") { return true; }
  constexpr std::string_view self_prefix = "<SELF>_";
  if (value.starts_with(self_prefix) && value.size() > self_prefix.size()) {
    return std::all_of(value.begin() + static_cast<std::string_view::difference_type>(self_prefix.size()), value.end(), [](char character) { return character >= '0' && character <= '9'; }); 
  }
  if (value.empty()) { return false; }
  const auto first = value.front();
  if (!asciiLetter(first) && !((first == '_' || first == '$') && value.size() > 1)) { return false; }
  return std::all_of(value.begin() + 1, value.end(), identifierContinuation);
}

class ModuleParser {
public:
  ModuleParser(std::span<const std::uint8_t> bytes, BinaryEnvelope envelope, const ParserLimits &limits) : bytes_(bytes), envelope_(std::move(envelope)), limits_(limits) {
    module_.raw_version = envelope_.raw_version;
    module_.version = envelope_.version;
  }

  [[nodiscard]] Module parse() {
    parseTables();

    BinaryReader footer(bytes_.subspan(envelope_.footer_offset), envelope_.footer_offset);
    module_.self_module_handle = readTableIndex(footer, "self module handle index");
    if (!footer.empty()) {
      throw Error(ErrorCode::Malformed, footer.absolutePosition(), "trailing bytes after module footer");
    }
    return std::move(module_);
  }

  [[nodiscard]] Script parseScript() {
    parseTables();
    BinaryReader footer(bytes_.subspan(envelope_.footer_offset), envelope_.footer_offset);
    Script result;
    result.type_parameters = parseAbilitySets(footer);
    result.parameters = readTableIndex(footer, "script parameter signature");
    if (module_.version >= 8) {
      result.access_specifiers = parseAccessSpecifiers(footer);
    }
    result.code = parseCodeUnit(footer);
    if (!footer.empty()) {
      throw Error(ErrorCode::Malformed, footer.absolutePosition(), "trailing bytes after script footer");
    }
    result.common = std::move(module_);
    return result;
  }

private:
  void parseTables() {
    for (const auto &table : envelope_.tables) {
      BinaryReader reader(envelope_.tableBytes(bytes_, table), envelope_.table_content_offset + static_cast<std::size_t>(table.offset));
      parseTable(table.kind, reader);
      if (!reader.empty()) {
        throw Error(ErrorCode::Malformed, reader.absolutePosition(), "table decoder did not consume the complete table");
      }
    }
  }
  template <typename Value, typename Loader> void parseEntries(BinaryReader &reader, std::vector<Value> &output, Loader loader) {
    while (!reader.empty()) {
      requireAdditional(output.size(), 1, limits_.max_table_entries, reader.absolutePosition(), "table entry count");
      output.push_back((this->*loader)(reader));
    }
  }

  void requireLimit(std::size_t value, std::size_t maximum, std::size_t offset, std::string_view subject) const {
    if (value <= maximum) {
      return;
    }
    std::ostringstream out;
    out << subject << " " << value << " exceeds configured limit " << maximum;
    throw Error(ErrorCode::ResourceLimit, offset, out.str());
  }

  void requireAdditional(std::size_t current, std::size_t additional, std::size_t maximum, std::size_t offset, std::string_view subject) const {
    if (current <= maximum && additional <= maximum - current) {
      return;
    }
    std::ostringstream out;
    out << subject << " exceeds configured limit " << maximum;
    throw Error(ErrorCode::ResourceLimit, offset, out.str());
  }

  [[nodiscard]] std::size_t readLimitedCount(BinaryReader &reader, std::uint64_t wire_maximum, std::size_t configured_maximum, std::string_view field) const {
    const auto offset = reader.absolutePosition();
    const auto value = readCount(reader, wire_maximum, field);
    requireLimit(value, configured_maximum, offset, field);
    return value;
  }

  [[nodiscard]] AbilitySet parseAbilitySet(BinaryReader &reader) const {
    const auto bits = reader.readUleb128(AbilitySet::All, "ability set");
    return AbilitySet{static_cast<std::uint8_t>(bits)};
  }

  [[nodiscard]] std::vector<AbilitySet> parseAbilitySets(BinaryReader &reader) const {
    const auto count = readLimitedCount(reader, kMaximumTypeParameters, limits_.max_list_elements, "type parameter count");
    std::vector<AbilitySet> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      result.push_back(parseAbilitySet(reader));
    }
    return result;
  }

  [[nodiscard]] std::vector<StructTypeParameter> parseStructTypeParameters(BinaryReader &reader) const {
    const auto count = readLimitedCount(reader, kMaximumTypeParameters, limits_.max_list_elements, "struct type parameter count");
    std::vector<StructTypeParameter> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto abilities = parseAbilitySet(reader);
      const auto phantom = reader.readUleb128(1, "phantom marker") != 0;
      result.push_back(StructTypeParameter{abilities, phantom});
    }
    return result;
  }

  [[nodiscard]] Type parseType(BinaryReader &reader) const {
    struct Frame {
      Type type;
      std::size_t arguments_remaining = 0;
      std::size_t results_remaining = 0;
    };

    const auto read_frame = [&]() -> Frame {
      requireAdditional(total_type_nodes_, 1, limits_.max_total_type_nodes, reader.absolutePosition(), "total signature type nodes");
      ++total_type_nodes_;
      const auto tag_offset = reader.absolutePosition();
      const auto tag = reader.readU8("serialized type");
      Frame frame;
      const auto simple = [&](TypeKind kind) { frame.type.kind = kind; };

      switch (tag) {
      case 0x01:
        simple(TypeKind::Bool);
        break;
      case 0x02:
        simple(TypeKind::U8);
        break;
      case 0x03:
        simple(TypeKind::U64);
        break;
      case 0x04:
        simple(TypeKind::U128);
        break;
      case 0x05:
        simple(TypeKind::Address);
        break;
      case 0x06:
        simple(TypeKind::Reference);
        frame.arguments_remaining = 1;
        break;
      case 0x07:
        simple(TypeKind::MutableReference);
        frame.arguments_remaining = 1;
        break;
      case 0x08:
        simple(TypeKind::Struct);
        frame.type.index = readTableIndex(reader, "struct handle index");
        break;
      case 0x09:
        simple(TypeKind::TypeParameter);
        frame.type.index = readBoundedU16(reader, 65535, "type parameter index");
        break;
      case 0x0a:
        simple(TypeKind::Vector);
        frame.arguments_remaining = 1;
        break;
      case 0x0b: {
        simple(TypeKind::StructInstantiation);
        frame.type.index = readTableIndex(reader, "struct handle index");
        const auto count = readLimitedCount(reader, kMaximumTypeParameters, limits_.max_list_elements, "struct type argument count");
        if (count == 0) {
          malformed(reader, "struct instantiation has zero type arguments");
        }
        frame.type.arguments.reserve(count);
        frame.arguments_remaining = count;
        break;
      }
      case 0x0c:
        simple(TypeKind::Signer);
        break;
      case 0x0d:
        requireVersion(6, tag_offset, "u16 type");
        simple(TypeKind::U16);
        break;
      case 0x0e:
        requireVersion(6, tag_offset, "u32 type");
        simple(TypeKind::U32);
        break;
      case 0x0f:
        requireVersion(6, tag_offset, "u256 type");
        simple(TypeKind::U256);
        break;
      case 0x10:
        requireVersion(8, tag_offset, "function type");
        simple(TypeKind::Function);
        frame.type.abilities = parseAbilitySet(reader);
        frame.arguments_remaining = readLimitedCount(reader, kMaximumTypeParameters, limits_.max_list_elements, "function type argument count");
        frame.results_remaining = readLimitedCount(reader, kMaximumTypeParameters, limits_.max_list_elements, "function type result count");
        frame.type.arguments.reserve(frame.arguments_remaining);
        frame.type.results.reserve(frame.results_remaining);
        break;
      case 0x11:
        requireVersion(9, tag_offset, "i8 type");
        simple(TypeKind::I8);
        break;
      case 0x12:
        requireVersion(9, tag_offset, "i16 type");
        simple(TypeKind::I16);
        break;
      case 0x13:
        requireVersion(9, tag_offset, "i32 type");
        simple(TypeKind::I32);
        break;
      case 0x14:
        requireVersion(9, tag_offset, "i64 type");
        simple(TypeKind::I64);
        break;
      case 0x15:
        requireVersion(9, tag_offset, "i128 type");
        simple(TypeKind::I128);
        break;
      case 0x16:
        requireVersion(9, tag_offset, "i256 type");
        simple(TypeKind::I256);
        break;
      default: {
        std::ostringstream out;
        out << "unknown serialized type 0x" << std::hex << static_cast<unsigned>(tag);
        throw Error(ErrorCode::UnknownSerializedType, tag_offset, out.str());
      }
      }
      return frame;
    };

    std::vector<Frame> stack;
    stack.reserve(kMaximumTypeDepth);
    requireLimit(1, limits_.max_type_nesting, reader.absolutePosition(), "signature type nesting");
    stack.push_back(read_frame());
    while (true) {
      auto &current = stack.back();
      if (current.arguments_remaining != 0 || current.results_remaining != 0) {
        if (stack.size() >= limits_.max_type_nesting) {
          throw Error(ErrorCode::ResourceLimit, reader.absolutePosition(),
                      "signature type nesting exceeds configured limit " + std::to_string(limits_.max_type_nesting));
        }
        if (stack.size() >= kMaximumTypeDepth) {
          throw Error(ErrorCode::Malformed, reader.absolutePosition(), "signature type nesting exceeds 256");
        }
        stack.push_back(read_frame());
        continue;
      }

      auto completed = std::move(current.type);
      stack.pop_back();
      if (stack.empty()) {
        return completed;
      }

      auto &parent = stack.back();
      if (parent.arguments_remaining != 0) {
        parent.type.arguments.push_back(std::move(completed));
        --parent.arguments_remaining;
      } else {
        parent.type.results.push_back(std::move(completed));
        --parent.results_remaining;
      }
    }
  }

  void requireVersion(std::uint32_t minimum, std::size_t offset, std::string_view feature) const {
    if (module_.version < minimum) {
      std::ostringstream out;
      out << feature << " requires bytecode version " << minimum << ", module uses version " << module_.version;
      throw Error(ErrorCode::UnsupportedFeature, offset, out.str());
    }
  }

  [[nodiscard]] std::optional<std::vector<AccessSpecifier>> parseAccessSpecifiers(BinaryReader &reader) const {
    if (!readOptionMarker(reader, "access specifier option")) {
      return std::nullopt;
    }
    const auto count = readLimitedCount(reader, kMaximumAccessSpecifiers, limits_.max_list_elements, "access specifier count");
    std::vector<AccessSpecifier> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      result.push_back(parseAccessSpecifier(reader));
    }
    return result;
  }

  [[nodiscard]] AccessSpecifier parseAccessSpecifier(BinaryReader &reader) const {
    AccessSpecifier result;
    const auto access = reader.readU8("access kind");
    if (access == 0x01U) {
      result.kind = AccessKind::Reads;
    } else if (access == 0x02U) {
      result.kind = AccessKind::Writes;
    } else {
      malformed(reader, "invalid access kind");
    }
    result.negated = readSerializedBool(reader, "access negation");

    const auto resource = reader.readU8("resource specifier kind");
    switch (resource) {
    case 0x01:
      result.resource.kind = ResourceSpecifierKind::Any;
      break;
    case 0x02:
      result.resource.kind = ResourceSpecifierKind::DeclaredAtAddress;
      result.resource.primary = readTableIndex(reader, "resource address index");
      break;
    case 0x03:
      result.resource.kind = ResourceSpecifierKind::DeclaredInModule;
      result.resource.primary = readTableIndex(reader, "resource module handle index");
      break;
    case 0x04:
      result.resource.kind = ResourceSpecifierKind::Resource;
      result.resource.primary = readTableIndex(reader, "resource struct handle index");
      break;
    case 0x05:
      result.resource.kind = ResourceSpecifierKind::ResourceInstantiation;
      result.resource.primary = readTableIndex(reader, "resource struct handle index");
      result.resource.signature = readTableIndex(reader, "resource signature index");
      break;
    default:
      malformed(reader, "invalid resource specifier kind");
    }

    const auto address = reader.readU8("address specifier kind");
    switch (address) {
    case 0x01:
      result.address.kind = AddressSpecifierKind::Any;
      break;
    case 0x02:
      result.address.kind = AddressSpecifierKind::Literal;
      result.address.value = readTableIndex(reader, "literal address identifier index");
      break;
    case 0x03:
      result.address.kind = AddressSpecifierKind::Parameter;
      result.address.value = static_cast<TableIndex>(reader.readUleb128(kMaximumLocalIndex, "address parameter local"));
      if (readOptionMarker(reader, "address derivation option")) {
        result.address.function_instantiation = readTableIndex(reader, "address derivation function instantiation");
      }
      break;
    default:
      malformed(reader, "invalid address specifier kind");
    }
    return result;
  }

  [[nodiscard]] std::vector<FunctionAttribute> parseFunctionAttributes(BinaryReader &reader) const {
    if (module_.version < 8) {
      return {};
    }
    const auto count = readLimitedCount(reader, kMaximumFunctionAttributes, limits_.max_list_elements, "function attribute count");
    std::vector<FunctionAttribute> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto offset = reader.absolutePosition();
      const auto tag = reader.readU8("function attribute");
      FunctionAttribute attribute{
          .kind = FunctionAttributeKind::Persistent,
          .value = std::nullopt,
      };
      switch (tag) {
      case 0x01:
        attribute.kind = FunctionAttributeKind::Persistent;
        break;
      case 0x02:
        attribute.kind = FunctionAttributeKind::ModuleLock;
        break;
      case 0x03:
        requireVersion(10, offset, "pack function attribute");
        attribute.kind = FunctionAttributeKind::Pack;
        break;
      case 0x04:
        requireVersion(10, offset, "pack-variant function attribute");
        attribute.kind = FunctionAttributeKind::PackVariant;
        attribute.value = reader.readU16("variant offset");
        break;
      case 0x05:
        requireVersion(10, offset, "unpack function attribute");
        attribute.kind = FunctionAttributeKind::Unpack;
        break;
      case 0x06:
        requireVersion(10, offset, "unpack-variant function attribute");
        attribute.kind = FunctionAttributeKind::UnpackVariant;
        attribute.value = reader.readU16("variant offset");
        break;
      case 0x07:
        requireVersion(10, offset, "test-variant function attribute");
        attribute.kind = FunctionAttributeKind::TestVariant;
        attribute.value = reader.readU16("variant offset");
        break;
      case 0x08:
        requireVersion(10, offset, "immutable-field-borrow function attribute");
        attribute.kind = FunctionAttributeKind::BorrowFieldImmutable;
        attribute.value = reader.readU16("field offset");
        break;
      case 0x09:
        requireVersion(10, offset, "mutable-field-borrow function attribute");
        attribute.kind = FunctionAttributeKind::BorrowFieldMutable;
        attribute.value = reader.readU16("field offset");
        break;
      default:
        malformed(reader, "invalid function attribute tag");
      }
      result.push_back(attribute);
    }
    return result;
  }

  [[nodiscard]] ModuleHandle parseModuleHandle(BinaryReader &reader) const {
    return ModuleHandle{
        .address = readTableIndex(reader, "address identifier index"),
        .name = readTableIndex(reader, "module name identifier index"),
    };
  }

  [[nodiscard]] StructHandle parseStructHandle(BinaryReader &reader) const {
    return StructHandle{
        .module = readTableIndex(reader, "module handle index"),
        .name = readTableIndex(reader, "struct name identifier index"),
        .abilities = parseAbilitySet(reader),
        .type_parameters = parseStructTypeParameters(reader),
    };
  }

  [[nodiscard]] FunctionHandle parseFunctionHandle(BinaryReader &reader) const {
    FunctionHandle result;
    result.module = readTableIndex(reader, "module handle index");
    result.name = readTableIndex(reader, "function name identifier index");
    result.parameters = readTableIndex(reader, "parameter signature index");
    result.returns = readTableIndex(reader, "return signature index");
    result.type_parameters = parseAbilitySets(reader);
    if (module_.version >= 7) {
      result.access_specifiers = parseAccessSpecifiers(reader);
    }
    result.attributes = parseFunctionAttributes(reader);
    return result;
  }

  [[nodiscard]] FunctionInstantiation parseFunctionInstantiation(BinaryReader &reader) const {
    return FunctionInstantiation{
        .handle = readTableIndex(reader, "function handle index"),
        .type_parameters = readTableIndex(reader, "type argument signature index"),
    };
  }

  [[nodiscard]] Signature parseSignature(BinaryReader &reader) const {
    const auto count = readLimitedCount(reader, kMaximumSignatureLength, limits_.max_signature_length, "signature length");
    Signature result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      result.push_back(parseType(reader));
    }
    return result;
  }

  [[nodiscard]] Constant parseConstant(BinaryReader &reader) const {
    Constant result;
    result.type = parseType(reader);
    const auto count = readLimitedCount(reader, kMaximumConstantBytes, limits_.max_constant_bytes, "constant byte count");
    const auto bytes = reader.readBytes(count, "constant data");
    result.data.assign(bytes.begin(), bytes.end());
    return result;
  }

  [[nodiscard]] std::string parseIdentifier(BinaryReader &reader) const {
    const auto offset = reader.absolutePosition();
    const auto count = readLimitedCount(reader, kMaximumIdentifierBytes, limits_.max_identifier_bytes, "identifier byte count");
    const auto bytes = reader.readBytes(count, "identifier");
    if (!validUtf8(bytes)) {
      throw Error(ErrorCode::Malformed, offset, "identifier is not canonical UTF-8");
    }
    const std::string result(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (!validMoveIdentifier(result)) {
      throw Error(ErrorCode::Malformed, offset, "invalid Move bytecode identifier");
    }
    if (module_.version < 9 && result.find('$') != std::string::npos) {
      throw Error(ErrorCode::Malformed, offset, "'$' in identifiers requires bytecode version 9");
    }
    return result;
  }

  [[nodiscard]] Address parseAddress(BinaryReader &reader) const {
    Address result{};
    const auto bytes = reader.readBytes(result.size(), "address identifier");
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
  }

  [[nodiscard]] Metadata parseMetadata(BinaryReader &reader) const {
    Metadata result;
    const auto key_count = readLimitedCount(reader, kMaximumMetadataKeyBytes, limits_.max_metadata_key_bytes, "metadata key byte count");
    const auto key = reader.readBytes(key_count, "metadata key");
    result.key.assign(key.begin(), key.end());
    const auto value_count = readLimitedCount(reader, kMaximumMetadataValueBytes, limits_.max_metadata_value_bytes, "metadata value byte count");
    const auto value = reader.readBytes(value_count, "metadata value");
    result.value.assign(value.begin(), value.end());
    return result;
  }

  [[nodiscard]] FieldDefinition parseFieldDefinition(BinaryReader &reader) const {
    return FieldDefinition{
        .name = readTableIndex(reader, "field name identifier index"),
        .type = parseType(reader),
    };
  }

  [[nodiscard]] std::vector<FieldDefinition> parseFieldDefinitions(BinaryReader &reader) const {
    const auto count = readLimitedCount(reader, kMaximumFields, limits_.max_list_elements, "field count");
    std::vector<FieldDefinition> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      result.push_back(parseFieldDefinition(reader));
    }
    return result;
  }

  [[nodiscard]] StructDefinition parseStructDefinition(BinaryReader &reader) const {
    StructDefinition result;
    result.handle = readTableIndex(reader, "struct handle index");
    const auto flag_offset = reader.absolutePosition();
    const auto flag = reader.readU8("struct field information flag");
    if (flag == 0x01U) {
      result.field_kind = StructFieldKind::Native;
    } else if (flag == 0x02U) {
      result.field_kind = StructFieldKind::Declared;
      result.fields = parseFieldDefinitions(reader);
    } else if (flag == 0x03U) {
      requireVersion(7, flag_offset, "enum struct definition");
      result.field_kind = StructFieldKind::Variants;
      const auto count = readLimitedCount(reader, kMaximumVariants, limits_.max_list_elements, "variant count");
      result.variants.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        result.variants.push_back(VariantDefinition{
            .name = readTableIndex(reader, "variant name identifier index"),
            .fields = parseFieldDefinitions(reader),
        });
      }
    } else {
      malformed(reader, "invalid struct field information flag");
    }
    return result;
  }

  [[nodiscard]] StructDefinitionInstantiation parseStructDefinitionInstantiation(BinaryReader &reader) const {
    return StructDefinitionInstantiation{
        .definition = readTableIndex(reader, "struct definition index"),
        .type_parameters = readTableIndex(reader, "type argument signature index"),
    };
  }

  [[nodiscard]] FieldHandle parseFieldHandle(BinaryReader &reader) const {
    return FieldHandle{
        .owner = readTableIndex(reader, "struct definition index"),
        .field = readBoundedU16(reader, kMaximumFields, "field offset"),
    };
  }

  [[nodiscard]] FieldInstantiation parseFieldInstantiation(BinaryReader &reader) const {
    return FieldInstantiation{
        .handle = readTableIndex(reader, "field handle index"),
        .type_parameters = readTableIndex(reader, "type argument signature index"),
    };
  }

  [[nodiscard]] VariantFieldHandle parseVariantFieldHandle(BinaryReader &reader) const {
    VariantFieldHandle result;
    result.owner = readTableIndex(reader, "struct definition index");
    result.field = readBoundedU16(reader, kMaximumFields, "variant field offset");
    const auto count = readLimitedCount(reader, kMaximumVariants, limits_.max_list_elements, "variant offset count");
    result.variants.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      result.variants.push_back(readBoundedU16(reader, kMaximumVariantOffset, "variant offset"));
    }
    return result;
  }

  [[nodiscard]] VariantFieldInstantiation parseVariantFieldInstantiation(BinaryReader &reader) const {
    return VariantFieldInstantiation{
        .handle = readTableIndex(reader, "variant field handle index"),
        .type_parameters = readTableIndex(reader, "type argument signature index"),
    };
  }

  [[nodiscard]] StructVariantHandle parseStructVariantHandle(BinaryReader &reader) const {
    return StructVariantHandle{
        .definition = readTableIndex(reader, "struct definition index"),
        .variant = readBoundedU16(reader, kMaximumVariantOffset, "variant offset"),
    };
  }

  [[nodiscard]] StructVariantInstantiation parseStructVariantInstantiation(BinaryReader &reader) const {
    return StructVariantInstantiation{
        .handle = readTableIndex(reader, "struct variant handle index"),
        .type_parameters = readTableIndex(reader, "type argument signature index"),
    };
  }

  [[nodiscard]] Instruction parseInstruction(BinaryReader &reader) const {
    const auto offset = reader.absolutePosition();
    const auto byte = reader.readU8("opcode");
    const auto opcode = opcodeFromByte(byte);
    if (!opcode.has_value()) {
      std::ostringstream out;
      out << "unknown Move opcode 0x" << std::hex << static_cast<unsigned>(byte);
      throw Error(ErrorCode::UnknownOpcode, offset, out.str());
    }
    const auto &info = opcodeInfo(*opcode);
    requireVersion(info.introduced_version, offset, info.name);

    Instruction result;
    result.opcode = *opcode;
    result.serialized_offset = offset;

    const auto table_index = [&]() { result.operands.push_back(readTableIndex(reader, "instruction table index")); };
    const auto signed_operand = [&](auto value) {
      const auto extended = static_cast<std::int64_t>(value);
      result.operands.push_back(std::bit_cast<std::uint64_t>(extended));
    };
    const auto fixed_wide = [&](const auto &value) { result.wide_operand.assign(value.little_endian_bytes.begin(), value.little_endian_bytes.end()); };

    switch (info.operands) {
    case OperandEncoding::None:
      break;
    case OperandEncoding::LocalIndex:
      result.operands.push_back(reader.readUleb128(kMaximumLocalIndex, "local index"));
      break;
    case OperandEncoding::TableIndex:
      table_index();
      break;
    case OperandEncoding::CodeOffset:
      result.operands.push_back(reader.readUleb128(kMaximumInstructions, "code offset"));
      break;
    case OperandEncoding::U8:
      result.operands.push_back(reader.readU8("8-bit immediate"));
      break;
    case OperandEncoding::I8:
      signed_operand(reader.readI8("8-bit signed immediate"));
      break;
    case OperandEncoding::U16:
      result.operands.push_back(reader.readU16("16-bit immediate"));
      break;
    case OperandEncoding::I16:
      signed_operand(reader.readI16("16-bit signed immediate"));
      break;
    case OperandEncoding::U32:
      result.operands.push_back(reader.readU32("32-bit immediate"));
      break;
    case OperandEncoding::I32:
      signed_operand(reader.readI32("32-bit signed immediate"));
      break;
    case OperandEncoding::U64:
      result.operands.push_back(reader.readU64("64-bit immediate"));
      break;
    case OperandEncoding::I64:
      signed_operand(reader.readI64("64-bit signed immediate"));
      break;
    case OperandEncoding::U128:
      fixed_wide(reader.readU128("128-bit immediate"));
      break;
    case OperandEncoding::U256:
      fixed_wide(reader.readU256("256-bit immediate"));
      break;
    case OperandEncoding::I128:
      fixed_wide(reader.readI128("128-bit signed immediate"));
      break;
    case OperandEncoding::I256:
      fixed_wide(reader.readI256("256-bit signed immediate"));
      break;
    case OperandEncoding::SignatureAndU64:
      table_index();
      result.operands.push_back(reader.readU64("vector element count"));
      break;
    case OperandEncoding::TableIndexAndClosureMask:
      table_index();
      result.operands.push_back(reader.readUleb128(std::numeric_limits<std::uint64_t>::max(), "closure mask"));
      break;
    }
    return result;
  }

  [[nodiscard]] CodeUnit parseCodeUnit(BinaryReader &reader) const {
    CodeUnit result;
    result.locals = readTableIndex(reader, "locals signature index");
    const auto count = readLimitedCount(reader, kMaximumInstructions, limits_.max_instructions_per_function, "instruction count");
    requireAdditional(total_instructions_, count, limits_.max_total_instructions, reader.absolutePosition(), "total instruction count");
    total_instructions_ += count;
    result.code.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      result.code.push_back(parseInstruction(reader));
    }
    return result;
  }

  [[nodiscard]] FunctionDefinition parseFunctionDefinition(BinaryReader &reader) const {
    FunctionDefinition result;
    result.handle = readTableIndex(reader, "function handle index");

    const auto visibility = reader.readU8("function visibility");
    if (visibility == 0x00U) { result.visibility = Visibility::Private; } 
    else if (visibility == 0x01U) { result.visibility = Visibility::Public; } 
    else if (visibility == 0x03U) { result.visibility = Visibility::Friend; } 
    else { malformed(reader, "invalid function visibility"); }

    auto flags = reader.readU8("function flags");
    result.is_entry = (flags & 0x04U) != 0;
    flags = static_cast<std::uint8_t>(flags & ~0x04U);
    const bool is_native = (flags & 0x02U) != 0;
    flags = static_cast<std::uint8_t>(flags & ~0x02U);
    if (flags != 0U) { malformed(reader, "unknown function flag bits"); }

    const auto acquire_count = readLimitedCount(reader, kMaximumAcquires, limits_.max_list_elements, "acquires count");
    result.acquires.reserve(acquire_count);
    for (std::size_t index = 0; index < acquire_count; ++index) {
      result.acquires.push_back(readTableIndex(reader, "acquired struct definition index"));
    }
    if (!is_native) { result.code = parseCodeUnit(reader); }
    return result;
  }

  void parseTable(format::TableKind kind, BinaryReader &reader) {
    using format::TableKind;
    switch (kind) {
    case TableKind::ModuleHandles:
      parseEntries(reader, module_.module_handles, &ModuleParser::parseModuleHandle);
      break;
    case TableKind::StructHandles:
      parseEntries(reader, module_.struct_handles, &ModuleParser::parseStructHandle);
      break;
    case TableKind::FunctionHandles:
      parseEntries(reader, module_.function_handles, &ModuleParser::parseFunctionHandle);
      break;
    case TableKind::FunctionInstantiations:
      parseEntries(reader, module_.function_instantiations, &ModuleParser::parseFunctionInstantiation);
      break;
    case TableKind::Signatures:
      parseEntries(reader, module_.signatures, &ModuleParser::parseSignature);
      break;
    case TableKind::Constants:
      parseEntries(reader, module_.constants, &ModuleParser::parseConstant);
      break;
    case TableKind::Identifiers:
      parseEntries(reader, module_.identifiers, &ModuleParser::parseIdentifier);
      break;
    case TableKind::AddressIdentifiers:
      parseEntries(reader, module_.addresses, &ModuleParser::parseAddress);
      break;
    case TableKind::StructDefinitions:
      parseEntries(reader, module_.struct_definitions, &ModuleParser::parseStructDefinition);
      break;
    case TableKind::StructDefinitionInstantiations:
      parseEntries(reader, module_.struct_definition_instantiations, &ModuleParser::parseStructDefinitionInstantiation);
      break;
    case TableKind::FunctionDefinitions:
      parseEntries(reader, module_.function_definitions, &ModuleParser::parseFunctionDefinition);
      break;
    case TableKind::FieldHandles:
      parseEntries(reader, module_.field_handles, &ModuleParser::parseFieldHandle);
      break;
    case TableKind::FieldInstantiations:
      parseEntries(reader, module_.field_instantiations, &ModuleParser::parseFieldInstantiation);
      break;
    case TableKind::FriendDeclarations:
      parseEntries(reader, module_.friends, &ModuleParser::parseModuleHandle);
      break;
    case TableKind::Metadata:
      parseEntries(reader, module_.metadata, &ModuleParser::parseMetadata);
      break;
    case TableKind::VariantFieldHandles:
      parseEntries(reader, module_.variant_field_handles, &ModuleParser::parseVariantFieldHandle);
      break;
    case TableKind::VariantFieldInstantiations:
      parseEntries(reader, module_.variant_field_instantiations, &ModuleParser::parseVariantFieldInstantiation);
      break;
    case TableKind::StructVariantHandles:
      parseEntries(reader, module_.struct_variant_handles, &ModuleParser::parseStructVariantHandle);
      break;
    case TableKind::StructVariantInstantiations:
      parseEntries(reader, module_.struct_variant_instantiations, &ModuleParser::parseStructVariantInstantiation);
      break;
    }
  }

  std::span<const std::uint8_t> bytes_;
  BinaryEnvelope envelope_;
  const ParserLimits &limits_;
  mutable std::size_t total_type_nodes_ = 0;
  mutable std::size_t total_instructions_ = 0;
  Module module_;
};

} // namespace

Module loadModule(std::span<const std::uint8_t> bytes, const ParserLimits &limits) {
  return ModuleParser(bytes, parseEnvelope(bytes, BinaryKind::Module, limits), limits).parse();
}

Script loadScript(std::span<const std::uint8_t> bytes, const ParserLimits &limits) {
  return ModuleParser(bytes, parseEnvelope(bytes, BinaryKind::Script, limits), limits).parseScript();
}

} // namespace movescape
