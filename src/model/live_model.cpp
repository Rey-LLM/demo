#include "model/live_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <xxhash.h>

#include "model/hub_taxonomy.hpp"

namespace ftv {

namespace {

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
  const std::int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

std::size_t ring_index(std::int64_t v, std::size_t n) noexcept {
  const std::int64_t m = v % static_cast<std::int64_t>(n);
  return static_cast<std::size_t>(m < 0 ? m + static_cast<std::int64_t>(n) : m);
}

template <class T>
void put(std::vector<std::uint8_t>& out, const T& v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}
template <class T>
void put_array(std::vector<std::uint8_t>& out, const T* v, std::size_t n) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(v);
  out.insert(out.end(), p, p + sizeof(T) * n);
}

struct Reader {
  const std::uint8_t* p;
  std::size_t left;
  template <class T>
  bool get(T& v) noexcept {
    if (left < sizeof(T)) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    left -= sizeof(T);
    return true;
  }
  template <class T>
  bool get_array(T* v, std::size_t n) noexcept {
    if (left < sizeof(T) * n) return false;
    std::memcpy(v, p, sizeof(T) * n);
    p += sizeof(T) * n;
    left -= sizeof(T) * n;
    return true;
  }
};

constexpr std::uint32_t kStateMagic = 0x4D4C5446u;  // "FTLM"
constexpr std::uint32_t kStateVersion = 1;

}  // namespace

LiveModel::LiveModel(LiveModelConfig cfg) : cfg_(std::move(cfg)) {
  cfg_.hub_count = std::clamp<std::size_t>(cfg_.hub_count, 1, kMaxModelHubs);
  flows_.assign(cfg_.hub_count * cfg_.hub_count * kAssetCount * kLabelCount, FlowCell{});
  pairs_.assign(15 * cfg_.hub_count * cfg_.hub_count, 0.0);
  whales_ = std::make_unique<WhaleRecord[]>(std::max<std::size_t>(cfg_.whale_capacity, 1));
  volumes_ = std::make_unique<VolumeRecord[]>(std::max<std::size_t>(cfg_.volume_capacity, 1));
  transitions_ = std::make_unique<FeedTransition[]>(std::max<std::size_t>(cfg_.transition_capacity, 1) * kFeedCount);
  reset(0);
}

void LiveModel::reset(std::int64_t sessionStartMs) {
  session_start_ms_ = sessionStartMs;
  now_ms_ = sessionStartMs;
  last_second_ = -1;
  corridor_second_ = floor_div(sessionStartMs, 1000) - 1;
  std::fill(flows_.begin(), flows_.end(), FlowCell{});
  std::fill(pairs_.begin(), pairs_.end(), 0.0);
  sec_ = {};
  min_ = {};
  feed_sec_ = {};
  sess_in_ = {};
  sess_out_ = {};
  sess_tx_ = {};
  sess_whales_ = {};
  hub_labels_ = {};
  feed_events_ = {};
  feed_usd_ = {};
  feed_label_ = {};
  vol_acc_ = {};
  vol_second_ = -1;
  whale_overflow_ = volume_overflow_ = 0;
  whale_n_ = volume_n_ = 0;
  transition_n_ = {};
}

LiveModel::SecBucket& LiveModel::sec_bucket(std::int64_t second) noexcept {
  SecBucket& b = sec_[ring_index(second, sec_.size())];
  if (b.second != second) {
    if (b.second > second) {  // older than the ring: account it nowhere (never in practice)
      static thread_local SecBucket dummy;
      dummy = SecBucket{};
      return dummy;
    }
    b = SecBucket{};
    b.second = second;
  }
  return b;
}

