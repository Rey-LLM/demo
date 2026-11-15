#pragma once
// Minimal JSON DOM parser for configuration files (hubs.json, render.json).
// Startup-only code: allocation is fine here, nothing in this file runs on a
// hot path. Supports the full JSON grammar except \u escapes outside the BMP
// surrogate handling (non-ASCII escapes are encoded as UTF-8).

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftv {

class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  JsonValue() = default;
  static JsonValue make_bool(bool b);
  static JsonValue make_number(double d);
  static JsonValue make_string(std::string s);
  static JsonValue make_array();
  static JsonValue make_object();

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
  [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
  [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
  [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
  [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

  [[nodiscard]] bool as_bool(bool fallback = false) const noexcept { return is_bool() ? bool_ : fallback; }
  [[nodiscard]] double as_number(double fallback = 0.0) const noexcept { return is_number() ? number_ : fallback; }
  [[nodiscard]] const std::string& as_string() const noexcept { return string_; }
  [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return array_; }
  [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept { return object_; }

  // Object lookup; returns nullptr when absent or not an object.
  [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
  [[nodiscard]] double number_or(std::string_view key, double fallback) const noexcept;
  [[nodiscard]] bool bool_or(std::string_view key, bool fallback) const noexcept;
  [[nodiscard]] std::string string_or(std::string_view key, std::string_view fallback) const;

  std::vector<JsonValue>& mutable_items() noexcept { return array_; }
  std::vector<std::pair<std::string, JsonValue>>& mutable_members() noexcept { return object_; }

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<JsonValue> array_;
  std::vector<std::pair<std::string, JsonValue>> object_;
};

// Parses text into a DOM. On failure returns nullopt and sets `error` to a
// message containing the line and column.
std::optional<JsonValue> parse_json(std::string_view text, std::string& error);

}  // namespace ftv
