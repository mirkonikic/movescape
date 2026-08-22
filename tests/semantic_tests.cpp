#include "test.hpp"

#include "movescape/semantic.hpp"

#include <string>
#include <vector>

namespace {

movescape::Type type(movescape::TypeKind kind) { return movescape::Type{.kind = kind}; }

movescape::Module semanticFixture() {
  using movescape::Opcode;

  movescape::Module module;
  module.version = 10;
  module.identifiers = {
      "M",           // 0
      "Ext",         // 1
      "caller",      // 2
      "callee",      // 3
      "external",    // 4
      "generic_fun", // 5
      "E",           // 6
      "V0",          // 7
      "V1",          // 8
      "common",      // 9
      "Box",         // 10
      "value",       // 11
      "$__",         // 12
      "ExternalS",   // 13
  };

  movescape::Address self{};
  self.back() = 1;
  movescape::Address external{};
  external.back() = 2;
  module.addresses = {self, external};
  module.module_handles = {
      {.address = 0, .name = 0},
      {.address = 1, .name = 1},
  };
  module.self_module_handle = 0;

  movescape::Type closure_type{
      .kind = movescape::TypeKind::Function,
      .abilities = movescape::AbilitySet{static_cast<std::uint8_t>(movescape::AbilitySet::Copy | movescape::AbilitySet::Drop)},
  };
  module.signatures = {
      {},
      {type(movescape::TypeKind::U8)},
      {closure_type},
  };

  module.function_handles = {
      {.module = 0, .name = 2, .parameters = 0, .returns = 0},
      {.module = 0, .name = 3, .parameters = 0, .returns = 0},
      {.module = 1, .name = 4, .parameters = 0, .returns = 0},
      {.module = 0, .name = 5, .parameters = 0, .returns = 0, .type_parameters = {movescape::AbilitySet{}}},
      {.module = 0, .name = 12, .parameters = 0, .returns = 0},
  };
  module.function_instantiations = {
      {.handle = 3, .type_parameters = 1},
  };

  movescape::CodeUnit caller;
  caller.locals = 0;
  caller.code = {
      {.opcode = Opcode::Call, .operands = {1}},
      {.opcode = Opcode::Call, .operands = {2}},
      {.opcode = Opcode::CallGeneric, .operands = {0}},
      {.opcode = Opcode::PackClosure, .operands = {1, 0}},
      {.opcode = Opcode::Pop},
      {.opcode = Opcode::PackClosureGeneric, .operands = {0, 0}},
      {.opcode = Opcode::Pop},
      {.opcode = Opcode::PackClosure, .operands = {1, 0}},
      {.opcode = Opcode::CallClosure, .operands = {2}},
      {.opcode = Opcode::Call, .operands = {0}},
      {.opcode = Opcode::Call, .operands = {1}},
      {.opcode = Opcode::Ret},
  };
  module.function_definitions = {
      {.handle = 0, .visibility = movescape::Visibility::Public, .code = caller},
      {.handle = 1, .visibility = movescape::Visibility::Private},
      {.handle = 3, .visibility = movescape::Visibility::Private},
      {.handle = 4, .visibility = movescape::Visibility::Private},
  };

  movescape::StructHandle enum_handle{
      .module = 0,
      .name = 6,
  };
  movescape::StructHandle box_handle{
      .module = 0,
      .name = 10,
  };
  box_handle.type_parameters.push_back({});
  movescape::StructHandle external_handle{
      .module = 1,
      .name = 13,
  };
  module.struct_handles = {enum_handle, box_handle, external_handle};

  module.struct_definitions = {
      {.handle = 0,
       .field_kind = movescape::StructFieldKind::Variants,
       .variants =
           {
               {.name = 7, .fields = {{.name = 9, .type = type(movescape::TypeKind::U8)}}},
               {.name = 8, .fields = {{.name = 9, .type = type(movescape::TypeKind::U8)}}},
           }},
      {.handle = 1,
       .field_kind = movescape::StructFieldKind::Declared,
       .fields = {{.name = 11,
                   .type =
                       movescape::Type{
                           .kind = movescape::TypeKind::TypeParameter,
                           .index = 0,
                       }}}},
  };
  module.struct_definition_instantiations = {
      {.definition = 1, .type_parameters = 1},
  };
  module.field_handles = {
      {.owner = 1, .field = 0},
  };
  module.field_instantiations = {
      {.handle = 0, .type_parameters = 1},
  };
  module.variant_field_handles = {
      {.owner = 0, .variants = {1, 0}, .field = 0},
  };
  module.variant_field_instantiations = {
      {.handle = 0, .type_parameters = 0},
  };
  module.struct_variant_handles = {
      {.definition = 0, .variant = 1},
  };
  module.struct_variant_instantiations = {
      {.handle = 0, .type_parameters = 0},
  };

  return module;
}

} // namespace

