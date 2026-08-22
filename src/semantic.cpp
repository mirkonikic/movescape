#include "movescape/semantic.hpp"

#include "movescape/source_names.hpp"
#include "movescape/validator.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace movescape {

namespace {

[[nodiscard]] std::string addressName(const Address &address) {
  std::ostringstream out;
  out << "0x";
  bool started = false;
  for (const auto byte : address) {
    if (!started && byte == 0) {
      continue;
    }
    if (!started) {
      out << std::hex << static_cast<unsigned>(byte);
      started = true;
    } else {
      out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    }
  }
  if (!started) {
    out << '0';
  }
  return out.str();
}

void addDirectEdge(SemanticModel &model, SemanticIndex caller, std::optional<SemanticIndex> callee) {
  if (!callee.has_value()) {
    return;
  }
  auto &targets = model.direct_callees[caller];
  if (std::find(targets.begin(), targets.end(), *callee) == targets.end()) {
    targets.push_back(*callee);
  }
}

[[nodiscard]] CallSite resolvedCall(const SemanticModel &model, SemanticIndex caller, std::size_t instruction, CallSiteKind kind, TableIndex function_handle) {
  return CallSite{
      .caller_definition = caller,
      .instruction = instruction,
      .kind = kind,
      .callee_handle = function_handle,
      .callee_definition = model.function_definition_by_handle[function_handle],
      .instantiation = std::nullopt,
      .closure_signature = std::nullopt,
      .type_arguments = {},
  };
}

template <typename Items, typename Render> [[nodiscard]] std::string join(const Items &items, std::string_view separator, Render render) {
  std::ostringstream out;
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index != 0) {
      out << separator;
    }
    out << render(items[index]);
  }
  return out.str();
}

[[nodiscard]] std::string semanticTypeName(const SemanticModel &model, const Type &type) {
  switch (type.kind) {
  case TypeKind::Bool:
    return "bool";
  case TypeKind::U8:
    return "u8";
  case TypeKind::U16:
    return "u16";
  case TypeKind::U32:
    return "u32";
  case TypeKind::U64:
    return "u64";
  case TypeKind::U128:
    return "u128";
  case TypeKind::U256:
    return "u256";
  case TypeKind::I8:
    return "i8";
  case TypeKind::I16:
    return "i16";
  case TypeKind::I32:
    return "i32";
  case TypeKind::I64:
    return "i64";
  case TypeKind::I128:
    return "i128";
  case TypeKind::I256:
    return "i256";
  case TypeKind::Address:
    return "address";
  case TypeKind::Signer:
    return "signer";
  case TypeKind::Vector:
    return "vector<" + semanticTypeName(model, type.arguments[0]) + ">";
  case TypeKind::Reference:
    return "&" + semanticTypeName(model, type.arguments[0]);
  case TypeKind::MutableReference:
    return "&mut " + semanticTypeName(model, type.arguments[0]);
  case TypeKind::Struct:
    return model.structures[type.index].qualified_name;
  case TypeKind::StructInstantiation:
    return model.structures[type.index].qualified_name + "<" +
           join(type.arguments, ", ", [&](const Type &argument) { return semanticTypeName(model, argument); }) + ">";
  case TypeKind::TypeParameter:
    return "T" + std::to_string(type.index);
  case TypeKind::Function:
    return "|" + join(type.arguments, ", ", [&](const Type &argument) { return semanticTypeName(model, argument); }) + "|(" +
           join(type.results, ", ", [&](const Type &result) { return semanticTypeName(model, result); }) + ")";
  }
  return "<type>";
}

[[nodiscard]] std::string typeArguments(const SemanticModel &model, const Signature &arguments) {
  return "[" + join(arguments, ", ", [&](const Type &type) { return semanticTypeName(model, type); }) + "]";
}

} // namespace

