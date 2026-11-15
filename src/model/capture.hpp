#pragma once
// .ftxcap replay captures: a time-sorted array of LiveEvents plus a capture
// index with a keyframe every 10 s (the LiveModel state at that instant and
// the first record at or after it). Seeking loads the nearest keyframe at or
// before the target and replays forward deterministically, so seek-then-play
// gives the same model checksum as continuous play.
//
// File layout (little-endian):
//   char[4]  "FTXC"            u32 version (1)
//   u32      flags             bit 0: SYNTHETIC (generated, not recorded)
//   u32      record count      i64 t0_ms   i64 duration_ms
//   u32      keyframe interval (ms)        u32 keyframe count
//   u64      model config hash (keyframes are valid only for that config)
//   char[160] description (NUL padded)
//   LiveEvent records[count]
//   per keyframe: u32 first record, i64 time (ms from t0), u32 encoded size, bytes
// Keyframe state is zero-run encoded: [u32 zeros][u32 literal n][n bytes]...
//
// When the runtime model configuration differs from the one the keyframes
// were built with (other thresholds or corridors), load() rebuilds them in
// memory with the current configuration.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ingest/live/live_event.hpp"
#include "model/live_model.hpp"

namespace ftv {

inline constexpr std::uint32_t kCaptureSynthetic = 1u;

struct Keyframe {
  std::uint32_t first_record = 0;
  std::int64_t time_ms = 0;  // from t0
  std::vector<std::uint8_t> state;  // decoded LiveModel::save_state() bytes
};

class Capture {
 public:
  bool load(const std::string& path, const LiveModelConfig& modelCfg, std::string& error);
  bool save(const std::string& path, std::string& error) const;

  // Builds keyframes by running a model over the records.
  void build_keyframes(const LiveModelConfig& modelCfg, std::uint32_t intervalMs = 10000);

  std::vector<LiveEvent> records;  // ts_ms absolute (capture clock), sorted
  std::int64_t t0_ms = 0;
  std::int64_t duration_ms = 0;
  std::uint32_t flags = 0;
  std::uint32_t keyframe_interval_ms = 10000;
  std::uint64_t config_hash = 0;
  std::string description;
  std::vector<Keyframe> keyframes;
  bool keyframes_rebuilt = false;  // load() had to rebuild them

  [[nodiscard]] bool synthetic() const noexcept { return (flags & kCaptureSynthetic) != 0; }
};

std::vector<std::uint8_t> zero_run_encode(const std::vector<std::uint8_t>& in);
bool zero_run_decode(const std::uint8_t* in, std::size_t n, std::vector<std::uint8_t>& out);

// Drives a LiveModel from a capture on the capture's own clock (ms from t0,
// looping). Used by --replay on the model thread, by the screenshot mode and
// by the seek-determinism test. Never allocates after construction except in
// seek() (keyframe decode is preallocated too; see .cpp).
class ReplayDriver {
 public:
  explicit ReplayDriver(const Capture& cap) : cap_(cap) {}

  // Resets the model to the capture start (session start = t0).
  void start(LiveModel& model) noexcept;
  // Plays forward to `relMs` (ms since t0 on the looping timeline). Whales
  // are reported through `onWhale(record, relSeconds)` when given.
  template <class F>
  // onApplied(event, whale or nullptr, timeline seconds) for every applied record.
  void advance_to(LiveModel& model, std::int64_t relMs, F&& onApplied) noexcept;
  void advance_to(LiveModel& model, std::int64_t relMs) noexcept {
    advance_to(model, relMs, [](const LiveEvent&, const WhaleRecord*, double) {});
  }
  // Jumps to `relMs` within [0, duration): nearest keyframe, then forward.
  bool seek(LiveModel& model, std::int64_t relMs) noexcept;

  [[nodiscard]] std::int64_t position_ms() const noexcept { return pos_ms_; }
  [[nodiscard]] std::int64_t loop_position_ms() const noexcept {
    return cap_.duration_ms > 0 ? pos_ms_ % cap_.duration_ms : 0;
  }
  [[nodiscard]] const Capture& capture() const noexcept { return cap_; }

 private:
  const Capture& cap_;
  std::size_t cursor_ = 0;      // next record within the current loop
  std::int64_t loop_ = 0;       // completed loops
  std::int64_t pos_ms_ = 0;     // timeline position (ms since t0, including loops)
};

template <class F>
void ReplayDriver::advance_to(LiveModel& model, std::int64_t relMs, F&& onApplied) noexcept {
  const std::int64_t D = cap_.duration_ms > 0 ? cap_.duration_ms : 1;
  while (true) {
    const std::int64_t loopBase = loop_ * D;
    while (cursor_ < cap_.records.size()) {
      const LiveEvent& src = cap_.records[cursor_];
      const std::int64_t rel = src.ts_ms - cap_.t0_ms + loopBase;
      if (rel > relMs) break;
      LiveEvent e = src;
      e.ts_ms = cap_.t0_ms + rel;
      e.label = SourceLabel::Replay;
      WhaleRecord w;
      const bool whale = model.apply(e, e.ts_ms, &w);
      onApplied(e, whale ? &w : nullptr, static_cast<double>(rel) / 1000.0);
      ++cursor_;
    }
    if (cursor_ < cap_.records.size() || relMs < loopBase + D) break;
    ++loop_;  // loop: the timeline continues, the records start over
    cursor_ = 0;
  }
  model.advance(cap_.t0_ms + relMs);
  pos_ms_ = relMs;
}

}  // namespace ftv
