#include "ingest/live/parsers.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <vector>

#include <simdjson.h>

#include "ingest/field_parsers.hpp"

namespace ftv {

namespace od = simdjson::ondemand;

namespace {
// For optional fields: a failed read leaves the default in place.
inline void ignore(simdjson::error_code) noexcept {}
}  // namespace

struct JsonParser::Impl {
  od::parser parser;
  // Scratch for parsers that must see a whole message before emitting
  // (sampling, field order independence). Reserved once.
  std::vector<double> values;
  std::vector<std::uint64_t> ids;
  struct Item {
    Asset base = Asset::Count, quote = Asset::Count;
    double price = 0.0, qty = 0.0, last = 0.0;
    std::int64_t ts = 0;
    std::uint64_t id = 0;
    bool buy = false, has_price = false, has_last = false;
  };
  std::vector<Item> items;
};

JsonParser::JsonParser() : impl_(std::make_unique<Impl>()) {
  // Preallocate for the largest frame we accept (8 MB), so parsing never
  // allocates once running.
  ignore(impl_->parser.allocate(8u << 20));
  impl_->values.reserve(EventBatch::kCapacity);
  impl_->ids.reserve(EventBatch::kCapacity);
  impl_->items.reserve(1024);
}
JsonParser::~JsonParser() = default;

FeedBuffer::FeedBuffer(std::size_t capacity)
    : data_(std::make_unique<char[]>(capacity + simdjson::SIMDJSON_PADDING)), capacity_(capacity) {
  std::memset(data_.get(), 0, capacity + simdjson::SIMDJSON_PADDING);
}

std::size_t FeedBuffer::assign(std::string_view s) noexcept {
  const std::size_t n = std::min(s.size(), capacity_);
  std::memcpy(data_.get(), s.data(), n);
  std::memset(data_.get() + n, 0, simdjson::SIMDJSON_PADDING);
  return n;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

bool parse_hex_u64(std::string_view s, std::uint64_t& out) noexcept {
  if (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
  s.remove_prefix(2);
  if (s.size() > 16) return false;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out, 16);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

double hex_to_double(std::string_view s) noexcept {
  if (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return 0.0;
  double v = 0.0;
  for (std::size_t i = 2; i < s.size(); ++i) {
    const char c = s[i];
    int d;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    } else {
      return 0.0;
    }
    v = v * 16.0 + d;
  }
  return v;
}

bool asset_from_code(std::string_view code, Asset& out) noexcept {
  for (std::size_t i = 0; i < kAssetCount; ++i) {
    if (code == kAssetNames[i]) {
      out = static_cast<Asset>(i);
      return true;
    }
  }
  if (code == "XBT") {  // Kraken's legacy code
    out = Asset::BTC;
    return true;
  }
  return false;
}

bool split_pair(std::string_view symbol, Asset& base, Asset& quote) noexcept {
  const std::size_t sep = symbol.find_first_of("-/");
  if (sep == std::string_view::npos) return false;
  return asset_from_code(symbol.substr(0, sep), base) && asset_from_code(symbol.substr(sep + 1), quote);
}

bool split_concat_pair(std::string_view symbol, Asset& base, Asset& quote) noexcept {
  char up[16];
  if (symbol.size() < 6 || symbol.size() > sizeof(up)) return false;
  for (std::size_t i = 0; i < symbol.size(); ++i) {
    char c = symbol[i];
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    up[i] = c;
  }
  const std::string_view s(up, symbol.size());
  // Longest quote first: "USDT" before "USD".
  static constexpr std::string_view kQuotes[] = {"USDT", "USD", "EUR", "GBP", "JPY"};
  for (std::string_view q : kQuotes) {
    if (s.size() > q.size() && s.substr(s.size() - q.size()) == q) {
      return asset_from_code(s.substr(0, s.size() - q.size()), base) && asset_from_code(q, quote);
    }
  }
  return false;
}

namespace {

bool to_double(std::string_view s, double& out) noexcept {
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

std::uint64_t fnv1a(std::string_view s) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ull;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ull;
  }
  return h;
}

// Number given either as a JSON number or as a decimal string.
using ValueResult = simdjson::simdjson_result<od::value>;

bool get_number(ValueResult vr, double& out) noexcept {
  od::value v;
  if (std::move(vr).get(v)) return false;
  od::json_type t;
  if (v.type().get(t)) return false;
  if (t == od::json_type::number) return !v.get_double().get(out);
  if (t == od::json_type::string) {
    std::string_view s;
    return !v.get_string().get(s) && to_double(s, out);
  }
  return false;
}

bool get_int(ValueResult v, std::int64_t& out) noexcept {
  double d = 0.0;
  if (!get_number(v, d)) return false;
  out = static_cast<std::int64_t>(d);
  return true;
}

bool open(JsonParser& p, const char* msg, std::size_t len, od::object& root, od::document& doc) noexcept {
  simdjson::padded_string_view psv(msg, len, len + simdjson::SIMDJSON_PADDING);
  if (p.impl().parser.iterate(psv).get(doc)) return false;
  return !doc.get_object().get(root);
}

bool contains_ci(std::string_view hay, std::string_view needle) noexcept {
  if (needle.size() > hay.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    bool eq = true;
    for (std::size_t k = 0; k < needle.size() && eq; ++k) {
      char a = hay[i + k];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      eq = a == needle[k];
    }
    if (eq) return true;
  }
  return false;
}

// JSON-RPC error object -> status.
ParseStatus rpc_error_status(ValueResult v) noexcept {
  od::object e;
  if (v.get_object().get(e)) return ParseStatus::Rejected;
  std::int64_t code = 0;
  std::string_view message;
  for (auto f : e) {
    std::string_view k;
    if (f.unescaped_key().get(k)) continue;
    if (k == "code") {
      get_int(f.value(), code);
    } else if (k == "message") {
      ignore(f.value().get_string().get(message));
    }
  }
  if (code == 429 || code == -32005 || contains_ci(message, "rate limit") || contains_ci(message, "too many")) {
    return ParseStatus::RateLimited;
  }
  return ParseStatus::Rejected;
}

LiveEvent make_event(FeedId feed, const ParseContext& ctx, EventKind kind, Asset asset) noexcept {
  LiveEvent e;
  e.feed = feed;
  e.label = ctx.label;
  e.kind = kind;
  e.asset = asset;
  e.ts_ms = ctx.recv_ms;
  return e;
}

void value_in_usd(LiveEvent& e, const ParseContext& ctx) noexcept {
  const double px = ctx.prices ? ctx.prices->usd(e.asset) : 0.0;
  if (px > 0.0) {
    e.usd_value = e.amount * px;
    e.price = px;
  } else {
    e.usd_value = 0.0;
    e.flags |= kEvPriceUnknown;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// blockchain.com: {"op":"utx","x":{... "time":..., "hash":"...", "out":[{"value":sat}, ...]}}
// ---------------------------------------------------------------------------

ParseResult parse_blockchain_com(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx,
                                 EventBatch& out) {
  ParseResult r;
  od::document doc;
  od::object root;
  if (!open(p, msg, len, root, doc)) return {ParseStatus::Malformed};
  std::string_view op;
  bool haveTx = false;
  LiveEvent e = make_event(FeedId::BtcMempool, ctx, EventKind::Transaction, Asset::BTC);
  e.src_hub = e.dst_hub = ctx.hubs.chain_hub[0];
  std::uint64_t sat = 0;
  for (auto f : root) {
    std::string_view k;
    if (f.unescaped_key().get(k)) return {ParseStatus::Malformed};
    if (k == "op") {
      if (f.value().get_string().get(op)) return {ParseStatus::Malformed};
    } else if (k == "x") {
      od::object x;
      if (f.value().get_object().get(x)) continue;  // "x" of other ops may be scalar
      for (auto g : x) {
        std::string_view gk;
        if (g.unescaped_key().get(gk)) return {ParseStatus::Malformed};
        if (gk == "time") {
          std::int64_t t = 0;
          if (get_int(g.value(), t) && t > 0) e.ts_ms = t * 1000;
        } else if (gk == "hash") {
          std::string_view h;
          if (!g.value().get_string().get(h)) e.id_hash = fnv1a(h);
        } else if (gk == "out") {
          od::array outs;
          if (g.value().get_array().get(outs)) return {ParseStatus::Malformed};
          for (auto o : outs) {
            od::object oo;
            if (o.get_object().get(oo)) return {ParseStatus::Malformed};
            for (auto of : oo) {
              std::string_view ok;
              if (of.unescaped_key().get(ok)) return {ParseStatus::Malformed};
              if (ok == "value") {
                std::uint64_t v = 0;
                if (!of.value().get_uint64().get(v)) sat += v;
              }
            }
          }
          haveTx = true;
        }
      }
    }
  }
  if (op.empty()) return {ParseStatus::Malformed};
  if (op != "utx") return {ParseStatus::Ignored};
  if (!haveTx) return {ParseStatus::Malformed};
  e.amount = static_cast<double>(sat) * 1e-8;
  value_in_usd(e, ctx);
  out.push(e);
  return r;
}

// ---------------------------------------------------------------------------
// mempool.space: {"block":{"id":..,"height":..,"timestamp":..,"tx_count":..}}
// The initial {"blocks":[...]} history is acknowledged without events.
// ---------------------------------------------------------------------------

ParseResult parse_mempool_space(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx,
                                EventBatch& out) {
  ParseResult r;
  od::document doc;
  od::object root;
  if (!open(p, msg, len, root, doc)) return {ParseStatus::Malformed};
  bool known = false;
  for (auto f : root) {
    std::string_view k;
    if (f.unescaped_key().get(k)) return {ParseStatus::Malformed};
    if (k == "block") {
      od::object b;
      if (f.value().get_object().get(b)) return {ParseStatus::Malformed};
      LiveEvent e = make_event(FeedId::BtcBlocks, ctx, EventKind::Block, Asset::BTC);
      e.src_hub = e.dst_hub = ctx.hubs.chain_hub[0];
      e.count = 0;
      bool haveHeight = false;
      for (auto g : b) {
        std::string_view gk;
        if (g.unescaped_key().get(gk)) return {ParseStatus::Malformed};
        std::int64_t v = 0;
        if (gk == "height" && get_int(g.value(), v)) {
          r.block_number = static_cast<std::uint64_t>(v);
          haveHeight = true;
        } else if (gk == "timestamp" && get_int(g.value(), v)) {
          e.ts_ms = v * 1000;
        } else if (gk == "tx_count" && get_int(g.value(), v)) {
          e.count = static_cast<std::uint32_t>(std::max<std::int64_t>(v, 0));
        } else if (gk == "id") {
          std::string_view h;
          if (!g.value().get_string().get(h)) e.id_hash = fnv1a(h);
        }
      }
      if (!haveHeight) return {ParseStatus::Malformed};
      r.block_announced = true;
      r.transactions_seen = e.count;
      out.push(e);
      known = true;
    } else if (k == "blocks" || k == "mempoolInfo" || k == "conversions" || k == "fees" || k == "da" ||
               k == "vBytesPerSecond" || k == "mempool-blocks" || k == "transactions" || k == "loadingIndicators" ||
               k == "backendInfo" || k == "pong") {
      known = true;
    }
  }
  r.status = known ? ParseStatus::Ok : ParseStatus::Ignored;
  return r;
}

// ---------------------------------------------------------------------------
// Ethereum JSON-RPC
// ---------------------------------------------------------------------------

ParseResult parse_eth_rpc(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out) {
  ParseResult r;
  od::document doc;
  od::object root;
  if (!open(p, msg, len, root, doc)) return {ParseStatus::Malformed};
  std::string_view method;
  bool any = false;
  auto parse_block = [&](od::object blk) -> bool {
    std::int64_t tsMs = ctx.recv_ms;
    const std::size_t first = out.size();
    std::uint32_t seen = 0;
    for (auto g : blk) {
      std::string_view gk;
      if (g.unescaped_key().get(gk)) return false;
      if (gk == "number") {
        std::string_view h;
        if (!g.value().get_string().get(h)) parse_hex_u64(h, r.block_number);
      } else if (gk == "timestamp") {
        std::string_view h;
        std::uint64_t t = 0;
        if (!g.value().get_string().get(h) && parse_hex_u64(h, t)) tsMs = static_cast<std::int64_t>(t) * 1000;
      } else if (gk == "transactions") {
        od::array txs;
        if (g.value().get_array().get(txs)) return false;
        for (auto t : txs) {
          ++seen;
          od::object tx;
          if (t.get_object().get(tx)) continue;  // hashes only (full=false): counted, no value
          LiveEvent e = make_event(FeedId::Eth, ctx, EventKind::Transaction, Asset::ETH);
          e.src_hub = e.dst_hub = ctx.hubs.chain_hub[1];
          for (auto tf : tx) {
            std::string_view tk;
            if (tf.unescaped_key().get(tk)) return false;
            std::string_view v;
            if (tk == "value") {
              if (!tf.value().get_string().get(v)) e.amount = hex_to_double(v) * 1e-18;
            } else if (tk == "hash") {
              if (!tf.value().get_string().get(v)) e.id_hash = fnv1a(v);
            }
          }
          value_in_usd(e, ctx);
          out.push(e);
        }
      }
    }
    for (std::size_t i = first; i < out.size(); ++i) out.at(i).ts_ms = tsMs;
    r.transactions_seen = seen;
    return true;
  };
  for (auto f : root) {
    std::string_view k;
    if (f.unescaped_key().get(k)) return {ParseStatus::Malformed};
    if (k == "id") {
      std::int64_t id = -1;
      if (get_int(f.value(), id)) r.rpc_id = id;
    } else if (k == "method") {
      ignore(f.value().get_string().get(method));
    } else if (k == "error") {
      r.status = rpc_error_status(f.value());
      any = true;
    } else if (k == "result") {
      od::json_type t;
      if (f.value().type().get(t)) return {ParseStatus::Malformed};
      if (t == od::json_type::object) {
        od::object blk;
        if (f.value().get_object().get(blk) || !parse_block(blk)) return {ParseStatus::Malformed};
      }
      any = true;  // string (subscription id), null (unknown block), bool
    } else if (k == "params") {
      od::object prm;
      if (f.value().get_object().get(prm)) return {ParseStatus::Malformed};
      for (auto g : prm) {
        std::string_view gk;
        if (g.unescaped_key().get(gk)) return {ParseStatus::Malformed};
        if (gk != "result") continue;
        od::json_type t;
        if (g.value().type().get(t)) return {ParseStatus::Malformed};
        if (t == od::json_type::object) {
          od::object head;
          if (g.value().get_object().get(head)) return {ParseStatus::Malformed};
          for (auto h : head) {
            std::string_view hk;
            if (h.unescaped_key().get(hk)) return {ParseStatus::Malformed};
            if (hk == "number") {
              std::string_view hs;
              if (!h.value().get_string().get(hs) && parse_hex_u64(hs, r.block_number)) r.block_announced = true;
            }
          }
        } else if (t == od::json_type::string) {
          // newPendingTransactions notification: a transaction hash only.
          std::string_view hs;
          if (!g.value().get_string().get(hs)) {
            LiveEvent e = make_event(FeedId::Eth, ctx, EventKind::Transaction, Asset::ETH);
            e.src_hub = e.dst_hub = ctx.hubs.chain_hub[1];
            e.id_hash = fnv1a(hs);
            e.flags |= kEvPriceUnknown;
            out.push(e);
          }
        }
      }
      any = true;
    } else if (k == "jsonrpc") {
      // nothing
    }
  }
  if (!any) return {ParseStatus::Ignored};
  if (!method.empty() && method != "eth_subscription") r.status = ParseStatus::Ignored;
  return r;
}

// ---------------------------------------------------------------------------
// Solana JSON-RPC: slotNotification and getBlock (transactionDetails=full,
// encoding=json). Vote transactions are filtered; the rest is sampled down to
// ctx.sample_cap evenly spaced transactions per block.
// ---------------------------------------------------------------------------

ParseResult parse_sol_rpc(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out) {
  static constexpr std::string_view kVoteProgram = "Vote111111111111111111111111111111111111111";
  ParseResult r;
  od::document doc;
  od::object root;
  if (!open(p, msg, len, root, doc)) return {ParseStatus::Malformed};
  auto& vals = p.impl().values;
  auto& ids = p.impl().ids;
  vals.clear();
  ids.clear();
  std::string_view method;
  std::int64_t blockTime = 0;
  bool any = false;
  for (auto f : root) {
    std::string_view k;
    if (f.unescaped_key().get(k)) return {ParseStatus::Malformed};
    if (k == "id") {
      std::int64_t id = -1;
      if (get_int(f.value(), id)) r.rpc_id = id;
    } else if (k == "method") {
      ignore(f.value().get_string().get(method));
    } else if (k == "error") {
      const ParseStatus s = rpc_error_status(f.value());
      // Skipped slots (-32007) and blocks not yet available (-32004) are
      // normal: no events, not a failure.
      r.status = s == ParseStatus::RateLimited ? s : ParseStatus::Ignored;
      any = true;
    } else if (k == "params") {
      od::object prm;
      if (f.value().get_object().get(prm)) return {ParseStatus::Malformed};
      for (auto g : prm) {
        std::string_view gk;
        if (g.unescaped_key().get(gk)) return {ParseStatus::Malformed};
        if (gk != "result") continue;
        od::object res;
        if (g.value().get_object().get(res)) return {ParseStatus::Malformed};
        for (auto h : res) {
          std::string_view hk;
          if (h.unescaped_key().get(hk)) return {ParseStatus::Malformed};
          std::int64_t v = 0;
          if (hk == "slot" && get_int(h.value(), v)) {
            r.block_number = static_cast<std::uint64_t>(v);
            r.block_announced = true;
          }
        }
      }
      any = true;
    } else if (k == "result") {
      od::json_type t;
      if (f.value().type().get(t)) return {ParseStatus::Malformed};
      any = true;
      if (t != od::json_type::object) continue;  // subscription id or null
      od::object blk;
      if (f.value().get_object().get(blk)) return {ParseStatus::Malformed};
      for (auto g : blk) {
        std::string_view gk;
        if (g.unescaped_key().get(gk)) return {ParseStatus::Malformed};
        if (gk == "blockTime") {
          get_int(g.value(), blockTime);
        } else if (gk == "parentSlot") {
          std::int64_t v = 0;
          if (get_int(g.value(), v)) r.block_number = static_cast<std::uint64_t>(v + 1);
        } else if (gk == "transactions") {
          od::array txs;
          if (g.value().get_array().get(txs)) return {ParseStatus::Malformed};
          for (auto t2 : txs) {
            ++r.transactions_seen;
            od::object tx;
            if (t2.get_object().get(tx)) return {ParseStatus::Malformed};
            bool vote = false, failed = false;
            double moved = 0.0;
            std::uint64_t sig = 0;
            // Balances are collected per account index; the delta is summed
            // over accounts that gained (lamports moved to recipients).
            std::int64_t pre[64], post[64];
            std::size_t npre = 0, npost = 0;
            for (auto tf : tx) {
              std::string_view tk;
              if (tf.unescaped_key().get(tk)) return {ParseStatus::Malformed};
              if (tk == "meta") {
                od::object meta;
                if (tf.value().get_object().get(meta)) continue;  // null meta
                for (auto mf : meta) {
                  std::string_view mk;
                  if (mf.unescaped_key().get(mk)) return {ParseStatus::Malformed};
                  if (mk == "err") {
                    bool isNull = false;
                    if (!mf.value().is_null().get(isNull)) failed = !isNull;
                  } else if (mk == "preBalances" || mk == "postBalances") {
                    od::array bal;
                    if (mf.value().get_array().get(bal)) return {ParseStatus::Malformed};
                    std::int64_t* dst = mk == "preBalances" ? pre : post;
                    std::size_t& n = mk == "preBalances" ? npre : npost;
                    for (auto b : bal) {
                      std::int64_t v = 0;
                      if (b.get_int64().get(v)) return {ParseStatus::Malformed};
                      if (n < 64) dst[n++] = v;
                    }
                  }
                }
              } else if (tk == "transaction") {
                od::object txo;
                if (tf.value().get_object().get(txo)) return {ParseStatus::Malformed};
                for (auto xf : txo) {
                  std::string_view xk;
                  if (xf.unescaped_key().get(xk)) return {ParseStatus::Malformed};
                  if (xk == "signatures") {
                    od::array sigs;
                    if (xf.value().get_array().get(sigs)) return {ParseStatus::Malformed};
                    for (auto s : sigs) {
                      std::string_view sv;
                      if (sig == 0 && !s.get_string().get(sv)) sig = fnv1a(sv);
                    }
                  } else if (xk == "message") {
                    od::object m;
                    if (xf.value().get_object().get(m)) return {ParseStatus::Malformed};
                    for (auto mf : m) {
                      std::string_view mk;
                      if (mf.unescaped_key().get(mk)) return {ParseStatus::Malformed};
                      if (mk != "accountKeys") continue;
                      od::array keys;
                      if (mf.value().get_array().get(keys)) return {ParseStatus::Malformed};
                      for (auto key : keys) {
                        std::string_view ks;
                        if (!key.get_string().get(ks) && ks == kVoteProgram) vote = true;
                      }
                    }
                  }
                }
              }
            }
            if (vote || failed) continue;
            const std::size_t n = std::min(npre, npost);
            for (std::size_t i = 0; i < n; ++i) {
              if (post[i] > pre[i]) moved += static_cast<double>(post[i] - pre[i]);
            }
            if (vals.size() < EventBatch::kCapacity) {
              vals.push_back(moved * 1e-9);
              ids.push_back(sig);
            }
          }
        }
      }
    }
  }
  if (!any) return {ParseStatus::Ignored};
  if (!method.empty() && method != "slotNotification") r.status = ParseStatus::Ignored;
  if (!vals.empty()) {
    // Evenly spaced deterministic sample of at most sample_cap transactions.
    const std::size_t cap = std::max<std::uint32_t>(ctx.sample_cap, 1);
    const std::size_t n = vals.size();
    const std::size_t take = std::min(cap, n);
    const std::uint32_t represents = static_cast<std::uint32_t>((n + take - 1) / take);
    for (std::size_t j = 0; j < take; ++j) {
      const std::size_t i = j * n / take;
      LiveEvent e = make_event(FeedId::Sol, ctx, EventKind::Transaction, Asset::SOL);
      e.src_hub = e.dst_hub = ctx.hubs.chain_hub[2];
      e.amount = vals[i];
      e.id_hash = ids[i];
      e.count = take < n ? represents : 1u;
      if (take < n) e.flags |= kEvSampled;
      if (blockTime > 0) e.ts_ms = blockTime * 1000;
      value_in_usd(e, ctx);
      out.push(e);
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// Exchanges
// ---------------------------------------------------------------------------

namespace {

// A trade moves value from the quote currency's financial centre to the base
// asset's chain cloud when the taker buys, and back when the taker sells.
void trade_hubs(LiveEvent& e, Asset base, Asset quote, bool buy, const ParseContext& ctx) noexcept {
  const std::size_t bi = static_cast<std::size_t>(base);
  const std::uint16_t chain = bi < 3 ? ctx.hubs.chain_hub[bi] : ctx.hubs.chain_hub[0];
  const std::uint16_t fiat = ctx.hubs.quote_hub[static_cast<std::size_t>(quote) % kAssetCount];
  e.src_hub = buy ? fiat : chain;
  e.dst_hub = buy ? chain : fiat;
}

// Fiat quotes other than USD are valued through the implied cross rate of the
// same exchange's tickers (e.g. GBPUSD = BTC-USD last / BTC-GBP last), never
// through an outside FX source.
void set_price(PriceBook* prices, Asset base, Asset quote, double last, std::int64_t ts) noexcept {
  if (!prices || !(last > 0.0) || base == Asset::Count || quote == Asset::Count) return;
  if (quote == Asset::USD) {
    prices->set(base, last, ts);
  } else if (static_cast<std::size_t>(base) < 3) {
    const double baseUsd = prices->usd(base);
    if (baseUsd > 0.0) prices->set(quote, baseUsd / last, ts);
  }
}

void finish_trade(LiveEvent& e, Asset base, Asset quote, const ParseContext& ctx, PriceBook* prices) noexcept {
  e.asset = base;
  if (quote == Asset::USD) {
    e.usd_value = e.amount * e.price;
    if (prices) prices->set(base, e.price, e.ts_ms);
  } else {
    // Fiat quotes need FX (exchange cross rates, else the ECB reference
    // rate); USDT is valued with Kraken's USDT/USD ticker or at par.
    const double q = ctx.prices ? ctx.prices->usd(quote) : 0.0;
    if (q > 0.0) {
      e.usd_value = e.amount * e.price * q;
    } else {
      e.usd_value = 0.0;
      e.flags |= kEvPriceUnknown;
    }
  }
}

}  // namespace

ParseResult parse_coinbase(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out,
                           PriceBook* prices) {
  ParseResult r;
  od::document doc;
  od::object root;
  if (!open(p, msg, len, root, doc)) return {ParseStatus::Malformed};
  std::string_view type, product, side, time, message;
  double price = 0.0, size = 0.0;
  bool havePrice = false, haveSize = false;
  std::uint64_t tradeId = 0;
  for (auto f : root) {
    std::string_view k;
    if (f.unescaped_key().get(k)) return {ParseStatus::Malformed};
    if (k == "type") {
      ignore(f.value().get_string().get(type));
    } else if (k == "product_id") {
      ignore(f.value().get_string().get(product));
    } else if (k == "side") {
      ignore(f.value().get_string().get(side));
    } else if (k == "time") {
      ignore(f.value().get_string().get(time));
    } else if (k == "price") {
      havePrice = get_number(f.value(), price);
    } else if (k == "size") {
      haveSize = get_number(f.value(), size);
    } else if (k == "trade_id") {
      std::int64_t id = 0;
      if (get_int(f.value(), id)) tradeId = static_cast<std::uint64_t>(id);
    } else if (k == "message" || k == "reason") {
      ignore(f.value().get_string().get(message));
    }
  }
  if (type.empty()) return {ParseStatus::Malformed};
  if (type == "error") {
    r.status = contains_ci(message, "rate") ? ParseStatus::RateLimited : ParseStatus::Rejected;
    return r;
  }
  if (type == "subscriptions" || type == "heartbeat") return r;
  Asset base, quote;
  if (type == "ticker") {
    if (!havePrice || !split_pair(product, base, quote)) return {ParseStatus::Malformed};
    std::int64_t ts = ctx.recv_ms;
    if (!time.empty()) parse_timestamp_ms(time, ts);
    set_price(prices, base, quote, price, ts);
    return r;
  }
  if (type == "match" || type == "last_match") {
    if (!havePrice || !haveSize || !split_pair(product, base, quote)) return {ParseStatus::Malformed};
    LiveEvent e = make_event(FeedId::Coinbase, ctx, EventKind::Trade, base);
    if (!time.empty()) parse_timestamp_ms(time, e.ts_ms);
    e.amount = size;
    e.price = price;
    e.id_hash = tradeId;
    // Coinbase reports the maker's side: a "sell" maker means the taker bought.
    const bool buy = side == "sell";
    if (buy) e.flags |= kEvBuy;
    trade_hubs(e, base, quote, buy, ctx);
    finish_trade(e, base, quote, ctx, prices);
    // last_match is the most recent trade at subscription time: history, so
    // it seeds the price book but is not counted as live volume.
    if (type == "match") out.push(e);
    return r;
  }
  return {ParseStatus::Ignored};
}

ParseResult parse_kraken(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out,
                         PriceBook* prices) {
  ParseResult r;
  od::document doc;
  od::object root;
  if (!open(p, msg, len, root, doc)) return {ParseStatus::Malformed};
  auto& items = p.impl().items;
  items.clear();
  std::string_view channel, type, method, error;
  bool haveSuccess = false, success = false;
  for (auto f : root) {
    std::string_view k;
    if (f.unescaped_key().get(k)) return {ParseStatus::Malformed};
    if (k == "channel") {
      ignore(f.value().get_string().get(channel));
    } else if (k == "type") {
      ignore(f.value().get_string().get(type));
    } else if (k == "method") {
      ignore(f.value().get_string().get(method));
    } else if (k == "success") {
      haveSuccess = !f.value().get_bool().get(success);
    } else if (k == "error") {
      ignore(f.value().get_string().get(error));
    } else if (k == "data") {
      od::array data;
      if (f.value().get_array().get(data)) continue;
      for (auto d : data) {
        od::object o;
        if (d.get_object().get(o)) continue;
        JsonParser::Impl::Item it;
        for (auto g : o) {
          std::string_view gk;
          if (g.unescaped_key().get(gk)) return {ParseStatus::Malformed};
          std::string_view sv;
          if (gk == "symbol") {
            if (!g.value().get_string().get(sv)) split_pair(sv, it.base, it.quote);
          } else if (gk == "side") {
            if (!g.value().get_string().get(sv)) it.buy = sv == "buy";
          } else if (gk == "price") {
            it.has_price = get_number(g.value(), it.price);
          } else if (gk == "qty") {
            get_number(g.value(), it.qty);
          } else if (gk == "last") {
            it.has_last = get_number(g.value(), it.last);
          } else if (gk == "timestamp") {
            if (!g.value().get_string().get(sv)) parse_timestamp_ms(sv, it.ts);
          } else if (gk == "trade_id") {
            std::int64_t id = 0;
            if (get_int(g.value(), id)) it.id = static_cast<std::uint64_t>(id);
          }
        }
        if (items.size() < items.capacity()) items.push_back(it);
      }
    }
  }
  if (!method.empty()) {
    if (haveSuccess && !success) {
      r.status = (contains_ci(error, "rate") || contains_ci(error, "exceeded")) ? ParseStatus::RateLimited
                                                                                : ParseStatus::Rejected;
    }
    return r;
  }
  if (channel.empty()) return {ParseStatus::Malformed};
  if (channel == "heartbeat" || channel == "status") return r;
  if (channel == "ticker") {
    for (const auto& it : items) {
      if (it.has_last) set_price(prices, it.base, it.quote, it.last, ctx.recv_ms);
    }
    return r;
  }
  if (channel == "trade") {
    for (const auto& it : items) {
      if (it.base == Asset::Count || it.quote == Asset::Count || !it.has_price) return {ParseStatus::Malformed};
      LiveEvent e = make_event(FeedId::Kraken, ctx, EventKind::Trade, it.base);
      if (it.ts > 0) e.ts_ms = it.ts;
      e.amount = it.qty;
      e.price = it.price;
      e.id_hash = it.id;
      if (it.buy) e.flags |= kEvBuy;
      trade_hubs(e, it.base, it.quote, it.buy, ctx);
      finish_trade(e, it.base, it.quote, ctx, prices);
      // The snapshot replays the last trades before the subscription:
      // history, used for prices only.
      if (type != "snapshot") out.push(e);
    }
    return r;
  }
  return {ParseStatus::Ignored};
}

// ---------------------------------------------------------------------------
// Binance: combined stream envelope around aggTrade / trade payloads.

ParseResult parse_binance(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx, EventBatch& out,
                          PriceBook* prices) {
  ParseResult r;
  od::document doc;
  od::object root;
  if (!open(p, msg, len, root, doc)) return {ParseStatus::Malformed};
  std::string_view ev, symbol, message;
  double price = 0.0, qty = 0.0;
  bool havePrice = false, haveQty = false, buyerMaker = false, haveId = false;
  std::int64_t tradeMs = 0, code = 0, id = 0;
  bool haveCode = false;
  // Fields of the payload object, which is either the root or root["data"].
  auto read_fields = [&](od::object& o) -> bool {
    for (auto f : o) {
      std::string_view k;
      if (f.unescaped_key().get(k)) return false;
      if (k == "e") {
        ignore(f.value().get_string().get(ev));
      } else if (k == "s") {
        ignore(f.value().get_string().get(symbol));
      } else if (k == "p") {
        havePrice = get_number(f.value(), price);
      } else if (k == "q") {
        haveQty = get_number(f.value(), qty);
      } else if (k == "T") {
        get_int(f.value(), tradeMs);
      } else if (k == "m") {
        ignore(f.value().get_bool().get(buyerMaker));
      } else if (k == "a" || k == "t") {
        haveId = get_int(f.value(), id);
      } else if (k == "data") {
        od::object d;
        if (f.value().get_object().get(d)) return false;
        for (auto g : d) {
          std::string_view gk;
          if (g.unescaped_key().get(gk)) return false;
          if (gk == "e") {
            ignore(g.value().get_string().get(ev));
          } else if (gk == "s") {
            ignore(g.value().get_string().get(symbol));
          } else if (gk == "p") {
            havePrice = get_number(g.value(), price);
          } else if (gk == "q") {
            haveQty = get_number(g.value(), qty);
          } else if (gk == "T") {
            get_int(g.value(), tradeMs);
          } else if (gk == "m") {
            ignore(g.value().get_bool().get(buyerMaker));
          } else if (gk == "a" || gk == "t") {
            haveId = get_int(g.value(), id);
          }
        }
      } else if (k == "code") {
        haveCode = get_int(f.value(), code);
      } else if (k == "msg") {
        ignore(f.value().get_string().get(message));
      }
    }
    return true;
  };
  if (!read_fields(root)) return {ParseStatus::Malformed};
  if (haveCode) {
    // {"code": 429 / -1003 "Too many requests", "msg": ...}
    r.status = (code == 429 || code == -1003 || contains_ci(message, "too many")) ? ParseStatus::RateLimited
                                                                                    : ParseStatus::Rejected;
    return r;
  }
  if (ev.empty()) return r;  // acknowledgement or other control message
  if (ev != "aggTrade" && ev != "trade") return {ParseStatus::Ignored};
  Asset base, quote;
  if (!havePrice || !haveQty || !split_concat_pair(symbol, base, quote) || static_cast<std::size_t>(base) >= 3) {
    return {ParseStatus::Malformed};
  }
  LiveEvent e = make_event(FeedId::Binance, ctx, EventKind::Trade, base);
  if (tradeMs > 0) e.ts_ms = tradeMs;
  e.amount = qty;
  e.price = price;
  e.id_hash = haveId ? static_cast<std::uint64_t>(id) : 0;
  // "m": the buyer is the market maker, so the taker sold.
  const bool buy = !buyerMaker;
  if (buy) e.flags |= kEvBuy;
  trade_hubs(e, base, quote, buy, ctx);
  finish_trade(e, base, quote, ctx, quote == Asset::USD ? prices : nullptr);
  // A USDT price is a USD price at the current USDT/USD rate: seed the book
  // only when Coinbase or Kraken have not set it in the last two minutes.
  if (quote == Asset::USDT && prices && e.usd_value > 0.0 && e.amount > 0.0 &&
      ctx.recv_ms - prices->updated_ms(base) > 120000) {
    prices->set(base, e.usd_value / e.amount, e.ts_ms);
  }
  out.push(e);
  return r;
}

// ---------------------------------------------------------------------------
// ECB reference rates (Frankfurter).

ParseResult parse_ecb_rates(JsonParser& p, const char* msg, std::size_t len, const ParseContext& ctx,
                            PriceBook* prices, FxBoard* board) {
  ParseResult r;
  od::document doc;
  od::object root;
  if (!open(p, msg, len, root, doc)) return {ParseStatus::Malformed};
  std::string_view base, date, message;
  double amount = 1.0;
  struct Rate {
    Asset a;
    double per_usd;
  };
  Rate rates[kAssetCount];
  std::size_t n = 0;
  bool haveRates = false;
  for (auto f : root) {
    std::string_view k;
    if (f.unescaped_key().get(k)) return {ParseStatus::Malformed};
    if (k == "base") {
      ignore(f.value().get_string().get(base));
    } else if (k == "date") {
      ignore(f.value().get_string().get(date));
    } else if (k == "amount") {
      get_number(f.value(), amount);
    } else if (k == "message" || k == "error") {
      ignore(f.value().get_string().get(message));
    } else if (k == "rates") {
      od::object o;
      if (f.value().get_object().get(o)) return {ParseStatus::Malformed};
      haveRates = true;
      for (auto g : o) {
        std::string_view code;
        if (g.unescaped_key().get(code)) return {ParseStatus::Malformed};
        double v = 0.0;
        Asset a;
        if (!get_number(g.value(), v) || !asset_from_code(code, a)) continue;
        if (v > 0.0 && n < kAssetCount) rates[n++] = Rate{a, v};
      }
    }
  }
  if (!haveRates) {
    if (!message.empty()) {
      r.status = contains_ci(message, "rate limit") || contains_ci(message, "too many") ? ParseStatus::RateLimited
                                                                                       : ParseStatus::Rejected;
      return r;
    }
    return {ParseStatus::Malformed};
  }
  if (base != "USD" || !(amount > 0.0) || date.size() != 10) return {ParseStatus::Malformed};
  std::uint32_t ymd = 0;
  for (char c : date) {
    if (c == '-') continue;
    if (c < '0' || c > '9') return {ParseStatus::Malformed};
    ymd = ymd * 10u + static_cast<std::uint32_t>(c - '0');
  }
  for (std::size_t i = 0; i < n; ++i) {
    const double usdPerUnit = amount / rates[i].per_usd;
    if (board) board->set(rates[i].a, usdPerUnit);
    // Exchange-implied cross rates are fresher; the daily reference rate
    // only fills in currencies without one from the last two minutes.
    if (prices && ctx.recv_ms - prices->updated_ms(rates[i].a) > 120000) {
      prices->set(rates[i].a, usdPerUnit, ctx.recv_ms);
    }
  }
  if (board) board->set_date(ymd, ctx.recv_ms);
  r.block_number = ymd;
  return r;
}

}  // namespace ftv
