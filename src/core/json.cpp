#include "cabin/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace cabin {
namespace {

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  Result<JsonValue> parse() {
    skip_whitespace();
    auto value = parse_value(0);
    if (!value)
      return value;
    skip_whitespace();
    if (position_ != text_.size()) {
      return error("trailing characters");
    }
    return value;
  }

 private:
  Error error(const std::string& message) const {
    return make_error(ErrorCode::kParse,
                      "json at byte " + std::to_string(position_) + ": " + message);
  }

  void skip_whitespace() {
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) {
    if (position_ < text_.size() && text_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  Result<JsonValue> parse_value(int depth) {
    if (depth > 64)
      return error("nesting too deep");
    skip_whitespace();
    if (position_ >= text_.size())
      return error("unexpected end of input");
    const char c = text_[position_];
    switch (c) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"': {
        auto text = parse_string();
        if (!text)
          return text.error();
        return JsonValue(std::move(text.value()));
      }
      case 't':
        if (text_.substr(position_, 4) == "true") {
          position_ += 4;
          return JsonValue(true);
        }
        return error("invalid literal");
      case 'f':
        if (text_.substr(position_, 5) == "false") {
          position_ += 5;
          return JsonValue(false);
        }
        return error("invalid literal");
      case 'n':
        if (text_.substr(position_, 4) == "null") {
          position_ += 4;
          return JsonValue();
        }
        return error("invalid literal");
      default:
        if (c == '-' || (c >= '0' && c <= '9'))
          return parse_number();
        return error("unexpected character");
    }
  }

  Result<JsonValue> parse_object(int depth) {
    consume('{');
    JsonValue object = JsonValue::object();
    skip_whitespace();
    if (consume('}'))
      return object;
    while (true) {
      skip_whitespace();
      if (position_ >= text_.size() || text_[position_] != '"') {
        return error("object key must be a string");
      }
      auto key = parse_string();
      if (!key)
        return key.error();
      skip_whitespace();
      if (!consume(':'))
        return error("expected ':'");
      auto value = parse_value(depth + 1);
      if (!value)
        return value;
      object.set(std::move(key.value()), std::move(value.value()));
      skip_whitespace();
      if (consume(','))
        continue;
      if (consume('}'))
        break;
      return error("expected ',' or '}'");
    }
    return object;
  }

  Result<JsonValue> parse_array(int depth) {
    consume('[');
    JsonValue array = JsonValue::array();
    skip_whitespace();
    if (consume(']'))
      return array;
    while (true) {
      auto value = parse_value(depth + 1);
      if (!value)
        return value;
      array.push_back(std::move(value.value()));
      skip_whitespace();
      if (consume(','))
        continue;
      if (consume(']'))
        break;
      return error("expected ',' or ']'");
    }
    return array;
  }

  static void append_utf8(std::string* out, unsigned codepoint) {
    if (codepoint <= 0x7F) {
      out->push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
      out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
      out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
  }

  Result<std::string> parse_string() {
    if (!consume('"'))
      return error("expected string");
    std::string out;
    while (position_ < text_.size()) {
      const char c = text_[position_++];
      if (c == '"')
        return out;
      if (c == '\\') {
        if (position_ >= text_.size())
          return error("truncated escape");
        const char escape = text_[position_++];
        switch (escape) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            if (position_ + 4 > text_.size())
              return error("truncated \\u escape");
            unsigned codepoint = 0;
            for (int index = 0; index < 4; ++index) {
              const char digit = text_[position_++];
              codepoint <<= 4;
              if (digit >= '0' && digit <= '9')
                codepoint |= static_cast<unsigned>(digit - '0');
              else if (digit >= 'a' && digit <= 'f')
                codepoint |= static_cast<unsigned>(digit - 'a' + 10);
              else if (digit >= 'A' && digit <= 'F')
                codepoint |= static_cast<unsigned>(digit - 'A' + 10);
              else
                return error("invalid \\u escape");
            }
            append_utf8(&out, codepoint);
            break;
          }
          default:
            return error("invalid escape");
        }
        continue;
      }
      if (static_cast<unsigned char>(c) < 0x20)
        return error("control character");
      out.push_back(c);
    }
    return error("unterminated string");
  }

  Result<JsonValue> parse_number() {
    const std::size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-')
      ++position_;
    while (position_ < text_.size() && text_[position_] >= '0' &&
           text_[position_] <= '9') {
      ++position_;
    }
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      while (position_ < text_.size() && text_[position_] >= '0' &&
             text_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < text_.size() &&
        (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() &&
          (text_[position_] == '+' || text_[position_] == '-')) {
        ++position_;
      }
      while (position_ < text_.size() && text_[position_] >= '0' &&
             text_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ == start || (position_ == start + 1 && text_[start] == '-')) {
      return error("invalid number");
    }
    const std::string text(text_.substr(start, position_ - start));
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
      return error("invalid number");
    }
    return JsonValue(value);
  }

  std::string_view text_;
  std::size_t position_{0};
};

