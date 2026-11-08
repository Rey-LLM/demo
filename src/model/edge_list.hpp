#pragma once
// Structure-of-Arrays list of aggregated edges. One entry per unique
// (src_idx, dst_idx, currency, channel) tuple, sorted by that tuple.

#include <cstddef>
#include <cstdint>

#include "core/aligned_allocator.hpp"

namespace ftv {

enum EdgeFlag : std::uint8_t {
  kEdgeInterHub = 1u << 0,  // endpoints live in different macro-hubs
  kEdgeSelfLoop = 1u << 1,  // src == dst
};

struct EdgeList {
  AlignedVector<std::uint32_t> src;
  AlignedVector<std::uint32_t> dst;
  AlignedVector<std::int64_t> weight;   // total_amount in minor units (exact sum clamped to int64)
  AlignedVector<std::uint32_t> count;   // tx_count
  AlignedVector<std::uint32_t> currency;
  AlignedVector<std::uint8_t> channel;
  AlignedVector<std::uint8_t> flags;
  AlignedVector<std::int64_t> first_ts;
  AlignedVector<std::int64_t> last_ts;

  [[nodiscard]] std::size_t size() const noexcept { return src.size(); }
  void resize(std::size_t n) {
    src.resize(n);
    dst.resize(n);
    weight.resize(n);
    count.resize(n);
    currency.resize(n);
    channel.resize(n);
    flags.resize(n);
    first_ts.resize(n);
    last_ts.resize(n);
  }
};

}  // namespace ftv
