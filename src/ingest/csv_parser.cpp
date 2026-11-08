#include "ingest/csv_parser.hpp"

#include <cstring>

#include "ingest/types.hpp"

namespace ftv {

namespace {

constexpr std::array<std::string_view, kCsvColumnCount> kColumnNames = {
    "tx_id",  "timestamp_utc", "src_account",      "dst_account",      "amount",
    "currency", "channel",     "src_jurisdiction", "dst_jurisdiction", "chain"};

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

}  // namespace

std::size_t csv_header_length(std::string_view data) noexcept {
  const void* nl = std::memchr(data.data(), '\n', data.size());
  return nl ? static_cast<std::size_t>(static_cast<const char*>(nl) - data.data()) + 1 : data.size();
}

bool parse_csv_header(std::string_view line, CsvSchema& schema, std::string& error) {
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
      static_cast<unsigned char>(line[2]) == 0xBF) {
    line.remove_prefix(3);
  }
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

  schema.field_of_column.fill(-1);
  schema.column_of_field.fill(-1);
  std::size_t field = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == ',') {
      std::string_view name = trim(line.substr(start, i - start));
      if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
      for (std::size_t c = 0; c < kCsvColumnCount; ++c) {
        if (iequals(name, kColumnNames[c])) {
          if (schema.field_of_column[c] != -1) {
            error = "duplicate CSV column '" + std::string(name) + "'";
            return false;
          }
          if (field >= kMaxCsvFields) {
            error = "required CSV column '" + std::string(name) + "' appears after field 64";
            return false;
          }
          schema.field_of_column[c] = static_cast<int>(field);
          schema.column_of_field[field] = static_cast<std::int8_t>(c);
        }
      }
      ++field;
      start = i + 1;
    }
  }
  int maxIdx = -1;
  for (std::size_t c = 0; c < kCsvColumnCount; ++c) {
    if (schema.field_of_column[c] < 0) {
      error = "CSV header is missing required column '" + std::string(kColumnNames[c]) + "'";
      return false;
    }
    if (schema.field_of_column[c] > maxIdx) maxIdx = schema.field_of_column[c];
  }
  schema.required_fields = maxIdx + 1;
  return true;
}

bool split_csv_line(std::string_view line, const CsvSchema& schema, CsvRowFields& out) noexcept {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  const char* p = line.data();
  const char* const end = p + line.size();
  int field = 0;
  for (;;) {
    CsvField f;
    if (p < end && *p == '"') {
      ++p;
      const char* contentBegin = p;
      for (;;) {
        const void* q = std::memchr(p, '"', static_cast<std::size_t>(end - p));
        if (!q) return false;  // unterminated quote (or embedded newline)
        p = static_cast<const char*>(q);
        if (p + 1 < end && p[1] == '"') {
          f.has_escapes = true;
          p += 2;
          continue;
        }
        break;
      }
      f.text = std::string_view(contentBegin, static_cast<std::size_t>(p - contentBegin));
      ++p;  // closing quote
      while (p < end && (*p == ' ' || *p == '\t')) ++p;
      if (p < end && *p != ',') return false;  // junk after closing quote
    } else {
      const void* c = std::memchr(p, ',', static_cast<std::size_t>(end - p));
      const char* fe = c ? static_cast<const char*>(c) : end;
      f.text = std::string_view(p, static_cast<std::size_t>(fe - p));
      p = fe;
    }
    if (field < static_cast<int>(kMaxCsvFields)) {
      const int col = schema.column_of_field[static_cast<std::size_t>(field)];
      if (col >= 0) out[static_cast<std::size_t>(col)] = f;
    }
    ++field;
    if (p >= end) break;
    ++p;  // skip ','
    if (p == end) {
      // trailing comma => one more empty field
      if (field < static_cast<int>(kMaxCsvFields)) {
        const int col = schema.column_of_field[static_cast<std::size_t>(field)];
        if (col >= 0) out[static_cast<std::size_t>(col)] = CsvField{};
      }
      ++field;
      break;
    }
  }
  return field >= schema.required_fields;
}

std::size_t csv_unescape(std::string_view in, char* out, std::size_t cap) noexcept {
  std::size_t n = 0;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (n >= cap) return static_cast<std::size_t>(-1);
    out[n++] = in[i];
    if (in[i] == '"' && i + 1 < in.size() && in[i + 1] == '"') ++i;
  }
  return n;
}

}  // namespace ftv