LiveModel::MinBucket& LiveModel::min_bucket(std::int64_t minute) noexcept {
  const std::size_t i = ring_index(minute, min_.size());
  MinBucket& b = min_[i];
  if (b.minute != minute) {
    if (b.minute > minute) {
      static thread_local MinBucket dummy;
      dummy = MinBucket{};
      return dummy;
    }
    b = MinBucket{};
    b.minute = minute;
    std::fill_n(pairs_.begin() + static_cast<std::ptrdiff_t>(i * cfg_.hub_count * cfg_.hub_count),
                cfg_.hub_count * cfg_.hub_count, 0.0);
  }
  return b;
}

double* LiveModel::pair_row(std::int64_t minute) noexcept {
  const std::size_t i = ring_index(minute, min_.size());
  if (min_[i].minute != minute) return nullptr;
  return pairs_.data() + i * cfg_.hub_count * cfg_.hub_count;
}

LiveModel::FeedSec& LiveModel::feed_sec(std::int64_t second) noexcept {
  FeedSec& b = feed_sec_[ring_index(second, feed_sec_.size())];
  if (b.second != second && b.second < second) {
    b = FeedSec{};
    b.second = second;
  }
  return b;
}

std::uint8_t LiveModel::whale_tier(FeedId f, double usd) const noexcept {
  const double t = cfg_.whale_usd[static_cast<std::size_t>(f)];
  if (t <= 0.0 || usd < t) return 0;
  return usd >= 10.0 * t ? 3 : usd >= 3.0 * t ? 2 : 1;
}

void LiveModel::flush_volume(std::int64_t second) noexcept {
  for (std::size_t i = 0; i < vol_acc_.size(); ++i) {
    VolAcc& a = vol_acc_[i];
    if (!a.dirty) continue;
    if (volume_n_ < cfg_.volume_capacity) {
      volumes_[volume_n_++] = VolumeRecord{second, static_cast<FeedId>(i / kLabelCount),
                                           static_cast<SourceLabel>(i % kLabelCount), 0, a.tx, a.usd};
    } else {
      ++volume_overflow_;
    }
    a = VolAcc{};
  }
}

void LiveModel::run_corridors(std::int64_t second) noexcept {
  if (!cfg_.fiat_enabled || cfg_.corridors.empty()) return;
  std::uint32_t txs = 0;
  double usd = 0.0;
  for (const Corridor& c : cfg_.corridors) {
    if (c.src_hub >= cfg_.hub_count || c.dst_hub >= cfg_.hub_count) continue;
    const double half = c.usd_per_day / 86400.0 * 0.5;  // even split between the two directions
    for (int dir = 0; dir < 2; ++dir) {
      const std::uint16_t s = dir == 0 ? c.src_hub : c.dst_hub;
      const std::uint16_t d = dir == 0 ? c.dst_hub : c.src_hub;
      const std::size_t L = static_cast<std::size_t>(SourceLabel::Modeled);
      FlowCell& cell = flows_[flow_index(s, d, static_cast<std::size_t>(c.currency), L)];
      cell.usd += half;
      SecBucket& sb = sec_bucket(second);
      sb.out[s] += half;
      sb.in[d] += half;
      const std::int64_t minute = floor_div(second, 60);
      MinBucket& mb = min_bucket(minute);
      mb.out[s] += half;
      mb.in[d] += half;
      if (double* row = pair_row(minute)) row[s * cfg_.hub_count + d] += half;
      sess_out_[s] += half;
      sess_in_[d] += half;
      hub_labels_[s] |= static_cast<std::uint8_t>(1u << L);
      hub_labels_[d] |= static_cast<std::uint8_t>(1u << L);
      usd += half;
    }
  }
  const std::size_t F = static_cast<std::size_t>(FeedId::Fiat);
  FeedSec& fs = feed_sec(second);
  fs.usd[F] += usd;
  fs.tx[F] += txs;
  feed_usd_[F] += usd;
  feed_label_[F] = static_cast<std::uint8_t>(SourceLabel::Modeled);
  if (volume_n_ < cfg_.volume_capacity) {
    volumes_[volume_n_++] = VolumeRecord{second, FeedId::Fiat, SourceLabel::Modeled, 0, txs, usd};
  } else {
    ++volume_overflow_;
  }
}

