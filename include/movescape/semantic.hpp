#pragma once

#include "movescape/module.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace movescape {

using SemanticIndex = std::size_t;

struct ModuleSymbol {
  TableIndex handle = 0;
  Address address{};
  std::string name;
  std::string source_name;
  std::string qualified_name;
  bool is_self = false;
};

struct StructSymbol {
  TableIndex handle = 0;
  TableIndex module = 0;
  std::string name;
  std::string source_name;
  std::string qualified_name;
  std::optional<SemanticIndex> definition;
};

struct FunctionSymbol {
  TableIndex handle = 0;
  TableIndex module = 0;
  std::string name;
  std::string source_name;
  std::string qualified_name;
  std::optional<SemanticIndex> definition;
};

struct VariantSymbol {
  SemanticIndex definition = 0;
  std::uint16_t variant = 0;
  SemanticIndex structure = 0;
  std::string name;
  std::string source_name;
  std::string qualified_name;
};

struct FieldSymbol {
  SemanticIndex definition = 0;
  std::optional<std::uint16_t> variant;
  std::uint16_t field = 0;
  SemanticIndex structure = 0;
  std::string name;
  std::string source_name;
  std::string qualified_name;
  Type type;
};

struct ResolvedStructInstantiation {
  SemanticIndex definition = 0;
  SemanticIndex structure = 0;
  TableIndex signature = 0;
  Signature type_arguments;
};

struct ResolvedFunctionInstantiation {
  TableIndex handle = 0;
  SemanticIndex function = 0;
  TableIndex signature = 0;
  Signature type_arguments;
};

struct ResolvedFieldInstantiation {
  TableIndex handle = 0;
  SemanticIndex field = 0;
  TableIndex signature = 0;
  Signature type_arguments;
};

struct ResolvedVariantFieldInstantiation {
  TableIndex handle = 0;
  std::vector<SemanticIndex> fields;
  TableIndex signature = 0;
  Signature type_arguments;
};

struct ResolvedStructVariantInstantiation {
  TableIndex handle = 0;
  SemanticIndex variant = 0;
  TableIndex signature = 0;
  Signature type_arguments;
};

enum class CallSiteKind {
  Direct,
  Generic,
  ClosureCreation,
  GenericClosureCreation,
  ClosureInvocation,
};

struct CallSite {
  SemanticIndex caller_definition = 0;
  std::size_t instruction = 0;
  CallSiteKind kind = CallSiteKind::Direct;
  std::optional<TableIndex> callee_handle;
  std::optional<SemanticIndex> callee_definition;
  std::optional<SemanticIndex> instantiation;
  std::optional<TableIndex> closure_signature;
  Signature type_arguments;
};

// A resolved, owning layer over Module. Vectors for modules, structures, and
// functions are aligned with their corresponding serialized handle tables.
// Flattened field and variant vectors are addressed through the explicit maps
// below, so clients never have to repeat table-offset arithmetic.
struct SemanticModel {
  std::vector<ModuleSymbol> modules;
  std::vector<StructSymbol> structures;
  std::vector<FunctionSymbol> functions;
  std::vector<VariantSymbol> variants;
  std::vector<FieldSymbol> fields;

  std::vector<std::optional<SemanticIndex>> struct_definition_by_handle;
  std::vector<std::optional<SemanticIndex>> function_definition_by_handle;
  std::vector<SemanticIndex> struct_handle_by_definition;
  std::vector<SemanticIndex> function_handle_by_definition;
  std::vector<std::vector<SemanticIndex>> variants_by_definition;
  std::vector<std::vector<SemanticIndex>> fields_by_definition;
  std::vector<SemanticIndex> field_handle_targets;
  std::vector<std::vector<SemanticIndex>> variant_field_handle_targets;
  std::vector<SemanticIndex> struct_variant_handle_targets;

  std::vector<ResolvedStructInstantiation> struct_instantiations;
  std::vector<ResolvedFunctionInstantiation> function_instantiations;
  std::vector<ResolvedFieldInstantiation> field_instantiations;
  std::vector<ResolvedVariantFieldInstantiation> variant_field_instantiations;
  std::vector<ResolvedStructVariantInstantiation> struct_variant_instantiations;

  std::vector<CallSite> call_sites;
  // Indexed by caller function-definition index. Only statically known calls
  // to definitions in this module are edges. Each target appears once, in
  // first-instruction order.
  std::vector<std::vector<SemanticIndex>> direct_callees;
};

// Validates the decoded module before resolving it. The returned model owns all
// names and type arguments and does not retain references into `module`.
[[nodiscard]] SemanticModel buildSemanticModel(const Module &module);
[[nodiscard]] std::string_view callSiteKindName(CallSiteKind kind) noexcept;
[[nodiscard]] std::string formatSemanticModel(const SemanticModel &model);

} // namespace movescape
