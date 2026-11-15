#include "core/json.hpp"

#include <charconv>
#include <cstdio>

namespace ftv {

JsonValue JsonValue::make_bool(bool b) {
  JsonValue v;
  v.type_ = Type::Bool;
  v.bool_ = b;
  return v;
}
JsonValue JsonValue::make_number(double d) {
  JsonValue v;
  v.type_ = Type::Number;
  v.number_ = d;
  return v;
}
JsonValue JsonValue::make_string(std::string s) {
  JsonValue v;
  v.type_ = Type::String;
  v.string_ = std::move(s);
  return v;
}
JsonValue JsonValue::make_array() {
  JsonValue v;
  v.type_ = Type::Array;
  return v;
}
JsonValue JsonValue::make_object() {
  JsonValue v;
  v.type_ = Type::Object;
  return v;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  if (!is_object()) return nullptr;
  for (const auto& kv : object_) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

double JsonValue::number_or(std::string_view key, double fallback) const noexcept {
  const JsonValue* v = find(key);
  return v && v->is_number() ? v->number_ : fallback;
}

bool JsonValue::bool_or(std::string_view key, bool fallback) const noexcept {
  const JsonValue* v = find(key);
  return v && v->is_bool() ? v->bool_ : fallback;
}

std::string JsonValue::string_or(std::string_view key, std::string_view fallback) const {
  const JsonValue* v = find(key);
  return v && v->is_string() ? v->string_ : std::string(fallback);
}

namespace {

class Parser {
 public:
  explicit Parser(std::string_view t) : text_(t) {}

  std::optional<JsonValue> run(std::string& error) {
    JsonValue v;
    skip_ws();
    if (!parse_value(v, 0)) {
      error = error_message();
      return std::nullopt;
    }
    skip_ws();
    if (pos_ != text_.size()) {
      fail("trailing characters after JSON document");
      error = error_message();
      return std::nullopt;
    }
    return v;
  }

 private:
  static constexpr int kMaxDepth = 128;

  bool fail(const char* msg) {
    if (!err_) err_ = msg;
    return false;
  }

  std::string error_message() const {
    std::size_t line = 1, col = 1;
    for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
      if (text_[i] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "JSON parse error at line %zu, column %zu: %s", line, col,
                  err_ ? err_ : "unknown error");
    return buf;
  }

  void skip_ws() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
        // Tolerate // line comments in config files.
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
      } else {
        break;
      }
    }
  }

  bool literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return fail("invalid literal");
    pos_ += word.size();
    return true;
  }

  bool parse_value(JsonValue& out, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    if (pos_ >= text_.size()) return fail("unexpected end of input");
    const char c = text_[pos_];
    switch (c) {
      case '{':
        return parse_object(out, depth);
      case '[':
        return parse_array(out, depth);
      case '"': {
        std::string s;
        if (!parse_string(s)) return false;
        out = JsonValue::make_string(std::move(s));
        return true;
      }
      case 't':
        if (!literal("true")) return false;
        out = JsonValue::make_bool(true);
        return true;
      case 'f':
        if (!literal("false")) return false;
        out = JsonValue::make_bool(false);
        return true;
      case 'n':
        if (!literal("null")) return false;
        out = JsonValue();
        return true;
      default:
        return parse_number(out);
    }
  }

  bool parse_number(JsonValue& out) {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
        ++pos_;
      } else {
        break;
      }
    }
    if (start == pos_) return fail("unexpected character");
    std::string_view tok = text_.substr(start, pos_ - start);
    if (!tok.empty() && tok.front() == '+') tok.remove_prefix(1);
    double d = 0.0;
    const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), d);
    if (res.ec != std::errc() || res.ptr != tok.data() + tok.size()) return fail("invalid number");
    out = JsonValue::make_number(d);
    return true;
  }

  static void append_utf8(std::string& s, unsigned cp) {
    if (cp < 0x80) {
      s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_hex4(unsigned& cp) {
    if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = text_[pos_++];
      cp <<= 4;
      if (h >= '0' && h <= '9') {
        cp |= static_cast<unsigned>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        cp |= static_cast<unsigned>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        cp |= static_cast<unsigned>(h - 'A' + 10);
      } else {
        return fail("invalid hex digit in \\u escape");
      }
    }
    return true;
  }

  bool parse_string(std::string& out) {
    ++pos_;  // opening quote
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) break;
      const char e = text_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned cp = 0;
          if (!parse_hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() && text_[pos_] == '\\' &&
              text_[pos_ + 1] == 'u') {
            pos_ += 2;
            unsigned lo = 0;
            if (!parse_hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          append_utf8(out, cp);
          break;
        }
        default:
          return fail("invalid escape sequence");
      }
    }
    return fail("unterminated string");
  }

  bool parse_array(JsonValue& out, int depth) {
    ++pos_;
    out = JsonValue::make_array();
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      JsonValue item;
      skip_ws();
      if (!parse_value(item, depth + 1)) return false;
      out.mutable_items().push_back(std::move(item));
      skip_ws();
      if (pos_ >= text_.size()) return fail("unterminated array");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parse_object(JsonValue& out, int depth) {
    ++pos_;
    out = JsonValue::make_object();
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      skip_ws();
      if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected object key");
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
      ++pos_;
      skip_ws();
      JsonValue val;
      if (!parse_value(val, depth + 1)) return false;
      out.mutable_members().emplace_back(std::move(key), std::move(val));
      skip_ws();
      if (pos_ >= text_.size()) return fail("unterminated object");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  const char* err_ = nullptr;
};

}  // namespace

std::optional<JsonValue> parse_json(std::string_view text, std::string& error) {
  Parser p(text);
  return p.run(error);
}

}  // namespace ftv
