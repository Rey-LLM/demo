#pragma once
// Message parsers for the public feeds, built on simdjson On-Demand. Each
// takes one complete message (a WebSocket text frame or an HTTP body) and
// appends LiveEvents to a fixed-capacity batch, plus control information the
// adapter acts on (a block number to fetch, a rejected subscription, a rate
// limit). They never allocate after the parser's first use and never block.
//
// Input buffers must have simdjson::SIMDJSON_PADDING readable bytes after
// the message (FeedBuffer below provides that). Schemas follow each
// provider's public documentation; tests/fixtures/live/ holds one fixture
// per message type.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "ingest/live/live_event.hpp"
#include "ingest/live/price_book.hpp"

namespace ftv {

// Hub indices the adapters need (config/hubs.json order), resolved once.
struct FeedHubs {
  std::uint16_t chain_hub[3] = {0, 0, 0};          // BTC, ETH, SOL chain-cloud hubs
  std::array<std::uint16_t, kAssetCount> quote_hub{};  // fiat quote currency -> its home financial centre
};

struct ParseContext {
  const PriceBook* prices = nullptr;
  FeedHubs hubs;
  std::int64_t recv_ms = 0;  // receipt time, used when a message has no timestamp
  SourceLabel label = SourceLabel::Live;
  std::uint32_t sample_cap = 64;  // SOL: at most this many non-vote transactions per block
};

class EventBatch {
 public:
  static constexpr std::size_t kCapacity = 8192;
  EventBatch() : events_(std::make_unique<LiveEvent[]>(kCapacity)) {}
  void clear() noexcept {
    size_ = 0;
    overflow_ = 0;
  }
  void push(const LiveEvent& e) noexcept {
    if (size_ < kCapacity) {
      events_[size_++] = e;
    } else {
      ++overflow_;
    }
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t overflow() const noexcept { return overflow_; }
  [[nodiscard]] const LiveEvent& operator[](std::size_t i) const noexcept { return events_[i]; }
  [[nodiscard]] LiveEvent& at(std::size_t i) noexcept { return events_[i]; }

 private:
  std::unique_ptr<LiveEvent[]> events_;
  std::size_t size_ = 0;
  std::size_t overflow_ = 0;
};

enum class ParseStatus : std::uint8_t {
  Ok = 0,        // understood (it may have produced no events: acks, heartbeats)
  Ignored,       // valid JSON of a type this adapter does not use
  Malformed,     // not valid JSON or missing required fields
  Rejected,      // the endpoint rejected a method or subscription
  RateLimited,   // the endpoint reported a rate limit
};

struct ParseResult {
  ParseStatus status = ParseStatus::Ok;
  std::int64_t rpc_id = -1;          // JSON-RPC id of a response (-1 for notifications)
  std::uint64_t block_number = 0;    // ETH newHeads: block to fetch; SOL slotNotification: slot
  bool block_announced = false;      // a newHeads / slot notification arrived
  std::uint32_t transactions_seen = 0;  // before filtering and sampling (block responses)
};

// Opaque simdjson parser wrapper (keeps simdjson out of most headers).
class JsonParser {
 public:
  JsonParser();
  ~JsonParser();
  JsonParser(const JsonParser&) = delete;
  JsonParser& operator=(const JsonParser&) = delete;
  struct Impl;
  Impl& impl() noexcept { return *impl_; }

 private:
  std::unique_ptr<Impl> impl_;
};

// Message buffer with the padding simdjson needs.
class FeedBuffer {
 public:
  explicit FeedBuffer(std::size_t capacity);
  [[nodiscard]] char* data() noexcept { return data_.get(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }  // usable bytes (excl. padding)
  // Copies `s` in (truncates to capacity); returns the stored length.
  std::size_t assign(std::string_view s) noexcept;

 private:
  std::unique_ptr<char[]> data_;
  std::size_t capacity_;
};

// blockchain.com WebSocket API: {"op":"utx","x":{...}} unconfirmed transactions.
ParseResult parse_blockchain_com(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx,
                                 EventBatch& out);
// mempool.space WebSocket: {"block":{...}} or {"blocks":[...]} after {"action":"want","data":["blocks"]}.
ParseResult parse_mempool_space(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx,
                                EventBatch& out);
// Ethereum JSON-RPC over WebSocket: eth_subscription newHeads notifications,
// eth_getBlockByNumber(n, true) results, subscription acks and errors.
ParseResult parse_eth_rpc(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out);
// Solana: slotNotification (WebSocket) and getBlock results (HTTP JSON-RPC).
ParseResult parse_sol_rpc(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out);
// Coinbase Exchange WebSocket feed: match / last_match / ticker / subscriptions / error.
ParseResult parse_coinbase(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out,
                           PriceBook* prices);
// Kraken WebSocket API v2: trade / ticker channel messages, heartbeat, status, method responses.
ParseResult parse_kraken(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out,
                         PriceBook* prices);

// Binance combined stream (wss://.../stream?streams=btcusdt@aggTrade/...):
// {"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","s":"BTCUSDT","p":..,"q":..,"T":..,"m":..}}
// and bare aggTrade / trade objects from a single-stream connection.
ParseResult parse_binance(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out,
                          PriceBook* prices);
// ECB reference rates via Frankfurter (GET /v1/latest?base=USD&symbols=...):
// {"amount":1.0,"base":"USD","date":"2026-11-13","rates":{"EUR":0.93,...}}.
// Fills `board` (USD per unit) and seeds `prices` for currencies that have
// no exchange-implied rate from the last two minutes. r.block_number = date as YYYYMMDD.
ParseResult parse_ecb_rates(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx,
                            PriceBook* prices, FxBoard* board);

// Helpers shared with the tests.
bool parse_hex_u64(std::string_view s, std::uint64_t& out) noexcept;       // "0x1b4"
double hex_to_double(std::string_view s) noexcept;                          // big "0x..." values (wei)
bool split_pair(std::string_view symbol, Asset& base, Asset& quote) noexcept;  // "BTC-USD", "BTC/USD"
bool split_concat_pair(std::string_view symbol, Asset& base, Asset& quote) noexcept;  // "BTCUSDT", "ethusdt"
bool asset_from_code(std::string_view code, Asset& out) noexcept;

}  // namespace ftv
