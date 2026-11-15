#include "cabin/trace.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "cabin/clock.hpp"
#include "cabin/log.hpp"

namespace cabin {
namespace {

std::atomic<std::uint64_t> g_next_trace{1};
std::atomic<std::uint64_t> g_next_span{1};

thread_local TraceId t_trace_id = 0;
thread_local ScopedSpan* t_span = nullptr;
thread_local std::string t_span_id;

} // namespace

TraceId new_trace_id() {
  // Keep the sequence in the low bits and set bit 63 so locally generated ids
  // are visually distinct from ids minted by an upstream caller.
  const std::uint64_t seq = g_next_trace.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t value = (0x8000000000000000ULL) | (seq & 0x7FFFFFFFFFFFFFFFULL);
  if (value == 0)
    value = 1;
  return value;
}

TraceId current_trace_id() {
  return t_trace_id;
}

void set_current_trace_id(TraceId id) {
  t_trace_id = id;
}

void clear_current_trace_id() {
  t_trace_id = 0;
}

std::string trace_id_hex(TraceId id) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
  return std::string(buf);
}

bool parse_trace_id(std::string_view text, TraceId* out) {
  if (out == nullptr || text.empty() || text.size() > 16)
    return false;
  TraceId value = 0;
  for (char c : text) {
    value <<= 4;
    if (c >= '0' && c <= '9')
      value |= static_cast<TraceId>(c - '0');
    else if (c >= 'a' && c <= 'f')
      value |= static_cast<TraceId>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      value |= static_cast<TraceId>(c - 'A' + 10);
    else
      return false;
  }
  *out = value;
  return true;
}

std::string_view current_span_id() {
  if (t_span == nullptr)
    return {};
  return t_span_id;
}

ScopedSpan::ScopedSpan(std::string_view service, std::string_view name)
    : service_(service), name_(name) {
  trace_id_ = current_trace_id();
  if (trace_id_ == 0) {
    trace_id_ = new_trace_id();
    set_current_trace_id(trace_id_);
    installed_trace_ = true;
  }
  parent_ = t_span;
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(
                    g_next_span.fetch_add(1, std::memory_order_relaxed)));
  id_ = buf;
  t_span = this;
  t_span_id = id_;
  begin(service, name);
}

ScopedSpan::ScopedSpan(std::string_view service, std::string_view name, TraceId trace)
    : service_(service), name_(name), trace_id_(trace) {
  previous_trace_ = current_trace_id();
  if (trace_id_ == 0)
    trace_id_ = new_trace_id();
  set_current_trace_id(trace_id_);
  installed_trace_ = true;
  parent_ = t_span;
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(
                    g_next_span.fetch_add(1, std::memory_order_relaxed)));
  id_ = buf;
  t_span = this;
  t_span_id = id_;
  begin(service, name);
}

void ScopedSpan::begin(std::string_view service, std::string_view name) {
  start_us_ = now_us();
  Logger::instance().log(Level::kTrace, service, "span_begin",
                         {Field("span_name", name), Field("span", id_)});
}

ScopedSpan::~ScopedSpan() {
  const std::uint64_t duration = now_us() - start_us_;
  Logger::instance().log(Level::kTrace, service_, "span_end",
                         {Field("span_name", name_), Field("span", id_),
                          Field("duration_us", static_cast<std::int64_t>(duration))});
  t_span = parent_;
  if (parent_ != nullptr) {
    t_span_id = parent_->id_;
  } else {
    t_span_id.clear();
  }
  if (installed_trace_) {
    set_current_trace_id(previous_trace_);
  }
}

std::uint64_t ScopedSpan::elapsed_us() const {
  return now_us() - start_us_;
}

} // namespace cabin
