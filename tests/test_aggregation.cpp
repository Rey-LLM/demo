// End-to-end ingestion + aggregation correctness and determinism.

#include <cstring>
#include <string>
#include <vector>

#include "ingest/ftxb_format.hpp"
#include "ingest/ingest.hpp"
#include "ingest/key_hash.hpp"
#include "model/edge_aggregation.hpp"
#include "model/graph_builder.hpp"
#include "model/hub_taxonomy.hpp"
#include "test_framework.hpp"
#include "test_util.hpp"

using namespace ftv;
using ftv::test::TempFile;

namespace {

const char* kCsv =
    "tx_id,timestamp_utc,src_account,dst_account,amount,currency,channel,src_jurisdiction,dst_jurisdiction,chain\n"
    "T1,2024-01-01T00:00:00Z,US-A,US-B,10.00,USD,ACH,US,US,\n"
    "T2,2024-01-02T00:00:00Z,us-a,US-B,5.50,USD,ACH,US,US,\r\n"          // same entity (case), CRLF
    "T3,2024-01-03T00:00:00Z,\" US-A \",US-B,1.25,USD,FEDWIRE,US,US,\n"  // quoted, different channel
    "T4,2024-01-04T00:00:00Z,US-A,US-B,3.00,EUR,ACH,US,US,\n"            // different currency
    "\n"                                                                 // blank line
    "T5,not-a-date,US-A,US-B,1.00,USD,ACH,US,US,\n"                      // malformed
    "T6,2024-01-05T00:00:00Z,US-A,,1.00,USD,ACH,US,US,\n"                // malformed: empty dst
    "T7,2024-01-06T00:00:00Z,0xAbC0000000000000000000000000000000000001,EXCH-AURORA-0001,2.000000000,ETH,ONCHAIN,,,ETH\n"
    "T8,2024-01-07T00:00:00Z,EXCH-AURORA-0001,GB-C,100.00,GBP,CHAPS,GB,GB,\n"
    "T9,2024-01-08T00:00:00Z,0xabc0000000000000000000000000000000000001,EXCH-AURORA-0001,0.5,ETH,ONCHAIN,,,ETH\n"
    "T10,2024-01-09T00:00:00Z,US-A,US-B,1.00,USD,ONCHAIN,US,US,\n"       // malformed: ONCHAIN without chain
    "T11,2024-01-10T00:00:00Z,US-A,US-B,1.00,USD,ACH,US,US,ETH\n"        // malformed: fiat row with chain
    "T12,2023-12-31T00:00:00Z,US-A,US-B,-2.00,USD,ACH,US,US,";           // reversal, no trailing newline

std::uint64_t fiat_key(std::string_view s) {
  NormalizedKey k;
  normalize_fiat_key(s, false, k);
  return hash_normalized(k, 0);
}

struct Loaded {
  IngestResult ingest;
  EdgeList edges;
};

bool load(const std::string& path, unsigned threads, std::size_t chunkBytes, Loaded& out) {
  ThreadPool pool(3);
  IngestOptions opt;
  opt.path = path;
  opt.parser_threads = threads;
  opt.chunk_bytes = chunkBytes;
  opt.log_malformed = 8;
  std::string err;
  if (!run_ingest(opt, pool, out.ingest, err)) {
    std::fprintf(stderr, "ingest failed: %s\n", err.c_str());
    return false;
  }
  out.edges = aggregate_edges(out.ingest, pool);
  return true;
}

std::uint32_t index_of(const IngestResult& r, std::uint64_t key) {
  for (std::size_t i = 0; i < r.keys.size(); ++i) {
    if (r.keys[i] == key) return static_cast<std::uint32_t>(i);
  }
  return 0xFFFFFFFFu;
}

}  // namespace

