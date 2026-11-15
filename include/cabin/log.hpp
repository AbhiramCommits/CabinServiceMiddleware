#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cabin/clock.hpp"

namespace cabin {

enum class Level : std::uint8_t { kTrace = 0, kDebug, kInfo, kWarn, kError };

const char* level_name(Level level);
bool parse_level(std::string_view text, Level* out);

// A flat scalar field. Values are serialized as JSON primitives; strings are
// escaped by the logger. Keeping a flat map (rather than nested JSON) keeps
// log lines cheap to emit and cheap for logtool.py to index.
class Field {
 public:
  Field(std::string key, std::string_view value)
      : key_(std::move(key)), value_(quote(value)) {}
  Field(std::string key, const char* value)
      : key_(std::move(key)), value_(quote(value)) {}
  Field(std::string key, bool value)
      : key_(std::move(key)), value_(value ? "true" : "false") {}
  Field(std::string key, std::int64_t value)
      : key_(std::move(key)), value_(std::to_string(value)) {}
  Field(std::string key, int value)
      : key_(std::move(key)), value_(std::to_string(value)) {}
  Field(std::string key, unsigned value)
      : key_(std::move(key)), value_(std::to_string(value)) {}
  Field(std::string key, double value);

  const std::string& key() const { return key_; }
  const std::string& json_value() const { return value_; }

  static std::string quote(std::string_view text);

 private:
  std::string key_;
  std::string value_;
};

struct LogRecord {
  std::uint64_t ts_us{0};
  Level level{Level::kInfo};
  std::string service;
  std::string trace_id;
  std::string span;
  std::string event;
  std::string fields_json;
};

// JSON-lines logger.
//
// * One object per line, keys always in this order: ts_us, level, service,
//   trace_id, span, event, fields.
// * CABIN_LOG_LEVEL filters at runtime (trace|debug|info|warn|error|off).
// * CABIN_LOG_FILE redirects to a file (append); otherwise stderr.
// * A single mutex serializes writes, so records from all threads stay
//   line-atomic. The JSON string is assembled exactly once per record.
class Logger {
 public:
  static Logger& instance();

  ~Logger();

  void set_level(Level level) { level_ = level; }
  Level level() const { return level_; }
  bool enabled(Level level) const {
    return level_ != kOff && static_cast<int>(level) >= static_cast<int>(level_);
  }

  // `service` and `event` are copied into the record; `fields` must already be
  // escaped scalars.
  void log(Level level, std::string_view service, std::string_view event,
           std::initializer_list<Field> fields = {});
  void log_fields(Level level, std::string_view service, std::string_view event,
                  const std::vector<std::pair<std::string, std::string>>& fields);

  // Test hooks: capture output without touching process-wide files.
  void set_sink_for_testing(std::string* sink) { test_sink_ = sink; }
  void reset_for_testing();

  static std::string format_record(const LogRecord& record);
  static Level level_from_env();
  static constexpr Level kOff = static_cast<Level>(255);

 private:
  Logger();

  Level level_{Level::kInfo};
  std::string trace_id_;
  std::string span_;
  std::string buf_;
  std::string* test_sink_{nullptr};
  void* file_{nullptr};
};

inline void log(Level level, std::string_view service, std::string_view event,
                std::initializer_list<Field> fields = {}) {
  Logger::instance().log(level, service, event, fields);
}

inline void log_trace(std::string_view service, std::string_view event,
                      std::initializer_list<Field> fields = {}) {
  log(Level::kTrace, service, event, fields);
}
inline void log_debug(std::string_view service, std::string_view event,
                      std::initializer_list<Field> fields = {}) {
  log(Level::kDebug, service, event, fields);
}
inline void log_info(std::string_view service, std::string_view event,
                     std::initializer_list<Field> fields = {}) {
  log(Level::kInfo, service, event, fields);
}
inline void log_warn(std::string_view service, std::string_view event,
                     std::initializer_list<Field> fields = {}) {
  log(Level::kWarn, service, event, fields);
}
inline void log_error(std::string_view service, std::string_view event,
                      std::initializer_list<Field> fields = {}) {
  log(Level::kError, service, event, fields);
}

} // namespace cabin
