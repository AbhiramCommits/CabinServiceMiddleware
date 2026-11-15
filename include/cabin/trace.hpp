#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace cabin {

using TraceId = std::uint64_t;

// Non-zero, process-wide unique-enough identifier. The high bit marks locally
// generated ids so ingest tools can distinguish them from upstream ids.
TraceId new_trace_id();

// Thread-local current trace id. Incoming frames set this before dispatch so
// logs from every hop carry the same id.
TraceId current_trace_id();
void set_current_trace_id(TraceId id);
void clear_current_trace_id();

std::string trace_id_hex(TraceId id);
bool parse_trace_id(std::string_view text, TraceId* out);

// Hex span id for the currently active ScopedSpan, or "".
std::string_view current_span_id();

// Logs span_begin on construction and span_end (with duration_us) on
// destruction. Nested spans form a simple stack. If no trace id is active, one
// is generated and installed for the lifetime of the span.
class ScopedSpan {
 public:
  ScopedSpan(std::string_view service, std::string_view name);
  ScopedSpan(std::string_view service, std::string_view name, TraceId trace);
  ~ScopedSpan();

  ScopedSpan(const ScopedSpan&) = delete;
  ScopedSpan& operator=(const ScopedSpan&) = delete;

  TraceId trace_id() const { return trace_id_; }
  std::string_view id() const { return id_; }
  std::uint64_t elapsed_us() const;

 private:
  void begin(std::string_view service, std::string_view name);

  std::string service_;
  std::string name_;
  std::string id_;
  TraceId trace_id_{0};
  TraceId previous_trace_{0};
  bool installed_trace_{false};
  std::uint64_t start_us_{0};
  ScopedSpan* parent_{nullptr};
};

} // namespace cabin
