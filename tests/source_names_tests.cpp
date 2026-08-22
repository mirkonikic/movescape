#include "test.hpp"

#include "movescape/source_names.hpp"

#include <string>

TEST(source_identifiers_preserve_valid_names_and_leading_underscores) {
  REQUIRE(movescape::isMoveSourceIdentifier("ordinary_name7"));
  REQUIRE(movescape::isMoveSourceIdentifier("__lambda__1__f"));
  REQUIRE_EQ(movescape::makeMoveSourceIdentifier("__lambda__1__f", "function", 3), std::string("__lambda__1__f"));
}

TEST(source_identifiers_rewrite_bytecode_only_and_reserved_names) {
  REQUIRE(!movescape::isMoveSourceIdentifier("$generated"));
  REQUIRE(!movescape::isMoveSourceIdentifier("fun"));
  REQUIRE_EQ(movescape::makeMoveSourceIdentifier("$generated", "function", 2), std::string("_generated"));
  REQUIRE_EQ(movescape::makeMoveSourceIdentifier("$__", "function", 2), std::string("function_2"));
  REQUIRE_EQ(movescape::makeMoveSourceIdentifier("fun", "function", 2), std::string("function_fun"));
}
