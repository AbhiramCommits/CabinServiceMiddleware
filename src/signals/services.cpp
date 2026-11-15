#include "cabin/signals/services.hpp"

#include <cmath>
#include <string>

#include "cabin/clock.hpp"
#include "cabin/log.hpp"

namespace cabin::signals {
namespace {

template <typename T>
void publish_typed(const EventPublisher& publisher, std::uint16_t event_id,
                   const T& value) {
  if (!publisher)
    return;
  auto encoded = gen::encode(value);
  if (!encoded) {
    log_warn("signals", "event_encode_failed",
             {Field("message", encoded.error().message)});
    return;
  }
  publisher(event_id, encoded.value());
}

SignalId id_for(const char* name) {
  const SignalMeta* meta = find_signal(name);
  return meta != nullptr ? meta->id : SignalId::kCount;
}

} // namespace

Status LocalSignalLayer::apply(SignalId id, double value, std::uint64_t now_us) {
  const SignalMeta& meta = signal_meta(id);
  if (meta.id != id)
    return make_error(ErrorCode::kInvalidArgument, "unknown signal");
  if (!std::isfinite(value) || value < meta.min || value > meta.max) {
    return make_error(ErrorCode::kOutOfRange,
                      std::string("value for ") + meta.name + " is out of range");
  }
  cache_.update(id, value, now_us);
  return ok_status();
}

Result<gen::SignalSnapshot> LocalSignalLayer::snapshot(std::uint64_t now_us) {
  return cache_.snapshot(now_us);
}

std::optional<double> LocalSignalLayer::get(SignalId id) {
  auto entry = cache_.get(id);
  if (!entry.has_value())
    return std::nullopt;
  return entry->value;
}

RemoteSignalLayer::RemoteSignalLayer(std::shared_ptr<ServiceClient> client,
                                     int timeout_ms)
    : client_(std::move(client)), proxy_(*client_, timeout_ms) {
  log_debug("signals", "remote_layer_ready",
            {Field("runtime_dir", client_->runtime_dir())});
}

Status RemoteSignalLayer::apply(SignalId id, double value, std::uint64_t now_us) {
  (void)now_us;
  const SignalMeta& meta = signal_meta(id);
  if (meta.id != id)
    return make_error(ErrorCode::kInvalidArgument, "unknown signal");
  auto response = proxy_.set_signal(gen::SignalSetRequest{std::string(meta.name), value});
  if (!response)
    return response.error();
  if (!response.value().accepted) {
    return make_error(ErrorCode::kOutOfRange,
                      std::string("signald rejected ") + meta.name);
  }
  return ok_status();
}

Result<gen::SignalSnapshot> RemoteSignalLayer::snapshot(std::uint64_t now_us) {
  (void)now_us;
  return proxy_.get_signals();
}

std::optional<double> RemoteSignalLayer::get(SignalId id) {
  auto snapshot = proxy_.get_signals();
  if (!snapshot)
    return std::nullopt;
  const auto& value = snapshot.value();
  switch (id) {
    case SignalId::kSeatPositionMm:
      return value.seat_position_mm;
    case SignalId::kSeatReclineDeg:
      return value.seat_recline_deg;
    case SignalId::kHvacSetpointC:
      return value.hvac_setpoint_c;
    case SignalId::kHvacFanLevel:
      return value.hvac_fan_level;
    case SignalId::kCabinLightLevel:
      return value.cabin_light_level;
    case SignalId::kCabinLightColor:
      return static_cast<double>(static_cast<std::uint8_t>(value.cabin_light_color));
    case SignalId::kDoorState:
      return static_cast<double>(static_cast<std::uint8_t>(value.door_state));
    case SignalId::kIgnitionState:
      return static_cast<double>(static_cast<std::uint8_t>(value.ignition_state));
    case SignalId::kCount:
      break;
  }
  return std::nullopt;
}

SeatServiceImpl::SeatServiceImpl(SignalLayer& layer, EventPublisher publisher,
                                 ClockFn clock)
    : layer_(layer), publisher_(std::move(publisher)), clock_(std::move(clock)) {
  if (!clock_)
    clock_ = [] { return now_us(); };
}

Result<gen::SeatSetPositionResponse> SeatServiceImpl::set_position(
    const gen::SeatSetPositionRequest& request) {
  auto valid = gen::validate(request);
  if (!valid)
    return valid.error();
  const std::uint64_t timestamp = clock_();
  auto position =
      layer_.apply(id_for("seat_position_mm"), request.position_mm, timestamp);
  if (!position)
    return position.error();
  auto recline = layer_.apply(id_for("seat_recline_deg"), request.recline_deg, timestamp);
  if (!recline)
    return recline.error();

  gen::SeatState state;
  state.position_mm = request.position_mm;
  state.recline_deg = request.recline_deg;
  state.timestamp_us = timestamp;
  publish_typed(publisher_, gen::kSeatServiceEventSeatStateChanged, state);

  gen::SeatSetPositionResponse response;
  response.accepted = true;
  response.position_mm = request.position_mm;
  response.recline_deg = request.recline_deg;
  return response;
}

Result<gen::SeatState> SeatServiceImpl::get_state() {
  auto snapshot = layer_.snapshot(clock_());
  if (!snapshot)
    return snapshot.error();
  gen::SeatState state;
  state.position_mm = snapshot.value().seat_position_mm;
  state.recline_deg = snapshot.value().seat_recline_deg;
  state.timestamp_us = snapshot.value().timestamp_us;
  return state;
}

HvacServiceImpl::HvacServiceImpl(SignalLayer& layer, EventPublisher publisher,
                                 ClockFn clock)
    : layer_(layer), publisher_(std::move(publisher)), clock_(std::move(clock)) {
  if (!clock_)
    clock_ = [] { return now_us(); };
}

Result<gen::HvacSetResponse> HvacServiceImpl::set_setpoint(
    const gen::HvacSetRequest& request) {
  auto valid = gen::validate(request);
  if (!valid)
    return valid.error();
  const std::uint64_t timestamp = clock_();
  auto setpoint = layer_.apply(id_for("hvac_setpoint_c"), request.setpoint_c, timestamp);
  if (!setpoint)
    return setpoint.error();
  auto fan = layer_.apply(id_for("hvac_fan_level"), request.fan_level, timestamp);
  if (!fan)
    return fan.error();

  gen::HvacState state;
  state.setpoint_c = request.setpoint_c;
  state.fan_level = request.fan_level;
  state.timestamp_us = timestamp;
  publish_typed(publisher_, gen::kHvacServiceEventHvacStateChanged, state);

  gen::HvacSetResponse response;
  response.accepted = true;
  response.setpoint_c = request.setpoint_c;
  response.fan_level = request.fan_level;
  return response;
}

Result<gen::HvacState> HvacServiceImpl::get_state() {
  auto snapshot = layer_.snapshot(clock_());
  if (!snapshot)
    return snapshot.error();
  gen::HvacState state;
  state.setpoint_c = snapshot.value().hvac_setpoint_c;
  state.fan_level = snapshot.value().hvac_fan_level;
  state.timestamp_us = snapshot.value().timestamp_us;
  return state;
}

LightingServiceImpl::LightingServiceImpl(SignalLayer& layer, EventPublisher publisher,
                                         ClockFn clock)
    : layer_(layer), publisher_(std::move(publisher)), clock_(std::move(clock)) {
  if (!clock_)
    clock_ = [] { return now_us(); };
}

Result<gen::LightSetResponse> LightingServiceImpl::set_light(
    const gen::LightSetRequest& request) {
  auto valid = gen::validate(request);
  if (!valid)
    return valid.error();
  const std::uint64_t timestamp = clock_();
  auto level = layer_.apply(id_for("cabin_light_level"), request.level, timestamp);
  if (!level)
    return level.error();
  auto color = layer_.apply(id_for("cabin_light_color"),
                            static_cast<double>(static_cast<std::uint8_t>(request.color)),
                            timestamp);
  if (!color)
    return color.error();

  gen::LightState state;
  state.level = request.level;
  state.color = request.color;
  state.timestamp_us = timestamp;
  publish_typed(publisher_, gen::kLightingServiceEventLightStateChanged, state);

  gen::LightSetResponse response;
  response.accepted = true;
  response.level = request.level;
  response.color = request.color;
  return response;
}

Result<gen::LightState> LightingServiceImpl::get_state() {
  auto snapshot = layer_.snapshot(clock_());
  if (!snapshot)
    return snapshot.error();
  gen::LightState state;
  state.level = snapshot.value().cabin_light_level;
  state.color = snapshot.value().cabin_light_color;
  state.timestamp_us = snapshot.value().timestamp_us;
  return state;
}

} // namespace cabin::signals
