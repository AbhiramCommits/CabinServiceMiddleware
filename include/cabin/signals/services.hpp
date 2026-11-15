#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "cabin/client.hpp"
#include "cabin/signals/cache.hpp"
#include "cabin_can_map.hpp"
#include "cabin_services.hpp"

namespace cabin::signals {

using EventPublisher = std::function<void(std::uint16_t event_id, const Bytes& payload)>;
using ClockFn = std::function<std::uint64_t()>;

// Abstraction over "where do actuated values go". LocalSignalLayer is used by
// tests and the replay harness; RemoteSignalLayer issues SignalService calls
// through the broker.
class SignalLayer {
 public:
  virtual ~SignalLayer() = default;
  virtual Status apply(SignalId id, double value, std::uint64_t now_us) = 0;
  virtual Result<gen::SignalSnapshot> snapshot(std::uint64_t now_us) = 0;
  virtual std::optional<double> get(SignalId id) = 0;
};

class LocalSignalLayer : public SignalLayer {
 public:
  explicit LocalSignalLayer(SignalCache& cache) : cache_(cache) {}
  Status apply(SignalId id, double value, std::uint64_t now_us) override;
  Result<gen::SignalSnapshot> snapshot(std::uint64_t now_us) override;
  std::optional<double> get(SignalId id) override;

 private:
  SignalCache& cache_;
};

class RemoteSignalLayer : public SignalLayer {
 public:
  explicit RemoteSignalLayer(std::shared_ptr<ServiceClient> client,
                             int timeout_ms = 2000);
  Status apply(SignalId id, double value, std::uint64_t now_us) override;
  Result<gen::SignalSnapshot> snapshot(std::uint64_t now_us) override;
  std::optional<double> get(SignalId id) override;

 private:
  std::shared_ptr<ServiceClient> client_;
  gen::SignalServiceClient proxy_;
};

class SeatServiceImpl final : public gen::ISeatService {
 public:
  SeatServiceImpl(SignalLayer& layer, EventPublisher publisher, ClockFn clock);
  Result<gen::SeatSetPositionResponse> set_position(
      const gen::SeatSetPositionRequest& request) override;
  Result<gen::SeatState> get_state() override;

 private:
  SignalLayer& layer_;
  EventPublisher publisher_;
  ClockFn clock_;
};

class HvacServiceImpl final : public gen::IHvacService {
 public:
  HvacServiceImpl(SignalLayer& layer, EventPublisher publisher, ClockFn clock);
  Result<gen::HvacSetResponse> set_setpoint(const gen::HvacSetRequest& request) override;
  Result<gen::HvacState> get_state() override;

 private:
  SignalLayer& layer_;
  EventPublisher publisher_;
  ClockFn clock_;
};

class LightingServiceImpl final : public gen::ILightingService {
 public:
  LightingServiceImpl(SignalLayer& layer, EventPublisher publisher, ClockFn clock);
  Result<gen::LightSetResponse> set_light(const gen::LightSetRequest& request) override;
  Result<gen::LightState> get_state() override;

 private:
  SignalLayer& layer_;
  EventPublisher publisher_;
  ClockFn clock_;
};

} // namespace cabin::signals
