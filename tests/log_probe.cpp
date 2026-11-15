#include <cstdio>

#include "cabin/log.hpp"
#include "cabin/trace.hpp"

int main() {
  cabin::set_current_trace_id(cabin::new_trace_id());
  cabin::ScopedSpan span("log_probe", "emit");
  cabin::log_info("log_probe", "sample",
                  {cabin::Field("driver_id", "driver-a"), cabin::Field("latency_us", 42),
                   cabin::Field("note", "quotes \" and slash \\")});
  std::puts("{\"probe\":\"ok\"}");
  return 0;
}