TEST(semantic_model_resolves_internal_and_external_identities) {
  const auto model = movescape::buildSemanticModel(semanticFixture());

  REQUIRE_EQ(model.modules.size(), 2U);
  REQUIRE(model.modules[0].is_self);
  REQUIRE(!model.modules[1].is_self);
  REQUIRE_EQ(model.modules[0].qualified_name, std::string("0x1::M"));
  REQUIRE_EQ(model.modules[1].qualified_name, std::string("0x2::Ext"));

  REQUIRE_EQ(model.functions[0].qualified_name, std::string("0x1::M::caller"));
  REQUIRE_EQ(model.functions[2].qualified_name, std::string("0x2::Ext::external"));
  REQUIRE(model.functions[0].definition.has_value());
  REQUIRE(!model.functions[2].definition.has_value());
  REQUIRE_EQ(*model.functions[0].definition, 0U);
  REQUIRE_EQ(model.functions[4].source_name, std::string("function_4"));
  REQUIRE_EQ(model.function_handle_by_definition, (std::vector<movescape::SemanticIndex>{0, 1, 3, 4}));

  REQUIRE_EQ(model.structures[2].qualified_name, std::string("0x2::Ext::ExternalS"));
  REQUIRE(!model.structures[2].definition.has_value());
  REQUIRE_EQ(model.struct_handle_by_definition, (std::vector<movescape::SemanticIndex>{0, 1}));
}

TEST(semantic_model_resolves_fields_variants_and_generic_tables) {
  const auto model = movescape::buildSemanticModel(semanticFixture());

  REQUIRE_EQ(model.variants.size(), 2U);
  REQUIRE_EQ(model.fields.size(), 3U);
  REQUIRE_EQ(model.variants[1].qualified_name, std::string("0x1::M::E::V1"));
  REQUIRE_EQ(model.fields[1].qualified_name, std::string("0x1::M::E::V1::common"));
  REQUIRE_EQ(model.fields[2].qualified_name, std::string("0x1::M::Box::value"));

  REQUIRE_EQ(model.field_handle_targets, (std::vector<movescape::SemanticIndex>{2}));
  REQUIRE_EQ(model.variant_field_handle_targets[0], (std::vector<movescape::SemanticIndex>{1, 0}));
  REQUIRE_EQ(model.struct_variant_handle_targets, (std::vector<movescape::SemanticIndex>{1}));

  REQUIRE_EQ(model.struct_instantiations[0].structure, 1U);
  REQUIRE_EQ(model.struct_instantiations[0].type_arguments[0].kind, movescape::TypeKind::U8);
  REQUIRE_EQ(model.function_instantiations[0].function, 3U);
  REQUIRE_EQ(model.field_instantiations[0].field, 2U);
  REQUIRE_EQ(model.variant_field_instantiations[0].fields, (std::vector<movescape::SemanticIndex>{1, 0}));
  REQUIRE_EQ(model.struct_variant_instantiations[0].variant, 1U);
}

TEST(semantic_model_records_calls_and_a_deterministic_direct_graph) {
  const auto model = movescape::buildSemanticModel(semanticFixture());

  REQUIRE_EQ(model.call_sites.size(), 9U);
  REQUIRE_EQ(model.call_sites[0].kind, movescape::CallSiteKind::Direct);
  REQUIRE_EQ(*model.call_sites[0].callee_definition, 1U);
  REQUIRE_EQ(model.call_sites[1].kind, movescape::CallSiteKind::Direct);
  REQUIRE(!model.call_sites[1].callee_definition.has_value());
  REQUIRE_EQ(model.call_sites[2].kind, movescape::CallSiteKind::Generic);
  REQUIRE_EQ(*model.call_sites[2].callee_definition, 2U);
  REQUIRE_EQ(*model.call_sites[2].instantiation, 0U);
  REQUIRE_EQ(model.call_sites[2].type_arguments[0].kind, movescape::TypeKind::U8);
  REQUIRE_EQ(model.call_sites[3].kind, movescape::CallSiteKind::ClosureCreation);
  REQUIRE_EQ(model.call_sites[4].kind, movescape::CallSiteKind::GenericClosureCreation);
  REQUIRE_EQ(model.call_sites[6].kind, movescape::CallSiteKind::ClosureInvocation);
  REQUIRE(!model.call_sites[6].callee_handle.has_value());
  REQUIRE_EQ(*model.call_sites[6].closure_signature, 2);
  REQUIRE_EQ(*model.call_sites[7].callee_definition, 0U);
  REQUIRE_EQ(*model.call_sites[8].callee_definition, 1U);

  REQUIRE_EQ(model.direct_callees[0], (std::vector<movescape::SemanticIndex>{1, 2, 0}));
  REQUIRE(model.direct_callees[1].empty());
  REQUIRE(model.direct_callees[2].empty());
  REQUIRE(model.direct_callees[3].empty());
}

TEST(semantic_model_rejects_unvalidated_cross_references) {
  auto module = semanticFixture();
  module.function_handles[0].module = 99;
  REQUIRE_ERROR(movescape::buildSemanticModel(module), movescape::ErrorCode::InvalidIndex);
}

TEST(semantic_formatter_is_deterministic_and_exposes_resolved_calls) {
  const auto model = movescape::buildSemanticModel(semanticFixture());
  const auto first = movescape::formatSemanticModel(model);
  const auto second = movescape::formatSemanticModel(model);

  REQUIRE_EQ(first, second);
  REQUIRE(first.starts_with("semantic-module 0x1::M\n"));
  REQUIRE(first.find("struct#2 0x2::Ext::ExternalS external") != std::string::npos);
  REQUIRE(first.find("variant-field-inst#0 fields=[field#1, field#0]") != std::string::npos);
  REQUIRE(first.find("@1 direct -> function#2 0x2::Ext::external external") != std::string::npos);
  REQUIRE(first.find("@8 closure-invoke signature#2 target=unknown") != std::string::npos);
  REQUIRE(first.find("definition#0 0x1::M::caller -> [definition#1 "
                     "0x1::M::callee, definition#2 0x1::M::generic_fun, "
                     "definition#0 0x1::M::caller]") != std::string::npos);
}
