// Live ingest: parser fixtures (one per message type), ring stress, reconnect
// backoff bounds and the feed state machine on an injected clock.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "core/mpsc_ring.hpp"
#include "core/spsc_ring.hpp"
#include "ingest/live/feed_state.hpp"
#include "ingest/live/net.hpp"
#include "ingest/live/parsers.hpp"
#include "test_framework.hpp"

#ifndef FTVIZ_SOURCE_DIR
#define FTVIZ_SOURCE_DIR "."
#endif

using namespace ftv;

namespace {

struct Fixture {
  FeedBuffer buf{std::size_t{1} << 20};
  std::size_t len = 0;
  bool ok = false;
  explicit Fixture(const char* name) {
    std::ifstream f(std::string(FTVIZ_SOURCE_DIR) + "/tests/fixtures/live/" + name, std::ios::binary);
    if (!f) return;
    const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    len = buf.assign(s);
    ok = true;
  }
};

struct Env {
  JsonParser parser;
  EventBatch batch;
  PriceBook prices;
  ParseContext ctx;
  Env() {
    ctx.prices = &prices;
    ctx.hubs.chain_hub[0] = 8;  // BTC
    ctx.hubs.chain_hub[1] = 7;  // ETH
    ctx.hubs.chain_hub[2] = 9;  // SOL
    ctx.hubs.quote_hub.fill(11);
    ctx.hubs.quote_hub[static_cast<std::size_t>(Asset::USD)] = 0;
    ctx.hubs.quote_hub[static_cast<std::size_t>(Asset::GBP)] = 1;
    ctx.recv_ms = 1'700'000'000'000;
  }
};

bool near(double a, double b, double rel = 1e-9) { return std::fabs(a - b) <= rel * std::max(1.0, std::fabs(b)); }

}  // namespace

TEST_CASE("live: blockchain.com unconfirmed transaction") {
  Env e;
  e.prices.set(Asset::BTC, 60000.0, 1);
  Fixture f("blockchain_utx.json");
  REQUIRE(f.ok);
  const ParseResult r = parse_blockchain_com(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
  CHECK(r.status == ParseStatus::Ok);
  REQUIRE(e.batch.size() == 1);
  const LiveEvent& ev = e.batch[0];
  CHECK(ev.feed == FeedId::BtcMempool);
  CHECK(ev.kind == EventKind::Transaction);
  CHECK(ev.asset == Asset::BTC);
  CHECK(near(ev.amount, 0.65564));  // 7,140,000 + 58,424,000 satoshi
  CHECK(near(ev.usd_value, 0.65564 * 60000.0));
  CHECK_EQ(ev.ts_ms, std::int64_t{1440086763} * 1000);
  CHECK(ev.src_hub == 8 && ev.dst_hub == 8);
  CHECK(ev.id_hash != 0);
  CHECK((ev.flags & kEvPriceUnknown) == 0);

  Fixture pong("blockchain_pong.json");
  REQUIRE(pong.ok);
  e.batch.clear();
  CHECK(parse_blockchain_com(e.parser, pong.buf.data(), pong.len, e.ctx, e.batch).status == ParseStatus::Ignored);
  CHECK(e.batch.size() == 0);
}

TEST_CASE("live: a transaction without a known price carries no USD value") {
  Env e;
  Fixture f("blockchain_utx.json");
  REQUIRE(f.ok);
  parse_blockchain_com(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
  REQUIRE(e.batch.size() == 1);
  CHECK(e.batch[0].usd_value == 0.0);
  CHECK((e.batch[0].flags & kEvPriceUnknown) != 0);
}

TEST_CASE("live: mempool.space block") {
  Env e;
  Fixture f("mempool_block.json");
  REQUIRE(f.ok);
  const ParseResult r = parse_mempool_space(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
  CHECK(r.status == ParseStatus::Ok);
  CHECK(r.block_announced);
  CHECK_EQ(r.block_number, 870123u);
  REQUIRE(e.batch.size() == 1);
  CHECK(e.batch[0].kind == EventKind::Block);
  CHECK(e.batch[0].feed == FeedId::BtcBlocks);
  CHECK_EQ(e.batch[0].count, 3217u);
  CHECK_EQ(e.batch[0].ts_ms, std::int64_t{1731666000} * 1000);
}

TEST_CASE("live: Ethereum newHeads, block, ack, rejection and rate limit") {
  Env e;
  e.prices.set(Asset::ETH, 3000.0, 1);
  {
    Fixture f("eth_newheads.json");
    REQUIRE(f.ok);
    const ParseResult r = parse_eth_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
    CHECK(r.status == ParseStatus::Ok);
    CHECK(r.block_announced);
    CHECK_EQ(r.block_number, 0x1406f40u);
    CHECK(e.batch.size() == 0);
  }
  {
    Fixture f("eth_sub_ack.json");
    REQUIRE(f.ok);
    const ParseResult r = parse_eth_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.rpc_id, 1);
    CHECK(!r.block_announced);
  }
  {
    Fixture f("eth_block.json");
    REQUIRE(f.ok);
    e.batch.clear();
    const ParseResult r = parse_eth_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.rpc_id, 100);
    CHECK_EQ(r.transactions_seen, 3u);
    REQUIRE(e.batch.size() == 3);
    CHECK(near(e.batch[0].amount, 1.0));       // 0xde0b6b3a7640000 wei
    CHECK(near(e.batch[0].usd_value, 3000.0));
    CHECK(e.batch[1].amount == 0.0);           // token transfer: no ETH value
    CHECK(near(e.batch[2].amount, 1000.0));    // 0x3635c9adc5dea00000 wei
    for (std::size_t i = 0; i < 3; ++i) {
      CHECK_EQ(e.batch[i].ts_ms, std::int64_t{0x6737240b} * 1000);
      CHECK(e.batch[i].src_hub == 7 && e.batch[i].dst_hub == 7);
      CHECK(e.batch[i].asset == Asset::ETH);
    }
  }
  {
    Fixture f("eth_pending_rejected.json");
    REQUIRE(f.ok);
    const ParseResult r = parse_eth_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
    CHECK(r.status == ParseStatus::Rejected);
    CHECK_EQ(r.rpc_id, 2);
  }
  {
    Fixture f("eth_rate_limited.json");
    REQUIRE(f.ok);
    CHECK(parse_eth_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch).status == ParseStatus::RateLimited);
  }
}

