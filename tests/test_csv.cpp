// CSV tokenisation and field parsing edge cases.

#include <string>

#include "ingest/csv_parser.hpp"
#include "ingest/field_parsers.hpp"
#include "ingest/types.hpp"
#include "test_framework.hpp"

using namespace ftv;

namespace {
CsvSchema standard_schema() {
  CsvSchema s;
  std::string err;
  parse_csv_header(
      "tx_id,timestamp_utc,src_account,dst_account,amount,currency,channel,src_jurisdiction,dst_jurisdiction,chain", s,
      err);
  return s;
}
std::string_view col(const CsvRowFields& f, CsvColumn c) { return f[static_cast<std::size_t>(c)].text; }
}  // namespace

TEST_CASE("csv: header with reordered and extra columns, BOM and CRLF") {
  CsvSchema s;
  std::string err;
  const std::string header =
      "\xEF\xBB\xBF" "extra,CHAIN,Channel,amount,currency,src_account,dst_account,timestamp_utc,tx_id,"
      "src_jurisdiction,dst_jurisdiction\r";
  REQUIRE(parse_csv_header(header, s, err));
  CHECK_EQ(s.field_of_column[static_cast<std::size_t>(CsvColumn::Chain)], 1);
  CHECK_EQ(s.field_of_column[static_cast<std::size_t>(CsvColumn::TxId)], 8);
  CHECK_EQ(s.required_fields, 11);
  CsvRowFields f{};
  REQUIRE(split_csv_line("x,ETH,ONCHAIN,1.5,ETH,a,b,2024-01-01T00:00:00Z,T1,,", s, f));
  CHECK(col(f, CsvColumn::Chain) == "ETH");
  CHECK(col(f, CsvColumn::SrcAccount) == "a");
  CHECK(col(f, CsvColumn::DstJurisdiction).empty());
}

TEST_CASE("csv: missing required column is a fatal header error") {
  CsvSchema s;
  std::string err;
  CHECK(!parse_csv_header("tx_id,timestamp_utc,src_account,dst_account,amount,currency,channel", s, err));
  CHECK(err.find("src_jurisdiction") != std::string::npos);
}

TEST_CASE("csv: quoted fields with commas and escaped quotes") {
  const CsvSchema s = standard_schema();
  CsvRowFields f{};
  REQUIRE(split_csv_line("T1,2024-01-01T00:00:00Z,\"ACME, Inc \"\"EU\"\"\",B,10.00,EUR,SEPA,DE,FR,", s, f));
  const CsvField& src = f[static_cast<std::size_t>(CsvColumn::SrcAccount)];
  CHECK(src.has_escapes);
  CHECK(src.text == "ACME, Inc \"\"EU\"\"");
  char buf[64];
  const std::size_t n = csv_unescape(src.text, buf, sizeof(buf));
  CHECK(std::string_view(buf, n) == "ACME, Inc \"EU\"");
  CHECK(col(f, CsvColumn::DstAccount) == "B");
  CHECK(col(f, CsvColumn::Chain).empty());
}

TEST_CASE("csv: CRLF line endings and empty fields") {
  const CsvSchema s = standard_schema();
  CsvRowFields f{};
  REQUIRE(split_csv_line("T1,2024-01-01T00:00:00Z,A,B,1,USD,ACH,,,\r", s, f));
  CHECK(col(f, CsvColumn::SrcJurisdiction).empty());
  CHECK(col(f, CsvColumn::Chain).empty());
  CHECK(col(f, CsvColumn::Channel) == "ACH");
  // Trailing comma produces a final empty field.
  REQUIRE(split_csv_line("T1,2024-01-01T00:00:00Z,A,B,1,USD,ACH,US,US,", s, f));
  CHECK(col(f, CsvColumn::Chain).empty());
}

TEST_CASE("csv: structurally malformed rows are rejected") {
  const CsvSchema s = standard_schema();
  CsvRowFields f{};
  CHECK(!split_csv_line("T1,2024-01-01T00:00:00Z,A", s, f));                       // too few fields
  CHECK(!split_csv_line("T1,2024-01-01T00:00:00Z,\"open,B,1,USD,ACH,US,US,", s, f));  // unterminated quote
  CHECK(!split_csv_line("T1,2024-01-01T00:00:00Z,\"A\"x,B,1,USD,ACH,US,US,", s, f));  // junk after quote
}

