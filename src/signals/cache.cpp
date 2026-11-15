#include "cabin/signals/cache.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace cabin::signals {
namespace {

std::size_t index_of(SignalId id) {
  return static_cast<std::size_t>(id);
}

template <typename T>
T rounded(double value) {
  return static_cast<T>(std::llround(value));
}

} // namespace

SignalCache::SignalCache(std::uint64_t max_age_us) : max_age_us_(max_age_us) {}

bool SignalCache::update(SignalId id, double value, std::uint64_t timestamp_us) {
  if (index_of(id) >= entries_.size())
    return false;
  Entry& entry = entries_[index_of(id)];
  entry.state.value = value;
  entry.state.timestamp_us = timestamp_us;
  entry.state.seen = true;
  if (timestamp_us > last_timestamp_us_)
    last_timestamp_us_ = timestamp_us;

  const bool changed =
      !entry.published || std::fabs(value - entry.published_value) > 1e-9;
  const bool refresh =
      !entry.published || timestamp_us - entry.published_timestamp_us >= max_age_us_;
  if (!changed && !refresh)
    return false;
  entry.published = true;
  entry.published_value = value;
  entry.published_timestamp_us = timestamp_us;
  return true;
}

bool SignalCache::mark_published(SignalId id, std::uint64_t timestamp_us) {
  if (index_of(id) >= entries_.size())
    return false;
  Entry& entry = entries_[index_of(id)];
  entry.published = entry.state.seen;
  entry.published_value = entry.state.value;
  entry.published_timestamp_us = timestamp_us;
  return entry.state.seen;
}

std::optional<CachedSignal> SignalCache::get(SignalId id) const {
  if (index_of(id) >= entries_.size())
    return std::nullopt;
  const Entry& entry = entries_[index_of(id)];
  if (!entry.state.seen)
    return std::nullopt;
  return entry.state;
}

bool SignalCache::is_stale(SignalId id, std::uint64_t now_us) const {
  if (index_of(id) >= entries_.size())
    return true;
  const Entry& entry = entries_[index_of(id)];
  if (!entry.state.seen)
    return true;
  return now_us > entry.state.timestamp_us &&
         now_us - entry.state.timestamp_us > max_age_us_;
}

std::vector<SignalId> SignalCache::stale_signals(std::uint64_t now_us) const {
  std::vector<SignalId> stale;
  for (std::size_t index = 0; index < kSignalCount; ++index) {
    const auto id = static_cast<SignalId>(index);
    if (is_stale(id, now_us))
      stale.push_back(id);
  }
  return stale;
}

std::size_t SignalCache::seen_count() const {
  std::size_t count = 0;
  for (const auto& entry : entries_) {
    if (entry.state.seen)
      ++count;
  }
  return count;
}

gen::SignalSnapshot SignalCache::snapshot(std::uint64_t now_us) const {
  gen::SignalSnapshot snapshot;
  const auto value = [this](SignalId id, double fallback) {
    const auto entry = get(id);
    return entry.has_value() ? entry->value : fallback;
  };
  snapshot.seat_position_mm =
      rounded<std::uint16_t>(value(SignalId::kSeatPositionMm, 0.0));
  snapshot.seat_recline_deg =
      rounded<std::uint8_t>(value(SignalId::kSeatReclineDeg, 0.0));
  snapshot.hvac_setpoint_c = static_cast<float>(value(SignalId::kHvacSetpointC, 16.0));
  snapshot.hvac_fan_level = rounded<std::uint8_t>(value(SignalId::kHvacFanLevel, 0.0));
  snapshot.cabin_light_level =
      rounded<std::uint8_t>(value(SignalId::kCabinLightLevel, 0.0));
  snapshot.cabin_light_color = static_cast<gen::LightColor>(
      rounded<std::uint8_t>(value(SignalId::kCabinLightColor, 0.0)));
  snapshot.door_state = static_cast<gen::DoorState>(
      rounded<std::uint8_t>(value(SignalId::kDoorState, 0.0)));
  snapshot.ignition_state = static_cast<gen::IgnitionState>(
      rounded<std::uint8_t>(value(SignalId::kIgnitionState, 0.0)));
  snapshot.timestamp_us = now_us;
  return snapshot;
}

std::string SignalCache::summary(std::uint64_t now_us) const {
  std::ostringstream out;
  out << "cabin signal summary: " << seen_count() << "/" << kSignalCount
      << " signals seen at t=" << now_us << "us\n";
  for (std::size_t index = 0; index < kSignalCount; ++index) {
    const auto id = static_cast<SignalId>(index);
    const SignalMeta& meta = signal_meta(id);
    const auto entry = get(id);
    out << "  " << meta.name << " (" << meta.cpp_type << ") = ";
    if (!entry.has_value()) {
      out << "<never seen>";
    } else {
      out.setf(std::ios::fixed);
      out.precision(meta.can.encoding == SignalEncoding::kFloat32 ? 2 : 0);
      out << entry->value << " " << meta.unit;
      if (meta.is_enum) {
        out << " (" << signal_value_name(id, static_cast<int>(std::llround(entry->value)))
            << ")";
      }
    }
    if (is_stale(id, now_us))
      out << " [STALE]";
    out << "\n";
  }
  return out.str();
}

SignalProcessor::SignalProcessor(std::uint64_t max_age_us) : cache_(max_age_us) {}

std::vector<SignalId> SignalProcessor::process(const CanFrame& frame,
                                               std::uint64_t timestamp_us) {
  std::vector<SignalId> changed;
  CanFrame stamped = frame;
  if (stamped.timestamp_us == 0)
    stamped.timestamp_us = timestamp_us;
  auto decoded = codec_.decode(stamped);
  if (!decoded)
    return changed;
  for (const DecodedSignal& signal : decoded.value()) {
    if (cache_.update(signal.id, signal.value, timestamp_us)) {
      changed.push_back(signal.id);
    }
  }
  return changed;
}

gen::SignalSnapshot SignalProcessor::snapshot() const {
  return cache_.snapshot(cache_.last_timestamp_us());
}

std::string SignalProcessor::summary() const {
  return cache_.summary(cache_.last_timestamp_us());
}

} // namespace cabin::signals