TEST_CASE("live: Solana slot, block with vote filtering, skipped slot") {
  Env e;
  e.prices.set(Asset::SOL, 150.0, 1);
  {
    Fixture f("sol_slot.json");
    REQUIRE(f.ok);
    const ParseResult r = parse_sol_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
    CHECK(r.status == ParseStatus::Ok);
    CHECK(r.block_announced);
    CHECK_EQ(r.block_number, 301234567u);
  }
  {
    Fixture f("sol_block.json");
    REQUIRE(f.ok);
    e.batch.clear();
    e.ctx.label = SourceLabel::Delayed;
    const ParseResult r = parse_sol_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
    CHECK(r.status == ParseStatus::Ok);
    CHECK_EQ(r.transactions_seen, 3u);  // one transfer, one vote, one failed
    REQUIRE(e.batch.size() == 1);
    const LiveEvent& ev = e.batch[0];
    CHECK(ev.feed == FeedId::Sol);
    CHECK(ev.label == SourceLabel::Delayed);
    CHECK(near(ev.amount, 2.0));  // 2,000,000,000 lamports reached the recipient
    CHECK(near(ev.usd_value, 300.0));
    CHECK_EQ(ev.ts_ms, std::int64_t{1731666000} * 1000);
    CHECK_EQ(ev.count, 1u);
  }
  {
    Fixture f("sol_skipped.json");
    REQUIRE(f.ok);
    e.batch.clear();
    CHECK(parse_sol_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch).status == ParseStatus::Ignored);
    CHECK(e.batch.size() == 0);
  }
}

