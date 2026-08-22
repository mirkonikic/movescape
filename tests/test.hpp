#pragma once

#include "movescape/error.hpp"

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace movescape::test {

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}

class Registration {
public:
  Registration(std::string name, TestFunction function) { registry().push_back(TestCase{std::move(name), function}); }
};

inline void fail(const char *file, int line, const std::string &message) {
  std::ostringstream out;
  out << file << ':' << line << ": " << message;
  throw std::runtime_error(out.str());
}

template <typename Left, typename Right>
void requireEqual(const Left &left, const Right &right, const char *left_text, const char *right_text, const char *file, int line) {
  if (!(left == right)) {
    std::ostringstream out;
    out << "expected " << left_text << " == " << right_text;
    fail(file, line, out.str());
  }
}

template <typename Function> void requireError(Function &&function, ErrorCode expected, const char *file, int line) {
  try {
    std::invoke(std::forward<Function>(function));
  } catch (const Error &error) {
    if (error.code() != expected) {
      std::ostringstream out;
      out << "expected error " << errorCodeName(expected) << ", received " << errorCodeName(error.code()) << ": " << error.what();
      fail(file, line, out.str());
    }
    return;
  }
  fail(file, line, "expected movescape::Error, but no exception was thrown");
}

} // namespace movescape::test

#define MOVESCAPE_CONCAT_INNER(left, right) left##right
#define MOVESCAPE_CONCAT(left, right) MOVESCAPE_CONCAT_INNER(left, right)

#define TEST(name)                                                                                                                                             \
  static void name();                                                                                                                                          \
  static ::movescape::test::Registration MOVESCAPE_CONCAT(registration_, name)(#name, &name);                                                                    \
  static void name()

#define REQUIRE(condition)                                                                                                                                     \
  do {                                                                                                                                                         \
    if (!(condition)) {                                                                                                                                        \
      ::movescape::test::fail(__FILE__, __LINE__, "requirement failed: " #condition);                                                                           \
    }                                                                                                                                                          \
  } while (false)

#define REQUIRE_EQ(left, right) ::movescape::test::requireEqual((left), (right), #left, #right, __FILE__, __LINE__)

#define REQUIRE_ERROR(expression, code) ::movescape::test::requireError([&]() { (void)(expression); }, (code), __FILE__, __LINE__)
