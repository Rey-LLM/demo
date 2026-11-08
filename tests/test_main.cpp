#include <cstring>

#include "test_framework.hpp"

namespace ftv::test {
std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
int& failure_count() {
  static int n = 0;
  return n;
}
}  // namespace ftv::test

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0, failedTests = 0;
  for (const auto& t : ftv::test::registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    const int before = ftv::test::failure_count();
    std::fprintf(stderr, "[ RUN  ] %s\n", t.name);
    t.fn();
    const bool ok = ftv::test::failure_count() == before;
    std::fprintf(stderr, "[ %s ] %s\n", ok ? " OK " : "FAIL", t.name);
    ++ran;
    failedTests += ok ? 0 : 1;
  }
  std::fprintf(stderr, "\n%d tests, %d failed, %d failed checks\n", ran, failedTests, ftv::test::failure_count());
  return failedTests == 0 ? 0 : 1;
}