TEST_CASE("aggregation: sums, counts, timestamps and malformed rows") {
  TempFile f("agg.csv", kCsv);
  Loaded L;
  REQUIRE(load(f.path(), 2, 64, L));  // tiny chunks: many '\n'-snapped boundaries
  const IngestStats& st = L.ingest.stats;
  CHECK_EQ(st.rows_ok, 8u);
  CHECK_EQ(st.rows_malformed, 4u);
  CHECK_EQ(st.rows_blank, 1u);
  CHECK_EQ(st.malformed_offsets.size(), 4u);
  // Entities: US-A, US-B, ETH address, EXCH-AURORA-0001, GB-C.
  CHECK_EQ(L.ingest.entity_count(), 5u);

  const std::uint32_t a = index_of(L.ingest, fiat_key("US-A"));
  const std::uint32_t b = index_of(L.ingest, fiat_key("US-B"));
  REQUIRE(a != 0xFFFFFFFFu && b != 0xFFFFFFFFu);

  // Edges: (A,B,USD,ACH) x3 [T1,T2,T12], (A,B,USD,FEDWIRE), (A,B,EUR,ACH),
  // (ETH->EXCH,ETH,ONCHAIN) x2, (EXCH->GB-C,GBP,CHAPS)
  CHECK_EQ(L.edges.size(), 5u);
  bool found = false;
  for (std::size_t i = 0; i < L.edges.size(); ++i) {
    if (L.edges.src[i] == a && L.edges.dst[i] == b && L.edges.currency[i] == pack_code4("USD") &&
        L.edges.channel[i] == static_cast<std::uint8_t>(Channel::ACH)) {
      found = true;
      CHECK_EQ(L.edges.count[i], 3u);
      CHECK_EQ(L.edges.weight[i], 1000 + 550 - 200);
      CHECK_EQ(L.edges.first_ts[i], 1703980800000LL);  // 2023-12-31
      CHECK_EQ(L.edges.last_ts[i], 1704153600000LL);   // 2024-01-02
    }
  }
  CHECK(found);
  // Edges are sorted by (src, dst, currency, channel).
  for (std::size_t i = 1; i < L.edges.size(); ++i) {
    const bool ordered = L.edges.src[i - 1] < L.edges.src[i] ||
                         (L.edges.src[i - 1] == L.edges.src[i] && L.edges.dst[i - 1] <= L.edges.dst[i]);
    CHECK(ordered);
  }
  // Labels are lazy views into the mapping.
  CHECK(L.ingest.label(a) == "US-A");
  // ETH amounts use gwei scale (9 decimals): 2 + 0.5 ETH.
  for (std::size_t i = 0; i < L.edges.size(); ++i) {
    if (L.edges.currency[i] == pack_code4("ETH")) {
      CHECK_EQ(L.edges.count[i], 2u);
      CHECK_EQ(L.edges.weight[i], 2500000000LL);
    }
  }
}

TEST_CASE("aggregation: deterministic regardless of thread count and chunking") {
  // Build a larger CSV with repeated pairs.
  std::string csv =
      "tx_id,timestamp_utc,src_account,dst_account,amount,currency,channel,src_jurisdiction,dst_jurisdiction,chain\n";
  for (int i = 0; i < 20000; ++i) {
    char line[200];
    std::snprintf(line, sizeof(line), "T%d,2024-01-01T00:00:%02dZ,ACC-%d,ACC-%d,%d.%02d,%s,%s,%s,%s,\n", i, i % 60,
                  (i * 7) % 997, (i * 13 + 5) % 991, i % 500, i % 100, (i % 3) ? "USD" : "EUR",
                  (i % 5) ? "ACH" : "SWIFT", (i % 2) ? "US" : "GB", (i % 4) ? "US" : "JP");
    csv += line;
  }
  TempFile f("det.csv", csv);
  Loaded one, many;
  REQUIRE(load(f.path(), 1, 1u << 20, one));
  REQUIRE(load(f.path(), 4, 4096, many));
  CHECK(one.ingest.keys == many.ingest.keys);
  CHECK(one.ingest.locations.offset == many.ingest.locations.offset);
  CHECK(one.ingest.locations.code == many.ingest.locations.code);
  CHECK(one.ingest.locations.count == many.ingest.locations.count);
  REQUIRE(one.edges.size() == many.edges.size());
  CHECK(one.edges.src == many.edges.src);
  CHECK(one.edges.dst == many.edges.dst);
  CHECK(one.edges.weight == many.edges.weight);
  CHECK(one.edges.count == many.edges.count);
  CHECK(one.edges.first_ts == many.edges.first_ts);
  CHECK(one.edges.last_ts == many.edges.last_ts);
  std::uint64_t total = 0;
  for (auto c : one.edges.count) total += c;
  CHECK_EQ(total, 20000u);
}

