#include "ingest/ingest.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

#include "core/log.hpp"
#include "core/mpmc_queue.hpp"
#include "core/timing.hpp"
#include "ingest/chunker.hpp"
#include "ingest/csv_parser.hpp"
#include "ingest/ftxb_format.hpp"
#include "ingest/types.hpp"

namespace ftv {

std::string_view IngestResult::label(std::uint32_t entity) const noexcept {
  if (entity >= label_offset.size()) return {};
  const std::uint64_t off = label_offset[entity];
  const std::uint64_t len = label_len[entity];
  if (off + len > file.size()) return {};
  std::string_view s = trim(std::string_view(file.data() + off, static_cast<std::size_t>(len)));
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

InputFormat detect_format(const std::string& path, std::span<const char> head) noexcept {
  if (head.size() >= 4 && std::memcmp(head.data(), kFtxbMagic, 4) == 0) return InputFormat::Ftxb;
  if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".ftxb") == 0) return InputFormat::Ftxb;
  return InputFormat::Csv;
}

namespace {

struct LocItem {
  std::uint32_t entity;
  std::uint32_t count;
  std::uint64_t code;
};

void merge_registry(ThreadPool& pool, std::vector<std::unique_ptr<ParseWorker>>& workers, EntityRegistry& reg) {
  std::size_t total = 0;
  for (auto& w : workers) total += w->votes().size();
  reg.reserve(total / 2 + 1024);
  pool.parallel_for_range(workers.size(), 1, [&](std::size_t b, std::size_t e) {
    for (std::size_t wi = b; wi < e; ++wi) {
      std::array<std::vector<RegistryItem>, EntityRegistry::kShardCount> buckets;
      const VoteTable& votes = workers[wi]->votes();
      for (auto& bk : buckets) bk.reserve(votes.size() / EntityRegistry::kShardCount + 16);
      votes.for_each([&](const VoteKey& k, const VoteVal& v) {
        buckets[EntityRegistry::shard_of(k.key)].push_back(RegistryItem{k.key, v.label_offset, v.label_len});
      });
      // Start at a worker-dependent shard to spread lock contention.
      for (unsigned i = 0; i < EntityRegistry::kShardCount; ++i) {
        const unsigned s = static_cast<unsigned>((i + wi * 7) % EntityRegistry::kShardCount);
        if (!buckets[s].empty()) reg.merge_batch(s, buckets[s]);
      }
    }
  });
}

void merge_locations(ThreadPool& pool, std::vector<std::unique_ptr<ParseWorker>>& workers, const EntityRegistry& reg,
                     std::size_t entityCount, EntityLocations& out) {
  const std::size_t P = std::max<std::size_t>(1, std::min<std::size_t>(256, entityCount));
  const std::size_t W = workers.size();
  std::vector<std::vector<std::vector<LocItem>>> buckets(W, std::vector<std::vector<LocItem>>(P));
  pool.parallel_for_range(W, 1, [&](std::size_t b, std::size_t e) {
    for (std::size_t wi = b; wi < e; ++wi) {
      auto& my = buckets[wi];
      workers[wi]->votes().for_each([&](const VoteKey& k, const VoteVal& v) {
        const std::uint32_t idx = reg.dense_of(k.key);
        const std::size_t p = static_cast<std::size_t>(static_cast<std::uint64_t>(idx) * P / entityCount);
        my[p].push_back(LocItem{idx, v.count, k.loc});
      });
      workers[wi]->votes().release();  // no longer needed; free memory early
    }
  });
  std::vector<std::vector<LocItem>> merged(P);
  pool.parallel_for_range(P, 1, [&](std::size_t b, std::size_t e) {
    for (std::size_t p = b; p < e; ++p) {
      std::size_t n = 0;
      for (std::size_t w = 0; w < W; ++w) n += buckets[w][p].size();
      auto& m = merged[p];
      m.reserve(n);
      for (std::size_t w = 0; w < W; ++w) {
        m.insert(m.end(), buckets[w][p].begin(), buckets[w][p].end());
        std::vector<LocItem>().swap(buckets[w][p]);
      }
      std::sort(m.begin(), m.end(), [](const LocItem& a, const LocItem& c) {
        return a.entity != c.entity ? a.entity < c.entity : a.code < c.code;
      });
      std::size_t o = 0;
      for (std::size_t i = 0; i < m.size(); ++i) {
        if (o > 0 && m[o - 1].entity == m[i].entity && m[o - 1].code == m[i].code) {
          m[o - 1].count += m[i].count;
        } else {
          m[o++] = m[i];
        }
      }
      m.resize(o);
    }
  });
  std::vector<std::size_t> base(P + 1, 0);
  for (std::size_t p = 0; p < P; ++p) base[p + 1] = base[p] + merged[p].size();
  out.offset.assign(entityCount + 1, 0);
  out.code.resize(base[P]);
  out.count.resize(base[P]);
  pool.parallel_for_range(P, 1, [&](std::size_t b, std::size_t e) {
    for (std::size_t p = b; p < e; ++p) {
      const auto& m = merged[p];
      // Entity range owned by partition p: idx with floor(idx*P/N) == p.
      const std::size_t lo = (p * entityCount + P - 1) / P;
      const std::size_t hi = ((p + 1) * entityCount + P - 1) / P;
      std::size_t cursor = 0;
      for (std::size_t idx = lo; idx < hi; ++idx) {
        out.offset[idx] = static_cast<std::uint32_t>(base[p] + cursor);
        while (cursor < m.size() && m[cursor].entity == idx) {
          out.code[base[p] + cursor] = m[cursor].code;
          out.count[base[p] + cursor] = m[cursor].count;
          ++cursor;
        }
      }
    }
  });
  out.offset[entityCount] = static_cast<std::uint32_t>(base[P]);
}

}  // namespace

