// Feed adapter threads: connect, subscribe, parse, push. One thread per feed.
//
// Every adapter follows the same loop:
//   CONNECTING -> open the WebSocket (TLS, verified) -> send the subscribe
//   messages -> read frames with a 250 ms timeout so the state machine's
//   watchdog and fallback timers tick at 4 Hz -> on failure, OFFLINE (or
//   RATE_LIMITED) and reconnect after a full-jitter backoff (0.5 s .. 30 s).
// ETH and SOL also make follow-up requests (eth_getBlockByNumber on the same
// socket; getBlock over HTTPS), limited by a token bucket to the configured
// requests per second (at most 5).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/log.hpp"
#include "ingest/live/live_runtime.hpp"
#include "ingest/live/net.hpp"

namespace ftv {

namespace {

// Token bucket: `rate` tokens per second, burst 1.
class RateLimiter {
 public:
  explicit RateLimiter(double rate) : rate_(std::clamp(rate, 0.1, 5.0)) {}
  bool try_take(std::int64_t nowMs) noexcept {
    if (last_ < 0) last_ = nowMs;
    tokens_ = std::min(1.0, tokens_ + static_cast<double>(nowMs - last_) * 1e-3 * rate_);
    last_ = nowMs;
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
  }

 private:
  double rate_;
  double tokens_ = 1.0;
  std::int64_t last_ = -1;
};

class Adapter {
 public:
  Adapter(LiveFeeds::Context& c, std::stop_token st)
      : c_(c),
        st_(std::move(st)),
        id_(c.cfg->id),
        sm_(static_cast<std::int64_t>(c.all->fallback_after_s * 1000.0),
            static_cast<std::int64_t>(c.all->stale_after_s * 1000.0)),
        backoff_(0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(c.cfg->id) + 1) * 0xBF58476D1CE4E5B9ull,
                 c.all->backoff_base_s, c.all->backoff_cap_s),
        ws_(c.all->max_frame_bytes),
        buf_(c.all->max_frame_bytes),
        http_buf_(id_ == FeedId::Sol ? c.all->max_frame_bytes : 16),
        limiter_(c.cfg->max_requests_per_s) {
    ctx_.prices = c.prices;
    ctx_.hubs = c.hubs;
    ctx_.label = c.cfg->label;
    ctx_.sample_cap = c.cfg->sample_cap;
  }

  void run() {
    net::Url url;
    if (!net::parse_url(c_.cfg->url, url)) {
      FTV_LOG_ERROR("[live] %s: bad url %s", feed_name(id_).data(), c_.cfg->url.c_str());
      note(sm_.on_offline(utc_now_ms()));
      while (!st_.stop_requested()) wait_ms(250);
      return;
    }
    if (id_ == FeedId::Sol) net::parse_url(c_.cfg->rpc_url, rpc_url_);
    while (!st_.stop_requested()) {
      note(sm_.on_connecting(utc_now_ms()));
      std::string detail;
      const net::NetError err = ws_.open(url, 10000, detail);
      if (err != net::NetError::None) {
        fail(err, detail);
        continue;
      }
      if (!subscribe()) {
        fail(net::NetError::Closed, "subscribe failed");
        continue;
      }
      read_loop();
      ws_.close();
      if (id_ == FeedId::Sol) http_.close();
    }
    ws_.close();
  }

 private:
  void note(bool changed) {
    const std::int64_t now = utc_now_ms();
    c_.status->state.store(static_cast<std::uint8_t>(sm_.state()), std::memory_order_relaxed);
    c_.status->replay.store(sm_.replay_active(), std::memory_order_relaxed);
    if (changed) {
      const FeedTransition t{now, sm_.state(), sm_.replay_active()};
      (void)c_.transitions->try_push(t);  // 64 slots; the model drains them every 10 ms
    }
  }