void dump_string(std::string* out, std::string_view text) {
  out->push_back('"');
  for (char c : text) {
    switch (c) {
      case '"':
        *out += "\\\"";
        break;
      case '\\':
        *out += "\\\\";
        break;
      case '\b':
        *out += "\\b";
        break;
      case '\f':
        *out += "\\f";
        break;
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
          *out += buffer;
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

void indent_to(std::string* out, int indent, int depth) {
  if (indent <= 0)
    return;
  out->push_back('\n');
  out->append(static_cast<std::size_t>(indent * depth), ' ');
}

} // namespace

std::string json_escape(std::string_view text) {
  std::string out;
  dump_string(&out, text);
  return out;
}

void JsonValue::set(std::string key, JsonValue value) {
  if (!is_object()) {
    type_ = Type::kObject;
    keys_.clear();
    values_.clear();
  }
  for (std::size_t index = 0; index < keys_.size(); ++index) {
    if (keys_[index] == key) {
      values_[index] = std::move(value);
      return;
    }
  }
  keys_.push_back(std::move(key));
  values_.push_back(std::move(value));
}

void JsonValue::push_back(JsonValue value) {
  if (!is_array()) {
    type_ = Type::kArray;
    array_.clear();
  }
  array_.push_back(std::move(value));
}

const JsonValue* JsonValue::find(std::string_view key) const {
  if (!is_object())
    return nullptr;
  for (std::size_t index = 0; index < keys_.size(); ++index) {
    if (keys_[index] == key)
      return &values_[index];
  }
  return nullptr;
}

JsonValue* JsonValue::find(std::string_view key) {
  if (!is_object())
    return nullptr;
  for (std::size_t index = 0; index < keys_.size(); ++index) {
    if (keys_[index] == key)
      return &values_[index];
  }
  return nullptr;
}

std::string JsonValue::get_string(std::string_view key, std::string fallback) const {
  const JsonValue* value = find(key);
  if (value == nullptr || !value->is_string())
    return fallback;
  return value->string_value();
}

double JsonValue::get_number(std::string_view key, double fallback) const {
  const JsonValue* value = find(key);
  if (value == nullptr || !value->is_number())
    return fallback;
  return value->number_value();
}

std::int64_t JsonValue::get_int(std::string_view key, std::int64_t fallback) const {
  const JsonValue* value = find(key);
  if (value == nullptr || !value->is_number())
    return fallback;
  return static_cast<std::int64_t>(value->number_value());
}

bool JsonValue::get_bool(std::string_view key, bool fallback) const {
  const JsonValue* value = find(key);
  if (value == nullptr || !value->is_bool())
    return fallback;
  return value->bool_value();
}

std::string JsonValue::dump(int indent) const {
  std::string out;
  dump_to(&out, indent, 0);
  return out;
}

void JsonValue::dump_to(std::string* out, int indent, int depth) const {
  switch (type_) {
    case Type::kNull:
      *out += "null";
      break;
    case Type::kBool:
      *out += bool_ ? "true" : "false";
      break;
    case Type::kNumber: {
      if (!std::isfinite(number_)) {
        *out += "null";
      } else if (number_ == std::floor(number_) && std::fabs(number_) < 9.0e15) {
        *out += std::to_string(static_cast<long long>(number_));
      } else {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", number_);
        *out += buffer;
      }
      break;
    }
    case Type::kString:
      dump_string(out, string_);
      break;
    case Type::kArray: {
      if (array_.empty()) {
        *out += "[]";
        break;
      }
      *out += '[';
      for (std::size_t index = 0; index < array_.size(); ++index) {
        if (index != 0)
          *out += ',';
        indent_to(out, indent, depth + 1);
        array_[index].dump_to(out, indent, depth + 1);
      }
      indent_to(out, indent, depth);
      *out += ']';
      break;
    }
    case Type::kObject: {
      if (keys_.empty()) {
        *out += "{}";
        break;
      }
      *out += '{';
      for (std::size_t index = 0; index < keys_.size(); ++index) {
        if (index != 0)
          *out += ',';
        indent_to(out, indent, depth + 1);
        dump_string(out, keys_[index]);
        *out += indent > 0 ? ": " : ":";
        values_[index].dump_to(out, indent, depth + 1);
      }
      indent_to(out, indent, depth);
      *out += '}';
      break;
    }
  }
}

Result<JsonValue> parse_json(std::string_view text) {
  JsonParser parser(text);
  return parser.parse();
}

} // namespace cabin
