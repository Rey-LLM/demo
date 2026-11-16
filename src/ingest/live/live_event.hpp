#pragma once
// The 64-byte record every live, replayed or modeled observation becomes on
// its way from a feed thread to the model thread (MpscRing<LiveEvent>), and
// the record format of .ftxcap replay captures.
//
// Every event carries a SourceLabel. The label travels with the event into
// the model's aggregates, the HUD, every export and the README table, so
// modeled or replayed data can never be presented as live.

#include <array>
#include <cstdint>
#include <string_view>

namespace ftv {

// Data provenance. LIVE: received from a public endpoint moments ago.
// DELAYED: real data fetched behind the tip on purpose (Solana blocks are
// requested a few slots back). MODELED: derived from published aggregates,
// not observed (fiat corridors). REPLAY: played back from a capture file.
enum class SourceLabel : std::uint8_t { Live = 0, Delayed = 1, Modeled = 2, Replay = 3, Count };
inline constexpr std::array<std::string_view, 4> kSourceLabelNames = {"LIVE", "DELAYED", "MODELED", "REPLAY"};
inline std::string_view label_name(SourceLabel l) noexcept {
  return kSourceLabelNames[static_cast<std::size_t>(l) & 3u];
}

// Feeds, in HUD order. Each has its own state machine and replay fallback.
// FX is the ECB reference-rate poller (prices only, no flow events); FIAT is
// the MODELED corridor generator and has no network connection.
// BINANCE_LIQ (USD-M futures forceOrder) and OKX (trades + liquidation-orders)
// were added in v4 and are appended so the ids stored in captures keep their
// meaning.
enum class FeedId : std::uint8_t {
  BtcMempool = 0, BtcBlocks, Eth, Sol, Coinbase, Kraken, Binance, Fx, Fiat, BinanceLiq, Okx, Count
};
inline constexpr std::size_t kFeedCount = static_cast<std::size_t>(FeedId::Count);
// Short HUD/export names.
inline constexpr std::array<std::string_view, kFeedCount> kFeedNames = {
    "BTC", "BTC_BLOCKS", "ETH", "SOL", "COINBASE", "KRAKEN", "BINANCE", "FX", "FIAT", "BINANCE_LIQ", "OKX"};
// Venue names for the tape (exchanges) or chain names.
inline constexpr std::array<std::string_view, kFeedCount> kFeedVenues = {
    "BTC", "BTC", "ETH", "SOL", "COINBASE", "KRAKEN", "BINANCE", "ECB", "FIAT", "BINANCE", "OKX"};
inline std::string_view feed_name(FeedId f) noexcept { return kFeedNames[static_cast<std::size_t>(f) % kFeedCount]; }

enum class EventKind : std::uint8_t {
  Transaction = 0,  // one on-chain transaction (or a sampled one)
  Block = 1,        // a block header (count = transactions in the block)
  Trade = 2,        // one exchange trade
  Ticker = 3,       // a price update (no volume)
  Corridor = 4,     // one second of modeled fiat corridor volume
  Liquidation = 5,  // a forced close of a derivatives position (kEvBuy: the liquidation order bought,
                    // i.e. a SHORT position was closed)
};

// Assets and currencies. Codes double as the export's currency column.
// USDT is only ever a quote currency (Binance pairs); it is valued with
// Kraken's USDT/USD ticker, or at par until the first tick arrives.
enum class Asset : std::uint8_t { BTC = 0, ETH, SOL, USD, EUR, GBP, JPY, CNY, HKD, USDT, Count };
inline constexpr std::size_t kAssetCount = static_cast<std::size_t>(Asset::Count);
inline constexpr std::array<std::string_view, kAssetCount> kAssetNames = {"BTC", "ETH", "SOL", "USD", "EUR",
                                                                          "GBP", "JPY", "CNY", "HKD", "USDT"};
inline std::string_view asset_name(Asset a) noexcept { return kAssetNames[static_cast<std::size_t>(a) % kAssetCount]; }

inline constexpr std::uint8_t kEvPriceUnknown = 1u << 0;  // no USD price yet: usd_value is 0
inline constexpr std::uint8_t kEvSampled = 1u << 1;       // one of a sampled subset (SOL getBlock cap)
inline constexpr std::uint8_t kEvBuy = 1u << 2;           // trade: taker bought the base asset

struct alignas(64) LiveEvent {
  std::int64_t ts_ms = 0;        // event time, UTC epoch milliseconds (source time when given, else receipt)
  double usd_value = 0.0;        // value in USD (0 when the price is unknown)
  double amount = 0.0;           // native amount (BTC, ETH, SOL, base asset of a trade, or USD for corridors)
  double price = 0.0;            // USD price carried by trades and tickers
  std::uint64_t id_hash = 0;     // hash of the source's id (tx hash, trade id), for diagnostics
  std::uint32_t count = 1;       // transactions represented (a block's tx count, a corridor's modeled tx count)
  std::uint16_t src_hub = 0;     // hub indices in config/hubs.json order
  std::uint16_t dst_hub = 0;
  FeedId feed = FeedId::BtcMempool;
  SourceLabel label = SourceLabel::Live;
  EventKind kind = EventKind::Transaction;
  Asset asset = Asset::BTC;
  std::uint8_t flags = 0;
  // Trades and liquidations: quote currency as Asset + 1 (0: unknown, which
  // is also what v3 captures hold in this byte). See quote_of() / set_quote().
  std::uint8_t quote_code = 0;
  // Address fragments for the tape ("0x3f..a1 -> 0x9c..07"): the first two
  // and last two characters of the sender and recipient (after any "0x"),
  // ASCII, zero when unknown. Enough to tell rows apart; never a full id.
  char from_tag[4] = {};
  char to_tag[4] = {};
  std::uint8_t pad[2] = {};
};
static_assert(sizeof(LiveEvent) == 64, "LiveEvent must stay one cache line");

inline void set_quote(LiveEvent& e, Asset q) noexcept { e.quote_code = static_cast<std::uint8_t>(static_cast<unsigned>(q) + 1u); }
// Asset::Count when unknown.
inline Asset quote_of(const LiveEvent& e) noexcept {
  return e.quote_code == 0 || e.quote_code > kAssetCount ? Asset::Count : static_cast<Asset>(e.quote_code - 1u);
}

// Effects that exist only as events: the model thread pushes these to the
// render thread (SpscRing), which turns them into live whale arcs.
struct VisualEvent {
  float start_s = 0.0f;  // live clock (s since session start) at which the arc leaves
  std::uint16_t src_hub = 0;
  std::uint16_t dst_hub = 0;
  std::uint8_t tier = 1;  // 1..3 whales; 0 a flow trail (below the whale threshold)
  FeedId feed = FeedId::BtcMempool;  // for the tape / arc filter
  std::uint8_t liquidation = 0;
  std::uint8_t pad = 0;
  float usd = 0.0f;
};

}  // namespace ftv