void LiveModel::advance(std::int64_t nowMs) noexcept {
  if (nowMs > now_ms_) now_ms_ = nowMs;
  const std::int64_t second = floor_div(now_ms_, 1000);
  if (vol_second_ >= 0 && second > vol_second_) {
    flush_volume(vol_second_);
    vol_second_ = second;
  }
  // Modeled corridors for every whole second that has ended.
  if (second - 1 > corridor_second_) {
    std::int64_t from = corridor_second_ + 1;
    if (second - from > 900) from = second - 900;  // catch-up cap (e.g. after a long pause)
    for (std::int64_t s = from; s <= second - 1; ++s) run_corridors(s);
    corridor_second_ = second - 1;
  }
}

bool LiveModel::apply(const LiveEvent& e, std::int64_t nowMs, WhaleRecord* visual) noexcept {
  advance(nowMs);
  const std::int64_t second = floor_div(now_ms_, 1000);
  if (vol_second_ < 0) vol_second_ = second;
  const std::size_t F = static_cast<std::size_t>(e.feed) % kFeedCount;
  const std::size_t L = static_cast<std::size_t>(e.label) % kLabelCount;
  const std::size_t A = static_cast<std::size_t>(e.asset) % kAssetCount;
  const std::size_t s = std::min<std::size_t>(e.src_hub, cfg_.hub_count - 1);
  const std::size_t d = std::min<std::size_t>(e.dst_hub, cfg_.hub_count - 1);
  if (e.kind == EventKind::Ticker) return false;

  const double usd = std::isfinite(e.usd_value) && e.usd_value > 0.0 ? e.usd_value : 0.0;
  const std::uint32_t n = e.count;
  ++feed_events_[F];
  feed_usd_[F] += usd;
  feed_label_[F] = static_cast<std::uint8_t>(L);
  FeedSec& fs = feed_sec(second);
  fs.tx[F] += n;
  fs.usd[F] += usd;
  VolAcc& va = vol_acc_[F * kLabelCount + L];
  va.tx += n;
  va.usd += usd;
  va.dirty = true;

  // A block header is a count, not a flow between hubs.
  if (e.kind == EventKind::Block) return false;

  FlowCell& cell = flows_[flow_index(s, d, A, L)];
  cell.usd += usd;
  cell.tx += n;
  SecBucket& sb = sec_bucket(second);
  sb.out[s] += usd;
  sb.in[d] += usd;
  sb.tx[s] += n;
  if (d != s) sb.tx[d] += n;
  const std::int64_t minute = floor_div(second, 60);
  MinBucket& mb = min_bucket(minute);
  mb.out[s] += usd;
  mb.in[d] += usd;
  mb.tx[s] += n;
  if (d != s) mb.tx[d] += n;
  if (double* row = pair_row(minute)) row[s * cfg_.hub_count + d] += usd;
  sess_out_[s] += usd;
  sess_in_[d] += usd;
  sess_tx_[s] += n;
  if (d != s) sess_tx_[d] += n;
  hub_labels_[s] |= static_cast<std::uint8_t>(1u << L);
  hub_labels_[d] |= static_cast<std::uint8_t>(1u << L);

  const std::uint8_t tier = (e.kind == EventKind::Transaction || e.kind == EventKind::Trade) ? whale_tier(e.feed, usd) : 0;
  if (tier == 0) return false;
  const WhaleRecord w{e.ts_ms, usd, e.feed, e.asset, e.label, tier, static_cast<std::uint16_t>(s),
                      static_cast<std::uint16_t>(d)};
  if (whale_n_ < cfg_.whale_capacity) {
    whales_[whale_n_++] = w;
  } else {
    ++whale_overflow_;
  }
  mb.whales[s] += 1;
  if (d != s) mb.whales[d] += 1;
  sess_whales_[s] += 1;
  if (d != s) sess_whales_[d] += 1;
  if (visual) *visual = w;
  return true;
}