SemanticModel buildSemanticModel(const Module &module) {
  validateModule(module);

  SemanticModel model;
  model.modules.reserve(module.module_handles.size());
  for (std::size_t index = 0; index < module.module_handles.size(); ++index) {
    const auto &handle = module.module_handles[index];
    const auto &address = module.addresses[handle.address];
    const auto &name = module.identifiers[handle.name];
    const auto qualified = addressName(address) + "::" + name;
    model.modules.push_back(ModuleSymbol{
        .handle = static_cast<TableIndex>(index),
        .address = address,
        .name = name,
        .source_name = makeMoveSourceIdentifier(name, "module", index),
        .qualified_name = qualified,
        .is_self = index == module.self_module_handle,
    });
  }

  model.struct_definition_by_handle.resize(module.struct_handles.size());
  model.struct_handle_by_definition.reserve(module.struct_definitions.size());
  for (std::size_t definition = 0; definition < module.struct_definitions.size(); ++definition) {
    const auto handle = module.struct_definitions[definition].handle;
    model.struct_definition_by_handle[handle] = definition;
    model.struct_handle_by_definition.push_back(handle);
  }
  model.structures.reserve(module.struct_handles.size());
  for (std::size_t index = 0; index < module.struct_handles.size(); ++index) {
    const auto &handle = module.struct_handles[index];
    const auto &name = module.identifiers[handle.name];
    model.structures.push_back(StructSymbol{
        .handle = static_cast<TableIndex>(index),
        .module = handle.module,
        .name = name,
        .source_name = makeMoveSourceIdentifier(name, "struct", index),
        .qualified_name = model.modules[handle.module].qualified_name + "::" + name,
        .definition = model.struct_definition_by_handle[index],
    });
  }

  model.function_definition_by_handle.resize(module.function_handles.size());
  model.function_handle_by_definition.reserve(module.function_definitions.size());
  for (std::size_t definition = 0; definition < module.function_definitions.size(); ++definition) {
    const auto handle = module.function_definitions[definition].handle;
    model.function_definition_by_handle[handle] = definition;
    model.function_handle_by_definition.push_back(handle);
  }
  model.functions.reserve(module.function_handles.size());
  for (std::size_t index = 0; index < module.function_handles.size(); ++index) {
    const auto &handle = module.function_handles[index];
    const auto &name = module.identifiers[handle.name];
    model.functions.push_back(FunctionSymbol{
        .handle = static_cast<TableIndex>(index),
        .module = handle.module,
        .name = name,
        .source_name = makeMoveSourceIdentifier(name, "function", index),
        .qualified_name = model.modules[handle.module].qualified_name + "::" + name,
        .definition = model.function_definition_by_handle[index],
    });
  }

  model.variants_by_definition.resize(module.struct_definitions.size());
  model.fields_by_definition.resize(module.struct_definitions.size());
  for (std::size_t definition_index = 0; definition_index < module.struct_definitions.size(); ++definition_index) {
    const auto &definition = module.struct_definitions[definition_index];
    const auto structure = static_cast<SemanticIndex>(definition.handle);
    const auto &structure_name = model.structures[structure].qualified_name;
    auto &definition_fields = model.fields_by_definition[definition_index];

    for (std::size_t field_index = 0; field_index < definition.fields.size(); ++field_index) {
      const auto &field = definition.fields[field_index];
      const auto &name = module.identifiers[field.name];
      definition_fields.push_back(model.fields.size());
      model.fields.push_back(FieldSymbol{
          .definition = definition_index,
          .variant = std::nullopt,
          .field = static_cast<std::uint16_t>(field_index),
          .structure = structure,
          .name = name,
          .source_name = makeMoveSourceIdentifier(name, "field", field_index),
          .qualified_name = structure_name + "::" + name,
          .type = field.type,
      });
    }

    auto &definition_variants = model.variants_by_definition[definition_index];
    for (std::size_t variant_index = 0; variant_index < definition.variants.size(); ++variant_index) {
      const auto &variant = definition.variants[variant_index];
      const auto &variant_name = module.identifiers[variant.name];
      const auto variant_symbol = model.variants.size();
      definition_variants.push_back(variant_symbol);
      model.variants.push_back(VariantSymbol{
          .definition = definition_index,
          .variant = static_cast<std::uint16_t>(variant_index),
          .structure = structure,
          .name = variant_name,
          .source_name = makeMoveSourceIdentifier(variant_name, "variant", variant_index),
          .qualified_name = structure_name + "::" + variant_name,
      });
      for (std::size_t field_index = 0; field_index < variant.fields.size(); ++field_index) {
        const auto &field = variant.fields[field_index];
        const auto &field_name = module.identifiers[field.name];
        definition_fields.push_back(model.fields.size());
        model.fields.push_back(FieldSymbol{
            .definition = definition_index,
            .variant = static_cast<std::uint16_t>(variant_index),
            .field = static_cast<std::uint16_t>(field_index),
            .structure = structure,
            .name = field_name,
            .source_name = makeMoveSourceIdentifier(field_name, "field", field_index),
            .qualified_name = structure_name + "::" + variant_name + "::" + field_name,
            .type = field.type,
        });
      }
    }
  }

  model.field_handle_targets.reserve(module.field_handles.size());
  for (const auto &handle : module.field_handles) {
    const auto &targets = model.fields_by_definition[handle.owner];
    model.field_handle_targets.push_back(targets[handle.field]);
  }

  model.variant_field_handle_targets.reserve(module.variant_field_handles.size());
  for (const auto &handle : module.variant_field_handles) {
    std::vector<SemanticIndex> targets;
    targets.reserve(handle.variants.size());
    const auto &definition = module.struct_definitions[handle.owner];
    std::vector<std::size_t> variant_field_bases;
    variant_field_bases.reserve(definition.variants.size());
    std::size_t variant_field_base = definition.fields.size();
    for (std::size_t variant_index = 0; variant_index < definition.variants.size(); ++variant_index) {
      variant_field_bases.push_back(variant_field_base);
      variant_field_base += definition.variants[variant_index].fields.size();
    }
    for (const auto variant : handle.variants) {
      targets.push_back(model.fields_by_definition[handle.owner][variant_field_bases[variant] + handle.field]);
    }
    model.variant_field_handle_targets.push_back(std::move(targets));
  }

  model.struct_variant_handle_targets.reserve(module.struct_variant_handles.size());
  for (const auto &handle : module.struct_variant_handles) {
    model.struct_variant_handle_targets.push_back(model.variants_by_definition[handle.definition][handle.variant]);
  }

  model.struct_instantiations.reserve(module.struct_definition_instantiations.size());
  for (const auto &instantiation : module.struct_definition_instantiations) {
    const auto &definition = module.struct_definitions[instantiation.definition];
    model.struct_instantiations.push_back(ResolvedStructInstantiation{
        .definition = instantiation.definition,
        .structure = definition.handle,
        .signature = instantiation.type_parameters,
        .type_arguments = module.signatures[instantiation.type_parameters],
    });
  }
  model.function_instantiations.reserve(module.function_instantiations.size());
  for (const auto &instantiation : module.function_instantiations) {
    model.function_instantiations.push_back(ResolvedFunctionInstantiation{
        .handle = instantiation.handle,
        .function = instantiation.handle,
        .signature = instantiation.type_parameters,
        .type_arguments = module.signatures[instantiation.type_parameters],
    });
  }
  model.field_instantiations.reserve(module.field_instantiations.size());
  for (const auto &instantiation : module.field_instantiations) {
    model.field_instantiations.push_back(ResolvedFieldInstantiation{
        .handle = instantiation.handle,
        .field = model.field_handle_targets[instantiation.handle],
        .signature = instantiation.type_parameters,
        .type_arguments = module.signatures[instantiation.type_parameters],
    });
  }
  model.variant_field_instantiations.reserve(module.variant_field_instantiations.size());
  for (const auto &instantiation : module.variant_field_instantiations) {
    model.variant_field_instantiations.push_back(ResolvedVariantFieldInstantiation{
        .handle = instantiation.handle,
        .fields = model.variant_field_handle_targets[instantiation.handle],
        .signature = instantiation.type_parameters,
        .type_arguments = module.signatures[instantiation.type_parameters],
    });
  }
  model.struct_variant_instantiations.reserve(module.struct_variant_instantiations.size());
  for (const auto &instantiation : module.struct_variant_instantiations) {
    model.struct_variant_instantiations.push_back(ResolvedStructVariantInstantiation{
        .handle = instantiation.handle,
        .variant = model.struct_variant_handle_targets[instantiation.handle],
        .signature = instantiation.type_parameters,
        .type_arguments = module.signatures[instantiation.type_parameters],
    });
  }

  model.direct_callees.resize(module.function_definitions.size());
  for (std::size_t caller = 0; caller < module.function_definitions.size(); ++caller) {
    const auto &definition = module.function_definitions[caller];
    if (!definition.code.has_value()) {
      continue;
    }
    const auto &code = definition.code->code;
    for (std::size_t pc = 0; pc < code.size(); ++pc) {
      const auto &instruction = code[pc];
      switch (instruction.opcode) {
      case Opcode::Call:
      case Opcode::PackClosure: {
        const auto handle = static_cast<TableIndex>(instruction.operands[0]);
        const auto kind = instruction.opcode == Opcode::Call ? CallSiteKind::Direct : CallSiteKind::ClosureCreation;
        auto site = resolvedCall(model, caller, pc, kind, handle);
        if (kind == CallSiteKind::Direct) {
          addDirectEdge(model, caller, site.callee_definition);
        }
        model.call_sites.push_back(std::move(site));
        break;
      }
      case Opcode::CallGeneric:
      case Opcode::PackClosureGeneric: {
        const auto instantiation = static_cast<SemanticIndex>(instruction.operands[0]);
        const auto &resolved = model.function_instantiations[instantiation];
        const auto kind = instruction.opcode == Opcode::CallGeneric ? CallSiteKind::Generic : CallSiteKind::GenericClosureCreation;
        auto site = resolvedCall(model, caller, pc, kind, resolved.handle);
        site.instantiation = instantiation;
        site.type_arguments = resolved.type_arguments;
        if (kind == CallSiteKind::Generic) {
          addDirectEdge(model, caller, site.callee_definition);
        }
        model.call_sites.push_back(std::move(site));
        break;
      }
      case Opcode::CallClosure:
        model.call_sites.push_back(CallSite{
            .caller_definition = caller,
            .instruction = pc,
            .kind = CallSiteKind::ClosureInvocation,
            .callee_handle = std::nullopt,
            .callee_definition = std::nullopt,
            .instantiation = std::nullopt,
            .closure_signature = static_cast<TableIndex>(instruction.operands[0]),
            .type_arguments = {},
        });
        break;
      default:
        break;
      }
    }
  }

  return model;
}

