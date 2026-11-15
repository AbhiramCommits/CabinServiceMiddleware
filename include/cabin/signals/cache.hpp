#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cabin/signals/codec.hpp"
#include "cabin_can_map.hpp"
#include "cabin_types.hpp"

namespace cabin::signals {

struct CachedSignal {
  double value{0.0};
  std::uint64_t timestamp_us{0};
  bool seen{false};
};

// Last-known value per signal plus change detection. A signal is published when
// it changes beyond a small epsilon or when max_age has elapsed (a keep-alive
// refresh so consumers can distinguish "unchanged" from "stale").
class SignalCache {
 public:
  explicit SignalCache(std::uint64_t max_age_us = 1000000);

  bool update(SignalId id, double value, std::uint64_t timestamp_us);
  bool mark_published(SignalId id, std::uint64_t timestamp_us);
  std::optional<CachedSignal> get(SignalId id) const;

  bool is_stale(SignalId id, std::uint64_t now_us) const;
  std::vector<SignalId> stale_signals(std::uint64_t now_us) const;
  std::size_t seen_count() const;

  gen::SignalSnapshot snapshot(std::uint64_t now_us) const;
  std::string summary(std::uint64_t now_us) const;

  void set_max_age_us(std::uint64_t max_age_us) { max_age_us_ = max_age_us; }
  std::uint64_t max_age_us() const { return max_age_us_; }
  std::uint64_t last_timestamp_us() const { return last_timestamp_us_; }

 private:
  struct Entry {
    CachedSignal state;
    double published_value{0.0};
    std::uint64_t published_timestamp_us{0};
    bool published{false};
  };

  std::array<Entry, kSignalCount> entries_{};
  std::uint64_t max_age_us_;
  std::uint64_t last_timestamp_us_{0};
};

// Replay/socket pipeline: decode every mapped signal in a frame, update the
// cache and report which signals should be published.
class SignalProcessor {
 public:
  explicit SignalProcessor(std::uint64_t max_age_us = 1000000);
  std::vector<SignalId> process(const CanFrame& frame, std::uint64_t timestamp_us);
  gen::SignalSnapshot snapshot() const;
  std::string summary() const;

  SignalCache& cache() { return cache_; }
  const SignalCache& cache() const { return cache_; }
  const SignalCodec& codec() const { return codec_; }
  std::uint64_t now_us() const { return cache_.last_timestamp_us(); }

 private:
  SignalCodec codec_;
  SignalCache cache_;
};

} // namespace cabin::signals
