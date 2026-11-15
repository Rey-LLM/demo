#pragma once
// RFC-4180-style CSV tokenisation over string_views into the mapped file.
// Supports quoted fields with embedded commas and "" escapes, CRLF line
// endings, empty fields and arbitrary column order / extra columns. Quoted
// fields containing raw newlines are not supported (chunks are split on '\n');
// such rows are reported as malformed instead of corrupting neighbours.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ftv {

enum class CsvColumn : std::uint8_t {
  TxId = 0,
  Timestamp,
  SrcAccount,
  DstAccount,
  Amount,
  Currency,
  Channel,
  SrcJurisdiction,
  DstJurisdiction,
  Chain,
  Count
};
inline constexpr std::size_t kCsvColumnCount = static_cast<std::size_t>(CsvColumn::Count);
inline constexpr std::size_t kMaxCsvFields = 64;

struct CsvSchema {
  // Physical field index for each logical column.
  std::array<int, kCsvColumnCount> field_of_column{};
  // Logical column for each physical field index, or -1 if ignored.
  std::array<std::int8_t, kMaxCsvFields> column_of_field{};
  int required_fields = 0;  // max physical index + 1
};

struct CsvField {
  std::string_view text;    // content without surrounding quotes
  bool has_escapes = false; // contains "" pairs that need collapsing
};

using CsvRowFields = std::array<CsvField, kCsvColumnCount>;

// Parses the header line (without the trailing newline). Skips a UTF-8 BOM.
bool parse_csv_header(std::string_view headerLine, CsvSchema& schema, std::string& error);

// Returns the byte offset just past the header line (first '\n' + 1).
std::size_t csv_header_length(std::string_view data) noexcept;

// Splits one record (no trailing '\n'; a trailing '\r' is stripped). Returns
// false for structurally malformed records.
bool split_csv_line(std::string_view line, const CsvSchema& schema, CsvRowFields& out) noexcept;

// Collapses "" escapes from `in` into `out` (capacity `cap`). Returns the
// written length, or SIZE_MAX if it does not fit.
std::size_t csv_unescape(std::string_view in, char* out, std::size_t cap) noexcept;

}  // namespace ftv
