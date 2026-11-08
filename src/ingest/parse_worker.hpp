#pragma once
// Per-thread parser state. Each T_ingest worker owns one ParseWorker and pops
// chunks from the shared MPMC queue. Everything a worker writes lives in its
// own cache-aligned object, so workers never share mutable state while parsing.

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/aligned_allocator.hpp"
#include "core/flat_hash_map.hpp"
#include "ingest/chunker.hpp"
#include "ingest/csv_parser.hpp"
#include "ingest/int128_acc.hpp"
#include "ingest/key_hash.hpp"
#include "ingest/types.hpp"

namespace ftv {

// (entity, location) vote: how many endpoint occurrences an entity had in a
// jurisdiction (fiat domain) or chain (chain domain). Drives hub assignment.
struct VoteKey {
  std::uint64_t key;
  std::uint64_t loc;
  bool operator==(const VoteKey&) const noexcept = default;
};
struct VoteKeyHash {
  std::size_t operator()(const VoteKey& k) const noexcept {
    return static_cast<std::size_t>(mix64(k.key ^ (k.loc * 0x9E3779B97F4A7C15ull)));
  }
};
struct VoteVal {
  std::uint64_t label_offset;  // absolute file offset of the first raw occurrence (min)
  std::uint32_t count;
  std::uint32_t label_len;
};

// Aggregation tuple (src, dst, currency, channel) on 64-bit entity keys.
struct EdgeKey {
  std::uint64_t src;
  std::uint64_t dst;
  std::uint32_t currency;
  std::uint32_t channel;
  bool operator==(const EdgeKey&) const noexcept = default;
};
static_assert(sizeof(EdgeKey) == 24);
// 64-bit hash of the tuple (XXH3 over the packed 24 bytes); used to place the
// tuple in the partial-aggregation table. Equality still compares the full
// tuple, so distinct edges never merge on a hash collision.
struct EdgeKeyHash {
  std::size_t operator()(const EdgeKey& k) const noexcept;
};
struct EdgeVal {
  Int128Acc total;
  std::int64_t first_ts;
  std::int64_t last_ts;
  std::uint32_t count;
};

using VoteTable = FlatHashMap<VoteKey, VoteVal, VoteKeyHash>;
using EdgeTable = FlatHashMap<EdgeKey, EdgeVal, EdgeKeyHash>;

inline constexpr unsigned kMaxLoggedMalformed = 16;

struct WorkerStats {
  std::uint64_t rows_ok = 0;
  std::uint64_t rows_malformed = 0;
  std::uint64_t rows_blank = 0;
  std::uint64_t amounts_truncated = 0;
  std::uint64_t bytes = 0;
  std::uint32_t malformed_logged = 0;
  std::array<std::uint64_t, kMaxLoggedMalformed> malformed_offsets{};
};

// One parsed record, all views pointing into the mapped file.
struct RowView {
  std::int64_t ts_ms = 0;
  std::int64_t amount_minor = 0;
  std::uint32_t currency = 0;
  Channel channel = Channel::INTERNAL;
  std::uint64_t src_juris = 0;
  std::uint64_t dst_juris = 0;
  std::uint64_t chain = 0;
  std::string_view src;
  std::string_view dst;
  bool src_escapes = false;
  bool dst_escapes = false;
  std::uint64_t src_file_offset = 0;
  std::uint64_t dst_file_offset = 0;
};

class alignas(kCacheLine) ParseWorker {
 public:
  ParseWorker(const CsvSchema* schema, std::size_t expectedRows, unsigned logMalformed,
              CollisionVerifier* verifier);

  void process_csv_chunk(const char* fileBase, const Chunk& chunk) noexcept;
  void process_ftxb_chunk(const char* fileBase, const Chunk& chunk, std::uint64_t poolOffset,
                          std::uint64_t poolSize) noexcept;

  [[nodiscard]] const WorkerStats& stats() const noexcept { return stats_; }
  [[nodiscard]] VoteTable& votes() noexcept { return votes_; }
  [[nodiscard]] EdgeTable& edges() noexcept { return edges_; }

  // Exposed for unit tests: validates and aggregates one row.
  bool accept_row(const RowView& row) noexcept;

 private:
  void record_malformed(std::uint64_t offset) noexcept;
  bool parse_csv_line(std::string_view line, const char* fileBase, RowView& row) noexcept;

  const CsvSchema* schema_;
  unsigned log_malformed_;
  CollisionVerifier* verifier_;
  WorkerStats stats_;
  VoteTable votes_;
  EdgeTable edges_;
  NormalizedKey src_key_;  // scratch buffers reused for every row (no allocation)
  NormalizedKey dst_key_;
};

}  // namespace ftv
