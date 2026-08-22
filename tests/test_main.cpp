#include "test.hpp"

#include <exception>
#include <iostream>

int main() {
  std::size_t failed = 0;
  for (const auto &test : movescape::test::registry()) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failed;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failed;
      std::cerr << "[fail] " << test.name << ": unknown exception\n";
    }
  }

  std::cout << (movescape::test::registry().size() - failed) << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
