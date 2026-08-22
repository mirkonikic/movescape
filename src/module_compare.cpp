#include "movescape/module_compare.hpp"

#include "movescape/semantic.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace movescape {

namespace {

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

[[nodiscard]] std::string abilities(AbilitySet set) {
  std::vector<std::string_view> names;
  if (set.has(AbilitySet::Copy)) {
    names.emplace_back("copy");
  }
  if (set.has(AbilitySet::Drop)) {
    names.emplace_back("drop");
  }
  if (set.has(AbilitySet::Store)) {
    names.emplace_back("store");
  }
  if (set.has(AbilitySet::Key)) {
    names.emplace_back("key");
  }
  return "[" + join(names, ",", [](std::string_view name) { return name; }) + "]";
}

[[nodiscard]] std::string typeName(const SemanticModel &model, const Type &type) {
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
    return "vector<" + typeName(model, type.arguments[0]) + ">";
  case TypeKind::Reference:
    return "&" + typeName(model, type.arguments[0]);
  case TypeKind::MutableReference:
    return "&mut " + typeName(model, type.arguments[0]);
  case TypeKind::Struct:
    return model.structures[type.index].qualified_name;
  case TypeKind::StructInstantiation:
    return model.structures[type.index].qualified_name + "<" + join(type.arguments, ",", [&](const Type &argument) { return typeName(model, argument); }) + ">";
  case TypeKind::TypeParameter:
    return "T" + std::to_string(type.index);
  case TypeKind::Function:
    return "fun" + abilities(type.abilities) + "(" + join(type.arguments, ",", [&](const Type &argument) { return typeName(model, argument); }) + ")->(" +
           join(type.results, ",", [&](const Type &result) { return typeName(model, result); }) + ")";
  }
  return "<type>";
}

[[nodiscard]] std::string signature(const SemanticModel &model, const Signature &types) {
  return "(" + join(types, ",", [&](const Type &type) { return typeName(model, type); }) + ")";
}

[[nodiscard]] std::string visibilityName(Visibility visibility) {
  switch (visibility) {
  case Visibility::Private:
    return "private";
  case Visibility::Public:
    return "public";
  case Visibility::Friend:
    return "friend";
  }
  return "unknown";
}

[[nodiscard]] std::string functionAttribute(const FunctionAttribute &attribute) {
  std::string result;
  switch (attribute.kind) {
  case FunctionAttributeKind::Persistent:
    result = "persistent";
    break;
  case FunctionAttributeKind::ModuleLock:
    result = "module-lock";
    break;
  case FunctionAttributeKind::Pack:
    result = "pack";
    break;
  case FunctionAttributeKind::PackVariant:
    result = "pack-variant";
    break;
  case FunctionAttributeKind::Unpack:
    result = "unpack";
    break;
  case FunctionAttributeKind::UnpackVariant:
    result = "unpack-variant";
    break;
  case FunctionAttributeKind::TestVariant:
    result = "test-variant";
    break;
  case FunctionAttributeKind::BorrowFieldImmutable:
    result = "borrow-field-immutable";
    break;
  case FunctionAttributeKind::BorrowFieldMutable:
    result = "borrow-field-mutable";
    break;
  }
  if (attribute.value.has_value()) {
    result += "(" + std::to_string(*attribute.value) + ")";
  }
  return result;
}

[[nodiscard]] std::string moduleHandleName(const Module &module, const ModuleHandle &handle) {
  return addressName(module.addresses[handle.address]) + "::" + module.identifiers[handle.name];
}

[[nodiscard]] std::string resourceSpecifier(const Module &module, const SemanticModel &model, const ResourceSpecifier &specifier) {
  switch (specifier.kind) {
  case ResourceSpecifierKind::Any:
    return "any";
  case ResourceSpecifierKind::DeclaredAtAddress:
    return "address(" + addressName(module.addresses[specifier.primary]) + ")";
  case ResourceSpecifierKind::DeclaredInModule:
    return "module(" + model.modules[specifier.primary].qualified_name + ")";
  case ResourceSpecifierKind::Resource:
    return "resource(" + model.structures[specifier.primary].qualified_name + ")";
  case ResourceSpecifierKind::ResourceInstantiation:
    return "resource(" + model.structures[specifier.primary].qualified_name + signature(model, module.signatures[specifier.signature]) + ")";
  }
  return "unknown";
}