void LiveModel::record_transition(FeedId feed, const FeedTransition& t) noexcept {
  const std::size_t f = static_cast<std::size_t>(feed) % kFeedCount;
  if (transition_n_[f] < cfg_.transition_capacity) {
    transitions_[f * cfg_.transition_capacity + transition_n_[f]++] = t;
  }
}

void LiveModel::fill_snapshot(LiveSnapshot& o) const noexcept {
  const std::size_t H = cfg_.hub_count;
  o.now_ms = now_ms_;
  o.session_start_ms = session_start_ms_;
  o.hub_count = H;
  const std::int64_t nowS = floor_div(now_ms_, 1000);
  const std::int64_t nowM = floor_div(nowS, 60);
  const double elapsed = std::max(1.0, static_cast<double>(now_ms_ - session_start_ms_) / 1000.0);
  for (std::size_t h = 0; h < H; ++h) {
    HubPanelStats st{};
    std::uint64_t tx60 = 0;
    for (const SecBucket& b : sec_) {
      if (b.second > nowS - 60 && b.second <= nowS) {
        st.in_usd[0] += b.in[h];
        st.out_usd[0] += b.out[h];
        tx60 += b.tx[h];
      }
    }
    double partner[kMaxModelHubs] = {};
    for (std::size_t m = 0; m < min_.size(); ++m) {
      const MinBucket& b = min_[m];
      if (b.minute <= nowM - 15 || b.minute > nowM) continue;
      st.in_usd[1] += b.in[h];
      st.out_usd[1] += b.out[h];
      st.whales_15m += b.whales[h];
      const double* row = pairs_.data() + m * H * H;
      for (std::size_t k = 0; k < H; ++k) {
        if (k == h) continue;
        partner[k] += row[h * H + k] + row[k * H + h];
      }
    }
    st.in_usd[2] = sess_in_[h];
    st.out_usd[2] = sess_out_[h];
    st.tx_per_s = static_cast<double>(tx60) / std::min(60.0, elapsed);
    st.label_mask = hub_labels_[h];
    for (int slot = 0; slot < 3; ++slot) {
      int best = -1;
      for (std::size_t k = 0; k < H; ++k) {
        if (partner[k] <= 0.0) continue;
        bool used = false;
        for (int j = 0; j < slot; ++j) used = used || st.top[j] == static_cast<int>(k);
        if (!used && (best < 0 || partner[k] > partner[static_cast<std::size_t>(best)])) best = static_cast<int>(k);
      }
      st.top[slot] = static_cast<std::int16_t>(best);
      st.top_usd[slot] = best >= 0 ? partner[static_cast<std::size_t>(best)] : 0.0;
    }
    o.hubs[h] = st;
  }
  for (std::size_t f = 0; f < kFeedCount; ++f) {
    FeedPanelStats fp{};
    double tx = 0.0, usd = 0.0;
    for (const FeedSec& b : feed_sec_) {
      if (b.second >= nowS - 10 && b.second < nowS) {
        tx += b.tx[f];
        usd += b.usd[f];
      }
    }
    const double span = std::min(10.0, std::max(1.0, elapsed - 1.0));
    fp.tx_per_s = tx / span;
    fp.usd_per_s = usd / span;
    fp.events = feed_events_[f];
    fp.usd_total = feed_usd_[f];
    fp.last_label = feed_label_[f];
    o.feeds[f] = fp;
    o.transition_count[f] = transition_n_[f];
  }
  const std::size_t cells = std::min(flows_.size(), o.flows.size());
  std::copy_n(flows_.begin(), cells, o.flows.begin());
  o.whale_count = whale_n_;
  o.volume_count = volume_n_;
  o.whale_overflow = whale_overflow_;
  o.volume_overflow = volume_overflow_;
  o.checksum = checksum();
}