  // Sleeps in 250 ms slices so the fallback timer keeps ticking.
  void wait_ms(std::int64_t ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!st_.stop_requested() && std::chrono::steady_clock::now() < until) {
      note(sm_.tick(utc_now_ms()));
      std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(ms, 250)));
    }
  }

  void fail(net::NetError err, const std::string& detail) {
    c_.status->last_error.store(static_cast<std::uint8_t>(err), std::memory_order_relaxed);
    c_.status->reconnects.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t now = utc_now_ms();
    note(err == net::NetError::RateLimited ? sm_.on_rate_limited(now) : sm_.on_offline(now));
    if (!logged_once_ || backoff_.attempt() % 8 == 0) {
      FTV_LOG_WARN("[live] %s: %s%s%s", feed_name(id_).data(), net::net_error_name(err), detail.empty() ? "" : ": ",
                   detail.c_str());
      logged_once_ = true;
    }
    ws_.close();
    const double s = backoff_.next_s();
    wait_ms(static_cast<std::int64_t>(s * 1000.0));
  }

  bool send(const std::string& text) { return ws_.send_text(text, 5000) == net::NetError::None; }

  bool subscribe() {
    for (const std::string& m : c_.cfg->subscribe) {
      if (!send(m)) return false;
    }
    if (id_ == FeedId::Eth) {
      if (!send(R"({"jsonrpc":"2.0","id":1,"method":"eth_subscribe","params":["newHeads"]})")) return false;
      if (c_.cfg->try_pending && pending_ok_ &&
          !send(R"({"jsonrpc":"2.0","id":2,"method":"eth_subscribe","params":["newPendingTransactions"]})")) {
        return false;
      }
    } else if (id_ == FeedId::Sol) {
      if (!send(R"({"jsonrpc":"2.0","id":1,"method":"slotSubscribe"})")) return false;
    }
    return true;
  }

  ParseResult parse(const char* msg, std::size_t len) {
    batch_.clear();
    ctx_.recv_ms = utc_now_ms();
    switch (id_) {
      case FeedId::BtcMempool: return parse_blockchain_com(parser_, msg, len, ctx_, batch_);
      case FeedId::BtcBlocks: return parse_mempool_space(parser_, msg, len, ctx_, batch_);
      case FeedId::Eth: return parse_eth_rpc(parser_, msg, len, ctx_, batch_);
      case FeedId::Sol: return parse_sol_rpc(parser_, msg, len, ctx_, batch_);
      case FeedId::Coinbase: return parse_coinbase(parser_, msg, len, ctx_, batch_, c_.prices);
      case FeedId::Kraken: return parse_kraken(parser_, msg, len, ctx_, batch_, c_.prices);
      default: return {ParseStatus::Ignored};
    }
  }

  void push_batch() {
    FeedStatus& s = *c_.status;
    std::uint64_t pushed = 0, dropped = batch_.overflow();
    for (std::size_t i = 0; i < batch_.size(); ++i) {
      if (c_.ring->try_push(batch_[i])) {
        ++pushed;
      } else {
        ++dropped;
      }
    }
    s.events.fetch_add(pushed, std::memory_order_relaxed);
    if (dropped) s.drops.fetch_add(dropped, std::memory_order_relaxed);
  }

  // Returns false when the connection must be dropped.
  bool handle(const ParseResult& r) {
    const std::int64_t now = utc_now_ms();
    FeedStatus& s = *c_.status;
    switch (r.status) {
      case ParseStatus::Ok:
      case ParseStatus::Ignored:
        s.messages.fetch_add(1, std::memory_order_relaxed);
        s.last_message_ms.store(now, std::memory_order_relaxed);
        note(sm_.on_message(now));
        backoff_.reset();
        push_batch();
        return true;
      case ParseStatus::Malformed:
        s.malformed.fetch_add(1, std::memory_order_relaxed);
        note(sm_.on_degraded(now));
        return true;
      case ParseStatus::Rejected:
        if (id_ == FeedId::Eth && r.rpc_id == 2) {
          // newPendingTransactions is optional: providers may refuse it.
          pending_ok_ = false;
          FTV_LOG_INFO("[live] ETH: newPendingTransactions rejected; using newHeads + getBlockByNumber only");
          return true;
        }
        note(sm_.on_degraded(now));
        return true;
      case ParseStatus::RateLimited:
        c_.status->last_error.store(static_cast<std::uint8_t>(net::NetError::RateLimited), std::memory_order_relaxed);
        note(sm_.on_rate_limited(now));
        return false;
    }
    return true;
  }

  void eth_follow_up(const ParseResult& r) {
    if (!r.block_announced || !limiter_.try_take(utc_now_ms())) return;
    char req[160];
    const int n = std::snprintf(req, sizeof req,
                                R"({"jsonrpc":"2.0","id":%u,"method":"eth_getBlockByNumber","params":["0x%llx",true]})",
                                100u + (next_id_++ % 1000u), static_cast<unsigned long long>(r.block_number));
    if (n > 0) (void)ws_.send_text(std::string_view(req, static_cast<std::size_t>(n)), 5000);
  }

  // SOL: fetch the block `slot_lag` slots behind the announced slot (DELAYED).
  bool sol_follow_up(const ParseResult& r) {
    if (!r.block_announced || r.block_number <= c_.cfg->slot_lag || rpc_url_.host.empty()) return true;
    if (!limiter_.try_take(utc_now_ms())) return true;
    const unsigned long long slot = r.block_number - c_.cfg->slot_lag;
    char req[320];
    const int n = std::snprintf(
        req, sizeof req,
        R"({"jsonrpc":"2.0","id":%u,"method":"getBlock","params":[%llu,{"encoding":"json","maxSupportedTransactionVersion":0,"transactionDetails":"full","rewards":false,"commitment":"confirmed"}]})",
        100u + (next_id_++ % 1000u), slot);
    if (n <= 0) return true;
    std::size_t len = 0;
    int status = 0;
    std::string detail;
    const net::NetError e = http_.post_json(rpc_url_, std::string_view(req, static_cast<std::size_t>(n)),
                                            http_buf_.data(), http_buf_.capacity(), len, status, 15000, detail);
    if (e == net::NetError::RateLimited) {
      note(sm_.on_rate_limited(utc_now_ms()));
      return false;
    }
    if (e == net::NetError::TooLarge) {
      c_.status->drops.fetch_add(1, std::memory_order_relaxed);  // a block over the 8 MB limit is skipped
      return true;
    }
    if (e != net::NetError::None) {
      http_.close();
      return true;  // the WebSocket is still healthy; the next slot retries
    }
    http_buf_.data()[len] = '\0';
    return handle(parse(http_buf_.data(), len));
  }

  void read_loop() {
    while (!st_.stop_requested()) {
      std::size_t len = 0;
      const net::NetError err = ws_.recv(buf_.data(), buf_.capacity(), len, 250);
      const std::int64_t now = utc_now_ms();
      note(sm_.tick(now));
      if (err == net::NetError::Timeout) {
        // Stale watchdog: silent for stale_after_s -> reconnect.
        const std::int64_t last = sm_.last_message_ms();
        const std::int64_t ref = last >= 0 ? last : sm_.since_ms();
        if (now - ref > static_cast<std::int64_t>(c_.all->stale_after_s * 1000.0)) {
          fail(net::NetError::Timeout, "no data within stale_after_s");
          return;
        }
        continue;
      }
      if (err != net::NetError::None) {
        fail(err, std::string());
        return;
      }
      const ParseResult r = parse(buf_.data(), len);
      if (!handle(r)) {
        fail(net::NetError::RateLimited, "provider rate limit");
        return;
      }
      if (id_ == FeedId::Eth) {
        eth_follow_up(r);
      } else if (id_ == FeedId::Sol && !sol_follow_up(r)) {
        fail(net::NetError::RateLimited, "getBlock rate limit");
        return;
      }
    }
  }

  LiveFeeds::Context& c_;
  std::stop_token st_;
  FeedId id_;
  FeedStateMachine sm_;
  Backoff backoff_;
  net::WebSocket ws_;
  net::HttpsClient http_;
  net::Url rpc_url_;
  FeedBuffer buf_;
  FeedBuffer http_buf_;
  JsonParser parser_;
  EventBatch batch_;
  ParseContext ctx_;
  RateLimiter limiter_;
  unsigned next_id_ = 0;
  bool pending_ok_ = true;
  bool logged_once_ = false;
};

}  // namespace

void run_feed_adapter(LiveFeeds::Context& c, std::stop_token stop) {
  Adapter a(c, std::move(stop));
  a.run();
}

}  // namespace ftv