TEST_CASE("live: Coinbase match, ticker, subscriptions and error") {
  Env e;
  {
    Fixture f("coinbase_match.json");
    REQUIRE(f.ok);
    const ParseResult r = parse_coinbase(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices);
    CHECK(r.status == ParseStatus::Ok);
    REQUIRE(e.batch.size() == 1);
    const LiveEvent& ev = e.batch[0];
    CHECK(ev.kind == EventKind::Trade);
    CHECK(ev.asset == Asset::BTC);
    CHECK(near(ev.amount, 5.23512));
    CHECK(near(ev.usd_value, 5.23512 * 60000.25));
    CHECK((ev.flags & kEvBuy) != 0);          // maker sold, so the taker bought
    CHECK(ev.src_hub == 0 && ev.dst_hub == 8);  // USD home hub -> BTC
    CHECK_EQ(ev.id_hash, 10u);
    CHECK_EQ(ev.ts_ms, std::int64_t{1794717612345});
    CHECK(near(e.prices.usd(Asset::BTC), 60000.25));
  }
  {
    Fixture f("coinbase_ticker.json");
    REQUIRE(f.ok);
    e.batch.clear();
    CHECK(parse_coinbase(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Ok);
    CHECK(e.batch.size() == 0);
    CHECK(near(e.prices.usd(Asset::ETH), 3000.50));
  }
  {
    Fixture f("coinbase_subscriptions.json");
    REQUIRE(f.ok);
    CHECK(parse_coinbase(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Ok);
  }
  {
    Fixture f("coinbase_error.json");
    REQUIRE(f.ok);
    const ParseStatus s = parse_coinbase(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status;
    CHECK(s == ParseStatus::RateLimited);
  }
}

TEST_CASE("live: Kraken v2 trade, ticker, heartbeat and failed subscription") {
  Env e;
  {
    Fixture f("kraken_trade.json");
    REQUIRE(f.ok);
    const ParseResult r = parse_kraken(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices);
    CHECK(r.status == ParseStatus::Ok);
    REQUIRE(e.batch.size() == 2);
    CHECK(near(e.batch[0].usd_value, 60010.5 * 0.25));
    CHECK((e.batch[0].flags & kEvBuy) != 0);
    CHECK(e.batch[0].src_hub == 0 && e.batch[0].dst_hub == 8);
    CHECK(e.batch[1].asset == Asset::ETH);
    CHECK((e.batch[1].flags & kEvBuy) == 0);
    CHECK(e.batch[1].src_hub == 7 && e.batch[1].dst_hub == 0);  // ETH -> USD home hub
    CHECK_EQ(e.batch[1].id_hash, 4665907u);
    CHECK_EQ(e.batch[0].ts_ms, std::int64_t{1794717614123});
  }
  {
    Fixture f("kraken_ticker.json");
    REQUIRE(f.ok);
    e.batch.clear();
    CHECK(parse_kraken(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Ok);
    CHECK(e.batch.size() == 0);
    CHECK(near(e.prices.usd(Asset::SOL), 150.0));
  }
  {
    Fixture f("kraken_heartbeat.json");
    REQUIRE(f.ok);
    CHECK(parse_kraken(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Ok);
  }
  {
    Fixture f("kraken_sub_error.json");
    REQUIRE(f.ok);
    CHECK(parse_kraken(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Rejected);
  }
}

TEST_CASE("live: Binance aggTrade (USDT and EUR), acknowledgement and error") {
  Env e;
  e.ctx.hubs.quote_hub[static_cast<std::size_t>(Asset::USDT)] = 0;  // USDT drawn to the USD home hub
  e.ctx.hubs.quote_hub[static_cast<std::size_t>(Asset::EUR)] = 2;
  {
    Fixture f("binance_aggtrade.json");
    REQUIRE(f.ok);
    const ParseResult r = parse_binance(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices);
    CHECK(r.status == ParseStatus::Ok);
    REQUIRE(e.batch.size() == 1);
    const LiveEvent& ev = e.batch[0];
    CHECK(ev.feed == FeedId::Binance);
    CHECK(ev.kind == EventKind::Trade);
    CHECK(ev.asset == Asset::BTC);
    CHECK(near(ev.amount, 0.75));
    CHECK(near(ev.usd_value, 0.75 * 60100.5));     // USDT at par: no USDT/USD tick yet
    CHECK((ev.flags & kEvBuy) == 0);              // buyer was the maker: the taker sold
    CHECK(ev.src_hub == 8 && ev.dst_hub == 0);    // BTC -> USD home hub
    CHECK_EQ(ev.ts_ms, std::int64_t{1794717615000});
    CHECK_EQ(ev.id_hash, 3141592653u);
    CHECK(near(e.prices.usd(Asset::BTC), 60100.5));  // seeds an empty price book
  }
  {
    // A USDT/USD tick (Kraken) re-values later USDT trades.
    e.prices.set(Asset::USDT, 0.998, e.ctx.recv_ms);
    Fixture f("binance_aggtrade.json");
    e.batch.clear();
    parse_binance(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices);
    REQUIRE(e.batch.size() == 1);
    CHECK(near(e.batch[0].usd_value, 0.75 * 60100.5 * 0.998));
  }
  {
    Fixture f("binance_aggtrade_eur.json");
    REQUIRE(f.ok);
    e.batch.clear();
    parse_binance(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices);
    REQUIRE(e.batch.size() == 1);
    CHECK(e.batch[0].asset == Asset::ETH);
    CHECK((e.batch[0].flags & kEvBuy) != 0);
    CHECK(e.batch[0].src_hub == 2 && e.batch[0].dst_hub == 7);  // EUR home hub -> ETH
    CHECK(e.batch[0].usd_value == 0.0);                          // no EUR rate known yet
    CHECK((e.batch[0].flags & kEvPriceUnknown) != 0);
  }
  {
    Fixture f("binance_ack.json");
    REQUIRE(f.ok);
    e.batch.clear();
    CHECK(parse_binance(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Ok);
    CHECK(e.batch.size() == 0);
  }
  {
    Fixture f("binance_error.json");
    REQUIRE(f.ok);
    CHECK(parse_binance(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Rejected);
    const std::string m = R"({"code":-1003,"msg":"Too many requests"})";
    FeedBuffer b(256);
    const std::size_t n = b.assign(m);
    CHECK(parse_binance(e.parser, b.data(), n, e.ctx, e.batch, &e.prices).status == ParseStatus::RateLimited);
  }
}

TEST_CASE("live: ECB reference rates fill the FX board and only missing prices") {
  Env e;
  FxBoard board;
  // GBP already has a fresh exchange-implied rate; the reference rate must not replace it.
  e.prices.set(Asset::GBP, 1.30, e.ctx.recv_ms - 1000);
  Fixture f("ecb_rates.json");
  REQUIRE(f.ok);
  const ParseResult r = parse_ecb_rates(e.parser, f.buf.data(), f.len, e.ctx, &e.prices, &board);
  CHECK(r.status == ParseStatus::Ok);
  CHECK_EQ(r.block_number, 20261113u);
  CHECK_EQ(board.date(), 20261113u);
  CHECK(near(board.usd(Asset::EUR), 1.25));
  CHECK(near(board.usd(Asset::GBP), 1.0 / 0.75));
  CHECK(near(board.usd(Asset::JPY), 1.0 / 150.0));
  CHECK(near(e.prices.usd(Asset::EUR), 1.25));      // filled in
  CHECK(near(e.prices.usd(Asset::CNY), 1.0 / 7.2));
  CHECK(near(e.prices.usd(Asset::GBP), 1.30));      // exchange rate kept
  Fixture g("ecb_error.json");
  REQUIRE(g.ok);
  CHECK(parse_ecb_rates(e.parser, g.buf.data(), g.len, e.ctx, &e.prices, &board).status == ParseStatus::Rejected);
  Fixture m("malformed.json");
  CHECK(parse_ecb_rates(e.parser, m.buf.data(), m.len, e.ctx, &e.prices, &board).status == ParseStatus::Malformed);
  // A response in another base currency is not accepted.
  const std::string eur = R"({"amount":1.0,"base":"EUR","date":"2026-11-13","rates":{"USD":1.25}})";
  FeedBuffer b(256);
  const std::size_t n = b.assign(eur);
  CHECK(parse_ecb_rates(e.parser, b.data(), n, e.ctx, &e.prices, &board).status == ParseStatus::Malformed);
}

TEST_CASE("live: fiat quotes are valued through the implied cross rate only") {
  Env e;
  // BTC-GBP trade before any GBP rate is known: no USD value.
  std::string m = R"({"type":"match","trade_id":1,"time":"2026-11-15T04:40:12Z","product_id":"BTC-GBP","size":"1.0","price":"48000","side":"buy"})";
  FeedBuffer b(4096);
  std::size_t n = b.assign(m);
  parse_coinbase(e.parser, b.data(), n, e.ctx, e.batch, &e.prices);
  REQUIRE(e.batch.size() == 1);
  CHECK(e.batch[0].usd_value == 0.0);
  CHECK((e.batch[0].flags & kEvPriceUnknown) != 0);
  CHECK(e.batch[0].src_hub == 8 && e.batch[0].dst_hub == 1);  // taker sold BTC for GBP -> GBP home hub
  // BTC-USD and BTC-GBP tickers imply GBPUSD = 60000 / 48000.
  e.prices.set(Asset::BTC, 60000.0, 1);
  m = R"({"type":"ticker","product_id":"BTC-GBP","price":"48000","time":"2026-11-15T04:40:13Z"})";
  n = b.assign(m);
  parse_coinbase(e.parser, b.data(), n, e.ctx, e.batch, &e.prices);
  CHECK(near(e.prices.usd(Asset::GBP), 1.25));
  m = R"({"type":"match","trade_id":2,"time":"2026-11-15T04:40:14Z","product_id":"BTC-GBP","size":"2.0","price":"48000","side":"sell"})";
  n = b.assign(m);
  e.batch.clear();
  parse_coinbase(e.parser, b.data(), n, e.ctx, e.batch, &e.prices);
  REQUIRE(e.batch.size() == 1);
  CHECK(near(e.batch[0].usd_value, 2.0 * 48000.0 * 1.25));
}

TEST_CASE("live: malformed and truncated input is rejected, never crashes") {
  Env e;
  Fixture f("malformed.json");
  REQUIRE(f.ok);
  CHECK(parse_blockchain_com(e.parser, f.buf.data(), f.len, e.ctx, e.batch).status == ParseStatus::Malformed);
  CHECK(parse_eth_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch).status == ParseStatus::Malformed);
  CHECK(parse_sol_rpc(e.parser, f.buf.data(), f.len, e.ctx, e.batch).status == ParseStatus::Malformed);
  CHECK(parse_coinbase(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Malformed);
  CHECK(parse_kraken(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Malformed);
  CHECK(parse_binance(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices).status == ParseStatus::Malformed);
  // Every prefix of a valid message parses or fails cleanly.
  Fixture g("kraken_trade.json");
  REQUIRE(g.ok);
  FeedBuffer b(1 << 16);
  for (std::size_t cut = 0; cut < g.len; cut += 7) {
    const std::size_t n = b.assign(std::string_view(g.buf.data(), cut));
    e.batch.clear();
    const ParseStatus s = parse_kraken(e.parser, b.data(), n, e.ctx, e.batch, &e.prices).status;
    CHECK(s == ParseStatus::Malformed || s == ParseStatus::Ignored || s == ParseStatus::Ok);
  }
  CHECK(e.batch.size() == 0);
}

TEST_CASE("live: helpers") {
  std::uint64_t v = 0;
  CHECK(parse_hex_u64("0x1b4", v) && v == 0x1b4);
  CHECK(!parse_hex_u64("1b4", v));
  CHECK(!parse_hex_u64("0x", v));
  CHECK(near(hex_to_double("0xde0b6b3a7640000"), 1e18));
  Asset b = Asset::Count, q = Asset::Count;
  CHECK(split_pair("BTC-USD", b, q) && b == Asset::BTC && q == Asset::USD);
  CHECK(split_pair("XBT/EUR", b, q) && b == Asset::BTC && q == Asset::EUR);
  CHECK(!split_pair("DOGEUSD", b, q));
  CHECK(split_concat_pair("BTCUSDT", b, q) && b == Asset::BTC && q == Asset::USDT);
  CHECK(split_concat_pair("solusdt", b, q) && b == Asset::SOL && q == Asset::USDT);
  CHECK(split_concat_pair("ETHEUR", b, q) && b == Asset::ETH && q == Asset::EUR);
  CHECK(split_concat_pair("BTCUSD", b, q) && b == Asset::BTC && q == Asset::USD);
  CHECK(!split_concat_pair("DOGEUSDT", b, q));
  CHECK(!split_concat_pair("USDT", b, q));
  net::Url u;
  CHECK(net::parse_url("wss://ws.kraken.com/v2", u) && u.host == "ws.kraken.com" && u.port == 443 && u.tls && u.path == "/v2");
  CHECK(net::parse_url("https://api.mainnet-beta.solana.com", u) && u.path == "/" && u.tls);
  CHECK(!net::parse_url("ftp://example.org", u));
}

TEST_CASE("live: MPSC ring under contention loses nothing it accepted") {
  constexpr int kProducers = 4;
  constexpr std::uint64_t kPerProducer = 200000;
  MpscRing<LiveEvent> ring(1024);
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<int> done{0};
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      std::uint64_t d = 0;
      for (std::uint64_t i = 0; i < kPerProducer; ++i) {
        LiveEvent e;
        e.id_hash = (static_cast<std::uint64_t>(p) << 32) | i;
        e.count = static_cast<std::uint32_t>(p);
        if (!ring.try_push(e)) ++d;  // full: drop and count, never wait
      }
      dropped.fetch_add(d);
      done.fetch_add(1);
    });
  }
  std::uint64_t received = 0;
  std::uint64_t last[kProducers];
  bool seen[kProducers] = {};
  bool ordered = true;
  LiveEvent e;
  while (true) {
    const bool finished = done.load() == kProducers;
    bool any = false;
    while (ring.try_pop(e)) {
      any = true;
      ++received;
      const auto p = static_cast<std::size_t>(e.id_hash >> 32);
      const std::uint64_t i = e.id_hash & 0xFFFFFFFFu;
      if (p >= kProducers) {
        ordered = false;
        continue;
      }
      if (seen[p] && i <= last[p]) ordered = false;  // per-producer FIFO, no duplicates
      seen[p] = true;
      last[p] = i;
    }
    if (finished && !any) break;
  }
  for (auto& t : producers) t.join();
  CHECK(ordered);
  CHECK_EQ(received + dropped.load(), kProducers * kPerProducer);
  CHECK(received > 0);
}

TEST_CASE("live: a full MPSC ring rejects pushes and recovers") {
  MpscRing<LiveEvent> ring(8);
  LiveEvent e;
  int accepted = 0;
  for (int i = 0; i < 100; ++i) accepted += ring.try_push(e) ? 1 : 0;
  CHECK_EQ(accepted, 8);
  CHECK(ring.try_pop(e));
  CHECK(ring.try_push(e));
  CHECK(!ring.try_push(e));
}

TEST_CASE("live: SPSC ring hands events over in order") {
  SpscRing<VisualEvent, 64> ring;
  std::thread producer([&] {
    for (std::uint32_t i = 0; i < 100000; ++i) {
      VisualEvent v;
      v.src_hub = static_cast<std::uint16_t>(i & 0xFFFF);
      v.dst_hub = static_cast<std::uint16_t>(i >> 16);
      while (!ring.try_push(v)) std::this_thread::yield();
    }
  });
  std::uint32_t next = 0;
  bool ok = true;
  VisualEvent v;
  while (next < 100000) {
    if (!ring.try_pop(v)) {
      std::this_thread::yield();
      continue;
    }
    const std::uint32_t i = static_cast<std::uint32_t>(v.src_hub) | (static_cast<std::uint32_t>(v.dst_hub) << 16);
    ok = ok && i == next;
    ++next;
  }
  producer.join();
  CHECK(ok);
  CHECK_EQ(next, 100000u);
}

TEST_CASE("live: backoff is full jitter within [0, min(cap, base * 2^n)]") {
  Backoff b(12345, 0.5, 30.0);
  double ceiling = 0.5;
  double maxSeen = 0.0;
  for (int n = 0; n < 40; ++n) {
    CHECK(near(b.ceiling_s(), std::min(30.0, ceiling)));
    const double d = b.next_s();
    CHECK(d >= 0.0);
    CHECK(d <= std::min(30.0, ceiling) + 1e-12);
    maxSeen = std::max(maxSeen, d);
    ceiling = std::min(ceiling * 2.0, 1e9);
  }
  CHECK(maxSeen <= 30.0);
  CHECK(maxSeen > 5.0);  // the cap is actually reached into, not stuck near zero
  b.reset();
  CHECK(near(b.ceiling_s(), 0.5));
  // Jitter: many first delays spread over the whole interval.
  double lo = 1.0, hi = 0.0;
  for (int i = 0; i < 2000; ++i) {
    Backoff c(static_cast<std::uint64_t>(i) * 7919 + 1, 0.5, 30.0);
    const double d = c.next_s();
    lo = std::min(lo, d);
    hi = std::max(hi, d);
  }
  CHECK(lo < 0.05);
  CHECK(hi > 0.45 && hi <= 0.5);
}

TEST_CASE("live: feed falls back to REPLAY after 15 s offline and returns on recovery") {
  FeedStateMachine sm(15000, 30000);
  std::int64_t t = 1'000'000;
  sm.on_connecting(t);
  CHECK(sm.state() == FeedState::Connecting);
  CHECK(!sm.replay_active());
  sm.on_message(t += 200);
  CHECK(sm.state() == FeedState::Live);
  CHECK(sm.label() == SourceLabel::Live);
  CHECK(sm.label(SourceLabel::Delayed) == SourceLabel::Delayed);

  // The socket drops: OFFLINE at once, REPLAY only after more than 15 s.
  CHECK(sm.on_offline(t += 1000));
  const std::int64_t down = t;
  CHECK(sm.state() == FeedState::Offline);
  CHECK(!sm.replay_active());
  sm.on_connecting(t += 500);  // reconnect attempts do not reset the timer
  sm.on_offline(t += 500);
  CHECK(!sm.tick(down + 14999));
  CHECK(!sm.replay_active());
  CHECK(!sm.tick(down + 15000));
  CHECK(sm.tick(down + 15001));
  CHECK(sm.replay_active());
  CHECK(sm.label() == SourceLabel::Replay);
  CHECK(sm.label(SourceLabel::Delayed) == SourceLabel::Replay);
  // Still trying to reconnect while the capture stands in.
  sm.on_connecting(down + 16000);
  CHECK(sm.replay_active());
  sm.on_offline(down + 17000);
  CHECK(sm.replay_active());

  // Recovery: the first valid live message ends the fallback.
  CHECK(sm.on_message(down + 40000));
  CHECK(sm.state() == FeedState::Live);
  CHECK(!sm.replay_active());
  CHECK(sm.label() == SourceLabel::Live);
}

TEST_CASE("live: rate limiting also triggers the fallback; a slow first connect does not") {
  FeedStateMachine sm(15000, 30000);
  sm.on_connecting(0);
  CHECK(!sm.tick(60000));  // CONNECTING alone is not a failure
  CHECK(!sm.replay_active());
  sm.on_message(60500);
  CHECK(sm.on_rate_limited(61000));
  CHECK(sm.state() == FeedState::RateLimited);
  CHECK(!sm.tick(61000 + 15000));
  CHECK(sm.tick(61000 + 15001));
  CHECK(sm.replay_active());
  CHECK(sm.on_message(100000));
  CHECK(!sm.replay_active());
}

TEST_CASE("live: a feed that never connects falls back 15 s after its first failure") {
  FeedStateMachine sm(15000, 30000);
  sm.on_connecting(1000);
  sm.on_offline(1200);
  sm.on_connecting(1700);
  sm.on_offline(1900);
  CHECK(!sm.tick(16000));
  CHECK(sm.tick(16300));
  CHECK(sm.replay_active());
  CHECK(sm.state() == FeedState::Offline);
}

TEST_CASE("live: stale watchdog degrades a silent feed; parse errors degrade without fallback") {
  FeedStateMachine sm(15000, 30000);
  sm.on_connecting(0);
  sm.on_message(100);
  CHECK(!sm.tick(30100));
  CHECK(sm.tick(30101));
  CHECK(sm.state() == FeedState::Degraded);
  CHECK(!sm.tick(200000));
  CHECK(!sm.replay_active());  // DEGRADED is still a connection; no fallback
  CHECK(sm.on_message(200100));
  CHECK(sm.state() == FeedState::Live);
  CHECK(sm.on_degraded(200200));
  CHECK(sm.state() == FeedState::Degraded);
  CHECK(!sm.replay_active());
}

TEST_CASE("live: GEO_BLOCKED leaves the feed out: no replay stand-in, a message recovers it") {
  FeedStateMachine sm(15000, 30000);
  sm.on_connecting(0);
  CHECK(sm.on_geo_blocked(200));
  CHECK(sm.state() == FeedState::GeoBlocked);
  CHECK(std::string(kFeedStateNames[static_cast<std::size_t>(sm.state())]) == "GEO_BLOCKED");
  CHECK(!sm.tick(60000));
  CHECK(!sm.tick(3600000));
  CHECK(!sm.replay_active());  // a refused venue is never replaced by synthetic data
  CHECK(sm.label() == SourceLabel::Live);
  // A later re-check that gets through.
  sm.on_connecting(3600100);
  CHECK(sm.on_message(3600200));
  CHECK(sm.state() == FeedState::Live);
  // An ordinary failure after that falls back as usual.
  sm.on_offline(3601000);
  CHECK(sm.tick(3616100));
  CHECK(sm.replay_active());
}

TEST_CASE("live: Binance USD-M forceOrder liquidations") {
  Env e;
  e.ctx.hubs.quote_hub[static_cast<std::size_t>(Asset::USDT)] = 0;
  {
    Fixture f("binance_force_order.json");
    REQUIRE(f.ok);
    const ParseResult r = parse_binance_force_order(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
    CHECK(r.status == ParseStatus::Ok);
    REQUIRE(e.batch.size() == 1);
    const LiveEvent& ev = e.batch[0];
    CHECK(ev.feed == FeedId::BinanceLiq);
    CHECK(ev.kind == EventKind::Liquidation);
    CHECK(ev.asset == Asset::BTC);
    CHECK(quote_of(ev) == Asset::USDT);
    CHECK(near(ev.amount, 0.5));            // filled quantity
    CHECK(near(ev.price, 59812.40));        // average price
    CHECK(near(ev.usd_value, 0.5 * 59812.40));
    CHECK((ev.flags & kEvBuy) == 0);        // SELL order: a LONG was liquidated
    CHECK(ev.src_hub == 8 && ev.dst_hub == 0);
    CHECK_EQ(ev.ts_ms, std::int64_t{1794717615100});
  }
  {
    Fixture f("binance_force_order_alt.json");
    REQUIRE(f.ok);
    e.batch.clear();
    const ParseResult r = parse_binance_force_order(e.parser, f.buf.data(), f.len, e.ctx, e.batch);
    CHECK(r.status == ParseStatus::Ok);  // another market: acknowledged, no event
    CHECK(r.transactions_seen == 1);
    CHECK(e.batch.size() == 0);
  }
  {
    Fixture f("malformed.json");
    REQUIRE(f.ok);
    e.batch.clear();
    CHECK(parse_binance_force_order(e.parser, f.buf.data(), f.len, e.ctx, e.batch).status != ParseStatus::Ok);
    CHECK(e.batch.size() == 0);
  }
}

TEST_CASE("live: OKX instruments, trades, liquidation orders, error and ack") {
  Env e;
  e.ctx.hubs.quote_hub[static_cast<std::size_t>(Asset::USDT)] = 0;
  OkxContracts contracts;
  {
    Fixture f("okx_instruments.json");
    REQUIRE(f.ok);
    CHECK(parse_okx_instruments(e.parser, f.buf.data(), f.len, contracts).status == ParseStatus::Ok);
    CHECK(contracts.count == 2);  // BTC linear and inverse; XRP is not valued
    const OkxContract* lin = contracts.find("BTC-USDT-SWAP");
    const OkxContract* inv = contracts.find("BTC-USD-SWAP");
    REQUIRE(lin != nullptr && inv != nullptr);
    CHECK(!lin->inverse && near(lin->ct_val, 0.01));
    CHECK(inv->inverse && near(inv->ct_val, 100.0));
  }
  {
    Fixture f("okx_trades.json");
    REQUIRE(f.ok);
    const ParseResult r = parse_okx(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices, &contracts);
    CHECK(r.status == ParseStatus::Ok);
    REQUIRE(e.batch.size() == 1);
    const LiveEvent& ev = e.batch[0];
    CHECK(ev.feed == FeedId::Okx);
    CHECK(ev.kind == EventKind::Trade);
    CHECK(ev.asset == Asset::ETH);
    CHECK(quote_of(ev) == Asset::USDT);
    CHECK(near(ev.amount, 2.5));
    CHECK(near(ev.usd_value, 2.5 * 3010.25));
    CHECK((ev.flags & kEvBuy) != 0);
    CHECK(ev.src_hub == 0 && ev.dst_hub == 7);  // USDT home hub -> ETH
    CHECK_EQ(ev.ts_ms, std::int64_t{1794717615500});
  }
  {
    Fixture f("okx_liquidation.json");
    REQUIRE(f.ok);
    e.batch.clear();
    const ParseResult r = parse_okx(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices, &contracts);
    CHECK(r.status == ParseStatus::Ok);
    CHECK(r.transactions_seen == 3);
    REQUIRE(e.batch.size() == 2);  // LTC has no known contract size: counted, not valued
    const LiveEvent& lin = e.batch[0];
    CHECK(lin.kind == EventKind::Liquidation);
    CHECK(near(lin.amount, 150 * 0.01));         // 150 contracts x 0.01 BTC
    CHECK(near(lin.usd_value, 1.5 * 60000.0));
    CHECK((lin.flags & kEvBuy) == 0);            // sell: a LONG was closed
    const LiveEvent& inv = e.batch[1];
    CHECK(near(inv.amount, 30 * 100.0 / 60000.0));  // 30 contracts x 100 USD
    CHECK(near(inv.usd_value, 3000.0, 1e-6));
    CHECK((inv.flags & kEvBuy) != 0);            // buy: a SHORT was closed
  }
  {
    Fixture f("okx_error.json");
    REQUIRE(f.ok);
    e.batch.clear();
    CHECK(parse_okx(e.parser, f.buf.data(), f.len, e.ctx, e.batch, &e.prices, &contracts).status ==
          ParseStatus::RateLimited);
    Fixture a("okx_sub_ack.json");
    REQUIRE(a.ok);
    CHECK(parse_okx(e.parser, a.buf.data(), a.len, e.ctx, e.batch, &e.prices, &contracts).status == ParseStatus::Ok);
    CHECK(e.batch.size() == 0);
    const char pong[] = "pong";
    FeedBuffer pb{64};
    const std::size_t pl = pb.assign(std::string_view(pong, 4));
    CHECK(parse_okx(e.parser, pb.data(), pl, e.ctx, e.batch, &e.prices, &contracts).status == ParseStatus::Ok);
  }
}
