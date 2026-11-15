#include "model/capture.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "core/file_util.hpp"
#include "model/feed_config.hpp"

namespace ftv {

namespace {

constexpr char kMagic[4] = {'F', 'T', 'X', 'C'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kDescLen = 160;

template <class T>
void put(std::vector<char>& out, const T& v) {
  const char* p = reinterpret_cast<const char*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

struct In {
  const std::vector<char>& b;
  std::size_t at = 0;
  template <class T>
  bool get(T& v) {
    if (at + sizeof(T) > b.size()) return false;
    std::memcpy(&v, b.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
  }
};

}  // namespace

std::vector<std::uint8_t> zero_run_encode(const std::vector<std::uint8_t>& in) {
  std::vector<std::uint8_t> out;
  std::size_t i = 0;
  auto put32 = [&out](std::uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
  };
  while (i < in.size()) {
    std::size_t z = i;
    while (z < in.size() && in[z] == 0) ++z;
    std::size_t l = z;
    // A literal run ends at the next run of 8+ zero bytes.
    while (l < in.size()) {
      if (in[l] == 0) {
        std::size_t r = l;
        while (r < in.size() && in[r] == 0 && r - l < 8) ++r;
        if (r - l >= 8 || r == in.size()) break;
        l = r;
      } else {
        ++l;
      }
    }
    put32(static_cast<std::uint32_t>(z - i));
    put32(static_cast<std::uint32_t>(l - z));
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(z), in.begin() + static_cast<std::ptrdiff_t>(l));
    i = l;
  }
  return out;
}

bool zero_run_decode(const std::uint8_t* in, std::size_t n, std::vector<std::uint8_t>& out) {
  out.clear();
  std::size_t i = 0;
  auto get32 = [&](std::uint32_t& v) {
    if (i + 4 > n) return false;
    v = static_cast<std::uint32_t>(in[i]) | (static_cast<std::uint32_t>(in[i + 1]) << 8) |
        (static_cast<std::uint32_t>(in[i + 2]) << 16) | (static_cast<std::uint32_t>(in[i + 3]) << 24);
    i += 4;
    return true;
  };
  while (i < n) {
    std::uint32_t zeros = 0, lit = 0;
    if (!get32(zeros) || !get32(lit) || i + lit > n || out.size() + zeros + lit > (std::size_t{64} << 20)) return false;
    out.insert(out.end(), zeros, 0);
    out.insert(out.end(), in + i, in + i + lit);
    i += lit;
  }
  return true;
}

void Capture::build_keyframes(const LiveModelConfig& modelCfg, std::uint32_t intervalMs) {
  keyframes.clear();
  keyframe_interval_ms = intervalMs;
  config_hash = model_config_hash(modelCfg);
  LiveModelConfig cfg = modelCfg;
  cfg.whale_capacity = records.size() + 16;
  cfg.volume_capacity = static_cast<std::size_t>(duration_ms / 1000 + 2) * kFeedCount * kLabelCount + 16;
  LiveModel model(cfg);
  ReplayDriver drv(*this);
  drv.start(model);
  for (std::int64_t t = 0; t < duration_ms; t += intervalMs) {
    drv.advance_to(model, t);
    Keyframe k;
    k.time_ms = t;
    // First record not yet applied: rel > t.
    const auto it = std::upper_bound(records.begin(), records.end(), t0_ms + t,
                                     [](std::int64_t v, const LiveEvent& e) { return v < e.ts_ms; });
    k.first_record = static_cast<std::uint32_t>(it - records.begin());
    model.save_state(k.state);
    keyframes.push_back(std::move(k));
  }
}

bool Capture::save(const std::string& path, std::string& error) const {
  std::vector<char> out;
  out.insert(out.end(), kMagic, kMagic + 4);
  put(out, kVersion);
  put(out, flags);
  put(out, static_cast<std::uint32_t>(records.size()));
  put(out, t0_ms);
  put(out, duration_ms);
  put(out, keyframe_interval_ms);
  put(out, static_cast<std::uint32_t>(keyframes.size()));
  put(out, config_hash);
  char desc[kDescLen] = {};
  std::memcpy(desc, description.data(), std::min(description.size(), kDescLen - 1));
  out.insert(out.end(), desc, desc + kDescLen);
  const char* rec = reinterpret_cast<const char*>(records.data());
  out.insert(out.end(), rec, rec + records.size() * sizeof(LiveEvent));
  for (const Keyframe& k : keyframes) {
    const std::vector<std::uint8_t> enc = zero_run_encode(k.state);
    put(out, k.first_record);
    put(out, k.time_ms);
    put(out, static_cast<std::uint32_t>(enc.size()));
    out.insert(out.end(), enc.begin(), enc.end());
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f.write(out.data(), static_cast<std::streamsize>(out.size()))) {
      error = "cannot write " + tmp;
      return false;
    }
  }
  std::remove(path.c_str());
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    error = "cannot rename " + tmp;
    return false;
  }
  return true;
}

bool Capture::load(const std::string& path, const LiveModelConfig& modelCfg, std::string& error) {
  const auto file = read_text_file(path);  // a path on disk or an embedded resource
  if (!file) {
    error = "cannot open " + path;
    return false;
  }
  const std::vector<char> b(file->begin(), file->end());
  In in{b};
  char magic[4] = {};
  if (b.size() < 4 || (std::memcpy(magic, b.data(), 4), std::memcmp(magic, kMagic, 4) != 0)) {
    error = path + ": not an .ftxcap file";
    return false;
  }
  in.at = 4;
  std::uint32_t version = 0, count = 0, kfCount = 0;
  char desc[kDescLen] = {};
  if (!in.get(version) || version != kVersion || !in.get(flags) || !in.get(count) || !in.get(t0_ms) ||
      !in.get(duration_ms) || !in.get(keyframe_interval_ms) || !in.get(kfCount) || !in.get(config_hash) ||
      in.at + kDescLen > b.size()) {
    error = path + ": bad header";
    return false;
  }
  std::memcpy(desc, b.data() + in.at, kDescLen);
  desc[kDescLen - 1] = 0;
  description = desc;
  in.at += kDescLen;
  if (in.at + static_cast<std::size_t>(count) * sizeof(LiveEvent) > b.size() || duration_ms <= 0) {
    error = path + ": truncated records";
    return false;
  }
  records.resize(count);
  std::memcpy(records.data(), b.data() + in.at, static_cast<std::size_t>(count) * sizeof(LiveEvent));
  in.at += static_cast<std::size_t>(count) * sizeof(LiveEvent);
  for (std::size_t i = 1; i < records.size(); ++i) {
    if (records[i].ts_ms < records[i - 1].ts_ms) {
      error = path + ": records are not sorted by time";
      return false;
    }
  }
  keyframes.clear();
  for (std::uint32_t k = 0; k < kfCount; ++k) {
    Keyframe kf;
    std::uint32_t n = 0;
    if (!in.get(kf.first_record) || !in.get(kf.time_ms) || !in.get(n) || in.at + n > b.size() ||
        !zero_run_decode(reinterpret_cast<const std::uint8_t*>(b.data() + in.at), n, kf.state)) {
      error = path + ": bad keyframe index";
      return false;
    }
    in.at += n;
    keyframes.push_back(std::move(kf));
  }
  keyframes_rebuilt = false;
  if (config_hash != model_config_hash(modelCfg) || keyframes.empty()) {
    build_keyframes(modelCfg, keyframe_interval_ms ? keyframe_interval_ms : 10000);
    keyframes_rebuilt = true;
  }
  return true;
}

void ReplayDriver::start(LiveModel& model) noexcept {
  model.reset(cap_.t0_ms);
  cursor_ = 0;
  loop_ = 0;
  pos_ms_ = 0;
  model.advance(cap_.t0_ms);
}

bool ReplayDriver::seek(LiveModel& model, std::int64_t relMs) noexcept {
  if (cap_.duration_ms <= 0) return false;
  relMs = std::clamp<std::int64_t>(relMs, 0, cap_.duration_ms - 1);
  if (loop_ == 0 && relMs >= pos_ms_) {  // forward within the first pass: just play on
    advance_to(model, relMs);
    return true;
  }
  const Keyframe* best = nullptr;
  for (const Keyframe& k : cap_.keyframes) {
    if (k.time_ms <= relMs) best = &k;
  }
  if (!best || !model.load_state(best->state.data(), best->state.size())) {
    // No usable keyframe (should not happen): replay from the start.
    start(model);
    advance_to(model, relMs);
    return false;
  }
  loop_ = 0;
  cursor_ = best->first_record;
  pos_ms_ = best->time_ms;
  advance_to(model, relMs);
  return true;
}

}  // namespace ftv