[[nodiscard]] std::string addressSpecifier(const Module &module, const SemanticModel &model, const AddressSpecifier &specifier) {
  std::string result;
  switch (specifier.kind) {
  case AddressSpecifierKind::Any:
    result = "any";
    break;
  case AddressSpecifierKind::Literal:
    result = "literal(" + addressName(module.addresses[specifier.value]) + ")";
    break;
  case AddressSpecifierKind::Parameter:
    result = "parameter(" + std::to_string(specifier.value) + ")";
    break;
  }
  if (specifier.function_instantiation.has_value()) {
    const auto &instantiation = model.function_instantiations[*specifier.function_instantiation];
    result += " via " + model.functions[instantiation.function].qualified_name + signature(model, instantiation.type_arguments);
  }
  return result;
}

[[nodiscard]] std::string accessSpecifier(const Module &module, const SemanticModel &model, const AccessSpecifier &specifier) {
  return std::string(specifier.negated ? "not " : "") + (specifier.kind == AccessKind::Reads ? "reads " : "writes ") +
         resourceSpecifier(module, model, specifier.resource) + " at " + addressSpecifier(module, model, specifier.address);
}

[[nodiscard]] std::string structValue(const Module &module, const SemanticModel &model, SemanticIndex definition_index) {
  const auto &definition = module.struct_definitions[definition_index];
  const auto &handle = module.struct_handles[definition.handle];
  std::ostringstream out;
  out << "abilities=" << abilities(handle.abilities) << " type-parameters=[" << join(handle.type_parameters, ",", [](const StructTypeParameter &parameter) {
    return std::string(parameter.is_phantom ? "phantom " : "") + abilities(parameter.constraints);
  }) << "] ";
  switch (definition.field_kind) {
  case StructFieldKind::Native:
    out << "native";
    break;
  case StructFieldKind::Declared:
    out << "fields=["
        << join(definition.fields, ",", [&](const FieldDefinition &field) { return module.identifiers[field.name] + ":" + typeName(model, field.type); })
        << ']';
    break;
  case StructFieldKind::Variants:
    out << "variants=[" << join(definition.variants, ",", [&](const VariantDefinition &variant) {
      return module.identifiers[variant.name] + "{" +
             join(variant.fields, ",", [&](const FieldDefinition &field) { return module.identifiers[field.name] + ":" + typeName(model, field.type); }) + "}";
    }) << ']';
    break;
  }
  return out.str();
}

[[nodiscard]] std::string functionValue(const Module &module, const SemanticModel &model, SemanticIndex definition_index) {
  const auto &definition = module.function_definitions[definition_index];
  const auto &handle = module.function_handles[definition.handle];
  std::ostringstream out;
  out << "visibility=" << visibilityName(definition.visibility) << " entry=" << (definition.is_entry ? "true" : "false")
      << " native=" << (!definition.code.has_value() ? "true" : "false") << " type-parameters=["
      << join(handle.type_parameters, ",", [](AbilitySet constraints) { return abilities(constraints); })
      << "] parameters=" << signature(model, module.signatures[handle.parameters]) << " returns=" << signature(model, module.signatures[handle.returns])
      << " acquires=["
      << join(definition.acquires, ",",
              [&](TableIndex acquired) {
                const auto struct_handle = model.struct_handle_by_definition[acquired];
                return model.structures[struct_handle].qualified_name;
              })
      << "] attributes=[" << join(handle.attributes, ",", functionAttribute) << ']';
  if (handle.access_specifiers.has_value()) {
    out << " access=[" << join(*handle.access_specifiers, ",", [&](const AccessSpecifier &specifier) { return accessSpecifier(module, model, specifier); })
        << ']';
  } else {
    out << " access=unspecified";
  }
  return out.str();
}

