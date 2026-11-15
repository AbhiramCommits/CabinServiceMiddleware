#include "cabin/personalization/personalization_service.hpp"

#include <cstdio>

#include "cabin/clock.hpp"
#include "cabin/log.hpp"
#include "cabin/trace.hpp"
#include "cabin_serdes.hpp"

namespace cabin {
namespace {

template <typename T>
bool publish_event(const ServiceEventPublisher& publisher, std::uint16_t event_id,
                   const T& value) {
  if (!publisher)
    return true;
  auto encoded = gen::encode(value);
  if (!encoded) {
    log_warn("personalization", "event_encode_failed",
             {Field("message", encoded.error().message)});
    return false;
  }
  publisher(event_id, encoded.value());
  return true;
}

} // namespace

PersonalizationService::PersonalizationService(ProfileStore& store,
                                               gen::IRpcChannel& channel,
                                               ServiceEventPublisher publisher,
                                               ServiceClock clock, int call_timeout_ms)
    : store_(store),
      seat_(channel, call_timeout_ms),
      hvac_(channel, call_timeout_ms),
      lighting_(channel, call_timeout_ms),
      publisher_(std::move(publisher)),
      clock_(std::move(clock)) {
  if (!clock_)
    clock_ = [] { return now_us(); };
}

Result<gen::GetProfileResponse> PersonalizationService::get_profile(
    const gen::GetProfileRequest& request) {
  gen::GetProfileResponse response;
  if (request.driver_id.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "driver_id is required");
  }
  auto profile = store_.get(request.driver_id);
  if (!profile.has_value()) {
    response.found = false;
    return response;
  }
  response.found = true;
  response.profile = profile.value();
  return response;
}

Result<gen::PutProfileResponse> PersonalizationService::put_profile(
    const gen::PutProfileRequest& request) {
  auto valid = gen::validate(request.profile);
  if (!valid)
    return valid.error();
  if (request.profile.driver_id.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "driver_id is required");
  }
  auto status = store_.put(request.profile);
  if (!status)
    return status.error();
  status = save();
  if (!status)
    return status.error();

  gen::PutProfileResponse response;
  response.accepted = true;
  response.schema_version = request.profile.schema_version;
  log_info("personalization", "profile_stored",
           {Field("driver_id", request.profile.driver_id)});
  return response;
}

Result<gen::ApplyProfileResponse> PersonalizationService::apply_profile(
    const gen::ApplyProfileRequest& request) {
  if (request.driver_id.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "driver_id is required");
  }
  return apply_profile_internal(request.driver_id);
}

Result<gen::ApplyProfileResponse> PersonalizationService::apply_profile_internal(
    const std::string& driver_id) {
  auto profile = store_.get(driver_id);
  if (!profile.has_value()) {
    return make_error(ErrorCode::kNotFound, "no profile for " + driver_id);
  }
  auto valid = gen::validate(profile.value());
  if (!valid)
    return valid.error();

  std::lock_guard<std::mutex> lock(apply_mutex_);
  ScopedSpan span("personalization", "ApplyProfile");
  const std::uint64_t started = clock_();

  // Fixed order: seat, HVAC, lighting. The order is part of the observable
  // contract and is asserted by tests (safety-relevant seat movement first).
  auto seat = seat_.set_position(
      gen::SeatSetPositionRequest{profile->seat_position_mm, profile->seat_recline_deg});
  if (!seat)
    return seat.error();
  auto hvac = hvac_.set_setpoint(
      gen::HvacSetRequest{profile->hvac_setpoint_c, profile->hvac_fan_level});
  if (!hvac)
    return hvac.error();
  auto light = lighting_.set_light(
      gen::LightSetRequest{profile->light_level, profile->light_color});
  if (!light)
    return light.error();

  const std::uint64_t finished = clock_();
  const std::uint32_t latency =
      static_cast<std::uint32_t>(finished > started ? finished - started : 0);
  last_apply_latency_us_.store(latency);

  auto active = store_.set_active_driver(driver_id);
  if (!active)
    return active.error();
  auto saved = save();
  if (!saved)
    return saved.error();

  gen::ProfileAppliedEvent event;
  event.driver_id = driver_id;
  event.latency_us = latency;
  publish_event(publisher_, gen::kPersonalizationServiceEventProfileApplied, event);

  log_info("personalization", "profile_applied",
           {Field("driver_id", driver_id),
            Field("latency_us", static_cast<std::int64_t>(latency))});

  gen::ApplyProfileResponse response;
  response.applied = true;
  response.latency_us = latency;
  return response;
}

Status PersonalizationService::on_ignition_state(gen::IgnitionState state) {
  if (state != gen::IgnitionState::kON) {
    last_ignition_.store(static_cast<int>(state));
    return ok_status();
  }
  const int previous = last_ignition_.exchange(static_cast<int>(state));
  if (previous == static_cast<int>(gen::IgnitionState::kON)) {
    return ok_status();
  }
  const std::string driver = store_.active_driver();
  if (driver.empty()) {
    log_info("personalization", "ignition_on_no_active_driver");
    return ok_status();
  }
  auto applied = apply_profile_internal(driver);
  if (!applied) {
    log_warn("personalization", "ignition_apply_failed",
             {Field("driver_id", driver), Field("message", applied.error().message)});
    return applied.error();
  }
  return ok_status();
}

Status PersonalizationService::save() {
  auto status = store_.save();
  if (!status) {
    log_error("personalization", "profile_save_failed",
              {Field("message", status.error().message)});
  }
  return status;
}

} // namespace cabin