std::string_view callSiteKindName(CallSiteKind kind) noexcept {
  switch (kind) {
  case CallSiteKind::Direct:
    return "direct";
  case CallSiteKind::Generic:
    return "generic";
  case CallSiteKind::ClosureCreation:
    return "closure-create";
  case CallSiteKind::GenericClosureCreation:
    return "generic-closure-create";
  case CallSiteKind::ClosureInvocation:
    return "closure-invoke";
  }
  return "unknown";
}

std::string formatSemanticModel(const SemanticModel &model) {
  std::ostringstream out;
  out << "semantic-module ";
  const auto self = std::find_if(model.modules.begin(), model.modules.end(), [](const ModuleSymbol &module) { return module.is_self; });
  out << (self == model.modules.end() ? "<none>" : self->qualified_name) << '\n';

  out << "modules:\n";
  for (std::size_t index = 0; index < model.modules.size(); ++index) {
    const auto &module = model.modules[index];
    out << "  module#" << index << ' ' << module.qualified_name;
    if (module.is_self) {
      out << " [self]";
    }
    out << " source=" << module.source_name << '\n';
  }

  out << "structures:\n";
  for (std::size_t index = 0; index < model.structures.size(); ++index) {
    const auto &structure = model.structures[index];
    out << "  struct#" << index << ' ' << structure.qualified_name;
    if (structure.definition.has_value()) {
      out << " definition#" << *structure.definition;
    } else {
      out << " external";
    }
    out << " source=" << structure.source_name << '\n';
  }

  out << "variants:\n";
  for (std::size_t index = 0; index < model.variants.size(); ++index) {
    const auto &variant = model.variants[index];
    out << "  variant#" << index << ' ' << variant.qualified_name << " definition#" << variant.definition << " offset=" << variant.variant
        << " source=" << variant.source_name << '\n';
  }

  out << "fields:\n";
  for (std::size_t index = 0; index < model.fields.size(); ++index) {
    const auto &field = model.fields[index];
    out << "  field#" << index << ' ' << field.qualified_name << " definition#" << field.definition;
    if (field.variant.has_value()) {
      out << " variant=" << *field.variant;
    }
    out << " offset=" << field.field << " type=" << semanticTypeName(model, field.type) << " source=" << field.source_name << '\n';
  }

  out << "functions:\n";
  for (std::size_t index = 0; index < model.functions.size(); ++index) {
    const auto &function = model.functions[index];
    out << "  function#" << index << ' ' << function.qualified_name;
    if (function.definition.has_value()) {
      out << " definition#" << *function.definition;
    } else {
      out << " external";
    }
    out << " source=" << function.source_name << '\n';
  }

  out << "instantiations:\n";
  for (std::size_t index = 0; index < model.struct_instantiations.size(); ++index) {
    const auto &instantiation = model.struct_instantiations[index];
    out << "  struct-inst#" << index << " struct#" << instantiation.structure << " definition#" << instantiation.definition << " signature#"
        << instantiation.signature << " args=" << typeArguments(model, instantiation.type_arguments) << '\n';
  }
  for (std::size_t index = 0; index < model.function_instantiations.size(); ++index) {
    const auto &instantiation = model.function_instantiations[index];
    out << "  function-inst#" << index << " function#" << instantiation.function << " signature#" << instantiation.signature
        << " args=" << typeArguments(model, instantiation.type_arguments) << '\n';
  }
  for (std::size_t index = 0; index < model.field_instantiations.size(); ++index) {
    const auto &instantiation = model.field_instantiations[index];
    out << "  field-inst#" << index << " field#" << instantiation.field << " signature#" << instantiation.signature
        << " args=" << typeArguments(model, instantiation.type_arguments) << '\n';
  }
  for (std::size_t index = 0; index < model.variant_field_instantiations.size(); ++index) {
    const auto &instantiation = model.variant_field_instantiations[index];
    out << "  variant-field-inst#" << index << " fields=["
        << join(instantiation.fields, ", ", [](SemanticIndex field) { return "field#" + std::to_string(field); }) << "] signature#" << instantiation.signature
        << " args=" << typeArguments(model, instantiation.type_arguments) << '\n';
  }
  for (std::size_t index = 0; index < model.struct_variant_instantiations.size(); ++index) {
    const auto &instantiation = model.struct_variant_instantiations[index];
    out << "  struct-variant-inst#" << index << " variant#" << instantiation.variant << " signature#" << instantiation.signature
        << " args=" << typeArguments(model, instantiation.type_arguments) << '\n';
  }

  out << "call-sites:\n";
  for (const auto &site : model.call_sites) {
    const auto caller_handle = model.function_handle_by_definition[site.caller_definition];
    out << "  definition#" << site.caller_definition << ' ' << model.functions[caller_handle].qualified_name << " @" << site.instruction << ' '
        << callSiteKindName(site.kind);
    if (site.callee_handle.has_value()) {
      out << " -> function#" << *site.callee_handle << ' ' << model.functions[*site.callee_handle].qualified_name;
      if (site.callee_definition.has_value()) {
        out << " definition#" << *site.callee_definition;
      } else {
        out << " external";
      }
    } else {
      out << " signature#" << *site.closure_signature << " target=unknown";
    }
    if (site.instantiation.has_value()) {
      out << " instantiation#" << *site.instantiation << " args=" << typeArguments(model, site.type_arguments);
    }
    out << '\n';
  }

  out << "direct-call-graph:\n";
  for (std::size_t caller = 0; caller < model.direct_callees.size(); ++caller) {
    const auto caller_handle = model.function_handle_by_definition[caller];
    out << "  definition#" << caller << ' ' << model.functions[caller_handle].qualified_name << " -> ";
    if (model.direct_callees[caller].empty()) {
      out << "[]\n";
      continue;
    }
    out << '[' << join(model.direct_callees[caller], ", ", [&](SemanticIndex callee) {
      const auto handle = model.function_handle_by_definition[callee];
      return "definition#" + std::to_string(callee) + " " + model.functions[handle].qualified_name;
    }) << "]\n";
  }
  return out.str();
}

} // namespace movescape
