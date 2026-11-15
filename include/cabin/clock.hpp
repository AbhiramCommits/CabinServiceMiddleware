#pragma once

#include <chrono>
#include <cstdint>

namespace cabin {

// Monotonic microseconds. Never wall-clock: latency measurements must not jump
// when NTP or the RTC adjusts the system time.
inline std::uint64_t now_us() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

} // namespace cabin
