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
//
// A feed with fallback_urls moves to the next URL when a connection attempt
// fails before any data arrived (DNS, TCP, proxy, TLS, HTTP status such as
// Binance's 451 for restricted regions) and wraps around after the last.
//
// FX is not a stream: it GETs the ECB reference rates every poll_s seconds.
//
// ETH and SOL use an endpoint pool (endpoint_pool.hpp): the active endpoint's
// token bucket gates every follow-up request; a connect failure, timeout or
// 429 cools that endpoint down and rotates to the next healthy one. Rotations
// are logged (RotationLog) for the HUD and the export.
//
// GEO_BLOCKED: when every URL of a feed answered 451 (or 403 at the upgrade),
// the venue refuses this region. The feed is marked GEO_BLOCKED, gets no
// REPLAY stand-in, and is re-checked every 10 minutes.
//
// OKX: contract sizes are fetched from the public instruments endpoint before
// the WebSocket opens; a "ping" text frame keeps the connection alive.
//
// Replay fallback policy (feeds.json "fallback_policy"): by default the
// capture stands in for a feed only while no feed at all is LIVE, i.e. the
// machine is offline; "per_feed" restores the v2 behaviour.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

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
        http_buf_(id_ == FeedId::Sol || id_ == FeedId::Okx ? c.all->max_frame_bytes : id_ == FeedId::Fx ? 65536 : 16),
        limiter_(c.cfg->max_requests_per_s),
        pool_(c.cfg->pool) {
    ctx_.prices = c.prices;
    ctx_.hubs = c.hubs;
    ctx_.label = c.cfg->label;
    ctx_.sample_cap = c.cfg->sample_cap;
  }

  void run() {
    // The primary URL, then the fallbacks; or the pool's endpoints in order.
    if (pool_.size() > 0) {
      for (std::size_t i = 0; i < pool_.size(); ++i) urls_.push_back(pool_.endpoint(i).url);
    } else {
      urls_.push_back(c_.cfg->url);
      for (const std::string& u : c_.cfg->fallback_urls) urls_.push_back(u);
    }
    c_.status->url_count.store(static_cast<std::uint8_t>(urls_.size()), std::memory_order_relaxed);
    for (const std::string& u : urls_) {
      net::Url parsed;
      if (!net::parse_url(u, parsed)) {
        FTV_LOG_ERROR("[live] %s: bad url %s", feed_name(id_).data(), u.c_str());
        note(sm_.on_offline(utc_now_ms()));
        while (!st_.stop_requested()) wait_ms(250);
        return;
      }
      parsed_.push_back(parsed);
    }
    if (id_ == FeedId::Sol && pool_.size() == 0) net::parse_url(c_.cfg->rpc_url, rpc_url_);
    if (id_ == FeedId::Fx) {
      run_poller();
      return;
    }
    while (!st_.stop_requested()) {
      note(sm_.on_connecting(utc_now_ms()));
      if (pool_.size() > 0) select_pool_endpoint();
      if (!c_.cfg->meta_url.empty() && !fetch_meta()) continue;
      std::string detail;
      const std::int64_t t0 = utc_now_ms();
      const net::NetError err = ws_.open(parsed_[url_index_], 10000, detail);
      if (err != net::NetError::None) {
        if (err == net::NetError::GeoBlocked) {
          if (++geo_refusals_ >= urls_.size()) {
            geo_blocked(detail);
            continue;
          }
        } else {
          geo_refusals_ = 0;
        }
        if (pool_.size() > 0) {
          pool_failure(err == net::NetError::RateLimited);
        } else {
          next_url();
        }
        fail(err, detail);
        continue;
      }
      geo_refusals_ = 0;
      if (pool_.size() > 0) pool_.on_success(utc_now_ms(), static_cast<double>(utc_now_ms() - t0));
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
  void next_url() {
    if (parsed_.size() < 2) return;
    url_index_ = (url_index_ + 1) % parsed_.size();
    c_.status->url_index.store(static_cast<std::uint8_t>(url_index_), std::memory_order_relaxed);
    FTV_LOG_INFO("[live] %s: trying %s", feed_name(id_).data(), urls_[url_index_].c_str());
  }

  // Pool: point url_index_ / rpc_url_ at the active endpoint.
  void select_pool_endpoint() {
    url_index_ = pool_.active();
    c_.status->url_index.store(static_cast<std::uint8_t>(url_index_), std::memory_order_relaxed);
    rpc_url_ = net::Url{};
    const std::string& rpc = pool_.current().rpc_url;
    if (!rpc.empty()) net::parse_url(rpc, rpc_url_);
    pending_ok_ = pool_.current().pending_tx;
    const std::int64_t wait = pool_.wait_ms(utc_now_ms());
    if (wait > 0) wait_ms(wait);  // every endpoint is cooling down: wait for the first one
  }

  // Pool: a provider-side failure on the active endpoint.
  void pool_failure(bool rateLimited) {
    const std::int64_t now = utc_now_ms();
    const std::size_t from = pool_.active();
    const std::size_t to = rateLimited ? pool_.on_rate_limited(now) : pool_.on_error(now);
    c_.status->rotations.fetch_add(1, std::memory_order_relaxed);
    if (c_.rotations) {
      RotationRecord rec;
      rec.at_ms = now;
      rec.feed = id_;
      rec.from = static_cast<std::uint8_t>(from);
      rec.to = static_cast<std::uint8_t>(to);
      rec.reason = rateLimited ? RotationReason::RateLimited : RotationReason::Error;
      c_.rotations->push(rec);
    }
    FTV_LOG_INFO("[live] %s: endpoint %zu -> %zu (%s): %s", feed_name(id_).data(), from, to,
                 rateLimited ? "rate limited" : "error", urls_[to].c_str());
  }

  void geo_blocked(const std::string& detail) {
    c_.status->last_error.store(static_cast<std::uint8_t>(net::NetError::GeoBlocked), std::memory_order_relaxed);
    note(sm_.on_geo_blocked(utc_now_ms()));
    FTV_LOG_WARN("[live] %s: GEO_BLOCKED (%s on every URL); continuing without it, re-check in 10 min",
                 feed_name(id_).data(), detail.c_str());
    ws_.close();
    geo_refusals_ = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (!st_.stop_requested() && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }

  // OKX: contract sizes from the public instruments endpoint (once per connection attempt
  // until it succeeds; kept afterwards).
  bool fetch_meta() {
    if (okx_contracts_.count > 0) return true;
    net::Url u;
    if (!net::parse_url(c_.cfg->meta_url, u)) return true;
    std::size_t len = 0;
    int status = 0;
    std::string detail;
    const net::NetError e = http_.get(u, http_buf_.data(), http_buf_.capacity(), len, status, 15000, detail);
    http_.close();
    if (e != net::NetError::None) {
      if (e == net::NetError::GeoBlocked) {
        geo_blocked(detail);
      } else {
        fail(e, "instruments: " + detail);
      }
      return false;
    }
    http_buf_.data()[len] = '\0';
    const ParseResult r = parse_okx_instruments(parser_, http_buf_.data(), len, okx_contracts_);
    if (r.status != ParseStatus::Ok) {
      // Trades still flow; liquidations stay unvalued (counted, no events).
      FTV_LOG_WARN("[live] %s: no contract sizes from the instruments endpoint", feed_name(id_).data());
    }
    return true;
  }

  // Is any feed (this one included) receiving data right now?
  bool any_feed_live() const noexcept {
    if (!c_.all_status) return false;
    for (std::size_t f = 0; f < kFeedCount; ++f) {
      if (!c_.all->feeds[f].enabled || static_cast<FeedId>(f) == FeedId::Fiat) continue;
      const auto s = static_cast<FeedState>((*c_.all_status)[f].state.load(std::memory_order_relaxed));
      if (s == FeedState::Live || s == FeedState::Degraded) return true;
    }
    return false;
  }

  void note(bool changed) {
    const std::int64_t now = utc_now_ms();
    // The state machine decides when this feed would need the capture; the
    // policy decides whether it gets it (by default only when nothing is LIVE).
    const bool replay = sm_.replay_active() && (c_.all->fallback_per_feed || !any_feed_live());
    c_.status->state.store(static_cast<std::uint8_t>(sm_.state()), std::memory_order_relaxed);
    c_.status->replay.store(replay, std::memory_order_relaxed);
    if (changed || replay != replay_published_) {
      replay_published_ = replay;
      const FeedTransition t{now, sm_.state(), replay};
      (void)c_.transitions->try_push(t);  // 64 slots; the model drains them every 10 ms
    }
  }

  // FX: one HTTPS GET every poll_s seconds (retries follow the backoff).
  void run_poller() {
    note(sm_.on_connecting(utc_now_ms()));
    while (!st_.stop_requested()) {
      std::size_t len = 0;
      int status = 0;
      std::string detail;
      const net::NetError e = http_.get(parsed_[url_index_], http_buf_.data(), http_buf_.capacity(), len, status,
                                        15000, detail);
      http_.close();  // one request per poll: no idle keep-alive connection
      if (e != net::NetError::None) {
        if (e != net::NetError::RateLimited) next_url();
        fail(e, detail);
        continue;
      }
      http_buf_.data()[len] = '\0';
      ParseResult r;
      batch_.clear();
      ctx_.recv_ms = utc_now_ms();
      r = parse_ecb_rates(parser_, http_buf_.data(), len, ctx_, c_.prices, c_.fx);
      if (r.status != ParseStatus::Ok) {
        fail(r.status == ParseStatus::RateLimited ? net::NetError::RateLimited : net::NetError::Protocol,
             "unexpected FX response");
        continue;
      }
      handle(r);
      // Between polls the feed stays LIVE; the stale watchdog does not apply.
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(static_cast<std::int64_t>(c_.cfg->poll_s * 1000.0));
      while (!st_.stop_requested() && std::chrono::steady_clock::now() < until) {
        sm_.on_message(utc_now_ms());
        note(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
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
      case FeedId::Binance: return parse_binance(parser_, msg, len, ctx_, batch_, c_.prices);
      case FeedId::BinanceLiq: return parse_binance_force_order(parser_, msg, len, ctx_, batch_);
      case FeedId::Okx: return parse_okx(parser_, msg, len, ctx_, batch_, c_.prices, &okx_contracts_);
      default: return {ParseStatus::Ignored};
    }
  }

  void push_batch() {
    FeedStatus& s = *c_.status;
    std::uint64_t pushed = 0, dropped = batch_.overflow();
    for (std::size_t i = 0; i < batch_.size(); ++i) {
      const LiveEvent& e = batch_[i];
      // Latency: only flow events stamped by the source (blocks carry miner
      // time, which can be minutes off, and are left out).
      if ((e.kind == EventKind::Transaction || e.kind == EventKind::Trade) && e.ts_ms != ctx_.recv_ms) {
        const double lag = static_cast<double>(std::clamp<std::int64_t>(ctx_.recv_ms - e.ts_ms, 0, 600000));
        latency_ema_ = latency_ema_ < 0.0 ? lag : latency_ema_ + 0.05 * (lag - latency_ema_);
        s.latency_ms.store(static_cast<std::int32_t>(latency_ema_ + 0.5), std::memory_order_relaxed);
      }
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

  // A request may be sent now: the pool's active endpoint bucket, or the feed's own limiter.
  bool take_token() {
    const std::int64_t now = utc_now_ms();
    return pool_.size() > 0 ? pool_.try_acquire(now) : limiter_.try_take(now);
  }

  // Response to a follow-up request: latency for the pool's health score.
  void note_response(const ParseResult& r) {
    if (pool_.size() == 0 || r.rpc_id < 100) return;
    const std::size_t slot = static_cast<std::size_t>(r.rpc_id) % sent_ms_.size();
    if (sent_ms_[slot] <= 0) return;
    const double lat = static_cast<double>(utc_now_ms() - sent_ms_[slot]);
    sent_ms_[slot] = 0;
    if (r.status == ParseStatus::Ok || r.status == ParseStatus::Ignored) {
      pool_.on_success(utc_now_ms(), lat);
      const double l = pool_.health(pool_.active()).latency_ewma_ms;
      c_.status->rpc_latency_ms.store(static_cast<std::int32_t>(l + 0.5), std::memory_order_relaxed);
    }
  }

  void eth_follow_up(const ParseResult& r) {
    if (!r.block_announced || !take_token()) return;
    char req[160];
    const unsigned id = 100u + (next_id_++ % 1000u);
    const int n = std::snprintf(req, sizeof req,
                                R"({"jsonrpc":"2.0","id":%u,"method":"eth_getBlockByNumber","params":["0x%llx",true]})",
                                id, static_cast<unsigned long long>(r.block_number));
    if (n > 0) {
      sent_ms_[id % sent_ms_.size()] = utc_now_ms();
      (void)ws_.send_text(std::string_view(req, static_cast<std::size_t>(n)), 5000);
    }
  }

  // SOL: fetch the block `slot_lag` slots behind the announced slot (DELAYED).
  bool sol_follow_up(const ParseResult& r) {
    if (!r.block_announced || r.block_number <= c_.cfg->slot_lag || rpc_url_.host.empty()) return true;
    if (!take_token()) return true;
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
    const std::int64_t t0 = utc_now_ms();
    const net::NetError e = http_.post_json(rpc_url_, std::string_view(req, static_cast<std::size_t>(n)),
                                            http_buf_.data(), http_buf_.capacity(), len, status, 15000, detail);
    if (e == net::NetError::RateLimited) {
      note(sm_.on_rate_limited(utc_now_ms()));
      return false;
    }
    if (e == net::NetError::None && pool_.size() > 0) {
      pool_.on_success(utc_now_ms(), static_cast<double>(utc_now_ms() - t0));
      c_.status->rpc_latency_ms.store(
          static_cast<std::int32_t>(pool_.health(pool_.active()).latency_ewma_ms + 0.5), std::memory_order_relaxed);
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
        if (c_.cfg->ping_s > 0.0 && !c_.cfg->ping_text.empty() &&
            now - std::max(ref, last_ping_ms_) > static_cast<std::int64_t>(c_.cfg->ping_s * 1000.0)) {
          last_ping_ms_ = now;
          (void)ws_.send_text(c_.cfg->ping_text, 5000);
        }
        if (now - ref > static_cast<std::int64_t>(c_.all->stale_after_s * 1000.0)) {
          if (pool_.size() > 0) pool_failure(false);
          fail(net::NetError::Timeout, "no data within stale_after_s");
          return;
        }
        continue;
      }
      if (err != net::NetError::None) {
        if (pool_.size() > 0) pool_failure(err == net::NetError::RateLimited);
        fail(err, std::string());
        return;
      }
      const ParseResult r = parse(buf_.data(), len);
      note_response(r);
      if (!handle(r)) {
        if (pool_.size() > 0) pool_failure(true);
        fail(net::NetError::RateLimited, "provider rate limit");
        return;
      }
      if (id_ == FeedId::Eth) {
        eth_follow_up(r);
      } else if (id_ == FeedId::Sol && !sol_follow_up(r)) {
        if (pool_.size() > 0) pool_failure(true);
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
  EndpointPool pool_;
  OkxContracts okx_contracts_;
  std::array<std::int64_t, 32> sent_ms_{};
  std::int64_t last_ping_ms_ = 0;
  std::size_t geo_refusals_ = 0;
  unsigned next_id_ = 0;
  bool pending_ok_ = true;
  bool logged_once_ = false;
  bool replay_published_ = false;
  double latency_ema_ = -1.0;
  std::vector<std::string> urls_;
  std::vector<net::Url> parsed_;
  std::size_t url_index_ = 0;
};

}  // namespace

void run_feed_adapter(LiveFeeds::Context& c, std::stop_token stop) {
  Adapter a(c, std::move(stop));
  a.run();
}

}  // namespace ftv
