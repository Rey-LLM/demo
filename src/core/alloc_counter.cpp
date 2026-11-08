#include "core/alloc_counter.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
// Per-thread counters (not shared singletons: every thread has its own copy).
// Trivial thread_locals: no TLS constructor runs inside operator new.
thread_local std::uint64_t t_allocations = 0;
thread_local std::uint64_t t_foreign_allocations = 0;
thread_local unsigned t_foreign_depth = 0;
}  // namespace

namespace ftv {

std::uint64_t thread_allocation_count() noexcept { return t_allocations; }
std::uint64_t thread_foreign_allocation_count() noexcept { return t_foreign_allocations; }
void enter_foreign_code() noexcept { ++t_foreign_depth; }
void leave_foreign_code() noexcept {
  if (t_foreign_depth != 0) --t_foreign_depth;
}

bool allocation_counting_enabled() noexcept {
#if defined(FTVIZ_COUNT_ALLOCATIONS)
  return true;
#else
  return false;
#endif
}

}  // namespace ftv

#if defined(FTVIZ_COUNT_ALLOCATIONS)

namespace {
void* counted_alloc(std::size_t n, std::size_t align) {
  if (t_foreign_depth != 0) {
    ++t_foreign_allocations;
  } else {
    ++t_allocations;
  }
  if (n == 0) n = 1;
  void* p = nullptr;
  if (align <= alignof(std::max_align_t)) {
    p = std::malloc(n);
  } else {
#if defined(_MSC_VER)
    p = _aligned_malloc(n, align);
#else
    const std::size_t rounded = (n + align - 1) / align * align;
    p = std::aligned_alloc(align, rounded);
#endif
  }
  if (!p) throw std::bad_alloc();
  return p;
}
void counted_free(void* p, std::size_t align) noexcept {
  if (!p) return;
#if defined(_MSC_VER)
  if (align > alignof(std::max_align_t)) {
    _aligned_free(p);
    return;
  }
#else
  (void)align;
#endif
  std::free(p);
}
}  // namespace

void* operator new(std::size_t n) { return counted_alloc(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return counted_alloc(n, alignof(std::max_align_t)); }
void* operator new(std::size_t n, std::align_val_t a) { return counted_alloc(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return counted_alloc(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return counted_alloc(n, alignof(std::max_align_t));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return counted_alloc(n, alignof(std::max_align_t));
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete[](void* p) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete(void* p, std::align_val_t a) noexcept { counted_free(p, static_cast<std::size_t>(a)); }
void operator delete[](void* p, std::align_val_t a) noexcept { counted_free(p, static_cast<std::size_t>(a)); }
#if defined(__cpp_sized_deallocation)
// Sized forms exist only when the compiler emits sized deallocation calls.
void operator delete(void* p, std::size_t) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { counted_free(p, static_cast<std::size_t>(a)); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { counted_free(p, static_cast<std::size_t>(a)); }
#endif

#endif