std::uint64_t LiveModel::checksum() const noexcept {
  XXH3_state_t st;
  XXH3_64bits_reset(&st);
  XXH3_64bits_update(&st, flows_.data(), flows_.size() * sizeof(FlowCell));
  XXH3_64bits_update(&st, sec_.data(), sizeof(sec_));
  XXH3_64bits_update(&st, min_.data(), sizeof(min_));
  XXH3_64bits_update(&st, pairs_.data(), pairs_.size() * sizeof(double));
  XXH3_64bits_update(&st, feed_sec_.data(), sizeof(feed_sec_));
  XXH3_64bits_update(&st, sess_in_.data(), sizeof(sess_in_));
  XXH3_64bits_update(&st, sess_out_.data(), sizeof(sess_out_));
  XXH3_64bits_update(&st, sess_tx_.data(), sizeof(sess_tx_));
  XXH3_64bits_update(&st, sess_whales_.data(), sizeof(sess_whales_));
  XXH3_64bits_update(&st, feed_events_.data(), sizeof(feed_events_));
  XXH3_64bits_update(&st, feed_usd_.data(), sizeof(feed_usd_));
  XXH3_64bits_update(&st, whales_.get(), whale_n_ * sizeof(WhaleRecord));
  XXH3_64bits_update(&st, volumes_.get(), volume_n_ * sizeof(VolumeRecord));
  return XXH3_64bits_digest(&st);
}

void LiveModel::save_state(std::vector<std::uint8_t>& out) const {
  out.clear();
  put(out, kStateMagic);
  put(out, kStateVersion);
  put(out, static_cast<std::uint64_t>(cfg_.hub_count));
  put(out, session_start_ms_);
  put(out, now_ms_);
  put(out, last_second_);
  put(out, corridor_second_);
  put(out, vol_second_);
  put_array(out, flows_.data(), flows_.size());
  put_array(out, sec_.data(), sec_.size());
  put_array(out, min_.data(), min_.size());
  put_array(out, pairs_.data(), pairs_.size());
  put_array(out, feed_sec_.data(), feed_sec_.size());
  put(out, sess_in_);
  put(out, sess_out_);
  put(out, sess_tx_);
  put(out, sess_whales_);
  put(out, hub_labels_);
  put(out, feed_events_);
  put(out, feed_usd_);
  put(out, feed_label_);
  put(out, vol_acc_);
  put(out, whale_overflow_);
  put(out, volume_overflow_);
  put(out, static_cast<std::uint64_t>(whale_n_));
  put(out, static_cast<std::uint64_t>(volume_n_));
}

bool LiveModel::load_state(const std::uint8_t* data, std::size_t size) noexcept {
  Reader r{data, size};
  std::uint32_t magic = 0, version = 0;
  std::uint64_t hubs = 0, wn = 0, vn = 0;
  if (!r.get(magic) || magic != kStateMagic || !r.get(version) || version != kStateVersion || !r.get(hubs) ||
      hubs != cfg_.hub_count) {
    return false;
  }
  bool ok = r.get(session_start_ms_) && r.get(now_ms_) && r.get(last_second_) && r.get(corridor_second_) &&
            r.get(vol_second_) && r.get_array(flows_.data(), flows_.size()) && r.get_array(sec_.data(), sec_.size()) &&
            r.get_array(min_.data(), min_.size()) && r.get_array(pairs_.data(), pairs_.size()) &&
            r.get_array(feed_sec_.data(), feed_sec_.size()) && r.get(sess_in_) && r.get(sess_out_) && r.get(sess_tx_) &&
            r.get(sess_whales_) && r.get(hub_labels_) && r.get(feed_events_) && r.get(feed_usd_) &&
            r.get(feed_label_) && r.get(vol_acc_) && r.get(whale_overflow_) && r.get(volume_overflow_) && r.get(wn) &&
            r.get(vn) && r.left == 0;
  if (!ok || wn > whale_n_ || vn > volume_n_) return false;
  truncate_journals(static_cast<std::size_t>(wn), static_cast<std::size_t>(vn));
  return true;
}

