#include "movescape/opcode.hpp"

#include <array>
#include <cstdlib>

namespace movescape {

namespace {

using OE = OperandEncoding;

constexpr std::array<OpcodeInfo, 104> kOpcodeInfo{{
    {Opcode::Pop, "Pop", OE::None, 1, false, false, false},
    {Opcode::Ret, "Ret", OE::None, 1, true, false, false},
    {Opcode::BrTrue, "BrTrue", OE::CodeOffset, 1, true, true, false},
    {Opcode::BrFalse, "BrFalse", OE::CodeOffset, 1, true, true, false},
    {Opcode::Branch, "Branch", OE::CodeOffset, 1, true, false, true},
    {Opcode::LdU64, "LdU64", OE::U64, 1, false, false, false},
    {Opcode::LdConst, "LdConst", OE::TableIndex, 1, false, false, false},
    {Opcode::LdTrue, "LdTrue", OE::None, 1, false, false, false},
    {Opcode::LdFalse, "LdFalse", OE::None, 1, false, false, false},
    {Opcode::CopyLoc, "CopyLoc", OE::LocalIndex, 1, false, false, false},
    {Opcode::MoveLoc, "MoveLoc", OE::LocalIndex, 1, false, false, false},
    {Opcode::StLoc, "StLoc", OE::LocalIndex, 1, false, false, false},
    {Opcode::MutBorrowLoc, "MutBorrowLoc", OE::LocalIndex, 1, false, false, false},
    {Opcode::ImmBorrowLoc, "ImmBorrowLoc", OE::LocalIndex, 1, false, false, false},
    {Opcode::MutBorrowField, "MutBorrowField", OE::TableIndex, 1, false, false, false},
    {Opcode::ImmBorrowField, "ImmBorrowField", OE::TableIndex, 1, false, false, false},
    {Opcode::Call, "Call", OE::TableIndex, 1, false, false, false},
    {Opcode::Pack, "Pack", OE::TableIndex, 1, false, false, false},
    {Opcode::Unpack, "Unpack", OE::TableIndex, 1, false, false, false},
    {Opcode::ReadRef, "ReadRef", OE::None, 1, false, false, false},
    {Opcode::WriteRef, "WriteRef", OE::None, 1, false, false, false},
    {Opcode::Add, "Add", OE::None, 1, false, false, false},
    {Opcode::Sub, "Sub", OE::None, 1, false, false, false},
    {Opcode::Mul, "Mul", OE::None, 1, false, false, false},
    {Opcode::Mod, "Mod", OE::None, 1, false, false, false},
    {Opcode::Div, "Div", OE::None, 1, false, false, false},
    {Opcode::BitOr, "BitOr", OE::None, 1, false, false, false},
    {Opcode::BitAnd, "BitAnd", OE::None, 1, false, false, false},
    {Opcode::Xor, "Xor", OE::None, 1, false, false, false},
    {Opcode::Or, "Or", OE::None, 1, false, false, false},
    {Opcode::And, "And", OE::None, 1, false, false, false},
    {Opcode::Not, "Not", OE::None, 1, false, false, false},
    {Opcode::Eq, "Eq", OE::None, 1, false, false, false},
    {Opcode::Neq, "Neq", OE::None, 1, false, false, false},
    {Opcode::Lt, "Lt", OE::None, 1, false, false, false},
    {Opcode::Gt, "Gt", OE::None, 1, false, false, false},
    {Opcode::Le, "Le", OE::None, 1, false, false, false},
    {Opcode::Ge, "Ge", OE::None, 1, false, false, false},
    {Opcode::Abort, "Abort", OE::None, 1, true, false, false},
    {Opcode::Nop, "Nop", OE::None, 1, false, false, false},
    {Opcode::Exists, "Exists", OE::TableIndex, 1, false, false, false},
    {Opcode::MutBorrowGlobal, "MutBorrowGlobal", OE::TableIndex, 1, false, false, false},
    {Opcode::ImmBorrowGlobal, "ImmBorrowGlobal", OE::TableIndex, 1, false, false, false},
    {Opcode::MoveFrom, "MoveFrom", OE::TableIndex, 1, false, false, false},
    {Opcode::MoveTo, "MoveTo", OE::TableIndex, 1, false, false, false},
    {Opcode::FreezeRef, "FreezeRef", OE::None, 1, false, false, false},
    {Opcode::Shl, "Shl", OE::None, 1, false, false, false},
    {Opcode::Shr, "Shr", OE::None, 1, false, false, false},
    {Opcode::LdU8, "LdU8", OE::U8, 1, false, false, false},
    {Opcode::LdU128, "LdU128", OE::U128, 1, false, false, false},
    {Opcode::CastU8, "CastU8", OE::None, 1, false, false, false},
    {Opcode::CastU64, "CastU64", OE::None, 1, false, false, false},
    {Opcode::CastU128, "CastU128", OE::None, 1, false, false, false},
    {Opcode::MutBorrowFieldGeneric, "MutBorrowFieldGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::ImmBorrowFieldGeneric, "ImmBorrowFieldGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::CallGeneric, "CallGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::PackGeneric, "PackGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::UnpackGeneric, "UnpackGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::ExistsGeneric, "ExistsGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::MutBorrowGlobalGeneric, "MutBorrowGlobalGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::ImmBorrowGlobalGeneric, "ImmBorrowGlobalGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::MoveFromGeneric, "MoveFromGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::MoveToGeneric, "MoveToGeneric", OE::TableIndex, 1, false, false, false},
    {Opcode::VecPack, "VecPack", OE::SignatureAndU64, 4, false, false, false},
    {Opcode::VecLen, "VecLen", OE::TableIndex, 4, false, false, false},
    {Opcode::VecImmBorrow, "VecImmBorrow", OE::TableIndex, 4, false, false, false},
    {Opcode::VecMutBorrow, "VecMutBorrow", OE::TableIndex, 4, false, false, false},
    {Opcode::VecPushBack, "VecPushBack", OE::TableIndex, 4, false, false, false},
    {Opcode::VecPopBack, "VecPopBack", OE::TableIndex, 4, false, false, false},
    {Opcode::VecUnpack, "VecUnpack", OE::SignatureAndU64, 4, false, false, false},
    {Opcode::VecSwap, "VecSwap", OE::TableIndex, 4, false, false, false},
    {Opcode::LdU16, "LdU16", OE::U16, 6, false, false, false},
    {Opcode::LdU32, "LdU32", OE::U32, 6, false, false, false},
    {Opcode::LdU256, "LdU256", OE::U256, 6, false, false, false},
    {Opcode::CastU16, "CastU16", OE::None, 6, false, false, false},
    {Opcode::CastU32, "CastU32", OE::None, 6, false, false, false},
    {Opcode::CastU256, "CastU256", OE::None, 6, false, false, false},
    {Opcode::ImmBorrowVariantField, "ImmBorrowVariantField", OE::TableIndex, 7, false, false, false},
    {Opcode::MutBorrowVariantField, "MutBorrowVariantField", OE::TableIndex, 7, false, false, false},
    {Opcode::ImmBorrowVariantFieldGeneric, "ImmBorrowVariantFieldGeneric", OE::TableIndex, 7, false, false, false},
    {Opcode::MutBorrowVariantFieldGeneric, "MutBorrowVariantFieldGeneric", OE::TableIndex, 7, false, false, false},
    {Opcode::PackVariant, "PackVariant", OE::TableIndex, 7, false, false, false},
    {Opcode::PackVariantGeneric, "PackVariantGeneric", OE::TableIndex, 7, false, false, false},
    {Opcode::UnpackVariant, "UnpackVariant", OE::TableIndex, 7, false, false, false},
    {Opcode::UnpackVariantGeneric, "UnpackVariantGeneric", OE::TableIndex, 7, false, false, false},
    {Opcode::TestVariant, "TestVariant", OE::TableIndex, 7, false, false, false},
    {Opcode::TestVariantGeneric, "TestVariantGeneric", OE::TableIndex, 7, false, false, false},
    {Opcode::PackClosure, "PackClosure", OE::TableIndexAndClosureMask, 8, false, false, false},
    {Opcode::PackClosureGeneric, "PackClosureGeneric", OE::TableIndexAndClosureMask, 8, false, false, false},
    {Opcode::CallClosure, "CallClosure", OE::TableIndex, 8, false, false, false},
    {Opcode::LdI8, "LdI8", OE::I8, 9, false, false, false},
    {Opcode::LdI16, "LdI16", OE::I16, 9, false, false, false},
    {Opcode::LdI32, "LdI32", OE::I32, 9, false, false, false},
    {Opcode::LdI64, "LdI64", OE::I64, 9, false, false, false},
    {Opcode::LdI128, "LdI128", OE::I128, 9, false, false, false},
    {Opcode::LdI256, "LdI256", OE::I256, 9, false, false, false},
    {Opcode::CastI8, "CastI8", OE::None, 9, false, false, false},
    {Opcode::CastI16, "CastI16", OE::None, 9, false, false, false},
    {Opcode::CastI32, "CastI32", OE::None, 9, false, false, false},
    {Opcode::CastI64, "CastI64", OE::None, 9, false, false, false},
    {Opcode::CastI128, "CastI128", OE::None, 9, false, false, false},
    {Opcode::CastI256, "CastI256", OE::None, 9, false, false, false},
    {Opcode::Negate, "Negate", OE::None, 9, false, false, false},
    {Opcode::AbortMsg, "AbortMsg", OE::None, 10, true, false, false},
}};

} // namespace

std::optional<Opcode> opcodeFromByte(std::uint8_t value) noexcept {
  if (value < static_cast<std::uint8_t>(Opcode::Pop) || value > static_cast<std::uint8_t>(Opcode::AbortMsg)) {
    return std::nullopt;
  }
  return static_cast<Opcode>(value);
}

const OpcodeInfo &opcodeInfo(Opcode opcode) noexcept {
  const auto index = static_cast<std::size_t>(static_cast<std::uint8_t>(opcode) - 1U);
  if (index >= kOpcodeInfo.size()) {
    std::abort();
  }
  return kOpcodeInfo[index];
}

} // namespace movescape