bool run_ingest(const IngestOptions& options, ThreadPool& pool, IngestResult& out, std::string& error) {
  Stopwatch wall;
  Stopwatch phase;
  IngestStats& st = out.stats;
  st = IngestStats{};

  if (!out.file.open(options.path, error)) return false;
  const std::span<const char> bytes = out.file.bytes();
  st.phases.push_back({"map", phase.lap_ms()});

  InputFormat fmt = options.format == InputFormat::Auto
                        ? detect_format(options.path, bytes.subspan(0, std::min<std::size_t>(bytes.size(), 64)))
                        : options.format;
  out.format = fmt;

  CsvSchema schema{};
  std::vector<Chunk> chunks;
  FtxbHeader fh{};
  std::size_t expectedRows = 0;
  if (fmt == InputFormat::Csv) {
    const std::string_view all(bytes.data(), bytes.size());
    const std::size_t headerLen = csv_header_length(all);
    if (headerLen == 0) {
      error = "input is empty; a CSV header is required";
      return false;
    }
    std::string_view header = all.substr(0, headerLen);
    if (!header.empty() && header.back() == '\n') header.remove_suffix(1);
    if (!parse_csv_header(header, schema, error)) return false;
    chunks = make_line_chunks(bytes, headerLen, options.chunk_bytes);
    expectedRows = bytes.size() / 110;  // typical row length of realistic datasets
  } else {
    if (!validate_ftxb_header(bytes, fh, error)) return false;
    chunks = make_record_chunks(fh.records_offset, fh.record_count, sizeof(FtxbRecord), options.chunk_bytes);
    expectedRows = fh.record_count;
  }
  st.chunk_count = chunks.size();
  st.phases.push_back({"chunk", phase.lap_ms()});

  const unsigned nThreads = std::max(1u, options.parser_threads ? options.parser_threads : default_parser_threads());
  st.parser_threads = nThreads;
  CollisionVerifier verifier;
  out.workers.clear();
  for (unsigned i = 0; i < nThreads; ++i) {
    out.workers.push_back(std::make_unique<ParseWorker>(&schema, expectedRows / nThreads + 1, options.log_malformed,
                                                        &verifier));
  }
  st.phases.push_back({"alloc", phase.lap_ms()});

  // ---- parse: producer pushes chunk descriptors, T_ingest[N] consume -------
  {
    MPMCQueue<Chunk> queue(std::max<std::size_t>(64, chunks.size()));
    std::vector<std::jthread> threads;
    threads.reserve(nThreads);
    const char* base = bytes.data();
    for (unsigned t = 0; t < nThreads; ++t) {
      threads.emplace_back([&, t] {
        ParseWorker& w = *out.workers[t];
        Chunk c;
        while (queue.pop(c)) {
          if (fmt == InputFormat::Csv) {
            w.process_csv_chunk(base, c);
          } else {
            w.process_ftxb_chunk(base, c, fh.pool_offset, fh.pool_size);
          }
        }
      });
    }
    for (const Chunk& c : chunks) queue.push(c);
    queue.close();
  }  // jthreads join here: all worker writes happen-before the code below
  st.parse_ms = phase.lap_ms();
  st.phases.push_back({"parse", st.parse_ms});

  std::vector<std::uint64_t> badOffsets;
  for (auto& w : out.workers) {
    const WorkerStats& ws = w->stats();
    st.rows_ok += ws.rows_ok;
    st.rows_malformed += ws.rows_malformed;
    st.rows_blank += ws.rows_blank;
    st.amounts_truncated += ws.amounts_truncated;
    st.bytes += ws.bytes;
    badOffsets.insert(badOffsets.end(), ws.malformed_offsets.begin(), ws.malformed_offsets.begin() + ws.malformed_logged);
  }
  std::sort(badOffsets.begin(), badOffsets.end());
  if (badOffsets.size() > options.log_malformed) badOffsets.resize(options.log_malformed);
  st.malformed_offsets = badOffsets;

  // ---- registry merge + deterministic dense indices ------------------------
  out.registry = std::make_unique<EntityRegistry>();
  merge_registry(pool, out.workers, *out.registry);
  st.phases.push_back({"registry_merge", phase.lap_ms()});
  st.unique_entities =
      out.registry->assign_dense_indices(pool, out.keys, out.label_offset, out.label_len);
  st.phases.push_back({"dense_index", phase.lap_ms()});

  merge_locations(pool, out.workers, *out.registry, out.keys.size(), out.locations);
  st.phases.push_back({"location_votes", phase.lap_ms()});

  st.wall_ms = wall.millis();
  return true;
}

}  // namespace ftv