TEST_CASE("aggregation: .ftxb input matches the CSV semantics") {
  // Two records with a shared string pool; second record is malformed.
  std::string pool = "US-AUS-B";
  std::vector<char> file(sizeof(FtxbHeader) + 2 * sizeof(FtxbRecord) + pool.size());
  FtxbHeader h{};
  std::memcpy(h.magic, kFtxbMagic, 4);
  h.version = kFtxbVersion;
  h.record_count = 2;
  h.records_offset = sizeof(FtxbHeader);
  h.pool_offset = sizeof(FtxbHeader) + 2 * sizeof(FtxbRecord);
  h.pool_size = pool.size();
  std::memcpy(file.data(), &h, sizeof(h));
  FtxbRecord r{};
  r.timestamp_ms = 1000;
  r.amount_minor = 1234;
  r.src_off = 0;
  r.src_len = 4;
  r.dst_off = 4;
  r.dst_len = 4;
  r.channel = static_cast<std::uint8_t>(Channel::ACH);
  r.currency = pack_code4("USD");
  r.src_jurisdiction = pack_code8("US");
  r.dst_jurisdiction = pack_code8("US");
  std::memcpy(file.data() + sizeof(FtxbHeader), &r, sizeof(r));
  r.dst_off = 400;  // out-of-range pool reference => malformed
  std::memcpy(file.data() + sizeof(FtxbHeader) + sizeof(FtxbRecord), &r, sizeof(r));
  std::memcpy(file.data() + h.pool_offset, pool.data(), pool.size());
  TempFile f("bin.ftxb", std::string_view(file.data(), file.size()));
  Loaded L;
  REQUIRE(load(f.path(), 2, 64, L));
  CHECK(L.ingest.format == InputFormat::Ftxb);
  CHECK_EQ(L.ingest.stats.rows_ok, 1u);
  CHECK_EQ(L.ingest.stats.rows_malformed, 1u);
  REQUIRE(L.edges.size() == 1u);
  CHECK_EQ(L.edges.weight[0], 1234);
  CHECK_EQ(L.edges.src[0], index_of(L.ingest, fiat_key("US-A")));
}

TEST_CASE("model: hub assignment, bridges and inter-hub classification") {
  TempFile f("hubs.csv", kCsv);
  Loaded L;
  REQUIRE(load(f.path(), 2, 1u << 20, L));
  std::string err;
  auto tax = HubTaxonomy::load(std::string(FTVIZ_SOURCE_DIR) + "/config/hubs.json", err);
  REQUIRE(tax.has_value());
  ThreadPool pool(2);
  Graph g = build_graph(L.ingest, std::move(L.edges), *tax, pool);
  const std::uint32_t a = index_of(L.ingest, fiat_key("US-A"));
  const std::uint32_t ex = index_of(L.ingest, fiat_key("EXCH-AURORA-0001"));
  const std::uint32_t gb = index_of(L.ingest, fiat_key("GB-C"));
  REQUIRE(a < g.node_count() && ex < g.node_count() && gb < g.node_count());
  CHECK_EQ(g.nodes.hub[a], *tax->find("NEW_YORK"));
  CHECK_EQ(g.nodes.hub[gb], *tax->find("LONDON"));
  // Exchange: 2 ETH votes vs 1 GB vote => majority hub is the ETH cloud, and
  // it appears in both domains => BRIDGE.
  CHECK((g.nodes.flags[ex] & kNodeBridge) != 0);
  CHECK_EQ(g.nodes.hub[ex], *tax->find("BLOCKCHAIN_ETH"));
  CHECK(g.inter_hub_edges >= 1u);  // EXCH (ETH cloud) -> GB-C (LONDON)
  CHECK_EQ(g.adjacency.offset.size(), g.node_count() + 1);
}

TEST_CASE("model: hub assignment ties and fallback") {
  std::string err;
  auto tax = HubTaxonomy::load(std::string(FTVIZ_SOURCE_DIR) + "/config/hubs.json", err);
  REQUIRE(tax.has_value());
  std::uint16_t flags = 0;
  const std::uint64_t codes1[] = {pack_code8("GB"), pack_code8("US")};
  const std::uint32_t counts1[] = {5, 5};
  // Tie => lower hub id (NEW_YORK is hub 0, LONDON hub 1).
  CHECK_EQ(assign_hub(codes1, counts1, 2, *tax, flags), *tax->find("NEW_YORK"));
  const std::uint64_t codes2[] = {pack_code8("KY"), 0};
  const std::uint32_t counts2[] = {9, 1};
  CHECK_EQ(assign_hub(codes2, counts2, 2, *tax, flags), tax->fallback());
  CHECK((flags & kNodeUnresolved) != 0);
  const std::uint64_t codes3[] = {pack_code8("KY"), pack_code8("JP")};
  const std::uint32_t counts3[] = {100, 1};
  CHECK_EQ(assign_hub(codes3, counts3, 2, *tax, flags), *tax->find("TOKYO"));  // real hub beats fallback
}
