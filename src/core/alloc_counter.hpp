#pragma once
// Per-thread heap allocation counter used to assert "zero allocations per
// frame" on the render thread in Debug builds.
//
// When FTVIZ_COUNT_ALLOCATIONS is defined (Debug builds of the ftviz binary),
// alloc_counter.cpp replaces the global operator new family and increments a
// thread_local counter. Allocations made through malloc (C code: GLFW, Xlib,
// most of a GL driver) never reach operator new and are not counted.
//
// Third-party C++ code is the catch: on ELF platforms a driver's own C++
// allocations (Mesa's LLVM-based shader JIT, for example) resolve to our
// replacement operator new and would be charged to the render thread. Code
// that calls into such libraries brackets the call with ForeignCodeScope (the
// Debug GL loader does this for every GL call), and allocations made inside
// the bracket are counted separately as foreign: they are reported, but they
// do not trip the zero-allocation assertion, which is about our own code.

#include <cstdint>

namespace ftv {

[[nodiscard]] std::uint64_t thread_allocation_count() noexcept;
[[nodiscard]] std::uint64_t thread_foreign_allocation_count() noexcept;
[[nodiscard]] bool allocation_counting_enabled() noexcept;

void enter_foreign_code() noexcept;
void leave_foreign_code() noexcept;

class ForeignCodeScope {
 public:
  ForeignCodeScope() noexcept { enter_foreign_code(); }
  ~ForeignCodeScope() { leave_foreign_code(); }
  ForeignCodeScope(const ForeignCodeScope&) = delete;
  ForeignCodeScope& operator=(const ForeignCodeScope&) = delete;
};

}  // namespace ftv