TEST_CASE("fields: fixed-point amounts without floating point") {
  std::int64_t v = 0;
  bool trunc = false;
  CHECK(parse_amount_minor("1234.56", 2, v, &trunc) && v == 123456 && !trunc);
  CHECK(parse_amount_minor("-0.5", 2, v) && v == -50);
  CHECK(parse_amount_minor("+7", 2, v) && v == 700);
  CHECK(parse_amount_minor("12.", 2, v) && v == 1200);
  CHECK(parse_amount_minor(".25", 2, v) && v == 25);
  CHECK(parse_amount_minor("12.345", 2, v, &trunc) && v == 1234 && trunc);  // truncated toward zero
  CHECK(parse_amount_minor("1500", 0, v) && v == 1500);                       // JPY
  CHECK(parse_amount_minor("0.00000001", 8, v) && v == 1);                    // 1 satoshi
  CHECK(parse_amount_minor("  42.10 ", 2, v) && v == 4210);
  CHECK(!parse_amount_minor("", 2, v));
  CHECK(!parse_amount_minor("1e5", 2, v));
  CHECK(!parse_amount_minor("1,000.00", 2, v));
  CHECK(!parse_amount_minor("abc", 2, v));
  CHECK(!parse_amount_minor("-", 2, v));
  CHECK(!parse_amount_minor("99999999999999999999", 2, v));  // overflow
  CHECK(parse_amount_minor("92233720368547758.07", 2, v) && v == INT64_MAX);
  CHECK(!parse_amount_minor("92233720368547758.08", 2, v));
}

TEST_CASE("fields: timestamps (ISO-8601 and epoch ms)") {
  std::int64_t t = 0;
  CHECK(parse_timestamp_ms("1970-01-01T00:00:00Z", t) && t == 0);
  CHECK(parse_timestamp_ms("2024-03-15T12:34:56.789Z", t) && t == 1710506096789LL);
  CHECK(parse_timestamp_ms("2024-03-15 12:34:56", t) && t == 1710506096000LL);
  CHECK(parse_timestamp_ms("2024-03-15T14:34:56+02:00", t) && t == 1710506096000LL);
  CHECK(parse_timestamp_ms("2024-03-15", t) && t == 1710460800000LL);
  CHECK(parse_timestamp_ms("1710506096789", t) && t == 1710506096789LL);
  CHECK(parse_timestamp_ms("2024-02-29T00:00:00Z", t));
  CHECK(parse_timestamp_ms("2000-02-29", t) && t == 951782400000LL);  // divisible by 400: leap
  CHECK(parse_timestamp_ms("2024-12-31T23:59:59Z", t) && t == 1735689599000LL);
  // Impossible calendar dates are malformed, never rolled into the next month.
  CHECK(!parse_timestamp_ms("2024-02-31T00:00:00Z", t));
  CHECK(!parse_timestamp_ms("2023-02-29T00:00:00Z", t));  // not a leap year
  CHECK(!parse_timestamp_ms("1900-02-29", t));            // century, not divisible by 400
  CHECK(!parse_timestamp_ms("2024-04-31", t));
  CHECK(!parse_timestamp_ms("2024-01-00", t));
  CHECK(!parse_timestamp_ms("2024-13-01T00:00:00Z", t));
  CHECK(!parse_timestamp_ms("2024-01-01T25:00:00Z", t));
  CHECK(!parse_timestamp_ms("yesterday", t));
  CHECK(!parse_timestamp_ms("", t));
}

TEST_CASE("fields: channels and packed codes") {
  Channel c{};
  CHECK(parse_channel("SWIFT", c) && c == Channel::SWIFT);
  CHECK(parse_channel("onchain", c) && c == Channel::ONCHAIN);
  CHECK(parse_channel(" ZENGIN ", c) && c == Channel::ZENGIN);
  CHECK(!parse_channel("TELEPORT", c));
  CHECK(!parse_channel("", c));
  CHECK_EQ(pack_code4("usd"), pack_code4("USD"));
  CHECK_EQ(pack_code4("USDT"), 0x54445355u);
  CHECK_EQ(pack_code4("TOOLONG"), 0u);
  CHECK_EQ(currency_scale(pack_code4("JPY")), 0);
  CHECK_EQ(currency_scale(pack_code4("BTC")), 8);
  CHECK_EQ(currency_scale(pack_code4("EUR")), 2);
}
