#pragma once
// Multi-threaded ingestion pipeline:
//   mmap -> chunk (~16 MB, '\n'-snapped) -> bounded MPMC queue ->
//   T_ingest[N] parser workers (thread-local dedup + partial aggregation) ->
//   bulk merge into the sharded registry -> deterministic dense indices ->
//   per-entity location votes (for hub assignment).
// Edge partials are merged afterwards by model::aggregate_edges().

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/aligned_allocator.hpp"
#include "core/thread_pool.hpp"
#include "ingest/entity_registry.hpp"
#include "ingest/key_hash.hpp"
#include "ingest/mapped_file.hpp"
#include "ingest/parse_worker.hpp"

namespace ftv {

enum class InputFormat { Auto, Csv, Ftxb };

struct IngestOptions {
  std::string path;
  InputFormat format = InputFormat::Auto;
  unsigned parser_threads = 0;          // 0 => max(1, hardware_concurrency - 2)
  std::size_t chunk_bytes = 16u << 20;  // ~16 MB
  unsigned log_malformed = 0;           // print the first K malformed row offsets (<= 16)
};

struct PhaseTiming {
  const char* name;
  double ms;
};

struct IngestStats {
  std::uint64_t bytes = 0;
  std::uint64_t rows_ok = 0;
  std::uint64_t rows_malformed = 0;
  std::uint64_t rows_blank = 0;
  std::uint64_t amounts_truncated = 0;
  std::uint64_t unique_entities = 0;
  std::uint64_t aggregated_edges = 0;
  std::uint64_t dropped_invalid_edges = 0;
  unsigned parser_threads = 0;
  std::size_t chunk_count = 0;
  double wall_ms = 0.0;
  double parse_ms = 0.0;
  std::vector<PhaseTiming> phases;
  std::vector<std::uint64_t> malformed_offsets;  // first K, sorted

  [[nodiscard]] double rows_per_sec() const noexcept {
    return parse_ms > 0 ? static_cast<double>(rows_ok + rows_malformed) / (parse_ms / 1000.0) : 0.0;
  }
  [[nodiscard]] double mb_per_sec() const noexcept {
    return parse_ms > 0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (parse_ms / 1000.0) : 0.0;
  }
};

// Per-entity location votes in CSR form: entity i has locations
// code[offset[i] .. offset[i+1]) with matching vote counts.
struct EntityLocations {
  AlignedVector<std::uint32_t> offset;
  AlignedVector<std::uint64_t> code;
  AlignedVector<std::uint32_t> count;
};

struct IngestResult {
  MappedFile file;  // kept mapped: tooltip labels are lazy views into it
  InputFormat format = InputFormat::Csv;
  AlignedVector<std::uint64_t> keys;          // dense index -> key64 (sorted ascending)
  AlignedVector<std::uint64_t> label_offset;  // dense index -> file offset of first raw identifier
  AlignedVector<std::uint16_t> label_len;
  EntityLocations locations;
  std::unique_ptr<EntityRegistry> registry;
  std::vector<std::unique_ptr<ParseWorker>> workers;  // hold edge partials until aggregation
  IngestStats stats;

  [[nodiscard]] std::size_t entity_count() const noexcept { return keys.size(); }
  // Lazily materialised label for tooltips (a view into the mapping, with
  // surrounding whitespace and quotes removed). Never used on hot paths.
  [[nodiscard]] std::string_view label(std::uint32_t entity) const noexcept;
};

// Runs ingestion. Returns false with `error` set on fatal errors (missing
// file, bad header). Malformed rows never fail the run.
bool run_ingest(const IngestOptions& options, ThreadPool& pool, IngestResult& out, std::string& error);

// Detects the format from the magic bytes / extension.
InputFormat detect_format(const std::string& path, std::span<const char> head) noexcept;

}  // namespace ftv
