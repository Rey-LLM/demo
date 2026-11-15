// Core concurrency primitives and config parsing.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/json.hpp"
#include "core/mpmc_queue.hpp"
#include "core/thread_pool.hpp"
#include "core/triple_buffer.hpp"
#include "ingest/int128_acc.hpp"
#include "ingest/chunker.hpp"
#include "ingest/types.hpp"
#include <cmath>
#include "model/hub_taxonomy.hpp"
#include "test_framework.hpp"

using namespace ftv;

TEST_CASE("core: thread pool covers every index exactly once") {
  ThreadPool pool(3);
  std::vector<int> hits(100003, 0);
  for (int round = 0; round < 20; ++round) {
    pool.parallel_for(hits.size(), 777, [&](std::size_t b, std::size_t e, unsigned slot) {
      CHECK(slot < pool.concurrency());
      for (std::size_t i = b; i < e; ++i) ++hits[i];
    });
  }
  bool ok = true;
  for (int h : hits) ok &= h == 20;
  CHECK(ok);
}

TEST_CASE("core: MPMC queue delivers every item once under contention") {
  MPMCQueue<std::uint64_t> q(64);
  constexpr std::uint64_t kPerProducer = 20000;
  std::atomic<std::uint64_t> sum{0}, count{0};
  std::vector<std::jthread> consumers;
  for (int c = 0; c < 3; ++c) {
    consumers.emplace_back([&] {
      std::uint64_t v;
      while (q.pop(v)) {
        sum.fetch_add(v, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  {
    std::vector<std::jthread> producers;
    for (int p = 0; p < 2; ++p) {
      producers.emplace_back([&, p] {
        for (std::uint64_t i = 1; i <= kPerProducer; ++i) q.push(i + static_cast<std::uint64_t>(p) * 1000000);
      });
    }
  }
  q.close();
  consumers.clear();
  CHECK_EQ(count.load(), 2 * kPerProducer);
  const std::uint64_t expect = 2 * (kPerProducer * (kPerProducer + 1) / 2) + kPerProducer * 1000000;
  CHECK_EQ(sum.load(), expect);
}

TEST_CASE("core: triple buffer hands over complete snapshots") {
  struct Snap {
    std::uint64_t a = 0, b = 0;
  };
  TripleBuffer<Snap> tb;
  std::atomic<bool> done{false};
  std::jthread writer([&] {
    for (std::uint64_t i = 1; i <= 200000; ++i) {
      tb.back().a = i;
      tb.back().b = i * 3;
      tb.publish();
    }
    done.store(true);
  });
  std::uint64_t last = 0;
  bool consistent = true, monotonic = true;
  while (!done.load()) {
    if (tb.acquire_latest()) {
      const Snap& s = tb.front();
      consistent &= s.b == s.a * 3;
      monotonic &= s.a >= last;
      last = s.a;
    }
  }
  writer.join();
  if (tb.acquire_latest()) last = tb.front().a;
  CHECK(consistent);
  CHECK(monotonic);
  CHECK_EQ(last, 200000u);
}

TEST_CASE("core: JSON parser") {
  std::string err;
  auto v = parse_json(R"({"a": 1.5, "b": [true, null, "x\"yé"], // comment
                         "c": {"d": -2e3}})",
                      err);
  REQUIRE(v.has_value());
  CHECK(v->number_or("a", 0) == 1.5);
  CHECK(v->find("b")->items().size() == 3);
  CHECK(v->find("b")->items()[2].as_string() == "x\"y\xC3\xA9");
  CHECK(v->find("c")->number_or("d", 0) == -2000.0);
  CHECK(!parse_json("{\"a\": }", err).has_value());
  CHECK(err.find("line 1") != std::string::npos);
  CHECK(!parse_json("[1, 2", err).has_value());
}

TEST_CASE("core: 128-bit accumulator is exact and order independent") {
  Int128Acc a, b;
  a.add(INT64_MAX);
  a.add(INT64_MAX);
  a.add(-INT64_MAX);
  CHECK_EQ(a.clamp_to_int64(), INT64_MAX);
  b.add(-INT64_MAX);
  b.add(INT64_MAX);
  b.add(INT64_MAX);
  CHECK(a == b);
  Int128Acc c;
  c.add(INT64_MAX);
  c.add(10);
  CHECK_EQ(c.clamp_to_int64(), INT64_MAX);  // saturates only at the end
  Int128Acc d;
  d.add(-5);
  d.add(3);
  CHECK_EQ(d.clamp_to_int64(), -2);
}

TEST_CASE("core: chunker snaps boundaries to newlines") {
  std::string text = "header\n";
  for (int i = 0; i < 1000; ++i) text += "row-" + std::to_string(i) + ",x,y\n";
  const auto chunks = make_line_chunks(std::span<const char>(text.data(), text.size()), 7, 100);
  std::uint64_t covered = 7;
  for (const auto& c : chunks) {
    CHECK_EQ(c.offset, covered);
    covered += c.size;
    CHECK(text[c.offset + c.size - 1] == '\n');
  }
  CHECK_EQ(covered, text.size());
}

TEST_CASE("model: default hub config layout matches the spec") {
  std::string err;
  auto tax = HubTaxonomy::load(std::string(FTVIZ_SOURCE_DIR) + "/config/hubs.json", err);
  REQUIRE(tax.has_value());
  for (const auto& h : tax->hubs()) {
    const float r = std::sqrt(h.anchor.x * h.anchor.x + h.anchor.z * h.anchor.z);
    if (h.type == HubType::FiatCenter) {
      CHECK(std::fabs(h.anchor.y) < 1e-3f && std::fabs(r - 1000.0f) < 1e-2f);
    } else if (h.type == HubType::ChainCloud) {
      CHECK(std::fabs(h.anchor.y - 600.0f) < 1e-3f && std::fabs(r - 700.0f) < 1e-2f);
    } else {
      CHECK(std::fabs(h.anchor.y + 600.0f) < 1e-3f && r < 1e-3f);
    }
  }
  CHECK(tax->find("NEW_YORK").has_value());
  CHECK(tax->find("LONDON").has_value());
  CHECK(tax->find("TOKYO").has_value());
  CHECK_EQ(tax->hub_for_location(pack_code8("US")), *tax->find("NEW_YORK"));
  CHECK_EQ(tax->hub_for_location(pack_code8("ETH") | kChainDomainBit), *tax->find("BLOCKCHAIN_ETH"));
  CHECK_EQ(tax->hub_for_location(pack_code8("ZZ")), tax->fallback());
  CHECK_EQ(tax->hub_for_location(0), tax->fallback());
}
