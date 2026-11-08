#pragma once
// Tiny self-registering test harness (no third-party dependency).

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ftv::test {

struct TestCase {
  const char* name;
  void (*fn)();
};

// Registry lives in a function-local static owned by the test binary only.
std::vector<TestCase>& registry();
int& failure_count();

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace ftv::test

#define FTV_CONCAT2(a, b) a##b
#define FTV_CONCAT(a, b) FTV_CONCAT2(a, b)
#define TEST_CASE(name)                                                                   \
  static void FTV_CONCAT(ftv_test_fn_, __LINE__)();                                       \
  static const ::ftv::test::Registrar FTV_CONCAT(ftv_test_reg_, __LINE__)(name, &FTV_CONCAT(ftv_test_fn_, __LINE__)); \
  static void FTV_CONCAT(ftv_test_fn_, __LINE__)()

#define CHECK(cond)                                                                        \
  do {                                                                                     \
    if (!(cond)) {                                                                         \
      ++::ftv::test::failure_count();                                                      \
      std::fprintf(stderr, "  CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__);    \
    }                                                                                      \
  } while (0)

#define REQUIRE(cond)                                                                      \
  do {                                                                                     \
    if (!(cond)) {                                                                         \
      ++::ftv::test::failure_count();                                                      \
      std::fprintf(stderr, "  REQUIRE failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__);  \
      return;                                                                              \
    }                                                                                      \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
