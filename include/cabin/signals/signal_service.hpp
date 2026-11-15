#pragma once

#include <cmath>
#include <functional>
#include <string>

#include "cabin/signals/cache.hpp"
#include "cabin_can_map.hpp"
#include "cabin_serdes.hpp"
#include "cabin_services.hpp"

namespace cabin::signals {

// Server-side implementation of SignalService. Hosted by signald; also used by
// integration tests to exercise the same path.
class SignalServiceImpl final : public gen::ISignalService {
 public:
  explicit SignalServiceImpl(SignalProcessor& processor) : processor_(processor) {}

  void set_on_change(std::function<void(const gen::SignalSnapshot&)> callback) {
    on_change_ = std::move(callback);
  }

  Result<gen::SignalSnapshot> get_signals() override { return processor_.snapshot(); }

  Result<gen::SignalSetResponse> set_signal(
      const gen::SignalSetRequest& request) override {
    const SignalMeta* meta = find_signal(request.name);
    if (meta == nullptr) {
      return make_error(ErrorCode::kNotFound, "unknown signal " + request.name);
    }
    if (!std::isfinite(request.value) || request.value < meta->min ||
        request.value > meta->max) {
      return make_error(ErrorCode::kOutOfRange,
                        "value for " + request.name + " is out of range");
    }
    const std::uint64_t now = processor_.now_us();
    processor_.cache().update(meta->id, request.value, now);
    gen::SignalSetResponse response;
    response.accepted = true;
    response.applied_value = request.value;
    response.timestamp_us = now;
    if (on_change_)
      on_change_(processor_.snapshot());
    return response;
  }

 private:
  SignalProcessor& processor_;
  std::function<void(const gen::SignalSnapshot&)> on_change_;
};

} // namespace cabin::signals
