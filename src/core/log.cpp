#include "cabin/log.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include "cabin/trace.hpp"

namespace cabin {
namespace {

std::mutex g_log_mutex;

void append_escaped(std::string& out, std::string_view text) {
  for (char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
    }
  }
}

std::string quote(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  append_escaped(out, text);
  out.push_back('"');
  return out;
}

} // namespace

const char* level_name(Level level) {
  switch (level) {
    case Level::kTrace:
      return "TRACE";
    case Level::kDebug:
      return "DEBUG";
    case Level::kInfo:
      return "INFO";
    case Level::kWarn:
      return "WARN";
    case Level::kError:
      return "ERROR";
    default:
      return "OFF";
  }
}

bool parse_level(std::string_view text, Level* out) {
  if (text == "trace" || text == "TRACE")
    *out = Level::kTrace;
  else if (text == "debug" || text == "DEBUG")
    *out = Level::kDebug;
  else if (text == "info" || text == "INFO")
    *out = Level::kInfo;
  else if (text == "warn" || text == "WARN")
    *out = Level::kWarn;
  else if (text == "error" || text == "ERROR")
    *out = Level::kError;
  else if (text == "off" || text == "OFF")
    *out = Logger::kOff;
  else
    return false;
  return true;
}

std::string Field::quote(std::string_view text) {
  return cabin::quote(text);
}

Field::Field(std::string key, double value) : key_(std::move(key)) {
  if (std::isnan(value)) {
    value_ = "\"nan\"";
  } else if (std::isinf(value)) {
    value_ = value > 0 ? "\"inf\"" : "\"-inf\"";
  } else {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    value_ = buf;
  }
}

Logger::Logger() {
  if (const char* level_env = std::getenv("CABIN_LOG_LEVEL")) {
    Level parsed{};
    if (parse_level(level_env, &parsed))
      level_ = parsed;
  }
  if (const char* path = std::getenv("CABIN_LOG_FILE"); path != nullptr && *path) {
    std::FILE* f = std::fopen(path, "a");
    if (f != nullptr)
      file_ = f;
  }
}

Logger::~Logger() {
  if (file_ != nullptr) {
    std::fclose(static_cast<std::FILE*>(file_));
    file_ = nullptr;
  }
}

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

Level Logger::level_from_env() {
  if (const char* level_env = std::getenv("CABIN_LOG_LEVEL")) {
    Level parsed{};
    if (parse_level(level_env, &parsed))
      return parsed;
  }
  return Level::kInfo;
}

void Logger::reset_for_testing() {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  test_sink_ = nullptr;
  level_ = Level::kInfo;
}

std::string Logger::format_record(const LogRecord& record) {
  std::string out;
  // One allocation: every key/value is appended to the same buffer.
  out.reserve(256 + record.fields_json.size());
  out += "{\"ts_us\":";
  out += std::to_string(record.ts_us);
  out += ",\"level\":\"";
  out += level_name(record.level);
  out += "\",\"service\":\"";
  append_escaped(out, record.service);
  out += "\",\"trace_id\":\"";
  append_escaped(out, record.trace_id);
  out += "\",\"span\":\"";
  append_escaped(out, record.span);
  out += "\",\"event\":\"";
  append_escaped(out, record.event);
  out += "\"";
  if (!record.fields_json.empty() && record.fields_json != "{}") {
    out += ",\"fields\":";
    out += record.fields_json;
  }
  out += "}\n";
  return out;
}

void Logger::log(Level level, std::string_view service, std::string_view event,
                 std::initializer_list<Field> fields) {
  if (!enabled(level))
    return;
  LogRecord record;
  record.ts_us = now_us();
  record.level = level;
  record.service.assign(service);
  record.event.assign(event);
  record.trace_id = trace_id_hex(current_trace_id());
  record.span = std::string(current_span_id());

  if (fields.size() > 0) {
    std::string json;
    json.reserve(fields.size() * 32);
    json.push_back('{');
    bool first = true;
    for (const Field& field : fields) {
      if (!first)
        json.push_back(',');
      first = false;
      json += quote(field.key());
      json.push_back(':');
      json += field.json_value();
    }
    json.push_back('}');
    record.fields_json = std::move(json);
  }

  std::string line = format_record(record);
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (test_sink_ != nullptr) {
    *test_sink_ += line;
    return;
  }
  if (file_ != nullptr) {
    std::FILE* f = static_cast<std::FILE*>(file_);
    std::fwrite(line.data(), 1, line.size(), f);
    std::fflush(f);
  } else {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
  }
}

void Logger::log_fields(Level level, std::string_view service, std::string_view event,
                        const std::vector<std::pair<std::string, std::string>>& fields) {
  if (!enabled(level))
    return;
  LogRecord record;
  record.ts_us = now_us();
  record.level = level;
  record.service.assign(service);
  record.event.assign(event);
  record.trace_id = trace_id_hex(current_trace_id());
  record.span = std::string(current_span_id());
  std::string json = "{";
  bool first = true;
  for (const auto& field : fields) {
    if (!first)
      json.push_back(',');
    first = false;
    json += quote(field.first);
    json.push_back(':');
    json += field.second;
  }
  json.push_back('}');
  record.fields_json = std::move(json);

  std::string line = format_record(record);
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (test_sink_ != nullptr) {
    *test_sink_ += line;
    return;
  }
  if (file_ != nullptr) {
    std::FILE* f = static_cast<std::FILE*>(file_);
    std::fwrite(line.data(), 1, line.size(), f);
    std::fflush(f);
  } else {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
  }
}

} // namespace cabin
