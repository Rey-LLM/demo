#include "ingest/parse_worker.hpp"

#include <xxhash.h>

#include <algorithm>
#include <cstring>

#include "ingest/field_parsers.hpp"
#include "ingest/ftxb_format.hpp"

namespace ftv {

std::size_t EdgeKeyHash::operator()(const EdgeKey& k) const noexcept {
  return static_cast<std::size_t>(XXH3_64bits(&k, sizeof(EdgeKey)));
}

ParseWorker::ParseWorker(const CsvSchema* schema, std::size_t expectedRows, unsigned logMalformed,
                         CollisionVerifier* verifier)
    : schema_(schema),
      log_malformed_(std::min(logMalformed, kMaxLoggedMalformed)),
      verifier_(verifier),
      // Reserve from the input-size estimate so the per-row path does not
      // rehash in the common case (a vote per endpoint, an edge per row).
      votes_(expectedRows / 2 + 1024),
      edges_(expectedRows * 3 / 4 + 1024) {}

void ParseWorker::record_malformed(std::uint64_t offset) noexcept {
  ++stats_.rows_malformed;
  if (stats_.malformed_logged < log_malformed_) stats_.malformed_offsets[stats_.malformed_logged++] = offset;
}

bool ParseWorker::parse_csv_line(std::string_view line, const char* fileBase, RowView& row) noexcept {
  CsvRowFields f;
  if (!split_csv_line(line, *schema_, f)) return false;

  const auto& fTs = f[static_cast<std::size_t>(CsvColumn::Timestamp)];
  const auto& fAmt = f[static_cast<std::size_t>(CsvColumn::Amount)];
  const auto& fCur = f[static_cast<std::size_t>(CsvColumn::Currency)];
  const auto& fChan = f[static_cast<std::size_t>(CsvColumn::Channel)];
  const auto& fSrc = f[static_cast<std::size_t>(CsvColumn::SrcAccount)];
  const auto& fDst = f[static_cast<std::size_t>(CsvColumn::DstAccount)];
  const auto& fSj = f[static_cast<std::size_t>(CsvColumn::SrcJurisdiction)];
  const auto& fDj = f[static_cast<std::size_t>(CsvColumn::DstJurisdiction)];
  const auto& fChain = f[static_cast<std::size_t>(CsvColumn::Chain)];

  if (fTs.has_escapes || fAmt.has_escapes || fCur.has_escapes || fChan.has_escapes) return false;
  if (!parse_timestamp_ms(fTs.text, row.ts_ms)) return false;
  row.currency = pack_code4(trim(fCur.text));
  if (row.currency == 0) return false;
  if (!parse_channel(fChan.text, row.channel)) return false;
  bool truncated = false;
  if (!parse_amount_minor(fAmt.text, currency_scale(row.currency), row.amount_minor, &truncated)) return false;
  if (truncated) ++stats_.amounts_truncated;

  const std::string_view sj = trim(fSj.text);
  const std::string_view dj = trim(fDj.text);
  const std::string_view ch = trim(fChain.text);
  row.src_juris = sj.empty() ? 0 : pack_code8(sj);
  row.dst_juris = dj.empty() ? 0 : pack_code8(dj);
  row.chain = ch.empty() ? 0 : pack_code8(ch);
  if ((!sj.empty() && row.src_juris == 0) || (!dj.empty() && row.dst_juris == 0) || (!ch.empty() && row.chain == 0)) {
    return false;  // over-long or non-printable codes
  }
  row.src = fSrc.text;
  row.dst = fDst.text;
  row.src_escapes = fSrc.has_escapes;
  row.dst_escapes = fDst.has_escapes;
  row.src_file_offset = static_cast<std::uint64_t>(fSrc.text.data() - fileBase);
  row.dst_file_offset = static_cast<std::uint64_t>(fDst.text.data() - fileBase);
  return true;
}

bool ParseWorker::accept_row(const RowView& row) noexcept {
  std::uint64_t srcLoc = 0;
  std::uint64_t dstLoc = 0;
  if (row.channel == Channel::ONCHAIN) {
    if (row.chain == 0) return false;  // ONCHAIN rows must name their chain
    if (!normalize_onchain_key(row.src, row.src_escapes, src_key_)) return false;
    if (!normalize_onchain_key(row.dst, row.dst_escapes, dst_key_)) return false;
    srcLoc = dstLoc = row.chain | kChainDomainBit;
  } else {
    if (row.chain != 0) return false;  // fiat rows must leave chain empty
    if (!normalize_fiat_key(row.src, row.src_escapes, src_key_)) return false;
    if (!normalize_fiat_key(row.dst, row.dst_escapes, dst_key_)) return false;
    srcLoc = row.src_juris;  // 0 = unknown jurisdiction -> fallback hub later
    dstLoc = row.dst_juris;
  }

  const std::uint64_t src64 = hash_normalized(src_key_, row.chain);
  const std::uint64_t dst64 = hash_normalized(dst_key_, row.chain);
#ifndef NDEBUG
  if (verifier_) {
    verifier_->check(src64, src_key_.is_chain_address() ? chain_namespace_tag(row.chain) : kFiatNamespaceTag,
                     src_key_.view());
    verifier_->check(dst64, dst_key_.is_chain_address() ? chain_namespace_tag(row.chain) : kFiatNamespaceTag,
                     dst_key_.view());
  }
#else
  (void)verifier_;
#endif

  const auto vote = [this](std::uint64_t key, std::uint64_t loc, std::uint64_t off, std::size_t len) {
    bool inserted = false;
    VoteVal& v = votes_.find_or_insert(VoteKey{key, loc}, inserted);
    if (inserted) {
      v.label_offset = off;
      v.label_len = static_cast<std::uint32_t>(std::min<std::size_t>(len, 0xFFFF));
    } else if (off < v.label_offset) {
      v.label_offset = off;  // keep the earliest occurrence: scheduling independent
      v.label_len = static_cast<std::uint32_t>(std::min<std::size_t>(len, 0xFFFF));
    }
    ++v.count;
  };
  vote(src64, srcLoc, row.src_file_offset, row.src.size());
  vote(dst64, dstLoc, row.dst_file_offset, row.dst.size());

  bool inserted = false;
  EdgeVal& e = edges_.find_or_insert(EdgeKey{src64, dst64, row.currency, static_cast<std::uint32_t>(row.channel)},
                                     inserted);
  if (inserted) {
    e.first_ts = row.ts_ms;
    e.last_ts = row.ts_ms;
  } else {
    e.first_ts = std::min(e.first_ts, row.ts_ms);
    e.last_ts = std::max(e.last_ts, row.ts_ms);
  }
  e.total.add(row.amount_minor);
  ++e.count;
  return true;
}

void ParseWorker::process_csv_chunk(const char* fileBase, const Chunk& chunk) noexcept {
  const char* p = fileBase + chunk.offset;
  const char* const end = p + chunk.size;
  stats_.bytes += chunk.size;
  RowView row;
  while (p < end) {
    const void* nl = std::memchr(p, '\n', static_cast<std::size_t>(end - p));
    const char* lineEnd = nl ? static_cast<const char*>(nl) : end;
    std::string_view line(p, static_cast<std::size_t>(lineEnd - p));
    const std::uint64_t lineOffset = static_cast<std::uint64_t>(p - fileBase);
    p = lineEnd + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (trim(line).empty()) {
      ++stats_.rows_blank;
      continue;
    }
    if (parse_csv_line(line, fileBase, row) && accept_row(row)) {
      ++stats_.rows_ok;
    } else {
      record_malformed(lineOffset);
    }
  }
}

void ParseWorker::process_ftxb_chunk(const char* fileBase, const Chunk& chunk, std::uint64_t poolOffset,
                                     std::uint64_t poolSize) noexcept {
  stats_.bytes += chunk.size;
  const std::uint64_t n = chunk.size / sizeof(FtxbRecord);
  const char* pool = fileBase + poolOffset;
  RowView row;
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::uint64_t recOffset = chunk.offset + i * sizeof(FtxbRecord);
    const FtxbRecord r = load_ftxb_record(fileBase + recOffset);
    const bool refsOk = r.channel < kChannelCount && r.reserved == 0 && r.currency != 0 &&
                        static_cast<std::uint64_t>(r.src_off) + r.src_len <= poolSize &&
                        static_cast<std::uint64_t>(r.dst_off) + r.dst_len <= poolSize &&
                        static_cast<std::uint64_t>(r.tx_id_off) + r.tx_id_len <= poolSize;
    if (!refsOk) {
      record_malformed(recOffset);
      continue;
    }
    row.ts_ms = r.timestamp_ms;
    row.amount_minor = r.amount_minor;
    row.currency = r.currency;
    row.channel = static_cast<Channel>(r.channel);
    row.src_juris = r.src_jurisdiction;
    row.dst_juris = r.dst_jurisdiction;
    row.chain = r.chain;
    row.src = std::string_view(pool + r.src_off, r.src_len);
    row.dst = std::string_view(pool + r.dst_off, r.dst_len);
    row.src_escapes = row.dst_escapes = false;
    row.src_file_offset = poolOffset + r.src_off;
    row.dst_file_offset = poolOffset + r.dst_off;
    if (accept_row(row)) {
      ++stats_.rows_ok;
    } else {
      record_malformed(recOffset);
    }
  }
}

}  // namespace ftv
