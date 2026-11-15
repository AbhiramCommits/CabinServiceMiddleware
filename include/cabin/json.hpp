#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cabin/result.hpp"

namespace cabin {

// Small stdlib-only JSON document used by profile persistence and the gateway.
// Objects preserve insertion order so generated files are stable and readable.
class JsonValue {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  JsonValue() = default;
  explicit JsonValue(bool value) : type_(Type::kBool), bool_(value) {}
  explicit JsonValue(double value) : type_(Type::kNumber), number_(value) {}
  explicit JsonValue(std::string value)
      : type_(Type::kString), string_(std::move(value)) {}
  explicit JsonValue(const char* value) : type_(Type::kString), string_(value) {}

  static JsonValue object() {
    JsonValue value;
    value.type_ = Type::kObject;
    return value;
  }
  static JsonValue array() {
    JsonValue value;
    value.type_ = Type::kArray;
    return value;
  }
  static JsonValue number(double value) { return JsonValue(value); }
  static JsonValue string(std::string value) { return JsonValue(std::move(value)); }
  static JsonValue boolean(bool value) { return JsonValue(value); }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::kNull; }
  bool is_bool() const { return type_ == Type::kBool; }
  bool is_number() const { return type_ == Type::kNumber; }
  bool is_string() const { return type_ == Type::kString; }
  bool is_array() const { return type_ == Type::kArray; }
  bool is_object() const { return type_ == Type::kObject; }

  bool bool_value(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
  double number_value(double fallback = 0.0) const {
    return is_number() ? number_ : fallback;
  }
  std::int64_t int_value(std::int64_t fallback = 0) const {
    return is_number() ? static_cast<std::int64_t>(number_) : fallback;
  }
  const std::string& string_value() const { return string_; }

  std::size_t size() const {
    if (is_array())
      return array_.size();
    if (is_object())
      return keys_.size();
    return 0;
  }
  const JsonValue* at(std::size_t index) const {
    return is_array() && index < array_.size() ? &array_[index] : nullptr;
  }
  JsonValue* at(std::size_t index) {
    return is_array() && index < array_.size() ? &array_[index] : nullptr;
  }
  void push_back(JsonValue value);

  void set(std::string key, JsonValue value);
  const JsonValue* find(std::string_view key) const;
  JsonValue* find(std::string_view key);
  bool has(std::string_view key) const { return find(key) != nullptr; }

  std::string get_string(std::string_view key, std::string fallback = "") const;
  double get_number(std::string_view key, double fallback = 0.0) const;
  std::int64_t get_int(std::string_view key, std::int64_t fallback = 0) const;
  bool get_bool(std::string_view key, bool fallback = false) const;

  std::string dump(int indent = 2) const;
  void dump_to(std::string* out, int indent, int depth) const;

  const std::vector<JsonValue>& array_items() const { return array_; }
  std::size_t object_size() const { return keys_.size(); }
  const std::string& key_at(std::size_t index) const { return keys_[index]; }
  const JsonValue& value_at(std::size_t index) const { return values_[index]; }

 private:
  Type type_{Type::kNull};
  bool bool_{false};
  double number_{0.0};
  std::string string_;
  std::vector<JsonValue> array_;
  std::vector<std::string> keys_;
  std::vector<JsonValue> values_;
};

Result<JsonValue> parse_json(std::string_view text);
std::string json_escape(std::string_view text);

} // namespace cabin