[[nodiscard]] std::map<std::string, std::string> declarationMap(const NormalizedModuleInterface &interface) {
  std::map<std::string, std::string> result;
  result.emplace("module", interface.module_name);
  for (const auto &declaration : interface.declarations) {
    result.emplace(declaration.identity, declaration.value);
  }
  return result;
}

} // namespace

NormalizedModuleInterface normalizeModuleInterface(const Module &module) {
  const auto model = buildSemanticModel(module);
  NormalizedModuleInterface result;
  result.module_name = model.modules[module.self_module_handle].qualified_name;

  for (const auto &friend_handle : module.friends) {
    const auto name = moduleHandleName(module, friend_handle);
    result.declarations.push_back({
        .identity = "friend " + name,
        .value = name,
    });
  }
  for (std::size_t definition = 0; definition < module.struct_definitions.size(); ++definition) {
    const auto handle = model.struct_handle_by_definition[definition];
    const auto &name = model.structures[handle].qualified_name;
    result.declarations.push_back({
        .identity = "struct " + name,
        .value = structValue(module, model, definition),
    });
  }
  for (std::size_t definition = 0; definition < module.function_definitions.size(); ++definition) {
    const auto handle = model.function_handle_by_definition[definition];
    const auto &name = model.functions[handle].qualified_name;
    result.declarations.push_back({
        .identity = "function " + name,
        .value = functionValue(module, model, definition),
    });
  }
  std::sort(result.declarations.begin(), result.declarations.end(), [](const NormalizedDeclaration &left, const NormalizedDeclaration &right) {
    if (left.identity != right.identity) {
      return left.identity < right.identity;
    }
    return left.value < right.value;
  });
  return result;
}

ModuleInterfaceComparison compareModuleInterfaces(const Module &reference, const Module &candidate) {
  const auto normalized_reference = normalizeModuleInterface(reference);
  const auto normalized_candidate = normalizeModuleInterface(candidate);
  const auto reference_declarations = declarationMap(normalized_reference);
  const auto candidate_declarations = declarationMap(normalized_candidate);

  ModuleInterfaceComparison result;
  for (const auto &[identity, value] : reference_declarations) {
    const auto found = candidate_declarations.find(identity);
    if (found == candidate_declarations.end()) {
      result.differences.push_back({
          .identity = identity,
          .reference = value,
          .candidate = std::nullopt,
      });
    } else if (found->second != value) {
      result.differences.push_back({
          .identity = identity,
          .reference = value,
          .candidate = found->second,
      });
    }
  }
  for (const auto &[identity, value] : candidate_declarations) {
    if (!reference_declarations.contains(identity)) {
      result.differences.push_back({
          .identity = identity,
          .reference = std::nullopt,
          .candidate = value,
      });
    }
  }
  std::sort(result.differences.begin(), result.differences.end(),
            [](const InterfaceDifference &left, const InterfaceDifference &right) { return left.identity < right.identity; });
  return result;
}

std::string formatModuleInterface(const NormalizedModuleInterface &interface) {
  std::ostringstream out;
  out << "module " << interface.module_name << '\n';
  for (const auto &declaration : interface.declarations) {
    out << declaration.identity << " = " << declaration.value << '\n';
  }
  return out.str();
}

std::string formatModuleInterfaceComparison(const ModuleInterfaceComparison &comparison) {
  if (comparison.equivalent()) {
    return "module interfaces are equivalent\n";
  }
  std::ostringstream out;
  out << "module interfaces differ (" << comparison.differences.size() << ")\n";
  for (const auto &difference : comparison.differences) {
    out << "  " << difference.identity << '\n';
    out << "    reference: " << (difference.reference.has_value() ? *difference.reference : "<missing>") << '\n';
    out << "    candidate: " << (difference.candidate.has_value() ? *difference.candidate : "<missing>") << '\n';
  }
  return out.str();
}

} // namespace movescape