void LiveModel::truncate_journals(std::size_t whales, std::size_t volumes) noexcept {
  whale_n_ = std::min(whale_n_, whales);
  volume_n_ = std::min(volume_n_, volumes);
}

// ---------------------------------------------------------------------------
// SnapshotExchange
// ---------------------------------------------------------------------------

bool SnapshotExchange::try_publish(const LiveModel& model) noexcept {
  // seq_cst on both sides (Dekker): the writer's readers_[b] load cannot be
  // reordered before a reader's increment-then-recheck, so a reader that saw
  // the old front either keeps the writer away from that slot or retries.
  const std::uint32_t f = front_.load(std::memory_order_seq_cst);
  const std::uint32_t b = published_.load(std::memory_order_seq_cst) ? 1u - f : f;
  if (readers_[b].load(std::memory_order_seq_cst) != 0) return false;  // a reader still holds it: retry next tick
  LiveSnapshot& s = slots_[b];
  model.fill_snapshot(s);
  s.seq = ++seq_;
  front_.store(b, std::memory_order_seq_cst);
  published_.store(true, std::memory_order_seq_cst);
  return true;
}

const LiveSnapshot* SnapshotExchange::acquire() noexcept {
  if (!published_.load(std::memory_order_seq_cst)) return nullptr;
  for (;;) {
    const std::uint32_t f = front_.load(std::memory_order_seq_cst);
    readers_[f].fetch_add(1, std::memory_order_seq_cst);
    if (front_.load(std::memory_order_seq_cst) == f) return &slots_[f];
    readers_[f].fetch_sub(1, std::memory_order_seq_cst);  // flipped meanwhile: pin the new front
  }
}

void SnapshotExchange::release(const LiveSnapshot* s) noexcept {
  if (!s) return;
  const auto i = static_cast<std::size_t>(s - slots_.data());
  readers_[i].fetch_sub(1, std::memory_order_seq_cst);
}

// ---------------------------------------------------------------------------
// flows_baseline.csv
// ---------------------------------------------------------------------------

bool load_corridors(const std::string& path, const HubTaxonomy& taxonomy, std::vector<Corridor>& out,
                    std::string& error) {
  out.clear();
  std::ifstream f(path);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  std::string line;
  bool header = true;
  int lineNo = 0;
  while (std::getline(f, line)) {
    ++lineNo;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string c;
    while (std::getline(ss, c, ',')) cols.push_back(c);
    if (header) {
      header = false;
      if (cols.size() < 7 || cols[0] != "currency") {
        error = path + ": unexpected header";
        return false;
      }
      continue;
    }
    if (cols.size() < 7) {
      error = path + ":" + std::to_string(lineNo) + ": expected 7 columns";
      return false;
    }
    Corridor k;
    bool found = false;
    for (std::size_t i = 0; i < kAssetCount; ++i) {
      if (cols[0] == kAssetNames[i]) {
        k.currency = static_cast<Asset>(i);
        found = true;
      }
    }
    const auto s = taxonomy.find(cols[1]);
    const auto d = taxonomy.find(cols[2]);
    if (!found || !s || !d) {
      error = path + ":" + std::to_string(lineNo) + ": unknown currency or hub";
      return false;
    }
    k.src_hub = *s;
    k.dst_hub = *d;
    k.share_pct = std::strtod(cols[3].c_str(), nullptr);
    k.usd_per_day = std::strtod(cols[4].c_str(), nullptr);
    k.source_url = cols[5];
    k.as_of_date = cols[6];
    if (!(k.usd_per_day >= 0.0) || k.source_url.empty() || k.as_of_date.empty()) {
      error = path + ":" + std::to_string(lineNo) + ": missing volume, source_url or as_of_date";
      return false;
    }
    out.push_back(std::move(k));
  }
  return true;
}

}  // namespace ftv
