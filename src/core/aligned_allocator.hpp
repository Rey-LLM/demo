#pragma once
// Cache-line aligned allocator and vector alias used for every hot SoA array.
// Storage returned by AlignedVector<T> starts on a 64-byte boundary, so SIMD
// loads are aligned and two arrays never share their first cache line.

#include <cstddef>
#include <limits>
#include <new>
#include <vector>

namespace ftv {

inline constexpr std::size_t kCacheLine = 64;

template <class T, std::size_t Align = kCacheLine>
struct AlignedAllocator {
  static_assert(Align >= alignof(T), "alignment must not be weaker than alignof(T)");
  static_assert((Align & (Align - 1)) == 0, "alignment must be a power of two");

  using value_type = T;
  template <class U>
  struct rebind {
    using other = AlignedAllocator<U, Align>;
  };

  AlignedAllocator() noexcept = default;
  template <class U>
  AlignedAllocator(const AlignedAllocator<U, Align>&) noexcept {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] T* allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length();
    }
    return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{Align}));
  }

  void deallocate(T* p, std::size_t n) noexcept {
    // Sized deallocation is optional before Clang 19 (-fsized-deallocation),
    // so fall back to the unsized aligned form where it is not declared.
#if defined(__cpp_sized_deallocation)
    ::operator delete(p, n * sizeof(T), std::align_val_t{Align});
#else
    (void)n;
    ::operator delete(p, std::align_val_t{Align});
#endif
  }

  template <class U>
  bool operator==(const AlignedAllocator<U, Align>&) const noexcept {
    return true;
  }
};

template <class T>
using AlignedVector = std::vector<T, AlignedAllocator<T>>;

// Pads a value to its own cache line to prevent false sharing between
// per-thread accumulators stored in an array.
template <class T>
struct alignas(kCacheLine) CachePadded {
  T value{};
};

}  // namespace ftv
