#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "cabin/client.hpp"
#include "cabin/personalization/profile_store.hpp"
#include "cabin_services.hpp"
#include "cabin_types.hpp"

namespace cabin {

using ServiceEventPublisher =
    std::function<void(std::uint16_t event_id, const Bytes& payload)>;
using ServiceClock = std::function<std::uint64_t()>;

class PersonalizationService final : public gen::IPersonalizationService {
 public:
  PersonalizationService(ProfileStore& store, gen::IRpcChannel& channel,
                         ServiceEventPublisher publisher, ServiceClock clock,
                         int call_timeout_ms = 2000);

  Result<gen::GetProfileResponse> get_profile(
      const gen::GetProfileRequest& request) override;
  Result<gen::PutProfileResponse> put_profile(
      const gen::PutProfileRequest& request) override;
  Result<gen::ApplyProfileResponse> apply_profile(
      const gen::ApplyProfileRequest& request) override;

  // Called by the ignition subscription. Applies only on a transition into ON.
  Status on_ignition_state(gen::IgnitionState state);

  std::uint32_t last_apply_latency_us() const { return last_apply_latency_us_.load(); }

 private:
  Result<gen::ApplyProfileResponse> apply_profile_internal(const std::string& driver_id);
  Status save();

  ProfileStore& store_;
  gen::SeatServiceClient seat_;
  gen::HvacServiceClient hvac_;
  gen::LightingServiceClient lighting_;
  ServiceEventPublisher publisher_;
  ServiceClock clock_;
  mutable std::mutex apply_mutex_;
  std::atomic<std::uint32_t> last_apply_latency_us_{0};
  std::atomic<int> last_ignition_{-1};
};

} // namespace cabin
